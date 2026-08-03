// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Pins what a snapshot request does to a GS dump that is already recording, extracted from
// GSRenderer's VSync path so the decision is checkable without a GS device.
//
// The bug this suite exists for: the screenshot hotkey and the dump hotkeys shared one frame
// counter, so taking a screenshot mid-recording reset the recording's budget to zero and the
// next VSync closed the dump early. The recording also lost that frame's boundary, because the
// snapshot branch and the recording branch were alternatives rather than independent.

#include "GS/Renderers/Common/GSSnapshotPolicy.h"

#include <gtest/gtest.h>

// A screenshot is a request for zero dump frames -- the case that used to truncate.
static constexpr u32 SCREENSHOT = 0;
static constexpr u32 SINGLE_FRAME = 1;
static constexpr u32 MULTI_FRAME = 0xFFFFFFFFu;

TEST(GSSnapshotPolicy, IdleFrameDoesNothing)
{
	const GSSnapshotAction a = SelectGSSnapshotAction(false, SCREENSHOT, false, 0);
	EXPECT_FALSE(a.open_dump);
	EXPECT_FALSE(a.refuse_dump);
	EXPECT_FALSE(a.record_vsync);
}

TEST(GSSnapshotPolicy, DumpRequestWithNothingRecordingOpensADump)
{
	const GSSnapshotAction single = SelectGSSnapshotAction(true, SINGLE_FRAME, false, 0);
	EXPECT_TRUE(single.open_dump);
	EXPECT_FALSE(single.refuse_dump);
	// The dump starts from the state frozen this frame, so this frame's VSync is not part of it.
	EXPECT_FALSE(single.record_vsync);

	EXPECT_TRUE(SelectGSSnapshotAction(true, MULTI_FRAME, false, 0).open_dump);
}

TEST(GSSnapshotPolicy, ScreenshotRequestNeverOpensADump)
{
	const GSSnapshotAction a = SelectGSSnapshotAction(true, SCREENSHOT, false, 0);
	EXPECT_FALSE(a.open_dump);
	EXPECT_FALSE(a.refuse_dump);
}

// The regression. A screenshot taken while a multi-frame dump is running must leave the
// recording exactly as it was: still recording, still not the last frame.
TEST(GSSnapshotPolicy, ScreenshotDuringARecordingDoesNotEndIt)
{
	const GSSnapshotAction a = SelectGSSnapshotAction(true, SCREENSHOT, true, MULTI_FRAME);
	EXPECT_FALSE(a.open_dump);
	EXPECT_FALSE(a.refuse_dump);
	EXPECT_TRUE(a.record_vsync);
	EXPECT_FALSE(a.dump_is_last);
}

// ... and the frame it lands on still reaches the dump, or the recording loses a frame boundary
// and two guest frames merge into one on replay.
TEST(GSSnapshotPolicy, ScreenshotFrameIsStillRecorded)
{
	EXPECT_TRUE(SelectGSSnapshotAction(true, SCREENSHOT, true, SINGLE_FRAME).record_vsync);
	EXPECT_TRUE(SelectGSSnapshotAction(true, SCREENSHOT, true, 0).record_vsync);
}

// A second dump request cannot open a dump while one is recording, and must not silently do
// nothing either -- the caller gets a refusal it can report.
TEST(GSSnapshotPolicy, DumpRequestDuringARecordingIsRefusedNotSilent)
{
	for (const u32 frames : {SINGLE_FRAME, MULTI_FRAME})
	{
		const GSSnapshotAction a = SelectGSSnapshotAction(true, frames, true, MULTI_FRAME);
		EXPECT_FALSE(a.open_dump);
		EXPECT_TRUE(a.refuse_dump);
		// The running dump is untouched: it keeps recording and does not become the last frame.
		EXPECT_TRUE(a.record_vsync);
		EXPECT_FALSE(a.dump_is_last);
	}
}

// An exhausted budget is the only thing that closes a recording, and it closes it whether or not
// a snapshot happens to land on the same frame.
TEST(GSSnapshotPolicy, AnExhaustedBudgetIsWhatEndsARecording)
{
	EXPECT_TRUE(SelectGSSnapshotAction(false, SCREENSHOT, true, 0).dump_is_last);
	EXPECT_TRUE(SelectGSSnapshotAction(true, SCREENSHOT, true, 0).dump_is_last);
	EXPECT_FALSE(SelectGSSnapshotAction(false, SCREENSHOT, true, 1).dump_is_last);
}

TEST(GSSnapshotPolicy, NothingIsRecordedWhenNoDumpIsOpen)
{
	EXPECT_FALSE(SelectGSSnapshotAction(false, SCREENSHOT, false, 0).record_vsync);
	EXPECT_FALSE(SelectGSSnapshotAction(true, MULTI_FRAME, false, 0).record_vsync);
	EXPECT_FALSE(SelectGSSnapshotAction(false, SCREENSHOT, false, 0).dump_is_last);
}
