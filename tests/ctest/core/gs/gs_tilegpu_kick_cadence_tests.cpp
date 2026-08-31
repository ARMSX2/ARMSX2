// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu pass-tail kick's DECISION: at a pass boundary, does the executor offer to submit the
// work it has recorded so far?
//
// Nothing about that decision can reach a pixel. Queue submissions execute in order, the plan's
// stream reservations are retained across the boundary, and every per-pass bind is re-established
// inside its pass -- so what a kick changes is WHEN recorded work is handed over, never what was
// recorded. That is why the arithmetic is worth pinning and the submit itself is not: the whole of
// the policy is below, and everything past it is Vulkan bookkeeping no unit test can hold.
//
// Why the composition needs a test at all. The kick landed with ONE trigger, near a readback, at a
// threshold of 8 render passes, and that trigger is worth -14.6% on GT4 Online Public Beta on the
// SD865. The cadence added here answers a different question -- a title that never reads back
// leaves the GPU idle for as long as the GS thread records, 35.99 ms of a 90.50 ms drawn frame on
// Stuntman, with the kick firing zero times because there is no readback for its gate to see. The
// two are OR'd rather than merged, and the arm that merged them (one uniform threshold of 32) would
// have quietly pushed the readback trigger from 8 out to 32 on every title that has readbacks,
// which is a retune of a landed win that nothing measured. So "additive, not a replacement" is a
// decision with a number behind it, and this is where it is held.
//
// What is pinned here:
//
//  - ZERO cadence is OFF, and off means the near-readback trigger behaves EXACTLY as it did before
//    the cadence existed. That is the control arm of the device A/B; if it drifts, the A/B is
//    measuring two changes.
//  - A NEGATIVE cadence is off too. It has no second meaning left to carry (zero already means
//    off), and a dev-only key should fall back to the previously shipped behaviour on a typo
//    rather than refuse the run.
//  - A POSITIVE cadence is taken verbatim, with NO upper clamp: a cadence larger than any frame's
//    pass count is a cadence that never fires, and rewriting it to a ceiling would silently change
//    what a device record's arm meant.
//  - The two triggers are independent. Either alone offers; neither offers at zero passes.
//  - The near-readback trigger keeps its own threshold of 8 whatever the cadence is, so on a frame
//    near a readback the effective threshold is the smaller of the two.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

#include <limits>

TEST(TileGpuKickCadence, ZeroIsOffAndNegativeIsOffToo)
{
	// Zero is the shipped-before arm and the A/B's control. Negative is a typo, and a typo in a
	// dev-only key costs a log line at most, never the run -- and it lands on the SAFE side, which
	// is the behaviour that was already measured.
	EXPECT_EQ(gsTileGpuKickPassCadence(0), 0u);
	EXPECT_EQ(gsTileGpuKickPassCadence(-1), 0u);
	EXPECT_EQ(gsTileGpuKickPassCadence(-32), 0u);
	EXPECT_EQ(gsTileGpuKickPassCadence(std::numeric_limits<int>::min()), 0u);
}

TEST(TileGpuKickCadence, PositiveIsTakenVerbatimAndNeverClamped)
{
	// The A/B road, in both directions: the sweep that picked 32 ran 8 / 32 / 128 / 512, and every
	// one of those has to mean itself in a device record.
	EXPECT_EQ(gsTileGpuKickPassCadence(1), 1u);
	EXPECT_EQ(gsTileGpuKickPassCadence(8), 8u);
	EXPECT_EQ(gsTileGpuKickPassCadence(32), kGSTileGpuKickPassCadenceDefault) << "the shipped default";
	EXPECT_EQ(gsTileGpuKickPassCadence(128), 128u);
	EXPECT_EQ(gsTileGpuKickPassCadence(512), 512u);
	// No ceiling. A huge cadence is "never", and "never" is a legitimate arm.
	EXPECT_EQ(gsTileGpuKickPassCadence(1000000), 1000000u);
	EXPECT_EQ(gsTileGpuKickPassCadence(std::numeric_limits<int>::max()),
		static_cast<u32>(std::numeric_limits<int>::max()));
}

