// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the feedback-loop carry decision (GS/Renderers/Common/GSFeedbackLoopCarryPolicy.h).
//
// The carry keeps a render pass open across a run of draws on one target once one of them has
// declared the pass self-reading, instead of ending the pass to clear the flag for the next
// non-reader. It exists because a tiler pays a full tile store and reload at every pass boundary,
// and on the framebuffer-fetch path the flag costs nothing to leave set.
//
// The rule is that a run may be latched wherever the ROAD orders the read, and there are three
// ways a road does that: rasterization-order attachment access on the framebuffer-fetch path
// (Mali), the driver's own coherent primitive mode on the attachment-feedback-loop layout path
// (Adreno under Turnip), and this backend's explicit per-draw feedback barriers on that same
// layout path everywhere else (Apple silicon under Honeykrisp, which takes the road by default).
//
// What these tests are for is the other half: a road with no ordering must reach exactly the
// behaviour it had before, and most of those combinations cannot be produced on any machine in
// the building. So the decision was written as a pure function and every row is pinned here by
// name -- the copy road, the layout road with texture barriers off, and each road's term proven
// not to leak into the other's answer.
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

	// The M2 on its shipped road. The layout road is its DEFAULT road, because Honeykrisp
	// advertises no rasterization-order extension, and with texture barriers on the backend orders
	// the self-read itself: SendHWDraw emits a framebuffer-local feedback barrier for every draw
	// that reads its own target. No driver primitive mode is involved, and none is needed.
	constexpr GSFeedbackLoopCarryInputs M2OnTheLayoutRoad()
	{
		GSFeedbackLoopCarryInputs in;
		in.feedback_loop_layout = true;
		in.barriers_order_reads = true;
		return in;
	}

	// The same machine with -no-tex-barriers. Nothing asks for a feedback barrier, so no draw reads
	// its own target and the road orders nothing -- the case the carry must not fire on.
	constexpr GSFeedbackLoopCarryInputs LayoutRoadWithNoOrdering()
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

// The two spellings are mutually exclusive on Vulkan, but the decision does not assume it: a
// device reporting both is answered by the layout road's terms alone, and the fetch vendor does
// not carry over into them. Here neither layout-road ordering is present, so it refuses.
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

// The layout road's other ordering, and the one that is live on hardware today: the backend's own
// per-draw feedback barriers. No vendor term, and it carries.
TEST(GSFeedbackLoopCarry, LayoutRoadCarriesWhenBarriersOrderTheRead)
{
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(M2OnTheLayoutRoad()));
	EXPECT_TRUE(CarryDepthFeedbackAcrossTargetRun(M2OnTheLayoutRoad()));
}

// The road with neither ordering carries nothing. This is the M2 under -no-tex-barriers, and it is
// what makes that arm inert by construction rather than by measurement: with no barrier there is
// no reader, so there is nothing for a latched pass to have been ordered against.
TEST(GSFeedbackLoopCarry, LayoutRoadDoesNotCarryWithNothingOrderingTheRead)
{
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(LayoutRoadWithNoOrdering()));
	EXPECT_FALSE(CarryDepthFeedbackAcrossTargetRun(LayoutRoadWithNoOrdering()));

	// ...and having the fetch road's vendor does not supply an ordering it does not have.
	GSFeedbackLoopCarryInputs fetch_vendor = LayoutRoadWithNoOrdering();
	fetch_vendor.device_is_measured_vendor = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(fetch_vendor));
}

// The four ways the layout road can be ordered, in one place: neither source, each alone, both.
// Either one is enough and neither is required of the other.
TEST(GSFeedbackLoopCarry, TheLayoutRoadNeedsEitherOrderingAndNotBoth)
{
	GSFeedbackLoopCarryInputs in;
	in.feedback_loop_layout = true;

	for (int driver = 0; driver < 2; driver++)
	{
		for (int barriers = 0; barriers < 2; barriers++)
		{
			in.device_is_layout_road_vendor = driver != 0;
			in.barriers_order_reads = barriers != 0;
			EXPECT_EQ(CarryFeedbackLoopAcrossTargetRun(in), driver != 0 || barriers != 0)
				<< "driver=" << driver << " barriers=" << barriers;
		}
	}
}

