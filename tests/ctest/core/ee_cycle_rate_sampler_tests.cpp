// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The measurement half of the dynamic EE cycle-rate governor: what a window's frames add
// up to. No VM, no clock, no limiter - every test is an array of per-frame tick counts and
// the EECycleRateWindowSample they produce.
//
// The controller's sequence tests cannot catch a ratio that means something other than its
// name, because the controller believes whatever it is handed. The sampler is checked here.

#include "EECycleRateSampler.h"

#include "Config.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
	using Lateness = EECycleRateLateness;

	// One tick per nanosecond, so a "tick" reads as a nanosecond throughout and 16'666'667
	// is a 60 Hz frame. Arbitrary - the arithmetic only ever divides ticks by this.
	constexpr u64 kTickFrequency = 1'000'000'000;
	constexpr u64 kFrame60Hz = 16'666'667;

	// Thread CPU time keeps its own frequency, because it is a different clock.
	constexpr u64 kCpuTimeFrequency = 1'000'000'000;

	EECycleRateFrameSample Frame(u64 active, u64 gs_wait = 0, u64 vu1_wait = 0, u64 target = kFrame60Hz)
	{
		EECycleRateFrameSample f;
		f.active_ticks = active;
		f.target_ticks = target;
		f.gs_wait_ticks = gs_wait;
		f.vu1_wait_ticks = vu1_wait;
		f.late = (active > target);
		return f;
	}

	EECycleRateSampler::WindowInputs Inputs(const std::vector<EECycleRateFrameSample>& frames, u64 cpu_time_delta = 0)
	{
		EECycleRateSampler::WindowInputs in;
		in.frames = frames.data();
		in.frame_count = static_cast<u32>(frames.size());
		in.tick_frequency = kTickFrequency;
		in.cpu_time_delta = cpu_time_delta;
		in.cpu_time_frequency = kCpuTimeFrequency;
		return in;
	}

	std::vector<EECycleRateFrameSample> UniformWindow(u32 count, u64 active, u64 gs_wait = 0, u64 vu1_wait = 0)
	{
		return std::vector<EECycleRateFrameSample>(count, Frame(active, gs_wait, vu1_wait));
	}
} // namespace

TEST(EECycleRateSamplerWindow, ThirtyIdenticalFramesReportTheirOwnRatio)
{
	// Half the budget, nothing blocked: every ratio is the one ratio the frames had.
	const auto frames = UniformWindow(30, kFrame60Hz / 2);
	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_TRUE(s.valid);
	EXPECT_EQ(s.frames, 30u);
	EXPECT_NEAR(s.target_seconds, 0.5, 1e-6);
	EXPECT_NEAR(s.p90_active_ratio, 0.5f, 1e-4f);
	EXPECT_NEAR(s.late_ratio, 0.0f, 1e-6f);
	EXPECT_NEAR(s.own_time_ratio, 0.5f, 1e-4f);
	EXPECT_NEAR(s.blocked_ratio, 0.0f, 1e-6f);
}

TEST(EECycleRateSamplerWindow, P90IsNearestRankSoTheTopTenPercentCannotSetIt)
{
	// 27 frames at half budget, 3 at four times it. Nearest-rank p90 over 30 frames is the
	// 27th value ascending, so the three spikes sit above it and do not move it. An
	// interpolated percentile would land between 0.5 and 4.0, a ratio no frame had.
	std::vector<EECycleRateFrameSample> frames = UniformWindow(27, kFrame60Hz / 2);
	for (int i = 0; i < 3; i++)
		frames.push_back(Frame(kFrame60Hz * 4));

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_TRUE(s.valid);
	EXPECT_NEAR(s.p90_active_ratio, 0.5f, 1e-4f);
	EXPECT_NEAR(s.late_ratio, 3.0f / 30.0f, 1e-4f);
}

