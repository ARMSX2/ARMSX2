// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The Tile renderer's device-side reinterpretation of one format's texture window
// over another format's target (GSTileSwizzleForms) addresses GS memory in a fragment
// shader from closed forms fitted at runtime to the tree's own swizzle tables. Two
// things are pinned here, both against the tree and not against literals:
//
//   - the fit: every table in GSTables.cpp the shader relies on IS a GF(2)-linear map
//     of the coordinate bits (measured first by the vkswz probe), the fitted forms
//     reproduce the tables over their whole domain, and the inverse forms invert;
//   - the arithmetic: Locate() — the exact computation the GLSL transcribes — sends
//     every texel of an index window to the owner texel and byte holding it, where
//     "holding" is decided by GSOffset::pa on both sides. That is the CPU readers'
//     own address function, so a disagreement here would be a disagreement with the
//     bytes the software renderer samples.
//
// The cases deliberately include the shapes the census found: a window whose base
// sits BLOCKS into a page (GT4's 0x3e04-style palettes-as-textures), a window with
// TBW=1 in an 8-bit format (pages-per-row zero — GSOffset's arithmetic, not a clamp),
// windows spanning several page rows of an owner with a different pitch, and every
// palette view over a CT32 layout.

#include "GS/GSLocalMemory.h"
#include "GS/GSTables.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"

#include <gtest/gtest.h>

using namespace GSTileSwizzleForms;

TEST(GSTileSwizzleForms, EveryTableFitsAndTheFormsReproduceThem)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (u32 by = 0; by < 4; by++)
		for (u32 bx = 0; bx < 8; bx++)
		{
			EXPECT_EQ(f.block48.Eval(bx, by), blockTable32.lookup(bx, by)) << bx << "," << by;
			EXPECT_EQ(f.block48.Eval(bx, by), blockTable8.lookup(bx, by)) << bx << "," << by;
		}
	for (u32 by = 0; by < 8; by++)
		for (u32 bx = 0; bx < 4; bx++)
			EXPECT_EQ(f.block84.Eval(bx, by), blockTable4.lookup(bx, by)) << bx << "," << by;
	for (u32 y = 0; y < 8; y++)
		for (u32 x = 0; x < 8; x++)
			EXPECT_EQ(f.col32.Eval(x, y), columnTable32[y][x]) << x << "," << y;
	for (u32 y = 0; y < 16; y++)
		for (u32 x = 0; x < 16; x++)
			EXPECT_EQ(f.col8.Eval(x, y), columnTable8[y][x]) << x << "," << y;
	for (u32 y = 0; y < 16; y++)
		for (u32 x = 0; x < 32; x++)
			EXPECT_EQ(f.col4.Eval(x, y), columnTable4[y][x]) << x << "," << y;
}

TEST(GSTileSwizzleForms, TheInverseFormsInvert)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (u32 by = 0; by < 4; by++)
		for (u32 bx = 0; bx < 8; bx++)
		{
			const u32 packed = f.inv_block48.Eval(f.block48.Eval(bx, by));
			EXPECT_EQ(packed & 7, bx);
			EXPECT_EQ(packed >> 3, by);
		}
	for (u32 y = 0; y < 8; y++)
		for (u32 x = 0; x < 8; x++)
		{
			const u32 packed = f.inv_col32.Eval(f.col32.Eval(x, y));
			EXPECT_EQ(packed & 7, x);
			EXPECT_EQ(packed >> 3, y);
		}
}

TEST(GSTileSwizzleForms, FitRefusesANonLinearTable)
{
	// A table with a constant term, and one that is not linear at all.
	XorForm2 out;
	EXPECT_FALSE(Fit2([](u32 x, u32 y) { return (x ^ y) | 1u; }, 3, 3, out));
	EXPECT_FALSE(Fit2([](u32 x, u32 y) { return x * y; }, 3, 3, out));
	EXPECT_TRUE(Fit2([](u32 x, u32 y) { return x ^ (y << 3); }, 3, 3, out));
	EXPECT_EQ(out.Eval(5, 6), 5u ^ (6u << 3));
}

TEST(GSTileSwizzleForms, ShaderDefinesCarryEveryConstant)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	const std::string s = ShaderDefines(f);
	// One line per basis entry: five 2-input forms × 12 entries + four 1-input forms × 10
	// (the two inverse forms and the two CLUT word-order forms).
	size_t lines = 0;
	for (char c : s)
		lines += (c == '\n');
	EXPECT_EQ(lines, 5u * 12u + 4u * 10u);
	EXPECT_TRUE(f.clut_valid);
	EXPECT_NE(s.find("#define TILE_SWZ_CLUT8_7 "), std::string::npos);
	EXPECT_NE(s.find("#define TILE_SWZ_B48_X0 "), std::string::npos);
	EXPECT_NE(s.find("#define TILE_SWZ_IC32_5 "), std::string::npos);
}

