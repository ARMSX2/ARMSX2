// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AutoTuneController, by replaying scripted windows. Nothing here touches the emulator: the controller has no
// clock, so a test is a list of windows and the actions that come back.

#include <gtest/gtest.h>

#include "AutoTune/AutoTuneController.h"

#include <algorithm>
#include <string>
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

		// Bad windows in a row after the warm-up: four start the diagnosis, and when it chooses a lever, four more
		// are the baseline the trial is compared with and the trial starts on the last of them. Returns the first
		// action that comes back (the fourth window's for a report, the eighth's for a trial).
		AutoTuneAction Trigger(const AutoTuneWindow& bad)
		{
			AutoTuneAction last;
			for (int i = 0; i < 4 + static_cast<int>(ctl.GetPolicy().baseline_extra_windows); i++)
			{
				last = Feed(bad);
				if (last.kind != Kind::None)
					break;
			}
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

	// The fourth bad window decides what to try; four more are its baseline, and the trial starts on the last.
	EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Baselining);
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::None);
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
	EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::None);
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Baselining) << "the fourth usable bad window completed the streak";
	// The baseline is counted in usable windows too.
	EXPECT_EQ(rig.FeedFor(paused, 30.0).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Baselining);
	EXPECT_EQ(rig.RunUntilAction(EeBound(70.0)).kind, Kind::StartTrial);
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

// The governor counts in game time: two seconds of warm-up, then two windows of overload, then a step, then
// two seconds of cooldown, then two windows for the second step. On a game running at 70% every second of
// that takes 1.4 seconds on the wall, and a trial that measured before the governor had acted would call it
// useless and remember that for good.

TEST(AutoTunePolicy, TheLightSettleIsOneAndAHalfSecondsAndTheHeavyOneIsStillSix)
{
	// After a resolution, blending or download mode change the new speed shows in the first full window, so the
	// light settle is short. The levers that reset the recompilers or reopen the renderer keep the six-second
	// settle; a hitch that outlasts the shorter one is the measurement extension's to absorb.
	EXPECT_DOUBLE_EQ(AutoTunePolicy().settle_seconds, 1.5);
	EXPECT_DOUBLE_EQ(AutoTunePolicy().settle_heavy_seconds, 6.0);
}

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

	rig.FeedFor(GpuBound(62.0), 1.5);
	a = rig.Trigger(GpuBound(62.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::Resolution);
	EXPECT_EQ(a.value, 1.0);

	a = rig.RunUntilAction(GpuBound(80.0));
	ASSERT_EQ(a.kind, Kind::Keep);
	rig.state.resolution = 1.0;

	// Still slow at 1x with nothing else to give up: the controller stops.
	rig.FeedFor(GpuBound(80.0), 1.5);
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

	// With nothing to lower and nothing to read back the back thread is next.
	Rig rig2;
	rig2.state.resolution = 1.0;
	rig2.state.gs_back_thread_allowed = true;
	rig2.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	EXPECT_EQ(rig2.WarmAndTrigger(GpuBound(60.0)).lever, AutoTuneLever::GsBackThread);
}

TEST(AutoTuneController, GpuBoundTriesTheDownloadModeBeforeTheBackThread)
{
	// A download mode that reads back is cheap to try (the light settle, no renderer reopen), and a stall on the
	// readbacks reads as the GPU. The back thread costs two renderer reopens when it does not help.
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.gs_back_thread_allowed = true;
	rig.state.download_mode = AutoTuneDownloadMode::Accurate;
	EXPECT_EQ(rig.WarmAndTrigger(GpuBound(60.0)).lever, AutoTuneLever::DownloadMode);

	// The GS on the CPU is the other way round: the back thread splits that work, the download mode does not.
	Rig cpu;
	cpu.state.resolution = 1.0;
	cpu.state.gs_back_thread_allowed = true;
	cpu.state.download_mode = AutoTuneDownloadMode::Accurate;
	EXPECT_EQ(cpu.WarmAndTrigger(GsCpuBound(60.0)).lever, AutoTuneLever::GsBackThread);
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

	rig.FeedFor(GpuBound(70.0), 1.5);
	a = rig.Trigger(GpuBound(70.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.value, 1.0);
	ASSERT_EQ(rig.RunUntilAction(GpuBound(80.0)).kind, Kind::Keep);
	rig.state.blending_level = 1;

	// At the database's minimum there is no step left.
	rig.FeedFor(GpuBound(80.0), 1.5);
	EXPECT_EQ(rig.Trigger(GpuBound(80.0)).kind, Kind::Exhausted);
}

TEST(AutoTuneController, BasicToMinimumIsRejectedWhenItDoesNotHelpAndNotTriedAgain)
{
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	rig.state.blending_level = 1; // Basic

	AutoTuneAction a = rig.WarmAndTrigger(GsCpuBound(60.0));
	ASSERT_EQ(a.kind, Kind::StartTrial);
	EXPECT_EQ(a.lever, AutoTuneLever::Blending);
	EXPECT_EQ(a.value, 0.0); // Minimum

	// Minimum does not always help even where the GS thread is the limit: no gain.
	a = rig.RunUntilAction(GsCpuBound(58.0));
	ASSERT_EQ(a.kind, Kind::Revert);
	EXPECT_TRUE(a.measured);
	EXPECT_EQ(a.lever, AutoTuneLever::Blending);
	EXPECT_EQ(a.value, 0.0);

	// Reverted (the caller put Basic back), settled, still slow: no second try, so exhausted.
	rig.FeedFor(GsCpuBound(60.0), 1.5);
	a = rig.Trigger(GsCpuBound(60.0));
	EXPECT_EQ(a.kind, Kind::Exhausted);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 1u);
}

TEST(AutoTuneController, BasicToMinimumIsNeverOfferedToAGpuBoundGame)
{
	// Measured on GPU-bound games it gained little or lost speed. It only paid where the GS thread on the CPU is
	// the limit.
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	rig.state.blending_level = 1; // Basic
	const AutoTuneAction a = rig.WarmAndTrigger(GpuBound(60.0));
	EXPECT_EQ(a.kind, Kind::Exhausted);
	EXPECT_EQ(a.bottleneck, AutoTuneBottleneck::Gpu);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 0u);

	// The same for the GPU case that has no GPU timer: the EE blocked on a GS that is not CPU-bound.
	Rig blind;
	blind.state.resolution = 1.0;
	blind.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	blind.state.blending_level = 1;
	AutoTuneWindow w = Win(60.0);
	w.ee_pct = 70.0;
	w.mtgs_pct = 50.0;
	w.ee_wait_gs_ratio = 0.4;
	EXPECT_EQ(blind.WarmAndTrigger(w).kind, Kind::Exhausted);
}

