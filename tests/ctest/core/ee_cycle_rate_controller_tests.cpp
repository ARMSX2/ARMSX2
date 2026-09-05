// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Deterministic sequence coverage for the dynamic EE cycle-rate governor's decision
// half. No VM, no clock, no settings: every test is a fixed list of window samples
// fed to EECycleRateController, and the assertions are the decisions it returns.
//
// This is where the feature's real risk lives. Whether the governor oscillates,
// walks to the floor against a bottleneck it cannot move, or reacts to one late
// frame is decided entirely by these rules, and none of it can be settled from a
// paced device run - a device run tells you the answer was wrong, not which rule
// produced it.
//
// The window is 500 ms of target time throughout, so the default two-second
// warm-up and cooldown are four windows each and the ten-second downshift inhibit
// is twenty. The tests spell those out rather than computing them, because a policy
// change that silently halves a dwell should fail here.

#include "EECycleRateController.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	using Decision = EECycleRateDecision;
	using State = EECycleRateController::State;

	constexpr double kWindow = 0.5;

	constexpr int kWarmupWindows = 4;
	constexpr int kCooldownWindows = 4;
	// The downshift inhibit an ineffective step starts. It drains through Observe
	// and Cooldown alike, so the four cooldown windows after the undo are the first
	// four of these twenty, not extra ones.
	constexpr int kInhibitWindows = 20;

	EECycleRateWindowSample MakeWindow(float p90, float late, float own, float cpu, float blocked)
	{
		EECycleRateWindowSample s;
		s.valid = true;
		s.frames = 30;
		s.target_seconds = kWindow;
		s.p90_active_ratio = p90;
		s.late_ratio = late;
		s.own_time_ratio = own;
		s.cpu_time_ratio = cpu;
		s.blocked_ratio = blocked;
		return s;
	}

	// Over the deadline, plenty of late frames, the EE thread itself is the cost,
	// nothing blocked on GS or VU1. All four downshift gates open.
	EECycleRateWindowSample OverloadWindow(float p90 = 1.20f)
	{
		return MakeWindow(p90, 0.40f, 0.95f, 1.00f, 0.05f);
	}

	// Comfortably inside the budget with no late frame at all.
	EECycleRateWindowSample HeadroomWindow(float p90 = 0.60f)
	{
		return MakeWindow(p90, 0.00f, 0.50f, 0.55f, 0.05f);
	}

	// Between the two thresholds: neither classification fires.
	EECycleRateWindowSample DeadbandWindow()
	{
		return MakeWindow(0.85f, 0.10f, 0.80f, 0.85f, 0.05f);
	}

	EECycleRateEligibility Eligible(s8 baseline = 0)
	{
		EECycleRateEligibility e;
		e.enabled = true;
		e.eligible = true;
		e.baseline = baseline;
		return e;
	}

	EECycleRateEligibility Suspended(s8 baseline = 0)
	{
		EECycleRateEligibility e;
		e.enabled = true;
		e.eligible = false;
		e.baseline = baseline;
		return e;
	}

	EECycleRateEligibility Off(s8 baseline = 0)
	{
		EECycleRateEligibility e;
		e.enabled = false;
		e.eligible = true;
		e.baseline = baseline;
		return e;
	}

	void FeedQuiet(EECycleRateController& c, const EECycleRateWindowSample& sample, const EECycleRateEligibility& e, int count)
	{
		for (int i = 0; i < count; i++)
			ASSERT_EQ(c.Update(sample, e), Decision::None) << "window " << i << " of " << count;
	}

	// Runs the warm-up out on neutral windows and leaves the controller observing.
	void WarmUp(EECycleRateController& c, const EECycleRateEligibility& e)
	{
		FeedQuiet(c, DeadbandWindow(), e, kWarmupWindows);
		ASSERT_EQ(c.GetState(), State::Observe);
	}

	// Warm-up plus the two overloaded windows that take one step down.
	void StepDownOnce(EECycleRateController& c, const EECycleRateEligibility& e, float p90 = 1.20f)
	{
		WarmUp(c, e);
		ASSERT_EQ(c.Update(OverloadWindow(p90), e), Decision::None);
		ASSERT_EQ(c.Update(OverloadWindow(p90), e), Decision::StepDown);
	}
} // namespace

