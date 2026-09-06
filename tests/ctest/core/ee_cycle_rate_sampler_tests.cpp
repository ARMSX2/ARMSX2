// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The measurement half of the dynamic EE cycle-rate governor: what a window's frames add
// up to. No VM, no clock, no limiter - every test is an array of per-frame tick counts and
// the EECycleRateWindowSample they produce.
//
// This is where a sampler bug looks like a working feature. A ratio that means something
// other than what it is named cannot be caught by the controller's sequence tests, because
// the controller believes whatever it is handed; it shows up months later as a governor
// that downshifts against a GS bottleneck it cannot move.

#include "EECycleRateSampler.h"

#include "Config.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
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
	// Twenty-nine unblocked half-budget frames and one fully blocked one.
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

// --- overlay word ----------------------------------------------------------
//
// The one piece of governor state that crosses a thread boundary. It is a single word so
// the display can never pair a configured selector from one window with an effective
// selector from the next, and that only holds if the round trip is exact over the whole
// selector range - including the negative half, which is where a bias that is off by one
// or a sign extension would show up.

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