TEST(EECycleRateSamplerWindow, OneMoreSpikeCrossesTheRankAndMovesP90)
{
	// The boundary the test above sits next to: four spikes in thirty frames put the 27th
	// value into the spike group, so p90 jumps. If the rank formula drifts by one, exactly
	// one of these two tests fails, which is the point of having both.
	std::vector<EECycleRateFrameSample> frames = UniformWindow(26, kFrame60Hz / 2);
	for (int i = 0; i < 4; i++)
		frames.push_back(Frame(kFrame60Hz * 4));

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_NEAR(s.p90_active_ratio, 4.0f, 1e-4f);
}

TEST(EECycleRateSamplerWindow, BlockedTimeComesOutOfOwnTimeAndIntoBlockedRatio)
{
	// Every frame fills its budget, but three quarters of it is spent waiting on the GS
	// thread. The EE thread is busy by the wall clock and has nothing to give back, which
	// is the exact case an underclock cannot help - so own_time collapses and blocked
	// rises.
	const auto frames = UniformWindow(30, kFrame60Hz, (kFrame60Hz / 4) * 3);
	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_NEAR(s.p90_active_ratio, 1.0f, 1e-4f);
	EXPECT_NEAR(s.own_time_ratio, 0.25f, 1e-3f);
	EXPECT_NEAR(s.blocked_ratio, 0.75f, 1e-3f);
}

TEST(EECycleRateSamplerWindow, GSAndVU1WaitsAreOneBlockedTotal)
{
	// Two counters exist so a trace can say which thread the EE was behind. The ratios do
	// not care which: both come out of own time.
	const auto frames = UniformWindow(30, kFrame60Hz, kFrame60Hz / 4, kFrame60Hz / 4);
	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_NEAR(s.own_time_ratio, 0.5f, 1e-3f);
	EXPECT_NEAR(s.blocked_ratio, 0.5f, 1e-3f);
}

TEST(EECycleRateSamplerWindow, TheGSAndVU1SharesAddUpToTheBlockedRatio)
{
	const auto frames = UniformWindow(30, kFrame60Hz / 2, kFrame60Hz / 8, kFrame60Hz / 16);
	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_NEAR(s.gs_wait_ratio, 0.25f, 1e-3f);
	EXPECT_NEAR(s.vu1_wait_ratio, 0.125f, 1e-3f);
	EXPECT_NEAR(s.gs_wait_ratio + s.vu1_wait_ratio, s.blocked_ratio, 1e-5f);
}

TEST(EECycleRateSamplerWindow, AnOverlongWaitIsClampedGSFirstSoTheSharesStillAddUp)
{
	// Waits that together exceed the frame: the GS share keeps what it measured, VU1 gets
	// the rest of the frame, and the total never passes the frame.
	const auto frames = UniformWindow(30, kFrame60Hz / 2, (kFrame60Hz * 3) / 8, (kFrame60Hz * 3) / 8);
	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_NEAR(s.gs_wait_ratio, 0.75f, 1e-3f);
	EXPECT_NEAR(s.vu1_wait_ratio, 0.25f, 1e-3f);
	EXPECT_NEAR(s.blocked_ratio, 1.0f, 1e-5f);
	EXPECT_NEAR(s.own_time_ratio, 0.0f, 1e-5f);
}

TEST(EECycleRateSamplerWindow, AWaitLongerThanItsFrameIsClampedToTheFrame)
{
	// A wait that started before the frame boundary is charged to the frame it ended in,
	// so it can exceed that frame's active time. Clamping is per frame: one such frame
	// must not drive the window's own time negative.
	std::vector<EECycleRateFrameSample> frames = UniformWindow(29, kFrame60Hz / 2);
	frames.push_back(Frame(kFrame60Hz / 2, kFrame60Hz * 3));

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_TRUE(s.valid);
	EXPECT_GE(s.own_time_ratio, 0.0f);
	EXPECT_LE(s.blocked_ratio, 1.0f);
	EXPECT_NEAR(s.own_time_ratio, 29.0f * 0.5f / 30.0f, 1e-3f);
}

