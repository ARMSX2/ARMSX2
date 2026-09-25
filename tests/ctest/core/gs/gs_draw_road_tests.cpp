// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the per-draw self-read decisions (GS/Renderers/Common/GSDrawRoad.h). Most rows are devices
// that cannot be run here, so the no-change cases are pinned by name.

#include "GS/Renderers/Common/GSDrawRoad.h"
#include "GS/Renderers/Common/GSFramebufferFetchPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// In-tile destination read: Mali or Adreno on Vulkan with rasterization-order access, Mali on GL
	// through ARM fetch, Apple GPUs under Metal. Fetch implies texture barriers on all of them.
	constexpr GSDrawRoadDevice FetchRoad()
	{
		return {.texture_barrier = true, .framebuffer_fetch = true};
	}

	// A real texture barrier and no in-tile read: desktop, and the M2 under Vulkan.
	constexpr GSDrawRoadDevice BarrierRoad()
	{
		return {.texture_barrier = true};
	}

	// No texture barrier: the backend clones the target for the read.
	constexpr GSDrawRoadDevice CopyRoad()
	{
		return {};
	}

	// The declared attachment feedback loop on a driver that orders it (Turnip with the fix).
	constexpr GSDrawRoadDevice DeclaredOrderedRoad()
	{
		return {.texture_barrier = true, .feedback_loop_layout = true, .declared_loop_orders_overlap = true};
	}

	constexpr bool Drops(const GSDrawRoadDevice& dev, bool elsewhere, bool overlap, bool depth)
	{
		return GSDrawDropsBarriers(dev, elsewhere, overlap, depth);
	}
} // namespace

// --- Offset self-read copy -------------------------------------------------------------------

// An offset read on the fetch road has neither a copy nor a barrier otherwise.
TEST(GSDrawRoad, FetchRoadOffsetReadCopies)
{
	EXPECT_TRUE(GSOffsetSelfReadNeedsCopy(FetchRoad()));
}

// Desktop keeps the disjoint-rect shortcut and its barrier.
TEST(GSDrawRoad, BarrierRoadOffsetReadDoesNotCopy)
{
	EXPECT_FALSE(GSOffsetSelfReadNeedsCopy(BarrierRoad()));
}

// Without a texture barrier the backend already copies, with or without fetch advertised.
TEST(GSDrawRoad, CopyRoadOffsetReadDoesNotCopyAgain)
{
	EXPECT_FALSE(GSOffsetSelfReadNeedsCopy(CopyRoad()));

	GSDrawRoadDevice with_fetch = CopyRoad();
	with_fetch.framebuffer_fetch = true;
	EXPECT_FALSE(GSOffsetSelfReadNeedsCopy(with_fetch));
}

// The layout road samples through an ordinary sampler.
TEST(GSDrawRoad, FeedbackLoopLayoutOffsetReadDoesNotCopy)
{
	GSDrawRoadDevice in = FetchRoad();
	in.feedback_loop_layout = true;
	EXPECT_FALSE(GSOffsetSelfReadNeedsCopy(in));
}

// The declared road is untiled, so the offset read's one barrier orders it and a clone would only
// cost a copy and a pass break.
TEST(GSDrawRoad, DeclaredOrderedRoadOffsetReadTakesTheBarrier)
{
	EXPECT_FALSE(GSOffsetSelfReadNeedsCopy(DeclaredOrderedRoad()));
	EXPECT_FALSE(Drops(DeclaredOrderedRoad(), /*elsewhere=*/true, false, false));
	EXPECT_FALSE(Drops(DeclaredOrderedRoad(), /*elsewhere=*/true, true, false));
}

// A device that somehow set both is still reading through tile memory.
TEST(GSDrawRoad, DeclaredBitDoesNotReleaseTheFetchRoad)
{
	GSDrawRoadDevice in = FetchRoad();
	in.declared_loop_orders_overlap = true;
	EXPECT_TRUE(GSOffsetSelfReadNeedsCopy(in));
}

// The copy happens for exactly one combination; the ordering bits never change it.
TEST(GSDrawRoad, OffsetReadCopiesOnlyOnTheFetchRoad)
{
	for (int bits = 0; bits < 32; bits++)
	{
		GSDrawRoadDevice in;
		in.framebuffer_fetch = (bits & 1) != 0;
		in.texture_barrier = (bits & 2) != 0;
		in.feedback_loop_layout = (bits & 4) != 0;
		in.fetch_orders_overlap = (bits & 8) != 0;
		in.declared_loop_orders_overlap = (bits & 16) != 0;

		const bool expected = in.framebuffer_fetch && in.texture_barrier && !in.feedback_loop_layout;
		EXPECT_EQ(GSOffsetSelfReadNeedsCopy(in), expected) << "bits=" << bits;
	}
}

