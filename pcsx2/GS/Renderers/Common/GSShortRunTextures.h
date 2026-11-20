// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

/// Which texture bindings each group of a short run pushes when the run is recorded with the ordinary calls
/// (GSDeviceVK::FlushRun). A group was taken with the texture flags the draw owed at that point, which can name bindings
/// its shaders do not read (every binding is owed after a run through the driver's entry; an untextured draw leaves the
/// previous texture and palette set). The group keeps only the textures it reads, so it cannot push the others. A
/// binding owed but not read is carried to the next group of the run that reads it, which pushes its own texture, and
/// what is still owed after the last group is marked dirty again. Bits are binding numbers, as in the texture flags.
///
/// Header-only so the tests can pin it without a device.
struct GSShortRunTextures
{
	u32 owed = 0;

	/// The bindings a group taken with texture flags `flags` and reading `reads` pushes.
	u32 Push(u32 flags, u32 reads)
	{
		owed |= flags;
		const u32 push = owed & reads;
		owed &= ~push;
		return push;
	}
};
