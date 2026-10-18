// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AutoTuneSampler: turning a metrics window plus what the CPU thread counted over the same stretch (the limiter's
// overran frames, the time the EE spent blocked) into an AutoTuneWindow.

#include <gtest/gtest.h>

#include "AutoTune/AutoTuneSampler.h"

namespace
{
	PerformanceMetrics::WindowSnapshot Snap(uint32_t sequence)
	{
		PerformanceMetrics::WindowSnapshot s;
		s.sequence = sequence;
		s.seconds = 0.5f;
		s.speed = 100.0f;
		return s;
	}

	AutoTuneConditions Steady()
	{
		AutoTuneConditions c;
		c.vm_running = true;
		c.nominal_limiter = true;
		return c;
	}

	// The CPU thread's running totals at `time_s` on its own clock.
	AutoTuneTally Tally(double time_s, uint64_t frames = 0, uint64_t overran = 0, double gs_wait_s = 0.0, double vu1_wait_s = 0.0,
		double iop_wait_s = 0.0)
	{
		AutoTuneTally t;
		t.time_s = time_s;
		t.frames = frames;
		t.overran_frames = overran;
		t.waits_measured = true;
		t.gs_wait_s = gs_wait_s;
		t.vu1_wait_s = vu1_wait_s;
		t.iop_wait_s = iop_wait_s;
		return t;
	}

	// A sampler whose baseline is the start of time, as ResetLive() leaves it when a session begins.
	AutoTuneSampler Started()
	{
		AutoTuneSampler sampler;
		sampler.Reset(0, Tally(0.0));
		return sampler;
	}
} // namespace

TEST(AutoTuneSampler, DeliversEachWindowOnceBySequenceNumber)
{
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	EXPECT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(0.5), &w));
	EXPECT_FALSE(sampler.Poll(Snap(1), Steady(), Tally(0.6), &w)) << "the same window is not read twice";
	EXPECT_TRUE(sampler.Poll(Snap(2), Steady(), Tally(1.0), &w));
	EXPECT_EQ(sampler.LastSequence(), 2u);
}

TEST(AutoTuneSampler, NoWindowYetIsNotAWindow)
{
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	EXPECT_FALSE(sampler.Poll(Snap(0), Steady(), Tally(0.5), &w));
}

TEST(AutoTuneSampler, ResetTreatsTheCurrentWindowAsSeen)
{
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	sampler.Reset(7, Tally(10.0));
	EXPECT_FALSE(sampler.Poll(Snap(7), Steady(), Tally(10.2), &w));
	EXPECT_TRUE(sampler.Poll(Snap(8), Steady(), Tally(10.5), &w));
}

TEST(AutoTuneSampler, LateFramesAreTheShareOfFramesThatOverranBetweenDeliveries)
{
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;

	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(0.5, 30, 6), &w)); // 6 overran of 30
	EXPECT_NEAR(w.late_frame_ratio, 0.2, 1e-9);

	// The totals only grow: the next window is judged on what was added since the last one.
	ASSERT_TRUE(sampler.Poll(Snap(2), Steady(), Tally(1.0, 60, 6), &w));
	EXPECT_EQ(w.late_frame_ratio, 0.0);
}

TEST(AutoTuneSampler, NoLimitedFramesMeansNoLateFrames)
{
	// The limiter is bypassed (host-vsync pacing): nothing was counted, and that is not a late frame.
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(0.5, 0, 0), &w));
	EXPECT_EQ(w.late_frame_ratio, 0.0);
}

TEST(AutoTuneSampler, ADroppedWindowDoesNotCarryItsLateFramesForward)
{
	// Two windows closed between polls: the one delivered covers both, and the count starts over after it.
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(Snap(5), Steady(), Tally(1.0, 60, 3), &w));
	EXPECT_NEAR(w.late_frame_ratio, 0.05, 1e-9);
	ASSERT_TRUE(sampler.Poll(Snap(6), Steady(), Tally(1.5, 61, 3), &w));
	EXPECT_EQ(w.late_frame_ratio, 0.0);
}

