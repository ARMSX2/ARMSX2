// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Which surfaces the TileGpu byte road serves, and the page geometry its two shaders address them
// with.
//
// The byte road is the pair of passes that move a resident target's pixels to guest bytes and back
// (tilegpu_writeback.glsl, tilegpu_seed.glsl). A surface the road cannot carry is not an error: the
// renderer counts its pages lossy and the bytes under it go stale. So "which layouts does the road
// serve" is a fact with visible consequences, and both halves of it are pinned here -- the road may
// not claim a layout its shaders cannot address, and it may not refuse one they can, because a
// refusal is silent staleness rather than a failure anybody sees.
//
// The geometry half exists because the two shaders are dispatched by the executor from a PSM alone,
// with no layout to ask. A 32-bit page is 64x32 pixels and a 16-bit page is 64x64; the writeback's
// workgroup count and the seed's scissor both come off that number, and getting it wrong writes
// half a page or twice one.

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <ios>

namespace
{
constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm, GSTileSurfaceKind kind)
{
	return GSTileSurfaceLayout{bp, bw, psm, kind};
}
} // namespace

TEST(TileGpuByteRoad, ServesEveryColourFormatThePoolCanBack)
{
	// The four colour formats with 64-pixel-wide pages. The index formats never reach the pool.
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT32, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT24, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT16, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT16S, GSTileSurfaceKind::Color)));

	// ...plus the 32-bit DEPTH pair as a COLOUR surface. That is not a depth buffer: it is what a
	// game makes by setting FRAME.PSM to PSMZ32/PSMZ24 and rendering ordinary colour into it, and
	// the road serves it with the CT32 program under one block XOR.
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ32, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ24, GSTileSurfaceKind::Color)));
	// The 16-bit depth pair has no such program, colour-kind or not: nothing in the corpus renders
	// into one, so it would be an untested road with no caller.
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ16, GSTileSurfaceKind::Color)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ16S, GSTileSurfaceKind::Color)));

	// A real Z BUFFER never travels, whatever its PSM -- its pixels are a depth attachment and no
	// shader turns one into guest bytes. That is the standing separate gap, and the DEPTH kind is
	// what keeps the line between it and the case above.
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ32, GSTileSurfaceKind::Depth)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ24, GSTileSurfaceKind::Depth)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ16, GSTileSurfaceKind::Depth)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ16S, GSTileSurfaceKind::Depth)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT32, GSTileSurfaceKind::Depth)));
}

TEST(TileGpuByteRoad, RefusesABaseThatIsNotPageAligned)
{
	// Both shaders derive the page from (row, col) off the base page, so a base sitting blocks
	// into a page has no expressible pixel space. Every colour format, not just the 32-bit ones.
	for (u8 psm : {u8(PSMCT32), u8(PSMCT24), u8(PSMCT16), u8(PSMCT16S), u8(PSMZ32), u8(PSMZ24)})
	{
		EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, psm, GSTileSurfaceKind::Color))) << int(psm);
		EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1184, 10, psm, GSTileSurfaceKind::Color))) << int(psm);
	}
}

