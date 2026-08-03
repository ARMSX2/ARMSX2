// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// What the next VSync should do about a queued snapshot request and a dump that may already be
// recording. The two are independent: a screenshot is a request to write one image, a recording
// is a budget of frames already being spent, and neither may reach into the other.
//
// They used to share a single frame counter, so the screenshot hotkey (a request for zero dump
// frames) zeroed the budget of a running multi-frame dump and the following VSync closed it as
// though the user had asked it to stop. The two branches were also alternatives rather than
// independent, so the frame a screenshot landed on never reached the dump and two guest frames
// merged into one on replay.
struct GSSnapshotAction
{
	/// Freeze state and start a recording from this frame.
	bool open_dump;
	/// The request asked for a dump but one is already recording. Only one can exist, so the
	/// request yields -- but it must say so rather than quietly writing just the screenshot.
	bool refuse_dump;
	/// Hand this frame's VSync to a recording that is already running.
	bool record_vsync;
	/// ... and tell it to close after this one.
	bool dump_is_last;
};

/// `requested_dump_frames` is what the queued request asked for (0 = screenshot only);
/// `dump_frames_remaining` is the budget of the recording already running, which no request
/// can touch.
constexpr GSSnapshotAction SelectGSSnapshotAction(
	bool snapshot_pending, u32 requested_dump_frames, bool dump_open, u32 dump_frames_remaining)
{
	const bool wants_dump = snapshot_pending && requested_dump_frames > 0;
	return GSSnapshotAction{
		wants_dump && !dump_open,
		wants_dump && dump_open,
		// A recording takes every frame it is open for, including one a snapshot lands on. Not
		// the frame it was opened on: that state was frozen into the dump's header, so replay
		// starts from the frame after.
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