TEST(AutoTuneSampler, WaitsAreAShareOfTheTimeBetweenTwoDeliveries)
{
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;

	// 0.15 s of 0.5 s on the GS, 0.025 s on VU1.
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(0.5, 30, 0, 0.15, 0.025), &w));
	ASSERT_TRUE(w.ee_wait_gs_ratio.has_value());
	EXPECT_NEAR(*w.ee_wait_gs_ratio, 0.3, 1e-9);
	ASSERT_TRUE(w.ee_wait_vu1_ratio.has_value());
	EXPECT_NEAR(*w.ee_wait_vu1_ratio, 0.05, 1e-9);

	// The next window starts from the totals the last one ended on.
	ASSERT_TRUE(sampler.Poll(Snap(2), Steady(), Tally(1.0, 60, 0, 0.20, 0.025), &w));
	EXPECT_NEAR(*w.ee_wait_gs_ratio, 0.1, 1e-9);
	EXPECT_NEAR(*w.ee_wait_vu1_ratio, 0.0, 1e-9);
}

TEST(AutoTuneSampler, TheEesWaitOnTheIopThreadIsAShareToo)
{
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;

	// The EE spent 0.2 s of 0.5 s spinning for the IOP thread to catch up or to stop for a hold.
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(0.5, 30, 0, 0.0, 0.0, 0.2), &w));
	ASSERT_TRUE(w.ee_wait_iop_ratio.has_value());
	EXPECT_NEAR(*w.ee_wait_iop_ratio, 0.4, 1e-9);

	// Without an IOP thread nothing is counted and the EE is not waiting on it.
	ASSERT_TRUE(sampler.Poll(Snap(2), Steady(), Tally(1.0, 60, 0, 0.0, 0.0, 0.2), &w));
	EXPECT_NEAR(*w.ee_wait_iop_ratio, 0.0, 1e-9);
}

TEST(AutoTuneSampler, IopTotalsThatStartedOverAreNotAnAnswer)
{
	// The IOP thread's counters start over when the thread is restarted, which a settings change can do.
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	sampler.Reset(0, Tally(5.0, 1000, 0, 1.0, 0.0, 3.0));
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(5.5, 1030, 0, 1.15, 0.0, 0.1), &w));
	EXPECT_NEAR(*w.ee_wait_gs_ratio, 0.3, 1e-9) << "the GS figure is unaffected";
	EXPECT_FALSE(w.ee_wait_iop_ratio.has_value());
}

TEST(AutoTuneSampler, TheDenominatorIsTheCpuThreadsOwnSpanNotTheMetricsWindow)
{
	// The metrics window is closed on the GS thread and says 0.5 s; the CPU thread read its totals 0.6 s
	// apart. The wait is over the CPU thread's stretch, so it is divided by that.
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(0.6, 30, 0, 0.15), &w));
	EXPECT_NEAR(*w.ee_wait_gs_ratio, 0.25, 1e-9);
}

TEST(AutoTuneSampler, AWaitCannotExceedTheTimeItHappenedIn)
{
	// A wait that began before the baseline reading and ended after it can put a little more than the
	// span into the totals.
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(0.5, 30, 0, 0.52), &w));
	EXPECT_EQ(*w.ee_wait_gs_ratio, 1.0);
}

TEST(AutoTuneSampler, WaitsThatAreNotMeasuredStayUnknown)
{
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	AutoTuneTally now = Tally(0.5, 30, 0, 0.1);
	now.waits_measured = false; // no tally was being kept
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), now, &w));
	EXPECT_FALSE(w.ee_wait_gs_ratio.has_value());
	EXPECT_FALSE(w.ee_wait_vu1_ratio.has_value());
	EXPECT_FALSE(w.ee_wait_iop_ratio.has_value());
}

