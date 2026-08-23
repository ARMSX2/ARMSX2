// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Texture shuffle, and the claim that this renderer needs NO code for it.
//
// A "texture shuffle" is how a game moves one colour channel into another on hardware that has no
// channel-select: it points a 16-bit frame buffer at the pages a 32-bit surface occupies (or at its
// own), and draws a textured sprite whose U is offset from its X by eight pixels. Inside one GS
// block the 16-bit view's columns 0-7 are the LOW halfwords of the 32-bit view's eight columns and
// its columns 8-15 are the HIGH halfwords of the same eight pixels, so reading at u = x + 8 and
// writing at x fetches the halfword carrying B,A and stores it where R,G live.
//
// Classic needs nine shuffle types and a two-pass detector for this, because its targets are
// format-typed RGBA8 images that share bytes with nothing: it has to RECOGNISE the intent and
// hand-simulate the channel surgery in a shader. A byte-addressed renderer has no such problem --
// the permutation is emergent from two addressing functions disagreeing about how to carve the same
// 8192-byte page, which is exactly what it models directly. So the falsifiable claim is:
//
//     with the 16-bit read arm and the 16-bit frame road in place, and NO shuffle-specific code
//     anywhere, the source address a shuffle draw reads and the destination address it writes
//     stand in precisely the relation the idiom needs -- because both are GSOffset's own.
//
// These are the three shapes the corpus census actually found (2026-08-23, 18 dumps, Classic arm,
// four dumps shuffle-active). They are pinned as ADDRESS statements rather than as rendered frames
// because that is where the claim lives: nothing downstream of the address can turn a correct
// halfword into the wrong channel.
//
//   ac5       RegionRepeat8, same-BP, PSMCT16S -> PSMCT16S, WMS = REGION_REPEAT, MINU = 0x3F7,
//             MAXU = 0.  Duplicates R,G into B,A.  Its mechanism IS the region arithmetic.
//   flatout2  Offset, cross-BP, PSMCT16 -> PSMCT16, FBP 0x3300 FBW 2 <- TBP 0x08c0 TBW 10,
//   mgs3      Offset, cross-BP, PSMCT16 -> PSMCT16, FBP 0x2000 FBW 8 <- TBP 0x2800 TBW 8.
//             8-pixel strips, x_u_offset 8: reads the RG half, writes the BA half.
//   outrun-b  Offset, same-BP, PSMZ16 -> PSMCT16 at 0x12c0 FBW 10, FBMSK 0x00003fff.  Reads the
//             frame's own green byte through the Z16 swizzle and deposits it in alpha.
//
// Split texture shuffle, and Classic's other eight types, have ZERO corpus incidence and are
// deliberately not modelled: in a bytes model each sub-draw is independently correct, so Classic's
// merge is unnecessary by construction rather than unimplemented.

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

using namespace GSTileSwizzleForms;

namespace
{
// tilegpu_wrap's REGION_REPEAT arm, and the GS's own rule: the coordinate is masked by MINU and
// or-ed with MAXU. Transcribed rather than called because the shader's copy is GLSL; the two are
// one line and this is the line.
u32 RegionRepeat(u32 c, u32 minu, u32 maxu)
{
	return (c & minu) | maxu;
}

// The 32-bit word a 16-bit guest byte address lives in, and which half of it.
u32 WordOf(u32 byte_addr)
{
	return byte_addr >> 2;
}
u32 HalfOf(u32 byte_addr)
{
	return (byte_addr >> 1) & 1;
}
} // namespace