namespace
{
struct Case
{
	u32 psm; // source (index) format
	u32 src_bp, src_bw; // TEX0.TBP0 / TBW
	u32 dst_bp, dst_bw; // owner base (page-aligned) / FBW
	u32 tw, th; // window size in source texels
};

// The byte address of source texel (u, v) under its own swizzle, plus the nibble for
// the four-bit formats — GSOffset::pa's answer.
void SourceByte(const Case& c, u32 u, u32 v, u32& byte_addr, u32& nibble)
{
	const GSOffset off = GSOffset::fromKnownPSM(c.src_bp, c.src_bw, static_cast<GS_PSM>(c.psm));
	const u32 pa = off.pa(static_cast<int>(u), static_cast<int>(v));
	switch (c.psm)
	{
		case PSMT8:
			byte_addr = pa;
			nibble = 0;
			break;
		case PSMT4:
			byte_addr = pa >> 1;
			nibble = pa & 1;
			break;
		case PSMT8H:
			byte_addr = pa * 4 + 3;
			nibble = 0;
			break;
		case PSMT4HL:
			byte_addr = pa * 4 + 3;
			nibble = 0;
			break;
		default: // PSMT4HH
			byte_addr = pa * 4 + 3;
			nibble = 1;
			break;
	}
}

void CheckCase(const FormSet& f, const Case& c)
{
	const int fmt = IndexFormatFor(c.psm);
	ASSERT_GE(fmt, 0);
	const GSOffset src_off = GSOffset::fromKnownPSM(c.src_bp, c.src_bw, static_cast<GS_PSM>(c.psm));
	const GSOffset dst_off = GSOffset::fromKnownPSM(c.dst_bp, c.dst_bw, PSMCT32);
	Reinterpretation r;
	r.src_bp = c.src_bp;
	r.src_bwpg = static_cast<u32>(src_off.pageRowWidth() >> src_off.pageShiftX());
	r.dst_bp = c.dst_bp;
	r.dst_bwpg = static_cast<u32>(dst_off.pageRowWidth() >> dst_off.pageShiftX());

	u32 mismatches = 0;
	for (u32 v = 0; v < c.th; v++)
	{
		for (u32 u = 0; u < c.tw; u++)
		{
			u32 want_byte, want_nibble;
			SourceByte(c, u, v, want_byte, want_nibble);
			const OwnerTexel o = Locate(f, static_cast<IndexFormat>(fmt), r, u, v);
			ASSERT_TRUE(o.in_range) << "psm " << c.psm << " (" << u << "," << v << ")";
			const u32 got_byte = dst_off.pa(static_cast<int>(o.x), static_cast<int>(o.y)) * 4 + o.byte;
			if (got_byte != want_byte || o.nibble != want_nibble)
			{
				if (mismatches++ < 8)
				{
					ADD_FAILURE() << "psm " << c.psm << " src bp " << c.src_bp << " bw " << c.src_bw << " dst bp "
								  << c.dst_bp << " bw " << c.dst_bw << " texel (" << u << "," << v << "): want byte "
								  << want_byte << " nibble " << want_nibble << ", got byte " << got_byte << " nibble "
								  << o.nibble << " via owner (" << o.x << "," << o.y << ") ch " << o.byte;
				}
			}
		}
	}
	EXPECT_EQ(mismatches, 0u);
}
} // namespace

TEST(GSTileSwizzleForms, LocateAgreesWithGSOffsetOnEveryTexel)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	const Case cases[] = {
		// GT4's loop: one page of a 640-wide CT24 target (FBW 10) read as a 128×64 P_8
		// window with TBW 2, one page in from the base.
		{PSMT8, 0x1180 + 0x20, 2, 0x1180, 10, 128, 64},
		// ...and its palettes-as-textures: an 8×32 P_8 window whose base sits four blocks
		// into a page, TBW 1 (pages-per-row zero in GSOffset's arithmetic).
		{PSMT8, 0x3e04, 1, 0x3e00, 1, 8, 32},
		// A P_8 window spanning several page rows of a wider owner.
		{PSMT8, 0x0800, 4, 0x0800, 10, 256, 128},
		// P_8 over the owner's own base at the owner's own pitch (Classic's same-base case).
		{PSMT8, 0x0000, 20, 0x0000, 10, 512, 64},
		// P_4 windows: page-aligned and block-offset, one and two page rows.
		{PSMT4, 0x0400, 2, 0x0400, 8, 128, 128},
		{PSMT4, 0x0400 + 5, 2, 0x0400, 8, 64, 32},
		{PSMT4, 0x0400, 4, 0x0400, 4, 256, 256},
		// The alpha-byte views over the CT32 layout, offset into the owner.
		{PSMT8H, 0x0c00 + 0x40, 10, 0x0c00, 10, 128, 64},
		{PSMT4HL, 0x0c00 + 3, 10, 0x0c00, 10, 64, 32},
		{PSMT4HH, 0x0c00, 5, 0x0c00, 5, 320, 96},
	};
	for (const Case& c : cases)
		CheckCase(f, c);
}