TEST(AutoTuneSampler, WithoutABaselineTheFirstWindowHasNoWaits)
{
	// A sampler that was never started has no earlier reading to take a difference against.
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(100.0, 5000, 100, 40.0), &w));
	EXPECT_FALSE(w.ee_wait_gs_ratio.has_value());
	EXPECT_EQ(w.late_frame_ratio, 0.0);

	// The reading it just took is the baseline for the next.
	ASSERT_TRUE(sampler.Poll(Snap(2), Steady(), Tally(100.5, 5030, 106, 40.15), &w));
	EXPECT_NEAR(*w.ee_wait_gs_ratio, 0.3, 1e-9);
	EXPECT_NEAR(w.late_frame_ratio, 0.2, 1e-9);
}

TEST(AutoTuneSampler, ASpanMuchLongerThanTheWindowSaysNothingAboutItsWaits)
{
	// A pause sat between the two readings: the totals cover seconds of nothing, and a share of them would
	// read as an idle EE. The frame counts are still a ratio and stay.
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(30.0, 30, 3, 0.2), &w));
	EXPECT_FALSE(w.ee_wait_gs_ratio.has_value());
	EXPECT_FALSE(w.ee_wait_vu1_ratio.has_value());
	EXPECT_NEAR(w.late_frame_ratio, 0.1, 1e-9);
}

TEST(AutoTuneSampler, TotalsThatWentBackwardsAreNotAnAnswer)
{
	// The counters start over with a new VM.
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	sampler.Reset(0, Tally(5.0, 1000, 10, 2.0));
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), Tally(5.5, 20, 1, 0.1), &w));
	EXPECT_FALSE(w.ee_wait_gs_ratio.has_value());
	EXPECT_EQ(w.late_frame_ratio, 0.0);
}

TEST(AutoTuneSampler, CopiesTheThreadFigures)
{
	PerformanceMetrics::WindowSnapshot s = Snap(1);
	s.speed = 87.0f;
	s.ee_usage = 91.0;
	s.gs_usage = 62.0f;
	s.gs_back_usage = 44.0f;
	s.has_gs_back_thread = true;
	s.vu_usage = 33.0f;
	s.iop_usage = 21.0f;
	s.has_iop_thread = true;
	s.gpu_usage = 77.0f;
	s.has_gpu_timer = true;

	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(s, Steady(), Tally(0.5, 30, 0, 0.15, 0.025), &w));
	EXPECT_TRUE(w.usable);
	EXPECT_DOUBLE_EQ(w.seconds, 0.5);
	EXPECT_NEAR(w.speed_pct, 87.0, 1e-4);
	EXPECT_DOUBLE_EQ(w.ee_pct, 91.0);
	EXPECT_DOUBLE_EQ(w.mtgs_pct, 62.0);
	EXPECT_DOUBLE_EQ(w.gs_back_pct, 44.0);
	EXPECT_DOUBLE_EQ(w.mtvu_pct, 33.0);
	EXPECT_DOUBLE_EQ(w.iop_pct, 21.0);
	EXPECT_DOUBLE_EQ(w.gpu_pct, 77.0);
	EXPECT_TRUE(w.has_gpu_timer);
	ASSERT_TRUE(w.ee_wait_gs_ratio.has_value());
	EXPECT_DOUBLE_EQ(*w.ee_wait_gs_ratio, 0.3);
	ASSERT_TRUE(w.ee_wait_vu1_ratio.has_value());
	EXPECT_DOUBLE_EQ(*w.ee_wait_vu1_ratio, 0.05);
}

TEST(AutoTuneSampler, CopiesWhatEachThreadCostPerFrame)
{
	PerformanceMetrics::WindowSnapshot s = Snap(1);
	s.ee_ms = 12.5f;
	s.gs_ms = 16.0f;
	s.gs_back_ms = 7.5f;
	s.has_gs_back_thread = true;
	s.gpu_ms = 9.25f;
	s.has_gpu_timer = true;

	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(s, Steady(), Tally(0.5), &w));
	EXPECT_DOUBLE_EQ(w.ee_ms, 12.5);
	EXPECT_DOUBLE_EQ(w.mtgs_ms, 16.0);
	EXPECT_DOUBLE_EQ(w.gs_back_ms, 7.5);
	EXPECT_DOUBLE_EQ(w.gpu_ms, 9.25);
}

