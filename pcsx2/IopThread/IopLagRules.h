// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// When the EE must not run far ahead of the IOP.
//
// The lag bound L (IopThread.h) is how many EE cycles the EE may run ahead of the IOP. A large L lets the EE overlap its work with the
// IOP's, which pays when the IOP thread is slow (a little core). It costs when the EE is waiting for the IOP: the EE races to the
// bound while it waits, so what the IOP produces reaches the EE up to L cycles after the IOP produced it, and a game that sends a
// request and waits for the reply spends L cycles of guest time per request.
//
// These rules say, from operations the emulator can see, when the EE is waiting for the IOP. While one holds, the bound is the tight
// bound (the default lag, 24,576 cycles) instead of L. Nothing else changes: no rule changes what the guest sees except through the
// time it takes, so a rule that fires when it was not needed costs speed and never correctness. With no rule on, the bound is L.
//
// The class is pure: it reads no global and owns no thread. The EE thread calls the On* functions and Bound() (in Lockstep the IOP
// thread calls OnIopSif0Reply while the EE is blocked in the handoff, which orders them). The streams word is the one input the IOP
// thread writes, and the caller passes it in.

#pragma once

#include "IopThread/IopThreadMode.h"

#include "common/Pcsx2Types.h"

namespace IopThread
{
	/// The rules, as indices (stats) and as bits (LagRulesConfig::mask).
	namespace LagRule
	{
		/// The EE started a SIF transfer: a request went to the IOP, or the EE armed SIF0 to receive. Tight for `window` EE cycles,
		/// or until the IOP's next SIF0 transfer reaches the EE, whichever is first.
		inline constexpr u32 Window = 0;
		/// The EE is idle (the event test was reached by a fast-forward) and a request is outstanding: the EE started a SIF1 transfer
		/// and no SIF0 transfer or SBUS interrupt has come from the IOP since, for at most `outstanding_max_age` cycles from the latest send.
		inline constexpr u32 IdleOutstanding = 1;
		/// The EE is idle, whatever it has sent. Experiment: what idle alone costs a game whose EE is mostly busy.
		inline constexpr u32 IdleAny = 2;
		/// A stream is running on the IOP: SPU2 auto-DMA input or a CDVD read (the streams word; StreamBit).
		inline constexpr u32 Stream = 3;
		/// The first `boot_frames` frames after the VM booted or reset: modules load, the IOP reboots, custom disc drivers handshake.
		inline constexpr u32 Boot = 4;
		/// The EE touched an SBUS mailbox word (the handshake of an IOP reboot; gameplay makes none). Tight for `mailbox_window` cycles.
		inline constexpr u32 Mailbox = 5;
		/// The EE read the status of a SIF DMA channel (D5 or D6 CHCR) with its STR bit set: it is polling for the end of a transfer.
		/// Tight for `poll_window` cycles.
		inline constexpr u32 Poll = 6;
		inline constexpr u32 kCount = 7;

		inline constexpr u32 Bit(u32 rule) { return 1u << rule; }
		inline constexpr u32 kAll = (1u << kCount) - 1;
		const char* Name(u32 rule);
	} // namespace LagRule

	/// The streams word: what the IOP thread publishes about the IOP's devices.
	namespace StreamBit
	{
		inline constexpr u32 SpuAdmaCore0 = 1 << 0; // SPU2 core 0 auto-DMA input on
		inline constexpr u32 SpuAdmaCore1 = 1 << 1; // SPU2 core 1 auto-DMA input on
		inline constexpr u32 CdvdBusy = 1 << 2; // a disc read or a drive action is running
	} // namespace StreamBit