TEST(AutoTuneController, TheStepsAboveBasicAreOfferedToEveryBottleneck)
{
	for (const AutoTuneWindow& w : {GpuBound(60.0), GsCpuBound(60.0)})
	{
		Rig rig;
		rig.state.resolution = 1.0;
		rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
		rig.state.blending_level = 2; // Medium, one step above Basic
		const AutoTuneAction a = rig.WarmAndTrigger(w);
		ASSERT_EQ(a.kind, Kind::StartTrial);
		EXPECT_EQ(a.lever, AutoTuneLever::Blending);
		EXPECT_EQ(a.value, 1.0);
	}
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

TEST(AutoTuneController, ExhaustedIsSaidOncePerSessionWhateverTheClass)
{
	// The class flips between GS (CPU) and GPU for a thread that sits on the saturation line; both have nothing
	// left. The player is told once.
	Rig rig;
	rig.state.resolution = 1.0;
	rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
	ASSERT_EQ(rig.WarmAndTrigger(GsCpuBound(70.0)).kind, Kind::Exhausted);
	EXPECT_EQ(rig.FeedFor(GpuBound(70.0), 20.0).kind, Kind::None);
	EXPECT_EQ(rig.FeedFor(GsCpuBound(70.0), 20.0).kind, Kind::None);
	EXPECT_EQ(rig.Count(Kind::Exhausted), 1u);

	// A new session says it again.
	rig.ctl.Reset();
	ASSERT_EQ(rig.WarmAndTrigger(GpuBound(70.0)).kind, Kind::Exhausted);
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

TEST(AutoTuneController, AGainThatClearlyIsNotThereIsRevertedAndRemembered)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(80.0)).kind, Kind::StartTrial);
	// 80 -> 80.8 is +1%: under the bar even allowing for the noise.
	const AutoTuneAction a = rig.RunUntilAction(EeBound(80.8));
	EXPECT_EQ(a.kind, Kind::Revert);
	EXPECT_TRUE(a.measured);
	EXPECT_FALSE(a.inconclusive);
	EXPECT_NEAR(a.gain, 0.01, 0.001);
}

TEST(AutoTuneController, AGainJustUnderTheBarIsInconclusiveNotRejected)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	ASSERT_EQ(rig.WarmAndTrigger(EeBound(80.0)).kind, Kind::StartTrial);
	// 80 -> 83.9 is +4.9%: under the bar, but within the noise of reaching it. Not enough to say it did not
	// help, so it is looked at for longer and then undone without being remembered.
	rig.FeedFor(EeBound(80.0), 6.0);
	for (int i = 0; i < 4 + 4 + 4 - 1; i++)
		EXPECT_EQ(rig.Feed(EeBound(83.9)).kind, Kind::None) << "window " << i << ": measured, then extended twice";
	const AutoTuneAction a = rig.Feed(EeBound(83.9));
	EXPECT_EQ(a.kind, Kind::Revert);
	EXPECT_FALSE(a.measured) << "an answer within the noise is not written to the game's file";
	EXPECT_TRUE(a.inconclusive);
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
	// 94 -> 99.5 is +5.9%: kept on the gain alone.
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

	// A second and a half of settle, then watching.
	EXPECT_EQ(rig.FeedFor(EeBound(70.0), 1.0).kind, Kind::None);
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
	EXPECT_EQ(rig.FeedFor(EeBound(70.0), 1.0).kind, Kind::None);
	// 1.5 s of settle, then 4 bad windows to trigger.
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

	rig.FeedFor(EeBound(70.0), 1.5);
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

TEST(AutoTuneController, AnIopThreadAtFullCpuWhileTheEeDoesNotWaitForItIsNotTheBottleneck)
{
	// The IOP thread paces itself against the EE and spins while it waits, so its CPU share reads saturated
	// whenever the EE is. With the EE spending none of its time waiting for the IOP, the EE is the cause.
	AutoTuneController ctl;
	AutoTuneWindow w = EeBound(22.0);
	w.iop_pct = 99.5;
	w.ee_wait_iop_ratio = 0.0;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Ee);

	// Without the wait figure the CPU share is all there is to go on.
	w.ee_wait_iop_ratio.reset();
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Iop);

	// A wait over the line is the IOP whatever its CPU share says (it can be asleep when woken).
	w.iop_pct = 40.0;
	w.ee_wait_iop_ratio = 0.4;
	EXPECT_EQ(ctl.Classify(w), AutoTuneBottleneck::Iop);
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
	// The diagnosis is made on those four; the baseline of the trial it chose is four more.
	for (int i = 0; i < 4; i++)
		rig.Feed(EeBound(60.0));
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
	policy.baseline_extra_windows = 1;
	Rig rig;
	rig.ctl = AutoTuneController(policy);
	rig.state.iop_thread_placeable = true;

	rig.FeedFor(Win(100.0), 2.0);
	rig.Feed(EeBound(70.0));
	EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::None) << "the second bad window starts the diagnosis";
	EXPECT_EQ(rig.Feed(EeBound(70.0)).kind, Kind::StartTrial) << "and one more window is the baseline";
}

TEST(AutoTuneStats, TheMedianIgnoresAHitchWindow)
{
	const AutoTuneStat steady = AutoTuneSummarize({12.2, 12.3, 12.2, 12.1, 12.2, 12.3, 12.2, 12.2});
	const AutoTuneStat hitch = AutoTuneSummarize({12.2, 12.3, 12.2, 12.1, 10.8, 12.3, 12.2, 12.2});
	EXPECT_NEAR(steady.centre, 12.2, 1e-9);
	EXPECT_NEAR(hitch.centre, 12.2, 1e-9) << "the mean would be 12.0";
	EXPECT_LT(hitch.error, 0.1) << "and one window out of eight does not make the figure uncertain";
	EXPECT_EQ(hitch.n, 8u);
}

TEST(AutoTuneStats, ASpreadMakesTheFigureUncertain)
{
	const AutoTuneStat noisy = AutoTuneSummarize({50.0, 60.0, 45.0, 62.0});
	EXPECT_NEAR(noisy.centre, 55.0, 1e-9);
	EXPECT_GT(noisy.error, 4.0);
	EXPECT_EQ(AutoTuneSummarize({}).n, 0u);
	EXPECT_EQ(AutoTuneSummarize({7.0}).centre, 7.0);
	EXPECT_EQ(AutoTuneSummarize({7.0}).error, 0.0);
}

TEST(AutoTuneStats, AGainHasToBeBothBigEnoughAndBeyondTheNoise)
{
	const AutoTunePolicy policy;
	const auto stat = [](std::vector<double> v) { return AutoTuneSummarize(std::move(v)); };

	// Steady windows, +21%: clear.
	AutoTuneGain g = AutoTuneJudgeGain(stat({10.0, 10.1, 10.1, 10.2}), stat({12.2, 12.3, 12.2, 12.1}), true, policy);
	EXPECT_TRUE(g.clear);
	EXPECT_FALSE(g.none);

	// Steady windows, +1.6%: clearly nothing.
	g = AutoTuneJudgeGain(stat({12.2, 12.2, 12.2, 12.2}), stat({12.4, 12.4, 12.4, 12.4}), true, policy);
	EXPECT_FALSE(g.clear);
	EXPECT_TRUE(g.none);

	// Steady windows, +4.9%: under the bar, but not clearly under it.
	g = AutoTuneJudgeGain(stat({80, 80, 80, 80}), stat({83.9, 83.9, 83.9, 83.9}), true, policy);
	EXPECT_FALSE(g.clear);
	EXPECT_FALSE(g.none);

	// +10% on windows that scatter by 20%: over the bar and not beyond the noise.
	g = AutoTuneJudgeGain(stat({50, 50, 50, 50}), stat({50, 60, 45, 62}), true, policy);
	EXPECT_GT(g.gain, 0.05);
	EXPECT_FALSE(g.clear);
	EXPECT_FALSE(g.none);

	// A cost is better lower: 10 ms to 7 ms is a 30% gain.
	g = AutoTuneJudgeGain(stat({10, 10, 10, 10}), stat({7, 7, 7, 7}), false, policy);
	EXPECT_NEAR(g.gain, 0.3, 1e-9);
	EXPECT_TRUE(g.clear);
	EXPECT_FALSE(g.worse);

	// The same for a loss: steady windows 12.5% under the baseline are clearly worse, and not a gain.
	g = AutoTuneJudgeGain(stat({80, 80, 80, 80}), stat({70, 70, 70, 70}), true, policy);
	EXPECT_NEAR(g.gain, -0.125, 1e-9);
	EXPECT_TRUE(g.worse);
	EXPECT_FALSE(g.clear);

	// A loss under the bar is not worse, nor is one inside the noise.
	g = AutoTuneJudgeGain(stat({80, 80, 80, 80}), stat({78, 78, 78, 78}), true, policy);
	EXPECT_FALSE(g.worse);
	g = AutoTuneJudgeGain(stat({50, 50, 50, 50, 50, 50}), stat({31, 38, 52, 60, 33, 66}), true, policy);
	EXPECT_LT(g.gain, -0.05);
	EXPECT_FALSE(g.worse) << "a spread that wide could be anything";
}

