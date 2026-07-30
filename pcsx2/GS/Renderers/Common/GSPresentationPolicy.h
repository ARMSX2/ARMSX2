// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Vulkan records GS work before presentation. A blank before the first GS output is startup noise
// and can be omitted, but a mid-game blank must reach the normal present path so Vulkan submits the
// commands recorded for a fade/transition instead of carrying incomplete state into the next frame.
//
// OpenGL does not have that deferred command-buffer hazard, so retain Android's existing alternating
// blank suppression there to avoid changing its already-correct output.
//
// Ported from sashkinbro/EmuCoreX ("Fix GS interlace and Vulkan presentation policies").
constexpr bool ShouldSkipAndroidBlankFrame(
	bool blank_frame, bool has_current_output, bool is_vulkan, unsigned consecutive_blank_frames)
{
	if (!blank_frame)
		return false;

	return is_vulkan ? !has_current_output : (consecutive_blank_frames == 1);
}

static_assert(ShouldSkipAndroidBlankFrame(true, false, true, 1));
static_assert(!ShouldSkipAndroidBlankFrame(true, true, true, 1));
static_assert(ShouldSkipAndroidBlankFrame(true, true, false, 1));
static_assert(!ShouldSkipAndroidBlankFrame(false, false, true, 0));
