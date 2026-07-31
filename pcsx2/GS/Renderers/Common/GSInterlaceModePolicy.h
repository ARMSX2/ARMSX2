// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

struct GSInterlaceModeSelection
{
	int field_offset;
	int shader_mode;
};

// shader_mode intentionally remains -1 for Automatic + full-frame output which does not need
// deinterlacing. GSDevice::Interlace() treats that value as a pass-through. Converting it to
// FastMAD would create and read temporal history during progressive/interlaced video-mode
// transitions, where no deinterlacing pass should run.
//
// Ported from sashkinbro/EmuCoreX ("Fix GS interlace and Vulkan presentation policies").
constexpr GSInterlaceModeSelection SelectGSInterlaceMode(
	int configured_mode, bool automatic, bool game_deinterlacing, bool ffmd, bool scanmask_frame)
{
	GSInterlaceModeSelection selection = {0, 3};
	if (!automatic || (!game_deinterlacing && !ffmd && !scanmask_frame))
	{
		selection.field_offset = configured_mode & 1;
		selection.shader_mode = (configured_mode < 2) ? -1 : ((configured_mode - 2) / 2);
	}

	return selection;
}

static_assert(SelectGSInterlaceMode(0, true, false, false, false).shader_mode == -1);
static_assert(SelectGSInterlaceMode(0, true, false, true, false).shader_mode == 3);
static_assert(SelectGSInterlaceMode(8, false, false, false, false).shader_mode == 3);
