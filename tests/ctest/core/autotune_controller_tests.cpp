// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AutoTuneController, by replaying scripted windows. Nothing here touches the emulator: the controller has no
// clock, so a test is a list of windows and the actions that come back.

#include <gtest/gtest.h>

#include "AutoTune/AutoTuneController.h"

#include <vector>

namespace
{
	using Kind = AutoTuneAction::Kind;

	AutoTuneWindow Win(double speed)
	{
		AutoTuneWindow w;
		w.usable = true;
		w.seconds = 0.5;
		w.speed_pct = speed;
		return w;
	}

	// An EE-bound window: the EE thread is saturated, nothing else is.
	AutoTuneWindow EeBound(double speed)
	{
		AutoTuneWindow w = Win(speed);
		w.ee_pct = 98.0;
		w.mtgs_pct = 30.0;
		return w;
	}

	// A GPU-bound window with a GPU timer.
	AutoTuneWindow GpuBound(double speed)
	{
		AutoTuneWindow w = Win(speed);
		w.ee_pct = 55.0;
		w.mtgs_pct = 40.0;
		w.gpu_pct = 97.0;
		w.has_gpu_timer = true;
		return w;
	}

	// The GS thread is the one that is saturated.
	AutoTuneWindow GsCpuBound(double speed)
	{
		AutoTuneWindow w = Win(speed);
		w.ee_pct = 60.0;
		w.mtgs_pct = 96.0;
		w.gpu_pct = 40.0;
		w.has_gpu_timer = true;
		return w;
	}

	class Rig
	{
	public:
		AutoTuneController ctl;
		AutoTuneLeverState state;
		std::vector<AutoTuneAction> log;

		Rig() { state.gs_back_thread_allowed = false; }

		AutoTuneAction Feed(const AutoTuneWindow& w)
		{
			const AutoTuneAction a = ctl.OnWindow(w, state);
			if (a.kind != Kind::None)
				log.push_back(a);
			return a;
		}

		// Feeds `seconds` of the same window; returns the first action that came back, or None.
		AutoTuneAction FeedFor(const AutoTuneWindow& w, double seconds)
		{
			AutoTuneAction first;
			for (double t = 0.0; t < seconds - 1e-9; t += w.seconds)
			{
				const AutoTuneAction a = Feed(w);
				if (first.kind == Kind::None && a.kind != Kind::None)
					first = a;
			}
			return first;
		}

		void WarmUp() { FeedFor(Win(100.0), 10.0); }

		// Four bad windows in a row after the warm-up. Returns what the fourth one produced.
		AutoTuneAction Trigger(const AutoTuneWindow& bad)
		{
			AutoTuneAction last;
			for (int i = 0; i < 4; i++)
				last = Feed(bad);
			return last;
		}

		// Plays `w` until the controller says something, up to a minute. None if it never does.
		AutoTuneAction RunUntilAction(const AutoTuneWindow& w)
		{
			for (int i = 0; i < 120; i++)
			{
				const AutoTuneAction a = Feed(w);
				if (a.kind != Kind::None)
					return a;
			}
			return AutoTuneAction();
		}

		// Warm up, trigger on `bad`, and return the action the trigger produced.
		AutoTuneAction WarmAndTrigger(const AutoTuneWindow& bad)
		{
			WarmUp();
			return Trigger(bad);
		}

		size_t Count(Kind kind) const
		{
			size_t n = 0;
			for (const AutoTuneAction& a : log)
				n += (a.kind == kind);
			return n;
		}
	};
} // namespace

// ---------------------------------------------------------------------------------------------- trigger

TEST(AutoTuneController, NothingHappensDuringWarmUp)
{
	Rig rig;
	// Ten seconds of terrible speed: all of it is warm-up.
	EXPECT_EQ(rig.FeedFor(EeBound(20.0), 10.0).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Watching);
	EXPECT_TRUE(rig.log.empty());
}

TEST(AutoTuneController, TriggersOnlyAfterFourConsecutiveBadWindows)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();

	for (int i = 0; i < 3; i++)
		EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::None);
	// A good window in between starts the count again.
	EXPECT_EQ(rig.Feed(Win(100.0)).kind, Kind::None);
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::None);
	EXPECT_EQ(rig.ctl.BadStreak(), 3u);

	const AutoTuneAction a = rig.Feed(EeBound(70.0));
	EXPECT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::IopThread);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Ee);
}

TEST(AutoTuneController, LateFramesAloneCountAsBad)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();
	AutoTuneWindow w = EeBound(100.0);
	w.late_frame_ratio = 0.10;
	EXPECT_EQ(rig.Trigger(w).kind, Kind::StartTrial);
}

TEST(AutoTuneController, SpeedJustAtTheLineIsNotBad)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();
	EXPECT_EQ(rig.FeedFor(EeBound(95.0), 60.0).kind, Kind::None);
}

