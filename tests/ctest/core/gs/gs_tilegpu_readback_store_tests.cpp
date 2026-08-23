// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSTileTargetPool::ReadbackPages — the store road must write inside the pages it was asked for.
//
// A readback brings a surface's pixels back down into CPU local memory. Both of its roads (the
// out-of-band copy and the drain) end in the same two writers: GSLocalMemory::WritePixel32 for a
// colour surface, WritePixel16 for a 16-bit depth one. Those writers walk GSOffset's pixel index,
// and that index counts pixels in the LAYOUT'S OWN format — 16-bit cells for a 16-bit format,
// 32-bit cells for a 32-bit one. WritePixel32 indexes vm32(), so pairing it with a 16-bit layout
// multiplies every destination byte offset by two and writes four bytes where two belong.
//
// That is not a wrong pixel, it is a wrong PAGE. Measured on OutRun 2006 (2026-08-22, TileGpu on
// M2/Asahi): a PSMCT16S surface based at page 380 wrote GS pages 293 and 297 — 87 pages outside
// its own footprint, through the wrapped VM alias so no watchpoint on the primary mapping even
// saw it. Those two pages are the game's PSMT8H fog-index plane; zeroed, they read as fog index 0,
// which is the maximum-haze end of the ramp, and the frame grows sky-lavender stripes across the
// bottom band that spread from 0 to 2 to 7 pages over three frames as the corruption is seeded
// into surfaces and written back out again.
//
// The upload road never had this problem: it goes through GSLocalMemory::ReadTexture, which
// dispatches on the format. Only the readback's store hard-codes the 32-bit writer for colour.
//
// So the pool declares which layouts its store road can address, and these tests hold that
// declaration to the property it stands for, in both directions: nothing it accepts may leave its
// pages, and it may not refuse a layout that would have stayed inside them.

#include "GS/Renderers/Tile/GSTileTargetPool.h"

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm, GSTileSurfaceKind kind)
{
	return GSTileSurfaceLayout{bp, bw, psm, kind};
}

struct Case
{
	const char* name;
	GSTileSurfaceLayout layout;
};

// Every layout family the pool serves (PoolSupports: 16 bpp and up), based far enough into GS
// memory that a doubled byte offset cannot land back inside the surface by luck.
const Case kCases[] = {
	{"PSMCT32", Layout(4800, 10, PSMCT32, GSTileSurfaceKind::Color)},
	{"PSMCT24", Layout(4800, 10, PSMCT24, GSTileSurfaceKind::Color)},
	{"PSMCT16", Layout(4800, 10, PSMCT16, GSTileSurfaceKind::Color)},
	{"PSMCT16S", Layout(12160, 10, PSMCT16S, GSTileSurfaceKind::Color)},
	{"PSMZ32", Layout(4800, 10, PSMZ32, GSTileSurfaceKind::Depth)},
	{"PSMZ24", Layout(4800, 10, PSMZ24, GSTileSurfaceKind::Depth)},
	{"PSMZ16", Layout(4800, 10, PSMZ16, GSTileSurfaceKind::Depth)},
	{"PSMZ16S", Layout(12160, 10, PSMZ16S, GSTileSurfaceKind::Depth)},
};

// The pages a readback of `pages` would actually write, spelled out the way ReadbackPages spells
// it: CollectRuns for the pixel rects, GSOffset::pa for each pixel's index in the layout's own
// units, and the cell width of the writer the store road picks for this surface kind.
GSPageBitmap StoreTouchedPages(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages)
{
	std::vector<GSVector4i> runs;
	GSTileTargetPool::CollectRuns(layout, pages, GSVramModel::kFullBlockMask, runs);

	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	const bool depth16 =
		layout.kind == GSTileSurfaceKind::Depth && (layout.psm == PSMZ16 || layout.psm == PSMZ16S);
	const u64 cell = depth16 ? sizeof(u16) : sizeof(u32);

	GSPageBitmap touched;
	for (const GSVector4i& r : runs)
	{
		for (int y = r.y; y < r.w; y++)
		{
			for (int x = r.x; x < r.z; x++)
			{
				const u64 first = static_cast<u64>(off.pa(x, y)) * cell;
				// GS local memory is a wrapped mapping, so an address past 4 MB does not fault --
				// it aliases. Fold it the way the hardware mapping does.
				for (u64 b = first; b < first + cell; b++)
					touched.set(static_cast<u32>((b / GS_PAGE_SIZE) % GS_MAX_PAGES));
			}
		}
	}
	return touched;
}

// A few pages inside the surface, one page-row down from its base so the run set is not the
// degenerate first row.
GSPageBitmap SomePagesOf(const GSTileSurfaceLayout& layout)
{
	const u32 base = layout.bp >> 5;
	GSPageBitmap pages;
	pages.set(base + layout.bw + 3);
	pages.set(base + layout.bw + 7);
	return pages;
}
} // namespace

TEST(TileGpuReadbackStore, AcceptedLayoutsStoreOnlyInsideTheirOwnPages)
{
	for (const Case& c : kCases)
	{
		if (!GSTileTargetPool::ReadbackAddressable(c.layout))
			continue;
		const GSPageBitmap pages = SomePagesOf(c.layout);
		const GSPageBitmap escaped = StoreTouchedPages(c.layout, pages).andnot(pages);
		EXPECT_TRUE(escaped.empty()) << c.name << ": readback store wrote " << escaped.count()
									 << " page(s) it was not asked for";
	}
}

TEST(TileGpuReadbackStore, RefusesExactlyTheLayoutsItCannotAddress)
{
	// Both directions. The first half is the bug; the second stops the repair from degenerating
	// into "refuse everything", which would silently retire the readback road altogether.
	for (const Case& c : kCases)
	{
		const GSPageBitmap pages = SomePagesOf(c.layout);
		const bool stays_inside = StoreTouchedPages(c.layout, pages).andnot(pages).empty();
		EXPECT_EQ(GSTileTargetPool::ReadbackAddressable(c.layout), stays_inside)
			<< c.name << ": the pool's declaration disagrees with where its store road lands";
	}
}

TEST(TileGpuReadbackStore, SixteenBitColourIsTheRefusedCase)
{
	// Named on its own so the reason survives a future widening of the case table: 16-bit COLOUR
	// is the gap (no 16-bit writer on the colour road), and 16-bit DEPTH is not (WritePixel16).
	EXPECT_FALSE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMCT16, GSTileSurfaceKind::Color)));
	EXPECT_FALSE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMCT16S, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMZ16, GSTileSurfaceKind::Depth)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMZ16S, GSTileSurfaceKind::Depth)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMCT32, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMCT24, GSTileSurfaceKind::Color)));
}