namespace
{
	// A GPU-bound window at `speed` with a GPU timer; the cost is the GPU's ms per frame.
	AutoTuneWindow Gpu(double speed, double gpu_ms = 0.0)
	{
		AutoTuneWindow w = GpuBound(speed);
		w.gpu_ms = gpu_ms;
		return w;
	}

	// Nothing for the other levers to do, so a trial is the resolution at `resolution`.
	void OnlyResolutionIsLeft(Rig& rig, double resolution)
	{
		rig.state.hw_renderer = true;
		rig.state.resolution = resolution;
		rig.state.download_mode = AutoTuneDownloadMode::NoReadbacks;
		rig.state.blending_level = 0;
		rig.state.gs_back_thread_allowed = false;
	}

	// The recorded windows in these tests came through a 3 s settle; the default 1.5 s settle would measure them
	// earlier and change what they mean.
	AutoTunePolicy RunWithThreeSecondSettle()
	{
		AutoTunePolicy policy;
		policy.settle_seconds = 3.0;
		return policy;
	}

	AutoTuneAction FeedAll(Rig& rig, const std::vector<double>& speeds)
	{
		AutoTuneAction first;
		for (const double speed : speeds)
		{
			const AutoTuneAction a = rig.Feed(Gpu(speed));
			if (first.kind == Kind::None && a.kind != Kind::None)
				first = a;
		}
		return first;
	}
} // namespace

TEST(AutoTuneJudging, AHitchInTheBaselineDoesNotTurnNothingIntoAGain)
{
	// Recorded God of War II windows at 6x with Maximum blending, the 6x to 5x step. The four bad windows that
	// triggered it were 12.1, 12.2, 10.8 (a hitch) and 12.0, a mean of 11.8; the windows it measured averaged
	// 12.4. A gate on the mean reads +5.3% and keeps the step, but the speed at 5x was the speed at 6x. Against
	// a median baseline of eight windows:
	const std::vector<double> trigger = {12.1, 12.2, 10.8, 12.0};
	const std::vector<double> extra = {12.3, 12.2, 12.3, 12.2};
	const std::vector<double> settle = {12.6, 13.1, 13.0, 12.6, 12.4, 12.4};
	const std::vector<double> measured = {12.4, 13.0, 12.4, 11.8};

	const double old_gain = (12.4 / ((12.1 + 12.2 + 10.8 + 12.0) / 4.0)) - 1.0;
	EXPECT_GT(old_gain, 0.05) << "this is the data of the false keep";

	Rig rig;
	rig.ctl = AutoTuneController(RunWithThreeSecondSettle());
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	EXPECT_EQ(FeedAll(rig, trigger).kind, Kind::None);
	const AutoTuneAction start = FeedAll(rig, extra);
	ASSERT_EQ(start.kind, Kind::StartTrial);
	EXPECT_EQ(start.lever, AutoTuneLever::Resolution);
	EXPECT_EQ(start.value, 5.0);
	EXPECT_NEAR(start.speed_before, 12.2, 1e-9) << "the median of the eight";

	EXPECT_EQ(FeedAll(rig, settle).kind, Kind::None);
	for (const double speed : measured)
		EXPECT_EQ(rig.Feed(Gpu(speed)).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring)
		<< "+1.6% on windows that scatter by 13.0 and 11.8 is not clearly nothing: look longer, do not keep";

	// What the run measured next (its settling-after windows, where the 5x figure came down to the 6x one).
	AutoTuneAction verdict = FeedAll(rig, {12.9, 10.0, 14.8, 12.3});
	EXPECT_EQ(verdict.kind, Kind::None);
	verdict = FeedAll(rig, {12.3, 12.3, 12.2, 12.2});
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_LT(verdict.gain, 0.05);
	EXPECT_NEAR(verdict.speed_after, 12.3, 0.06);
}

TEST(AutoTuneJudging, AClearGainIsKeptOnTheMedians)
{
	// 7x to 6x in the same run: 10.0, 10.1, 10.1, 10.2 against 12.2 ... 12.3.
	Rig rig;
	rig.ctl = AutoTuneController(RunWithThreeSecondSettle());
	OnlyResolutionIsLeft(rig, 7.0);
	rig.WarmUp();
	FeedAll(rig, {10.1, 10.0, 10.1, 10.2, 10.1, 10.1, 10.2, 10.0});
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	FeedAll(rig, {10.5, 11.8, 11.9, 12.3, 12.3, 12.3});
	AutoTuneAction verdict;
	for (const double speed : {12.3, 12.2, 12.2, 12.3})
		verdict = rig.Feed(Gpu(speed));
	ASSERT_EQ(verdict.kind, Kind::Keep);
	EXPECT_NEAR(verdict.gain, 0.2178, 0.005);
}

TEST(AutoTuneJudging, AnAnswerWithinTheNoiseIsExtendedThenUndoneWithoutBeingRemembered)
{
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	FeedAll(rig, {50, 50, 50, 50, 50, 50, 50, 50});
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	rig.FeedFor(Gpu(50.0), 1.5);

	// Scattered windows with a median 10% above the baseline: neither a clear gain nor clearly nothing.
	const std::vector<double> scatter = {50, 60, 45, 62};
	EXPECT_EQ(FeedAll(rig, scatter).kind, Kind::None) << "measured four windows: extend";
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);
	EXPECT_EQ(FeedAll(rig, scatter).kind, Kind::None) << "extended once: extend again";
	const AutoTuneAction verdict = FeedAll(rig, scatter);
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_FALSE(verdict.measured);
	EXPECT_TRUE(verdict.inconclusive);

	// This session does not offer the same step again: nothing else is left, so it says so.
	rig.FeedFor(Gpu(50.0), 1.5);
	const AutoTuneAction next = rig.Trigger(Gpu(50.0));
	EXPECT_EQ(next.kind, Kind::Exhausted);
}

TEST(AutoTuneJudging, ALongerLookCanSettleAnInconclusiveFirstAnswer)
{
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	FeedAll(rig, {50, 50, 50, 50, 50, 50, 50, 50});
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	rig.FeedFor(Gpu(50.0), 1.5);

	EXPECT_EQ(FeedAll(rig, {50, 60, 45, 62}).kind, Kind::None);
	// The next four are steady at 60: eight windows with a median of 60 and a small spread.
	AutoTuneAction verdict;
	for (const double speed : {60.0, 60.0, 59.0, 61.0})
		verdict = rig.Feed(Gpu(speed));
	ASSERT_EQ(verdict.kind, Kind::Keep);
	EXPECT_GT(verdict.gain, 0.15);
}

TEST(AutoTuneJudging, AnEmptyBaselineWindowIsNotACostOfZero)
{
	// The GPU figure is missing from every window: the cost cannot be judged and the speed decides.
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	FeedAll(rig, {40, 40, 40, 40, 40, 40, 40, 40});
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	EXPECT_EQ(rig.log.back().cost_before, 0.0);
	rig.FeedFor(Gpu(40.0), 1.5);
	AutoTuneAction verdict;
	for (int i = 0; i < 4; i++)
		verdict = rig.Feed(Gpu(50.0));
	EXPECT_EQ(verdict.kind, Kind::Keep);
	EXPECT_EQ(verdict.cost_after, 0.0);
}

