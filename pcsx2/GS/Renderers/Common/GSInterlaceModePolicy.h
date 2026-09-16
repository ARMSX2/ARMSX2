// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

struct GSInterlaceModeSelection
{
	int field_offset;
	int shader_mode;
	// Present the merge as it stands, with no deinterlace pass. Set only for Automatic + field
	// mode at an integer upscale of 2 or more, where the field render already holds every display
	// line of the screen at that field's moment and a weave could only replace half of them with
	// lines from a different one. shader_mode is -1 alongside it, so GSDevice::Interlace passes
	// through; the flag is what tells the caller the FFMD offset is now the only correction left.
	bool present_field_direct;
};

// shader_mode intentionally remains -1 for Automatic + full-frame output which does not need
// deinterlacing. GSDevice::Interlace() treats that value as a pass-through. Converting it to
// FastMAD would create and read temporal history during progressive/interlaced video-mode
// transitions, where no deinterlacing pass should run.
//
// Ported from sashkinbro/EmuCoreX ("Fix GS interlace and Vulkan presentation policies").
//
// field_render_is_whole_picture is the caller's verdict on the scale and the circuits: true only
// when every enabled circuit was rendered at an integer upscale of 2 or more, so that each field
// line occupies that many device rows. At 1x and at fractional scales a field render does NOT hold
// every display line and the weave still has work to do.
constexpr GSInterlaceModeSelection SelectGSInterlaceMode(
	int configured_mode, bool automatic, bool game_deinterlacing, bool ffmd, bool scanmask_frame,
	bool field_render_is_whole_picture = false)
{
	GSInterlaceModeSelection selection = {0, 3, false};
	if (!automatic || (!game_deinterlacing && !ffmd && !scanmask_frame))
	{
		selection.field_offset = configured_mode & 1;
		selection.shader_mode = (configured_mode < 2) ? -1 : ((configured_mode - 2) / 2);
	}
	else if (ffmd && !scanmask_frame && field_render_is_whole_picture)
	{
		selection.shader_mode = -1;
		selection.present_field_direct = true;
	}

	return selection;
}

static_assert(SelectGSInterlaceMode(0, true, false, false, false).shader_mode == -1);
static_assert(SelectGSInterlaceMode(0, true, false, true, false).shader_mode == 3);
static_assert(SelectGSInterlaceMode(8, false, false, false, false).shader_mode == 3);
static_assert(SelectGSInterlaceMode(0, true, false, true, false, true).shader_mode == -1);
static_assert(SelectGSInterlaceMode(0, true, false, true, false, true).present_field_direct);
// A scanmask frame keeps its deinterlace pass whatever the scale is, and an explicitly chosen mode
// is never taken over.
static_assert(!SelectGSInterlaceMode(0, true, false, true, true, true).present_field_direct);
static_assert(!SelectGSInterlaceMode(2, false, false, true, false, true).present_field_direct);