TEST(EECycleRateController, StartsDisabledAndBelievesTheBaseline)
{
	EECycleRateController c;
	EXPECT_EQ(c.GetState(), State::Disabled);
	EXPECT_EQ(c.GetEffective(), 0);

	c.Reset(-1);
	EXPECT_EQ(c.GetState(), State::Warmup);
	EXPECT_EQ(c.GetBaseline(), -1);
	EXPECT_EQ(c.GetEffective(), -1);
	EXPECT_EQ(c.GetFloor(), -3);
}

TEST(EECycleRateController, FloorIsTwoStepsDownAndNeverPastMinusThree)
{
	EECycleRateController c;
	c.Reset(3);
	EXPECT_EQ(c.GetFloor(), 1);
	c.Reset(0);
	EXPECT_EQ(c.GetFloor(), -2);
	c.Reset(-2);
	EXPECT_EQ(c.GetFloor(), -3);
	c.Reset(-3);
	EXPECT_EQ(c.GetFloor(), -3);
}

TEST(EECycleRateControllerDownshift, TwoQualifyingOverloadWindowsLowerOneStep)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::None);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 1u);
	EXPECT_EQ(c.GetEffective(), 0);

	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::StepDown);
	EXPECT_EQ(c.GetEffective(), -1);
	EXPECT_EQ(c.GetState(), State::Cooldown);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 0u);
	EXPECT_EQ(c.GetSnapshot().transitions, 1u);
}

TEST(EECycleRateControllerDownshift, OneOverloadWindowFollowedByACleanOneDoesNotLower)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::None);
	EXPECT_EQ(c.Update(HeadroomWindow(), e), Decision::None);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 0u);

	// The run has to start over, so the next overloaded window is only the first.
	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::None);
	EXPECT_EQ(c.GetEffective(), 0);
}

TEST(EECycleRateControllerDownshift, WallPressureWithoutOwnTimeDoesNotLower)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	// Late every window, but the EE thread is not where the time went, so slowing
	// the guest down cannot buy any of it back.
	const auto sample = MakeWindow(1.30f, 0.45f, 0.40f, 0.95f, 0.10f);
	FeedQuiet(c, sample, e, 8);
	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 0u);
}

TEST(EECycleRateControllerDownshift, WallAndCpuPressureWithHighBlockedRatioDoesNotLower)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	// 1.30 active over target with 35% of it blocked on GS or VU1 still leaves 0.845
	// of own time, so only the blocked ceiling rejects this one. A VU1 bottleneck is
	// not answered by underclocking the EE.
	const auto sample = MakeWindow(1.30f, 0.45f, 0.845f, 1.05f, 0.35f);
	FeedQuiet(c, sample, e, 8);
	EXPECT_EQ(c.GetEffective(), 0);
}

TEST(EECycleRateControllerDownshift, RepeatedOverloadStopsAtTheFloor)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	// Each step measurably helps, so the effectiveness check passes and the governor
	// is allowed to keep going - until the two-step floor stops it.
	EXPECT_EQ(c.Update(OverloadWindow(1.40f), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(1.40f), e), Decision::StepDown);
	EXPECT_EQ(c.GetEffective(), -1);
	FeedQuiet(c, DeadbandWindow(), e, kCooldownWindows);

	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::StepDown);
	EXPECT_EQ(c.GetEffective(), -2);
	EXPECT_EQ(c.GetEffective(), c.GetFloor());
	FeedQuiet(c, DeadbandWindow(), e, kCooldownWindows);

	FeedQuiet(c, OverloadWindow(1.05f), e, 6);
	EXPECT_EQ(c.GetEffective(), -2);
	EXPECT_EQ(c.GetSnapshot().transitions, 2u);
}

