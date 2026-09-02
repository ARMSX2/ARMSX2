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

	/// Number of frames in one pass over the loaded dump, i.e. its VSync packet count.
	/// GetFrameNumber() resets to zero on every wrap, so this is what turns a frame
	/// index back into "which loop, and how far into it". Zero when no dump is loaded.
	u32 GetDumpFrameCount();

	/// Called on the CPU thread after each packet is dispatched, with that packet's
	/// index in the dump and whether it was a vsync. Nothing in the core sets this;
	/// gsrunner uses it to schedule a readback at an exact point in the stream, which
	/// is what lets a hardware capture and a local run name the same rung.
	using PacketHook = void (*)(u32 packet_index, bool is_vsync);
	void SetPacketHook(PacketHook hook);

	/// Called once, after the dump's freeze has been applied and before the first
	/// packet runs. That instant is the local twin of the console replayer's
	/// post-restore control, and it is not reachable from the packet hook: by the
	/// time the first packet reports, its own effect is already in memory.
	using InitialStateHook = void (*)();
	void SetInitialStateHook(InitialStateHook hook);

	/// While enabled, every packet publishes its own index to the GS thread ahead of its
	/// work, so per-draw instrumentation running there can name the packet a draw came
	/// out of. Off by default -- it costs a queued store per packet, and outside a replay
	/// there is no packet to name.
	///
	/// It has to travel down the GS queue rather than being read from the replayer's
	/// counter: the CPU thread runs ahead of the GS thread, so a direct read is not the
	/// packet whose draws are executing. The wrong answer looks entirely reasonable.
	void SetPublishPacketMarks(bool enabled);

	void RenderUI();
} // namespace GSDumpReplayer
