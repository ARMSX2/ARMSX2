// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// How late the EE takes what the IOP produced, and whether the EE was idle across the lateness.
//
// With a lag bound L above the default the EE may run up to L EE cycles ahead of the IOP. What the IOP produces for the EE
// (the SBUS interrupt, a SIF pump it started) then reaches the EE up to L cycles after the IOP's time of production
// (I2EItem::stamp). If the guest is idle in between (the EE fast-forwarded its clock through a wait loop, FastForward in
// IopThreadMode.h) and it was waiting for exactly that reply, the guest's idle time ran past the reply and the game slows
// down by the lateness. If the guest was waiting for something else (the next vsync) the lateness costs it nothing.
//
// ReplyMonitor keeps the EE-side books to tell the two apart, with no knowledge of what the guest waits for:
//  - OnEventTest(): at every EE event test, whether it was reached by a fast-forward. An event test reached that way ends an
//    idle interval, from the previous event test to this one. The monitor keeps the start of the unbroken run of idle
//    intervals that ends now (the idle streak), and the idle cycles summed since the start (the ledger), with a sample of the
//    sum per 4096-cycle bucket of EE time so that it can be read back at an earlier cycle.
//  - OnReply(): the EE takes a reply that was produced at IOP time t, at EE cycle `now`. The lateness is now - t. The reply is
//    an OVERSHOOT IN IDLE when it is late by at least kOvershootMin, and the event test it is taken in was reached by a
//    fast-forward, and at least kIdleShareMin percent of [t, now] was idle by the ledger: the EE idled across the reply.
//    A guest idles with interruptions (an interrupt handler, a thread that wakes and sleeps again), so an unbroken run of idle
//    event tests back to t is too strict; the monitor also counts that reading (idle_overshoots_unbroken) and a 90 percent
//    share (idle_overshoots_90) to show how much the choice matters.
//    The late replies that are not overshoots are counted by why: the EE was not in an idle event test (late_ee_busy), or it
//    was but idle for less than the share (late_idle_light).
//
// The class is pure: it reads no global, so a test drives it with numbers. It belongs to the EE thread; in Lockstep the IOP
// thread calls OnReply with the EE blocked in the handoff, which orders the two.

#pragma once

#include "IopThread/IopThreadMode.h"

#include "common/Pcsx2Types.h"

#include <vector>

namespace IopThread
{
	/// A reply is an overshoot only if it is at least this late: twice the default lag bound. With L at the default a reply is
	/// late by at most about L plus two event-test spacings (about 31,000 cycles), so nothing is an overshoot at the default
	/// bound, whatever the guest does.
	inline constexpr s32 kOvershootMin = 2 * kDefaultLag;

	/// The share of [production, taken] that has to have been idle, by the ledger, for a late reply taken in an idle event test to be
	/// an overshoot in idle, in percent.
	inline constexpr u32 kIdleShareMin = 50;

	/// How long after an overshoot in idle the monitor watches the EE, to see whether the reply woke the guest (it ran for a good part
	/// of the window) or only ran a handler and went back to idle. In EE cycles.
	inline constexpr u32 kWakeWindow = 2 * kOvershootMin;

	/// What the EE takes from the IOP.
	namespace ReplyKind
	{
		inline constexpr u32 SbusIrq = 0; // the SBUS interrupt (Threaded: an I2E item; Lockstep: raised directly)
		inline constexpr u32 Call = 1; // a function the IOP had the EE run (Threaded CallOnEE: the SIF pumps the IOP starts)
		inline constexpr u32 Sif0Pump = 2; // Lockstep only: a SIF0 pump the IOP started, run directly
		inline constexpr u32 Sif1Pump = 3; // Lockstep only: a SIF1 pump the IOP started, run directly
		inline constexpr u32 kCount = 4;
		const char* Name(u32 kind);
	} // namespace ReplyKind

	/// Lateness histogram bucket edges, in EE cycles. Bucket i holds late < kLateEdges[i] and late >= kLateEdges[i-1]; the last
	/// holds everything from the last edge up. 4,915,200 is one NTSC frame.
	inline constexpr u32 kLateEdgeCount = 13;
	inline constexpr u32 kLateEdges[kLateEdgeCount] = {1024, 3072, 6144, 12288, 24576, 49152, 98304, 196608, 393216, 786432, 1179648, 2359296, 4915200};
	inline constexpr u32 kLateBuckets = kLateEdgeCount + 1;
	u32 LateBucket(u64 late);

	struct LateHist
	{
		u64 count = 0;
		u64 sum = 0;
		u64 max = 0;
		u64 bucket[kLateBuckets] = {};
		void Add(u64 late);
		double Mean() const { return count ? static_cast<double>(sum) / static_cast<double>(count) : 0.0; }
	};

	/// Frames are binned by how many overshoots in idle they had: 0, 1, 2, 3 to 4, 5 to 8, 9 or more.
	inline constexpr u32 kFrameBins = 6;
	u32 FrameBin(u64 events);

