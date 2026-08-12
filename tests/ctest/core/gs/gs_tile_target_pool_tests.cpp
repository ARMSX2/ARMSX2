// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSTileTargetPool::CollectRuns — the pixel rects a spill actually moves.
//
// The model tracks GPU truth on two axes: which BYTES of a cell (the plane window)
// and, through the block sidecar, which of a page's 32 physical BLOCKS. A transfer
// that overwrites part of a truth page shrinks that page's truth block-wise instead
// of forcing a readback, which is only sound if the readback then moves those blocks
// and no others. It did not: runs were built per whole page, so pulling a partially-
// truth page wrote the GPU's stale copy back over the blocks the CPU had just been
// given. The gs-sync console capture (SCPH-30001, 2026-08-12) caught it as an
// ordering defect — an upload over a region a draw had just filled came back 0 of
// 2048 pixels fresh under Tile where console, SW and Classic are all 2048 of 2048.
//
// These pin the block refinement itself: the runs must cover exactly the masked
// blocks, in the format's own swizzle order, for every layout the pool serves.

#include "GS/Renderers/Tile/GSTileTargetPool.h"

#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

namespace
{
constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm, GSTileSurfaceKind kind = GSTileSurfaceKind::Color)
{
	return GSTileSurfaceLayout{bp, bw, psm, kind};
}

struct Geom
{
	GSVector2i pgs;
	GSVector2i bs;
};

// GSVector4i has no gtest-usable operator==; compare the four edges.
::testing::AssertionResult RectEq(const GSVector4i& got, const GSVector4i& want)
{
	if (got.x == want.x && got.y == want.y && got.z == want.z && got.w == want.w)
		return ::testing::AssertionSuccess();
	return ::testing::AssertionFailure() << "got (" << got.x << "," << got.y << "," << got.z << "," << got.w
										 << ") want (" << want.x << "," << want.y << "," << want.z << "," << want.w
										 << ")";
}

Geom GeomOf(const GSTileSurfaceLayout& layout)
{
	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	return Geom{GSVector2i(1 << off.pageShiftX(), 1 << off.pageShiftY()),
		GSVector2i(1 << off.blockShiftX(), 1 << off.blockShiftY())};
}

// Every pixel the runs cover, as a set. Independent of how the runs were shaped:
// coalescing is free to change, coverage is not.
std::set<std::pair<int, int>> PixelsOf(const std::vector<GSVector4i>& runs)
{
	std::set<std::pair<int, int>> px;
	for (const GSVector4i& r : runs)
	{
		for (int y = r.y; y < r.w; y++)
		{
			for (int x = r.x; x < r.z; x++)
				EXPECT_TRUE(px.emplace(x, y).second) << "run overlap at " << x << "," << y;
		}
	}
	return px;
}

// The reference: brute-force every pixel of every set page and keep it when its own
// physical block is in the mask. Uses GSOffset::bn directly — the same question the
// swizzle tables answer, asked without the run machinery.
std::set<std::pair<int, int>> ReferencePixels(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages,
	u32 block_mask)
{
	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	const Geom g = GeomOf(layout);
	const u32 base = (layout.bp >> 5) & (GS_MAX_PAGES - 1);

	std::set<std::pair<int, int>> px;
	pages.forEachSetPage([&](u32 page) {
		const u32 rel = (page + GS_MAX_PAGES - base) & (GS_MAX_PAGES - 1);
		const int row = static_cast<int>(rel) / layout.bw;
		const int col = static_cast<int>(rel) % layout.bw;
		const int px0 = col * g.pgs.x;
		const int py0 = row * g.pgs.y;
		for (int y = 0; y < g.pgs.y; y++)
		{
			for (int x = 0; x < g.pgs.x; x++)
			{
				const u32 bn = off.bn(px0 + x, py0 + y);
				if (block_mask & (1u << (bn & 31)))
					px.emplace(px0 + x, py0 + y);
			}
		}
	});
	return px;
}

void ExpectRunsMatch(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages, u32 block_mask)
{
	std::vector<GSVector4i> runs;
	GSTileTargetPool::CollectRuns(layout, pages, block_mask, runs);
	EXPECT_EQ(PixelsOf(runs), ReferencePixels(layout, pages, block_mask));
}

const u8 kFormats[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};

GSTileSurfaceKind KindOf(u8 psm)
{
	return (psm == PSMZ32 || psm == PSMZ24 || psm == PSMZ16 || psm == PSMZ16S) ? GSTileSurfaceKind::Depth :
																				 GSTileSurfaceKind::Color;
}
} // namespace

