// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The ring's compose contract: a slot nobody prefills must be a slot the GPU composes WHOLE.
//
// GSDevice.h states it — a ring page is "prefilled by memcpy from src when that is non-null …
// and left for the GPU to compose otherwise (a page whose every byte comes from a target
// writeback)". Two places used to decide the two halves of that sentence and neither knew about
// the other:
//
//  - EnsureRingSlot chose "nobody prefills" from WHOLE-PAGE, PER-PLANE state: every plane's
//    owner holds full-block truth, unsynced, with a byte road.
//  - EmitPrepOp gave the writeback a PER-BLOCK, PER-OWNER mask: the union of the truth masks of
//    the planes that one surface owns on the page.
//
// A block that is neither prefilled nor inside some writeback's mask keeps whatever the executor
// left in the slot, and that is zero — not a stale guest byte, a byte that was never guest data.
// The model counts the page composed either way, so nothing says so. On OutRun 2006 the same
// class of hole, arriving through a different door, reads as fog index 0 (maximum haze) and
// paints two 64x32 pages sky-lavender.
//
// gsTileComposableBlocks states EmitPrepOp's rule once, as a pure function, and EnsureRingSlot
// now asks it rather than trusting the proxy. These tests pin the rule itself: what the writebacks
// cover, and — the case the proxy misses — a page where every plane looks composable and the
// blocks still do not add up.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include "GS/Renderers/Tile/GSTileTypes.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
constexpr u32 kFull = GSVramModel::kFullBlockMask;
constexpr GSTileSurfaceId kA = 1;
constexpr GSTileSurfaceId kB = 2;

// All four planes owned by one surface, full truth, unsynced, with a road: the ordinary
// "the GPU composes this whole page" case.
struct Planes
{
	GSTileRingPlaneState p[kGSTilePlaneCount];

	Planes(GSTileSurfaceId owner = kA, u32 mask = kFull, bool synced = false, bool road = true)
	{
		for (GSTileRingPlaneState& s : p)
			s = GSTileRingPlaneState{owner, mask, synced, road};
	}
	Planes& Set(u32 pi, GSTileSurfaceId owner, u32 mask, bool synced = false, bool road = true)
	{
		p[pi] = GSTileRingPlaneState{owner, mask, synced, road};
		return *this;
	}
	u32 Composable() const { return gsTileComposableBlocks(p); }
};
} // namespace

TEST(TileGpuRingCompose, OneOwnerHoldingEverythingComposesEveryBlock)
{
	EXPECT_EQ(Planes().Composable(), kFull);
}

TEST(TileGpuRingCompose, NoOwnerComposesNothing)
{
	EXPECT_EQ(Planes(kGSTileNoSurface, 0).Composable(), 0u);
}

TEST(TileGpuRingCompose, AnOwnerWithoutAByteRoadComposesNothing)
{
	// Depth, 16-bit, unaligned: in the model, no writeback shader. Its blocks stay whatever the
	// slot was prefilled with, which is why the page must be prefilled.
	EXPECT_EQ(Planes(kA, kFull, /*synced=*/false, /*road=*/false).Composable(), 0u);
}

TEST(TileGpuRingCompose, ASyncedOwnerJoinsNoWritebackOfItsOwn)
{
	// Synced means the ring already carries those bytes, so ComposeRingPages emits nothing for
	// them this time round. Nothing to compose from this owner.
	EXPECT_EQ(Planes(kA, kFull, /*synced=*/true).Composable(), 0u);
}

TEST(TileGpuRingCompose, ASyncedPlaneStillRidesAnOwnerThatIsWritingBackAnyway)
{
	// EmitPrepOp's mask is the union over every plane the surface owns, WITHOUT consulting
	// synced: once the op exists, the shader writes those blocks. So a synced plane of an owner
	// that some other plane already put on the road contributes its blocks too. Pinned because
	// the two rules must match byte for byte, not merely be conservative in the same direction.
	Planes pl;
	pl.Set(2, kA, kFull, /*synced=*/true); // plane index 2 == GSTilePlaneAlphaHigh
	EXPECT_EQ(pl.Composable(), kFull);
}

