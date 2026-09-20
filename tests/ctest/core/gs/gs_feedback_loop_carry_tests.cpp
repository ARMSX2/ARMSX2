// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the feedback-loop carry decision (GS/Renderers/Common/GSFeedbackLoopCarryPolicy.h).
//
// The carry keeps a render pass open across a run of draws on one target once one of them has
// declared the pass self-reading, instead of ending the pass to clear the flag for the next
// non-reader. It exists because a tiler pays a full tile store and reload at every pass boundary,
// and on the framebuffer-fetch path the flag costs nothing to leave set.
//
// There are two such paths -- framebuffer fetch on Mali, and the attachment-feedback-loop layout
// on Adreno under Turnip, where the declaration is itself what orders the read.
//
// What these tests are for is the OTHER half: every device that is not on one of those paths must
// reach exactly the behaviour it had before, and there is no way to see that on the machine the
// gates run on, which is neither vendor. So the decision was written as a pure function and the
// no-change cases are pinned here by name -- including the M2, which DOES take the layout road and
// still must not carry.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSFeedbackLoopCarryPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// A device on the framebuffer-fetch path.
	constexpr GSFeedbackLoopCarryInputs MaliWithFetch()
	{
		GSFeedbackLoopCarryInputs in;
		in.device_is_measured_vendor = true;
		in.framebuffer_fetch = true;
		return in;
	}

	// A desktop GPU: no fetch, no vendor match. A shipping SD865 lands here too -- it takes the
	// copy road, with neither in-pass spelling live.
	constexpr GSFeedbackLoopCarryInputs OffTheFetchPath()
	{
		return GSFeedbackLoopCarryInputs();
	}

	// Adreno under Turnip on the attachment-feedback-loop layout road. The declaration is what
	// orders the read there: Turnip refuses to tile a pass holding a pipeline that declares a
	// texture feedback loop, and on the untiled path the same declaration programs the coherent
	// primitive mode. In this tree that combination is not reachable -- UseFeedbackLoopLayout()
	// wants the rasterization-order extension absent and Turnip advertises it -- which is why the
	// decision is pinned here as a function rather than observed on a device.
	constexpr GSFeedbackLoopCarryInputs AdrenoOnTheLayoutRoad()
	{
		GSFeedbackLoopCarryInputs in;
		in.device_is_layout_road_vendor = true;
		in.feedback_loop_layout = true;
		return in;
	}

	// The M2. The layout road is its DEFAULT road, because Honeykrisp advertises no
	// rasterization-order extension -- but its self-read is ordered by the backend's own barriers,
	// not by any driver primitive mode, and it is the machine every byte-identity gate in this
	// programme runs on. It must reach exactly the behaviour it had before.
	constexpr GSFeedbackLoopCarryInputs M2OnTheLayoutRoad()
	{
		GSFeedbackLoopCarryInputs in;
		in.feedback_loop_layout = true;
		return in;
	}
} // namespace

TEST(GSFeedbackLoopCarry, FetchPathCarries)
{
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(MaliWithFetch()));
}

// The gate that makes every guard device a no-op by construction. Without fetch the destination
// read is a copy of the target, so declaring the pass self-reading buys nothing and the reasoning
// that says the carry is free does not apply.
TEST(GSFeedbackLoopCarry, NoFetchNoCarry)
{
	GSFeedbackLoopCarryInputs in = MaliWithFetch();
	in.framebuffer_fetch = false;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in));
}

TEST(GSFeedbackLoopCarry, OffTheFetchPathNothingCarries)
{
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(OffTheFetchPath()));

	// ...including a device that has fetch but is not the vendor the carry was measured on.
	// Widening it there is a separate decision with its own device round.
	GSFeedbackLoopCarryInputs other_vendor = OffTheFetchPath();
	other_vendor.framebuffer_fetch = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(other_vendor));
}