// The narrower question three OTHER roads ask, kept distinct on purpose. A donor build, the CLUT
// block copy and a rule-2 bind all read an owner's texture as a CT32-shaped window -- 64x32 pages
// of 8x8 blocks -- so they must go on refusing a 16-bit owner even though the byte road now carries
// one. Two predicates, because one predicate serving both is how a 16-bit target would come to be
// reinterpreted through the 32-bit block form and silently produce texels from the wrong bytes.
TEST(TileGpuByteRoad, TheCt32PixelSpaceQuestionStaysNarrow)
{
	EXPECT_TRUE(gsTileSurfaceHasCt32PixelSpace(Layout(0x1180, 10, PSMCT32, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(gsTileSurfaceHasCt32PixelSpace(Layout(0x1180, 10, PSMCT24, GSTileSurfaceKind::Color)));
	EXPECT_FALSE(gsTileSurfaceHasCt32PixelSpace(Layout(0x1180, 10, PSMCT16, GSTileSurfaceKind::Color)));
	EXPECT_FALSE(gsTileSurfaceHasCt32PixelSpace(Layout(0x1180, 10, PSMCT16S, GSTileSurfaceKind::Color)));
	EXPECT_FALSE(gsTileSurfaceHasCt32PixelSpace(Layout(0x1184, 10, PSMCT32, GSTileSurfaceKind::Color)));
	EXPECT_FALSE(gsTileSurfaceHasCt32PixelSpace(Layout(0x1180, 10, PSMZ32, GSTileSurfaceKind::Depth)));
	// ...and it is a subset of the byte road, never the other way round.
	for (u8 psm : {u8(PSMCT32), u8(PSMCT24), u8(PSMCT16), u8(PSMCT16S), u8(PSMZ24), u8(PSMZ16)})
	{
		for (GSTileSurfaceKind k : {GSTileSurfaceKind::Color, GSTileSurfaceKind::Depth})
		{
			const GSTileSurfaceLayout l = Layout(0x1180, 10, psm, k);
			EXPECT_TRUE(!gsTileSurfaceHasCt32PixelSpace(l) || gsTileSurfaceHasByteRoad(l)) << int(psm);
		}
	}
}

// ...and the THIRD predicate, which is the one the CLUT gather asks once it has a 16-bit copy
// geometry. It admits the two 16-bit colour formats; the narrow one above still refuses them, and
// that is the guard against somebody noticing the two look alike and folding them into one.
TEST(TileGpuByteRoad, TheClutGatherPixelSpaceQuestionIsWiderAndTheNarrowOneIsNot)
{
	for (u8 psm : {u8(PSMCT32), u8(PSMCT24), u8(PSMCT16), u8(PSMCT16S)})
	{
		const GSTileSurfaceLayout l = Layout(0x1180, 10, psm, GSTileSurfaceKind::Color);
		EXPECT_TRUE(gsTileSurfaceHasClutGatherPixelSpace(l)) << int(psm);
	}
	// The pair the wider one adds, and the narrow one must GO ON refusing: three other callers read
	// an owner's texture through CT32 block and column forms, and a 16-bit owner reaching one of them
	// produces texels out of the wrong bytes with nothing to say so.
	EXPECT_FALSE(gsTileSurfaceHasCt32PixelSpace(Layout(0x1180, 10, PSMCT16, GSTileSurfaceKind::Color)));
	EXPECT_FALSE(gsTileSurfaceHasCt32PixelSpace(Layout(0x1180, 10, PSMCT16S, GSTileSurfaceKind::Color)));

	// Everything else is refused by both: the base test and the kind test are unchanged, and the
	// 16-bit DEPTH pair has no gather geometry any more than it has a byte road.
	for (u8 psm : {u8(PSMCT32), u8(PSMCT24), u8(PSMCT16), u8(PSMCT16S)})
	{
		EXPECT_FALSE(gsTileSurfaceHasClutGatherPixelSpace(Layout(0x1184, 10, psm, GSTileSurfaceKind::Color)))
			<< int(psm);
		EXPECT_FALSE(gsTileSurfaceHasClutGatherPixelSpace(Layout(0x1180, 10, psm, GSTileSurfaceKind::Depth)))
			<< int(psm);
	}
	for (u8 psm : {u8(PSMZ32), u8(PSMZ24), u8(PSMZ16), u8(PSMZ16S), u8(PSMT8), u8(PSMT4)})
	{
		EXPECT_FALSE(gsTileSurfaceHasClutGatherPixelSpace(Layout(0x1180, 10, psm, GSTileSurfaceKind::Color)))
			<< int(psm);
	}

	// ...and it is a superset of the narrow one and a subset of the byte road, in that order. The
	// three predicates are a chain, and a change that broke the chain would let one road admit a
	// layout the road under it cannot carry.
	for (u8 psm : {u8(PSMCT32), u8(PSMCT24), u8(PSMCT16), u8(PSMCT16S), u8(PSMZ32), u8(PSMZ24), u8(PSMZ16)})
	{
		for (GSTileSurfaceKind k : {GSTileSurfaceKind::Color, GSTileSurfaceKind::Depth})
		{
			for (u32 bp : {0x1180u, 0x1184u})
			{
				const GSTileSurfaceLayout l = Layout(bp, 10, psm, k);
				EXPECT_TRUE(!gsTileSurfaceHasCt32PixelSpace(l) || gsTileSurfaceHasClutGatherPixelSpace(l))
					<< int(psm);
				EXPECT_TRUE(!gsTileSurfaceHasClutGatherPixelSpace(l) || gsTileSurfaceHasByteRoad(l)) << int(psm);
			}
		}
	}
}

TEST(TileGpuByteRoad, TheShaderVariantIsOnePerSwizzleUniverse)
{
	// CT32 and CT24 share a program (same addresses, the byte mask is what differs), and the two
	// 16-bit families do not (different block tables).
	EXPECT_EQ(gsTileByteRoadFormat(PSMCT32), gsTileByteRoadFormat(PSMCT24));
	EXPECT_LT(gsTileByteRoadFormat(PSMCT32), kGSTileByteRoadFormats);
	EXPECT_LT(gsTileByteRoadFormat(PSMCT16), kGSTileByteRoadFormats);
	EXPECT_LT(gsTileByteRoadFormat(PSMCT16S), kGSTileByteRoadFormats);
	EXPECT_NE(gsTileByteRoadFormat(PSMCT16), gsTileByteRoadFormat(PSMCT32));
	EXPECT_NE(gsTileByteRoadFormat(PSMCT16), gsTileByteRoadFormat(PSMCT16S));
	// The 32-bit depth pair shares a program with each other and with NEITHER colour universe: its
	// addresses are CT32's under a block XOR, which is a different program even though it is the
	// same page geometry -- one #define apart, and the two must not collapse to one index or a Z32
	// surface would be written back to its colour twin's blocks.
	EXPECT_EQ(gsTileByteRoadFormat(PSMZ32), gsTileByteRoadFormat(PSMZ24));
	EXPECT_LT(gsTileByteRoadFormat(PSMZ32), kGSTileByteRoadFormats);
	EXPECT_NE(gsTileByteRoadFormat(PSMZ32), gsTileByteRoadFormat(PSMCT32));
	EXPECT_NE(gsTileByteRoadFormat(PSMZ32), gsTileByteRoadFormat(PSMCT16));
	EXPECT_NE(gsTileByteRoadFormat(PSMZ32), gsTileByteRoadFormat(PSMCT16S));
	// Everything else is off the road, and says so with an out-of-range index rather than a 0
	// that would run the CT32 program over the wrong bytes.
	for (u32 psm : {u32(PSMZ16), u32(PSMZ16S), u32(PSMT8), u32(PSMT4), u32(PSMT8H)})
		EXPECT_GE(gsTileByteRoadFormat(psm), kGSTileByteRoadFormats) << psm;
	// And every index the road hands out has a program: the device sizes its pipeline arrays by
	// kGSTileByteRoadFormats, so a format numbered past it dispatches nothing while the model
	// believes the pages travelled.
	for (u32 psm = 0; psm < 64; psm++)
	{
		const u32 rf = gsTileByteRoadFormat(psm);
		EXPECT_TRUE(rf < kGSTileByteRoadFormats || rf == kGSTileByteRoadFormats) << psm;
		if (rf < kGSTileByteRoadFormats)
			EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, static_cast<u8>(psm), GSTileSurfaceKind::Color)))
				<< psm;
	}
}

