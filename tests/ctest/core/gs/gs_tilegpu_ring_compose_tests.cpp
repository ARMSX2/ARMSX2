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
//  - EmitPrepOp gave the writeback a PER-BLOCK, PER-OWNER, PER-BYTE reach: the union of the truth
//    masks of the planes that one surface owns on the page, landed only in the bytes of a cell
//    that surface's format stores.
//
// A byte that is neither prefilled nor inside some writeback's reach keeps whatever the PREVIOUS
// TENANT of that ring offset left there. Only the zero slot is ever cleared, so it is neither a
// zero nor a stale byte of this page: on a desktop allocator it is another page's bytes from an
// earlier frame, and on one that recycles foreign pages it is arbitrary content. The model counts
// the page composed either way, so nothing says so. On OutRun 2006 the same class of hole,
// arriving through a different door, reads as fog index 0 (maximum haze) and paints two 64x32
// pages sky-lavender.
//
// gsTileComposableBlocksPerByte states EmitPrepOp's rule once, as a pure function, and
// EnsureRingSlot now asks it rather than trusting the proxy. These tests pin the rule itself: what
// the writebacks cover, and — the cases the proxy misses — a page where every plane looks
// composable and the blocks still do not add up, and a page whose blocks add up and whose BYTES
// do not.

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
//
// `bytes` is the owner's writeback byte mask -- 0xFFFFFFFF for a surface whose format stores every
// byte of the cell, 0x00FFFFFF for a PSMCT24 one, 0 for a surface with no writeback shader at all.
struct Planes
{
	GSTileRingPlaneState p[kGSTilePlaneCount];