	struct ReplyStats
	{
		LateHist all[ReplyKind::kCount]; // every reply taken, by kind
		LateHist overshoot[ReplyKind::kCount]; // the overshoots in idle, by kind
		u64 replies = 0;
		u64 late = 0; // late by at least kOvershootMin
		u64 idle_overshoots = 0; // ... taken in an idle event test, with at least kIdleShareMin percent of [production, taken] idle: the signal
		u64 idle_overshoot_cycles = 0; // their lateness, summed
		u64 idle_overshoot_max = 0;
		u64 idle_overshoots_unbroken = 0; // late, idle event test, and the idle streak reaches back to the production: no EE run in between
		u64 idle_overshoots_90 = 0; // late, idle event test, and at least 90 percent of [production, taken] idle
		u64 late_ee_busy = 0; // late, and the event test the EE took the reply in was not reached by a fast-forward (or it was no event test)
		u64 late_ee_busy_cycles = 0;
		u64 late_idle_light = 0; // late, idle event test, but less than kIdleShareMin percent of [production, taken] idle
		u64 late_idle_light_cycles = 0;
		u64 late_idle_unknown = 0; // late replies whose production time is older than the ledger's samples (counted in the groups above too)
		u64 wake_decile[11] = {}; // overshoots in idle by the share of the kWakeWindow after them that the EE ran (was not idle), in tenths: 0 .. 10
		u64 wake_done = 0; // overshoots whose window has ended
		u64 coverage_decile[11] = {}; // late replies by the idle share of [production, now], in tenths: 0 .. 10
		u64 frames = 0; // frames seen (completed)
		u64 frames_with_overshoot = 0;
		u64 frame_bins[kFrameBins] = {}; // completed frames by overshoots in idle in the frame
		u64 event_tests = 0;
		u64 ff_event_tests = 0; // event tests reached by a fast-forward
		u64 ff_cycles = 0; // EE cycles of the idle intervals, summed (the ledger)
		u64 ee_cycles = 0; // EE cycles from the first event test to the last
	};

	/// One completed frame, when the trace is on.
	struct FrameRecord
	{
		u32 frame;
		u32 replies;
		u32 late;
		u32 idle_overshoots;
		u32 unbroken;
		u32 share90;
		u64 max_late;
		u64 ff_cycles; // idle cycles of the frame, by the ledger
		u64 ee_cycles;
	};

	class ReplyMonitor
	{
	public:
		/// What OnReply found.
		struct Verdict
		{
			u64 late; // now - production, 0 when the IOP was ahead
			bool overshoot; // late, in an idle event test, idle for kIdleShareMin percent or more of the lateness: the signal
		};

		void Reset();

		/// Keeps the FrameRecord of every completed frame (bounded at kMaxTrace). Set before the run.
		void EnableTrace(bool on) { m_trace_on = on; }
		const std::vector<FrameRecord>& Trace() const { return m_trace; }

		/// An EE event test begins at EE cycle `cycle`, in frame `frame`. `via_ff`: reached by a fast-forward. Returns true if
		/// this event test begins a new frame.
		bool OnEventTest(u64 cycle, bool via_ff, u32 frame);

		/// The EE takes a reply of `kind` produced at the IOP time with low 32 bits `stamp_lo`, at EE cycle `now`.
		/// `in_event_test`: the EE is inside the event test OnEventTest last saw. A reply taken elsewhere (a hold on the guest's access
		/// to IOP space) cannot be an overshoot in idle: the guest was running.
		Verdict OnReply(u32 kind, u32 stamp_lo, u64 now, bool in_event_test);

		/// The counters. The frame in progress is not in the frame bins.
		const ReplyStats& Stats() const { return m_stats; }

		/// EE cycles of the idle ledger up to now, and the sample for `bucket` (tests).
		u64 IdleCycles() const { return m_ff_cum; }
		/// The start of the current idle streak: the EE cycle of the event test before the first of the unbroken run of event tests
		/// reached by a fast-forward that ends now. Meaningful while the last event test was one of them.
		u64 StreakStart() const { return m_streak_start; }
		bool LastEventTestWasIdle() const { return m_event_ff; }

		static constexpr u32 kBucketShift = 12;
		static constexpr u32 kBuckets = 4096; // 16.7 million cycles of history: more than kMaxLag, less than 4 frames
		static constexpr size_t kMaxTrace = 1 << 14;

	private:
		struct Sample
		{
			u32 tag; // the bucket number, low 32 bits
			u64 cum; // the ledger at the last event test in the bucket
		};

		/// The ledger at the end of the last bucket that finished at or before `t`. False if the samples do not reach back that far.
		bool IdleCyclesAt(u64 t, u64& cum) const;
		void EndFrame();

		// Overshoots in idle whose kWakeWindow is still open.
		struct Wake
		{
			u64 from, until, busy;
		};
		static constexpr u32 kMaxWake = 8;
		Wake m_wake[kMaxWake] = {};
		u32 m_wake_n = 0;

		ReplyStats m_stats;

		u64 m_prev = 0; // the cycle of the previous event test
		bool m_have_prev = false;
		bool m_event_ff = false; // the last event test was reached by a fast-forward
		bool m_in_streak = false;
		u64 m_streak_start = 0;
		u64 m_ff_cum = 0;
		u64 m_first = 0;
		u32 m_frame = 0;
		bool m_have_frame = false;

		// The frame in progress.
		u64 m_f_unbroken = 0, m_f_share90 = 0;
		u64 m_f_replies = 0, m_f_late = 0, m_f_events = 0, m_f_max_late = 0, m_f_ff_start = 0, m_f_cycle_start = 0;

		bool m_trace_on = false;
		std::vector<FrameRecord> m_trace;
		Sample m_ring[kBuckets] = {};
	};
} // namespace IopThread
