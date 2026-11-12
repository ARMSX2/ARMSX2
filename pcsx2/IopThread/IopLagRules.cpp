// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopLagRules.h"

#include <algorithm>

namespace IopThread
{
	const char* LagRule::Name(u32 rule)
	{
		switch (rule)
		{
			case Window:
				return "window";
			case IdleOutstanding:
				return "idle-outstanding";
			case IdleAny:
				return "idle-any";
			case Stream:
				return "stream";
			case Boot:
				return "boot";
			case Mailbox:
				return "mailbox";
			case Poll:
				return "poll";
			default:
				return "?";
		}
	}

	void LagRules::Configure(const LagRulesConfig& config)
	{
		m_cfg = config;
		m_cfg.mask &= LagRule::kAll;
		m_cfg.tight = std::max<s32>(m_cfg.tight, kMinLag);
	}

	void LagRules::Reset(u32 frame)
	{
		m_stats = {};
		m_window_until = m_mailbox_until = m_poll_until = 0;
		m_outstanding = false;
		m_outstanding_since = 0;
		m_was_idle_out = m_was_idle_any = m_was_stream = m_was_boot = false;
		m_boot_from = frame;
		m_have_prev = false;
		m_prev = m_first = 0;
		m_last_rule = -1;
	}

	void LagRules::OnEeSifStart(u64 cycle, u32 channel)
	{
		if (!m_cfg.mask || channel > 1)
			return;
		if (channel == 1)
		{
			// A new send is the request now; an older one nobody answered is forgotten.
			m_outstanding = true;
			m_outstanding_since = cycle;
		}
		if ((m_cfg.mask & LagRule::Bit(LagRule::Window)) && ((m_cfg.window_channels >> channel) & 1))
		{
			m_window_until = cycle + m_cfg.window;
			m_stats.activations[LagRule::Window]++;
		}
	}

	void LagRules::OnIopReply(u64 cycle)
	{
		if (!m_cfg.mask || !m_cfg.close_on_reply)
			return;
		m_outstanding = false;
		if (m_cfg.window_close_on_reply && Open(m_window_until, cycle))
		{
			m_window_until = cycle;
			m_stats.window_closed_by_reply++;
		}
	}

	void LagRules::OnMailboxAccess(u64 cycle)
	{
		if (!(m_cfg.mask & LagRule::Bit(LagRule::Mailbox)))
			return;
		m_mailbox_until = cycle + m_cfg.mailbox_window;
		m_stats.activations[LagRule::Mailbox]++;
	}

	void LagRules::OnEePoll(u64 cycle)
	{
		if (!(m_cfg.mask & LagRule::Bit(LagRule::Poll)))
			return;
		m_poll_until = cycle + m_cfg.poll_window;
		m_stats.activations[LagRule::Poll]++;
	}

	s64 LagRules::Bound(u64 cycle, bool idle, u32 streams, u32 frame, s64 loose)
	{
		m_stats.total_event_tests++;
		const u64 span = m_have_prev ? cycle - m_prev : 0;
		if (!m_have_prev)
		{
			m_have_prev = true;
			m_first = cycle;
		}
		m_prev = cycle;
		m_stats.total_cycles = cycle - m_first;

		if (m_cfg.mask == 0 || loose <= m_cfg.tight)
		{
			m_last_rule = -1;
			return loose;
		}

		const u32 mask = m_cfg.mask;
		const bool stream = (mask & LagRule::Bit(LagRule::Stream)) && (streams & m_cfg.stream_bits) != 0;
		const bool window = (mask & LagRule::Bit(LagRule::Window)) && Open(m_window_until, cycle);
		const bool poll = (mask & LagRule::Bit(LagRule::Poll)) && Open(m_poll_until, cycle);
		const bool mailbox = (mask & LagRule::Bit(LagRule::Mailbox)) && Open(m_mailbox_until, cycle);
		const bool idle_out = (mask & LagRule::Bit(LagRule::IdleOutstanding)) && idle && m_outstanding &&
		                      static_cast<u64>(cycle - m_outstanding_since) <= m_cfg.outstanding_max_age;
		const bool idle_any = (mask & LagRule::Bit(LagRule::IdleAny)) && idle;
		const bool boot = (mask & LagRule::Bit(LagRule::Boot)) && static_cast<u32>(frame - m_boot_from) < m_cfg.boot_frames;

		// Edges of the conditions that last across event tests. A window counts when it opens (On*).
		if (stream && !m_was_stream)
			m_stats.activations[LagRule::Stream]++;
		if (idle_out && !m_was_idle_out)
			m_stats.activations[LagRule::IdleOutstanding]++;
		if (idle_any && !m_was_idle_any)
			m_stats.activations[LagRule::IdleAny]++;
		if (boot && !m_was_boot)
			m_stats.activations[LagRule::Boot]++;
		m_was_stream = stream;
		m_was_idle_out = idle_out;
		m_was_idle_any = idle_any;
		m_was_boot = boot;

		s32 rule = -1;
		if (stream)
			rule = LagRule::Stream;
		else if (window)
			rule = LagRule::Window;
		else if (poll)
			rule = LagRule::Poll;
		else if (mailbox)
			rule = LagRule::Mailbox;
		else if (idle_out)
			rule = LagRule::IdleOutstanding;
		else if (idle_any)
			rule = LagRule::IdleAny;
		else if (boot)
			rule = LagRule::Boot;

		m_last_rule = rule;
		if (rule < 0)
			return loose;
		m_stats.event_tests[rule]++;
		m_stats.cycles[rule] += span;
		return m_cfg.tight;
	}
} // namespace IopThread
