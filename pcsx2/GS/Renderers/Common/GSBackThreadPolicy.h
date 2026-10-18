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

// Where the back thread goes once the front thread (MTGS) has its processor, as masks of processors
// (VMManager.cpp builds them from the topology). Every draw crosses between front and back through a shared
// ring, so the back wants the front's cache; and both are busy, so it wants a processor of its own:
//
//   fast_cluster  the rest of the front's cluster, in the performance tier
//   fast_rest     the rest of the performance tier
//   others        every processor but the front's
//   occupied      processors pinned to other busy emu threads (EE, MTVU, IOP, the front itself)
//
// In order: the cluster's free processors, the tier's free processors, then the same two without regard to
// who is on them, then everything else. 0 (unpinned) only when there is nothing at all.
struct GSBackThreadPlacement
{
	u64 mask = 0;
	/// The mask includes a processor another busy emu thread is pinned to, so the back thread would compete
	/// with that thread for it. Measured on the MQ65 with the IOP thread on (EE, MTVU, MTGS and IOP on its four
	/// big cores): 66.6% without the back thread, 53.7% with it.
	bool shares_busy_core = false;
};

constexpr GSBackThreadPlacement GSPlaceBackThread(u64 fast_cluster, u64 fast_rest, u64 others, u64 occupied)
{
	const u64 candidates[] = {fast_cluster & ~occupied, fast_rest & ~occupied, fast_cluster, fast_rest, others};
	for (const u64 candidate : candidates)
	{
		if (candidate != 0)
			return {candidate, (candidate & occupied) != 0};
	}
	return {};
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
