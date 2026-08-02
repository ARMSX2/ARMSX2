// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Loader for legacy-format savestates: upstream-PCSX2 formats of the
// AetherSX2/NetherSX2 era. The container (zip entry set) is unchanged across
// eras; what differs is the layout of the versioned blobs. The supported
// legacy majors and the blob reader live in SaveStateLegacy.cpp; the per-entry
// legacy handling (GS/SPU2/PAD) hangs off SaveState_UnzipFromZip.
namespace SaveStateLegacy
{
	// The legacy savestate majors the in-engine reader understands:
	//   0x9A2C — upstream PCSX2 at 0312e902 (the AetherSX2 v1.5-era format)
	//   0x9A34 — upstream PCSX2 at 7e939b75 (the NetherSX2 v2.1 format)
	bool IsSupportedVersion(u32 savever);

	// Widens a wrapping 32-bit cycle counter to 64 bits relative to a domain
	// base: the signed 32-bit delta to the old base is preserved against the
	// new base. Correct across u32 wraps, which zero-extension is not.
	__fi u64 WidenCycle(u32 old_value, u32 old_base, u64 new_base)
	{
		return new_base + static_cast<s64>(static_cast<s32>(old_value - old_base));
	}
} // namespace SaveStateLegacy