TEST(TileGpuByteRoad, PageGeometryIsGSLocalMemorysOwn)
{
	// Not a table of literals: the executor's page height has to be the same number GSOffset gives
	// the pool, or the writeback covers a different rectangle than CollectRuns handed it.
	// GSLocalMemory::m_psm is filled by a GSLocalMemory constructor, so one has to exist.
	const GSLocalMemory mem;
	for (u32 psm : {u32(PSMCT32), u32(PSMCT24), u32(PSMCT16), u32(PSMCT16S), u32(PSMZ32), u32(PSMZ24)})
	{
		EXPECT_EQ(gsTilePageHeight(psm), static_cast<u32>(GSLocalMemory::m_psm[psm].pgs.y)) << psm;
		EXPECT_EQ(GSLocalMemory::m_psm[psm].pgs.x, 64) << psm;
	}
	EXPECT_EQ(gsTilePageHeight(PSMCT32), 32u);
	EXPECT_EQ(gsTilePageHeight(PSMCT16), 64u);
	EXPECT_EQ(gsTilePageHeight(PSMCT16S), 64u);
	// The depth pair's page is its colour twin's, which is what lets arm 3 be arm 0 plus a term.
	EXPECT_EQ(gsTilePageHeight(PSMZ32), gsTilePageHeight(PSMCT32));
	EXPECT_EQ(gsTilePageHeight(PSMZ24), gsTilePageHeight(PSMCT32));
}

