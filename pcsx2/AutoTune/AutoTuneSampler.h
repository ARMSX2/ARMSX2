// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Builds an AutoTuneWindow once per metrics window (about every half second) on the CPU thread.
//
// PerformanceMetrics closes a window on the GS thread. The sampler is polled once per frame on the CPU
// thread, sees the window's sequence number change, and combines that window's figures with the late-frame
// count it has kept itself. Late frames are counted at the frame limiter (VMManager::Internal::Throttle),
// which runs on the CPU thread too, so the counters need no locking.
//
// Whether a window is usable is decided here from what the VM was doing: not paused, at the nominal
// limiter, not frame-advancing, not a GS dump. The warm-up after a boot and the settle after a change are
// the controller's, because they are about auto-tune's own actions.

#pragma once

#include "AutoTune/AutoTuneController.h"
#include "PerformanceMetrics.h"

#include <optional>

/// How long the EE thread waited on the GS and on VU1, as a share of the window. nullopt = not measured.
struct AutoTuneEeWaits
{
	std::optional<double> gs;
	std::optional<double> vu1;
};

/// The EE's wait times for the window that just closed, and a reset for the next one.
///
/// NOT MEASURED YET: this returns "unknown" for both. The measurement belongs to the EE governor's sampler
/// (PR #662, branch ee-governor-rebase), which times the EE thread at the MTGS and MTVU wait sites; when
/// that is merged, this function reads its per-window totals, divides by `window_seconds`, and clears them.
/// Until then the controller classifies without them (see AutoTuneController::Classify).
AutoTuneEeWaits AutoTuneSampleEeWaits(double window_seconds);

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
	/// One frame went through the limiter; `late` is true if it took two frame times or more.
	/// Call once per frame (not per limiter call), on the CPU thread.
	void NoteLimitedFrame(bool late);

	/// Forget the counters, and treat the window with this sequence number as already seen.
	void Reset(uint32_t seen_sequence = 0);

	/// Builds the window if `snapshot` is a new one; false if it is the one already delivered (or none).
	bool Poll(const PerformanceMetrics::WindowSnapshot& snapshot, const AutoTuneConditions& conditions,
		const AutoTuneEeWaits& waits, AutoTuneWindow* out);

	/// Poll against the live PerformanceMetrics and VMManager. CPU thread.
	bool PollLive(AutoTuneWindow* out);

	/// Reset against the live PerformanceMetrics.
	void ResetLive();

	uint32_t LastSequence() const { return m_last_sequence; }

private:
	uint32_t m_last_sequence = 0;
	uint32_t m_frames = 0;
	uint32_t m_late = 0;
};
