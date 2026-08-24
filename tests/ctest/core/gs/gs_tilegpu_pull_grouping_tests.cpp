// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// How a CPU readback's page set becomes pool calls, which is the same thing as how many times the
// GS thread blocks on the GPU to serve it.
//
// A readback's price is the device round trip, not the bytes. Every pool call ends in a
// submit-and-wait -- a drain of the frame's command buffer, or an out-of-band submit plus its own
// fence -- so what a readback costs is the NUMBER OF CALLS it issues, and the page set it asks for
// barely matters. That makes the grouping rule a performance contract, and this file pins it in
// both directions:
//
//  - Pages sharing an owner surface, a truth block mask and a byte window collapse into ONE call.
//    The pool has always taken a page bitmap and derived its copy rects from it; asking page by
//    page pays a fresh round trip for each. Beyond Good & Evil's one local read a frame is 30
//    pages, and was 30 round trips (238 device readbacks a run, measured on M2/Asahi 2026-08-24).
//
//  - ...EXCEPT where a page needs more than one call, and that exception is a correctness one, not
//    a heuristic. Two calls on one page can have OVERLAPPING byte windows: a depth plane's
//    PlaneByteMask is all four bytes of the cell and a colour plane's is the low three, so when a
//    page is aliased by a colour owner and a depth owner the depth store lands on top of the
//    colour one and the ORDER of the two decides the bytes. Bucketing across pages can float the
//    second ahead of the first -- one page contributing only the depth group creates that bucket
//    first, and a later page's colour write then lands behind it. So a page with two or more calls
//    keeps them to itself, in plane order. Pages are disjoint guest memory, so nothing else is
//    ordered against anything.
//
// The second property is the one that will be quietly broken by a future simplification, because
// the arrangement it protects looks redundant until you know what PlaneByteMask returns for Z.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include "GS/Renderers/Tile/GSTileTypes.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
using PullCall = GSRendererTileGpu::PullCall;

constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm, GSTileSurfaceKind kind)
{
	return GSTileSurfaceLayout{bp, bw, psm, kind};
}

/// A colour surface whose pixel rect spans `pages_high` page-rows of a 1-page-wide layout, so its
/// page set is a simple contiguous run from `bp / 32`.
GSTileSurfaceId MakeColour(GSVramModel& m, u32 bp, int rows, u32 handle)
{
	return m.Create(Layout(bp, 1, PSMCT32, GSTileSurfaceKind::Color), GSVector4i(0, 0, 64, 32 * rows), handle);
}

std::vector<u32> PagesOf(const GSPageBitmap& b)
{
	std::vector<u32> out;
	b.forEachSetPage([&out](u32 p) { out.push_back(p); });
	return out;
}

GSPageBitmap Bits(std::initializer_list<u32> pages)
{
	GSPageBitmap b;
	for (const u32 p : pages)
		b.set(p);
	return b;
}
} // namespace

// Every page of one owner, one block mask, one byte window: one call, not one per page. This is
// the whole coalescing claim, and the number it moves is the blocking-wait count.
TEST(GSTileGpuPullGrouping, PagesOfOneOwnerCollapseToASingleCall)
{
	GSVramModel m;
	const GSTileSurfaceId id = MakeColour(m, 0, 8, 1);
	const GSPageBitmap drawn = Bits({0, 1, 2, 3, 4, 5, 6, 7});
	m.OnNativeDraw(id, drawn, kGSTilePlanesAll);

	std::vector<PullCall> calls;
	GSRendererTileGpu::PlanPull(m, m.ReadbackNeeded(drawn, kGSTilePlanesAll), calls);

	ASSERT_EQ(calls.size(), 1u) << "eight pages of one surface must be one round trip, not eight";
	EXPECT_EQ(calls[0].owner, id);
	EXPECT_EQ(calls[0].block_mask, GSVramModel::kFullBlockMask);
	EXPECT_EQ(PagesOf(calls[0].pages), (std::vector<u32>{0, 1, 2, 3, 4, 5, 6, 7}));
}

// Two owners, disjoint pages: one call each, and neither drags the other's pages along. The merge
// key is the owner, so a wider page set can never widen a call onto a texture that does not hold
// those pages -- which would assert in the pool and read garbage if it did not.
TEST(GSTileGpuPullGrouping, TwoOwnersStayTwoCallsCarryingOnlyTheirOwnPages)
{
	GSVramModel m;
	const GSTileSurfaceId a = MakeColour(m, 0, 4, 1);
	const GSTileSurfaceId b = MakeColour(m, 4 * 32, 4, 2);
	m.OnNativeDraw(a, Bits({0, 1, 2, 3}), kGSTilePlanesAll);
	m.OnNativeDraw(b, Bits({4, 5, 6, 7}), kGSTilePlanesAll);

	std::vector<PullCall> calls;
	GSRendererTileGpu::PlanPull(m, m.ReadbackNeeded(Bits({0, 1, 2, 3, 4, 5, 6, 7}), kGSTilePlanesAll), calls);

	ASSERT_EQ(calls.size(), 2u);
	EXPECT_EQ(calls[0].owner, a);
	EXPECT_EQ(PagesOf(calls[0].pages), (std::vector<u32>{0, 1, 2, 3}));
	EXPECT_EQ(calls[1].owner, b);
	EXPECT_EQ(PagesOf(calls[1].pages), (std::vector<u32>{4, 5, 6, 7}));
}