// --- Dropping the draw's barriers ------------------------------------------------------------

// Neither ordered spelling, nothing dropped.
TEST(GSDrawRoad, UnorderedRoadsKeepTheirBarriers)
{
	for (const GSDrawRoadDevice& dev : {BarrierRoad(), CopyRoad()})
	{
		for (int bits = 0; bits < 8; bits++)
			EXPECT_FALSE(Drops(dev, (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0)) << "bits=" << bits;
	}
}

// GL's EXT fetch orders nothing, so an overlapping draw keeps its barrier: the software blend path
// enabled for it reads a destination its own predecessor writes.
TEST(GSDrawRoad, UnorderedFetchKeepsTheBarrierWhenPrimitivesOverlap)
{
	EXPECT_FALSE(Drops(FetchRoad(), false, true, false));
}

// With no overlap a live in-tile read and a pre-draw snapshot are the same value.
TEST(GSDrawRoad, NonOverlappingDrawsDropTheBarrierOnEveryOrderedRoad)
{
	GSDrawRoadDevice ordered_fetch = FetchRoad();
	ordered_fetch.fetch_orders_overlap = true;

	EXPECT_TRUE(Drops(FetchRoad(), false, false, false));
	EXPECT_TRUE(Drops(ordered_fetch, false, false, false));
	EXPECT_TRUE(Drops(DeclaredOrderedRoad(), false, false, false));
}

// Rasterization-order access, Metal programmable blending and ARM fetch order overlapping
// fragments by contract; the declared road's driver does too.
TEST(GSDrawRoad, OrderingRoadsDropTheBarrierWithOverlap)
{
	GSDrawRoadDevice ordered_fetch = FetchRoad();
	ordered_fetch.fetch_orders_overlap = true;

	EXPECT_TRUE(Drops(ordered_fetch, false, true, false));
	EXPECT_TRUE(Drops(DeclaredOrderedRoad(), false, true, false));
}

// Depth read through a texture is covered by neither spelling.
TEST(GSDrawRoad, DepthFeedbackBarriersSurviveEveryRoad)
{
	GSDrawRoadDevice ordered_fetch = FetchRoad();
	ordered_fetch.fetch_orders_overlap = true;

	for (const GSDrawRoadDevice& dev : {FetchRoad(), ordered_fetch, DeclaredOrderedRoad()})
	{
		for (bool overlap : {false, true})
			EXPECT_FALSE(Drops(dev, false, overlap, true));
	}
}

// Without an ordering guarantee the answer tracks overlap exactly.
TEST(GSDrawRoad, UnorderedFetchDropsExactlyWhenNothingOverlaps)
{
	for (bool overlap : {false, true})
		EXPECT_EQ(Drops(FetchRoad(), false, overlap, false), !overlap) << "overlap=" << overlap;
}

// The GL fetch backend decides the ordering bit: ARM orders overlapping primitives by its spec,
// EXT does not.
TEST(GSDrawRoad, GLFetchBackendDecidesTheOverlapDrop)
{
	GSDrawRoadDevice arm = FetchRoad();
	arm.fetch_orders_overlap = FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::ARM);
	EXPECT_TRUE(Drops(arm, false, true, false));

	GSDrawRoadDevice ext = FetchRoad();
	ext.fetch_orders_overlap = FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::EXT);
	EXPECT_FALSE(Drops(ext, false, true, false));
}

// The swept rule.
TEST(GSDrawRoad, DropRuleSwept)
{
	for (int bits = 0; bits < 256; bits++)
	{
		GSDrawRoadDevice dev;
		dev.texture_barrier = (bits & 1) != 0;
		dev.framebuffer_fetch = (bits & 2) != 0;
		dev.feedback_loop_layout = (bits & 4) != 0;
		dev.fetch_orders_overlap = (bits & 8) != 0;
		dev.declared_loop_orders_overlap = (bits & 16) != 0;
		const bool elsewhere = (bits & 32) != 0;
		const bool overlap = (bits & 64) != 0;
		const bool depth = (bits & 128) != 0;

		const bool ordered_spelling = dev.framebuffer_fetch || dev.declared_loop_orders_overlap;
		const bool orders = dev.fetch_orders_overlap || dev.declared_loop_orders_overlap;
		const bool expected = ordered_spelling && !elsewhere && !depth && (orders || !overlap);
		EXPECT_EQ(Drops(dev, elsewhere, overlap, depth), expected) << "bits=" << bits;
	}
}
