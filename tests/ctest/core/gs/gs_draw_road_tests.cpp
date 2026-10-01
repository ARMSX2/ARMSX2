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

// --- Feedback-loop carry ---------------------------------------------------------------------

namespace
{
	constexpr GSFeedbackLoopCarryInputs MaliWithFetch()
	{
		return {.device_is_measured_vendor = true, .barriers_order_reads = true, .framebuffer_fetch = true};
	}

	// Turnip on the declared loop.
	constexpr GSFeedbackLoopCarryInputs AdrenoOnTheLayoutRoad()
	{
		return {.device_is_layout_road_vendor = true, .barriers_order_reads = true, .feedback_loop_layout = true};
	}

	// Honeykrisp's default road: our own feedback barriers order the read.
	constexpr GSFeedbackLoopCarryInputs M2OnTheLayoutRoad()
	{
		return {.barriers_order_reads = true, .device_is_barrier_road_vendor = true, .feedback_loop_layout = true};
	}

	// NVIDIA, AMD, Intel: same road and barriers as the M2, never timed.
	constexpr GSFeedbackLoopCarryInputs DesktopOnTheLayoutRoad()
	{
		return {.barriers_order_reads = true, .feedback_loop_layout = true};
	}

	constexpr GSDrawRoad Decide(GSFeedbackCarry carry, bool any_barrier, bool writes_depth)
	{
		GSDrawRoadDevice dev;
		dev.carry = carry;
		GSDrawRoadDraw draw;
		draw.any_barrier = any_barrier;
		draw.writes_depth = writes_depth;
		return GSDecideDrawRoad(dev, draw);
	}
} // namespace

TEST(GSDrawRoad, CarryByDevice)
{
	EXPECT_EQ(GSFeedbackCarryForDevice(MaliWithFetch()), GSFeedbackCarry::NonReaders);
	EXPECT_EQ(GSFeedbackCarryForDevice(AdrenoOnTheLayoutRoad()), GSFeedbackCarry::NonReaders);
	EXPECT_EQ(GSFeedbackCarryForDevice(M2OnTheLayoutRoad()), GSFeedbackCarry::NonReaders);
	EXPECT_EQ(GSFeedbackCarryForDevice({.device_always_carries = true}), GSFeedbackCarry::All);

	// Desktop keeps draw-local bits, and so does the copy road on any vendor.
	EXPECT_EQ(GSFeedbackCarryForDevice(DesktopOnTheLayoutRoad()), GSFeedbackCarry::None);
	EXPECT_EQ(GSFeedbackCarryForDevice({}), GSFeedbackCarry::None);
	EXPECT_EQ(GSFeedbackCarryForDevice({.device_is_layout_road_vendor = true}), GSFeedbackCarry::None);
	EXPECT_EQ(GSFeedbackCarryForDevice({.barriers_order_reads = true}), GSFeedbackCarry::None);
}

// Fetch carries only on the vendor it was measured on, and only with fetch live.
TEST(GSDrawRoad, FetchCarryNeedsFetchAndItsVendor)
{
	GSFeedbackLoopCarryInputs no_fetch = MaliWithFetch();
	no_fetch.framebuffer_fetch = false;
	EXPECT_EQ(GSFeedbackCarryForDevice(no_fetch), GSFeedbackCarry::None);

	GSFeedbackLoopCarryInputs other_vendor = MaliWithFetch();
	other_vendor.device_is_measured_vendor = false;
	EXPECT_EQ(GSFeedbackCarryForDevice(other_vendor), GSFeedbackCarry::None);

	// A device reporting both spellings is answered by the layout road's terms alone.
	GSFeedbackLoopCarryInputs both = MaliWithFetch();
	both.feedback_loop_layout = true;
	EXPECT_EQ(GSFeedbackCarryForDevice(both), GSFeedbackCarry::None);
}

// With texture barriers off the layout road has no ordering, so -no-tex-barriers is inert.
TEST(GSDrawRoad, LayoutRoadWithoutOrderingDoesNotCarry)
{
	GSFeedbackLoopCarryInputs in = M2OnTheLayoutRoad();
	in.barriers_order_reads = false;
	EXPECT_EQ(GSFeedbackCarryForDevice(in), GSFeedbackCarry::None);

	in.device_is_measured_vendor = true;
	EXPECT_EQ(GSFeedbackCarryForDevice(in), GSFeedbackCarry::None);
}

// A reader asks for its own barrier and so never inherits a carried bit; carrying onto it would
// emit a barrier that was not there before. Broadcom carries onto every draw.
TEST(GSDrawRoad, ReadersSetTheirOwnBits)
{
	EXPECT_TRUE(Decide(GSFeedbackCarry::NonReaders, false, false).carry_rt);
	EXPECT_FALSE(Decide(GSFeedbackCarry::NonReaders, true, false).carry_rt);
	EXPECT_TRUE(Decide(GSFeedbackCarry::All, true, false).carry_rt);
	EXPECT_FALSE(Decide(GSFeedbackCarry::None, false, false).carry_rt);
}

// A depth writer never inherits the depth bits, on any device; the colour carry is untouched.
TEST(GSDrawRoad, DepthBitsAreNotCarriedAcrossADepthWriter)
{
	for (GSFeedbackCarry carry : {GSFeedbackCarry::NonReaders, GSFeedbackCarry::All})
	{
		const GSDrawRoad writer = Decide(carry, false, true);
		EXPECT_TRUE(writer.carry_rt);
		EXPECT_FALSE(writer.carry_depth);

		const GSDrawRoad non_writer = Decide(carry, false, false);
		EXPECT_TRUE(non_writer.carry_depth);
	}
}

