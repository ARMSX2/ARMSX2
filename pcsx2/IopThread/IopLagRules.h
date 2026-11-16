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
		/// A stream is running on the IOP: SPU2 auto-DMA input, a CDVD read or a call to the memory card server (the streams word; StreamBit).
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
		inline constexpr u32 McServerCall = 1 << 3; // a call to the memory card server is in progress (McServerCalls)
	} // namespace StreamBit

	/// StreamBit::McServerCall: the calls to the IOP's memory card server (the RPC server with id 0x80000400) that the server has not
	/// finished. A game that loads or saves sends one call per operation (open, read, close, directory, ...) and polls for its end; at a
	/// large bound each end reached the EE up to a frame late, and a game that times its operations reports a good save as damaged.
	///
	/// The IOP side of SIF sees every command packet: the EE's RPC calls land in IOP memory over SIF1, and the server's replies leave
	/// over SIF0. A call packet names the server it is for by the address of the server's data, whose first word is the server's id. A
	/// call is in progress from its packet's arrival until the server's RPC end packet for its client leaves, or, for a call made
	/// without the no-wait mode, until the IOP writes the end straight into the EE's copy of the call packet (a SIF0 transfer to the
	/// call's packet address). The IOP thread owns it (whichever thread runs the IOP, in Off). A savestate load and a VM reset forget
	/// the calls (ResyncClocks).
	class McServerCalls
	{
	public:
		/// The memory card server's RPC id (mcserv and its successors).
		static constexpr u32 kServerId = 0x80000400;
		/// A call older than this, in EE cycles of IOP time, is forgotten: two seconds. A call whose end never comes (the IOP was
		/// rebooted under it) would otherwise hold the bound tight for good.
		static constexpr u64 kMaxAge = 120 * 4915200ull;

		// SIF command ids (sifcmd).
		static constexpr u32 kCmdRpcEnd = 0x80000008;
		static constexpr u32 kCmdRpcCall = 0x8000000A;

		/// A transfer the EE sent over SIF1 landed in IOP memory: `count` words at `words`. `read_iop(addr)` reads an IOP word.
		template <typename ReadIop>
		void OnSif1Transfer(const u32* words, u32 count, ReadIop&& read_iop)
		{
			// SifRpcCallPkt: the command header {size, dest, cid, opt}, rec_id, pkt_addr, rpc_id, client, rpc_number, send_size,
			// receive, recv_size, rmode, server. The size in the header's low byte covers at least those.
			if (count < 14 || words[2] != kCmdRpcCall || (words[0] & 0xff) < 56)
				return;
			if (read_iop(words[13]) != kServerId)
				return;
			const Call call = {words[7], words[5], kNotSeen};
			for (u32 i = 0; i < m_count; i++)
			{
				if (m_calls[i].client == call.client)
				{
					m_calls[i] = call;
					return;
				}
			}
			// Full: the oldest call goes, its end most likely lost.
			if (m_count == kSlots)
			{
				for (u32 i = 1; i < kSlots; i++)
					m_calls[i - 1] = m_calls[i];
				m_count--;
			}
			m_calls[m_count++] = call;
		}

		/// A transfer the IOP sends over SIF0: `count` words at `words` in IOP memory, to EE address `ee_dest`.
		void OnSif0Transfer(const u32* words, u32 count, u32 ee_dest)
		{
			// SifRpcRendPkt: the command header, rec_id, pkt_addr, rpc_id, client, cid, server, buff, cbuff.
			const bool end_packet = count >= 8 && words[2] == kCmdRpcEnd;
			for (u32 i = 0; i < m_count;)
			{
				const Call& c = m_calls[i];
				if ((end_packet && c.client == words[7]) || SameEeAddress(c.pkt_addr, ee_dest))
					m_calls[i] = m_calls[--m_count];
				else
					i++;
			}
		}

		/// Whether a call is in progress at IOP position `now`. Ages the calls from the first look at each.
		bool InProgress(u64 now)
		{
			for (u32 i = 0; i < m_count;)
			{
				Call& c = m_calls[i];
				if (c.since == kNotSeen)
					c.since = now;
				if (now - c.since > kMaxAge)
					c = m_calls[--m_count];
				else
					i++;
			}
			return m_count != 0;
		}

		void Reset() { m_count = 0; }

	private:
		// EE RAM through any of its mappings (cached, uncached, uncached accelerated).
		static bool SameEeAddress(u32 a, u32 b) { return ((a ^ b) & 0x0FFFFFFF) == 0; }

		static constexpr u64 kNotSeen = ~0ull;
		static constexpr u32 kSlots = 4;
		struct Call
		{
			u32 client;
			u32 pkt_addr; // the EE's copy of the call packet
			u64 since;
		};
		Call m_calls[kSlots] = {};
		u32 m_count = 0;
	};

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
		/// LagRule::Stream: which StreamBit bits count. SPU2 auto-DMA and memory card server calls, not a disc read: with a disc read counted the rule held the
		/// bound tight for 59 percent of the EE's cycles in a God of War II state, whose engine reads the disc all the time, and gained nothing for the
		/// games that need it.
		u32 stream_bits = StreamBit::SpuAdmaCore0 | StreamBit::SpuAdmaCore1 | StreamBit::McServerCall;
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