// The road is a condition, not just a vendor: the same device with neither in-pass spelling live
// is a shipping Adreno on the copy road, and carries nothing.
TEST(GSFeedbackLoopCarry, TheLayoutVendorNeedsTheLayoutRoad)
{
	GSFeedbackLoopCarryInputs copy_road;
	copy_road.device_is_layout_road_vendor = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(copy_road));
}

// And neither does the barrier term. Texture barriers are on for every device that reaches this
// decision with either in-pass spelling live, so this is the row that would move if the ordering
// input were ever consulted outside its road: a desktop GPU on the copy road with barriers on.
TEST(GSFeedbackLoopCarry, BarriersAloneAreNotARoad)
{
	GSFeedbackLoopCarryInputs copy_road;
	copy_road.barriers_order_reads = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(copy_road));

	// ...including on a device that has fetch but is not the vendor the fetch carry was measured
	// on. Barriers do not stand in for measuring that vendor.
	GSFeedbackLoopCarryInputs unmeasured_fetch;
	unmeasured_fetch.framebuffer_fetch = true;
	unmeasured_fetch.barriers_order_reads = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(unmeasured_fetch));
}

// The barrier term reaches the new road for the same reason it reaches the old one: the backend
// hands SendHWDraw a target to barrier against only when the pipeline's feedback bit is set.
TEST(GSFeedbackLoopCarry, ALayoutRoadBarrierRequestBlocksTheCarry)
{
	GSFeedbackLoopCarryInputs in = AdrenoOnTheLayoutRoad();
	in.draw_needs_own_barrier = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in));

	// The barrier-ordered road is where this term does its real work: it is what keeps the READING
	// draws out of the carry, so they go on setting their own flag word and emitting their own
	// barrier inside the held-open pass. A road ordered by barriers that also latched its readers
	// would be latching the draws whose ordering it depends on.
	GSFeedbackLoopCarryInputs reader = M2OnTheLayoutRoad();
	reader.draw_needs_own_barrier = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(reader));
	EXPECT_FALSE(CarryDepthFeedbackAcrossTargetRun(reader));
}

// The fetch road's answer is untouched by the layout road's vendor term, either way.
TEST(GSFeedbackLoopCarry, TheFetchRoadIsUnchangedByTheLayoutTerm)
{
	GSFeedbackLoopCarryInputs in = MaliWithFetch();
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	in.device_is_layout_road_vendor = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	// The barrier term is true on Mali too -- framebuffer_fetch is masked by texture_barrier, so
	// fetch being live means barriers are on -- and it must not be what that road's answer rests
	// on. Removing the fetch vendor still refuses, with barriers on either way.
	in.barriers_order_reads = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	in.device_is_layout_road_vendor = false;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	in.device_is_measured_vendor = false;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in));
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