TEST(TileGpuByteRoad, TheWritebackDispatchCoversExactlyOnePage)
{
	// The compute pass is TileGpuWritebackGroupDim square invocations per workgroup, and that is a
	// DEVICE answer -- 16 on Adreno and everything unmeasured, 8 on Mali -- so the coverage claim has
	// to hold at every value a device may name, not at one constant. A 32-bit page is one texel per
	// invocation (2048 texels); a 16-bit page is one WORD per invocation -- the two texels 8 apart
	// that share it -- which is also 2048. Both must come out to the page's word count exactly at
	// EVERY dim: short and the ring keeps stale words, long and a neighbour's page is overwritten.
	for (u32 dim : {kGSTileWritebackGroupDimDefault, kGSTileWritebackGroupDimMali})
	{
		SCOPED_TRACE(dim);
		// The workgroup must tile the page in whole 8x8-word BLOCKS, because the shader's shared-memory
		// stage gathers a block at a time: a workgroup holding half a block has no one to write the
		// other half's quads. Held here as well as by the static_asserts beside the constants, so that a
		// retune that slips past a header change fails a test rather than a title.
		EXPECT_TRUE(gsTileWritebackGroupDimFits(dim));
		EXPECT_EQ(dim % 8u, 0u);
		EXPECT_GE(dim, 8u);

		for (u32 psm : {u32(PSMCT32), u32(PSMCT24), u32(PSMCT16), u32(PSMCT16S), u32(PSMZ32), u32(PSMZ24)})
		{
			const GSTileDispatch2D groups = gsTileWritebackGroups(psm, dim);
			EXPECT_EQ(groups.x * dim * groups.y * dim, 2048u) << psm;
			// ...and the grid is the page's own shape in words: 64 wide for a 32-bit page, 32 for the
			// 16-bit pair-packed one.
			const u32 want_x = (gsTileStorageBpp(psm) == 32) ? 64u : 32u;
			EXPECT_EQ(groups.x * dim, want_x) << psm;
			EXPECT_EQ(groups.y * dim, gsTilePageHeight(psm)) << psm;
		}
	}
	// A dim that does NOT tile the page has to be rejected rather than silently truncating the
	// dispatch, which is the failure the predicate exists to catch: 1, 4 and 12 are not whole 8x8
	// blocks, and 24 and 48 divide neither page word extent.
	for (u32 bad : {0u, 1u, 4u, 12u, 24u, 48u})
		EXPECT_FALSE(gsTileWritebackGroupDimFits(bad)) << bad;
}

// -- what a 16-bit frame STORES, as the draw now has to say ---------------------------------------
//
// A TileGpu colour target is an RGBA8 image whatever the guest format is, so a 16-bit frame's pixels
// live there expanded. Every road that puts bytes into such a target already agreed on the form --
// the writeback packs `byte >> 3`, the seed unpacks `bits << 3`, and gsTilePack5551 /
// gsTileUnpack5551 above are the CPU spelling of the same pair. The DRAW did not: it wrote eight
// bits a channel, so the image held precision the console never stored and everything that read the
// target back (a destination blend, DATE, a TCC=1 sample) saw it.
//
// These pin the draw's quantisation to the roads it has to agree with, rather than to a restatement
// of the format.

TEST(TileGpuCT16Road, DrawQuantisationIsTheWritebackAndSeedRoundTrip)
{
	// The strong form: quantising a byte and then sending it out through the writeback's pack and
	// back through the seed's unpack has to be the same value. If the draw quantised by rounding
	// where the pack truncates, or kept a sixth bit, this is where the two roads would disagree --
	// and the disagreement would only ever show as a target that reads back differently from the
	// guest bytes under it.
	for (u32 r = 0; r < 256; r++)
	{
		for (u32 a = 0; a < 256; a += 5)
		{
			const u32 px = r | (r << 8) | (r << 16) | (a << 24);
			const u32 q = gsTileGpuQuantise5551(px);
			EXPECT_EQ(gsTileUnpack5551(gsTilePack5551(q)), q) << "r=" << r << " a=" << a;
			// ...and quantising is idempotent, which is what makes a target that has been written,
			// read back and written again stable rather than drifting.
			EXPECT_EQ(gsTileGpuQuantise5551(q), q) << "r=" << r << " a=" << a;
			// ...and it agrees with the pack/unpack pair applied to the UNquantised pixel, which is
			// what the writeback would have stored for the draw before this existed.
			EXPECT_EQ(gsTileUnpack5551(gsTilePack5551(px)), q) << "r=" << r << " a=" << a;
		}
	}
}