TEST(AutoTuneController, UnusableWindowsDoNotCountAndDoNotBreakTheStreak)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;

	// Unusable windows do not advance the warm-up.
	AutoTuneWindow paused = EeBound(0.0);
	paused.usable = false;
	rig.FeedFor(paused, 60.0);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::WarmUp);

	rig.WarmUp();
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::None);
	// A pause in the middle neither completes the streak nor resets it.
	EXPECT_EQ(rig.FeedFor(paused, 30.0).kind, Kind::None);
	EXPECT_EQ(rig.ctl.BadStreak(), 3u);
	EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::StartTrial);
}

TEST(AutoTuneController, UnusableWindowsDoNotRunTheSettleClockOrTheMeasurement)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(70.0)).kind, Kind::StartTrial);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingTrial);

	AutoTuneWindow paused = EeBound(0.0);
	paused.usable = false;
	rig.FeedFor(paused, 60.0);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingTrial);

	// The heavy settle is 6 s of usable play.
	rig.FeedFor(EeBound(70.0), 5.5);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingTrial);
	rig.Feed(EeBound(70.0));
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);

	// Measuring takes four usable windows however many unusable ones come between.
	rig.Feed(EeBound(85.0));
	rig.FeedFor(paused, 10.0);
	rig.Feed(EeBound(85.0));
	rig.Feed(EeBound(85.0));
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);
	EXPECT_EQ(rig.Feed(EeBound(85.0)).kind, Kind::Keep);
}

// ---------------------------------------------------------------------------------------------- EE

TEST(AutoTuneController, EeBoundTriesTheIopThreadAndKeepsItWhenSpeedRises)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.state.ee_governor_eligible = true;

	const AutoTuneAction start = rig.WarmAndTrigger(EeBound(70.0));
	ASSERT_EQ(start.kind, Kind::StartTrial);
	EXPECT_EQ(start.lever, AutoTuneLever::IopThread);
	EXPECT_EQ(start.value, 1.0);
	EXPECT_DOUBLE_EQ(start.speed_before, 70.0);

	// 6 s of settle, then four windows at 82.6: +18%.
	EXPECT_EQ(rig.FeedFor(EeBound(70.0), 6.0).kind, Kind::None);
	const AutoTuneAction keep = rig.RunUntilAction(EeBound(82.6));
	ASSERT_EQ(keep.kind, Kind::Keep);
	EXPECT_EQ(keep.lever, AutoTuneLever::IopThread);
	EXPECT_NEAR(keep.gain, 0.18, 0.001);
	EXPECT_TRUE(keep.measured);

	// The caller turns the thread on; at full speed nothing more happens.
	rig.state.iop_thread_on = true;
	EXPECT_EQ(rig.FeedFor(Win(100.0), 60.0).kind, Kind::None);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 1u);
}

TEST(AutoTuneController, AnIopThreadThatCannotBePlacedIsSkippedForTheGovernor)
{
	Rig rig;
	rig.state.iop_thread_placeable = false;
	rig.state.ee_governor_eligible = true;

	const AutoTuneAction start = rig.WarmAndTrigger(EeBound(70.0));
	ASSERT_EQ(start.kind, Kind::StartTrial);
	EXPECT_EQ(start.lever, AutoTuneLever::EeGovernor);
}

TEST(AutoTuneController, AfterTheIopThreadFailsTheGovernorIsNext)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.state.ee_governor_eligible = true;

	ASSERT_EQ(rig.WarmAndTrigger(EeBound(70.0)).lever, AutoTuneLever::IopThread);
	const AutoTuneAction revert = rig.RunUntilAction(EeBound(70.5));
	ASSERT_EQ(revert.kind, Kind::Revert);
	EXPECT_TRUE(revert.measured);
	EXPECT_EQ(revert.lever, AutoTuneLever::IopThread);

	// Reverted, settled, still slow: the next lever.
	rig.FeedFor(EeBound(70.0), 6.0);
	const AutoTuneAction next = rig.Trigger(EeBound(70.0));
	ASSERT_EQ(next.kind, Kind::StartTrial);
	EXPECT_EQ(next.lever, AutoTuneLever::EeGovernor);
}

// ---------------------------------------------------------------------------------------------- the governor's clocks
//
// The governor counts in game time: two seconds of warm-up, then two windows of overload, then a step, then
// two seconds of cooldown, then two windows for the second step. On a game running at 70% every second of
// that takes 1.4 seconds on the wall, and a trial that measured before the governor had acted would call it
// useless and remember that for good.

TEST(AutoTunePolicy, TheGovernorSettleFollowsTheGovernorsOwnPolicy)
{
	// 2 s warm-up + 2 steps x (2 windows of 0.5 s) + 1 cooldown of 2 s between them + 1 window of slack.
	EXPECT_DOUBLE_EQ(AutoTunePolicy::GovernorSettleGameSeconds(EECycleRatePolicy()), 6.5);
	EXPECT_DOUBLE_EQ(AutoTunePolicy().governor_settle_game_seconds, 6.5);

	EECycleRatePolicy slower;
	slower.warmup_seconds = 4.0;
	slower.cooldown_seconds = 3.0;
	slower.max_steps_down = 1;
	EXPECT_DOUBLE_EQ(AutoTunePolicy::GovernorSettleGameSeconds(slower), 4.0 + 1.0 + 0.0 + 0.5);
}

