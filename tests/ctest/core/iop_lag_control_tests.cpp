// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// ReplyMonitor (IopThread/IopLagControl.h): how late the EE takes what the IOP produced, and whether the EE was idle across the
// lateness. The monitor reads no global, so these tests drive it with event tests and replies at chosen EE cycles.

#include <gtest/gtest.h>

#include "IopThread/IopLagControl.h"

using namespace IopThread;

namespace
{
	constexpr u64 kStep = 3072; // the EE's event-test spacing
	constexpr u64 kBase = 1'000'000;

	// Runs event tests from `from` (exclusive; the test at `from` is assumed done) up to and including `to`, every kStep cycles.
	// Returns the cycle of the last one.
	u64 Tick(ReplyMonitor& m, u64 from, u64 to, bool idle, u32 frame = 1)
	{
		u64 c = from;
		while (c < to)
		{
			c = std::min(c + kStep, to);
			m.OnEventTest(c, idle, frame);
		}
		return c;
	}

	u32 Stamp(u64 t) { return static_cast<u32>(t); }
} // namespace

TEST(ReplyMonitor, AReplyTakenInAnIdleRunItWasProducedInIsAnOvershoot)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 t = kBase + 10 * kStep;
	Tick(m, kBase, t + 40 * kStep, true); // the EE idles across the production and for a long time after
	const u64 now = kBase + 50 * kStep;
	ASSERT_GT(now - t, static_cast<u64>(kOvershootMin));
	const ReplyMonitor::Verdict v = m.OnReply(ReplyKind::SbusIrq, Stamp(t), now, true);
	EXPECT_EQ(v.late, now - t);
	EXPECT_TRUE(v.overshoot);
	const ReplyStats& s = m.Stats();
	EXPECT_EQ(s.replies, 1u);
	EXPECT_EQ(s.late, 1u);
	EXPECT_EQ(s.idle_overshoots, 1u);
	EXPECT_EQ(s.idle_overshoots_unbroken, 1u);
	EXPECT_EQ(s.idle_overshoots_90, 1u);
	EXPECT_EQ(s.idle_overshoot_cycles, now - t);
	EXPECT_EQ(s.idle_overshoot_max, now - t);
	EXPECT_EQ(s.overshoot[ReplyKind::SbusIrq].count, 1u);
	EXPECT_EQ(s.all[ReplyKind::SbusIrq].count, 1u);
	EXPECT_EQ(s.late_ee_busy, 0u);
	EXPECT_EQ(s.late_idle_light, 0u);
}

TEST(ReplyMonitor, AReplyTakenWhileTheEeRunsIsLateButNotAnOvershoot)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 t = kBase + 10 * kStep;
	Tick(m, kBase, t + 40 * kStep - kStep, true);
	m.OnEventTest(t + 40 * kStep, false, 1); // the EE ran into this event test
	const ReplyMonitor::Verdict v = m.OnReply(ReplyKind::SbusIrq, Stamp(t), t + 40 * kStep, true);
	EXPECT_FALSE(v.overshoot);
	EXPECT_EQ(m.Stats().late, 1u);
	EXPECT_EQ(m.Stats().late_ee_busy, 1u);
	EXPECT_EQ(m.Stats().idle_overshoots, 0u);
}

TEST(ReplyMonitor, AReplyTakenOutsideAnEventTestIsNeverAnOvershoot)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 t = kBase + 10 * kStep;
	const u64 now = Tick(m, kBase, t + 40 * kStep, true);
	const ReplyMonitor::Verdict v = m.OnReply(ReplyKind::Call, Stamp(t), now, false); // a hold on the guest's own access to IOP space
	EXPECT_FALSE(v.overshoot);
	EXPECT_EQ(m.Stats().late_ee_busy, 1u);
}

TEST(ReplyMonitor, AnEeThatRanMostOfTheTimeSinceTheReplyWasProducedIsLightNotAnOvershoot)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 t = kBase + 10 * kStep;
	Tick(m, kBase, t, false);
	Tick(m, t, t + 40 * kStep, false); // 40 event tests of the EE running ...
	const u64 now = Tick(m, t + 40 * kStep, t + 44 * kStep, true); // ... then 4 idle ones: 10 percent
	const ReplyMonitor::Verdict v = m.OnReply(ReplyKind::SbusIrq, Stamp(t), now, true);
	ASSERT_GE(now - t, static_cast<u64>(kOvershootMin));
	EXPECT_FALSE(v.overshoot);
	EXPECT_EQ(m.Stats().late_idle_light, 1u);
	EXPECT_EQ(m.Stats().idle_overshoots_unbroken, 0u);
	EXPECT_EQ(m.Stats().idle_overshoots_90, 0u);
}

