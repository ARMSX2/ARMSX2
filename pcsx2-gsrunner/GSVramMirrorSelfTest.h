// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

/// gsrunner -gpu-decode-selftest: the GPU texture decode (GSVramMirrorVK) against the CPU readers,
/// on whatever Vulkan device the runner opened. Debug-only, not a setting.
namespace GSVramMirrorSelfTest
{
	struct Options
	{
		u32 seed = 1;
		/// Random windows per paletted format. The design's gate is 1000.
		u32 windows_per_format = 1000;
	};

	/// Runs on the GS thread, with the Vulkan device open. Prints one GPU-DECODE-SELFTEST line per
	/// phase and a result line, and returns 0 only if every comparison matched.
	int Run(const Options& options);

	/// -vram-mirror-verify. Call before the device is created: every GSVramMirrorVK made after it keeps a
	/// CPU copy of what the mirror holds, checks the dirty marks against it as windows are queued, and
	/// reads the mirror and each decoded texture back to compare them (GSVramMirrorVK::SetVerify).
	void SetVerify(bool enable);

	/// Prints one VRAM-MIRROR-VERIFY line (after the device is gone: the counts are process-wide) and
	/// returns false if any comparison failed.
	bool ReportVerify();
} // namespace GSVramMirrorSelfTest
