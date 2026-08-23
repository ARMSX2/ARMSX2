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

#include <vector>

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

// The 16-bit colour families' own tables. blockTable16 is byte-identical to blockTable4 in
// GSTables.cpp, so the shader evaluates one form for both -- which is a fact about the tree that
// must fail loudly if it ever stops being true, exactly as blockTable8 == blockTable32 does.
// blockTable16S and columnTable16 are their own fits.
TEST(GSTileSwizzleForms, TheSixteenBitTablesFitAndTheFormsReproduceThem)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (u32 by = 0; by < 8; by++)
		for (u32 bx = 0; bx < 4; bx++)
		{
			EXPECT_EQ(blockTable16.lookup(bx, by), blockTable4.lookup(bx, by))
				<< "blockTable16 is no longer blockTable4 at " << bx << "," << by;
			EXPECT_EQ(f.block84.Eval(bx, by), blockTable16.lookup(bx, by)) << bx << "," << by;
			EXPECT_EQ(f.block84s.Eval(bx, by), blockTable16S.lookup(bx, by)) << bx << "," << by;
		}
	for (u32 y = 0; y < 8; y++)
		for (u32 x = 0; x < 16; x++)
			EXPECT_EQ(f.col16.Eval(x, y), columnTable16[y][x]) << x << "," << y;
}

namespace
{
// Locate16's answer restated as a guest BYTE address, the way the writeback and seed shaders
// spend it: page * 8 KB + the word inside the page's slot * 4 + the halfword.
u64 Locate16ByteAddr(const FormSet& f, u32 psm, u32 bp, u32 bw, u32 x, u32 y)
{
	Texel16 t;
	EXPECT_TRUE(Locate16(f, psm, bp, bw, x, y, t));
	return static_cast<u64>(t.page) * GS_PAGE_SIZE + static_cast<u64>(t.word_in_page) * 4 +
		   static_cast<u64>(t.half) * 2;
}
} // namespace

// The 16-bit colour byte road's address, against GSOffset's own. GSOffset::pa counts HALFWORDS for
// a 16-bit format, so its byte address is pa * 2 -- folded into the 4 MB wrap, because a surface
// near the top of memory legitimately addresses pages below its base.
TEST(GSTileSwizzleForms, Locate16AgreesWithGSOffsetOnEveryTexel)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	struct Case
	{
		u32 psm;
		u32 bp, bw; // page-aligned base in blocks, stride in pages
		u32 w, h;   // pixels to sweep
	};
	const Case cases[] = {
		// A 640-wide 16-bit frame buffer, the bgda2/yugioh shape, over two page rows.
		{PSMCT16, 0x0000, 10, 640, 128},
		{PSMCT16S, 0x0000, 10, 640, 128},
		// Based well into memory, and a stride that is not a power of two.
		{PSMCT16, 4800, 10, 640, 64},
		{PSMCT16S, 12160, 10, 640, 64},
		// One page wide, several rows down: the degenerate stride the display road can hand it.
		{PSMCT16, 0x1180, 1, 64, 256},
		{PSMCT16S, 0x1180, 1, 64, 256},
		// A wide surface whose base is high enough that its own rows wrap past the top of memory.
		{PSMCT16, 15872, 8, 512, 192},
	};

	for (const Case& c : cases)
	{
		const GSOffset off = GSOffset::fromKnownPSM(c.bp, c.bw, static_cast<GS_PSM>(c.psm));
		u32 mismatches = 0;
		for (u32 y = 0; y < c.h; y++)
		{
			for (u32 x = 0; x < c.w; x++)
			{
				const u64 want = (static_cast<u64>(off.pa(static_cast<int>(x), static_cast<int>(y))) * 2) %
								 (static_cast<u64>(GS_MAX_PAGES) * GS_PAGE_SIZE);
				const u64 got = Locate16ByteAddr(f, c.psm, c.bp, c.bw, x, y);
				if (got != want && mismatches++ < 8)
				{
					ADD_FAILURE() << "psm " << c.psm << " bp " << c.bp << " bw " << c.bw << " texel (" << x << ","
								  << y << "): want byte " << want << ", got " << got;
				}
			}
		}
		EXPECT_EQ(mismatches, 0u) << "psm " << c.psm << " bp " << c.bp;
	}
}

