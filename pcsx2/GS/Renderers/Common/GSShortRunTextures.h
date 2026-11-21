// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

/// Which texture bindings each group of a short run pushes when the run is recorded with the ordinary calls
/// (GSDeviceVK::FlushRun), and which bindings the flush pushes once the waiting groups are recorded. A group was taken
/// with the texture flags the draw owed at that point, which can name bindings its shaders do not read (an untextured
/// draw leaves the previous texture and palette set). The group keeps only the textures it reads, so it cannot push the
/// others. A binding owed but not read is carried to the next group of the run that reads it, which pushes its own
/// texture.
///
/// The promise the flush keeps, whoever calls it: when it returns, set 1 holds the device's current texture for every
/// binding whose flag is clear. A flush usually runs inside the next draw's state setup, after that draw has read its
/// flags, so a binding marked dirty there would be pushed one draw too late. What is still owed after the last group,
/// and every binding after a run through the driver's entry (which leaves set 1 unknown), is therefore pushed by the
/// flush itself, from the device's current textures. Bits are binding numbers, as in the texture flags.
///
/// Header-only so the tests can pin it without a device.
struct GSShortRunTextures
{
	/// The set-1 binding numbers (GSDeviceVK's TFX_TEXTURE_*, which it asserts equal).
	enum : u32
	{
		TEXTURE,
		PALETTE,
		RT,
		PRIMID,
		DEPTH,
		RT_ROV,
		DEPTH_ROV,
		BINDINGS,
	};
	static constexpr u32 ALL = (1u << BINDINGS) - 1;

	u32 owed = 0;

	/// The bindings a group taken with texture flags `flags` and reading `reads` pushes.
	u32 Push(u32 flags, u32 reads)
	{
		owed |= flags;
		const u32 push = owed & reads;
		owed &= ~push;
		return push;
	}

	/// After the last group of a short run: the bindings the flush pushes from the current textures.
	u32 Finish()
	{
		const u32 left = owed;
		owed = 0;
		return left;
	}

	/// After a run through the driver's entry: the bindings the flush pushes from the current textures.
	static constexpr u32 AfterEntry() { return ALL; }
};
