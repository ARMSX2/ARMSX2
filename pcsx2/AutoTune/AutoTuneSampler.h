// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Builds an AutoTuneWindow once per metrics window (about every half second) on the CPU thread.
//
// PerformanceMetrics closes a window on the GS thread. The sampler is polled once per frame on the CPU
// thread, sees the window's sequence number change, and combines that window's figures with what the CPU
// thread counted over the same stretch: the frames the limiter dropped, and the time the EE thread spent
// blocked on the GS, VU1 and IOP threads.
//
// Where the counts come from, and why they line up. The EE governor's sampler (EECycleRateSampler) keeps
// running totals at the frame limiter and at the MTGS/MTVU wait sites; auto-tune asks it to (RequestTally)
// and reads the totals at each delivery. The totals only grow, so a window's figures are the difference
// between two deliveries, and the share of time spent waiting is that difference divided by the time between
// the same two deliveries on the same clock. Numerator and denominator are the same stretch by construction,
// wherever the metrics window's edge fell on the CPU thread's timeline (it is closed on the GS thread, up to
// a few frames behind the EE). The governor's own windows are not used: they are half a second of game time,
// which is longer on the wall in a slow game and has no fixed relation to the metrics windows.
//
// Whether a window is usable is decided here from what the VM was doing: not paused, at the nominal
// limiter, not frame-advancing, not a GS dump. The warm-up after a boot and the settle after a change are
// the controller's, because they are about auto-tune's own actions.

#pragma once

#include "AutoTune/AutoTuneController.h"
#include "PerformanceMetrics.h"

#include <cstdint>

/// The CPU thread's running totals at one moment, as EECycleRateSampler keeps them. Two readings make a window.
struct AutoTuneTally
{
	/// The CPU thread's clock when the totals were read, in seconds. Only differences mean anything.
	double time_s = 0.0;
	/// Frames the limiter paced, and the ones among them that ended a whole frame or more after their
	/// deadline (the limiter had to move its schedule forward: a frame the player did not get on time).
	uint64_t frames = 0;
	uint64_t overran_frames = 0;
	/// Whether the wait totals below were being kept. False when nothing had asked for the tally.
	bool waits_measured = false;
	/// Time the EE thread spent blocked on the GS thread, on the VU1 thread and on the IOP thread, in seconds.
	/// The IOP figure is the EE's wait for the IOP thread to catch up (CheckLag) and to stop for a hold, from
	/// IopThread::Stats; it stays zero while the IOP runs inline.
	double gs_wait_s = 0.0;
	double vu1_wait_s = 0.0;
	double iop_wait_s = 0.0;
};

/// The totals now. CPU thread. Waits are unmeasured unless EECycleRateSampler::RequestTally(true) is in force.
AutoTuneTally AutoTuneReadLiveTally();

/// What the VM was doing, as far as deciding that a window is steady play.
struct AutoTuneConditions
{
	bool vm_running = false;
	bool nominal_limiter = false; // not turbo, slow-motion or unlimited
	bool frame_advancing = false;
	bool gs_dump = false;
	/// VMManager::GetTargetSpeed(): the speed the limiter aims for, as a multiple of the console's. The
	/// window's speed is divided by it, so "100%" always means "on target".
	double target_speed = 1.0;
};

class AutoTuneSampler
{
public:
	/// Treat the window with this sequence number as already seen, and take `baseline` as the reading the
	/// next window is measured from.
	void Reset(uint32_t seen_sequence, const AutoTuneTally& baseline);

	/// Builds the window if `snapshot` is a new one; false if it is the one already delivered (or none).
	/// `now` is the CPU thread's totals at this delivery; the window's late frames and waits are the change
	/// since the previous delivery (or since Reset). An unusable window still consumes them, so a pause does
	/// not leave its late frames to the first window after it.
	bool Poll(const PerformanceMetrics::WindowSnapshot& snapshot, const AutoTuneConditions& conditions,
		const AutoTuneTally& now, AutoTuneWindow* out);

	/// Poll against the live PerformanceMetrics and VMManager. CPU thread.
	bool PollLive(AutoTuneWindow* out);

	/// Reset against the live PerformanceMetrics.
	void ResetLive();

	uint32_t LastSequence() const { return m_last_sequence; }

private:
	uint32_t m_last_sequence = 0;
	AutoTuneTally m_previous;
	bool m_have_previous = false;
};
