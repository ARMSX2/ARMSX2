// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The measurement half of the dynamic EE cycle-rate governor.
//
// EECycleRateController decides; this decides what it is deciding about. The two are
// split because they fail differently: a controller bug is a rule that oscillates or
// walks to the floor, reproducible from a fixed array of structs, while a sampler bug
// is a number that means something other than what it is named, and no amount of
// deterministic sequence testing will catch that.
//
// So the arithmetic that turns a window's frames into one EECycleRateWindowSample is a
// pure function over an array, testable without a VM, a clock or a limiter. The rest of
// this file - the per-frame accumulation, the wait accounting, the trace - is the part
// that needs a running emulator, and it is deliberately thin.

#pragma once

#include "EECycleRateController.h"

#include "common/Pcsx2Types.h"

// One frame as measured on the CPU thread at the limiter boundary. Every tick count is in
// the GetCPUTicks() domain, which is the domain the limiter's own per-frame target is
// expressed in, so no conversion happens per frame.
struct EECycleRateFrameSample
{
	// Wall time from the limiter's post-sleep return to the next Throttle() entry: the
	// frame's active work with the deliberate limiter sleep taken out. It is wall time,
	// not CPU time, so it includes any stretch the EE thread spent blocked.
	u64 active_ticks = 0;

	// The limiter's budget for this frame (s_limiter_ticks_per_frame).
	u64 target_ticks = 0;

	// Wall time inside active_ticks that the EE thread spent waiting on the GS thread -
	// the vsync-queue wait, the ring-full sleep and spin, and WaitGS. Both the sleeping
	// and the spinning kinds count: the spin burns a core without retiring guest work,
	// which is exactly the case an EE underclock cannot help.
	u64 gs_wait_ticks = 0;

	// The same for the VU1 thread under MTVU: the ring-full yield loop and WaitVU.
	u64 vu1_wait_ticks = 0;

	// The limiter's own verdict that this frame missed its deadline, taken at Throttle
	// entry. In practice it equals active_ticks > target_ticks, because the limiter's
	// frame start IS the post-sleep instant this frame's active time is measured from;
	// it is carried separately so the window arithmetic stays a pure function of the
	// array rather than re-deriving the limiter's rule.
	bool late = false;
};

namespace EECycleRateSampler
{
	// Frames one window can hold. 500 ms of target time is 30 frames at 60 Hz and 25 at
	// 50 Hz; the headroom is for a limiter target the settings can make shorter (turbo
	// doubles the frame rate), and a window that fills the array closes early rather than
	// dropping frames on the floor, so the ratios always describe the frames they claim to.
	inline constexpr u32 kMaxWindowFrames = 128;

	// Everything BuildWindowSample needs, so the arithmetic never reads a clock or a global.
	struct WindowInputs
	{
		const EECycleRateFrameSample* frames = nullptr;
		u32 frame_count = 0;

		// GetTickFrequency(): converts the frame ticks above to seconds.
		u64 tick_frequency = 0;

		// The CPU thread's own CPU time over the window, and its own frequency
		// (Threading::GetThreadTicksPerSecond()). A separate clock from the frame ticks,
		// hence a separate frequency.
		u64 cpu_time_delta = 0;
		u64 cpu_time_frequency = 0;

		// A frame inside this window was not measurable - the limiter went unlimited or
		// host-vsync paced, or execution was interrupted. The window is reported invalid
		// rather than repaired: a partial window is not a smaller window, it is a
		// different measurement.
		bool invalidated = false;
	};

	// The window's frames as one sample. Pure: no clock, no VM, no globals.
	//
	// p90 is nearest-rank over the per-frame active/target ratios - index
	// ceil(0.9 * n) - 1 into the ascending sort, so 30 frames take the 27th. Nearest-rank
	// rather than an interpolated percentile because the value has to be one a frame
	// actually produced; an interpolated p90 of a bimodal window is a ratio no frame had.
	EECycleRateWindowSample BuildWindowSample(const WindowInputs& in);
} // namespace EECycleRateSampler