// The CLUT gather's own use of the same pair: a palette word read off a 16-bit owner is TWO cells,
// packed and concatenated. Pinned here rather than in the gather's own file because it is the pack
// pair's statement, and the pair is this file's business -- and because the low/high order is the one
// thing about it that is not derivable from either half alone.
TEST(TileGpuCT16Road, AClutWordIsTheLowCellThenTheHigh)
{
	// A guest word of a 16-bit surface is halfword 2n in its low half and 2n+1 in its high half, so a
	// palette word is pack(low) | pack(high) << 16 and never the other way round. Swapping them is a
	// palette of plausible colours in the wrong order, which is exactly the failure this file's
	// round-trip test cannot see.
	const u32 samples[][2] = {{0x00000000u, 0xFFFFFFFFu}, {0xFFFFFFFFu, 0x00000000u}, {0x80402010u, 0x01020304u},
		{0x00FFFFFFu, 0xFF000000u}, {0xF8F8F8F8u, 0x12345678u}};
	for (const auto& s : samples)
	{
		const u32 want = static_cast<u32>(gsTilePack5551(s[0])) | (static_cast<u32>(gsTilePack5551(s[1])) << 16);
		EXPECT_EQ(gsTileClut16Word(s[0], s[1]), want);
		EXPECT_EQ(gsTileClut16Word(s[0], s[1]) & 0xFFFFu, gsTilePack5551(s[0]));
		EXPECT_EQ(gsTileClut16Word(s[0], s[1]) >> 16, gsTilePack5551(s[1]));
	}
	// ...and the whole 16-bit value space round-trips through the pair, which is what makes the
	// gather's test able to seed a target image from one number.
	for (u32 v = 0; v < 0x10000u; v++)
	{
		const u32 cell = gsTileUnpack5551(static_cast<u16>(v));
		ASSERT_EQ(gsTilePack5551(cell), static_cast<u16>(v)) << "cell 0x" << std::hex << v;
	}
}

TEST(TileGpuCT16Road, DrawQuantisationTruncatesAndTakesAlphasTopBitOnly)
{
	// Truncation, not rounding: the console drops the low bits. 0xFF must not become 0x100.
	EXPECT_EQ(gsTileGpuQuantise5551(0x00FFFFFFu), 0x00F8F8F8u);
	EXPECT_EQ(gsTileGpuQuantise5551(0x00070707u), 0u);
	EXPECT_EQ(gsTileGpuQuantise5551(0x00F9FAFBu), 0x00F8F8F8u);
	// Alpha keeps one bit, and it is the top one.
	EXPECT_EQ(gsTileGpuQuantise5551(0x7F000000u), 0u);
	EXPECT_EQ(gsTileGpuQuantise5551(0x80000000u), 0x80000000u);
	EXPECT_EQ(gsTileGpuQuantise5551(0xFF000000u), 0x80000000u);
}

TEST(TileGpuCT16Road, OnlyTheSixteenBitFamilyQuantises)
{
	// fmt is GSLocalMemory::m_psm[PSM].fmt: 0 = 32-bit, 1 = 24-bit, 2 = the 16-bit family. A 24-bit
	// frame stores eight bits a channel and must NOT be quantised -- it is the alpha byte it does
	// not keep, and the write mask is what says so.
	EXPECT_FALSE(gsTileGpuFrameQuantises(0));
	EXPECT_FALSE(gsTileGpuFrameQuantises(1));
	EXPECT_TRUE(gsTileGpuFrameQuantises(2));

	// ...and against the format table itself, so a PSM joining the family cannot be missed.
	// GSLocalMemory::m_psm is filled by a GSLocalMemory constructor, so one has to exist -- without
	// it every fmt reads zero and this test passes for PSMCT32 by accident while failing here.
	const GSLocalMemory mem;
	EXPECT_TRUE(gsTileGpuFrameQuantises(GSLocalMemory::m_psm[PSMCT16].fmt));
	EXPECT_TRUE(gsTileGpuFrameQuantises(GSLocalMemory::m_psm[PSMCT16S].fmt));
	EXPECT_FALSE(gsTileGpuFrameQuantises(GSLocalMemory::m_psm[PSMCT32].fmt));
	EXPECT_FALSE(gsTileGpuFrameQuantises(GSLocalMemory::m_psm[PSMCT24].fmt));
}