// outrun-b's shape, and the cleanest statement of the whole idiom: a SAME-BASE draw reading at
// u = x + 8 reads the OTHER HALF OF THE WORD IT WRITES. Nothing else has to be true for the
// channel move to happen -- the GS quantises, masks and stores as it does for any draw.
TEST(TileGpuTextureShuffle, SameBaseOffsetEightReadsTheOtherHalfOfTheWordItWrites)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	// FBP = TBP0 = 0x12c0, FBW = TBW = 10.  The frame is PSMCT16 and the texture PSMZ16 -- the two
	// disagree about the block table by the depth XOR, which is the whole reason this draw needs
	// the Z arm and not just the colour one.
	constexpr u32 kBp = 0x12c0, kBw = 10;
	u32 pairs = 0;
	for (u32 y = 0; y < 64; y++)
	{
		for (u32 x = 0; x < 640; x++)
		{
			u32 dst = 0;
			ASSERT_TRUE(Address16(f, PSMCT16, kBp, kBw, x, y, dst));
			// The GS's own answer for the destination, so the model is not marking its own work.
			const GSOffset foff = GSOffset::fromKnownPSM(kBp, kBw, PSMCT16);
			ASSERT_EQ(dst, static_cast<u32>(foff.pa(static_cast<int>(x), static_cast<int>(y))) * 2);

			// And the source, eight texels along, through PSMZ16's swizzle.
			u32 src = 0;
			ASSERT_TRUE(Address16(f, PSMZ16, kBp, kBw, x + 8, y, src));

			// The claim, for a destination in the LOW half of its 16-texel group.
			if ((x & 8) == 0)
			{
				// Same word, other half: the read is the high halfword of the cell the write
				// lands in the low halfword of.  That IS "move B,A into R,G".
				EXPECT_EQ(HalfOf(dst), 0u) << x << "," << y;
				EXPECT_EQ(HalfOf(src), 1u) << x << "," << y;
				pairs++;
			}
			else
			{
				EXPECT_EQ(HalfOf(dst), 1u) << x << "," << y;
				EXPECT_EQ(HalfOf(src), 0u) << x << "," << y;
			}
		}
	}
	EXPECT_EQ(pairs, 64u * 320u);
}

// The word relation stated without the depth twist: for a same-format, same-base pair, u = x + 8
// and x land in ONE word.  This is the statement outrun-b's Z16 source cannot make (its block XOR
// moves the word), and it is the one ac5's self-copy relies on.
TEST(TileGpuTextureShuffle, SameFormatSameBaseOffsetEightSharesTheWord)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (u32 psm : {u32(PSMCT16), u32(PSMCT16S)})
	{
		for (u32 y = 0; y < 64; y++)
		{
			for (u32 x = 0; x < 64; x += 16)
			{
				u32 lo = 0, hi = 0;
				ASSERT_TRUE(Address16(f, psm, 0x2400, 2, x, y, lo));
				ASSERT_TRUE(Address16(f, psm, 0x2400, 2, x + 8, y, hi));
				EXPECT_EQ(WordOf(lo), WordOf(hi)) << psm << " " << x << "," << y;
				EXPECT_EQ(HalfOf(lo), 0u);
				EXPECT_EQ(HalfOf(hi), 1u);
			}
		}
	}
}

// ac5's shape.  Its mechanism is CLAMP_REGION_REPEAT, not a coordinate offset: MINU clears bit 3 of
// the source U, so wherever the destination walks, the source is FORCED into the low half of its
// 16-texel group.  That is a broadcast of R,G across both halves, and it is entirely the region
// arithmetic -- get that wrong and the draw is wrong, whatever the swizzle does.
TEST(TileGpuTextureShuffle, RegionRepeatEightForcesTheSourceIntoTheLowHalf)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	constexpr u32 kMinu = 1015; // 0x3F7 -- every bit but bit 3
	constexpr u32 kMaxu = 0;
	ASSERT_EQ(kMinu & 8u, 0u) << "the census shape is MINU with bit 3 clear; without that there is no broadcast";

	constexpr u32 kBp = 0x2400, kBw = 2;
	for (u32 y = 0; y < 64; y++)
	{
		for (u32 x = 0; x < 128; x++)
		{
			const u32 u = RegionRepeat(x, kMinu, kMaxu);
			EXPECT_EQ(u & 8u, 0u) << x;

			u32 src = 0, dst = 0;
			ASSERT_TRUE(Address16(f, PSMCT16S, kBp, kBw, u, y, src));
			ASSERT_TRUE(Address16(f, PSMCT16S, kBp, kBw, x, y, dst));
			// Whichever half the destination is in, the source is always the low one -- so the
			// low halfword's value is written to both halves as the destination sweeps them.
			EXPECT_EQ(HalfOf(src), 0u) << x << "," << y;
			EXPECT_EQ(HalfOf(dst), (x & 8u) ? 1u : 0u) << x << "," << y;
			if ((x & 8u) == 0)
				EXPECT_EQ(src, dst) << "a same-base region-repeat draw at an even group is the identity";
		}
	}
}