TEST(AutoTuneJudging, ACostThatClearlyFallsDoesNotEarnAKeepOnItsOwn)
{
	// A GPU that spends 20 ms per frame and then 14, while the speed stays at 40% because something else is
	// still the limit: -30% on the cost the lever targets, +0.5% on the speed. Splitting work over more threads
	// (the GS back thread) lowers the busiest one's cost whether or not the game gets faster, and burns a big
	// core for it. The cost may keep the trial open for a longer look; it never makes the trial a keep. A trial
	// that ends flat with its cost down did not help.
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	for (int i = 0; i < 8; i++)
		rig.Feed(Gpu(40.0, 20.0));
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	EXPECT_NEAR(rig.log.back().cost_before, 20.0, 1e-9);
	rig.FeedFor(Gpu(40.0, 20.0), 1.5);

	for (int i = 0; i < 4 + 4 + 4 - 1; i++)
		EXPECT_EQ(rig.Feed(Gpu(40.2, 14.0)).kind, Kind::None) << "window " << i << ": the cost is down, so it is looked at for longer";
	const AutoTuneAction verdict = rig.Feed(Gpu(40.2, 14.0));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured) << "the speed is clearly flat: that is a measurement, and it is remembered";
	EXPECT_FALSE(verdict.inconclusive);
	EXPECT_NEAR(verdict.cost_gain, 0.3, 1e-9) << "the cost has its own figure";
	EXPECT_NEAR(verdict.gain, 0.005, 1e-9);
	EXPECT_EQ(rig.Count(Kind::Keep), 0u);
}

TEST(AutoTuneJudging, ACostThatFallsWhileTheSpeedRisesALittleIsNotKeptEither)
{
	// +3% on the speed is under the 5% bar and not clearly nothing either (the noise is 3%): the cost that fell
	// is no reason to keep it. Looked at for longer, then undone without being remembered.
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	for (int i = 0; i < 8; i++)
		rig.Feed(Gpu(40.0, 20.0));
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	rig.FeedFor(Gpu(40.0, 20.0), 1.5);

	AutoTuneAction verdict;
	for (int i = 0; i < 12; i++)
	{
		verdict = rig.Feed(Gpu(41.2, 14.0));
		EXPECT_NE(verdict.kind, Kind::Keep);
	}
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_FALSE(verdict.measured);
	EXPECT_TRUE(verdict.inconclusive);
}

TEST(AutoTuneJudging, ACostThatFallsAndASpeedThatClearlyRisesIsKeptOnTheSpeed)
{
	// The same lever when the game does get faster: +10% on the speed is the keep, and the cost is only reported.
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	for (int i = 0; i < 8; i++)
		rig.Feed(Gpu(40.0, 20.0));
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	rig.FeedFor(Gpu(40.0, 20.0), 1.5);

	AutoTuneAction verdict;
	for (int i = 0; i < 4; i++)
		verdict = rig.Feed(Gpu(44.0, 14.0));
	ASSERT_EQ(verdict.kind, Kind::Keep);
	EXPECT_NEAR(verdict.gain, 0.10, 1e-9);
	EXPECT_NEAR(verdict.cost_gain, 0.3, 1e-9);
}

TEST(AutoTuneJudging, ACostThatFallsWhileTheSpeedFallsAGoodDealIsRejectedAtTheFirstLook)
{
	// The cost fell 30% and the speed fell 15%: the lever made things worse. That is a measurement, and it is
	// remembered; it is not looked at for eight more windows with the player at the lower speed.
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	for (int i = 0; i < 8; i++)
		rig.Feed(Gpu(40.0, 20.0));
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	rig.FeedFor(Gpu(40.0, 20.0), 1.5);

	AutoTuneAction verdict;
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(rig.Feed(Gpu(34.0, 14.0)).kind, Kind::None);
	verdict = rig.Feed(Gpu(34.0, 14.0));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured);
	EXPECT_FALSE(verdict.inconclusive);
	EXPECT_NEAR(verdict.gain, -0.15, 1e-9);
}

TEST(AutoTuneJudging, ACostThatFallsWhileTheSpeedFallsALittleDidNotHelp)
{
	// The cost fell 30% and the speed fell 2.5%: under the bar for a loss, so the first look cannot call it
	// worse, and a cost on its own is no keep. It is looked at for longer, and when the speed is still not up it
	// did not help: the work moved between threads and the game is no faster.
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	for (int i = 0; i < 8; i++)
		rig.Feed(Gpu(40.0, 20.0));
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	rig.FeedFor(Gpu(40.0, 20.0), 1.5);

	AutoTuneAction verdict;
	for (int i = 0; i < 12; i++)
	{
		verdict = rig.Feed(Gpu(39.0, 14.0));
		EXPECT_NE(verdict.kind, Kind::Keep);
		if (i < 11)
			EXPECT_EQ(verdict.kind, Kind::None) << "window " << i;
	}
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured);
	EXPECT_FALSE(verdict.inconclusive);
}

TEST(AutoTuneJudging, TheCostThatIsJudgedFollowsTheBottleneck)
{
	// Without a GPU timer a GPU case is judged on the GS threads: the busier of MTGS and the back thread.
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	AutoTuneWindow w = Gpu(40.0);
	w.has_gpu_timer = false;
	w.gpu_pct = 0.0;
	w.ee_wait_gs_ratio = 0.5; // blocked on the GS, not CPU-bound there: the GPU case without a timer
	w.mtgs_pct = 70.0;
	w.mtgs_ms = 11.0;
	w.gs_back_ms = 6.0;
	for (int i = 0; i < 8; i++)
		rig.Feed(w);
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	EXPECT_NEAR(rig.log.back().cost_before, 11.0, 1e-9) << "MTGS, as it is the busier";

	// An EE case is judged on the EE thread.
	Rig ee;
	ee.state.iop_thread_placeable = true;
	ee.WarmUp();
	AutoTuneWindow e = EeBound(70.0);
	e.ee_ms = 15.0;
	e.mtgs_ms = 99.0; // nothing to do with it
	for (int i = 0; i < 8; i++)
		ee.Feed(e);
	ASSERT_EQ(ee.log.back().kind, Kind::StartTrial);
	EXPECT_NEAR(ee.log.back().cost_before, 15.0, 1e-9);
}

TEST(AutoTuneController, AResolutionTrialMeasuresAfterOneAndAHalfSeconds)
{
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	for (int i = 0; i < 8; i++)
		rig.Feed(Gpu(40.0));
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingTrial);

	rig.FeedFor(Gpu(40.0), 1.0);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingTrial);
	rig.Feed(Gpu(40.0));
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);
}

TEST(AutoTuneController, TheSettleAfterAKeepOrAnUndoIsTheLightOneForTheLightLevers)
{
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();
	for (int i = 0; i < 8; i++)
		rig.Feed(Gpu(40.0));
	rig.FeedFor(Gpu(40.0), 1.5);
	AutoTuneAction verdict;
	for (int i = 0; i < 4; i++)
		verdict = rig.Feed(Gpu(60.0));
	ASSERT_EQ(verdict.kind, Kind::Keep);
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingAfter);
	rig.FeedFor(Gpu(60.0), 1.0);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingAfter);
	rig.Feed(Gpu(60.0));
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Watching);
}

