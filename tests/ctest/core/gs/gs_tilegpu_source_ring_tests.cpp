// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu source-descriptor ring's DEPTH: how many sets rule 3's frame-wide materialised
// sources are bound through, before the ring comes round to one it has to reuse.
//
// Nothing about depth can reach a pixel. Every set in the ring carries identical descriptors, and
// which physical index a plan lands on is invisible to the shader -- so this is a lever on ONE
// number, the count of times the GS thread blocks mid-plan waiting for a set's last reader to
// retire (GSDeviceVK::GpuWaitCause::SourceSet). That is why the resolution is worth pinning and the
// ring itself is not: the arithmetic below is the whole of the policy, and everything past it is a
// Vulkan resource count no unit test can hold.
//
// Why depth matters at all, since the shipped 8 looked obviously sufficient: a set is written once
// per PLAN, not once per frame, and a mid-frame plan flush is a plan. The corpus's flush rate spans
// two orders of magnitude -- Katamari 1.5-2 a frame, SotC 5.9, Ace Combat 5 6.8, Yu-Gi-Oh 12.5,
// OutRun 42, GT4 52 -- so at depth 8 the ring reached back four frames on one title and two thirds
// of a frame on another. The SD865's steady-state blocking waits ranked in that same order.
//
// What is pinned here:
//
//  - ZERO takes the built-in default. The shipped arm, and the one a capture arm must leave alone.
//  - A POSITIVE forces that depth, which is the A/B road back from whatever the default becomes.
//  - A NEGATIVE has no meaning -- unlike a pass cap there is no "off" position for a ring -- and
//    reads as the default rather than as an error. A dev-only key should not be able to refuse a
//    run over a typo.
//  - Both BOUNDS clamp rather than reject, at exactly the boundary and not one either side. The
//    lower bound is what stops a forced depth from configuring a ring that cannot hold the set a
//    plan is currently reading; the upper bound is what stops a typo from asking for a descriptor
//    pool nobody sized for.
//  - The default is inside its own bounds, which is the one way this could be self-inconsistent.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

TEST(TileGpuSourceRing, ZeroTakesTheBuiltInDefault)
{
	// The shipped arm.
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(0), kGSTileGpuSourceSetRingDefault);
}

TEST(TileGpuSourceRing, PositiveForcesThatDepth)
{
	// The A/B road back. Both directions have to work: the whole point of the lever is being able
	// to run the OLD depth against the new default on a device.
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(8), 8u) << "the depth this shipped as, which is the control arm";
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(16), 16u);
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(64), 64u);
}

TEST(TileGpuSourceRing, NegativeMeansNothingAndReadsAsTheDefault)
{
	// A pass cap's negative means "no cap". A ring has no such position -- a ring of no sets is not
	// a configuration -- so a negative is a typo, and a typo in a dev-only key should cost a log
	// line at most, never the run.
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(-1), kGSTileGpuSourceSetRingDefault);
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(-64), kGSTileGpuSourceSetRingDefault);
}

TEST(TileGpuSourceRing, BoundsClampAtExactlyTheBoundary)
{
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(1), kGSTileGpuSourceSetRingMin) << "below the floor clamps up";
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(static_cast<int>(kGSTileGpuSourceSetRingMin)),
		kGSTileGpuSourceSetRingMin)
		<< "the floor itself is admitted";
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(static_cast<int>(kGSTileGpuSourceSetRingMax)),
		kGSTileGpuSourceSetRingMax)
		<< "the ceiling itself is admitted";
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(static_cast<int>(kGSTileGpuSourceSetRingMax) + 1),
		kGSTileGpuSourceSetRingMax)
		<< "above the ceiling clamps down";
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(1 << 20), kGSTileGpuSourceSetRingMax) << "and stays clamped, however wild";
}

TEST(TileGpuSourceRing, TheDefaultIsInsideItsOwnBounds)
{
	// The self-consistency check: a default outside the clamp would make the "zero asks the
	// built-in" arm silently a different depth from the constant everything else quotes.
	static_assert(kGSTileGpuSourceSetRingMin <= kGSTileGpuSourceSetRingDefault, "default below the floor");
	static_assert(kGSTileGpuSourceSetRingDefault <= kGSTileGpuSourceSetRingMax, "default above the ceiling");
	EXPECT_EQ(gsTileGpuSourceSetRingDepth(0, kGSTileGpuSourceSetRingDefault), kGSTileGpuSourceSetRingDefault);
}

TEST(TileGpuSourceRing, TheDepthsTheMeasurementAsksForAreReachable)
{
	// The measurement the numbers were chosen against, pinned so a later shrink has to argue with it
	// rather than with a bare constant. Set writes per frame, M2, 2026-08-24, one per plan: GT4 52,
	// OutRun 43, Yu-Gi-Oh 13, AC5 7, SotC 6, Katamari 2. A ring has to reach back at least a full
	// frame of those for a title whose GPU is a frame behind to find a retired set.
	constexpr u32 kWorstMeasuredWritesPerFrame = 52;
	EXPECT_GE(kGSTileGpuSourceSetRingMax, kWorstMeasuredWritesPerFrame)
		<< "the ceiling has to be reachable by the heaviest title, or the lever cannot ask the question";
	EXPECT_GT(kGSTileGpuSourceSetRingDefault, 8u) << "deeper than the depth this shipped as";
	EXPECT_GE(kGSTileGpuSourceSetRingDefault, 13u)
		<< "the default reaches a frame back on every corpus title but the two heaviest";
}