TEST(TileGpuRingCompose, TwoOwnersEachCoverTheirOwnBlocks)
{
	// A page split between surfaces: each writes back the union of its own planes' masks, and
	// together they can still reach every block.
	Planes pl(kA, 0x0000FFFFu);
	pl.Set(2, kB, 0xFFFF0000u);
	pl.Set(3, kB, 0xFFFF0000u);
	EXPECT_EQ(pl.Composable(), kFull);
}

TEST(TileGpuRingCompose, ShortMasksLeaveTheirBlocksUncomposed)
{
	// The shape the ring contract cares about: the writebacks run, they just do not reach block
	// 31. Nothing else in the model would have said so.
	Planes pl(kA, kFull & ~(1u << 31));
	EXPECT_EQ(pl.Composable(), kFull & ~(1u << 31));
	EXPECT_NE(pl.Composable(), kFull);
}

TEST(TileGpuRingCompose, TheWholePageProxyImpliesFullCoverageToday)
{
	// Why the added test is free on today's corpus rather than a new source of prefills: when
	// EVERY plane satisfies the per-plane proxy, every plane's owner is on the road and every
	// plane's mask is full, so the union cannot be short. Measured to agree — outrun-b, four
	// frames, 878 ring pages and 231 unprefilled slots per frame, zero disagreements.
	//
	// This is a proof about the CONJUNCTION, which is exactly why the coverage test is worth
	// having: it survives any one of those clauses being relaxed, and the proxy does not.
	for (u32 mask : {kFull})
	{
		for (u32 split = 0; split < 4; split++)
		{
			Planes pl(kA, mask);
			// Hand some planes to a second surface; both still hold full truth with a road.
			for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
			{
				if ((split >> (pi & 1)) & 1)
					pl.Set(pi, kB, mask);
			}
			EXPECT_EQ(pl.Composable(), kFull) << "split " << split;
		}
	}
}

TEST(TileGpuRingCompose, RelaxingAnyOneProxyClauseCanLeaveBlocksUncovered)
{
	// Relax the whole-page clause for ONE plane whose owner owns nothing else: the other planes
	// are somebody else's, so nothing else brings those blocks in and the union is short. Under
	// the proxy alone this page would go unprefilled and ship executor zeros in blocks 16-31.
	Planes pl(kA, 0x0000FFFFu);
	pl.Set(0, kB, 0x0000FFFFu);
	EXPECT_EQ(pl.Composable(), 0x0000FFFFu);
	EXPECT_NE(pl.Composable(), kFull);

	// Relax the road clause for one owner, and its planes' blocks are simply not composed.
	Planes road(kA, 0x000000FFu);
	road.Set(3, kB, 0xFFFFFF00u, /*synced=*/false, /*road=*/false);
	EXPECT_EQ(road.Composable(), 0x000000FFu);
}

// ---------------------------------------------------------------------------------------
// GSTileBlockFill — which blocks of a surface's texture actually hold texels.
// ---------------------------------------------------------------------------------------
//
// The other half of the same mismatch. The model tracks guest bytes per BLOCK; the record of
// what the surface's texture holds was a page bitmap, so it could only answer per PAGE. A page
// bitmap that stands in for a block mask has to round, and the rounding that costs nothing at
// the call site is the one that over-claims: a page counted held whose texels are half allocator
// leftovers, then sampled by the present or written back into the ring as if it were guest data.
//
// The rule is that nothing is marked without a proof. These pin the accumulation, the promotion
// to whole, and — the property the readers depend on — that a partially held page never answers
// "yes" to a whole-page question.

namespace
{
constexpr u32 kPage = 293;
}

TEST(TileGpuBlockFill, AFreshFillHoldsNothing)
{
	GSTileBlockFill f;
	GSPageBitmap one;
	one.set(kPage);
	EXPECT_EQ(f.BlocksOf(kPage), 0u);
	EXPECT_FALSE(f.HoldsWhole(one));
	EXPECT_FALSE(f.MissingFrom(one).empty());
}