	Planes(GSTileSurfaceId owner = kA, u32 mask = kFull, bool synced = false, bool road = true,
		u32 bytes = 0xFFFFFFFFu)
	{
		for (GSTileRingPlaneState& s : p)
			s = GSTileRingPlaneState{owner, mask, synced, road ? bytes : 0u};
	}
	Planes& Set(u32 pi, GSTileSurfaceId owner, u32 mask, bool synced = false, bool road = true,
		u32 bytes = 0xFFFFFFFFu)
	{
		p[pi] = GSTileRingPlaneState{owner, mask, synced, road ? bytes : 0u};
		return *this;
	}
	u32 Composable() const { return gsTileComposableBlocks(p); }
	bool CoversWholePage() const { return gsTileComposeCoversWholePage(p); }
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

TEST(TileGpuRingCompose, TheWholePageProxyImpliesFullBlockCoverageToday)
{
	// Why the BLOCK half of the added test costs no prefills on today's corpus: when EVERY plane
	// satisfies the per-plane proxy, every plane's owner is on the road and every plane's mask is
	// full, so the union cannot be short. Measured to agree — outrun-b, four frames, 878 ring pages
	// and 231 unprefilled slots per frame, zero disagreements.
	//
	// This is a proof about the CONJUNCTION, which is exactly why the coverage test is worth
	// having: it survives any one of those clauses being relaxed, and the proxy does not.
	//
	// ⚠️ It says nothing about BYTES, and that is where the proxy was actually wrong: every writer
	// below stores all four, and a PSMCT24 writer satisfies the same conjunction while composing
	// three. The lane tests further down are that case.
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
// The same hole one level down: every BLOCK covered, and a byte of every cell still unwritten.
// ---------------------------------------------------------------------------------------
//
// A PSMCT24 surface stores nothing in the alpha byte, so its writeback's byte_mask is 0x00FFFFFF
// and byte 3 of every word it touches is left for whoever does hold that byte. Nobody does: the
// model hands a 24-bit draw the alpha PLANES along with the channels it stores (GSTileTypes.h says
// why), so all four planes name the same owner, the block coverage is whole, and the page went
// unprefilled. Byte 3 of all 2048 words then read the previous tenant of that ring offset — which
// on this box is another page's bytes and on an allocator that recycles foreign pages is anything
// at all. Eight of the sixteen corpus titles shipped such a page every frame; the
// allocation-poison lever caught two of them consuming the bytes on the M2 box, MGS3 over 58% of
// its pixels and Ratchet & Clank UYA over a 2461-pixel block of its effects scene.
//
// So the coverage question is per (block, byte), and the block-only answer is a fold of it.

namespace
{
constexpr u32 kCT24Bytes = 0x00FFFFFFu;
}

TEST(TileGpuRingCompose, ATwentyFourBitOwnerComposesEveryBlockAndNotEveryByte)
{
	const Planes pl(kA, kFull, /*synced=*/false, /*road=*/true, kCT24Bytes);
	// The block-level answer is unchanged, and on its own it says "prefill nothing".
	EXPECT_EQ(pl.Composable(), kFull);
	// The byte-level one is the truth: lane 3 is composed in no block at all.
	u32 per_byte[4];
	gsTileComposableBlocksPerByte(pl.p, per_byte);
	EXPECT_EQ(per_byte[0], kFull);
	EXPECT_EQ(per_byte[1], kFull);
	EXPECT_EQ(per_byte[2], kFull);
	EXPECT_EQ(per_byte[3], 0u);
	EXPECT_FALSE(pl.CoversWholePage());
}

TEST(TileGpuRingCompose, AThirtyTwoBitCoOwnerSuppliesTheByteTheTwentyFourBitOneDoesNot)
{
	// The page's alpha planes belong to a surface that does store alpha, so its writeback fills the
	// lane the 24-bit one masks off and the slot needs no prefill after all. The rule has to admit
	// this case or it degenerates into "prefill every page any 24-bit surface touches".
	Planes pl(kA, kFull, /*synced=*/false, /*road=*/true, kCT24Bytes);
	pl.Set(1, kB, kFull); // AlphaLow
	pl.Set(2, kB, kFull); // AlphaHigh
	EXPECT_TRUE(pl.CoversWholePage());
}

TEST(TileGpuRingCompose, ATwentyFourBitOwnerShortOfBlocksIsShortInBothAnswers)
{
	Planes pl(kA, 0x0000FFFFu, /*synced=*/false, /*road=*/true, kCT24Bytes);
	u32 per_byte[4];
	gsTileComposableBlocksPerByte(pl.p, per_byte);
	EXPECT_EQ(per_byte[0], 0x0000FFFFu);
	EXPECT_EQ(per_byte[3], 0u);
	EXPECT_FALSE(pl.CoversWholePage());
}

TEST(TileGpuRingCompose, TheWholePageAnswerIsTheFoldOfTheFourLanes)
{
	// gsTileComposableBlocks must stay exactly the union of the lanes, so the block-reach callers
	// and the byte-reach callers read one computation. Checked over a spread of shapes rather than
	// asserted in prose.
	for (u32 mask : {kFull, 0x0000FFFFu, 0x000000FFu, 0u})
	{
		for (u32 bytes : {0xFFFFFFFFu, kCT24Bytes})
		{
			const Planes pl(kA, mask, /*synced=*/false, /*road=*/true, bytes);
			u32 per_byte[4];
			gsTileComposableBlocksPerByte(pl.p, per_byte);
			EXPECT_EQ(pl.Composable(), per_byte[0] | per_byte[1] | per_byte[2] | per_byte[3])
				<< "mask " << mask << " bytes " << bytes;
			EXPECT_EQ(pl.CoversWholePage(), mask == kFull && bytes == 0xFFFFFFFFu)
				<< "mask " << mask << " bytes " << bytes;
		}
	}
}

TEST(TileGpuRingCompose, NoWriterCoversNoLane)
{
	const Planes pl(kGSTileNoSurface, 0);
	u32 per_byte[4];
	gsTileComposableBlocksPerByte(pl.p, per_byte);
	for (u32 b = 0; b < 4; b++)
		EXPECT_EQ(per_byte[b], 0u) << "lane " << b;
	EXPECT_FALSE(pl.CoversWholePage());
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