TEST(ReplyMonitor, ABriefInterruptionOfTheIdleMakesItBrokenButStillAnOvershootByShare)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 t = kBase + 4 * kStep;
	Tick(m, kBase, t + 20 * kStep, true);
	m.OnEventTest(t + 21 * kStep, false, 1); // one event test the EE ran into: an interrupt handler
	const u64 now = Tick(m, t + 21 * kStep, t + 80 * kStep, true);
	const ReplyMonitor::Verdict v = m.OnReply(ReplyKind::Call, Stamp(t), now, true);
	EXPECT_TRUE(v.overshoot) << "about 98 percent of the lateness was idle";
	EXPECT_EQ(m.Stats().idle_overshoots, 1u);
	EXPECT_EQ(m.Stats().idle_overshoots_90, 1u);
	EXPECT_EQ(m.Stats().idle_overshoots_unbroken, 0u) << "the streak began after the production";
}

TEST(ReplyMonitor, AReplyUnderTheMinimumIsCountedAndBinnedButNeverLate)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 now = Tick(m, kBase, kBase + 20 * kStep, true);
	const u64 t = now - (kOvershootMin - 1);
	const ReplyMonitor::Verdict v = m.OnReply(ReplyKind::SbusIrq, Stamp(t), now, true);
	EXPECT_FALSE(v.overshoot);
	EXPECT_EQ(v.late, static_cast<u64>(kOvershootMin) - 1);
	EXPECT_EQ(m.Stats().replies, 1u);
	EXPECT_EQ(m.Stats().late, 0u);
	EXPECT_EQ(m.Stats().all[ReplyKind::SbusIrq].count, 1u);
	EXPECT_EQ(m.Stats().all[ReplyKind::SbusIrq].bucket[LateBucket(kOvershootMin - 1)], 1u);
}

TEST(ReplyMonitor, AReplyAtExactlyTheMinimumIsLate)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 now = Tick(m, kBase, kBase + 40 * kStep, true);
	const u64 t = now - kOvershootMin;
	EXPECT_TRUE(m.OnReply(ReplyKind::SbusIrq, Stamp(t), now, true).overshoot);
}

TEST(ReplyMonitor, AReplyFromTheIopsFutureIsNotLate)
{
	// The IOP can stamp a time ahead of the EE's cycle (a state loaded under it); the lateness is then zero, not a huge unsigned number.
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const ReplyMonitor::Verdict v = m.OnReply(ReplyKind::SbusIrq, Stamp(kBase + 5000), kBase, true);
	EXPECT_EQ(v.late, 0u);
	EXPECT_EQ(m.Stats().late, 0u);
}

TEST(ReplyMonitor, TheLatenessIsAModularDifferenceAcrossTheWrapOf32Bits)
{
	ReplyMonitor m;
	const u64 base = 0x1'0000'0000ull - 20 * kStep;
	m.OnEventTest(base, false, 1);
	const u64 t = base + 2 * kStep; // below 2^32
	const u64 now = Tick(m, base, base + 60 * kStep, true); // above 2^32
	ASSERT_GT(now, 0x1'0000'0000ull);
	const ReplyMonitor::Verdict v = m.OnReply(ReplyKind::SbusIrq, Stamp(t), now, true);
	EXPECT_EQ(v.late, now - t);
	EXPECT_TRUE(v.overshoot);
}

TEST(ReplyMonitor, TheIdleShareOfEveryLateReplyIsBinnedInTenths)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 t = kBase;
	const u64 now = Tick(m, kBase, kBase + 60 * kStep, true);
	m.OnReply(ReplyKind::SbusIrq, Stamp(t), now, true);
	EXPECT_EQ(m.Stats().coverage_decile[10], 1u);
	// Another, produced after 45 busy event tests of the same window: 25 percent idle.
	ReplyMonitor m2;
	m2.OnEventTest(kBase, false, 1);
	const u64 mid = Tick(m2, kBase, kBase + 45 * kStep, false);
	const u64 end = Tick(m2, mid, mid + 15 * kStep, true);
	m2.OnReply(ReplyKind::SbusIrq, Stamp(kBase), end, true);
	EXPECT_EQ(m2.Stats().coverage_decile[2], 1u);
}

