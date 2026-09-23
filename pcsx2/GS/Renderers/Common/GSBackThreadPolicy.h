// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"

// What the GS back-thread setting resolves to for the renderer about to open.
//
// The setting names a mode, and the named mode is what the renderer gets, with one exception: a
// Pipelined split that cannot pipeline. Two cases cannot:
//
//   * a hardware renderer on a backend other than Vulkan -- the back thread would issue GL calls
//     off the thread that owns the context;
//   * Unsynchronized hardware downloads -- the EE thread reads live GS local memory with no lock
//     and no drain, so a back thread running behind the front leaves that memory arbitrarily far
//     behind what the EE expects.
//
// Both used to degrade quietly to a slower shape of the split (inline records, or lockstep, which
// drains the queue after every record and costs up to three times the frame time of Off). They
// now resolve to Off, which renders the same pixels at the single-thread speed. Lockstep and inline
// records remain reachable by asking for them; they are debugging rungs.
//
// Resolved before the renderer is constructed, because the renderer's constructor starts the back
// thread.

enum class GSBackThreadReason : u8
{
	/// The request named a mode and got it.
	Requested,
	/// Pipelined was requested, but the hardware renderer is not on Vulkan.
	BackendCannotQueue,
	/// Pipelined was requested, but downloads read live GS memory from the EE thread.
	LiveMemoryDownloads,
};

struct GSBackThreadInputs
{
	GSBackThreadMode requested = GSBackThreadMode::Off;
	/// A hardware renderer. The software renderer never touches the device off the present path,
	/// so it queues on any API.
	bool hardware_renderer = true;
	/// The device is Vulkan. Only read for a hardware renderer.
	bool vulkan = true;
	GSHardwareDownloadMode download_mode = GSHardwareDownloadMode::Enabled;
};

struct GSBackThreadDecision
{
	GSBackThreadMode mode = GSBackThreadMode::Off;
	GSBackThreadReason reason = GSBackThreadReason::Requested;
};

constexpr GSBackThreadDecision GSDecideBackThreadMode(const GSBackThreadInputs& in)
{
	if (in.requested != GSBackThreadMode::Pipelined)
		return {in.requested, GSBackThreadReason::Requested};

	if (in.hardware_renderer && !in.vulkan)
		return {GSBackThreadMode::Off, GSBackThreadReason::BackendCannotQueue};

	if (in.hardware_renderer && in.download_mode == GSHardwareDownloadMode::Unsynchronized)
		return {GSBackThreadMode::Off, GSBackThreadReason::LiveMemoryDownloads};

	return {GSBackThreadMode::Pipelined, GSBackThreadReason::Requested};
}

constexpr const char* GSBackThreadModeName(GSBackThreadMode mode)
{
	switch (mode)
	{
		case GSBackThreadMode::Off: return "off";
		case GSBackThreadMode::InlineRecords: return "inline records";
		case GSBackThreadMode::Lockstep: return "lockstep";
		case GSBackThreadMode::Pipelined: return "pipelined";
	}
	return "unknown";
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