TEST(TileGpuBlockFill, AWholePageMarkHoldsEveryBlock)
{
	GSTileBlockFill f;
	GSPageBitmap one;
	one.set(kPage);
	f.MarkWhole(one);
	EXPECT_EQ(f.BlocksOf(kPage), kFull);
	EXPECT_TRUE(f.HoldsWhole(one));
	EXPECT_TRUE(f.MissingFrom(one).empty());
}

TEST(TileGpuBlockFill, PartialBlocksDoNotAnswerAWholePageQuestion)
{
	// The whole point. Twenty-eight of thirty-two blocks is not a held page, and every reader
	// (the seed gate, the rule-2 bind, the donor road, the display materialise) asks the
	// whole-page question.
	GSTileBlockFill f;
	GSPageBitmap one;
	one.set(kPage);
	f.Mark(kPage, 0xFFFFFFF0u);
	EXPECT_EQ(f.BlocksOf(kPage), 0xFFFFFFF0u);
	EXPECT_FALSE(f.HoldsWhole(one));
	EXPECT_FALSE(f.MissingFrom(one).empty());
}

TEST(TileGpuBlockFill, BlocksAccumulateAndPromote)
{
	GSTileBlockFill f;
	GSPageBitmap one;
	one.set(kPage);
	f.Mark(kPage, 0x0000FFFFu);
	EXPECT_FALSE(f.HoldsWhole(one));
	f.Mark(kPage, 0x00FF0000u);
	EXPECT_EQ(f.BlocksOf(kPage), 0x00FFFFFFu);
	EXPECT_FALSE(f.HoldsWhole(one));
	f.Mark(kPage, 0xFF000000u);
	// The blocks add up to the page, so it answers the whole-page question from here.
	EXPECT_EQ(f.BlocksOf(kPage), kFull);
	EXPECT_TRUE(f.HoldsWhole(one));
}

TEST(TileGpuBlockFill, MarkingNothingChangesNothing)
{
	GSTileBlockFill f;
	f.Mark(kPage, 0);
	EXPECT_EQ(f.BlocksOf(kPage), 0u);
}

TEST(TileGpuBlockFill, AWholePageMarkSubsumesEarlierPartials)
{
	GSTileBlockFill f;
	GSPageBitmap one;
	one.set(kPage);
	f.Mark(kPage, 0x0000000Fu);
	f.MarkWhole(one);
	EXPECT_EQ(f.BlocksOf(kPage), kFull);
	// ...and a later partial mark cannot take blocks away again.
	f.Mark(kPage, 0x0000000Fu);
	EXPECT_EQ(f.BlocksOf(kPage), kFull);
	EXPECT_TRUE(f.HoldsWhole(one));
}

TEST(TileGpuBlockFill, PagesAreIndependentAndClearResetsThem)
{
	GSTileBlockFill f;
	GSPageBitmap two;
	two.set(kPage);
	two.set(kPage + 4);
	f.Mark(kPage, kFull);
	EXPECT_FALSE(f.HoldsWhole(two));
	EXPECT_EQ(f.MissingFrom(two).count(), 1u);
	f.Mark(kPage + 4, kFull);
	EXPECT_TRUE(f.HoldsWhole(two));
	f.Clear();
	EXPECT_EQ(f.BlocksOf(kPage), 0u);
	EXPECT_FALSE(f.HoldsWhole(two));
}

