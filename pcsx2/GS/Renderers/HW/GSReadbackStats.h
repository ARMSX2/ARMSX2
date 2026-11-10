// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Readback statistics: measurement scaffolding for the gs-runs readback lane, turned on by gsrunner
// (-readback-stats, -readback-trace) and off in every other build. Nothing here changes what the
// renderer does; with s_enabled false every hook is one branch on a global.
//
// A request is one GS-to-EE transfer (GSState::InitReadFIFO) or one local-to-local move that fell to
// the CPU (GSRendererHW::Move). A read is one GSTextureCache::Read of a target that reached the device.
// A round trip is one blocking wait of the GS thread on the GPU made for a read. Time is split into the
// pending draws flushed before the copy, the copy's recording, the submit (with the ring's recycle wait
// when the next command buffer is still busy), the fence wait, and the CPU's map and write of the
// downloaded pixels into local memory.

#include "common/Pcsx2Types.h"

#include <chrono>
#include <cstdio>

namespace GSReadbackStats
{
	inline bool s_enabled = false; // per-frame counters (-readback-stats)
	inline std::FILE* s_fifo_dump = nullptr; // every GS-to-EE transfer's bytes (-readback-fifo-dump)
	inline bool s_trace = false; // one line per request and per read (-readback-trace)
	inline int s_loop = 0; // set by the runner: the dump loop (counting down) and frame in the dump
	inline int s_frame_no = 0;
	inline const char* s_site = nullptr; // the request being serviced, for the trace

	using Clock = std::chrono::steady_clock;

	inline double Us(Clock::time_point a, Clock::time_point b)
	{
		return std::chrono::duration<double, std::micro>(b - a).count();
	}

	struct Counters
	{
		u64 requests = 0; // transfers that asked the texture cache for local memory
		u64 reads = 0; // target reads that reached the device
		u64 stashed = 0; // of those, reads made ahead for the next read of the same target
		u64 served = 0; // reads answered from such a read, without the device
		u64 round_trips = 0; // blocking GPU waits made for a read
		u64 submits = 0; // command buffer submissions of any kind
		double request_us = 0; // whole time inside the texture cache for requests
		double flush_draws_us = 0;
		double record_us = 0;
		double submit_us = 0;
		double wait_us = 0;
		double map_write_us = 0;
		u64 read_pixels = 0;
		u64 fifo_bytes = 0;
		u64 fifo_hash = 0; // order-dependent combination of the hashes of the bytes handed to the EE
	};

	inline Counters s_frame; // reset by the runner at each frame boundary

	// Device-side counters the read is attributed with.
	inline u32 s_draws_since_submit = 0;
	inline u32 s_passes_since_submit = 0;

	// The read being timed, so the device can attribute its submit and wait to it.
	struct ReadTrace
	{
		bool live = false;
		double flush_draws_us = 0;
		double record_us = 0;
		double submit_us = 0;
		double wait_us = 0;
		double map_write_us = 0;
		u32 draws_pending = 0;
		u32 passes_pending = 0;
		u32 round_trips = 0;
	};
	inline ReadTrace* s_read = nullptr;

	inline void OnSubmit()
	{
		if (!s_enabled)
			return;
		s_frame.submits++;
		s_draws_since_submit = 0;
		s_passes_since_submit = 0;
	}

	inline void OnDraw()
	{
		if (s_enabled)
			s_draws_since_submit++;
	}

	inline void OnPass()
	{
		if (s_enabled)
			s_passes_since_submit++;
	}

	// The device's submit-and-wait (or plain fence wait) for a read.
	inline void OnReadWait(double submit_us, double wait_us)
	{
		if (!s_enabled)
			return;
		s_frame.round_trips++;
		s_frame.submit_us += submit_us;
		s_frame.wait_us += wait_us;
		if (s_read)
		{
			s_read->round_trips++;
			s_read->submit_us += submit_us;
			s_read->wait_us += wait_us;
		}
	}

	inline u64 Combine(u64 h, u64 v)
	{
		return (h ^ v) * 0x100000001b3ull + 0x9e3779b97f4a7c15ull;
	}

	// The last submit-and-wait the device made, read back by the download texture's flush.
	inline double s_last_submit_us = 0;
	inline double s_last_wait_us = 0;

	// A request: one GS-to-EE transfer or one CPU move, timed over its texture cache work.
	struct RequestScope
	{
		const char* prev_site;
		bool live;
		bool done = false;
		Clock::time_point t0;
		u64 reads0 = 0;
		u64 trips0 = 0;
		double us = 0;
		u64 reads = 0;
		u64 trips = 0;
		explicit RequestScope(const char* site)
			: prev_site(s_site)
			, live(s_enabled)
		{
			s_site = site;
			if (!live)
				return;
			t0 = Clock::now();
			reads0 = s_frame.reads;
			trips0 = s_frame.round_trips;
		}
		void Finish()
		{
			if (done)
				return;
			done = true;
			s_site = prev_site;
			if (!live)
				return;
			us = Us(t0, Clock::now());
			reads = s_frame.reads - reads0;
			trips = s_frame.round_trips - trips0;
			s_frame.requests++;
			s_frame.request_us += us;
		}
		~RequestScope() { Finish(); }
	};
} // namespace GSReadbackStats