// The composed rule equals the per-draw carry it replaced, over every input.
TEST(GSDrawRoad, CarrySwept)
{
	for (int bits = 0; bits < 512; bits++)
	{
		GSFeedbackLoopCarryInputs dev;
		dev.device_always_carries = (bits & 1) != 0;
		dev.device_is_measured_vendor = (bits & 2) != 0;
		dev.framebuffer_fetch = (bits & 4) != 0;
		dev.feedback_loop_layout = (bits & 8) != 0;
		dev.device_is_layout_road_vendor = (bits & 16) != 0;
		dev.barriers_order_reads = (bits & 32) != 0;
		dev.device_is_barrier_road_vendor = (bits & 64) != 0;
		const bool any_barrier = (bits & 128) != 0;
		const bool writes_depth = (bits & 256) != 0;

		const bool ordered = dev.feedback_loop_layout ?
		                         (dev.device_is_layout_road_vendor ||
		                             (dev.barriers_order_reads && dev.device_is_barrier_road_vendor)) :
		                         (dev.device_is_measured_vendor && dev.framebuffer_fetch);
		const bool colour = dev.device_always_carries || (ordered && !any_barrier);

		const GSDrawRoad road = Decide(GSFeedbackCarryForDevice(dev), any_barrier, writes_depth);
		EXPECT_EQ(road.carry_rt, colour) << "bits=" << bits;
		EXPECT_EQ(road.carry_depth, colour && !writes_depth) << "bits=" << bits;
	}
}

// --- Feedback-loop bits and the clone --------------------------------------------------------

// The pipeline's feedback bits follow the read on every in-pass road, whether or not the draw kept
// its barriers; the copy road has none and clones instead.
TEST(GSDrawRoad, FeedbackBitsFollowTheRead)
{
	const GSDrawRoad in_pass = GSDecideDrawRoad(BarrierRoad(), {.reads_rt = true, .reads_depth = true});
	EXPECT_TRUE(in_pass.rt_loop);
	EXPECT_TRUE(in_pass.depth_loop);
	EXPECT_FALSE(in_pass.clone_rt);

	const GSDrawRoad copy = GSDecideDrawRoad(CopyRoad(), {.reads_rt = true, .reads_depth = true, .one_barrier = true});
	EXPECT_FALSE(copy.rt_loop);
	EXPECT_FALSE(copy.depth_loop);
	EXPECT_TRUE(copy.clone_rt);
}

// The copy road clones only for a one-barrier draw that reads the target in either pass.
TEST(GSDrawRoad, CloneSwept)
{
	for (int bits = 0; bits < 16; bits++)
	{
		GSDrawRoadDevice dev;
		dev.texture_barrier = (bits & 1) != 0;
		GSDrawRoadDraw draw;
		draw.reads_rt = (bits & 2) != 0;
		draw.second_pass_reads_rt = (bits & 4) != 0;
		draw.one_barrier = (bits & 8) != 0;

		const bool expected = !dev.texture_barrier && draw.one_barrier && (draw.reads_rt || draw.second_pass_reads_rt);
		EXPECT_EQ(GSDecideDrawRoad(dev, draw).clone_rt, expected) << "bits=" << bits;
	}
}

// An offset read whose filter reaches pixels the same draw writes is cloned on the barrier road
// too: a barrier orders the read against earlier draws, not against this draw's own writes.
// Beyond Good & Evil's bloom blur reads one column into its own write area and came out different
// from run to run on Honeykrisp until it was.
TEST(GSDrawRoad, OffsetReadIntoItsOwnWriteAreaClones)
{
	const GSDrawRoadDraw hits = {.reads_rt = true, .one_barrier = true, .offset_read_hits_write = true};
	EXPECT_TRUE(GSDecideDrawRoad(BarrierRoad(), hits).clone_rt);
	EXPECT_TRUE(GSDecideDrawRoad(DeclaredOrderedRoad(), hits).clone_rt);
	EXPECT_TRUE(GSDecideDrawRoad(CopyRoad(), hits).clone_rt);

	// The read stays declared: the live target is still bound as an attachment it reads.
	EXPECT_TRUE(GSDecideDrawRoad(BarrierRoad(), hits).rt_loop);

	const GSDrawRoadDraw misses = {.reads_rt = true, .one_barrier = true};
	EXPECT_FALSE(GSDecideDrawRoad(BarrierRoad(), misses).clone_rt);
}

// Sampling the attached depth buffer is read-only depth feedback unless the draw already reads and
// writes it, and needs no texture barrier.
TEST(GSDrawRoad, DepthReadIsReadOnlyFeedback)
{
	EXPECT_TRUE(GSDecideDrawRoad(CopyRoad(), {.samples_attached_depth = true}).depth_read);
	EXPECT_TRUE(GSDecideDrawRoad(BarrierRoad(), {.samples_attached_depth = true}).depth_read);

	const GSDrawRoad rw = GSDecideDrawRoad(BarrierRoad(), {.reads_depth = true, .samples_attached_depth = true});
	EXPECT_TRUE(rw.depth_loop);
	EXPECT_FALSE(rw.depth_read);
}
