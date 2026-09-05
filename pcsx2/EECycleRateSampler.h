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

	// --- wait accounting -----------------------------------------------------------
	//
	// Bracket the EE thread's blocking sites on the GS and VU1 threads. These are the
	// difference between "the EE thread is the cost" and "the EE thread is waiting", and
	// slowing the guest down only answers the first.
	//
	// Both sleeping and spinning waits count. The spin branches burn a core without
	// retiring guest work, so from the deadline's point of view they are identical to the
	// sleeping ones, and they are the branches an underclock most obviously cannot help.
	//
	// Every call site must be on a path that is already about to block. BeginWait() reads
	// a clock, and a call that finds the ring or the queue with room must not reach it.

	// Opens a wait. Returns 0 - and touches no clock - when the governor and its trace are
	// both off, or when the caller is not the CPU thread: MTVU reaches MTGS::WaitGS and the
	// ring-full stall from its own thread, and the per-frame counters belong to the EE.
	//
	// Zero is the "not timing" sentinel, so a tick value of exactly zero costs one
	// unmeasured wait. GetCPUTicks() counts from host boot on every platform we build for,
	// so that is a theoretical case, and losing one wait out of a window of thousands is
	// not worth a wider return type to rule out.
	u64 BeginWait();

	// Closes a wait opened by BeginWait(), charging it to the current frame. A zero start
	// is a wait that was never opened and is ignored.
	void EndGSWait(u64 begin_ticks);
	void EndVU1Wait(u64 begin_ticks);

	// --- lifecycle ------------------------------------------------------------------
	//
	// CPU thread only. Each of these either starts the evidence over or ends it; none of
	// them is on a per-frame path.

	// A VM is up and its settings and game database entry are resolved. Decides whether
	// the sampler runs at all, resets the controller to the configured baseline, and says
	// so once.
	void OnVMStart();

	// The VM is going away. Puts the effective selector back to the configured one, so the
	// next VM cannot inherit a transient decision, and stops sampling.
	void OnVMShutdown();

	// Anything that makes the accumulated evidence a statement about a machine that no
	// longer exists: VM reset, save-state load, settings apply, limiter-mode change,
	// pause and resume, frame advance. Restores the configured selector - which is also
	// how an eligibility loss that stops Throttle() from sampling at all still gets its
	// baseline back - and starts the window and the controller over.
	void OnLifecycleReset(const char* reason);

	// --- frame boundary -------------------------------------------------------------

	// One paced frame, from VMManager::Internal::Throttle() before the limiter sleeps.
	// Costs a handful of loads and stores; the window arithmetic and the one thread
	// CPU-time read happen at window close, twice a second.
	void OnFrameThrottled(u64 active_ticks, u64 target_ticks, bool late);

	// A frame Throttle() did not pace - the limiter is unlimited or host-vsync driven, or
	// execution was interrupted. The window in progress is marked unmeasurable rather
	// than having a sample invented for it.
	void OnFrameNotThrottled();
} // namespace EECycleRateSampler
