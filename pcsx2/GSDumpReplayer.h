// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Error.h"

#include <string>
#include <vector>

namespace GSDumpReplayer
{
	bool IsReplayingDump();

	/// If set, playback will repeat once it reaches the last frame.
	void SetLoopCount(s32 loop_count = 0);
	int GetLoopCount();
	bool IsRunner();
	void SetIsDumpRunner(bool is_runner);

	bool Initialize(const char* filename, Error* error = nullptr);
	bool ChangeDump(const char* filename);
	void Shutdown();

	std::string GetDumpSerial();
	u32 GetDumpCRC();

	u32 GetFrameNumber();

	/// Called on the CPU thread after each packet is dispatched, with that packet's
	/// index in the dump and whether it was a vsync. Nothing in the core sets this;
	/// gsrunner uses it to schedule a readback at an exact point in the stream, which
	/// is what lets a hardware capture and a local run name the same rung.
	using PacketHook = void (*)(u32 packet_index, bool is_vsync);
	void SetPacketHook(PacketHook hook);

	void RenderUI();
} // namespace GSDumpReplayer