TEST(EECycleRateControllerUpshift, FourHeadroomWindowsRestoreOneStep)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	StepDownOnce(c, e);
	c.OnApplied(-1);
	FeedQuiet(c, DeadbandWindow(), e, kCooldownWindows);

	FeedQuiet(c, HeadroomWindow(), e, 3);
	EXPECT_EQ(c.GetSnapshot().headroom_dwell, 3u);
	EXPECT_EQ(c.Update(HeadroomWindow(), e), Decision::StepUp);
	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetState(), State::Cooldown);
}

TEST(EECycleRateControllerUpshift, HeadroomNeverRaisesAboveTheBaseline)
{
	const auto e = Eligible(-1);
	EECycleRateController c;
	c.Reset(-1);
	WarmUp(c, e);

	FeedQuiet(c, HeadroomWindow(), e, 20);
	EXPECT_EQ(c.GetEffective(), -1);
	EXPECT_EQ(c.GetSnapshot().transitions, 0u);
}

TEST(EECycleRateControllerDeadband, AlternatingSamplesAroundTheThresholdsDoNotOscillate)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	// One tick under the overload threshold, one tick over the headroom threshold.
	// Both are deadband, so the counters decay instead of accumulating.
	const auto near_overload = MakeWindow(0.94f, 0.40f, 0.95f, 1.00f, 0.05f);
	const auto near_headroom = MakeWindow(0.76f, 0.00f, 0.50f, 0.55f, 0.05f);
	for (int i = 0; i < 20; i++)
	{
		ASSERT_EQ(c.Update(near_overload, e), Decision::None) << "pair " << i;
		ASSERT_EQ(c.Update(near_headroom, e), Decision::None) << "pair " << i;
	}

	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetSnapshot().transitions, 0u);
}

TEST(EECycleRateControllerDeadband, AlternatingOverloadAndHeadroomDoesNotTransition)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	// The classic oscillation driver: each classification clears the other's run, so
	// neither dwell ever reaches its threshold.
	for (int i = 0; i < 20; i++)
	{
		ASSERT_EQ(c.Update(OverloadWindow(), e), Decision::None) << "pair " << i;
		ASSERT_EQ(c.Update(HeadroomWindow(), e), Decision::None) << "pair " << i;
	}

	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetSnapshot().transitions, 0u);
}

TEST(EECycleRateControllerTimers, NothingTransitionsDuringWarmup)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);

	// Boot, state load and JIT warm-up all look overloaded. Four windows of target
	// time have to pass before any of it counts.
	FeedQuiet(c, OverloadWindow(), e, kWarmupWindows);
	EXPECT_EQ(c.GetState(), State::Observe);
	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 0u);

	// And the warm-up windows did not count toward the dwell either.
	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::StepDown);
}

TEST(EECycleRateControllerTimers, NothingTransitionsDuringCooldown)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	StepDownOnce(c, e);
	c.OnApplied(-1);

	// The cache rebuild the step just cost would classify as overload on its own.
	FeedQuiet(c, OverloadWindow(), e, kCooldownWindows - 1);
	EXPECT_EQ(c.GetState(), State::Cooldown);
	EXPECT_EQ(c.GetEffective(), -1);

	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::None);
	EXPECT_EQ(c.GetState(), State::Observe);
	EXPECT_EQ(c.GetEffective(), -1);
}