TEST(AutoTuneController, AGovernorTrialSettlesForTheGovernorsClocksAtTheGamesSpeed)
{
	Rig rig;
	rig.state.ee_governor_eligible = true;

	const AutoTuneAction start = rig.WarmAndTrigger(EeBound(70.0));
	ASSERT_EQ(start.kind, Kind::StartTrial);
	ASSERT_EQ(start.lever, AutoTuneLever::EeGovernor);

	// 6 s for the cache reset the switch causes + 6.5 s of game time at 70% (9.3 s) = 15.3 s.
	EXPECT_EQ(rig.FeedFor(EeBound(70.0), 15.0).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingTrial);
	rig.FeedFor(EeBound(70.0), 1.0);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);
}

TEST(AutoTuneController, ASlowerGameGivesTheGovernorMoreWallTimeButNeverWithoutLimit)
{
	// 40%: 6 + 6.5/0.4 = 22.25 s.
	Rig slow;
	slow.state.ee_governor_eligible = true;
	ASSERT_EQ(slow.WarmAndTrigger(EeBound(40.0)).lever, AutoTuneLever::EeGovernor);
	slow.FeedFor(EeBound(40.0), 22.0);
	EXPECT_EQ(slow.ctl.GetPhase(), AutoTuneController::Phase::SettlingTrial);
	slow.FeedFor(EeBound(40.0), 1.0);
	EXPECT_EQ(slow.ctl.GetPhase(), AutoTuneController::Phase::Measuring);

	// A game at 5% is assumed to run at 20%: 6 + 6.5/0.2 = 38.5 s, not 136 s.
	Rig crawl;
	crawl.state.ee_governor_eligible = true;
	ASSERT_EQ(crawl.WarmAndTrigger(EeBound(5.0)).lever, AutoTuneLever::EeGovernor);
	crawl.FeedFor(EeBound(5.0), 38.0);
	EXPECT_EQ(crawl.ctl.GetPhase(), AutoTuneController::Phase::SettlingTrial);
	crawl.FeedFor(EeBound(5.0), 1.0);
	EXPECT_EQ(crawl.ctl.GetPhase(), AutoTuneController::Phase::Measuring);
}

TEST(AutoTuneController, AGovernorTrialMeasuresLongerThanOtherTrials)
{
	// The governor steps down and back up on its own; a longer look averages over a cycle of that.
	Rig rig;
	rig.state.ee_governor_eligible = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(70.0)).lever, AutoTuneLever::EeGovernor);
	rig.FeedFor(EeBound(70.0), 15.5); // 31 windows: the 15.3 s settle, rounded up to whole windows
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);

	for (int i = 0; i < 7; i++)
		EXPECT_EQ(rig.Feed(EeBound(99.0)).kind, Kind::None) << "window " << i;
	const AutoTuneAction keep = rig.Feed(EeBound(99.0));
	ASSERT_EQ(keep.kind, Kind::Keep);
	EXPECT_EQ(keep.lever, AutoTuneLever::EeGovernor);
}

TEST(AutoTuneController, OtherLeversKeepTheShortSettleAndMeasure)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.state.ee_governor_eligible = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(40.0)).lever, AutoTuneLever::IopThread);

	// The speed does not stretch the IOP thread's 6 s, and it measures four windows.
	rig.FeedFor(EeBound(40.0), 6.0);
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(rig.Feed(EeBound(40.0)).kind, Kind::None);
	EXPECT_EQ(rig.Feed(EeBound(40.0)).kind, Kind::Revert);
}

TEST(AutoTuneController, HardcoreModeSkipsTheGovernor)
{
	Rig rig;
	rig.state.iop_thread_placeable = false;
	rig.state.ee_governor_eligible = true;
	rig.state.hardcore = true;

	const AutoTuneAction a = rig.WarmAndTrigger(EeBound(70.0));
	EXPECT_EQ(a.kind, Kind::Exhausted);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Ee);
}

TEST(AutoTuneController, AGameDatabaseForcedLeverIsNeverTouched)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.state.iop_thread_forced = true;
	rig.state.ee_governor_eligible = true;
	rig.state.ee_governor_forced = true;

	EXPECT_EQ(rig.WarmAndTrigger(EeBound(70.0)).kind, Kind::Exhausted);

	// Forced IOP thread, free governor: the governor.
	Rig rig2;
	rig2.state.iop_thread_placeable = true;
	rig2.state.iop_thread_forced = true;
	rig2.state.ee_governor_eligible = true;
	EXPECT_EQ(rig2.WarmAndTrigger(EeBound(70.0)).lever, AutoTuneLever::EeGovernor);
}