// A rejection is written into the game's file for good, so it has to be about the lever and not about the game
// having changed while the lever was tried. Two checks, each on a trial that would otherwise be a rejection: the
// first and the second half of its windows must agree on the speed, and a resource the lever does not act on must
// cost what it cost before.

namespace
{
	// A GPU-bound window with a figure for the EE's time per frame as well.
	AutoTuneWindow GpuWithEe(double speed, double ee_ms)
	{
		AutoTuneWindow w = Gpu(speed, 20.0);
		w.ee_ms = ee_ms;
		return w;
	}

	// The baseline, the start of a resolution trial from 6x, and its settle: the controller is ready to measure.
	void StartResolutionTrial(Rig& rig, double baseline_speed, double baseline_ee_ms)
	{
		OnlyResolutionIsLeft(rig, 6.0);
		rig.WarmUp();
		for (int i = 0; i < 8; i++)
			rig.Feed(GpuWithEe(baseline_speed, baseline_ee_ms));
		ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
		rig.FeedFor(GpuWithEe(baseline_speed, baseline_ee_ms), 1.5);
		ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);
	}
}

TEST(AutoTuneScene, ALossOnASteadySceneIsRejectedAndRemembered)
{
	Rig rig;
	StartResolutionTrial(rig, 60.0, 10.0);
	AutoTuneAction verdict;
	for (int i = 0; i < 4; i++)
		verdict = rig.Feed(GpuWithEe(50.0, 10.0));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured);
	EXPECT_FALSE(verdict.retry);
}

TEST(AutoTuneScene, ALossWhileTheEeCostMovedByATenthIsNotRemembered)
{
	// 60% to 50% with the EE's time per frame going from 10 to 13 ms: a resolution step does not touch the EE, so
	// the game got heavier. Undone, to be measured again, and not held back for the session.
	Rig rig;
	StartResolutionTrial(rig, 60.0, 10.0);
	AutoTuneAction verdict;
	for (int i = 0; i < 4; i++)
		verdict = rig.Feed(GpuWithEe(50.0, 13.0));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_FALSE(verdict.measured);
	EXPECT_TRUE(verdict.inconclusive);
	EXPECT_TRUE(verdict.retry);

	// A shift of 9% is inside what a steady scene does.
	Rig calm;
	StartResolutionTrial(calm, 60.0, 10.0);
	for (int i = 0; i < 4; i++)
		verdict = calm.Feed(GpuWithEe(50.0, 10.9));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured);
}

TEST(AutoTuneScene, TheResourceAGpuLeverDoesNotTouchIsTheEeAndAnEeLeversIsTheGs)
{
	// The EE lever: the GS thread's time moves 30% while the EE trial loses 17% of its speed.
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();
	AutoTuneWindow before = EeBound(60.0);
	before.ee_ms = 20.0;
	before.mtgs_ms = 10.0;
	for (int i = 0; i < 8; i++)
		rig.Feed(before);
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	rig.FeedFor(before, 6.0);

	AutoTuneWindow during = EeBound(50.0);
	during.ee_ms = 22.0;
	during.mtgs_ms = 13.0;
	AutoTuneAction verdict;
	for (int i = 0; i < 4; i++)
		verdict = rig.Feed(during);
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.retry) << "the GS thread has nothing to do with the IOP thread";

	// The EE's own time moving is the lever's business, not the scene's.
	Rig own;
	own.state.iop_thread_placeable = true;
	own.WarmUp();
	for (int i = 0; i < 8; i++)
		own.Feed(before);
	ASSERT_EQ(own.log.back().kind, Kind::StartTrial);
	own.FeedFor(before, 6.0);
	during.mtgs_ms = 10.0;
	for (int i = 0; i < 4; i++)
		verdict = own.Feed(during);
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured);
}

TEST(AutoTuneScene, TwoHalvesThatDisagreeAreNotAMeasuredRejection)
{
	// Eight windows with a median 23% under the baseline; the first half is at the baseline's level and the
	// second a third lower. Whatever happened, it was not one state of the game.
	Rig rig;
	StartResolutionTrial(rig, 60.0, 10.0);
	for (const double speed : {50.0, 70.0, 52.0, 68.0})
		EXPECT_EQ(rig.Feed(GpuWithEe(speed, 10.0)).kind, Kind::None) << "inconclusive: looked at for longer";
	AutoTuneAction verdict;
	for (int i = 0; i < 4; i++)
		verdict = rig.Feed(GpuWithEe(40.0, 10.0));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_FALSE(verdict.measured);
	EXPECT_TRUE(verdict.retry);
	EXPECT_LT(verdict.gain, -0.2);

	// The same median with all eight windows alike is a plain, remembered rejection.
	Rig steady;
	StartResolutionTrial(steady, 60.0, 10.0);
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(steady.Feed(GpuWithEe(46.0, 10.0)).kind, Kind::None);
	verdict = steady.Feed(GpuWithEe(46.0, 10.0));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured);
}

TEST(AutoTuneScene, FourWindowsAreTooFewToCompareHalves)
{
	// Two windows against two prove nothing; the scene is judged by its cost at the first look.
	Rig rig;
	StartResolutionTrial(rig, 60.0, 10.0);
	AutoTuneAction verdict;
	for (const double speed : {40.0, 41.0, 49.0, 50.0})
		verdict = rig.Feed(GpuWithEe(speed, 10.0));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured);
}

TEST(AutoTuneScene, TheEeGovernorMayStepDownAndBackWithinATrial)
{
	// Its windows are not one state by design (see AutoTunePolicy::governor_measure_windows), so the halves of
	// a governor trial are allowed to differ.
	Rig rig;
	rig.state.ee_governor_eligible = true;
	rig.WarmUp();
	for (int i = 0; i < 8; i++)
		rig.Feed(EeBound(60.0));
	ASSERT_EQ(rig.log.back().kind, Kind::StartTrial);
	ASSERT_EQ(rig.log.back().lever, AutoTuneLever::EeGovernor);
	// The settle of the governor: 6 s, and the 6.5 s of its own clocks at 60% speed, 16.8 s in all.
	for (int i = 0; i < 34; i++)
		rig.Feed(EeBound(60.0));
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Measuring);

	AutoTuneAction verdict;
	for (const double speed : {50.0, 70.0, 52.0, 68.0, 40.0, 40.0, 40.0, 40.0})
		verdict = rig.Feed(EeBound(speed));
	ASSERT_EQ(verdict.kind, Kind::Revert);
	EXPECT_TRUE(verdict.measured);
	EXPECT_FALSE(verdict.retry);
}

TEST(AutoTuneScene, AStepIsMeasuredAgainTwiceAndThenGivenUpWithoutBeingRememberedOrRetriedForever)
{
	Rig rig;
	OnlyResolutionIsLeft(rig, 6.0);
	rig.WarmUp();

	std::vector<AutoTuneAction> verdicts;
	for (int round = 0; round < 3; round++)
	{
		AutoTuneAction start;
		for (int i = 0; i < 8 && start.kind == Kind::None; i++)
			start = rig.Feed(GpuWithEe(60.0, 10.0));
		ASSERT_EQ(start.kind, Kind::StartTrial) << "round " << round;
		rig.FeedFor(GpuWithEe(60.0, 10.0), 1.5);
		AutoTuneAction verdict;
		for (int i = 0; i < 4; i++)
			verdict = rig.Feed(GpuWithEe(50.0, 13.0));
		ASSERT_EQ(verdict.kind, Kind::Revert) << "round " << round;
		verdicts.push_back(verdict);
		rig.FeedFor(GpuWithEe(60.0, 10.0), 1.5);
	}
	EXPECT_TRUE(verdicts[0].retry);
	EXPECT_TRUE(verdicts[1].retry);
	EXPECT_FALSE(verdicts[2].retry) << "two retries for one step";
	for (const AutoTuneAction& v : verdicts)
	{
		EXPECT_FALSE(v.measured);
		EXPECT_TRUE(v.inconclusive);
	}

	// Now the step is out for the session, like any inconclusive one: nothing is left to try.
	EXPECT_EQ(rig.Trigger(GpuWithEe(60.0, 10.0)).kind, Kind::Exhausted);
}