TEST(ReplyMonitor, FramesAreBinnedByTheirOvershootsAndANewFrameIsReported)
{
	ReplyMonitor m;
	u64 c = kBase;
	m.OnEventTest(c, false, 10);
	const auto frame = [&](u32 f, u32 events) {
		const u64 start = c;
		c = Tick(m, c, c + 60 * kStep, true, f);
		for (u32 i = 0; i < events; i++)
			m.OnReply(ReplyKind::SbusIrq, Stamp(start), c, true);
	};
	frame(10, 0);
	frame(11, 1);
	frame(12, 2);
	frame(13, 4);
	frame(14, 7);
	frame(15, 12);
	EXPECT_TRUE(m.OnEventTest(c + kStep, true, 16)) << "a frame number that changed begins a new frame";
	EXPECT_FALSE(m.OnEventTest(c + 2 * kStep, true, 16));
	const ReplyStats& s = m.Stats();
	EXPECT_EQ(s.frames, 6u);
	EXPECT_EQ(s.frames_with_overshoot, 5u);
	EXPECT_EQ(s.frame_bins[0], 1u);
	EXPECT_EQ(s.frame_bins[1], 1u);
	EXPECT_EQ(s.frame_bins[2], 1u);
	EXPECT_EQ(s.frame_bins[3], 1u);
	EXPECT_EQ(s.frame_bins[4], 1u);
	EXPECT_EQ(s.frame_bins[5], 1u);
}

TEST(ReplyMonitor, TheTraceKeepsOneRecordPerCompletedFrameWhenAsked)
{
	ReplyMonitor m;
	m.EnableTrace(true);
	u64 c = kBase;
	m.OnEventTest(c, false, 1);
	c = Tick(m, c, c + 60 * kStep, true, 1);
	m.OnReply(ReplyKind::SbusIrq, Stamp(kBase), c, true);
	c = Tick(m, c, c + 5 * kStep, false, 2);
	m.OnEventTest(c + kStep, false, 3);
	ASSERT_EQ(m.Trace().size(), 2u);
	EXPECT_EQ(m.Trace()[0].frame, 1u);
	EXPECT_EQ(m.Trace()[0].idle_overshoots, 1u);
	EXPECT_EQ(m.Trace()[0].replies, 1u);
	EXPECT_GT(m.Trace()[0].ff_cycles, 0u);
	EXPECT_EQ(m.Trace()[1].frame, 2u);
	EXPECT_EQ(m.Trace()[1].idle_overshoots, 0u);
	EXPECT_EQ(m.Trace()[1].ff_cycles, 0u);
}

TEST(ReplyMonitor, TheLedgerSumsTheIdleIntervalsAndNothingElse)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	Tick(m, kBase, kBase + 10 * kStep, true);
	Tick(m, kBase + 10 * kStep, kBase + 15 * kStep, false);
	Tick(m, kBase + 15 * kStep, kBase + 17 * kStep, true);
	EXPECT_EQ(m.IdleCycles(), 12 * kStep);
	EXPECT_EQ(m.Stats().ff_cycles, 12 * kStep);
	EXPECT_EQ(m.Stats().ee_cycles, 17 * kStep);
	EXPECT_EQ(m.Stats().event_tests, 18u);
	EXPECT_EQ(m.Stats().ff_event_tests, 12u);
}

TEST(ReplyMonitor, AClockThatGoesBackBreaksTheStreakAndKeepsGoing)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	Tick(m, kBase, kBase + 10 * kStep, true);
	m.OnEventTest(kBase - 500000, false, 1); // a state was loaded
	EXPECT_FALSE(m.LastEventTestWasIdle());
	Tick(m, kBase - 500000, kBase - 500000 + 60 * kStep, true);
	const u64 now = kBase - 500000 + 60 * kStep;
	EXPECT_TRUE(m.OnReply(ReplyKind::SbusIrq, Stamp(kBase - 500000 + kStep), now, true).overshoot);
}

TEST(ReplyMonitor, ResetClearsTheBooksAndKeepsTheTraceSetting)
{
	ReplyMonitor m;
	m.EnableTrace(true);
	m.OnEventTest(kBase, false, 1);
	const u64 now = Tick(m, kBase, kBase + 60 * kStep, true);
	m.OnReply(ReplyKind::SbusIrq, Stamp(kBase), now, true);
	ASSERT_EQ(m.Stats().idle_overshoots, 1u);
	m.Reset();
	EXPECT_EQ(m.Stats().replies, 0u);
	EXPECT_EQ(m.Stats().idle_overshoots, 0u);
	EXPECT_EQ(m.IdleCycles(), 0u);
	m.OnEventTest(kBase, false, 1);
	Tick(m, kBase, kBase + 60 * kStep, true, 2);
	m.OnEventTest(kBase + 61 * kStep, false, 3);
	EXPECT_FALSE(m.Trace().empty()) << "the trace setting survived the reset";
}