TEST(EECycleRateSamplerWindow, LateRatioCountsTheFramesTheLimiterCalledLate)
{
	// The flag is the limiter's verdict, carried per frame rather than re-derived, so the
	// arithmetic reports what it was given.
	auto frames = UniformWindow(30, kFrame60Hz / 2);
	for (int i = 0; i < 6; i++)
		frames[i].late = true;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_NEAR(s.late_ratio, 0.2f, 1e-4f);
	// And it did not quietly recompute it from the tick counts, which say no frame was late.
	EXPECT_NEAR(s.p90_active_ratio, 0.5f, 1e-4f);
}

TEST(EECycleRateSamplerWindow, CpuTimeRatioIsAgainstTargetTimeInItsOwnClock)
{
	// The thread CPU clock has its own frequency, so the ratio has to convert both sides to
	// seconds rather than dividing tick counts.
	const auto frames = UniformWindow(30, kFrame60Hz / 2);
	EECycleRateSampler::WindowInputs in = Inputs(frames, kCpuTimeFrequency / 4); // 0.25 s of CPU
	in.cpu_time_frequency = kCpuTimeFrequency;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(in);

	EXPECT_NEAR(s.target_seconds, 0.5, 1e-6);
	EXPECT_NEAR(s.cpu_time_ratio, 0.5f, 1e-3f);
}

TEST(EECycleRateSamplerWindow, NoCpuTimeClockIsNotAReasonToThrowTheWindowAway)
{
	const auto frames = UniformWindow(30, kFrame60Hz / 2);
	EECycleRateSampler::WindowInputs in = Inputs(frames);
	in.cpu_time_frequency = 0;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(in);

	EXPECT_TRUE(s.valid);
	EXPECT_NEAR(s.cpu_time_ratio, 0.0f, 1e-6f);
}

TEST(EECycleRateSamplerWindow, AnInvalidatedWindowKeepsItsNumbersButNotItsValidity)
{
	// Throttle() stopped pacing partway through - the limiter went unlimited, or execution
	// was interrupted. The frames it did see are still reported, so a trace can show what
	// was thrown away, but the controller must not act on them.
	const auto frames = UniformWindow(12, kFrame60Hz * 2);
	EECycleRateSampler::WindowInputs in = Inputs(frames);
	in.invalidated = true;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(in);

	EXPECT_FALSE(s.valid);
	EXPECT_FALSE(EECycleRateController::IsSampleUsable(s));
	EXPECT_EQ(s.frames, 12u);
	EXPECT_NEAR(s.p90_active_ratio, 2.0f, 1e-4f);
}

TEST(EECycleRateSamplerWindow, AnEmptyWindowIsInvalid)
{
	EECycleRateSampler::WindowInputs in;
	in.tick_frequency = kTickFrequency;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(in);

	EXPECT_FALSE(s.valid);
	EXPECT_EQ(s.frames, 0u);
	EXPECT_FALSE(EECycleRateController::IsSampleUsable(s));
}

TEST(EECycleRateSamplerWindow, AFrameWithNoBudgetInvalidatesTheWholeWindow)
{
	// target_ticks == 0 means the limiter had no deadline for that frame. Dividing by it
	// would produce an infinity that IsSampleUsable would have to catch; refusing the
	// window here is the same answer, arrived at where the fact is known.
	auto frames = UniformWindow(30, kFrame60Hz / 2);
	frames[7].target_ticks = 0;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_FALSE(s.valid);
}

TEST(EECycleRateSamplerWindow, AWindowLongerThanTheArrayIsRefused)
{
	// The sampler closes a window early rather than dropping frames, so this cannot happen
	// from the frontend. It can happen from a caller that got the count wrong, and a
	// silently truncated percentile is worse than no answer.
	const auto frames = UniformWindow(4, kFrame60Hz / 2);
	EECycleRateSampler::WindowInputs in = Inputs(frames);
	in.frame_count = EECycleRateSampler::kMaxWindowFrames + 1;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(in);

	EXPECT_FALSE(s.valid);
}

TEST(EECycleRateSamplerWindow, AFullWindowIsUsableEndToEnd)
{
	// The whole point of the array: what the sampler builds is what the controller accepts.
	const auto frames = UniformWindow(EECycleRateSampler::kMaxWindowFrames, kFrame60Hz / 2);
	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));

	EXPECT_TRUE(s.valid);
	EXPECT_TRUE(EECycleRateController::IsSampleUsable(s));
	EXPECT_EQ(s.frames, EECycleRateSampler::kMaxWindowFrames);
}

