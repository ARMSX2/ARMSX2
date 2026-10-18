// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AutoTuneSampler: turning a metrics window plus the limiter's late-frame count into an AutoTuneWindow.

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
} // namespace

TEST(AutoTuneSampler, DeliversEachWindowOnceBySequenceNumber)
{
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	EXPECT_TRUE(sampler.Poll(Snap(1), Steady(), {}, &w));
	EXPECT_FALSE(sampler.Poll(Snap(1), Steady(), {}, &w)) << "the same window is not read twice";
	EXPECT_TRUE(sampler.Poll(Snap(2), Steady(), {}, &w));
	EXPECT_EQ(sampler.LastSequence(), 2u);
}

TEST(AutoTuneSampler, NoWindowYetIsNotAWindow)
{
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	EXPECT_FALSE(sampler.Poll(Snap(0), Steady(), {}, &w));
}

TEST(AutoTuneSampler, ResetTreatsTheCurrentWindowAsSeen)
{
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	sampler.Reset(7);
	EXPECT_FALSE(sampler.Poll(Snap(7), Steady(), {}, &w));
	EXPECT_TRUE(sampler.Poll(Snap(8), Steady(), {}, &w));
}

TEST(AutoTuneSampler, LateFramesAreCountedPerWindowAndStartOverAfterEach)
{
	AutoTuneSampler sampler;
	AutoTuneWindow w;

	for (int i = 0; i < 30; i++)
		sampler.NoteLimitedFrame(i < 6); // 6 late of 30
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), {}, &w));
	EXPECT_NEAR(w.late_frame_ratio, 0.2, 1e-9);

	for (int i = 0; i < 30; i++)
		sampler.NoteLimitedFrame(false);
	ASSERT_TRUE(sampler.Poll(Snap(2), Steady(), {}, &w));
	EXPECT_EQ(w.late_frame_ratio, 0.0);
}

TEST(AutoTuneSampler, NoLimitedFramesMeansNoLateFrames)
{
	// The limiter is bypassed (host-vsync pacing): nothing was counted, and that is not a late frame.
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), {}, &w));
	EXPECT_EQ(w.late_frame_ratio, 0.0);
}

TEST(AutoTuneSampler, ADroppedWindowDoesNotCarryItsLateFramesForward)
{
	// Two windows closed between polls: the one delivered covers both, and the count starts over after it.
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	for (int i = 0; i < 60; i++)
		sampler.NoteLimitedFrame(i < 3);
	ASSERT_TRUE(sampler.Poll(Snap(5), Steady(), {}, &w));
	EXPECT_NEAR(w.late_frame_ratio, 0.05, 1e-9);
	sampler.NoteLimitedFrame(false);
	ASSERT_TRUE(sampler.Poll(Snap(6), Steady(), {}, &w));
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

	AutoTuneEeWaits waits;
	waits.gs = 0.3;
	waits.vu1 = 0.05;

	AutoTuneSampler sampler;
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(s, Steady(), waits, &w));
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

TEST(AutoTuneSampler, AThreadThatDoesNotExistReadsZero)
{
	PerformanceMetrics::WindowSnapshot s = Snap(1);
	s.gs_back_usage = 55.0f; // stale figure with no thread registered
	s.iop_usage = 55.0f;
	s.has_gs_back_thread = false;
	s.has_iop_thread = false;

	AutoTuneSampler sampler;
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(s, Steady(), {}, &w));
	EXPECT_EQ(w.gs_back_pct, 0.0);
	EXPECT_EQ(w.iop_pct, 0.0);
}

TEST(AutoTuneSampler, WaitsThatAreNotMeasuredStayUnknown)
{
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(Snap(1), Steady(), AutoTuneSampleEeWaits(0.5), &w));
	EXPECT_FALSE(w.ee_wait_gs_ratio.has_value());
	EXPECT_FALSE(w.ee_wait_vu1_ratio.has_value());
}

TEST(AutoTuneSampler, UsableOnlyInSteadyPlay)
{
	const auto usable = [](auto&& tweak) {
		AutoTuneConditions c = Steady();
		tweak(c);
		AutoTuneSampler sampler;
		AutoTuneWindow w;
		EXPECT_TRUE(sampler.Poll(Snap(1), c, {}, &w));
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

	AutoTuneSampler sampler;
	AutoTuneWindow w;
	ASSERT_TRUE(sampler.Poll(s, c, {}, &w));
	EXPECT_NEAR(w.speed_pct, 100.0, 1e-4);

	s.sequence = 2;
	s.speed = 120.0f;
	ASSERT_TRUE(sampler.Poll(s, c, {}, &w));
	EXPECT_NEAR(w.speed_pct, 80.0, 1e-4);
}

TEST(AutoTuneSampler, AWindowThatIsNotUsableStillConsumesItsLateFrames)
{
	// A pause must not leave its late frames to be blamed on the first window after it.
	AutoTuneSampler sampler;
	AutoTuneWindow w;
	for (int i = 0; i < 30; i++)
		sampler.NoteLimitedFrame(true);
	AutoTuneConditions paused = Steady();
	paused.vm_running = false;
	ASSERT_TRUE(sampler.Poll(Snap(1), paused, {}, &w));
	EXPECT_FALSE(w.usable);

	ASSERT_TRUE(sampler.Poll(Snap(2), Steady(), {}, &w));
	EXPECT_EQ(w.late_frame_ratio, 0.0);
}