// ---------------------------------------------------------------------------------------
// GSTileComposeBuckets — which surfaces write back, and in what order.
// ---------------------------------------------------------------------------------------
//
// The other way a ring slot ends up holding bytes nobody wrote, and the one that shipped for
// months. ComposeRingPages grouped the writebacks it must emit by owning surface into a
// hard-coded eight-entry array, commented "tiny in practice (one or two owners per read
// window)". MGS3 tiles its post-process scratch by walking FRAME.FBP one page at a time, so it
// has 55 one-page targets and its full-screen composite gathers 49 owners in one read. The ninth
// and later were dropped: no writeback op, the slot force-prefilled from the CPU shadow, the page
// marked synced anyway, and every later reader served stale guest memory. On screen that is
// blocky garbage blended over the whole frame; against the software golden it was mad 47 instead
// of 8.
//
// These pin the property the constant cannot have: every owner of the window gets a writeback,
// whatever the count. Plus the two things the dynamic scratch itself could get wrong — the
// emission order (array order is execution order at the pass head, so it must stay first
// page-ascending appearance) and reuse across gathers (an entry left over from the last gather
// would write back pages this one never asked for).

namespace
{
constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm, GSTileSurfaceKind kind = GSTileSurfaceKind::Color)
{
	return GSTileSurfaceLayout{bp, bw, psm, kind};
}

// One CT32 page is 64x32 pixels. MGS3's scratch tile, exactly.
constexpr u32 kBlocksPerPage = 32;
GSVector4i OnePageRect()
{
	return GSVector4i(0, 0, 64, 32);
}
} // namespace

TEST(TileGpuComposeBuckets, EveryOwnerOfTheWindowGetsAWriteback)
{
	// MGS3's gather: 49 one-page targets walked out of FRAME.FBP, all read by one composite.
	GSVramModel m;
	constexpr u32 kOwners = 49;
	constexpr u32 kBasePage = 256; // GS pages 256.. — where MGS3 puts the scratch
	GSPageBitmap window;
	for (u32 i = 0; i < kOwners; i++)
	{
		const auto l = Layout((kBasePage + i) * kBlocksPerPage, 1, PSMCT32);
		const GSTileSurfaceId id = m.Create(l, OnePageRect(), i + 1);
		const GSPageBitmap p = GSVramModel::PagesForRect(l, OnePageRect());
		ASSERT_EQ(p.count(), 1u) << "tile " << i;
		m.OnNativeDraw(id, p, kGSTilePlanesColor);
		window |= p;
	}
	ASSERT_EQ(window.count(), kOwners);

	const GSPageBitmap need = m.ReadbackNeeded(window, kGSTilePlanesAll);
	ASSERT_EQ(need.count(), kOwners);

	GSTileComposeBuckets bk;
	bk.Gather(m, need);

	// The whole point. Eight buckets here means 41 of the 49 pages are served out of the CPU
	// shadow instead of the target that actually holds them.
	EXPECT_EQ(bk.Count(), kOwners);
	EXPECT_EQ(bk.LossyPages(), 0u);
	EXPECT_EQ(bk.LossyDepthPages(), 0u);

	GSPageBitmap covered;
	for (u32 b = 0; b < bk.Count(); b++)
	{
		EXPECT_EQ(bk.PagesAt(b).count(), 1u) << "bucket " << b;
		EXPECT_FALSE(covered.intersects(bk.PagesAt(b))) << "bucket " << b;
		covered |= bk.PagesAt(b);
	}
	EXPECT_TRUE(covered == window);
}

TEST(TileGpuComposeBuckets, EmissionOrderIsFirstPageAscendingAppearance)
{
	// Array order is execution order at the pass head, so the order the buckets come out in is
	// part of the contract, not an implementation detail. Created back to front on purpose: the
	// order must come from the pages, not from when the surface was made.
	GSVramModel m;
	constexpr u32 kOwners = 12; // past the old table, so this also exercises the growth path
	constexpr u32 kBasePage = 64;
	std::vector<GSTileSurfaceId> by_page(kOwners, kGSTileNoSurface);
	GSPageBitmap window;
	for (u32 i = kOwners; i-- > 0;)
	{
		const auto l = Layout((kBasePage + i) * kBlocksPerPage, 1, PSMCT32);
		const GSTileSurfaceId id = m.Create(l, OnePageRect(), i + 1);
		const GSPageBitmap p = GSVramModel::PagesForRect(l, OnePageRect());
		m.OnNativeDraw(id, p, kGSTilePlanesColor);
		by_page[i] = id;
		window |= p;
	}

	GSTileComposeBuckets bk;
	bk.Gather(m, m.ReadbackNeeded(window, kGSTilePlanesAll));
	ASSERT_EQ(bk.Count(), kOwners);
	for (u32 b = 0; b < kOwners; b++)
	{
		EXPECT_EQ(bk.IdAt(b), by_page[b]) << "bucket " << b;
		EXPECT_TRUE(bk.PagesAt(b).test(kBasePage + b)) << "bucket " << b;
	}
}