TEST(EECycleRateSamplerWindow, AnOverloadedWindowOpensAllFourDownshiftGates)
{
	// The end-to-end shape the governor is looking for: over the deadline, frames late, the
	// EE thread's own time is the cost, nothing blocked. Built from ticks rather than from
	// hand-written ratios, so the arithmetic and the policy are checked against each other.
	auto frames = UniformWindow(30, (kFrame60Hz * 6) / 5, kFrame60Hz / 20);
	for (auto& f : frames)
		f.late = true;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));
	const EECycleRatePolicy policy;

	EXPECT_GE(s.p90_active_ratio, policy.overload_p90);
	EXPECT_GE(s.late_ratio, policy.overload_late);
	EXPECT_GE(s.own_time_ratio, policy.min_own_time);
	EXPECT_LE(s.blocked_ratio, policy.max_blocked);
}

TEST(EECycleRateSamplerWindow, AGSBoundWindowFailsTheBlockedGateEvenThoughItIsLate)
{
	// Same wall-clock pressure, same missed deadlines, but the time went to the GS thread.
	// Two of the four gates have to close, or the governor answers a bottleneck it cannot
	// reach.
	auto frames = UniformWindow(30, (kFrame60Hz * 6) / 5, kFrame60Hz);
	for (auto& f : frames)
		f.late = true;

	const EECycleRateWindowSample s = EECycleRateSampler::BuildWindowSample(Inputs(frames));
	const EECycleRatePolicy policy;

	EXPECT_GE(s.p90_active_ratio, policy.overload_p90);
	EXPECT_GE(s.late_ratio, policy.overload_late);
	EXPECT_LT(s.own_time_ratio, policy.min_own_time);
	EXPECT_GT(s.blocked_ratio, policy.max_blocked);
}

// --- two segments per frame ----------------------------------------------------
//
// The limiter waits at vsync start and again at vsync end, so a frame's work reaches the
// sampler as two segments. These pin how they fold into one frame: the sum is the frame's
// active time, only the vsync-end segment closes it, and a frame that lost a segment is
// unmeasurable rather than short.

TEST(EECycleRateFrameAccumulator, TheTwoSegmentsAddAndOnlyTheLastClosesTheFrame)
{
	EECycleRateFrameAccumulator acc;
	using Outcome = EECycleRateFrameAccumulator::Outcome;

	EXPECT_EQ(acc.AddSegment(10'000'000, false, Lateness::OnTime), Outcome::Pending);
	EXPECT_EQ(acc.active_ticks, 10'000'000u);

	EXPECT_EQ(acc.AddSegment(3'000'000, true, Lateness::OnTime), Outcome::Closed);
	EXPECT_EQ(acc.active_ticks, 13'000'000u);
	EXPECT_FALSE(acc.late);
	EXPECT_FALSE(acc.overran);
}

TEST(EECycleRateFrameAccumulator, AMissingFirstSegmentMakesTheFrameUnmeasurable)
{
	// A pause or an unpaced frame zeroes the work period, so the vsync-start wait finds none.
	EECycleRateFrameAccumulator acc;
	using Outcome = EECycleRateFrameAccumulator::Outcome;

	EXPECT_EQ(acc.AddSegment(0, false, Lateness::OnTime), Outcome::Pending);
	EXPECT_EQ(acc.AddSegment(3'000'000, true, Lateness::OnTime), Outcome::Unmeasured)
		<< "the second segment alone is not the frame; reading it as one would report a short frame";
}

TEST(EECycleRateFrameAccumulator, AMissingSecondSegmentMakesTheFrameUnmeasurable)
{
	EECycleRateFrameAccumulator acc;
	using Outcome = EECycleRateFrameAccumulator::Outcome;

	EXPECT_EQ(acc.AddSegment(10'000'000, false, Lateness::OnTime), Outcome::Pending);
	EXPECT_EQ(acc.AddSegment(0, true, Lateness::OnTime), Outcome::Unmeasured);
}