TEST(EECycleRateControllerEffectiveness, AnIneffectiveStepIsGivenBackAndInhibitsAnother)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	StepDownOnce(c, e, 1.20f);
	c.OnApplied(-1);
	FeedQuiet(c, DeadbandWindow(), e, kCooldownWindows);

	// p90 has not moved, so the step bought nothing: something other than the EE
	// selector is the bottleneck. One step down is one step to give back, which
	// from baseline - 1 lands on the baseline.
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::StepUp);
	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetState(), State::Cooldown);
	EXPECT_EQ(c.GetSnapshot().ineffective_count, 1u);
	EXPECT_DOUBLE_EQ(c.GetSnapshot().downshift_inhibit_seconds, 10.0);
	c.OnApplied(0);

	// The undo's own cooldown drains the inhibit rather than extending it, so the
	// whole thing is twenty windows and not twenty-four.
	FeedQuiet(c, OverloadWindow(1.20f), e, kCooldownWindows);
	EXPECT_EQ(c.GetState(), State::Observe);
	FeedQuiet(c, OverloadWindow(1.20f), e, kInhibitWindows - kCooldownWindows);
	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 0u);
	EXPECT_DOUBLE_EQ(c.GetSnapshot().downshift_inhibit_seconds, 0.0);

	// Once it is spent the governor is allowed to try again.
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::StepDown);
	EXPECT_EQ(c.GetEffective(), -1);
}

TEST(EECycleRateControllerEffectiveness, AnIneffectiveSecondStepKeepsTheEffectiveFirstOne)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	// Step one: 1.40 down to 1.20 clears the five percent margin, so it stands.
	EXPECT_EQ(c.Update(OverloadWindow(1.40f), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(1.40f), e), Decision::StepDown);
	c.OnApplied(-1);
	FeedQuiet(c, DeadbandWindow(), e, kCooldownWindows);
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::StepDown);
	EXPECT_EQ(c.GetEffective(), -2);
	c.OnApplied(-2);
	FeedQuiet(c, DeadbandWindow(), e, kCooldownWindows);

	// Step two bought nothing. Only step two goes back: step one was judged on its
	// own two windows and passed, and giving it back would throw away time we have
	// already measured.
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::StepUp);
	EXPECT_EQ(c.GetEffective(), -1);
	EXPECT_EQ(c.GetSnapshot().ineffective_count, 1u);
	c.OnApplied(-1);

	// Eight inhibited windows of overload, and it stays where it is.
	FeedQuiet(c, OverloadWindow(1.20f), e, kCooldownWindows);
	FeedQuiet(c, OverloadWindow(1.20f), e, 4);
	EXPECT_EQ(c.GetEffective(), -1);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 0u);

	// Headroom is not inhibited, so the way back to the baseline stays open.
	FeedQuiet(c, HeadroomWindow(), e, 3);
	EXPECT_EQ(c.Update(HeadroomWindow(), e), Decision::StepUp);
	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_GT(c.GetSnapshot().downshift_inhibit_seconds, 0.0);
	c.OnApplied(0);

	// That upshift's cooldown does not extend the inhibit either: eight more
	// windows and the original ten seconds are spent.
	FeedQuiet(c, OverloadWindow(1.20f), e, kCooldownWindows);
	FeedQuiet(c, OverloadWindow(1.20f), e, 4);
	EXPECT_DOUBLE_EQ(c.GetSnapshot().downshift_inhibit_seconds, 0.0);
	EXPECT_EQ(c.GetEffective(), 0);

	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(1.20f), e), Decision::StepDown);
	EXPECT_EQ(c.GetEffective(), -1);
}

TEST(EECycleRateControllerEffectiveness, AnEffectiveStepStands)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	StepDownOnce(c, e, 1.20f);
	c.OnApplied(-1);
	FeedQuiet(c, DeadbandWindow(), e, kCooldownWindows);

	// 0.85 against a pre-downshift 1.20 clears the 5% margin comfortably, and the
	// window is in the deadband so nothing else fires either.
	FeedQuiet(c, DeadbandWindow(), e, 6);
	EXPECT_EQ(c.GetEffective(), -1);
	EXPECT_EQ(c.GetState(), State::Observe);
	EXPECT_EQ(c.GetSnapshot().ineffective_count, 0u);
	EXPECT_EQ(c.GetSnapshot().transitions, 1u);
}