// The invariant, swept: carrying requires a road that orders the read, whatever else is true. The
// fetch road is ordered by rasterization-order access and is scoped to the vendor it was measured
// on; the layout road is ordered by the driver or by our own barriers, either one. A shipping
// Adreno is the all-false row and the M2 under -no-tex-barriers is the feedback_loop_layout row
// with neither ordering term. It fails if a later term is ever added that can carry on no road.
TEST(GSFeedbackLoopCarry, CarryingAlwaysRequiresARoadAndItsVendor)
{
	for (int bits = 0; bits < 128; bits++)
	{
		GSFeedbackLoopCarryInputs in;
		in.device_is_measured_vendor = (bits & 1) != 0;
		in.framebuffer_fetch = (bits & 2) != 0;
		in.feedback_loop_layout = (bits & 4) != 0;
		in.draw_needs_own_barrier = (bits & 8) != 0;
		in.device_is_layout_road_vendor = (bits & 16) != 0;
		in.barriers_order_reads = (bits & 32) != 0;
		in.override_off = (bits & 64) != 0;

		const bool road = in.feedback_loop_layout ?
		                      (in.device_is_layout_road_vendor || in.barriers_order_reads) :
		                      (in.device_is_measured_vendor && in.framebuffer_fetch);
		EXPECT_EQ(CarryFeedbackLoopAcrossTargetRun(in), road && !in.draw_needs_own_barrier && !in.override_off)
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
	for (int bits = 0; bits < 512; bits++)
	{
		GSFeedbackLoopCarryInputs in;
		in.device_always_carries = (bits & 1) != 0;
		in.device_is_measured_vendor = (bits & 2) != 0;
		in.framebuffer_fetch = (bits & 4) != 0;
		in.feedback_loop_layout = (bits & 8) != 0;
		in.draw_needs_own_barrier = (bits & 16) != 0;
		in.draw_writes_depth = (bits & 32) != 0;
		in.device_is_layout_road_vendor = (bits & 64) != 0;
		in.barriers_order_reads = (bits & 128) != 0;
		in.override_off = (bits & 256) != 0;

		const bool colour = CarryFeedbackLoopAcrossTargetRun(in);
		EXPECT_EQ(CarryDepthFeedbackAcrossTargetRun(in), colour && !in.draw_writes_depth)
			<< "bits=" << bits;
	}
}

// ---------------------------------------------------------------------------------------------
// The harness override (gsrunner -no-feedback-carry).
//
// Its whole job is to be unconditional. A declared-road arm that measures slow has two candidate
// causes -- the declaration on the draws that read, and this carry handing the same pipeline
// create flag to every draw in the latched pass -- and they cannot be told apart unless one of
// them can be switched off with everything else held still.
// ---------------------------------------------------------------------------------------------

// Every road that carries stops carrying, colour and depth alike.
TEST(GSFeedbackLoopCarry, TheOverrideStopsEveryRoad)
{
	GSFeedbackLoopCarryInputs fetch = MaliWithFetch();
	fetch.override_off = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(fetch));
	EXPECT_FALSE(CarryDepthFeedbackAcrossTargetRun(fetch));

	GSFeedbackLoopCarryInputs layout = AdrenoOnTheLayoutRoad();
	layout.override_off = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(layout));
	EXPECT_FALSE(CarryDepthFeedbackAcrossTargetRun(layout));
}

// Including the one carry that no device fact can switch off. A device that could out-vote the
// override would make one row of the A/B silently measure the other arm.
TEST(GSFeedbackLoopCarry, TheOverrideBeatsTheUnconditionalCarry)
{
	GSFeedbackLoopCarryInputs in;
	in.device_always_carries = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	in.override_off = true;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in));
	EXPECT_FALSE(CarryDepthFeedbackAcrossTargetRun(in));
}

// And left alone it is not there at all: the default-constructed field reproduces every answer
// this file pinned before the override existed. This is the inertness gate in miniature -- the
// 94-cell byte-identity check is the same statement about the whole binary.
TEST(GSFeedbackLoopCarry, TheOverrideIsInertWhenNotAsked)
{
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(MaliWithFetch()));
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(AdrenoOnTheLayoutRoad()));
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(M2OnTheLayoutRoad()));
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(LayoutRoadWithNoOrdering()));
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(OffTheFetchPath()));

	GSFeedbackLoopCarryInputs broadcom;
	broadcom.device_always_carries = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(broadcom));
}

// The process-wide switch itself: default off, settable, readable. It is a plain inline global
// rather than a setting because which road a device should take here is a measurement result and
// a user has no way to know which side of the trade their driver is on.
TEST(GSFeedbackLoopCarry, TheProcessOverrideDefaultsOff)
{
	EXPECT_FALSE(GSFeedbackLoopCarryPolicy::IsForcedOff());

	GSFeedbackLoopCarryPolicy::SetForcedOff(true);
	EXPECT_TRUE(GSFeedbackLoopCarryPolicy::IsForcedOff());

	GSFeedbackLoopCarryPolicy::SetForcedOff(false);
	EXPECT_FALSE(GSFeedbackLoopCarryPolicy::IsForcedOff());
}