// The 16-bit texture READ arm's address, against GSOffset's own. Same property as Locate16, asked
// of the road that samples rather than the road that owns -- so the base is NOT page-aligned in
// every case, and the two depth formats carry the block XOR that Locate16 never sees.
TEST(GSTileSwizzleForms, Address16AgreesWithGSOffsetOnEveryTexel)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	// The depth twins really are the colour tables plus one constant, and it is the constant
	// GSOffset carries. Stated here as well as checked inside Fit(), so the fit's silent
	// invalidation cannot pass for agreement.
	EXPECT_NE(f.z16_block_xor, 0u);

	struct Case
	{
		u32 psm;
		u32 tbp0, tbw;
		u32 w, h;
	};
	const Case cases[] = {
		// Page-aligned windows in all four formats, one and several page rows.
		{PSMCT16, 0x1180, 4, 256, 128},
		{PSMCT16S, 0x1180, 4, 256, 128},
		{PSMZ16, 0x1180, 4, 256, 128},
		{PSMZ16S, 0x1180, 4, 256, 128},
		// The corpus shapes: full-width 640 windows at the bases the census recorded.
		{PSMCT16, 0x08c0, 10, 640, 128},
		{PSMCT16, 0x2800, 8, 512, 128},
		{PSMZ16, 0x12c0, 10, 640, 128},
		{PSMCT16S, 0x2400, 2, 128, 256},
		// A base sitting BLOCKS into a page, which a texture window legitimately does and a
		// surface never does -- and which is where the depth XOR stops distributing over the sum.
		{PSMCT16, 0x1184, 2, 128, 64},
		{PSMZ16, 0x1187, 2, 128, 64},
		{PSMZ16S, 0x1183, 1, 64, 128},
		// TBW = 0: GSOffset folds every row onto page column zero, and so must this.
		{PSMCT16, 0x1180, 0, 64, 64},
		// High enough to wrap past the top of memory.
		{PSMZ16, 15872, 8, 512, 192},
	};

	for (const Case& c : cases)
	{
		const GSOffset off = GSOffset::fromKnownPSM(c.tbp0, c.tbw, static_cast<GS_PSM>(c.psm));
		u32 mismatches = 0;
		for (u32 v = 0; v < c.h; v++)
		{
			for (u32 u = 0; u < c.w; u++)
			{
				u32 got = 0;
				ASSERT_TRUE(Address16(f, c.psm, c.tbp0, c.tbw, u, v, got));
				// GSOffset::pa counts HALFWORDS for a 16-bit format, and GS memory wraps at 4 MB.
				const u32 want = static_cast<u32>(
					(static_cast<u64>(off.pa(static_cast<int>(u), static_cast<int>(v))) * 2) %
					(static_cast<u64>(GS_MAX_PAGES) * GS_PAGE_SIZE));
				if (got != want && mismatches++ < 8)
				{
					ADD_FAILURE() << "psm " << c.psm << " tbp0 " << c.tbp0 << " tbw " << c.tbw << " texel (" << u
								  << "," << v << "): want byte " << want << ", got " << got;
				}
			}
		}
		EXPECT_EQ(mismatches, 0u) << "psm " << c.psm << " tbp0 " << c.tbp0;
	}
}

// The state row's index_format is ONE numbering built from two lists. The fragment shader switches
// on it; nothing links this file to the GLSL, so a renumbering that moved one list and not the
// other would read PSMZ16S through PSMCT16's block table and produce plausible wrong texels.
TEST(GSTileSwizzleForms, TheDirect16NumberingIsWhatTheShaderSwitchesOn)
{
	EXPECT_EQ(Direct16FormatFor(PSMCT16), 0);
	EXPECT_EQ(Direct16FormatFor(PSMCT16S), 1);
	EXPECT_EQ(Direct16FormatFor(PSMZ16), 2);
	EXPECT_EQ(Direct16FormatFor(PSMZ16S), 3);
	// Bit 0 is "strided block table", bit 1 is "depth XOR" -- which is how the shader decodes it
	// rather than as four separate arms.
	EXPECT_EQ(Direct16FormatFor(PSMCT16S) & 1, 1);
	EXPECT_EQ(Direct16FormatFor(PSMZ16S) & 1, 1);
	EXPECT_EQ(Direct16FormatFor(PSMZ16) & 2, 2);
	EXPECT_EQ(Direct16FormatFor(PSMZ16S) & 2, 2);
	// Disjoint from the index list, and both refuse everything else: index_format 6 + this must
	// never collide with 1 + IndexFormatFor, whose largest is 5.
	for (u32 psm : {u32(PSMCT32), u32(PSMCT24), u32(PSMT8), u32(PSMT4), u32(PSMT8H), u32(PSMT4HL), u32(PSMT4HH),
			 u32(PSMZ32), u32(PSMZ24)})
	{
		EXPECT_EQ(Direct16FormatFor(psm), -1) << psm;
	}
	for (u32 psm : {u32(PSMCT16), u32(PSMCT16S), u32(PSMZ16), u32(PSMZ16S)})
		EXPECT_EQ(IndexFormatFor(psm), -1) << psm;
}

