// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the feedback-loop carry decision (GS/Renderers/Common/GSFeedbackLoopCarryPolicy.h).
//
// The carry keeps a render pass open across a run of draws on one target once one of them has
// declared the pass self-reading, instead of ending the pass to clear the flag for the next
// non-reader. It exists because a tiler pays a full tile store and reload at every pass boundary,
// and on the framebuffer-fetch path the flag costs nothing to leave set.
//
// What these tests are for is the OTHER half: every device that is not on that path must reach
// exactly the behaviour it had before, and there is no way to see that on a desktop GPU, which
// never takes the fetch path at all. So the decision was written as a pure function and the
// no-change cases are pinned here by name.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSFeedbackLoopCarryPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// A device on the framebuffer-fetch path with the key at its default.
	constexpr GSFeedbackLoopCarryInputs MaliWithFetch()
	{
		GSFeedbackLoopCarryInputs in;
		in.device_is_measured_vendor = true;
		in.framebuffer_fetch = true;
		in.carry_key = true;
		return in;
	}

	// A desktop GPU: no fetch, no vendor match. The M2 and the SD865 both land here.
	constexpr GSFeedbackLoopCarryInputs OffTheFetchPath()
	{
		GSFeedbackLoopCarryInputs in;
		in.carry_key = true;
		return in;
	}
} // namespace

TEST(GSFeedbackLoopCarry, FetchPathCarries)
{
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(MaliWithFetch()));
}

// The key is what makes one binary run both arms of the device A/B. False must be the old
// behaviour exactly, on the same device.
TEST(GSFeedbackLoopCarry, KeyOffIsTheOldBehaviour)
{
	GSFeedbackLoopCarryInputs in = MaliWithFetch();
	in.carry_key = false;
	EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in));
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

// The attachment-feedback-loop image layout is the other way of reading the target, and it is not
// the one the "declaring a pass self-reading is free" argument was made about.
TEST(GSFeedbackLoopCarry, FeedbackLoopLayoutDoesNotCarry)
{
	GSFeedbackLoopCarryInputs in = MaliWithFetch();
	in.feedback_loop_layout = true;
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
// that: neither the key nor the barrier term reaches it.
TEST(GSFeedbackLoopCarry, BroadcomCarryIsUnchanged)
{
	GSFeedbackLoopCarryInputs in;
	in.device_always_carries = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	in.carry_key = false;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));

	in.draw_needs_own_barrier = true;
	EXPECT_TRUE(CarryFeedbackLoopAcrossTargetRun(in));
}

// The invariant, swept: with the key off and the unconditional device out of the picture, the
// answer is no for every combination of the remaining inputs. This is the statement the guard
// devices need, and it fails if a later term is added that can carry without the key.
TEST(GSFeedbackLoopCarry, KeyOffNeverCarriesAnywhere)
{
	for (int bits = 0; bits < 16; bits++)
	{
		GSFeedbackLoopCarryInputs in;
		in.carry_key = false;
		in.device_is_measured_vendor = (bits & 1) != 0;
		in.framebuffer_fetch = (bits & 2) != 0;
		in.feedback_loop_layout = (bits & 4) != 0;
		in.draw_needs_own_barrier = (bits & 8) != 0;
		EXPECT_FALSE(CarryFeedbackLoopAcrossTargetRun(in)) << "bits=" << bits;
	}
}

// And the same sweep with the key on: carrying requires the fetch path, whatever else is true.
TEST(GSFeedbackLoopCarry, KeyOnStillRequiresTheFetchPath)
{
	for (int bits = 0; bits < 16; bits++)
	{
		GSFeedbackLoopCarryInputs in;
		in.carry_key = true;
		in.device_is_measured_vendor = (bits & 1) != 0;
		in.framebuffer_fetch = (bits & 2) != 0;
		in.feedback_loop_layout = (bits & 4) != 0;
		in.draw_needs_own_barrier = (bits & 8) != 0;

		const bool expected = in.device_is_measured_vendor && in.framebuffer_fetch &&
		                      !in.feedback_loop_layout && !in.draw_needs_own_barrier;
		EXPECT_EQ(CarryFeedbackLoopAcrossTargetRun(in), expected) << "bits=" << bits;
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
// combination of the rest, key on and key off. If a term is ever added that lets the depth bits
// through on their own, this is what catches it.
TEST(GSFeedbackLoopCarry, DepthCarryIsTheColourCarryMinusDepthWriters)
{
	for (int bits = 0; bits < 64; bits++)
	{
		GSFeedbackLoopCarryInputs in;
		in.device_always_carries = (bits & 1) != 0;
		in.device_is_measured_vendor = (bits & 2) != 0;
		in.framebuffer_fetch = (bits & 4) != 0;
		in.feedback_loop_layout = (bits & 8) != 0;
		in.draw_needs_own_barrier = (bits & 16) != 0;
		in.draw_writes_depth = (bits & 32) != 0;

		for (const bool key : {false, true})
		{
			in.carry_key = key;
			const bool colour = CarryFeedbackLoopAcrossTargetRun(in);
			EXPECT_EQ(CarryDepthFeedbackAcrossTargetRun(in), colour && !in.draw_writes_depth)
				<< "bits=" << bits << " key=" << key;
		}
	}
}
