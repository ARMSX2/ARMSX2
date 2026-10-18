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
	// Wall time from each limiter post-sleep return to the next Throttle() entry, summed
	// over the frame's two segments (vsync start and vsync end): the frame's active work
	// with the deliberate limiter sleeps taken out. It is wall time, not CPU time, so it
	// includes any stretch the EE thread spent blocked.
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

	// The limiter's own verdict that this frame missed its deadline, taken at the
	// vsync-end Throttle() entry. It is carried separately from active_ticks because the
	// limiter sleeps inside the frame (to 0.9 of the budget at vsync start), so a frame
	// whose second segment overran its small share is late without its summed active
	// time reaching the budget. Keeping the verdict beside the array also keeps the
	// window arithmetic a pure function of it rather than a re-derivation of the
	// limiter's rule.
	bool late = false;
};

// How a paced segment ended against the limiter's deadline. One measurement at two
// thresholds, taken once per Throttle() call so that everything that cares about lateness
// reads the same verdict:
//
//   Late     the segment ended after its deadline. The governor gates on this: a frame that
//            misses its deadline at all is deadline pressure the EE thread may be the cause
//            of, even though the next frame can still catch up.
//   Overran  it ended a whole frame or more after it. The limiter gives up on the schedule
//            and moves the frame start forward, so this is a frame the player does not see
//            on time. Auto-tune gates on this: a game that makes its deadline by a hair on
//            half its frames is running at full speed and has nothing to fix.
//
// Overran implies Late.
enum class EECycleRateLateness : u8
{
	OnTime,
	Late,
	Overran,
};

// Folds the limiter's two waits per frame into one frame. The limiter waits at vsync start
// and again at vsync end, and the work between waits is what PerformanceMetrics measures,
// so a frame arrives as two segments. Pure: no clock, no VM, no globals, so the rules
// (segments add, only the vsync-end wait closes a frame, a missing segment makes the frame
// unmeasurable, any late verdict sticks) are testable without a limiter.
struct EECycleRateFrameAccumulator
{
	enum class Outcome : u8
	{
		// A vsync-start segment: the frame is still open.
		Pending,
		// The vsync-end segment closed a measured frame; active_ticks and late describe it.
		Closed,
		// The vsync-end segment closed a frame with a missing segment; its length is unknown.
		Unmeasured,
	};

	u64 active_ticks = 0;
	bool unmeasured = false;
	bool late = false;
	bool overran = false;

	// segment_ticks is zero when no work period was open at this wait (the previous frame
	// was not paced, or a pause landed inside this one). `lateness` is the limiter's verdict
	// at this wait; any verdict worse than OnTime marks the frame late, and Overran marks it
	// overran. After Closed or Unmeasured the fields stay readable until Clear().
	Outcome AddSegment(u64 segment_ticks, bool frame_end, EECycleRateLateness lateness)
	{
		if (segment_ticks == 0)
			unmeasured = true;
		active_ticks += segment_ticks;
		late |= (lateness != EECycleRateLateness::OnTime);
		overran |= (lateness == EECycleRateLateness::Overran);

		if (!frame_end)
			return Outcome::Pending;
		return unmeasured ? Outcome::Unmeasured : Outcome::Closed;
	}

	void Clear() { *this = EECycleRateFrameAccumulator(); }
};

// Running totals of the CPU thread's frames and of the time its EE spent blocked, for
// consumers that are not the governor (auto-tune). They only grow, so a reader takes the
// difference between two readings and divides by the time between them on its own clock:
// numerator and denominator then cover the same stretch by construction, wherever the
// governor's windows happen to fall. The governor's windows are half a second of game time,
// which is longer on the wall in a slow game and has no fixed relation to any other window.
struct EECycleRateTally
{
	// Paced frames the limiter closed, measurable or not.
	u64 frames = 0;
	// ...of which ended a whole frame or more after their deadline (EECycleRateLateness::Overran).
	u64 overran_frames = 0;
	// Wall time the EE thread spent waiting on the GS thread and on the VU1 thread, in the
	// GetCPUTicks() domain. Both sleeping and spinning waits count.
	u64 gs_wait_ticks = 0;
	u64 vu1_wait_ticks = 0;

	// A frame the accumulator has just closed (Closed or Unmeasured).
	void NoteFrame(const EECycleRateFrameAccumulator& closed)
	{
		frames++;
		overran_frames += closed.overran ? 1 : 0;
	}

	// A wait from `begin_ticks` to `end_ticks`. A clock that did not advance adds nothing.
	void NoteGSWait(u64 begin_ticks, u64 end_ticks)
	{
		if (end_ticks > begin_ticks)
			gs_wait_ticks += end_ticks - begin_ticks;
	}

	void NoteVU1Wait(u64 begin_ticks, u64 end_ticks)
	{
		if (end_ticks > begin_ticks)
			vu1_wait_ticks += end_ticks - begin_ticks;
	}
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