// flatout2's and mgs3's shape: CROSS-base 8-pixel strips at x_u_offset 8.  Base and stride differ,
// so there is no shared word to appeal to; what has to hold is that the read takes the HIGH
// halfword of its own word and the write lands in the LOW halfword of its own -- which is what
// "reads the RG half, writes the BA half" is, once both addresses are GSOffset's.
TEST(TileGpuTextureShuffle, CrossBaseEightPixelStripsReadHighAndWriteLow)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	struct Shape
	{
		const char* name;
		u32 fbp, fbw;
		u32 tbp, tbw;
		u32 h;
	};
	const Shape shapes[] = {
		{"flatout2", 0x3300, 2, 0x08c0, 10, 128},
		{"mgs3", 0x2000, 8, 0x2800, 8, 128},
	};

	for (const Shape& s : shapes)
	{
		const GSOffset foff = GSOffset::fromKnownPSM(s.fbp, s.fbw, PSMCT16);
		const GSOffset toff = GSOffset::fromKnownPSM(s.tbp, s.tbw, PSMCT16);
		for (u32 y = 0; y < s.h; y++)
		{
			for (u32 x = 0; x < 64; x += 16)
			{
				u32 dst = 0, src = 0;
				ASSERT_TRUE(Address16(f, PSMCT16, s.fbp, s.fbw, x, y, dst));
				ASSERT_TRUE(Address16(f, PSMCT16, s.tbp, s.tbw, x + 8, y, src));
				// Both are the hardware's own addresses...
				EXPECT_EQ(dst, static_cast<u32>(foff.pa(static_cast<int>(x), static_cast<int>(y))) * 2)
					<< s.name;
				EXPECT_EQ(src, static_cast<u32>(toff.pa(static_cast<int>(x + 8), static_cast<int>(y))) * 2)
					<< s.name;
				// ...and the halves are the ones the idiom needs.
				EXPECT_EQ(HalfOf(dst), 0u) << s.name << " " << x << "," << y;
				EXPECT_EQ(HalfOf(src), 1u) << s.name << " " << x << "," << y;
			}
		}
	}
}

// outrun-b's shape depends on its FBMSK as much as on its addresses, and this is the one place in
// the three shapes where the road is NOT bit-exact.
//
// FBMSK 0x00003fff against PSMCT16's stored bits (0x80F8F8F8) covers red's five bits entirely, the
// LOW THREE of green's five, and nothing of blue or alpha. Classic reaches the same place by
// converting the register to a 16-bit mask of 0xFF -- the low byte of the cell, which is red plus
// green's low three bits -- so the two agree about the register; what differs is what a renderer
// can DO with it. A raster pipeline masks at channel granularity and the GS masks at bit
// granularity, so a channel the mask covers only PARTLY is written whole and the bits the GS would
// have kept are lost. Green is that channel here.
//
// Pinned rather than fixed because it is the standing sub-channel approximation the write-mask
// rule already documents, it predates this road, and outrun-b is the corpus's example of it. The
// exact form needs a shader read-modify-write at bit granularity, which is the fbmask road.
TEST(TileGpuTextureShuffle, OutRunsFbmskIsTheSubChannelApproximation)
{
	constexpr u32 kFbmsk = 0x00003fffu;
	constexpr u32 kFmsk16 = 0x80F8F8F8u; // PSMCT16's stored bits, as GSLocalMemory records them
	const GSLocalMemory mem;
	ASSERT_EQ(GSLocalMemory::m_psm[PSMCT16].fmsk, kFmsk16);

	const u8 m = gsTileFrameWriteMask(kFbmsk, kFmsk16);
	EXPECT_EQ(m & 0x1u, 0u) << "red's five stored bits are fully covered, so red is preserved";
	EXPECT_NE(m & 0x2u, 0u) << "green is only partly covered, so the road writes it whole";
	EXPECT_NE(m & 0x4u, 0u) << "blue is untouched by this mask";
	EXPECT_NE(m & 0x8u, 0u) << "so is the alpha bit";

	// The draw lands something but not everything, so neither the seed-skip nor the
	// nothing-written carve-out may claim it.
	EXPECT_FALSE(gsTileFrameWriteIsTotal(kFbmsk, kFmsk16));
	EXPECT_FALSE(gsTileFrameWritesNothing(kFbmsk, kFmsk16));

	// ...and the road says so itself: this mask is NOT exactly expressible at channel granularity.
	EXPECT_FALSE(gsTileFrameWriteMaskIsExact(kFbmsk, kFmsk16))
		<< "if this ever becomes exact the approximation above has been fixed and the comment is stale";
}