// ---------------------------------------------------------------------------------------------- GS

TEST(AutoTuneController, GpuBoundStepsResolutionDownAndStopsAtNative)
{
	Rig rig;
	rig.state.resolution = 3.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	rig.state.blending_level = 0;

	AutoTuneAction a = rig.WarmAndTrigger(GpuBound(50.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::Resolution);
	EXPECT_EQ(a.value, 2.0);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Gpu);

	a = rig.RunUntilAction(GpuBound(62.0));
	ASSERT_EQ(a.kind, Kind::Keep);
	rig.state.resolution = 2.0;

	rig.FeedFor(GpuBound(62.0), 3.0);
	a = rig.Trigger(GpuBound(62.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::Resolution);
	EXPECT_EQ(a.value, 1.0);

	a = rig.RunUntilAction(GpuBound(80.0));
	ASSERT_EQ(a.kind, Kind::Keep);
	rig.state.resolution = 1.0;

	// Still slow at 1x with nothing else to give up: the controller stops.
	rig.FeedFor(GpuBound(80.0), 3.0);
	a = rig.Trigger(GpuBound(80.0));
	EXPECT_EQ(a.kind, Kind::Exhausted);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Gpu);
	for (const AutoTuneAction& logged : rig.log)
	{
		if (logged.kind == Kind::StartTrial && logged.lever == AutoTuneLever::Resolution)
			EXPECT_GE(logged.value, 1.0);
	}
}

TEST(AutoTuneController, ResolutionStepsToTheNotchBelowNotTheNextQuarter)
{
	Rig rig;
	rig.state.resolution = 2.5;
	EXPECT_EQ(rig.WarmAndTrigger(GpuBound(50.0)).value, 2.0);

	Rig rig2;
	rig2.state.resolution = 1.5;
	EXPECT_EQ(rig2.WarmAndTrigger(GpuBound(50.0)).value, 1.0);

	Rig rig3;
	rig3.state.resolution = 16.0;
	EXPECT_EQ(rig3.WarmAndTrigger(GpuBound(50.0)).value, 12.0);

	// Sub-native choices and 1x itself have nowhere to go.
	Rig rig4;
	rig4.state.resolution = 0.75;
	rig4.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	EXPECT_EQ(rig4.WarmAndTrigger(GpuBound(50.0)).kind, Kind::Exhausted);
}

TEST(AutoTuneController, GsCpuBoundTriesTheBackThreadBeforeResolution)
{
	Rig rig;
	rig.state.resolution = 3.0;
	rig.state.gs_back_thread_allowed = true;

	const AutoTuneAction a = rig.WarmAndTrigger(GsCpuBound(60.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::GsBackThread);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::GsCpu);
}

TEST(AutoTuneController, GpuBoundTriesResolutionBeforeTheBackThread)
{
	Rig rig;
	rig.state.resolution = 3.0;
	rig.state.gs_back_thread_allowed = true;
	EXPECT_EQ(rig.WarmAndTrigger(GpuBound(60.0)).lever, AutoTuneLever::Resolution);

	// With nothing to lower the back thread is next.
	Rig rig2;
	rig2.state.resolution = 1.0;
	rig2.state.gs_back_thread_allowed = true;
	EXPECT_EQ(rig2.WarmAndTrigger(GpuBound(60.0)).lever, AutoTuneLever::GsBackThread);
}

TEST(AutoTuneController, TheBackThreadIsOnlyTriedWhereItWouldRun)
{
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.gs_back_thread_allowed = false; // not Vulkan, or Unsynchronized downloads
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	EXPECT_EQ(rig.WarmAndTrigger(GsCpuBound(60.0)).kind, Kind::Exhausted);
}

TEST(AutoTuneController, DownloadModeStepsTowardFewerReadbacksOnlyWhileReadingBack)
{
	const auto first_target = [](int mode) {
		Rig rig;
		rig.state.resolution = 1.0;
		rig.state.download_mode = mode;
		return rig.WarmAndTrigger(GpuBound(60.0));
	};

	AutoTuneAction a = first_target(AutoTuneDownloadMode::Accurate);
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::DownloadMode);
	EXPECT_EQ(a.value, static_cast<double>(AutoTuneDownloadMode::Asynchronous));

	a = first_target(AutoTuneDownloadMode::AccurateForceFull);
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.value, static_cast<double>(AutoTuneDownloadMode::Asynchronous));

	a = first_target(AutoTuneDownloadMode::Asynchronous);
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.value, static_cast<double>(AutoTuneDownloadMode::NoReadbacks));

	// Modes that do not read back have nothing to give up.
	EXPECT_EQ(first_target(AutoTuneDownloadMode::NoReadbacks).kind, Kind::Exhausted);
	EXPECT_EQ(first_target(AutoTuneDownloadMode::Unsynchronized).kind, Kind::Exhausted);
	EXPECT_EQ(first_target(AutoTuneDownloadMode::Disabled).kind, Kind::Exhausted);
}

