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

namespace
{
constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm, GSTileSurfaceKind kind)
{
	return GSTileSurfaceLayout{bp, bw, psm, kind};
}
} // namespace

TEST(TileGpuByteRoad, ServesEveryColourFormatThePoolCanBack)
{
	// The four colour formats with 64-pixel-wide pages. Depth has no writeback shader at all --
	// a separate, standing gap -- and the index formats never reach the pool.
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT32, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT24, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT16, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMCT16S, GSTileSurfaceKind::Color)));

	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ32, GSTileSurfaceKind::Depth)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ24, GSTileSurfaceKind::Depth)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ16, GSTileSurfaceKind::Depth)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Layout(0x1180, 10, PSMZ16S, GSTileSurfaceKind::Depth)));
}

TEST(TileGpuByteRoad, RefusesABaseThatIsNotPageAligned)
{
	// Both shaders derive the page from (row, col) off the base page, so a base sitting blocks
	// into a page has no expressible pixel space. Every colour format, not just the 32-bit ones.
	for (u8 psm : {u8(PSMCT32), u8(PSMCT24), u8(PSMCT16), u8(PSMCT16S)})
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
	// Everything else is off the road, and says so with an out-of-range index rather than a 0
	// that would run the CT32 program over the wrong bytes.
	for (u32 psm : {u32(PSMZ32), u32(PSMZ24), u32(PSMZ16), u32(PSMZ16S), u32(PSMT8), u32(PSMT4), u32(PSMT8H)})
		EXPECT_GE(gsTileByteRoadFormat(psm), kGSTileByteRoadFormats) << psm;
}

TEST(TileGpuByteRoad, PageGeometryIsGSLocalMemorysOwn)
{
	// Not a table of literals: the executor's page height has to be the same number GSOffset gives
	// the pool, or the writeback covers a different rectangle than CollectRuns handed it.
	// GSLocalMemory::m_psm is filled by a GSLocalMemory constructor, so one has to exist.
	const GSLocalMemory mem;
	for (u32 psm : {u32(PSMCT32), u32(PSMCT24), u32(PSMCT16), u32(PSMCT16S)})
	{
		EXPECT_EQ(gsTilePageHeight(psm), static_cast<u32>(GSLocalMemory::m_psm[psm].pgs.y)) << psm;
		EXPECT_EQ(GSLocalMemory::m_psm[psm].pgs.x, 64) << psm;
	}
	EXPECT_EQ(gsTilePageHeight(PSMCT32), 32u);
	EXPECT_EQ(gsTilePageHeight(PSMCT16), 64u);
	EXPECT_EQ(gsTilePageHeight(PSMCT16S), 64u);
}

TEST(TileGpuByteRoad, TheWritebackDispatchCoversExactlyOnePage)
{
	// The compute pass is 8x8 invocations per workgroup. A 32-bit page is one texel per invocation
	// (2048 texels); a 16-bit page is one WORD per invocation -- the two texels 8 apart that share
	// it -- which is also 2048. Both must come out to the page's word count exactly: short and the
	// ring keeps stale words, long and a neighbour's page is overwritten.
	for (u32 psm : {u32(PSMCT32), u32(PSMCT24), u32(PSMCT16), u32(PSMCT16S)})
	{
		const GSTileDispatch2D groups = gsTileWritebackGroups(psm);
		EXPECT_EQ(groups.x * 8u * groups.y * 8u, 2048u) << psm;
		// ...and the grid is the page's own shape in words: 64 wide for a 32-bit page, 32 for the
		// 16-bit pair-packed one.
		const u32 want_x = (gsTileStorageBpp(psm) == 32) ? 64u : 32u;
		EXPECT_EQ(groups.x * 8u, want_x) << psm;
		EXPECT_EQ(groups.y * 8u, gsTilePageHeight(psm)) << psm;
	}
}
