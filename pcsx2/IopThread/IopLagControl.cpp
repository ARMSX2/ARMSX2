// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopLagControl.h"

#include <algorithm>

namespace IopThread
{
	const char* ReplyKind::Name(u32 kind)
	{
		switch (kind)
		{
			case SbusIrq:
				return "sbus_irq";
			case Call:
				return "call";
			case Sif0Pump:
				return "sif0_pump";
			case Sif1Pump:
				return "sif1_pump";
			default:
				return "?";
		}
	}

	u32 LateBucket(u64 late)
	{
		u32 i = 0;
		while (i < kLateEdgeCount && late >= kLateEdges[i])
			i++;
		return i;
	}

	void LateHist::Add(u64 late)
	{
		count++;
		sum += late;
		max = std::max(max, late);
		bucket[LateBucket(late)]++;
	}

	u32 FrameBin(u64 events)
	{
		if (events <= 2)
			return static_cast<u32>(events);
		if (events <= 4)
			return 3;
		if (events <= 8)
			return 4;
		return 5;
	}

	void ReplyMonitor::Reset()
	{
		const bool trace = m_trace_on;
		m_stats = ReplyStats();
		m_prev = 0;
		m_have_prev = false;
		m_event_ff = false;
		m_in_streak = false;
		m_streak_start = 0;
		m_ff_cum = 0;
		m_first = 0;
		m_frame = 0;
		m_have_frame = false;
		m_f_replies = m_f_late = m_f_events = m_f_max_late = m_f_ff_start = m_f_cycle_start = 0;
		m_f_unbroken = m_f_share90 = 0;
		m_wake_n = 0;
		m_trace.clear();
		m_trace_on = trace;
		std::fill(std::begin(m_ring), std::end(m_ring), Sample{0, 0});
	}

	void ReplyMonitor::EndFrame()
	{
		m_stats.frames++;
		if (m_f_events != 0)
			m_stats.frames_with_overshoot++;
		m_stats.frame_bins[FrameBin(m_f_events)]++;
		if (m_trace_on && m_trace.size() < kMaxTrace)
		{
			m_trace.push_back({m_frame, static_cast<u32>(m_f_replies), static_cast<u32>(m_f_late), static_cast<u32>(m_f_events),
				static_cast<u32>(m_f_unbroken), static_cast<u32>(m_f_share90), m_f_max_late, m_ff_cum - m_f_ff_start, m_prev - m_f_cycle_start});
		}
		m_f_replies = m_f_late = m_f_events = m_f_max_late = 0;
		m_f_unbroken = m_f_share90 = 0;
		m_f_ff_start = m_ff_cum;
		m_f_cycle_start = m_prev;
	}

	bool ReplyMonitor::OnEventTest(u64 cycle, bool via_ff, u32 frame)
	{
		// A clock that went backwards (a state was loaded) starts the books of the interval again.
		if (m_have_prev && cycle < m_prev)
		{
			m_prev = cycle;
			m_in_streak = false;
			m_f_cycle_start = cycle;
		}

		bool new_frame = false;
		if (!m_have_frame)
		{
			m_have_frame = true;
			m_frame = frame;
			m_first = cycle;
			m_f_ff_start = m_ff_cum;
			m_f_cycle_start = cycle;
		}
		else if (frame != m_frame)
		{
			EndFrame();
			m_frame = frame;
			new_frame = true;
		}

		const u64 prev = m_have_prev ? m_prev : cycle;
		for (u32 i = 0; i < m_wake_n;)
		{
			Wake& w = m_wake[i];
			const u64 lo = std::max(prev, w.from);
			const u64 hi = std::min(cycle, w.until);
			if (!via_ff && hi > lo)
				w.busy += hi - lo;
			if (cycle >= w.until)
			{
				m_stats.wake_decile[std::min<u64>(w.busy * 10 / (w.until - w.from), 10)]++;
				m_stats.wake_done++;
				m_wake[i] = m_wake[--m_wake_n];
			}
			else
			{
				i++;
			}
		}
		m_stats.event_tests++;
		if (via_ff)
		{
			m_ff_cum += cycle - prev;
			m_stats.ff_event_tests++;
			if (!m_in_streak)
			{
				m_in_streak = true;
				m_streak_start = prev;
			}
		}
		else
		{
			m_in_streak = false;
		}
		m_event_ff = via_ff;
		m_prev = cycle;
		m_have_prev = true;

		const u64 bucket = cycle >> kBucketShift;
		m_ring[bucket % kBuckets] = Sample{static_cast<u32>(bucket), m_ff_cum};
		m_stats.ff_cycles = m_ff_cum;
		m_stats.ee_cycles = cycle - m_first;
		return new_frame;
	}