// -- the destination-alpha test, folded before the draw ------------------------------------------
//
// The tree had no test for DATE at all, on any road, and the one thing about it that is easy to get
// backwards is the 24-bit case. It is not "there is no alpha bit, so everything passes" -- it is
// "nothing passes, the draw is ignored", for BOTH values of DATM, measured on a PS2. The software
// renderer says so in as many words (GSRendererSW::GetScanlineGlobalData, "When the format is 24bit
// (Z or C), DATE ceases to function ... after testing this on a PS2 it turns out nothing passes").

TEST(TileGpuDate, TwentyFourBitFrameDropsTheDrawForBothDatmValues)
{
	// trbpp 24 is the two 24-bit frame formats and only those, which is the same set the software
	// renderer names as (PSM & 0xF) == PSMCT24 -- so asking trbpp is asking its question.
	const GSLocalMemory mem;
	for (u32 psm : {u32(PSMCT24), u32(PSMZ24)})
	{
		EXPECT_EQ(GSLocalMemory::m_psm[psm].trbpp, 24u) << psm;
		for (u32 datm = 0; datm < 2; datm++)
			EXPECT_EQ(gsTileFoldDate(true, datm, GSLocalMemory::m_psm[psm].trbpp), GSTileDateFold::DropDraw)
				<< "psm " << psm << " datm " << datm;
	}
	// ...and the formats that DO carry an alpha bit must not be dropped.
	for (u32 psm : {u32(PSMCT32), u32(PSMCT16), u32(PSMCT16S)})
	{
		EXPECT_NE(GSLocalMemory::m_psm[psm].trbpp, 24u) << psm;
		EXPECT_NE(gsTileFoldDate(true, 0, GSLocalMemory::m_psm[psm].trbpp), GSTileDateFold::DropDraw) << psm;
	}
}

TEST(TileGpuDate, DatmPicksWhichAlphaBitValuePasses)
{
	EXPECT_EQ(gsTileFoldDate(false, 0, 32), GSTileDateFold::Off);
	EXPECT_EQ(gsTileFoldDate(false, 1, 32), GSTileDateFold::Off);
	EXPECT_EQ(gsTileFoldDate(true, 0, 32), GSTileDateFold::PassOnAlphaClear);
	EXPECT_EQ(gsTileFoldDate(true, 1, 32), GSTileDateFold::PassOnAlphaSet);
	EXPECT_EQ(gsTileFoldDate(true, 0, 16), GSTileDateFold::PassOnAlphaClear);
	EXPECT_EQ(gsTileFoldDate(true, 1, 16), GSTileDateFold::PassOnAlphaSet);
}

TEST(TileGpuDate, TheStateRowCarriesTheFoldAndNothingElse)
{
	// The fragment stage switches on 0 / 1 / 2 and reads "pass where the alpha MSB is set" as row 2.
	// A renumbering here inverts the test on every DATE draw in the corpus and nothing else says so.
	EXPECT_EQ(gsTileGpuDateRow(GSTileDateFold::Off), 0u);
	EXPECT_EQ(gsTileGpuDateRow(GSTileDateFold::PassOnAlphaClear), 1u);
	EXPECT_EQ(gsTileGpuDateRow(GSTileDateFold::PassOnAlphaSet), 2u);
	// DropDraw never reaches a row: the caller returns before one is written. Zero is what lands if
	// anybody ever forgets, which is "no test" -- the draw would render, but it would not render
	// with the test inverted.
	EXPECT_EQ(gsTileGpuDateRow(GSTileDateFold::DropDraw), 0u);
}

TEST(TileGpuDate, TheAlphaBitTheTestReadsIsTheBitEachFormatStores)
{
	// The shader decides on "the destination alpha byte >= 128", and the destination is the target's
	// expanded RGBA8. For a 32-bit frame that byte is the guest's alpha byte, so bit 7 is bit 31 of
	// the word. For a 16-bit frame the target holds the one stored alpha bit expanded to 0x80, which
	// is the same threshold -- and both the seed and the draw's own quantisation put it there.
	EXPECT_GE(gsTileUnpack5551(0x8000u) >> 24, 128u);
	EXPECT_LT(gsTileUnpack5551(0x7FFFu) >> 24, 128u);
	EXPECT_GE(gsTileGpuQuantise5551(0xFF000000u) >> 24, 128u);
	EXPECT_LT(gsTileGpuQuantise5551(0x7F000000u) >> 24, 128u);
}
