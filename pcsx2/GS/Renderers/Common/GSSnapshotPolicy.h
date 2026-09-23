// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// What the next VSync should do about a queued snapshot request and a dump that may already be
// recording. The two are independent: a screenshot writes one image, a recording spends a frame
// budget, and neither may touch the other. In particular a screenshot must not zero a running
// dump's budget, and the frame a screenshot lands on must still reach the dump (otherwise two
// guest frames merge into one on replay).
struct GSSnapshotAction
{
	/// Freeze state and start a recording from this frame.
	bool open_dump;
	/// A dump was requested while one is recording. Only one can exist, so the request is refused,
	/// and must be reported rather than silently writing just the screenshot.
	bool refuse_dump;
	/// Hand this frame's VSync to a recording that is already running.
	bool record_vsync;
	/// ... and tell it to close after this one.
	bool dump_is_last;
};

/// `requested_dump_frames`: the queued request (0 = screenshot only). `dump_frames_remaining`:
/// the running recording's budget, which no request can touch.
constexpr GSSnapshotAction SelectGSSnapshotAction(
	bool snapshot_pending, u32 requested_dump_frames, bool dump_open, u32 dump_frames_remaining)
{
	const bool wants_dump = snapshot_pending && requested_dump_frames > 0;
	return GSSnapshotAction{
		wants_dump && !dump_open,
		wants_dump && dump_open,
		// A recording takes every frame it is open for, including one a snapshot lands on, but not
		// the frame it was opened on (frozen into the header; replay starts after it).
		dump_open,
		dump_open && dump_frames_remaining == 0,
	};
}

// A screenshot mid-recording changes nothing about the recording.
static_assert(!SelectGSSnapshotAction(true, 0, true, 8).open_dump);
static_assert(!SelectGSSnapshotAction(true, 0, true, 8).refuse_dump);
static_assert(SelectGSSnapshotAction(true, 0, true, 8).record_vsync);
static_assert(!SelectGSSnapshotAction(true, 0, true, 8).dump_is_last);
// A dump request mid-recording is refused, and equally leaves it alone.
static_assert(SelectGSSnapshotAction(true, 1, true, 8).refuse_dump);
static_assert(!SelectGSSnapshotAction(true, 1, true, 8).open_dump);
// An exhausted budget is the only thing that ends a recording.
static_assert(SelectGSSnapshotAction(false, 0, true, 0).dump_is_last);
// The frame a dump is opened on belongs to its frozen state, not to its frame stream.
static_assert(SelectGSSnapshotAction(true, 1, false, 0).open_dump);
static_assert(!SelectGSSnapshotAction(true, 1, false, 0).record_vsync);