TEST(AutoTuneController, AGameDatabaseDownloadModeIsLeftAlone)
{
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.download_mode = AutoTuneDownloadMode::Accurate;
	rig.state.download_mode_forced = true;
	EXPECT_EQ(rig.WarmAndTrigger(GpuBound(60.0)).kind, Kind::Exhausted);
}

TEST(AutoTuneController, BlendingStepsOneLevelAtATimeToTheFloor)
{
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	rig.state.blending_level = 3;
	rig.state.blending_floor = 1; // the game database's minimum

	AutoTuneAction a = rig.WarmAndTrigger(GpuBound(60.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::Blending);
	EXPECT_EQ(a.value, 2.0);
	ASSERT_EQ(rig.RunUntilAction(GpuBound(70.0)).kind, Kind::Keep);
	rig.state.blending_level = 2;

	rig.FeedFor(GpuBound(70.0), 3.0);
	a = rig.Trigger(GpuBound(70.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.value, 1.0);
	ASSERT_EQ(rig.RunUntilAction(GpuBound(80.0)).kind, Kind::Keep);
	rig.state.blending_level = 1;

	// At the database's minimum there is no step left.
	rig.FeedFor(GpuBound(80.0), 3.0);
	EXPECT_EQ(rig.Trigger(GpuBound(80.0)).kind, Kind::Exhausted);
}

TEST(AutoTuneController, BasicToMinimumIsRejectedWhenItDoesNotHelpAndNotTriedAgain)
{
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	rig.state.blending_level = 1; // Basic

	AutoTuneAction a = rig.WarmAndTrigger(GpuBound(60.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::Blending);
	EXPECT_EQ(a.value, 0.0); // Minimum

	// Minimum can be slower than Basic on a GPU-bound device: no gain.
	a = rig.RunUntilAction(GpuBound(58.0));
	ASSERT_EQ(a.kind, Kind::Revert);
	EXPECT_TRUE(a.measured);
	EXPECT_EQ(a.lever, AutoTuneLever::Blending);
	EXPECT_EQ(a.value, 0.0);

	// Reverted (the caller put Basic back), settled, still slow: no second try, so exhausted.
	rig.FeedFor(GpuBound(60.0), 3.0);
	a = rig.Trigger(GpuBound(60.0));
	EXPECT_EQ(a.kind, Kind::Exhausted);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 1u);
}

TEST(AutoTuneController, WhatAnEarlierSessionRejectedIsNotTried)
{
	Rig rig;
	rig.state.resolution = 3.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	rig.state.rejected = {{AutoTuneLever::Resolution, 2.0}};

	// Resolution 3 -> 2 is remembered as useless, so the controller goes to blending.
	rig.state.blending_level = 2;
	const AutoTuneAction a = rig.WarmAndTrigger(GpuBound(60.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::Blending);
}

TEST(AutoTuneController, ARejectionIsAboutOneStepNotTheWholeLever)
{
	Rig rig;
	rig.state.resolution = 4.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	// 2x -> 1x was useless once. From 4x the step is 4x -> 3x, which is a different trial.
	rig.state.rejected = {{AutoTuneLever::Resolution, 1.0}};
	const AutoTuneAction a = rig.WarmAndTrigger(GpuBound(60.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.value, 3.0);
}

// ---------------------------------------------------------------------------------------------- report only

TEST(AutoTuneController, Vu1BoundIsReportedOnceAndNothingIsTried)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.state.resolution = 3.0;
	AutoTuneWindow w = Win(60.0);
	w.ee_pct = 70.0;
	w.mtvu_pct = 96.0;

	const AutoTuneAction a = rig.WarmAndTrigger(w);
	EXPECT_EQ(a.kind, Kind::ReportBottleneck);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Vu1);

	// Said once per session.
	rig.FeedFor(w, 60.0);
	EXPECT_EQ(rig.Count(Kind::ReportBottleneck), 1u);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 0u);
}

TEST(AutoTuneController, IopBoundIsReportedOnceAndNothingIsTried)
{
	Rig rig;
	rig.state.iop_thread_on = true;
	AutoTuneWindow w = Win(60.0);
	w.ee_pct = 60.0;
	w.iop_pct = 95.0;

	const AutoTuneAction a = rig.WarmAndTrigger(w);
	EXPECT_EQ(a.kind, Kind::ReportBottleneck);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Iop);
	rig.FeedFor(w, 60.0);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 0u);
}

// ---------------------------------------------------------------------------------------------- end states

TEST(AutoTuneController, ExhaustedIsSaidOnceThenTheSessionIsQuiet)
{
	Rig rig;
	// Nothing can be tried for an EE-bound game.
	const AutoTuneAction a = rig.WarmAndTrigger(EeBound(70.0));
	EXPECT_EQ(a.kind, Kind::Exhausted);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Ee);

	rig.FeedFor(EeBound(70.0), 120.0);
	EXPECT_EQ(rig.Count(Kind::Exhausted), 1u);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 0u);
}