TEST(ReplyMonitor, TheLateHistogramBucketsFollowTheEdges)
{
	EXPECT_EQ(LateBucket(0), 0u);
	EXPECT_EQ(LateBucket(1023), 0u);
	EXPECT_EQ(LateBucket(1024), 1u);
	EXPECT_EQ(LateBucket(kOvershootMin - 1), LateBucket(kLateEdges[5] - 1));
	EXPECT_EQ(LateBucket(kOvershootMin), 6u);
	EXPECT_EQ(LateBucket(4'915'199), kLateBuckets - 2);
	EXPECT_EQ(LateBucket(4'915'200), kLateBuckets - 1);
	EXPECT_EQ(LateBucket(~u64(0)), kLateBuckets - 1);
	LateHist h;
	h.Add(10);
	h.Add(4'000'000);
	h.Add(30);
	EXPECT_EQ(h.count, 3u);
	EXPECT_EQ(h.max, 4'000'000u);
	EXPECT_DOUBLE_EQ(h.Mean(), (10 + 4'000'000 + 30) / 3.0);
	EXPECT_EQ(h.bucket[0], 2u);
}

TEST(ReplyMonitor, TheOvershootMinimumIsAboveWhatTheDefaultBoundLetsAReplyBeLateBy)
{
	// L plus two event-test spacings and the IOP's block overshoot.
	EXPECT_GT(kOvershootMin, kDefaultLag + 2 * kEeWaitCycles + 1024);
}

TEST(ReplyMonitor, FrameBinsFollowTheirLabels)
{
	EXPECT_EQ(FrameBin(0), 0u);
	EXPECT_EQ(FrameBin(1), 1u);
	EXPECT_EQ(FrameBin(2), 2u);
	EXPECT_EQ(FrameBin(3), 3u);
	EXPECT_EQ(FrameBin(4), 3u);
	EXPECT_EQ(FrameBin(5), 4u);
	EXPECT_EQ(FrameBin(8), 4u);
	EXPECT_EQ(FrameBin(9), 5u);
	EXPECT_EQ(FrameBin(1000), 5u);
}

TEST(ReplyMonitor, AnOvershootThatWakesTheGuestShowsAsBusyTimeInTheWindowAfterIt)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 t = kBase;
	const u64 now = Tick(m, kBase, kBase + 60 * kStep, true);
	ASSERT_TRUE(m.OnReply(ReplyKind::SbusIrq, Stamp(t), now, true).overshoot);
	// After the reply the EE runs for half of the window (an interval that is not idle is counted whole), then idles again.
	const u64 half = kWakeWindow / 2;
	u64 c = now;
	while (c < now + half)
	{
		c += kStep;
		m.OnEventTest(c, false, 1);
	}
	Tick(m, c, now + kWakeWindow + kStep, true);
	EXPECT_EQ(m.Stats().wake_done, 1u);
	EXPECT_EQ(m.Stats().wake_decile[5], 1u) << "about half of the window the EE ran";
}

TEST(ReplyMonitor, AnOvershootAfterWhichTheEeStaysIdleHasNoBusyTimeInTheWindow)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 now = Tick(m, kBase, kBase + 60 * kStep, true);
	ASSERT_TRUE(m.OnReply(ReplyKind::Call, Stamp(kBase), now, true).overshoot);
	m.OnEventTest(now + kStep, false, 1); // a handler: one interval of running
	Tick(m, now + kStep, now + kWakeWindow + 2 * kStep, true);
	EXPECT_EQ(m.Stats().wake_done, 1u);
	EXPECT_EQ(m.Stats().wake_decile[0], 1u) << "3,072 of 98,304 cycles is under a tenth";
}

TEST(ReplyMonitor, TheWindowIsClosedByTheEventTestThatPassesIt)
{
	ReplyMonitor m;
	m.OnEventTest(kBase, false, 1);
	const u64 now = Tick(m, kBase, kBase + 60 * kStep, true);
	ASSERT_TRUE(m.OnReply(ReplyKind::Call, Stamp(kBase), now, true).overshoot);
	Tick(m, now, now + kWakeWindow - kStep, true);
	EXPECT_EQ(m.Stats().wake_done, 0u) << "not over yet";
	m.OnEventTest(now + kWakeWindow, true, 1);
	EXPECT_EQ(m.Stats().wake_done, 1u);
}