TEST(EECycleRateFrameAccumulator, ALateVerdictFromEitherWaitSticks)
{
	using Outcome = EECycleRateFrameAccumulator::Outcome;

	// The vsync-start wait reports a verdict only when the limiter had to move the frame start,
	// which would otherwise hide the lateness from the vsync-end verdict.
	EECycleRateFrameAccumulator first;
	first.AddSegment(10'000'000, false, Lateness::Overran);
	EXPECT_EQ(first.AddSegment(3'000'000, true, Lateness::OnTime), Outcome::Closed);
	EXPECT_TRUE(first.late);

	EECycleRateFrameAccumulator second;
	second.AddSegment(10'000'000, false, Lateness::OnTime);
	EXPECT_EQ(second.AddSegment(3'000'000, true, Lateness::Late), Outcome::Closed);
	EXPECT_TRUE(second.late);
}

// Late is "ended after its deadline" and is the governor's gate; Overran is "a whole frame or more
// after it" and is what auto-tune counts as a dropped frame. They are one measurement at two
// thresholds, so an overran frame is always a late one and never the other way round.
TEST(EECycleRateFrameAccumulator, OverranIsLateAtTheHigherThreshold)
{
	using Outcome = EECycleRateFrameAccumulator::Outcome;

	EECycleRateFrameAccumulator late;
	late.AddSegment(10'000'000, false, Lateness::OnTime);
	EXPECT_EQ(late.AddSegment(3'000'000, true, Lateness::Late), Outcome::Closed);
	EXPECT_TRUE(late.late);
	EXPECT_FALSE(late.overran) << "a few milliseconds past the deadline is late, not a dropped frame";

	EECycleRateFrameAccumulator overran;
	overran.AddSegment(10'000'000, false, Lateness::OnTime);
	EXPECT_EQ(overran.AddSegment(3'000'000, true, Lateness::Overran), Outcome::Closed);
	EXPECT_TRUE(overran.late);
	EXPECT_TRUE(overran.overran);

	EECycleRateFrameAccumulator start_only;
	start_only.AddSegment(10'000'000, false, Lateness::Overran);
	EXPECT_EQ(start_only.AddSegment(3'000'000, true, Lateness::OnTime), Outcome::Closed);
	EXPECT_TRUE(start_only.overran) << "the vsync-start wait's whole-frame overrun sticks too";
}

TEST(EECycleRateFrameAccumulator, ClearStartsTheNextFrameFromNothing)
{
	EECycleRateFrameAccumulator acc;
	using Outcome = EECycleRateFrameAccumulator::Outcome;

	acc.AddSegment(0, false, Lateness::Overran);
	acc.AddSegment(1'000'000, true, Lateness::OnTime);
	acc.Clear();

	EXPECT_EQ(acc.AddSegment(4'000'000, false, Lateness::OnTime), Outcome::Pending);
	EXPECT_EQ(acc.AddSegment(5'000'000, true, Lateness::OnTime), Outcome::Closed);
	EXPECT_EQ(acc.active_ticks, 9'000'000u);
	EXPECT_FALSE(acc.late);
	EXPECT_FALSE(acc.overran);
}

// --- the tally other consumers read --------------------------------------------------------
//
// Auto-tune reads running totals rather than the governor's windows, so what it sees does not
// depend on whether the governor is running or on where the governor's windows fall. These pin
// what goes into the totals.

TEST(EECycleRateTally, EveryClosedFrameCountsWhetherOrNotItWasMeasurable)
{
	EECycleRateTally tally;
	EECycleRateFrameAccumulator acc;

	acc.AddSegment(10'000'000, false, Lateness::OnTime);
	ASSERT_EQ(acc.AddSegment(3'000'000, true, Lateness::Overran), EECycleRateFrameAccumulator::Outcome::Closed);
	tally.NoteFrame(acc);
	acc.Clear();

	// A frame with a missing segment (the first one after a pause) has no known length but did
	// end on the limiter, and is still one of the frames the speed is made of.
	acc.AddSegment(0, false, Lateness::OnTime);
	ASSERT_EQ(acc.AddSegment(3'000'000, true, Lateness::OnTime), EECycleRateFrameAccumulator::Outcome::Unmeasured);
	tally.NoteFrame(acc);

	EXPECT_EQ(tally.frames, 2u);
	EXPECT_EQ(tally.overran_frames, 1u);
}