TEST(AutoTuneController, ABottleneckThatMovedGetsItsOwnLevers)
{
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	ASSERT_EQ(rig.WarmAndTrigger(GpuBound(70.0)).kind, Kind::Exhausted);

	// Later the game is EE-bound and the IOP thread has become available.
	rig.state.iop_thread_placeable = true;
	const AutoTuneAction a = rig.Trigger(EeBound(70.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::IopThread);
}

TEST(AutoTuneController, SlowButUnexplainedIsReportedWithACooldown)
{
	Rig rig;
	AutoTuneWindow w = Win(70.0);
	w.has_gpu_timer = true; // the GPU is idle, no thread is saturated, nothing is blocked
	w.ee_pct = 50.0;
	w.mtgs_pct = 30.0;
	w.gpu_pct = 20.0;
	w.ee_wait_gs_ratio = 0.02;

	EXPECT_EQ(rig.WarmAndTrigger(w).kind, Kind::Unclassified);
	// Not again for half a minute.
	EXPECT_EQ(rig.FeedFor(w, 28.0).kind, Kind::None);
	EXPECT_EQ(rig.FeedFor(w, 6.0).kind, Kind::Unclassified);
}

// ---------------------------------------------------------------------------------------------- keep or revert

TEST(AutoTuneController, AGainUnderFivePercentIsReverted)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(80.0)).kind, Kind::StartTrial);
	// 80 -> 83.9 is +4.9%.
	const AutoTuneAction a = rig.RunUntilAction(EeBound(83.9));
	EXPECT_EQ(a.kind, Kind::Revert);
	EXPECT_NEAR(a.gain, 0.04875, 0.001);
}

TEST(AutoTuneController, AGainOfFivePercentIsKept)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(80.0)).kind, Kind::StartTrial);
	EXPECT_EQ(rig.RunUntilAction(EeBound(84.0)).kind, Kind::Keep);
}

TEST(AutoTuneController, ReachingFullSpeedIsKeptEvenWithASmallGain)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(94.0)).kind, Kind::StartTrial);
	// 94 -> 99.5 is +5.9%: kept either way. Use a case that is only the "reached" rule:
	const AutoTuneAction a = rig.RunUntilAction(EeBound(99.5));
	EXPECT_EQ(a.kind, Kind::Keep);

	Rig rig2;
	rig2.state.iop_thread_placeable = true;
	ASSERT_EQ(rig2.WarmAndTrigger(EeBound(95.0 - 0.001)).kind, Kind::StartTrial);
	// 94.999 -> 99.0 is +4.2%, under the bar, but it is full speed with no late frames.
	EXPECT_EQ(rig2.RunUntilAction(EeBound(99.0)).kind, Kind::Keep);
}

TEST(AutoTuneController, FullSpeedWithLateFramesIsNotFullSpeed)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(94.5)).kind, Kind::StartTrial);
	AutoTuneWindow w = EeBound(99.0);
	w.late_frame_ratio = 0.2;
	EXPECT_EQ(rig.RunUntilAction(w).kind, Kind::Revert);
}

TEST(AutoTuneController, AStutterOnlyProblemIsKeptWhenTheLateFramesGoAway)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	AutoTuneWindow stutter = EeBound(100.0);
	stutter.late_frame_ratio = 0.2;
	ASSERT_EQ(rig.WarmAndTrigger(stutter).kind, Kind::StartTrial);
	EXPECT_EQ(rig.RunUntilAction(EeBound(100.0)).kind, Kind::Keep);

	// Full speed all along and no change in the late frames: not a gain.
	Rig rig2;
	rig2.state.iop_thread_placeable = true;
	ASSERT_EQ(rig2.WarmAndTrigger(stutter).kind, Kind::StartTrial);
	EXPECT_EQ(rig2.RunUntilAction(stutter).kind, Kind::Revert);
}

// ---------------------------------------------------------------------------------------------- stability

TEST(AutoTuneController, SpeedHoveringAtTheLineNeverTriggers)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.state.resolution = 3.0;
	rig.WarmUp();
	for (int i = 0; i < 2000; i++)
		rig.Feed(EeBound((i & 1) ? 95.1 : 94.9));
	EXPECT_TRUE(rig.log.empty());
}

TEST(AutoTuneController, SpeedStuckJustUnderTheLineEndsInFewTrialsAndStaysQuiet)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.state.ee_governor_eligible = true;

	// Every trial changes nothing. Each lever is tried once, then the controller stops.
	rig.WarmUp();
	for (int i = 0; i < 4000; i++)
		rig.Feed(EeBound(94.9));

	EXPECT_EQ(rig.Count(Kind::StartTrial), 2u);
	EXPECT_EQ(rig.Count(Kind::Revert), 2u);
	EXPECT_EQ(rig.Count(Kind::Keep), 0u);
	EXPECT_EQ(rig.Count(Kind::Exhausted), 1u);
}

