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

	/// The CPU thread's frame-work clock, driven by the frame limiter
	/// (VMManager::Internal::Throttle). The period it measures is the frame's active EE/GS/VU
	/// work, EXCLUDING the deliberate limiter sleep: OnFrameWorkComplete() closes the period
	/// that just ended (called at Throttle entry, before the sleep) and returns its length in
	/// GetCPUTicks() units; OnFrameWorkBegin() opens a new one after the sleep;
	/// OnFrameWorkPaused() invalidates it when we are not frame-limiting (unlimited /
	/// host-vsync pacing / execution interrupted), so no wall-time-with-wait duration is
	/// measured. Zero back from OnFrameWorkComplete() means no period was open.
	///
	/// Two consumers share this one timestamp: the Android ADPF hint, which reports the
	/// duration to PerformanceHintManager (reportActualWork = the last workload cycle, not the
	/// frame interval), and the dynamic EE cycle-rate sampler, which is the same measurement
	/// on every platform.
	u64 OnFrameWorkComplete(u64 now_ticks);
	void OnFrameWorkBegin();
	void OnFrameWorkPaused();

	/// The CPU (EE) thread's own CPU time, in Threading::GetThreadTicksPerSecond() units.
	/// Reads the same handle SetCPUThread() registered, so there is one handle rather than
	/// one per consumer. Zero if no CPU thread is registered.
	u64 GetCPUThreadCPUTime();

	/// Sets the EE thread for CPU usage calculations.
	void SetCPUThread(Threading::ThreadHandle thread);

	/// Sets timers for GS software threads.
	void SetGSSWThreadCount(u32 count);
	void SetGSSWThread(u32 index, Threading::ThreadHandle thread);

	/// Sets the timer for the GS back thread (GSBackThreadMode >= Lockstep). Registered by
	/// the back thread itself at entry and cleared once it has joined; an empty handle means
	/// no such thread exists, which is the default configuration. Under the pipelined split
	/// the GS work is roughly halved between this thread and the MTGS thread, so the plain
	/// "GS" figure alone reads as a ~50% drop in GS cost that never happened.
	void SetGSBackThread(Threading::ThreadHandle thread);

	u64 GetFrameNumber();

	InternalFPSMethod GetInternalFPSMethod();
	bool IsInternalFPSValid();

	float GetFPS();
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