TEST(AutoTuneBaseline, ATrialIsComparedWithTheWindowsRightBeforeIt)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();
	for (const double speed : {60.0, 60.0, 60.0, 60.0})
		EXPECT_EQ(rig.Feed(EeBound(speed)).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Baselining);

	// The speed drifted while the trial was being prepared: the baseline is where it is now.
	for (const double speed : {80.0, 80.0, 80.0})
		EXPECT_EQ(rig.Feed(EeBound(speed)).kind, Kind::None);
	const AutoTuneAction start = rig.Feed(EeBound(80.0));
	ASSERT_EQ(start.kind, Kind::StartTrial);
	EXPECT_NEAR(start.speed_before, 70.0, 1e-9) << "the median of 60 60 60 60 80 80 80 80";
}

TEST(AutoTuneBaseline, AWindowThatIsFineCancelsThePreparation)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();
	for (int i = 0; i < 4; i++)
		rig.Feed(EeBound(60.0));
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Baselining);

	EXPECT_EQ(rig.Feed(Win(100.0)).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Watching);
	EXPECT_EQ(rig.ctl.BadStreak(), 0u);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 0u);

	// It has to come back for four bad windows and another four.
	EXPECT_EQ(rig.Trigger(EeBound(60.0)).kind, Kind::StartTrial);
}

TEST(AutoTuneBaseline, NoExtraWindowsStartsTheTrialAtOnce)
{
	AutoTunePolicy policy;
	policy.baseline_extra_windows = 0;
	Rig rig;
	rig.ctl = AutoTuneController(policy);
	rig.state.iop_thread_placeable = true;
	EXPECT_EQ(rig.WarmAndTrigger(EeBound(70.0)).kind, Kind::StartTrial);
}

TEST(AutoTuneBaseline, AReportDoesNotWaitForOne)
{
	Rig rig;
	rig.state.iop_thread_on = true;
	AutoTuneWindow w = Win(60.0);
	w.mtvu_pct = 96.0;
	EXPECT_EQ(rig.WarmAndTrigger(w).kind, Kind::ReportBottleneck);
}

TEST(AutoTuneBaseline, ASettingsChangeWhilePreparingDropsTheTrial)
{
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();
	for (int i = 0; i < 4; i++)
		rig.Feed(EeBound(60.0));
	ASSERT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Baselining);

	EXPECT_FALSE(rig.ctl.OnDisturbance(AutoTuneDisturbance::SettingsChange).has_value()) << "no trial was running to undo";
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::SettlingAfter);
}

TEST(AutoTuneBaseline, AStepThatIsNoLongerThereIsNotTried)
{
	// The player turned the IOP thread on while the baseline was being taken.
	Rig rig;
	rig.state.iop_thread_placeable = true;
	rig.WarmUp();
	for (int i = 0; i < 7; i++)
		rig.Feed(EeBound(60.0));
	rig.state.iop_thread_on = true;
	EXPECT_EQ(rig.Feed(EeBound(60.0)).kind, Kind::None);
	EXPECT_EQ(rig.ctl.GetPhase(), AutoTuneController::Phase::Watching);
}

namespace
{
	// A GPU-bound game with the blending at Maximum and a download mode that reads back.
	void BlendingAndDownloadAreOpen(Rig& rig, double resolution)
	{
		rig.state.hw_renderer = true;
		rig.state.resolution = resolution;
		rig.state.blending_level = 5;
		rig.state.download_mode = AutoTuneDownloadMode::Accurate;
		rig.state.gs_back_thread_allowed = true;
	}

	AutoTuneLever FirstLeverTried(double speed, const AutoTunePolicy& policy = AutoTunePolicy())
	{
		Rig rig;
		rig.ctl = AutoTuneController(policy);
		BlendingAndDownloadAreOpen(rig, 8.0);
		rig.WarmUp();
		return rig.Trigger(Gpu(speed)).lever;
	}
} // namespace

TEST(AutoTuneOrder, FarBelowFullSpeedTheBlendingGoesBeforeTheResolution)
{
	// 9% speed: a notch of resolution can give at most 1.3x at 8x, so the blending step goes first.
	EXPECT_EQ(FirstLeverTried(9.0), AutoTuneLever::Blending);
}

TEST(AutoTuneOrder, CloserToFullSpeedTheResolutionComesFirst)
{
	EXPECT_EQ(FirstLeverTried(70.0), AutoTuneLever::Resolution);
	EXPECT_EQ(FirstLeverTried(50.0), AutoTuneLever::Resolution) << "the line is at 50, and 50 is not under it";
}

TEST(AutoTuneOrder, TheLineIsAPolicyAndZeroTurnsItOff)
{
	AutoTunePolicy off;
	off.cheap_levers_first_below_pct = 0.0;
	EXPECT_EQ(FirstLeverTried(9.0, off), AutoTuneLever::Resolution);

	AutoTunePolicy high;
	high.cheap_levers_first_below_pct = 80.0;
	EXPECT_EQ(FirstLeverTried(70.0, high), AutoTuneLever::Blending);
}

TEST(AutoTuneOrder, FarBelowFullSpeedTheOrderIsBlendingDownloadResolutionBackThread)
{
	Rig rig;
	BlendingAndDownloadAreOpen(rig, 8.0);
	rig.state.blending_level = 2; // one step left to give that a GPU-bound game is offered: Medium to Basic
	rig.WarmUp();

	std::vector<AutoTuneLever> tried;
	for (int round = 0; round < 4; round++)
	{
		const AutoTuneAction start = rig.Trigger(Gpu(9.0));
		ASSERT_EQ(start.kind, Kind::StartTrial) << "round " << round;
		tried.push_back(start.lever);
		// The back thread reopens the renderer: its settle is the longer one, before and after.
		const double settle = start.lever == AutoTuneLever::GsBackThread ? 6.0 : 3.0;
		rig.FeedFor(Gpu(9.0), settle);
		const AutoTuneAction verdict = rig.RunUntilAction(Gpu(9.0));
		ASSERT_EQ(verdict.kind, Kind::Revert);
		rig.FeedFor(Gpu(9.0), settle);
	}
	const std::vector<AutoTuneLever> expected = {AutoTuneLever::Blending, AutoTuneLever::DownloadMode, AutoTuneLever::Resolution,
		AutoTuneLever::GsBackThread};
	EXPECT_EQ(tried, expected);
}

TEST(AutoTuneOrder, WithBasicBlendingThereIsNothingCheapToTryFirstSoTheResolutionGoesFirst)
{
	// 3x at 48% with blending Basic: the only blending step is Basic to Minimum, which is not a speed lever on a
	// GPU-bound game, so the resolution goes first.
	Rig rig;
	BlendingAndDownloadAreOpen(rig, 3.0);
	rig.state.blending_level = 1;
	rig.WarmUp();
	EXPECT_EQ(rig.Trigger(Gpu(48.0)).lever, AutoTuneLever::Resolution);

	// 5x at 45%: the same.
	Rig rig2;
	BlendingAndDownloadAreOpen(rig2, 5.0);
	rig2.state.blending_level = 1;
	rig2.WarmUp();
	EXPECT_EQ(rig2.Trigger(Gpu(44.6)).lever, AutoTuneLever::Resolution);
}