// ---------------------------------------------------------------------------------------------- disturbances

TEST(AutoTuneController, ASceneChangeCancelsATrialAndWarmsUpAgain)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(70.0)).kind, Kind::StartTrial);
	rig.FeedFor(EeBound(70.0), 7.0);
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);

	const std::optional<AutoTuneAction> revert = rig.ctl.OnDisturbance(AutoTuneDisturbance::SceneChange);
	ASSERT_TRUE(revert.has_value());
	EXPECT_EQ(revert->kind, Kind::Revert);
	EXPECT_EQ(revert->lever, AutoTuneLever::IopThread);
	EXPECT_FALSE(revert->measured);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::WarmUp);

	// A cancelled trial is not remembered as useless: after the warm-up it is tried again.
	rig.FeedFor(Win(100.0), 10.0);
	EXPECT_EQ(rig.Trigger(EeBound(70.0)).lever, AutoTuneLever::IopThread);
}

TEST(AutoTuneController, AnExternalSettingsChangeCancelsATrialAndSettles)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(70.0)).kind, Kind::StartTrial);

	const std::optional<AutoTuneAction> revert = rig.ctl.OnDisturbance(AutoTuneDisturbance::SettingsChange);
	ASSERT_TRUE(revert.has_value());
	EXPECT_EQ(revert->kind, Kind::Revert);
	EXPECT_FALSE(revert->measured);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingAfter);

	// Three seconds of settle, then watching.
	EXPECT_EQ(rig.FeedFor(EeBound(70.0), 2.5).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingAfter);
	rig.Feed(EeBound(70.0));
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Watching);
}

TEST(AutoTuneController, ASettingsChangeOutsideATrialJustSettles)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();
	rig.Feed(EeBound(70.0));
	rig.Feed(EeBound(70.0));

	EXPECT_FALSE(rig.ctl.OnDisturbance(AutoTuneDisturbance::SettingsChange).has_value());
	EXPECT_EQ(rig.ctl.BadStreak(), 0u);
	EXPECT_EQ(rig.FeedFor(EeBound(70.0), 2.5).kind, Kind::None);
	// 3 s of settle, then 4 bad windows to trigger.
	rig.Feed(EeBound(70.0));
	EXPECT_EQ(rig.Trigger(EeBound(70.0)).kind, Kind::StartTrial);
}

TEST(AutoTuneController, ResumingFromAPauseKeepsATrialRunning)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(70.0)).kind, Kind::StartTrial);
	rig.FeedFor(EeBound(70.0), 7.0);
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);

	EXPECT_FALSE(rig.ctl.OnDisturbance(AutoTuneDisturbance::Resumed).has_value());
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);
}

TEST(AutoTuneController, AFailedApplyCountsAsATrialThatDidNotHelp)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.state.ee_governor_eligible = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(70.0)).lever, AutoTuneLever::IopThread);
	rig.ctl.NoteApplyFailed();
	EXPECT_FALSE(rig.ctl.TrialActive());

	rig.FeedFor(EeBound(70.0), 3.0);
	EXPECT_EQ(rig.Trigger(EeBound(70.0)).lever, AutoTuneLever::EeGovernor);
}

TEST(AutoTuneController, ResetForgetsTheSession)
{
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	ASSERT_EQ(rig.WarmAndTrigger(GpuBound(70.0)).kind, Kind::Exhausted);
	rig.ctl.Reset();
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::WarmUp);
	rig.WarmUp();
	EXPECT_EQ(rig.Trigger(GpuBound(70.0)).kind, Kind::Exhausted);
	EXPECT_EQ(rig.Count(Kind::Exhausted), 2u);
}

// ---------------------------------------------------------------------------------------------- classification

TEST(AutoTuneController, ClassificationTable)
{
	AutoTuneController ctl;

	// EE saturated, not blocked: EE.
	AutoTuneWindow w = EeBound(70.0);
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Ee);

	// EE saturated but waiting on the GS for 40% of the time: the GS side.
	w.ee_wait_gs_ratio = 0.40;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Gpu);

	// ...unless the GS thread itself is saturated.
	w.mtgs_pct = 95.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::GsCpu);

	// The back thread counts as a GS thread.
	w = EeBound(70.0);
	w.gs_back_pct = 93.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::GsCpu);

	// A busy GPU timer.
	EXPECT_EQ(ctl.Classify(GpuBound(70.0)), AutoTuneBottleneck::Gpu);

	// The GS thread reads just as busy when it is feeding a busy GPU: the GPU wins.
	w = GpuBound(70.0);
	w.mtgs_pct = 99.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Gpu);
	// With the GPU idle the same thread is the cause.
	w.gpu_pct = 40.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::GsCpu);

	// VU1 and IOP.
	w = Win(70.0);
	w.mtvu_pct = 91.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Vu1);
	w = Win(70.0);
	w.iop_pct = 91.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Iop);
}

