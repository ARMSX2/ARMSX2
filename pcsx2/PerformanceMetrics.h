// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include "common/Threading.h"

namespace PerformanceMetrics
{
	enum class InternalFPSMethod
	{
		None,
		GSPrivilegedRegister,
		DISPFBBlit
	};

	class AverageFPS
	{
	private:
		static constexpr size_t FPS_BUFFER_SIZE = 16;
		std::array<float, FPS_BUFFER_SIZE> fps_buff{};
		size_t count = 0;
		size_t pos = 0;
		float avg_fps = 0.0f;

	public:
		void ClearStats()
		{
			count = 0;
			pos = 0;
			fps_buff.fill(0.0f);
			avg_fps = 0.0f;
		}

		void UpdateAvgFPS(float new_fps_val)
		{
			// If the change is quite dramatic, the user has either turned off the frame limiter or the game is dying.
			// Clear it to catch it up quickly.
			if (count > 0)
			{
				const float last_fps_val = fps_buff[(pos - 1) & (FPS_BUFFER_SIZE - 1)];
				if (std::abs(last_fps_val - new_fps_val) > last_fps_val * 0.20f)
					ClearStats();
			}

			fps_buff[pos] = new_fps_val;
			pos = (pos + 1) & (FPS_BUFFER_SIZE - 1); // if you use values which don't become all 1's, make it %.
			count = std::min(count + 1, FPS_BUFFER_SIZE);

			float avg_sum = 0.0f;

			for (size_t i = 0; i < count; i++)
			{
				avg_sum += fps_buff[i];
			}

			avg_fps = avg_sum / static_cast<float>(count);
		}

		float GetAvgFPS()
		{
			return avg_fps;
		}
	};

	static constexpr u32 NUM_FRAME_TIME_SAMPLES = 150;
	using FrameTimeHistory = std::array<float, NUM_FRAME_TIME_SAMPLES>;

	void Clear();
	void Reset();
	void Update(bool gs_register_write, bool fb_blit, bool is_skipping_present);
	void OnGPUPresent(float gpu_time, u64 vs_invocations, u64 ps_invocations);

	/// Logs the whole-session average framerate (frames since Clear() over
	/// wall time). Called at VM shutdown so every -logfile run records it.
	void LogSessionSummary();

	/// Android ADPF (PerformanceHintManager): register the calling thread as perf-critical
	/// so the OS ramps its CPU/GPU clocks toward the frame deadline instead of leaving them
	/// low under emulation's bursty load. Called on the EE (CPU), GS and MTVU threads.
	/// No-op on non-Android and below API 33 (symbols resolved via dlsym).
	void AdpfRegisterCallingThread();
	/// Enable/disable ADPF hinting at runtime (settings toggle). Default OFF (experimental).
	void AdpfSetEnabled(bool enabled);
	/// Close the ADPF session and forget registered threads (VM shutdown).
	void AdpfShutdown();

	/// ADPF work-period brackets, driven by the frame limiter (VMManager::Internal::Throttle).
	/// The reported duration must be the active EE/GS/VU work per frame, EXCLUDING the deliberate
	/// limiter sleep and present wait, per the PerformanceHintManager contract (reportActualWork
	/// = the last workload cycle, not the frame interval). The limiter waits twice per frame, at
	/// vsync start and at vsync end, so a frame's work is two segments. EndWorkSegment() closes the
	/// segment that just ended (called at Throttle entry, before the sleep) and, at the frame's
	/// last wait, reports both segments as one period; BeginFrameWork() opens the next segment
	/// after the sleep; PauseFrameWork() invalidates the frame when we are not frame-limiting
	/// (unlimited / host-vsync pacing / interrupted), so no bogus duration is reported.
	void AdpfEndWorkSegment(bool frame_end);
	void AdpfBeginFrameWork();
	void AdpfPauseFrameWork();

	/// Sets the EE thread for CPU usage calculations.
	void SetCPUThread(Threading::ThreadHandle thread);

	/// Sets timers for GS software threads.
	void SetGSSWThreadCount(u32 count);
	void SetGSSWThread(u32 index, Threading::ThreadHandle thread);

	/// Sets the timer for the GS back thread (exists only with GS multi-threading on). Registered by
	/// the back thread itself at entry and cleared once it has joined; an empty handle means
	/// no such thread exists, which is the default configuration. Under the pipelined split
	/// the GS work is roughly halved between this thread and the MTGS thread, so the plain
	/// "GS" figure alone reads as a ~50% drop in GS cost that never happened.
	void SetGSBackThread(Threading::ThreadHandle thread);

	u64 GetFrameNumber();

	InternalFPSMethod GetInternalFPSMethod();
	bool IsInternalFPSValid();

	float GetFPS();
	float GetAvgVPS();
	float GetInternalFPS();
	float GetSpeed();
	float GetAverageFrameTime();
	float GetMinimumFrameTime();
	float GetMaximumFrameTime();

	double GetCPUThreadUsage();
	double GetCPUThreadAverageTime();
	float GetGSThreadUsage();
	float GetGSThreadAverageTime();
	/// True while a GS back thread is registered. Both figures below read zero when it is not.
	bool HasGSBackThread();
	float GetGSBackThreadUsage();
	float GetGSBackThreadAverageTime();
	float GetVUThreadUsage();
	float GetVUThreadAverageTime();

	u32 GetGSSWThreadCount();
	double GetGSSWThreadUsage(u32 index);
	double GetGSSWThreadAverageTime(u32 index);

	float GetGPUUsage();
	float GetGPUAverageTime();
	/// GPU time for the most recent present only, not a window average. Used by the
	/// per-frame stats series, where an averaged value would hide the spike.
	float GetLastGPUTime();
	double GetGPUAverageVSInvocations();
	double GetGPUAveragePSInvocations();

	const FrameTimeHistory& GetFrameTimeHistory();
	u32 GetFrameTimeHistoryPos();
} // namespace PerformanceMetrics
