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