// The attachment-feedback-loop image layout is the other way of reading the target. It has its own
// carry now (below), on its own vendor -- so the fetch vendor does not inherit it, and a device
// that somehow reported both spellings would be answered by the layout road's term alone.
TEST(GSFeedbackLoopCarry, FeedbackLoopLayoutDoesNotCarry)
{
	GSFeedbackLoopCarryInputs in = MaliWithFetch();
	in.feedback_loop_layout = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in));
}

// The layout road's carry, on the device whose driver orders the read. Its depth half follows the
// enclosing decision exactly as the fetch road's does.
TEST(GSFeedbackLoopCarry, LayoutRoadCarriesOnTheDeviceThatOrdersTheRead)
{
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(AdrenoOnTheLayoutRoad()));
	EXPECT_TRUE(CarryDepthFeedbackAcrossTargetRun(AdrenoOnTheLayoutRoad()));
}

// The guard that makes this lane inert on every machine the gates run on. The M2 takes the layout
// road by default, so it is the row that would move if the road alone were the condition.
TEST(GSFeedbackLoopCarry, LayoutRoadDoesNotCarryOnAnyOtherDevice)
{
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(M2OnTheLayoutRoad()));
	EXPECT_FALSE(CarryDepthFeedbackAcrossTargetRun(M2OnTheLayoutRoad()));

	// ...and having the fetch road's vendor does not help it, on the road it is not measured on.
	GSFeedbackLoopCarryInputs fetch_vendor = M2OnTheLayoutRoad();
	fetch_vendor.device_is_measured_vendor = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(fetch_vendor));
}

// The road is a condition, not just a vendor: the same device with neither in-pass spelling live
// is a shipping Adreno on the copy road, and carries nothing.
TEST(GSFeedbackLoopCarry, TheLayoutVendorNeedsTheLayoutRoad)
{
	GSFeedbackLoopCarryInputs copy_road;
	copy_road.device_is_layout_road_vendor = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(copy_road));
}

// The barrier term reaches the new road for the same reason it reaches the old one: the backend
// hands SendHWDraw a target to barrier against only when the pipeline's feedback bit is set.
TEST(GSFeedbackLoopCarry, ALayoutRoadBarrierRequestBlocksTheCarry)
{
	GSFeedbackLoopCarryInputs in = AdrenoOnTheLayoutRoad();
	in.draw_needs_own_barrier = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in));
}

// The fetch road's answer is untouched by the layout road's vendor term, either way.
TEST(GSFeedbackLoopCarry, TheFetchRoadIsUnchangedByTheLayoutTerm)
{
	GSFeedbackLoopCarryInputs in = MaliWithFetch();
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	in.device_is_layout_road_vendor = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));
}

// The carry must never be the thing that introduces a barrier: the backend hands SendHWDraw a
// target to barrier against only when the pipeline's feedback bit is set, so carrying the bit onto
// a draw that still asks for a barrier would emit one that was not emitted before.
TEST(GSFeedbackLoopCarry, ABarrierRequestBlocksTheCarry)
{
	GSFeedbackLoopCarryInputs in = MaliWithFetch();
	in.draw_needs_own_barrier = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in));
}

// Broadcom carried the flag unconditionally before this policy existed. Nothing here may change
// that: the barrier term does not reach it.
TEST(GSFeedbackLoopCarry, BroadcomCarryIsUnchanged)
{
	GSFeedbackLoopCarryInputs in;
	in.device_always_carries = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	in.draw_needs_own_barrier = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));
}