// The two texels the writeback pairs into one word really are one word apart in the ring, and the
// low half is the one at the lower x. That pairing is what lets the compute pass store a whole
// word instead of read-modify-writing a halfword from two workgroups at once.
TEST(GSTileSwizzleForms, TheSixteenBitWordPairsAreEightTexelsApart)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		for (u32 y = 0; y < 64; y++)
		{
			for (u32 g = 0; g < 4; g++) // the four 16-texel groups of a page row
			{
				for (u32 i = 0; i < 8; i++)
				{
					Texel16 lo, hi;
					ASSERT_TRUE(Locate16(f, psm, 0, 1, g * 16 + i, y, lo));
					ASSERT_TRUE(Locate16(f, psm, 0, 1, g * 16 + i + 8, y, hi));
					EXPECT_EQ(lo.page, hi.page);
					EXPECT_EQ(lo.word_in_page, hi.word_in_page);
					EXPECT_EQ(lo.half, 0u);
					EXPECT_EQ(hi.half, 1u);
				}
			}
		}
	}
}

// Every word of a 16-bit page is covered exactly once by the writeback's invocation grid, and
// every invocation stays inside its own page. A gap here is a word the ring never receives.
TEST(GSTileSwizzleForms, TheSixteenBitWritebackGridCoversAPageExactlyOnce)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		std::vector<int> seen(2048, 0);
		for (u32 gy = 0; gy < 64; gy++) // 8 workgroups of 8 in y
		{
			for (u32 gx = 0; gx < 32; gx++) // 4 workgroups of 8 in x
			{
				// The shader's own mapping: invocation gx owns the low half of one 16-texel group.
				const u32 px = (gx >> 3) * 16 + (gx & 7);
				Texel16 t;
				ASSERT_TRUE(Locate16(f, psm, 0, 1, px, gy, t));
				EXPECT_EQ(t.page, 0u);
				ASSERT_LT(t.word_in_page, 2048u);
				seen[t.word_in_page]++;
			}
		}
		for (u32 w = 0; w < 2048; w++)
			EXPECT_EQ(seen[w], 1) << "psm " << psm << " word " << w;
	}
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
	// One line per basis entry: seven 2-input forms × 12 entries + four 1-input forms × 10
	// (the two inverse forms and the two CLUT word-order forms), plus the depth block XOR.
	size_t lines = 0;
	for (char c : s)
		lines += (c == '\n');
	EXPECT_EQ(lines, 7u * 12u + 4u * 10u + 1u);
	EXPECT_TRUE(f.clut_valid);
	EXPECT_NE(s.find("#define TILE_SWZ_CLUT8_7 "), std::string::npos);
	EXPECT_NE(s.find("#define TILE_SWZ_B48_X0 "), std::string::npos);
	EXPECT_NE(s.find("#define TILE_SWZ_IC32_5 "), std::string::npos);
	// The 16-bit colour road's forms: the strided block table and the halfword column table.
	EXPECT_NE(s.find("#define TILE_SWZ_B84S_X0 "), std::string::npos);
	EXPECT_NE(s.find("#define TILE_SWZ_C16_X3 "), std::string::npos);
	EXPECT_NE(s.find("#define TILE_SWZ_C16_Y2 "), std::string::npos);
	// ...and the one constant that is not a form.
	EXPECT_NE(s.find("#define TILE_SWZ_Z16XOR "), std::string::npos);
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