TEST(AutoTuneOrder, TheCheapLeversGoFirstOnlyWhereOneNotchCannotReachFullSpeed)
{
	// One notch from r removes at most (r / (r - 1))^2 of the pixels. The speed times that decides.
	const auto first = [](double resolution, double speed) {
		Rig rig;
		BlendingAndDownloadAreOpen(rig, resolution);
		rig.WarmUp();
		return rig.Trigger(Gpu(speed)).lever;
	};

	// 2x at 26%: 26 x 4 = 104, one notch gets there.
	EXPECT_EQ(first(2.0, 26.0), AutoTuneLever::Resolution);
	// 3x at 48%: 48 x 2.25 = 108.
	EXPECT_EQ(first(3.0, 48.0), AutoTuneLever::Resolution);
	// 8x at 9%: 9 x 1.31 = 12, nowhere near.
	EXPECT_EQ(first(8.0, 9.0), AutoTuneLever::Blending);
	// 4x at 40%: 40 x 1.78 = 71, not enough; 2x at 20%: 20 x 4 = 80.
	EXPECT_EQ(first(4.0, 40.0), AutoTuneLever::Blending);
	EXPECT_EQ(first(2.0, 20.0), AutoTuneLever::Blending);
}

TEST(AutoTuneOrder, AtNativeResolutionThereIsNoNotchSoTheCheapLeversGoFirst)
{
	Rig rig;
	BlendingAndDownloadAreOpen(rig, 1.0);
	rig.WarmUp();
	EXPECT_EQ(rig.Trigger(Gpu(30.0)).lever, AutoTuneLever::Blending);
}

TEST(AutoTuneOrder, TheNotchIsTheOneTheResolutionLeverWouldTake)
{
	// 2.5x steps to 2x, a ratio of 1.56 in pixels, not the 2.8 of a whole multiplier down to 1.5x.
	Rig rig;
	BlendingAndDownloadAreOpen(rig, 2.5);
	rig.WarmUp();
	EXPECT_EQ(rig.Trigger(Gpu(60.0 / 1.56 - 1.0)).lever, AutoTuneLever::Blending) << "37% x 1.5625 = 58%: not there";
}

TEST(AutoTuneOrder, ABackThreadStillGoesFirstWhenTheGsThreadsAreTheCause)
{
	Rig rig;
	BlendingAndDownloadAreOpen(rig, 8.0);
	rig.WarmUp();
	AutoTuneWindow w = GsCpuBound(9.0);
	const AutoTuneAction start = rig.Trigger(w);
	ASSERT_EQ(start.kind, Kind::StartTrial);
	EXPECT_EQ(start.lever, AutoTuneLever::GsBackThread);
}

// Replay of a recorded God of War II run at 8x with Maximum blending: 9% speed with the GPU at 100%. Walking
// the resolution down gains little (9.4% to 13.1%), while one blending step, Maximum to Full, takes the game
// to 100%.
//
// The speeds below are the recorded windows for each resolution, in order, transients and hitches included (the
// 6x series has its 10.8 window, the 5x series its first twelve windows of 12.6 ... 14.8 before it came down to
// the 6x figure). The GPU cost per frame of a game that is GPU-bound is the frame time. Full blending at 4x
// cost 28.5% of a 60 Hz frame (4.75 ms); at other resolutions it is taken to scale with the pixels, which is
// an assumption of this test and not a measurement.
namespace
{
	struct TraceWorld
	{
		int resolution = 8;
		int blending = 5;

		// Speeds per resolution at Maximum blending: the windows after a change, in order. The last eight are
		// repeated once they run out.
		static const std::vector<double>& Series(int resolution)
		{
			static const std::vector<double> r8 = {9.4, 9.4, 9.4, 9.3, 9.3, 9.4, 9.8, 9.9, 9.3, 9.2, 9.4, 9.3, 9.4, 9.2, 9.4, 9.4, 9.4, 9.3, 9.3, 9.2};
			static const std::vector<double> r7 = {9.7, 10.1, 10.0, 9.9, 10.0, 10.1, 10.1, 10.1, 10.2, 10.0, 10.1, 10.1, 10.1, 10.1, 10.2, 10.2, 10.1, 10.1};
			static const std::vector<double> r6 = {11.8, 11.9, 12.3, 12.3, 12.3, 12.2, 12.2, 12.2, 12.3, 12.3, 12.2, 12.3, 12.2, 12.1, 12.1, 12.2, 10.8, 12.0, 12.3};
			static const std::vector<double> r5 = {12.6, 13.1, 13.0, 12.6, 12.4, 12.4, 13.0, 12.4, 11.8, 12.9, 10.0, 14.8, 12.3, 12.3, 12.3, 12.2, 12.2, 12.1, 12.1, 12.2};
			static const std::vector<double> r4 = {13.1, 13.3, 13.2, 13.1, 13.1, 13.0, 13.2, 13.0, 13.0, 13.1, 13.1, 13.1, 13.2, 13.2, 13.1, 13.0, 13.4, 13.3};
			static const std::vector<double> r3 = {12.5, 12.3, 12.4, 12.4, 12.4, 12.4, 12.4, 12.4};
			switch (resolution)
			{
				case 8:
					return r8;
				case 7:
					return r7;
				case 6:
					return r6;
				case 5:
					return r5;
				case 4:
					return r4;
				default:
					return r3;
			}
		}

		void Set(int new_resolution, int new_blending)
		{
			if (new_resolution != resolution || new_blending != blending)
				index = 0;
			resolution = new_resolution;
			blending = new_blending;
		}

		AutoTuneWindow Next()
		{
			constexpr double kFrame60 = 1000.0 / 59.94;
			double speed;
			double gpu_ms;
			double mtgs_pct = 99.5;
			if (blending >= 5)
			{
				const std::vector<double>& series = Series(resolution);
				const size_t i = index < series.size() ? index : series.size() - 8 + (index - series.size()) % 8;
				speed = series[i];
				gpu_ms = kFrame60 * 100.0 / speed * 0.99;
				mtgs_pct = resolution >= 5 ? 99.5 : (resolution == 4 ? 70.0 : 55.0);
			}
			else
			{
				// Full blending: 4.75 ms at 4x, with the 28.1 ... 30.0% wobble of that run's windows.
				static const double wobble[] = {1.0, 1.02, 0.99, 1.0, 1.04, 1.01, 0.98, 1.0};
				gpu_ms = 4.75 * (resolution / 4.0) * (resolution / 4.0) * wobble[index % 8];
				const double frame_ms = std::max(kFrame60, gpu_ms);
				speed = kFrame60 / frame_ms * 100.0;
				mtgs_pct = 20.0;
			}
			index++;

			AutoTuneWindow w;
			w.usable = true;
			w.seconds = 0.5;
			w.speed_pct = speed;
			w.late_frame_ratio = speed < 99.0 ? 1.0 : 0.0;
			w.ee_pct = speed < 99.0 ? 2.0 : 27.7;
			w.mtgs_pct = mtgs_pct;
			w.gpu_pct = std::min(110.0, gpu_ms / (kFrame60 * 100.0 / speed) * 100.0);
			w.has_gpu_timer = true;
			w.gpu_ms = gpu_ms;
			w.mtgs_ms = kFrame60 * 100.0 / speed * mtgs_pct / 100.0;
			return w;
		}

		size_t index = 0;
	};

	struct Replay
	{
		TraceWorld world;
		AutoTuneController controller{RunWithThreeSecondSettle()};
		std::vector<AutoTuneAction> actions;
		std::vector<AutoTuneRejection> persisted; // what the game's file would remember
		std::vector<double> speeds;
		double now = 0.0;