TEST(EECycleRateControllerEligibility, LosingItRestoresTheBaselineAndClearsHistory)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	StepDownOnce(c, e);
	c.OnApplied(-1);

	EXPECT_EQ(c.Update(DeadbandWindow(), Suspended()), Decision::RestoreBaseline);
	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetState(), State::Disabled);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 0u);
	EXPECT_EQ(c.GetSnapshot().headroom_dwell, 0u);
	c.OnApplied(0);

	// Already at the baseline: nothing more to ask for.
	EXPECT_EQ(c.Update(DeadbandWindow(), Suspended()), Decision::None);

	// Coming back starts a fresh warm-up, so the pre-suspension evidence is gone.
	FeedQuiet(c, OverloadWindow(), e, kWarmupWindows);
	EXPECT_EQ(c.GetEffective(), 0);
}

TEST(EECycleRateControllerEligibility, TheDynamicSettingGoingOffBehavesTheSameWay)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	StepDownOnce(c, e);
	c.OnApplied(-1);

	EXPECT_EQ(c.Update(DeadbandWindow(), Off()), Decision::RestoreBaseline);
	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetState(), State::Disabled);
}

TEST(EECycleRateControllerEligibility, LosingItCancelsAnUnappliedLowerRequest)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	StepDownOnce(c, e);

	// The integration never got to a safe boundary, so OnApplied never came. The
	// restore is still the right request: whatever is applied, the baseline is what
	// has to be running now.
	EXPECT_EQ(c.Update(DeadbandWindow(), Suspended()), Decision::RestoreBaseline);
	EXPECT_EQ(c.GetEffective(), 0);
}

TEST(EECycleRateControllerEligibility, LosingItAtTheBaselineJustDisables)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	EXPECT_EQ(c.Update(DeadbandWindow(), Suspended()), Decision::None);
	EXPECT_EQ(c.GetState(), State::Disabled);
	EXPECT_EQ(c.GetSnapshot().transitions, 0u);
}

TEST(EECycleRateControllerEligibility, ABaselineChangeRestoresAndStartsOver)
{
	EECycleRateController c;
	c.Reset(0);
	StepDownOnce(c, Eligible());
	c.OnApplied(-1);

	// A settings apply moved the configured rate. The old effective selector was a
	// decision about a baseline that no longer exists.
	EXPECT_EQ(c.Update(DeadbandWindow(), Eligible(1)), Decision::RestoreBaseline);
	EXPECT_EQ(c.GetBaseline(), 1);
	EXPECT_EQ(c.GetEffective(), 1);
	EXPECT_EQ(c.GetFloor(), -1);
	EXPECT_EQ(c.GetState(), State::Warmup);
}

TEST(EECycleRateControllerValidity, UnusableWindowsCannotTransition)
{
	std::vector<EECycleRateWindowSample> bad;

	auto flagged = OverloadWindow();
	flagged.valid = false;
	bad.push_back(flagged);

	auto no_frames = OverloadWindow();
	no_frames.frames = 0;
	bad.push_back(no_frames);

	auto zero_target = OverloadWindow();
	zero_target.target_seconds = 0.0;
	bad.push_back(zero_target);

	auto negative_target = OverloadWindow();
	negative_target.target_seconds = -kWindow;
	bad.push_back(negative_target);

	auto nan_p90 = OverloadWindow();
	nan_p90.p90_active_ratio = std::numeric_limits<float>::quiet_NaN();
	bad.push_back(nan_p90);

	auto inf_own = OverloadWindow();
	inf_own.own_time_ratio = std::numeric_limits<float>::infinity();
	bad.push_back(inf_own);

	auto negative_blocked = OverloadWindow();
	negative_blocked.blocked_ratio = -0.5f;
	bad.push_back(negative_blocked);

	auto wrapped_cpu = OverloadWindow();
	wrapped_cpu.cpu_time_ratio = 1.0e9f;
	bad.push_back(wrapped_cpu);

	auto nan_target = OverloadWindow();
	nan_target.target_seconds = std::numeric_limits<double>::quiet_NaN();
	bad.push_back(nan_target);

	const auto e = Eligible();
	for (size_t i = 0; i < bad.size(); i++)
	{
		EXPECT_FALSE(EECycleRateController::IsSampleUsable(bad[i])) << "sample " << i;

		EECycleRateController c;
		c.Reset(0);
		WarmUp(c, e);
		FeedQuiet(c, bad[i], e, 8);
		EXPECT_EQ(c.GetEffective(), 0) << "sample " << i;
		EXPECT_EQ(c.GetSnapshot().transitions, 0u) << "sample " << i;
	}
}

