// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cmath>

struct GSInterlaceModeSelection
{
	int field_offset;
	int shader_mode;
	// Present the merge with no deinterlace pass. Set only for Automatic + field mode at integer
	// upscale >= 2, where the field render already holds every display line and a weave would replace
	// half of them with the other field's. shader_mode is -1 alongside it (GSDevice::Interlace passes
	// through); the flag tells the caller the FFMD offset is the only correction left.
	bool present_field_direct;
};

// shader_mode intentionally remains -1 for Automatic + full-frame output which does not need
// deinterlacing. GSDevice::Interlace() treats that value as a pass-through. Converting it to
// FastMAD would create and read temporal history during progressive/interlaced video-mode
// transitions, where no deinterlacing pass should run.
//
// Ported from sashkinbro/EmuCoreX ("Fix GS interlace and Vulkan presentation policies").
//
// field_render_is_whole_picture: true only when every enabled circuit was rendered at integer
// upscale >= 2, so each field line covers that many device rows. At 1x or fractional scales the
// weave is still needed.
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
// A scanmask frame keeps its deinterlace pass at any scale; an explicit mode is never overridden.
static_assert(!SelectGSInterlaceMode(0, true, false, true, true, true).present_field_direct);
static_assert(!SelectGSInterlaceMode(2, false, false, true, false, true).present_field_direct);

// ---------------------------------------------------------------------------------------------
// The undrawn field band
//
// In FFMD mode (outside field-direct presentation) GSRenderer::Merge draws one field a native line
// lower, leaving undrawn rows between the display rect's top and the shifted picture's top. The
// weave and MAD buffering passes read the first drawn row instead of that hole.
//
// The band starts at the RECT top, not the merge-target top; rows above a lower rect are background
// and are read as themselves.

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