// Two pages of one owner whose truth was shrunk to DIFFERENT block subsets cannot share a call:
// the block mask picks the copy rect, so one mask for both would move the wrong blocks. They stay
// two calls, which is the coalescer refusing a merge that is not byte-identical.
TEST(GSTileGpuPullGrouping, DifferentBlockMasksDoNotMerge)
{
	GSVramModel m;
	const GSTileSurfaceId id = MakeColour(m, 0, 4, 1);
	m.OnNativeDraw(id, Bits({0, 1, 2}), kGSTilePlanesAll);

	// Shrink page 1's truth by a CPU write over part of it, leaving pages 0 and 2 whole.
	GSVramModel::RectFootprint fp;
	GSVramModel::FootprintForRect(Layout(32, 1, PSMCT32, GSTileSurfaceKind::Color), GSVector4i(0, 0, 64, 8), fp);
	m.OnCpuWrite(fp, kGSTilePlanesAll);
	ASSERT_NE(m.TruthMask(1, 0), GSVramModel::kFullBlockMask) << "the write must actually have shrunk page 1";

	std::vector<PullCall> calls;
	GSRendererTileGpu::PlanPull(m, m.ReadbackNeeded(Bits({0, 1, 2}), kGSTilePlanesAll), calls);

	ASSERT_EQ(calls.size(), 2u);
	// Page 1 is seen second, so its own mask opens the second call; 0 and 2 share the first.
	EXPECT_EQ(PagesOf(calls[0].pages), (std::vector<u32>{0, 2}));
	EXPECT_EQ(PagesOf(calls[1].pages), (std::vector<u32>{1}));
	EXPECT_NE(calls[0].block_mask, calls[1].block_mask);
}

// ⚠️ The order rule. A page owned by a colour surface on the RGB/alpha planes and a depth surface
// on the Z plane needs two calls whose byte windows OVERLAP (Z writes all four bytes, RGB the low
// three). Those two must stay in plane order on that page, and -- the part a cross-page bucket
// breaks -- must not be reordered by a bucket another page opened first.
//
// The arrangement below is exactly the hazard: page 10 contributes ONLY the depth call, so a
// naive coalescer creates the depth bucket first and page 11's colour write then lands after its
// depth write instead of before it.
TEST(GSTileGpuPullGrouping, AnAliasedPageKeepsItsCallsInPlaneOrderAndJoinsNoBucket)
{
	GSVramModel m;
	const GSTileSurfaceId colour = MakeColour(m, 11 * 32, 1, 1);
	const GSTileSurfaceId depth =
		m.Create(Layout(10 * 32, 1, PSMZ32, GSTileSurfaceKind::Depth), GSVector4i(0, 0, 64, 64), 2);

	// The depth surface takes the Z plane of pages 10 and 11; the colour surface takes the colour
	// planes of page 11 only. Page 10 is therefore single-call, page 11 aliased.
	m.OnNativeDraw(depth, Bits({10, 11}), GSTilePlaneZ);
	m.OnNativeDraw(colour, Bits({11}), GSTilePlaneRGB | GSTilePlaneAlphaLow | GSTilePlaneAlphaHigh);

	std::vector<PullCall> calls;
	GSRendererTileGpu::PlanPull(m, m.ReadbackNeeded(Bits({10, 11}), kGSTilePlanesAll), calls);

	// Page 11's two calls both exist, both name page 11 alone, and the colour one comes first.
	std::vector<size_t> on11;
	for (size_t i = 0; i < calls.size(); i++)
	{
		if (calls[i].pages.test(11))
			on11.push_back(i);
	}
	ASSERT_EQ(on11.size(), 2u) << "an aliased page needs one call per owner";
	EXPECT_EQ(calls[on11[0]].owner, colour) << "the colour write must land before the depth write";
	EXPECT_EQ(calls[on11[1]].owner, depth);
	EXPECT_EQ(PagesOf(calls[on11[0]].pages), (std::vector<u32>{11}))
		<< "an aliased page must join no bucket -- a bucket can float its second call ahead of its first";
	EXPECT_EQ(PagesOf(calls[on11[1]].pages), (std::vector<u32>{11}));

	// ...and page 10, which needs only one call, is still allowed to be its own call.
	bool saw10 = false;
	for (const PullCall& c : calls)
		saw10 |= c.pages.test(10);
	EXPECT_TRUE(saw10);
}

// A page whose truth some plane holds but whose owner has no pool texture contributes nothing --
// there is no image to copy from. It must not silently widen someone else's call either.
TEST(GSTileGpuPullGrouping, AnOwnerWithoutAPoolTextureIssuesNoCall)
{
	GSVramModel m;
	const GSTileSurfaceId id = MakeColour(m, 0, 2, /*pool_handle=*/0);
	m.OnNativeDraw(id, Bits({0, 1}), kGSTilePlanesAll);

	std::vector<PullCall> calls;
	GSRendererTileGpu::PlanPull(m, m.ReadbackNeeded(Bits({0, 1}), kGSTilePlanesAll), calls);
	EXPECT_TRUE(calls.empty());
}

// An empty ask is not a call. Cheap, and the pull road leans on it: the common CLUT load reaches
// here with nothing needed at all.
TEST(GSTileGpuPullGrouping, NothingNeededIsNoCalls)
{
	GSVramModel m;
	std::vector<PullCall> calls;
	calls.push_back(PullCall{});
	GSRendererTileGpu::PlanPull(m, GSPageBitmap(), calls);
	EXPECT_TRUE(calls.empty()) << "PlanPull must clear its output, not append to it";
}