TEST(AutoTuneSampler, ACostOfAThreadOrTimerThatIsNotThereReadsZero)
{
	PerformanceMetrics::WindowSnapshot s = Snap(1);
	s.gs_back_ms = 7.5f; // stale figure with no thread registered
	s.has_gs_back_thread = false;
	s.gpu_ms = 9.25f; // stale figure with no timer
	s.has_gpu_timer = false;

	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(s, Steady(), Tally(0.5), &w));
	EXPECT_EQ(w.gs_back_ms, 0.0);
	EXPECT_EQ(w.gpu_ms, 0.0);
}

TEST(AutoTuneSampler, AThreadThatDoesNotExistReadsZero)
{
	PerformanceMetrics::WindowSnapshot s = Snap(1);
	s.gs_back_usage = 55.0f; // stale figure with no thread registered
	s.iop_usage = 55.0f;
	s.has_gs_back_thread = false;
	s.has_iop_thread = false;

	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(s, Steady(), Tally(0.5), &w));
	EXPECT_EQ(w.gs_back_pct, 0.0);
	EXPECT_EQ(w.iop_pct, 0.0);
}

TEST(AutoTuneSampler, UsableOnlyInSteadyPlay)
{
	const auto usable = [](auto&& tweak) {
		AutoTuneConditions c = Steady();
		tweak(c);
		AutoTuneSampler sampler = Started();
		AutoTuneWindow w;
		EXPECT_TRUE(sampler.Poll(Snap(1), c, Tally(0.5), &w));
		return w.usable;
	};

	EXPECT_TRUE(usable([](AutoTuneConditions&) {}));
	EXPECT_FALSE(usable([](AutoTuneConditions& c) { c.vm_running = false; })) << "paused";
	EXPECT_FALSE(usable([](AutoTuneConditions& c) { c.nominal_limiter = false; })) << "turbo, slow-motion, unlimited";
	EXPECT_FALSE(usable([](AutoTuneConditions& c) { c.frame_advancing = true; }));
	EXPECT_FALSE(usable([](AutoTuneConditions& c) { c.gs_dump = true; }));
	EXPECT_FALSE(usable([](AutoTuneConditions& c) { c.target_speed = 0.0; })) << "no frame limit";
}

TEST(AutoTuneSampler, SpeedIsRelativeToTheLimitersTarget)
{
	// The player's nominal speed is 150%: running exactly on target reads 100.
	AutoTuneConditions c = Steady();
	c.target_speed = 1.5;
	PerformanceMetrics::WindowSnapshot s = Snap(1);
	s.speed = 150.0f;

	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(s, c, Tally(0.5), &w));
	EXPECT_NEAR(w.speed_pct, 100.0, 1e-4);

	s.sequence = 2;
	s.speed = 120.0f;
	ASSERT_TRUE(sampler.Poll(s, c, Tally(1.0), &w));
	EXPECT_NEAR(w.speed_pct, 80.0, 1e-4);
}

TEST(AutoTuneSampler, AWindowThatIsNotUsableStillConsumesItsLateFrames)
{
	// A pause must not leave its late frames to be blamed on the first window after it.
	AutoTuneSampler sampler = Started();
	AutoTuneWindow w;
	AutoTuneConditions paused = Steady();
	paused.vm_running = false;
	ASSERT_TRUE(sampler.Poll(Snap(1), paused, Tally(0.5, 30, 30), &w));
	EXPECT_FALSE(w.usable);

	ASSERT_TRUE(sampler.Poll(Snap(2), Steady(), Tally(1.0, 60, 30), &w));
	EXPECT_EQ(w.late_frame_ratio, 0.0);
}