// A full mask must keep the whole-page behaviour byte for byte — the hot path is not
// allowed to change shape when the refinement lands.
TEST(GSTileTargetPoolRuns, FullMaskCoversWholePages)
{
	for (const u8 psm : kFormats)
	{
		const GSTileSurfaceLayout layout = Layout(0, 8, psm, KindOf(psm));
		GSPageBitmap pages;
		pages.set(0);
		pages.set(1);
		pages.set(9); // next page row, so the runs cannot merge

		std::vector<GSVector4i> runs;
		GSTileTargetPool::CollectRuns(layout, pages, GSVramModel::kFullBlockMask, runs);

		const Geom g = GeomOf(layout);
		ASSERT_EQ(runs.size(), 2u) << "psm " << static_cast<int>(psm); // 0..1 coalesce, 9 stands alone
		EXPECT_TRUE(RectEq(runs[0], GSVector4i(0, 0, 2 * g.pgs.x, g.pgs.y)));
		EXPECT_TRUE(RectEq(runs[1], GSVector4i(g.pgs.x, g.pgs.y, 2 * g.pgs.x, 2 * g.pgs.y)));
		EXPECT_EQ(PixelsOf(runs), ReferencePixels(layout, pages, GSVramModel::kFullBlockMask));
	}
}

// The defect itself, in the shape gs-sync's cell 3 produced: a draw owns the whole
// page, then a transfer overwrites its left half. Truth shrinks to the blocks the
// transfer did not touch, and the spill must move only those.
TEST(GSTileTargetPoolRuns, PartialMaskMovesOnlyItsBlocks)
{
	for (const u8 psm : kFormats)
	{
		const GSTileSurfaceLayout layout = Layout(0, 4, psm, KindOf(psm));
		const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
		const Geom g = GeomOf(layout);

		// The blocks of the page's right half — what survives as truth when the
		// left half is transferred over.
		u32 right_half = 0;
		for (int y = 0; y < g.pgs.y; y += g.bs.y)
		{
			for (int x = g.pgs.x / 2; x < g.pgs.x; x += g.bs.x)
				right_half |= 1u << (off.bn(x, y) & 31);
		}
		ASSERT_NE(right_half, GSVramModel::kFullBlockMask) << "psm " << static_cast<int>(psm);

		GSPageBitmap pages;
		pages.set(0);
		pages.set(1);

		std::vector<GSVector4i> runs;
		GSTileTargetPool::CollectRuns(layout, pages, right_half, runs);

		// Coverage is exactly the right half of both pages, and — the part that was
		// broken — the left half is never written.
		EXPECT_EQ(PixelsOf(runs), ReferencePixels(layout, pages, right_half));
		for (const GSVector4i& r : runs)
		{
			EXPECT_GE(r.x % g.pgs.x, g.pgs.x / 2) << "psm " << static_cast<int>(psm)
												  << ": a run reaches into the CPU-newest half";
		}
	}
}

// Block masks the swizzle scatters: a single block, alternating blocks, and the
// complement of the case above. Nothing here is contiguous in pixel space.
TEST(GSTileTargetPoolRuns, ScatteredMasksMatchTheSwizzle)
{
	for (const u8 psm : kFormats)
	{
		const GSTileSurfaceLayout layout = Layout(0, 2, psm, KindOf(psm));
		GSPageBitmap pages;
		pages.set(0);

		for (u32 b = 0; b < GSVramModel::kBlocksPerPage; b++)
			ExpectRunsMatch(layout, pages, 1u << b);

		ExpectRunsMatch(layout, pages, 0xAAAAAAAAu);
		ExpectRunsMatch(layout, pages, 0x55555555u);
		ExpectRunsMatch(layout, pages, 0x0000FFFFu);
		ExpectRunsMatch(layout, pages, 0xFFFF0000u);
	}
}

// An empty mask moves nothing at all — the caller relies on this to skip a plane
// whose truth a transfer wholly consumed.
TEST(GSTileTargetPoolRuns, EmptyMaskMovesNothing)
{
	GSPageBitmap pages;
	pages.set(3);
	std::vector<GSVector4i> runs;
	EXPECT_EQ(GSTileTargetPool::CollectRuns(Layout(0, 8, PSMCT32), pages, 0, runs), 0u);
	EXPECT_TRUE(runs.empty());
}

// A non-zero base page: the runs are surface-relative, and the block swizzle is
// page-periodic, so neither the rects nor the mask meaning may drift with bp.
TEST(GSTileTargetPoolRuns, BaseOffsetDoesNotMoveTheBlocks)
{
	const GSTileSurfaceLayout layout = Layout(32 * 20, 4, PSMCT32);
	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));

	u32 top_row = 0;
	for (int x = 0; x < 64; x += 8)
		top_row |= 1u << (off.bn(x, 0) & 31);

	GSPageBitmap pages;
	pages.set(20); // the surface's own first page
	pages.set(21);
	ExpectRunsMatch(layout, pages, top_row);

	std::vector<GSVector4i> runs;
	GSTileTargetPool::CollectRuns(layout, pages, top_row, runs);
	for (const GSVector4i& r : runs)
		EXPECT_EQ(r.y, 0) << "the top block row must sit at surface-relative y 0";
}