	// One paced segment of a frame, from VMManager::Internal::Throttle() before the
	// limiter sleeps. The limiter waits twice per frame, so a frame is two segments: the
	// vsync-start call (frame_end false) accumulates its length and returns; the
	// vsync-end call (frame_end true) adds the second segment and closes the frame.
	// Costs a handful of loads and stores; the window arithmetic and the one thread
	// CPU-time read happen at window close, twice a second.
	//
	// segment_ticks is the work period PerformanceMetrics closed for this wait, zero when
	// none was open. A frame with a zero segment has no known length, so it makes its
	// window unmeasurable rather than entering the window as a short frame.
	//
	// `lateness` is the limiter's verdict for this segment (see EECycleRateLateness). On the
	// vsync-end call it is taken against the full-frame deadline; on the vsync-start call it
	// is OnTime or Overran, because that wait's own deadline is 0.9 of the frame and says
	// nothing about the frame except when it overran by a whole frame - which moves the frame
	// start and would hide the lateness from the vsync-end verdict. The worst verdict of the
	// two segments is the frame's.
	//
	// Runs while the governor is measuring or a tally is requested; otherwise it is a
	// not-taken branch.
	void OnSegmentThrottled(u64 segment_ticks, u64 target_ticks, bool frame_end, EECycleRateLateness lateness);

	// A segment Throttle() did not pace - the limiter is unlimited or host-vsync driven,
	// or execution was interrupted. The window in progress is marked unmeasurable rather
	// than having a sample invented for it, and the frame in progress is dropped.
	void OnFrameNotThrottled();

	// --- reading the latest window ----------------------------------------------------
	//
	// For other consumers of the governor's measurements. CPU thread only: the sampler's
	// state is CPU-thread owned, and unlike the overlay word nothing here is published for
	// other threads.
	//
	// Windows are produced only while the sampler runs: the governor is enabled for this
	// game, or a trace path puts it in shadow mode. With both off there is nothing to read.
	struct LastWindow
	{
		// Counts closed windows, usable or not, for the whole session. A reader that
		// remembers the last serial it handled knows whether a new window has closed.
		u32 serial = 0;

		// The window's frames as one sample, including the p90 active ratio, late ratio,
		// own-time ratio, blocked ratio and the GS and VU1 shares of it. See
		// EECycleRateWindowSample for what each one divides by.
		EECycleRateWindowSample sample;

		// EECycleRateController::IsSampleUsable(sample): measured, non-empty and finite.
		// A window that is not usable carries numbers that mean nothing.
		bool usable = false;

		// The selectors in force while the window's frames ran. The governor's decision at
		// the end of the window may already have moved the live one.
		s8 configured = 0;
		s8 effective = 0;
	};

	// False until a window has closed since the sampler last started over (VM start, VM
	// reset, save-state load, pause, settings apply, limiter change, frame advance), because
	// a window from before any of those describes a machine that no longer exists.
	bool GetLastWindow(LastWindow* out);

	// --- the tally -------------------------------------------------------------------
	//
	// Frame verdicts and EE wait totals for a consumer that is not the governor. The governor's
	// own sampler is off unless the governor is enabled for the game or a trace path is set, so
	// a consumer asks for the part it needs. A request starts the wait timing at the blocking
	// sites and the per-frame counting, and does not make the governor act: nothing here feeds
	// the controller. Cost while requested: two clock reads on a path that is already about to
	// block, and a few loads and adds twice per frame.
	//
	// CPU thread only.

	// Starts or stops counting for the tally. Idempotent.
	void RequestTally(bool wanted);

	// The totals so far, since the VM started or since counting began, whichever is later. False
	// when no tally is requested: the totals are not being kept and would read as zero waits.
	bool GetTally(EECycleRateTally* out);

	// --- overlay ---------------------------------------------------------------------
	//
	// What the on-screen display is allowed to know, as one packed word published by the
	// CPU thread. A word rather than the fields, because the overlay runs on the GS thread
	// and a field-by-field read can pair a configured selector from one window with an
	// effective selector from the next - which is exactly the frame a screenshot of a bug
	// report gets taken on. This is the only cross-thread read of governor state there is.
	struct OverlayState
	{
		s8 configured = 0;
		s8 effective = 0;
		EECycleRateController::State state = EECycleRateController::State::Disabled;

		// The governor is live: it may lower the selector.
		bool enabled = false;

		// Measuring and deciding into a trace, applying nothing.
		bool shadow = false;
	};

	// Pure, and each other's inverse over the whole selector range.
	u32 PackOverlayWord(const OverlayState& state);
	OverlayState UnpackOverlayWord(u32 word);

	// The published word. Any thread. A word whose enabled and shadow bits are both clear
	// says the governor is not running, and the display should fall back to the configured
	// settings it already reads.
	u32 GetOverlayWord();
} // namespace EECycleRateSampler
