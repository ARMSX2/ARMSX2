// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cmath>

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

// ---------------------------------------------------------------------------------------------
// The undrawn field band
//
// In FFMD mode (outside the field-direct presentation) GSRenderer::Merge draws one field a native
// line lower than the other, so the merge target holds no picture in the device rows between where
// that circuit's display rect starts and where its shifted picture starts. The weave and MAD
// buffering passes read the first drawn row instead of that hole.
//
// The band is where the RECT starts, not at the top of the merge target: a display rect that
// starts lower on the screen leaves its hole lower too, and the rows above it are the background,
// which must be read as themselves.

/// Device rows [first, end) of the merge target that the shifted field left undrawn.
struct GSFieldPadRows
{
	float first = 0.0f;
	float end = 0.0f;
};

/// `unshifted_top` and `shifted_top` are the circuit's destination rect top before and after the
/// shift, in device pixels. A row is drawn when its centre is at or below a rect's top edge, so the
/// first drawn row of a rect starting at y is ceil(y - 0.5).
inline GSFieldPadRows GSComputeFieldPadRows(float unshifted_top, float shifted_top)
{
	const float first = std::ceil(unshifted_top - 0.5f);
	const float end = std::ceil(shifted_top - 0.5f);
	return {first, (end > first) ? end : first};
}

/// The merge-target row a deinterlace pass reads for destination row `row`. The shaders run this
/// same expression with FieldPad.xy = (first, end).
inline float GSFieldPadSourceRow(float row, const GSFieldPadRows& pad)
{
	return (row >= pad.first && row < pad.end) ? pad.end : row;
}