		AutoTuneLeverState State() const
		{
			AutoTuneLeverState s;
			s.hw_renderer = true;
			s.resolution = world.resolution;
			s.blending_level = world.blending;
			s.download_mode = AutoTuneDownloadMode::Accurate;
			s.rejected = persisted;
			return s;
		}

		void Run(double seconds)
		{
			for (const double end = now + seconds; now < end; now += 0.5)
			{
				const AutoTuneWindow w = world.Next();
				speeds.push_back(w.speed_pct);
				const AutoTuneAction a = controller.OnWindow(w, State());
				if (a.kind == AutoTuneAction::Kind::None)
					continue;
				actions.push_back(a);

				if (a.kind == AutoTuneAction::Kind::StartTrial)
					Apply(a.lever, a.value);
				else if (a.kind == AutoTuneAction::Kind::Revert)
				{
					Undo(a.lever, a.value);
					if (a.measured)
						persisted.push_back({a.lever, a.value});
				}
			}
		}

		void Apply(AutoTuneLever lever, double value)
		{
			previous_resolution = world.resolution;
			previous_blending = world.blending;
			if (lever == AutoTuneLever::Resolution)
				world.Set(static_cast<int>(value), world.blending);
			else if (lever == AutoTuneLever::Blending)
				world.Set(world.resolution, static_cast<int>(value));
			// The download mode and the back thread did nothing in that run.
		}

		void Undo(AutoTuneLever lever, double)
		{
			if (lever == AutoTuneLever::Resolution || lever == AutoTuneLever::Blending)
				world.Set(previous_resolution, previous_blending);
		}

		int previous_resolution = 8;
		int previous_blending = 5;
	};

	size_t CountKind(const Replay& r, AutoTuneAction::Kind kind)
	{
		return static_cast<size_t>(std::count_if(r.actions.begin(), r.actions.end(), [&](const AutoTuneAction& a) { return a.kind == kind; }));
	}
} // namespace

TEST(AutoTuneReplay, TheGow2TraceFindsTheBlendingFirstAndKeepsNothingInsideTheNoise)
{
	Replay r;
	r.Run(150.0);

	// Under 50% speed the blending goes before the resolution: the first thing tried is Maximum to Full.
	const auto first = std::find_if(r.actions.begin(), r.actions.end(), [](const AutoTuneAction& a) { return a.kind == AutoTuneAction::Kind::StartTrial; });
	ASSERT_NE(first, r.actions.end());
	EXPECT_EQ(first->lever, AutoTuneLever::Blending);
	EXPECT_EQ(first->value, 4.0);

	const auto keep = std::find_if(r.actions.begin(), r.actions.end(), [](const AutoTuneAction& a) { return a.kind == AutoTuneAction::Kind::Keep; });
	ASSERT_NE(keep, r.actions.end());
	EXPECT_EQ(keep->lever, AutoTuneLever::Blending);
	EXPECT_GT(keep->speed_after, 80.0) << "9% to the high eighties in one step";
	EXPECT_GT(keep->cost_gain, 0.7) << "and the GPU's cost per frame fell by most of itself";

	// It goes on from there: a notch of resolution takes the rest of the way, and the game is at full speed.
	EXPECT_EQ(r.world.blending, 4);
	EXPECT_LE(r.world.resolution, 7);
	EXPECT_GE(r.speeds.back(), 99.0);

	// Nothing was kept or rejected on a difference inside the noise.
	for (const AutoTuneAction& a : r.actions)
	{
		if (a.kind == AutoTuneAction::Kind::Keep)
			EXPECT_GE(a.gain, 0.05) << AutoTuneLeverName(a.lever);
		if (a.kind == AutoTuneAction::Kind::Revert && a.measured)
			ADD_FAILURE() << "a rejection was recorded: " << AutoTuneLeverName(a.lever) << " " << a.value << " gain " << a.gain;
	}

	// Under a minute after the warm-up.
	size_t windows_to_full_speed = 0;
	while (windows_to_full_speed < r.speeds.size() && r.speeds[windows_to_full_speed] < 99.0)
		windows_to_full_speed++;
	EXPECT_LT(windows_to_full_speed * 0.5, 70.0) << "seconds from the start, 10 of them warm-up";
}

TEST(AutoTuneReplay, TheResolutionWalkStopsAtTheStepThatShowsNothingInsteadOfWalkingIntoTheNoise)
{
	// Only the resolution is left to try: the blending cannot go lower, and the download mode already does not
	// read back.
	struct ResolutionOnly
	{
		TraceWorld world;
		AutoTuneController controller{RunWithThreeSecondSettle()};
		std::vector<AutoTuneAction> actions;
		std::vector<AutoTuneRejection> persisted;
		int previous = 8;
		double now = 0.0;

		void Run(double seconds)
		{
			for (const double end = now + seconds; now < end; now += 0.5)
			{
				AutoTuneLeverState s;
				s.hw_renderer = true;
				s.resolution = world.resolution;
				s.blending_level = 0;
				s.download_mode = AutoTuneDownloadMode::NoReadbacks;
				s.rejected = persisted;
				const AutoTuneAction a = controller.OnWindow(world.Next(), s);
				if (a.kind == AutoTuneAction::Kind::None)
					continue;
				actions.push_back(a);
				if (a.kind == AutoTuneAction::Kind::StartTrial)
				{
					previous = world.resolution;
					world.Set(static_cast<int>(a.value), world.blending);
				}
				else if (a.kind == AutoTuneAction::Kind::Revert)
				{
					world.Set(previous, world.blending);
					if (a.measured)
						persisted.push_back({a.lever, a.value});
				}
			}
		}
	};

	ResolutionOnly walk;
	walk.Run(220.0);

	std::vector<std::string> story;
	for (const AutoTuneAction& a : walk.actions)
	{
		if (a.kind == AutoTuneAction::Kind::StartTrial)
			story.push_back("try " + std::to_string(static_cast<int>(a.value)));
		else if (a.kind == AutoTuneAction::Kind::Keep)
			story.push_back("keep");
		else if (a.kind == AutoTuneAction::Kind::Revert)
			story.push_back(a.measured ? "reject" : "undo");
		else if (a.kind == AutoTuneAction::Kind::Exhausted)
			story.push_back("exhausted");
	}

	// 8 to 7 (+7%) and 7 to 6 (+21%) are real; 6 to 5 is the same speed with a hitch in the numbers: the first
	// half of its windows are the transient after the change and the second half the steady figure, so the
	// scene check calls the trial unstable and measures the step again, twice, with the same transient each
	// time. It ends undone and not written down. Having found a step that shows nothing, the walk stops: it
	// does not go on to 4x and 3x on the noise.
	ASSERT_GE(story.size(), 7u);
	EXPECT_EQ(story[0], "try 7");
	EXPECT_EQ(story[1], "keep");
	EXPECT_EQ(story[2], "try 6");
	EXPECT_EQ(story[3], "keep");
	size_t next = 4;
	size_t tries_of_5 = 0;
	while (next + 1 < story.size() && story[next] == "try 5")
	{
		EXPECT_EQ(story[next + 1], "undo") << "never written down";
		tries_of_5++;
		next += 2;
	}
	EXPECT_EQ(tries_of_5, 3u) << "the trial and its two retries";
	ASSERT_LT(next, story.size());
	EXPECT_EQ(story[next], "exhausted");
	EXPECT_TRUE(walk.persisted.empty());
	EXPECT_EQ(walk.world.resolution, 6);
	for (const std::string& line : story)
		EXPECT_TRUE(line != "try 4" && line != "try 3") << line;
}