TEST(EECycleRateControllerValidity, AnUnusableWindowBreaksADwellRun)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	auto gap = OverloadWindow();
	gap.valid = false;

	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::None);
	EXPECT_EQ(c.Update(gap, e), Decision::None);
	EXPECT_EQ(c.GetSnapshot().overload_dwell, 0u);
	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::None);
	EXPECT_EQ(c.GetEffective(), 0);
}

TEST(EECycleRateControllerValidity, AnUnusableWindowDoesNotAdvanceTheWarmupClock)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);

	auto gap = DeadbandWindow();
	gap.valid = false;
	FeedQuiet(c, gap, e, kWarmupWindows * 3);
	EXPECT_EQ(c.GetState(), State::Warmup);
	EXPECT_EQ(c.GetSnapshot().clock_seconds, 0.0);

	FeedQuiet(c, DeadbandWindow(), e, kWarmupWindows);
	EXPECT_EQ(c.GetState(), State::Observe);
}

TEST(EECycleRateControllerSnapshot, ReportsWhatTheTraceNeeds)
{
	const auto e = Eligible();
	EECycleRateController c;
	c.Reset(0);
	WarmUp(c, e);

	c.Update(OverloadWindow(1.35f), e);
	const auto s = c.GetSnapshot();
	EXPECT_EQ(s.state, State::Observe);
	EXPECT_EQ(s.baseline, 0);
	EXPECT_EQ(s.effective, 0);
	EXPECT_EQ(s.overload_dwell, 1u);
	EXPECT_EQ(s.headroom_dwell, 0u);
	EXPECT_FLOAT_EQ(s.last_p90, 1.35f);
	EXPECT_EQ(s.transitions, 0u);
	EXPECT_EQ(s.ineffective_count, 0u);
	EXPECT_DOUBLE_EQ(s.downshift_inhibit_seconds, 0.0);
}

TEST(EECycleRateControllerPolicy, ATestPolicyDrivesTheDwellAndTheFloor)
{
	EECycleRatePolicy policy;
	policy.warmup_seconds = 0.0;
	policy.cooldown_seconds = 0.0;
	policy.downshift_dwell = 1;
	policy.max_steps_down = 1;
	policy.effectiveness_windows = 0;

	const auto e = Eligible();
	EECycleRateController c(policy);
	c.Reset(0);
	EXPECT_EQ(c.GetFloor(), -1);

	// A zero warm-up still costs the one window that expires it.
	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::None);
	EXPECT_EQ(c.Update(OverloadWindow(), e), Decision::StepDown);
	EXPECT_EQ(c.GetEffective(), -1);

	// One step is the whole range for this policy.
	FeedQuiet(c, OverloadWindow(), e, 8);
	EXPECT_EQ(c.GetEffective(), -1);
}

// --- CSV replay ------------------------------------------------------------
//
// The fixture is embedded rather than shipped as a file under a data directory,
// because nothing else in tests/ctest locates a file from the source tree - there is
// no CMake-defined data path and no convention to follow. The replay helper still
// takes a path, so a captured trace from a device run can be handed to it directly;
// the test just writes its own copy to a temp file first.