// The invariant, swept: carrying requires a road AND that road's own measured vendor, whatever
// else is true. (This replaces a sweep that said "requires the fetch path", which stopped being
// the whole rule when the layout road got a carry of its own.) It is the statement every guard
// device needs -- a shipping Adreno is the all-false row, the M2 is the feedback_loop_layout row
// with no vendor term -- and it fails if a later term is added that can carry without one.
TEST(GSFeedbackLoopCarry, CarryingAlwaysRequiresARoadAndItsVendor)
{
	for (int bits = 0; bits < 32; bits++)
	{
		GSFeedbackLoopCarryInputs in;
		in.device_is_measured_vendor = (bits & 1) != 0;
		in.framebuffer_fetch = (bits & 2) != 0;
		in.feedback_loop_layout = (bits & 4) != 0;
		in.draw_needs_own_barrier = (bits & 8) != 0;
		in.device_is_layout_road_vendor = (bits & 16) != 0;

		const bool road = in.feedback_loop_layout ?
		                      in.device_is_layout_road_vendor :
		                      (in.device_is_measured_vendor && in.framebuffer_fetch);
		EXPECT_EQ(CarryFeedbackLoopAcrossTargetRun(in), road && !in.draw_needs_own_barrier)
			<< "bits=" << bits;
	}
}

// The depth half. A pass carrying the depth feedback bits is a pass in which nothing wrote the
// depth being sampled -- the renderer only lets a draw sample its own depth buffer when it does
// not write it -- so carrying those bits onto a draw that WRITES depth puts a depth writer and a
// depth sampler in one pass and leaves the depth image in the feedback layout while it is written.
TEST(GSFeedbackLoopCarry, DepthBitsAreNotCarriedAcrossADepthWriter)
{
	GSFeedbackLoopCarryInputs writer = MaliWithFetch();
	writer.draw_writes_depth = true;

	// The colour carry is untouched: the pass is still kept open for it.
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(writer));
	EXPECT_FALSE(CarryDepthFeedbackAcrossTargetRun(writer));
}

// The case that must stay carried: a non-reader that does not write depth, inside a run whose
// pass already declared the depth sampling. Dropping this one would give back the pass boundary
// the carry exists to remove.
TEST(GSFeedbackLoopCarry, DepthBitsStayCarriedAcrossANonWriter)
{
	GSFeedbackLoopCarryInputs non_writer = MaliWithFetch();
	non_writer.draw_writes_depth = false;

	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(non_writer));
	EXPECT_TRUE(CarryDepthFeedbackAcrossTargetRun(non_writer));
}

// The unconditional Broadcom carry is not an exemption from this. Its colour carry predates the
// policy and stays unconditional; its depth carry across a depth writer is the same hazard on the
// same hardware class, so the depth term reaches it.
TEST(GSFeedbackLoopCarry, BroadcomDepthCarryStopsAtADepthWriter)
{
	GSFeedbackLoopCarryInputs in;
	in.device_always_carries = true;
	EXPECT_TRUE(CarryDepthFeedbackAcrossTargetRun(in));

	in.draw_writes_depth = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));
	EXPECT_FALSE(CarryDepthFeedbackAcrossTargetRun(in));
}

// Swept: the depth answer is the colour answer with the depth writer removed, for every
// combination of the rest. If a term is ever added that lets the depth bits through on their own,
// this is what catches it.
TEST(GSFeedbackLoopCarry, DepthCarryIsTheColourCarryMinusDepthWriters)
{
	for (int bits = 0; bits < 128; bits++)
	{
		GSFeedbackLoopCarryInputs in;
		in.device_always_carries = (bits & 1) != 0;
		in.device_is_measured_vendor = (bits & 2) != 0;
		in.framebuffer_fetch = (bits & 4) != 0;
		in.feedback_loop_layout = (bits & 8) != 0;
		in.draw_needs_own_barrier = (bits & 16) != 0;
		in.draw_writes_depth = (bits & 32) != 0;
		in.device_is_layout_road_vendor = (bits & 64) != 0;

		const bool colour = CarryFeedbackLoopAcrossTargetRun(in);
		EXPECT_EQ(CarryDepthFeedbackAcrossTargetRun(in), colour && !in.draw_writes_depth)
			<< "bits=" << bits;
	}
}