	struct LagRulesConfig
	{
		/// LagRule::Bit() of every rule that is on. 0 is no rules: Bound() returns the loose bound.
		u32 mask = 0;
		/// The bound a rule imposes, in EE cycles. At or above the loose bound it changes nothing.
		s32 tight = kDefaultLag;
		/// LagRule::Window: how long after an EE-started transfer the bound stays tight if no reply comes.
		u32 window = 98304;
		/// LagRule::Window: which channels' starts open it: bit 0 SIF0 (the EE arms to receive), bit 1 SIF1 (the EE sends).
		u32 window_channels = 3;
		/// LagRule::Window and IdleOutstanding: a SIF0 transfer or SBUS interrupt from the IOP ends the window and the outstanding request.
		bool close_on_reply = true;
		/// LagRule::Window: with close_on_reply, whether a reply ends the window too. Off, the window runs its full length: the first thing
		/// the IOP sends back after a request is often not the end of the request (God of War II's movie player, IopThreadVm.cpp).
		bool window_close_on_reply = true;
		/// LagRule::IdleOutstanding: a request older than this (EE cycles) is no longer outstanding.
		u32 outstanding_max_age = 4915200;
		/// LagRule::Mailbox and Poll: window lengths.
		u32 mailbox_window = 4915200;
		u32 poll_window = 98304;
		/// LagRule::Stream: which StreamBit bits count. SPU2 auto-DMA only: with a disc read counted too the rule held the bound tight for 59 percent
		/// of the EE's cycles in a God of War II state, whose engine reads the disc all the time, and gained nothing for the games that need it.
		u32 stream_bits = StreamBit::SpuAdmaCore0 | StreamBit::SpuAdmaCore1;
		/// LagRule::Boot: frames after a boot or reset.
		u32 boot_frames = 600;
	};

	struct LagRulesStats
	{
		u64 activations[LagRule::kCount] = {}; // times the rule began to hold (a window opened, an idle stretch began, a stream started)
		u64 event_tests[LagRule::kCount] = {}; // event tests it made tight (the first rule in the order below that held gets the credit)
		u64 cycles[LagRule::kCount] = {}; // EE cycles of the intervals ending in those event tests
		u64 window_closed_by_reply = 0; // windows a reply ended before their time
		u64 total_event_tests = 0; // event tests seen
		u64 total_cycles = 0; // EE cycles from the first to the last
	};

	class LagRules
	{
	public:
		void Configure(const LagRulesConfig& config);
		const LagRulesConfig& Config() const { return m_cfg; }
		/// Forget the state (a window, an outstanding request, the stats) and set the boot clock to `frame`.
		void Reset(u32 frame = 0);

		/// Anything on at all for this loose bound? False lets the caller skip the rest.
		bool Live(s64 loose) const { return m_cfg.mask != 0 && loose > m_cfg.tight; }

		// ---- EE thread
		/// The EE started a SIF transfer (`channel` 0 or 1) at EE cycle `cycle`.
		void OnEeSifStart(u64 cycle, u32 channel);
		/// A SIF0 transfer or an SBUS interrupt from the IOP reached the EE.
		void OnIopReply(u64 cycle);
		/// The EE touched an SBUS mailbox word.
		void OnMailboxAccess(u64 cycle);
		/// The EE read D5 or D6 CHCR with STR set.
		void OnEePoll(u64 cycle);

		/// The bound for an event test at `cycle`. `idle`: the event test was reached by a fast-forward. `streams`: StreamBit bits.
		/// `frame`: the frame count. `loose`: the bound with no rule. Returns at most `loose`. Counts the event test.
		s64 Bound(u64 cycle, bool idle, u32 streams, u32 frame, s64 loose);

		/// The rule that held at the last Bound() call that was tight, or -1.
		s32 LastRule() const { return m_last_rule; }

		const LagRulesStats& Stats() const { return m_stats; }

	private:
		bool Open(u64 until, u64 cycle) const { return static_cast<s64>(until - cycle) > 0; }

		LagRulesConfig m_cfg;
		LagRulesStats m_stats;

		u64 m_window_until = 0; // LagRule::Window
		u64 m_mailbox_until = 0;
		u64 m_poll_until = 0;
		bool m_outstanding = false;
		u64 m_outstanding_since = 0;

		// Edge detection for the activation counts of the conditions that last across event tests.
		bool m_was_idle_out = false;
		bool m_was_idle_any = false;
		bool m_was_stream = false;
		bool m_was_boot = false;

		u32 m_boot_from = 0;
		bool m_have_prev = false;
		u64 m_prev = 0;
		u64 m_first = 0;
		s32 m_last_rule = -1;
	};
} // namespace IopThread
