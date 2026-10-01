// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"

// What the GS multi-threading setting resolves to for the renderer about to open.
//
// A request for it is granted unless the split cannot work. Two cases cannot:
//
//   * a hardware renderer on a backend other than Vulkan -- the back thread would issue GL calls
//     off the thread that owns the context;
//   * Unsynchronized hardware downloads -- the EE thread reads live GS local memory with no lock
//     and no drain, so a back thread running behind the front leaves that memory arbitrarily far
//     behind what the EE expects.
//
// Both resolve to off, which renders the same pixels single-threaded.
//
// Resolved before the renderer is constructed, because the renderer's constructor starts the back
// thread.

enum class GSBackThreadReason : u8
{
	/// The setting was granted as asked (on or off).
	Requested,
	/// Requested, but the hardware renderer is not on Vulkan.
	BackendCannotQueue,
	/// Requested, but downloads read live GS memory from the EE thread.
	LiveMemoryDownloads,
};

struct GSBackThreadInputs
{
	bool requested = false;
	/// A hardware renderer. The software renderer never touches the device off the present path,
	/// so it queues on any API.
	bool hardware_renderer = true;
	/// The device is Vulkan. Only read for a hardware renderer.
	bool vulkan = true;
	GSHardwareDownloadMode download_mode = GSHardwareDownloadMode::Enabled;
};

struct GSBackThreadDecision
{
	bool on = false;
	GSBackThreadReason reason = GSBackThreadReason::Requested;
};

constexpr GSBackThreadDecision GSDecideBackThread(const GSBackThreadInputs& in)
{
	if (!in.requested)
		return {false, GSBackThreadReason::Requested};

	if (in.hardware_renderer && !in.vulkan)
		return {false, GSBackThreadReason::BackendCannotQueue};

	if (in.hardware_renderer && in.download_mode == GSHardwareDownloadMode::Unsynchronized)
		return {false, GSBackThreadReason::LiveMemoryDownloads};

	return {true, GSBackThreadReason::Requested};
}

constexpr const char* GSBackThreadReasonText(GSBackThreadReason reason)
{
	switch (reason)
	{
		case GSBackThreadReason::Requested: return "as set";
		case GSBackThreadReason::BackendCannotQueue: return "the split needs Vulkan for the hardware renderer";
		case GSBackThreadReason::LiveMemoryDownloads: return "unsynchronized hardware downloads read live GS memory";
	}
	return "unknown";
}