namespace
{
	const char* const kReplayCsv =
		"# valid,frames,target_seconds,p90,late,own,cpu,blocked,enabled,eligible,baseline,expected_decision\n"
		"# Warm-up: four windows of target time before anything is classified.\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"# Two overloaded windows: late, EE-owned, not blocked. One step down.\n"
		"1,30,0.5,1.20,0.40,0.95,1.00,0.05,1,1,0,none\n"
		"1,30,0.5,1.20,0.40,0.95,1.00,0.05,1,1,0,stepdown\n"
		"# Cooldown covers the EE and microVU0 rebuild the step just cost.\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"# The step worked, and the scene got lighter: four headroom windows go back up.\n"
		"1,30,0.5,0.60,0.00,0.50,0.55,0.05,1,1,0,none\n"
		"1,30,0.5,0.60,0.00,0.50,0.55,0.05,1,1,0,none\n"
		"1,30,0.5,0.60,0.00,0.50,0.55,0.05,1,1,0,none\n"
		"1,30,0.5,0.60,0.00,0.50,0.55,0.05,1,1,0,stepup\n"
		"# Cooldown again.\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n"
		"# Suspended at the baseline is a no-op, and coming back re-warms.\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,0,0,none\n"
		"1,30,0.5,0.85,0.10,0.80,0.85,0.05,1,1,0,none\n";

	Decision ParseDecision(const std::string& text)
	{
		if (text == "none")
			return Decision::None;
		if (text == "stepdown")
			return Decision::StepDown;
		if (text == "stepup")
			return Decision::StepUp;
		if (text == "restorebaseline")
			return Decision::RestoreBaseline;

		ADD_FAILURE() << "unknown expected_decision '" << text << "'";
		return Decision::None;
	}

	std::vector<std::string> SplitFields(const std::string& line)
	{
		std::vector<std::string> fields;
		std::stringstream ss(line);
		std::string field;
		while (std::getline(ss, field, ','))
			fields.push_back(field);
		return fields;
	}

	// Replays a CSV of recorded window samples, checking every decision. Columns are
	// valid,frames,target_seconds,p90,late,own,cpu,blocked,enabled,eligible,baseline,
	// expected_decision. Blank lines and '#' comments are skipped.
	void ReplayCsvFile(const std::string& path, EECycleRateController& controller)
	{
		std::ifstream in(path);
		ASSERT_TRUE(in.is_open()) << path;

		std::string line;
		int row = 0;
		int replayed = 0;
		while (std::getline(in, line))
		{
			row++;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;

			const std::vector<std::string> f = SplitFields(line);
			ASSERT_EQ(f.size(), 12u) << "row " << row << ": " << line;

			EECycleRateWindowSample sample;
			sample.valid = std::stoi(f[0]) != 0;
			sample.frames = static_cast<u32>(std::stoul(f[1]));
			sample.target_seconds = std::stod(f[2]);
			sample.p90_active_ratio = std::stof(f[3]);
			sample.late_ratio = std::stof(f[4]);
			sample.own_time_ratio = std::stof(f[5]);
			sample.cpu_time_ratio = std::stof(f[6]);
			sample.blocked_ratio = std::stof(f[7]);

			EECycleRateEligibility eligibility;
			eligibility.enabled = std::stoi(f[8]) != 0;
			eligibility.eligible = std::stoi(f[9]) != 0;
			eligibility.baseline = static_cast<s8>(std::stoi(f[10]));

			EXPECT_EQ(controller.Update(sample, eligibility), ParseDecision(f[11]))
				<< "row " << row << ": " << line;
			replayed++;
		}

		EXPECT_GT(replayed, 0) << "no rows in " << path;
	}
} // namespace

TEST(EECycleRateControllerReplay, ARecordedWindowSequenceProducesTheRecordedDecisions)
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path() /
	                                   (std::string("armsx2-ee-cycle-rate-windows-") + std::to_string(stamp) + ".csv");

	{
		std::ofstream out(path);
		ASSERT_TRUE(out.is_open()) << path.string();
		out << kReplayCsv;
	}

	EECycleRateController c;
	c.Reset(0);
	ReplayCsvFile(path.string(), c);

	EXPECT_EQ(c.GetEffective(), 0);
	EXPECT_EQ(c.GetSnapshot().transitions, 2u);

	std::filesystem::remove(path);
}