TEST(TileGpuKickCadence, TheShippedDefaultIsWhatTheSweepPicked)
{
	// 8 taxes the short frames through the per-submit GPU cost (SotC +2.02 ms, R&C effects +2.28)
	// and 512 gives most of Stuntman's gain back. This is the knee, and it is not a round number
	// chosen for looks.
	EXPECT_EQ(kGSTileGpuKickPassCadenceDefault, 32u);
	// And the near-readback trigger's threshold is UNDER it, which is what makes the two additive
	// rather than one shadowing the other on a readback title.
	EXPECT_LT(kGSTileGpuKickReadbackThreshold, kGSTileGpuKickPassCadenceDefault);
}

TEST(TileGpuKickCadence, CadenceOffLeavesTheNearReadbackTriggerExactlyAsItWas)
{
	// The control arm, pass by pass. Everything here is the behaviour that shipped: near a
	// readback, offer at 8 passes; not near one, never offer however long the frame runs.
	constexpr u32 kOff = 0;
	for (u32 passes = 0; passes < kGSTileGpuKickReadbackThreshold; passes++)
	{
		EXPECT_FALSE(gsTileGpuKickWantsSubmit(passes, kOff, true, false)) << "passes " << passes;
	}
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(kGSTileGpuKickReadbackThreshold, kOff, true, false));
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(kGSTileGpuKickReadbackThreshold + 1, kOff, true, false));

	// Not near a readback: no offer at any depth. This is exactly the hole the cadence fills --
	// Stuntman records 113 passes a frame here and offers zero times.
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(1, kOff, false, false));
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(64, kOff, false, false));
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(4096, kOff, false, false));
}

TEST(TileGpuKickCadence, ProducingReadbackDataOffersBeforeTheThreshold)
{
	// A pass that wrote a texture a recent pull read is almost certainly producing the data for the
	// next pull, so it does not wait for the count. But not at ZERO passes: there would be nothing
	// recorded to submit, and the submit would cost a command-buffer cycle for an empty buffer.
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(0, 0, true, true)) << "nothing recorded yet";
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(1, 0, true, true));
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(kGSTileGpuKickReadbackThreshold - 1, 0, true, true));

	// And it is inside the readback window's gate, not outside it: a stale pointer match on a frame
	// nothing has pulled from in three frames is not evidence of an imminent pull.
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(1, 0, false, true));
}

TEST(TileGpuKickCadence, TheCadenceFiresOnAnyFrameReadbackOrNot)
{
	// The whole point. No readback anywhere, and the offer comes at the cadence.
	constexpr u32 kCadence = kGSTileGpuKickPassCadenceDefault;
	for (u32 passes = 0; passes < kCadence; passes++)
	{
		EXPECT_FALSE(gsTileGpuKickWantsSubmit(passes, kCadence, false, false)) << "passes " << passes;
	}
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(kCadence, kCadence, false, false));
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(kCadence + 1, kCadence, false, false));

	// A cadence of one submits at every pass boundary -- the stress arm, and it has to be reachable.
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(1, 1, false, false));
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(0, 1, false, false)) << "still nothing recorded at zero passes";
}

TEST(TileGpuKickCadence, AdditiveNotAReplacement)
{
	// ⚠️ The decision this file exists for. With the cadence at 32, a frame near a readback must
	// still offer at 8 -- the threshold GT4 Online Public Beta's -14.6% was measured at. A
	// "replacement" composition (one uniform threshold of 32) would answer false here, and it would
	// answer false silently, on a title outside the probe set that measured the cadence.
	constexpr u32 kCadence = kGSTileGpuKickPassCadenceDefault;
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(kGSTileGpuKickReadbackThreshold, kCadence, true, false));
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(kCadence - 1, kCadence, true, false));
	// And with no readback near, the same pass count waits for the cadence.
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(kGSTileGpuKickReadbackThreshold, kCadence, false, false));
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(kCadence - 1, kCadence, false, false));

	// The converse direction: a cadence TIGHTER than the readback threshold binds on a readback
	// frame too, because the two are OR'd and the smaller wins.
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(2, 2, true, false));
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(2, 2, false, false));
}

TEST(TileGpuKickCadence, TheKeyAndThePredicateAgreeOnOff)
{
	// The two halves have to compose: what the key resolves to is what the predicate is handed, so
	// a negative key must reach the predicate as a cadence that never fires, not as a huge one.
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(100000, gsTileGpuKickPassCadence(-1), false, false));
	EXPECT_FALSE(gsTileGpuKickWantsSubmit(100000, gsTileGpuKickPassCadence(0), false, false));
	EXPECT_TRUE(gsTileGpuKickWantsSubmit(100000, gsTileGpuKickPassCadence(32), false, false));
}