TEST(TileGpuComposeBuckets, AnOwnerWithoutAByteRoadIsCountedNotBucketed)
{
	// Depth has no writeback shader at all, so its truth cannot join a bucket — the page keeps
	// its prefill and the loss is counted. Unchanged by the fix; pinned so the counter cannot
	// quietly move when the dropped-writeback road stops feeding it.
	GSVramModel m;
	const auto colour = Layout(0, 1, PSMCT32);
	const auto depth = Layout(kBlocksPerPage, 1, PSMZ32, GSTileSurfaceKind::Depth);
	const GSTileSurfaceId cid = m.Create(colour, OnePageRect(), 1);
	const GSTileSurfaceId did = m.Create(depth, OnePageRect(), 2);
	const GSPageBitmap cpages = GSVramModel::PagesForRect(colour, OnePageRect());
	const GSPageBitmap dpages = GSVramModel::PagesForRect(depth, OnePageRect());
	m.OnNativeDraw(cid, cpages, kGSTilePlanesColor);
	m.OnNativeDraw(did, dpages, GSTilePlaneZ);

	GSTileComposeBuckets bk;
	bk.Gather(m, m.ReadbackNeeded(cpages | dpages, kGSTilePlanesAll));
	ASSERT_EQ(bk.Count(), 1u);
	EXPECT_EQ(bk.IdAt(0), cid);
	EXPECT_TRUE(bk.PagesAt(0) == cpages);
	EXPECT_EQ(bk.LossyPages(), 1u);
	EXPECT_EQ(bk.LossyDepthPages(), 1u);
}

TEST(TileGpuComposeBuckets, AGatherCarriesNothingOverFromTheLastOne)
{
	// The scratch is reused across gathers, so a bucket must hold this gather's pages and no
	// others: a stale bit would write back a page the reader never asked about, over whatever the
	// ring slot holds now.
	GSVramModel m;
	const auto l = Layout(0, 1, PSMCT32);
	const GSVector4i two_pages(0, 0, 64, 64); // 64x32 per CT32 page, so two pages down the column
	const GSTileSurfaceId id = m.Create(l, two_pages, 1);
	const GSPageBitmap pages = GSVramModel::PagesForRect(l, two_pages);
	ASSERT_EQ(pages.count(), 2u);
	m.OnNativeDraw(id, pages, kGSTilePlanesColor);

	GSTileComposeBuckets bk;
	bk.Gather(m, m.ReadbackNeeded(pages, kGSTilePlanesAll));
	ASSERT_EQ(bk.Count(), 1u);
	ASSERT_EQ(bk.PagesAt(0).count(), 2u);

	// Sync the second of them: the next gather over the same window needs only the first.
	std::vector<u32> page_list;
	pages.forEachSetPage([&](u32 page) { page_list.push_back(page); });
	ASSERT_EQ(page_list.size(), 2u);
	const u32 kept = page_list[0];
	GSPageBitmap second;
	second.set(page_list[1]);
	m.OnReadback(second);

	bk.Gather(m, m.ReadbackNeeded(pages, kGSTilePlanesAll));
	ASSERT_EQ(bk.Count(), 1u);
	EXPECT_EQ(bk.PagesAt(0).count(), 1u);
	EXPECT_TRUE(bk.PagesAt(0).test(kept));

	// And a gather that needs nothing leaves no buckets behind.
	m.OnReadback(pages);
	bk.Gather(m, m.ReadbackNeeded(pages, kGSTilePlanesAll));
	EXPECT_EQ(bk.Count(), 0u);
}