TEST(EECycleRateTally, AWaitIsTheTimeBetweenItsTwoReadings)
{
	EECycleRateTally tally;
	tally.NoteGSWait(1'000, 4'000);
	tally.NoteGSWait(10'000, 10'500);
	tally.NoteVU1Wait(2'000, 2'250);
	EXPECT_EQ(tally.gs_wait_ticks, 3'500u);
	EXPECT_EQ(tally.vu1_wait_ticks, 250u);
}

TEST(EECycleRateTally, AClockThatDidNotAdvanceAddsNothing)
{
	EECycleRateTally tally;
	tally.NoteGSWait(5'000, 5'000);
	tally.NoteGSWait(5'000, 4'000);
	tally.NoteVU1Wait(5'000, 4'999);
	EXPECT_EQ(tally.gs_wait_ticks, 0u);
	EXPECT_EQ(tally.vu1_wait_ticks, 0u);
}

// --- overlay word ----------------------------------------------------------
//
// The one piece of governor state that crosses a thread boundary. It is a single word so the
// display never pairs a configured selector from one window with an effective selector from
// the next. That holds only if the round trip is exact over the whole selector range,
// including the negative half, where an off-by-one bias or a sign extension would show up.

TEST(EECycleRateSamplerOverlay, EverySelectorPairRoundTrips)
{
	using OverlayState = EECycleRateSampler::OverlayState;

	for (int configured = Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE;
		 configured <= Pcsx2Config::SpeedhackOptions::MAX_EE_CYCLE_RATE; configured++)
	{
		for (int effective = Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE;
			 effective <= Pcsx2Config::SpeedhackOptions::MAX_EE_CYCLE_RATE; effective++)
		{
			OverlayState in;
			in.configured = static_cast<s8>(configured);
			in.effective = static_cast<s8>(effective);

			const OverlayState out = EECycleRateSampler::UnpackOverlayWord(EECycleRateSampler::PackOverlayWord(in));
			EXPECT_EQ(out.configured, in.configured) << configured << "," << effective;
			EXPECT_EQ(out.effective, in.effective) << configured << "," << effective;
		}
	}
}

TEST(EECycleRateSamplerOverlay, StateAndFlagsRoundTrip)
{
	using OverlayState = EECycleRateSampler::OverlayState;
	using State = EECycleRateController::State;

	for (const State state : {State::Disabled, State::Warmup, State::Observe, State::Cooldown})
	{
		for (const bool enabled : {false, true})
		{
			for (const bool shadow : {false, true})
			{
				OverlayState in;
				in.configured = -1;
				in.effective = -3;
				in.state = state;
				in.enabled = enabled;
				in.shadow = shadow;

				const OverlayState out = EECycleRateSampler::UnpackOverlayWord(EECycleRateSampler::PackOverlayWord(in));
				EXPECT_EQ(out.state, in.state);
				EXPECT_EQ(out.enabled, in.enabled);
				EXPECT_EQ(out.shadow, in.shadow);
				EXPECT_EQ(out.configured, in.configured);
				EXPECT_EQ(out.effective, in.effective);
			}
		}
	}
}

TEST(EECycleRateSamplerOverlay, TheZeroWordMeansTheGovernorIsNotRunning)
{
	// The published word before any VM has started, and what the display falls back on.
	const EECycleRateSampler::OverlayState out = EECycleRateSampler::UnpackOverlayWord(0);

	EXPECT_FALSE(out.enabled);
	EXPECT_FALSE(out.shadow);
	EXPECT_EQ(out.state, EECycleRateController::State::Disabled);
	EXPECT_EQ(out.configured, Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE);
	EXPECT_EQ(out.effective, Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE);
}