	bool ReplyMonitor::IdleCyclesAt(u64 t, u64& cum) const
	{
		if (!m_have_prev || t <= m_first)
		{
			cum = 0;
			return m_have_prev;
		}
		const u64 b = t >> kBucketShift;
		const u64 now_bucket = m_prev >> kBucketShift;
		if (now_bucket - std::min(now_bucket, b) >= kBuckets - 2)
			return false; // older than the samples
		for (u32 k = 1; k <= 8 && k <= b; k++)
		{
			const u64 want = b - k;
			const Sample& s = m_ring[want % kBuckets];
			if (s.tag == static_cast<u32>(want))
			{
				cum = s.cum;
				return true;
			}
		}
		// Nothing sampled in the eight buckets before t's: the EE ran no event test for 32,768 cycles. That is a single long
		// stretch with no fast-forward in it.
		cum = m_ff_cum;
		return false;
	}

	ReplyMonitor::Verdict ReplyMonitor::OnReply(u32 kind, u32 stamp_lo, u64 now, bool in_event_test)
	{
		Verdict v{0, false};
		if (kind >= ReplyKind::kCount)
			kind = ReplyKind::Call;

		const s32 diff = static_cast<s32>(static_cast<u32>(now) - stamp_lo);
		const u64 late = diff > 0 ? static_cast<u64>(diff) : 0;
		v.late = late;

		m_stats.replies++;
		m_stats.all[kind].Add(late);
		m_f_replies++;
		if (late < static_cast<u64>(kOvershootMin))
			return v;

		m_stats.late++;
		m_f_late++;
		m_f_max_late = std::max(m_f_max_late, late);

		const u64 t = now - late;
		const bool idle_now = in_event_test && m_event_ff;

		u64 cum_t = 0;
		const bool known = IdleCyclesAt(t, cum_t);
		u64 idle = 0;
		if (known)
		{
			idle = std::min(m_ff_cum - std::min(m_ff_cum, cum_t), late);
			m_stats.coverage_decile[std::min<u64>(idle * 10 / late, 10)]++;
		}
		else
		{
			m_stats.late_idle_unknown++;
		}

		const bool unbroken = idle_now && m_in_streak && t >= m_streak_start;
		const bool share_ok = idle_now && known && idle * 100 >= late * kIdleShareMin;
		if (unbroken)
		{
			m_stats.idle_overshoots_unbroken++;
			m_f_unbroken++;
		}
		if (idle_now && known && idle * 10 >= late * 9)
		{
			m_stats.idle_overshoots_90++;
			m_f_share90++;
		}

		if (share_ok)
		{
			v.overshoot = true;
			m_stats.idle_overshoots++;
			m_stats.idle_overshoot_cycles += late;
			m_stats.idle_overshoot_max = std::max(m_stats.idle_overshoot_max, late);
			m_stats.overshoot[kind].Add(late);
			m_f_events++;
			if (m_wake_n < kMaxWake)
				m_wake[m_wake_n++] = Wake{now, now + kWakeWindow, 0};
		}
		else if (!idle_now)
		{
			m_stats.late_ee_busy++;
			m_stats.late_ee_busy_cycles += late;
		}
		else
		{
			m_stats.late_idle_light++;
			m_stats.late_idle_light_cycles += late;
		}
		return v;
	}
} // namespace IopThread