TEST(AutoTuneController, AnEeBlockedOnTheIopThreadIsIopBoundNotEeBound)
{
	AutoTuneController ctl;

	// The EE thread reads saturated because it is spinning for the IOP thread. Slowing it, or giving it
	// more threads, does not help: report the IOP.
	AutoTuneWindow w = EeBound(70.0);
	w.iop_pct = 55.0; // the IOP thread itself is not at its limit: it is behind, or asleep when woken
	w.ee_wait_iop_ratio = 0.40;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Iop);

	// A small wait is part of normal life with an IOP thread.
	w.ee_wait_iop_ratio = 0.10;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Ee);

	// No figure (no baseline yet, or the counters started over) is not a wait.
	w.ee_wait_iop_ratio.reset();
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Ee);

	// The GS side still goes first: a GS-bound game with the IOP thread on is not an IOP case.
	w.ee_wait_iop_ratio = 0.40;
	w.ee_wait_gs_ratio = 0.40;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Gpu);
}

TEST(AutoTuneController, AnIopBoundCaseIsReportedAndTheGovernorIsNotTried)
{
	Rig rig;
	rig.state.iop_thread_on = true;
	rig.state.ee_governor_eligible = true;
	AutoTuneWindow w = EeBound(60.0);
	w.ee_wait_iop_ratio = 0.35;

	const AutoTuneAction a = rig.WarmAndTrigger(w);
	EXPECT_EQ(a.kind, Kind::ReportBottleneck);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Iop);
	rig.FeedFor(w, 60.0);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 0u) << "the governor would underclock an EE that is only waiting";
}

TEST(AutoTuneController, TheIopWaitIsAveragedOverTheWindowsThatTriggered)
{
	// Two windows blocked, two not: the mean, 0.20, is under the line.
	Rig rig;
	rig.state.iop_thread_on = true;
	rig.state.ee_governor_eligible = true;
	rig.WarmUp();
	for (const double wait : {0.40, 0.40, 0.0, 0.0})
	{
		AutoTuneWindow w = EeBound(60.0);
		w.ee_wait_iop_ratio = wait;
		rig.Feed(w);
	}
	ASSERT_EQ(rig.log.size(), 1u);
	EXPECT_EQ(rig.log[0].kind, Kind::StartTrial);
	EXPECT_EQ(rig.log[0].lever, AutoTuneLever::EeGovernor);
}

TEST(AutoTuneController, WithoutAGpuTimerBlockedOnTheGsMeansGpuBound)
{
	AutoTuneController ctl;
	AutoTuneWindow w = Win(70.0);
	w.ee_pct = 70.0;
	w.mtgs_pct = 50.0;
	w.has_gpu_timer = false;
	w.ee_wait_gs_ratio = 0.30;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Gpu);

	// Waiting 10%, nothing saturated, and the timer cannot say: nothing to go on.
	w.ee_wait_gs_ratio = 0.10;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::None);
}

TEST(AutoTuneController, WithTheWaitsUnknownTheFallbackIsNothingSaturated)
{
	AutoTuneController ctl;
	// Speed bad, EE not saturated, GS threads not saturated, no timer, no waits: GPU.
	AutoTuneWindow w = Win(70.0);
	w.ee_pct = 70.0;
	w.mtgs_pct = 50.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Gpu);

	// With a timer that says the GPU is idle, the fallback does not apply.
	w.has_gpu_timer = true;
	w.gpu_pct = 10.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::None);

	// EE saturated is EE-bound, with or without the waits.
	w = EeBound(70.0);
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Ee);
}

TEST(AutoTuneController, TheControllerWorksEndToEndWithTheWaitsUnknown)
{
	Rig rig;
	rig.state.resolution = 2.0;
	AutoTuneWindow w = Win(60.0);
	w.ee_pct = 65.0;
	w.mtgs_pct = 45.0;
	const AutoTuneAction a = rig.WarmAndTrigger(w);
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::Resolution);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Gpu);
}

TEST(AutoTuneController, LeverNamesRoundTrip)
{
	for (size_t i = 0; i < static_cast<size_t>(AutoTuneLever::Count); i++)
	{
		const AutoTuneLever lever = static_cast<AutoTuneLever>(i);
		const std::optional<AutoTuneLever> back = AutoTuneLeverFromName(AutoTuneLeverName(lever));
		ASSERT_TRUE(back.has_value());
		EXPECT_EQ(*back, lever);
	}
	EXPECT_FALSE(AutoTuneLeverFromName("nonsense").has_value());
}

TEST(AutoTuneController, EveryThresholdLivesInThePolicy)
{
	// A policy with a longer warm-up and a different trigger changes behaviour with no other edit.
	AutoTunePolicy policy;
	policy.warmup_seconds = 2.0;
	policy.trigger_windows = 2;
	Rig rig;
	rig.ctl = AutoTuneController(policy);
	rig.state.iop_thread_placeable = true;

	rig.FeedFor(Win(100.0), 2.0);
	rig.Feed(EeBound(70.0));
	EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::StartTrial);
}
