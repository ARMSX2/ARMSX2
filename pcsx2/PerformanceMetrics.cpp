// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "common/Console.h"
#include "common/Timer.h"
#include "common/Threading.h"

#include "PerformanceMetrics.h"

#include "GS.h"
#include "IopThread/IopPlacement.h"
#include "MTGS.h"
#include "MTVU.h"
#include "VMManager.h"

#if defined(__ANDROID__)
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static const float UPDATE_INTERVAL = 0.5f;

static float s_fps = 0.0f;
static PerformanceMetrics::AverageFPS s_avg_vps;
static float s_internal_fps = 0.0f;
static float s_minimum_frame_time = 0.0f;
static float s_minimum_frame_time_accumulator = 0.0f;
static float s_average_frame_time = 0.0f;
static float s_average_frame_time_accumulator = 0.0f;
static float s_maximum_frame_time = 0.0f;
static float s_maximum_frame_time_accumulator = 0.0f;
static u32 s_frames_since_last_update = 0;
static u32 s_unskipped_frames_since_last_update = 0;
static Common::Timer s_last_update_time;
static Common::Timer s_last_frame_time;

// Session perf logging: a rolling emulog line every ~30s of presented
// frames, and a whole-session average at shutdown (LogSessionSummary).
// Gives every -logfile run a durable framerate record. Wall-clock based:
// paused time dilutes the session average but not the rolling lines.
static const float LOG_INTERVAL = 30.0f;
static Common::Timer s_session_timer;
static float s_log_accum_time = 0.0f;
static u32 s_log_accum_frames = 0;

// frame number, updated by the GS thread
static u64 s_frame_number = 0;

// internal fps heuristics
static PerformanceMetrics::InternalFPSMethod s_internal_fps_method = PerformanceMetrics::InternalFPSMethod::None;
static u32 s_gs_framebuffer_blits_since_last_update = 0;
static u32 s_gs_privileged_register_writes_since_last_update = 0;

static Threading::ThreadHandle s_cpu_thread_handle;
static u64 s_last_cpu_time = 0;

// The CPU thread's frame-work clock, in GetCPUTicks() units. It opens at the limiter's
// post-sleep return and closes at the next Throttle() entry, so it measures the frame's
// active work with the deliberate limiter sleep taken out. Zero means no period is open, which
// is how a frame the limiter did not pace is refused rather than reported.
//
// CPU-thread-owned. It uses the GetCPUTicks() domain rather than Common::Timer's: on AArch64
// that is one mrs of the architected counter instead of a vDSO call, and it is the clock the
// limiter's own target is expressed in.
static u64 s_frame_work_start = 0;
static u64 s_last_gs_time = 0;
static u64 s_last_gs_back_time = 0;
static u64 s_last_vu_time = 0;
static u64 s_last_iop_time = 0;
static u64 s_last_ticks = 0;

// The GS back thread registers itself from its own entry point and is cleared from the MTGS
// thread after the join, so unlike every other handle here this one is written by a thread
// other than the one sampling it. Handle and running total are therefore both owned by the
// mutex, which is taken twice a second at most.
static std::mutex s_gs_back_thread_mutex;
static Threading::ThreadHandle s_gs_back_thread_handle;

/// CPU time consumed by the back thread since the last call, in Threading tick units.
/// A thread that started or stopped inside the window contributes only the part of it that
/// the handle was installed for; the alternative is a u64 subtraction that wraps into a
/// nonsense percentage on the first window after a GSreopen.
static u64 SampleGSBackThreadCPUTime()
{
	std::unique_lock lock(s_gs_back_thread_mutex);
	const u64 now = s_gs_back_thread_handle ? s_gs_back_thread_handle.GetCPUTime() : s_last_gs_back_time;
	const u64 delta = (now > s_last_gs_back_time) ? (now - s_last_gs_back_time) : 0;
	s_last_gs_back_time = now;
	return delta;
}

// The IOP thread is started and stopped by the CPU thread and sampled by whichever thread presents a
// frame, so like the GS back thread its handle and running total are owned by a mutex, taken twice a
// second at most.
static std::mutex s_iop_thread_mutex;
static Threading::ThreadHandle s_iop_thread_handle;

/// CPU time consumed by the IOP thread since the last call, in Threading tick units. A thread that
/// started or stopped inside the window contributes only the part of it that the handle was installed
/// for, for the same reason as the GS back thread's.
static u64 SampleIOPThreadCPUTime()
{
	std::unique_lock lock(s_iop_thread_mutex);
	const u64 now = s_iop_thread_handle ? s_iop_thread_handle.GetCPUTime() : s_last_iop_time;
	const u64 delta = (now > s_last_iop_time) ? (now - s_last_iop_time) : 0;
	s_last_iop_time = now;
	return delta;
}

#if defined(__ANDROID__)
// ---- Android ADPF (PerformanceHintManager) ---------------------------------
// Tells the OS "these threads produce a frame every N ns, clock them to hit it."
// PS2 emulation is bursty and latency-sensitive, so Android's DVFS governor
// routinely under-clocks the CPU/GPU under it; ADPF is the purpose-built fix.
// Symbols are resolved at runtime from libandroid.so so the app still loads on
// pre-API-33 devices (there the session is never created — everything no-ops).
struct AdpfApi
{
	void* (*getManager)() = nullptr;
	void* (*createSession)(void*, const int32_t*, size_t, int64_t) = nullptr;
	int (*updateTarget)(void*, int64_t) = nullptr;
	int (*reportActual)(void*, int64_t) = nullptr;
	void (*closeSession)(void*) = nullptr;
	bool tried = false;

	bool Available()
	{
		if (!tried)
		{
			tried = true;
			if (void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL))
			{
				getManager = reinterpret_cast<decltype(getManager)>(dlsym(lib, "APerformanceHint_getManager"));
				createSession = reinterpret_cast<decltype(createSession)>(dlsym(lib, "APerformanceHint_createSession"));
				updateTarget = reinterpret_cast<decltype(updateTarget)>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
				reportActual = reinterpret_cast<decltype(reportActual)>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
				closeSession = reinterpret_cast<decltype(closeSession)>(dlsym(lib, "APerformanceHint_closeSession"));
			}
		}
		return getManager && createSession && updateTarget && reportActual && closeSession;
	}
};

static std::mutex s_adpf_mutex;
static AdpfApi s_adpf;
static std::vector<int32_t> s_adpf_tids;
static void* s_adpf_manager = nullptr;
static void* s_adpf_session = nullptr;
static bool s_adpf_enabled = false; // experimental, opt-in
static bool s_adpf_create_failed = false;
static bool s_adpf_report_warned = false;
static bool s_adpf_paused = false; // reporting suspended (unlimited/vsync/interrupted), edge-logged
static int64_t s_adpf_target_ns = 0;
static int64_t s_adpf_work_accum_ns = 0; // this frame's segments closed so far; -1 = frame incomplete, skip it

// The frame deadline in ns: emulated refresh scaled by the limiter's target speed, so turbo /
// slow-motion move the deadline correctly. Returns 0 when there is no finite deadline (Unlimited,
// GetTargetSpeed()==0), so the caller pauses ADPF rather than feeding a bogus/inf target.
static int64_t AdpfTargetNs()
{
	const double fps = VMManager::GetFrameRate() * static_cast<double>(VMManager::GetTargetSpeed());
	return (fps > 1.0) ? static_cast<int64_t>(1.0e9 / fps) : 0;
}

// Must hold s_adpf_mutex. Creates the session once a perf thread has registered and a finite
// deadline exists, and LOGS the real outcome — so "ACTIVE" means a session genuinely exists,
// not merely that the user flipped the toggle.
static void AdpfEnsureSession()
{
	if (s_adpf_session || s_adpf_create_failed || !s_adpf_enabled || s_adpf_tids.empty())
		return;
	if (!s_adpf.Available())
	{
		s_adpf_create_failed = true; // pre-API-33 / no libandroid; stop retrying
		Console.WriteLn("ADPF: PerformanceHintManager symbols unavailable (needs Android 13+) — clock hint inactive.");
		return;
	}
	if (!s_adpf_manager)
		s_adpf_manager = s_adpf.getManager();
	if (!s_adpf_manager)
	{
		s_adpf_create_failed = true;
		Console.WriteLn("ADPF: getManager() returned null — clock hint inactive.");
		return;
	}
	const int64_t target = AdpfTargetNs();
	if (target <= 0)
		return; // no finite deadline yet (Unlimited / VM not paced); retry once one exists
	s_adpf_session = s_adpf.createSession(s_adpf_manager, s_adpf_tids.data(), s_adpf_tids.size(), target);
	if (!s_adpf_session)
	{
		s_adpf_create_failed = true;
		Console.Warning("ADPF: createSession() over %zu threads failed — clock hint inactive.", s_adpf_tids.size());
		return;
	}
	s_adpf_target_ns = target;
	s_adpf_report_warned = false;
	s_adpf_paused = false;
	std::string tid_list;
	for (size_t i = 0; i < s_adpf_tids.size(); i++)
		tid_list += (i ? "," : "") + std::to_string(s_adpf_tids[i]);
	Console.WriteLn("ADPF: session ACTIVE over %zu threads [tids %s], deadline %.2f ms.", s_adpf_tids.size(),
		tid_list.c_str(), static_cast<double>(target) / 1.0e6);
}
#endif // __ANDROID__

static double s_cpu_thread_usage = 0.0f;
static double s_cpu_thread_time = 0.0f;
static float s_gs_thread_usage = 0.0f;
static float s_gs_thread_time = 0.0f;
static float s_gs_back_thread_usage = 0.0f;
static float s_gs_back_thread_time = 0.0f;
static float s_vu_thread_usage = 0.0f;
static float s_vu_thread_time = 0.0f;
static float s_iop_thread_usage = 0.0f;
static float s_iop_thread_time = 0.0f;

// Update() closes a window by writing a dozen statics. TryGetWindowSnapshot() reads them from another
// thread, so the writer brackets its stores with this counter (odd while writing, even otherwise) and
// the reader keeps a copy only if the counter was even and did not move.
static std::atomic<u32> s_window_seq{0};
static float s_window_seconds = 0.0f;
static std::atomic<bool> s_gpu_timer_supported{false};

static PerformanceMetrics::FrameTimeHistory s_frame_time_history;
static u32 s_frame_time_history_pos = 0;

struct GSSWThreadStats
{
	Threading::ThreadHandle handle;
	u64 last_cpu_time = 0;
	double usage = 0.0;
	double time = 0.0;
};
std::vector<GSSWThreadStats> s_gs_sw_threads;

static float s_average_gpu_time = 0.0f;
static float s_last_gpu_time = 0.0f;
static float s_accumulated_gpu_time = 0.0f;
static float s_gpu_usage = 0.0f;
static u32 s_presents_since_last_update = 0;
static double s_average_gpu_vs_invocations = 0.0;
static double s_average_gpu_ps_invocations = 0.0;
static u64 s_accumulated_gpu_vs_invocations = 0;
static u64 s_accumulated_gpu_ps_invocations = 0;

void PerformanceMetrics::Clear()
{
	Reset();

	s_fps = 0.0f;
	s_avg_vps.ClearStats();
	s_internal_fps = 0.0f;
	s_minimum_frame_time = 0.0f;
	s_average_frame_time = 0.0f;
	s_maximum_frame_time = 0.0f;
	s_internal_fps_method = PerformanceMetrics::InternalFPSMethod::None;

	s_cpu_thread_usage = 0.0f;
	s_cpu_thread_time = 0.0f;
	s_gs_thread_usage = 0.0f;
	s_gs_thread_time = 0.0f;
	s_gs_back_thread_usage = 0.0f;
	s_gs_back_thread_time = 0.0f;
	s_vu_thread_usage = 0.0f;
	s_vu_thread_time = 0.0f;
	s_iop_thread_usage = 0.0f;
	s_iop_thread_time = 0.0f;

	s_average_gpu_time = 0.0f;
	s_last_gpu_time = 0.0f;
	s_gpu_usage = 0.0f;

	s_frame_number = 0;

	s_session_timer.Reset();
	s_log_accum_time = 0.0f;
	s_log_accum_frames = 0;

	s_frame_time_history.fill(0.0f);
	s_frame_time_history_pos = 0;
}

void PerformanceMetrics::LogSessionSummary()
{
	const double elapsed = s_session_timer.GetTimeSeconds();
	if (s_frame_number == 0 || elapsed < 1.0)
		return;
	Console.WriteLn("PerfLog session: %llu frames in %.1fs wall = %.2f fps average",
		static_cast<unsigned long long>(s_frame_number), elapsed,
		static_cast<double>(s_frame_number) / elapsed);
}

void PerformanceMetrics::Reset()
{
	s_frames_since_last_update = 0;
	s_unskipped_frames_since_last_update = 0;
	s_gs_framebuffer_blits_since_last_update = 0;
	s_gs_privileged_register_writes_since_last_update = 0;
	s_minimum_frame_time_accumulator = 0.0f;
	s_average_frame_time_accumulator = 0.0f;
	s_maximum_frame_time_accumulator = 0.0f;

	s_accumulated_gpu_time = 0.0f;
	s_presents_since_last_update = 0;

	s_last_update_time.Reset();
	s_last_frame_time.Reset();

	s_last_cpu_time = s_cpu_thread_handle.GetCPUTime();
	s_last_gs_time = MTGS::GetThreadHandle().GetCPUTime();
	SampleGSBackThreadCPUTime(); // rebases the running total; the delta is deliberately dropped
	s_last_vu_time = THREAD_VU1 ? vu1Thread.GetThreadHandle().GetCPUTime() : 0;
	SampleIOPThreadCPUTime(); // rebases the running total; the delta is deliberately dropped
	s_last_ticks = GetCPUTicks();

	for (GSSWThreadStats& stat : s_gs_sw_threads)
		stat.last_cpu_time = stat.handle.GetCPUTime();
}

void PerformanceMetrics::Update(bool gs_register_write, bool fb_blit, bool is_skipping_present)
{
	if (!is_skipping_present)
	{
		const float frame_time = s_last_frame_time.GetTimeMillisecondsAndReset();
		s_minimum_frame_time_accumulator = (s_minimum_frame_time_accumulator == 0.0f) ? frame_time : std::min(s_minimum_frame_time_accumulator, frame_time);
		s_average_frame_time_accumulator += frame_time;
		s_maximum_frame_time_accumulator = std::max(s_maximum_frame_time_accumulator, frame_time);
		s_frame_time_history[s_frame_time_history_pos] = frame_time;
		s_frame_time_history_pos = (s_frame_time_history_pos + 1) % NUM_FRAME_TIME_SAMPLES;
		s_unskipped_frames_since_last_update++;
	}

	s_frames_since_last_update++;
	s_gs_privileged_register_writes_since_last_update += static_cast<u32>(gs_register_write);
	s_gs_framebuffer_blits_since_last_update += static_cast<u32>(fb_blit);
	s_frame_number++;

	const Common::Timer::Value now_ticks = Common::Timer::GetCurrentValue();
	const Common::Timer::Value ticks_diff = now_ticks - s_last_update_time.GetStartValue();
	const float time = Common::Timer::ConvertValueToSeconds(ticks_diff);
	if (time < UPDATE_INTERVAL)
		return;

	s_last_update_time.ResetTo(now_ticks);
	s_window_seq.fetch_add(1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	s_window_seconds = time;
	s_minimum_frame_time = std::exchange(s_minimum_frame_time_accumulator, 0.0f);
	s_average_frame_time = std::exchange(s_average_frame_time_accumulator, 0.0f) / static_cast<float>(s_unskipped_frames_since_last_update);
	s_maximum_frame_time = std::exchange(s_maximum_frame_time_accumulator, 0.0f);
	s_fps = static_cast<float>(s_frames_since_last_update) / time;
	s_avg_vps.UpdateAvgFPS(s_fps);
	s_average_gpu_time = s_accumulated_gpu_time / static_cast<float>(s_unskipped_frames_since_last_update);
	s_average_gpu_vs_invocations = static_cast<double>(s_accumulated_gpu_vs_invocations) / static_cast<double>(s_unskipped_frames_since_last_update);
	s_average_gpu_ps_invocations = static_cast<double>(s_accumulated_gpu_ps_invocations) / static_cast<double>(s_unskipped_frames_since_last_update);
	s_gpu_usage = s_accumulated_gpu_time / (time * 10.0f);
	s_accumulated_gpu_time = 0.0f;
	s_accumulated_gpu_vs_invocations = 0;
	s_accumulated_gpu_ps_invocations = 0;

	// prefer privileged register write based framerate detection, it's less likely to have false positives
	if (s_gs_privileged_register_writes_since_last_update > 0 && !EmuConfig.Gamefixes.BlitInternalFPSHack)
	{
		s_internal_fps = static_cast<float>(s_gs_privileged_register_writes_since_last_update) / time;
		s_internal_fps_method = InternalFPSMethod::GSPrivilegedRegister;
	}
	else if (s_gs_framebuffer_blits_since_last_update > 0)
	{
		s_internal_fps = static_cast<float>(s_gs_framebuffer_blits_since_last_update) / time;
		s_internal_fps_method = InternalFPSMethod::DISPFBBlit;
	}
	else
	{
		s_internal_fps = 0;
		s_internal_fps_method = InternalFPSMethod::None;
	}

	s_gs_privileged_register_writes_since_last_update = 0;
	s_gs_framebuffer_blits_since_last_update = 0;

	const u64 ticks = GetCPUTicks();
	const u64 ticks_delta = ticks - s_last_ticks;
	s_last_ticks = ticks;

	const double pct_divider =
		100.0 * (1.0 / ((static_cast<double>(ticks_delta) * static_cast<double>(Threading::GetThreadTicksPerSecond())) /
						   static_cast<double>(GetTickFrequency())));
	const double time_divider = 1000.0 * (1.0 / static_cast<double>(Threading::GetThreadTicksPerSecond())) *
								(1.0 / static_cast<double>(s_frames_since_last_update));

	const u64 cpu_time = s_cpu_thread_handle.GetCPUTime();
	const u64 gs_time = MTGS::GetThreadHandle().GetCPUTime();
	const u64 gs_back_delta = SampleGSBackThreadCPUTime();
	const u64 vu_time = THREAD_VU1 ? vu1Thread.GetThreadHandle().GetCPUTime() : 0;
	const u64 iop_delta = SampleIOPThreadCPUTime();

	const u64 cpu_delta = cpu_time - s_last_cpu_time;
	const u64 gs_delta = gs_time - s_last_gs_time;
	const u64 vu_delta = vu_time - s_last_vu_time;
	s_last_cpu_time = cpu_time;
	s_last_gs_time = gs_time;
	s_last_vu_time = vu_time;

	s_cpu_thread_usage = static_cast<double>(cpu_delta) * pct_divider;
	s_gs_thread_usage = static_cast<double>(gs_delta) * pct_divider;
	s_gs_back_thread_usage = static_cast<double>(gs_back_delta) * pct_divider;
	s_vu_thread_usage = static_cast<double>(vu_delta) * pct_divider;
	s_iop_thread_usage = static_cast<double>(iop_delta) * pct_divider;
	s_cpu_thread_time = static_cast<double>(cpu_delta) * time_divider;
	s_gs_thread_time = static_cast<double>(gs_delta) * time_divider;
	s_gs_back_thread_time = static_cast<double>(gs_back_delta) * time_divider;
	s_vu_thread_time = static_cast<double>(vu_delta) * time_divider;
	s_iop_thread_time = static_cast<double>(iop_delta) * time_divider;

	{
		IopThread::PrimeLoad load;
		load.present[IopThread::kPrimeEE] = true;
		load.busy[IopThread::kPrimeEE] = s_cpu_thread_usage;
		load.present[IopThread::kPrimeVU] = THREAD_VU1;
		load.busy[IopThread::kPrimeVU] = s_vu_thread_usage;
		load.present[IopThread::kPrimeGSFront] = true;
		load.busy[IopThread::kPrimeGSFront] = s_gs_thread_usage;
		load.present[IopThread::kPrimeGSBack] = HasGSBackThread();
		load.busy[IopThread::kPrimeGSBack] = s_gs_back_thread_usage;
		VMManager::Internal::UpdatePrimeCorePlacement(load);
	}

	for (GSSWThreadStats& thread : s_gs_sw_threads)
	{
		const u64 time = thread.handle.GetCPUTime();
		const u64 delta = time - thread.last_cpu_time;
		thread.last_cpu_time = time;
		thread.usage = static_cast<double>(delta) * pct_divider;
		thread.time = static_cast<double>(delta) * time_divider;
	}

	// Rolling perf log (uses this window's frame count before it resets).
	s_log_accum_time += time;
	s_log_accum_frames += s_frames_since_last_update;
	if (s_log_accum_time >= LOG_INTERVAL)
	{
		// The back thread only exists with GS multi-threading on, so the field is
		// omitted rather than logged as a permanent 0% in the default configuration.
		char gs_back[32] = {};
		if (HasGSBackThread())
			std::snprintf(gs_back, sizeof(gs_back), " GSB %.0f%%", s_gs_back_thread_usage);

		// The IOP thread only exists while the IOP runs concurrently, so likewise omitted otherwise.
		char iop[32] = {};
		if (HasIOPThread())
			std::snprintf(iop, sizeof(iop), " IOP %.0f%%", s_iop_thread_usage);

		Console.WriteLn("PerfLog: %.1f fps | EE %.0f%% GS %.0f%%%s VU %.0f%%%s GPU %.0f%% | frame %llu",
			static_cast<float>(s_log_accum_frames) / s_log_accum_time, s_cpu_thread_usage,
			s_gs_thread_usage, gs_back, s_vu_thread_usage, iop, s_gpu_usage,
			static_cast<unsigned long long>(s_frame_number));
		s_log_accum_time = 0.0f;
		s_log_accum_frames = 0;
	}

	s_frames_since_last_update = 0;
	s_unskipped_frames_since_last_update = 0;
	s_presents_since_last_update = 0;

	s_window_seq.fetch_add(1, std::memory_order_release);

	Host::OnPerformanceMetricsUpdated();
}

void PerformanceMetrics::SetGPUTimerSupported(bool supported)
{
	s_gpu_timer_supported.store(supported, std::memory_order_relaxed);
}

bool PerformanceMetrics::HasGPUTimer()
{
	return s_gpu_timer_supported.load(std::memory_order_relaxed);
}

bool PerformanceMetrics::TryGetWindowSnapshot(WindowSnapshot* out)
{
	const u32 before = s_window_seq.load(std::memory_order_acquire);
	if (before == 0 || (before & 1))
		return false;

	WindowSnapshot snap;
	snap.sequence = before / 2;
	snap.seconds = s_window_seconds;
	snap.speed = GetSpeed();
	snap.ee_usage = s_cpu_thread_usage;
	snap.gs_usage = s_gs_thread_usage;
	snap.gs_back_usage = s_gs_back_thread_usage;
	snap.vu_usage = s_vu_thread_usage;
	snap.iop_usage = s_iop_thread_usage;
	snap.gpu_usage = s_gpu_usage;
	snap.has_gs_back_thread = HasGSBackThread();
	snap.has_iop_thread = HasIOPThread();
	snap.has_gpu_timer = HasGPUTimer();
	snap.ee_ms = static_cast<float>(s_cpu_thread_time);
	snap.gs_ms = static_cast<float>(s_gs_thread_time);
	snap.gs_back_ms = static_cast<float>(s_gs_back_thread_time);
	snap.gpu_ms = s_average_gpu_time;

	std::atomic_thread_fence(std::memory_order_acquire);
	if (s_window_seq.load(std::memory_order_relaxed) != before)
		return false;

	*out = snap;
	return true;
}

void PerformanceMetrics::OnGPUPresent(float gpu_time, u64 vs_invocations, u64 ps_invocations)
{
	s_last_gpu_time = gpu_time;
	s_accumulated_gpu_time += gpu_time;
	s_accumulated_gpu_vs_invocations += vs_invocations;
	s_accumulated_gpu_ps_invocations += ps_invocations;
	s_presents_since_last_update++;
}

void PerformanceMetrics::SetCPUThread(Threading::ThreadHandle thread)
{
	s_last_cpu_time = thread ? thread.GetCPUTime() : 0;
	s_cpu_thread_handle = std::move(thread);
}

void PerformanceMetrics::AdpfRegisterCallingThread()
{
#if defined(__ANDROID__)
	const int32_t tid = static_cast<int32_t>(syscall(SYS_gettid));
	std::lock_guard<std::mutex> lock(s_adpf_mutex);
	if (std::find(s_adpf_tids.begin(), s_adpf_tids.end(), tid) != s_adpf_tids.end())
		return;
	s_adpf_tids.push_back(tid);
	// The session's thread list is fixed at creation, so a newly-registered thread
	// means the current session is missing it — drop it and let the next frame
	// recreate it over the full set.
	if (s_adpf_session)
	{
		s_adpf.closeSession(s_adpf_session);
		s_adpf_session = nullptr;
	}
	s_adpf_create_failed = false;
#endif
}

void PerformanceMetrics::AdpfSetEnabled(bool enabled)
{
#if defined(__ANDROID__)
	std::lock_guard<std::mutex> lock(s_adpf_mutex);
	if (s_adpf_enabled == enabled)
		return;
	s_adpf_enabled = enabled;
	if (!enabled && s_adpf_session)
	{
		s_adpf.closeSession(s_adpf_session);
		s_adpf_session = nullptr;
	}
	if (enabled)
		s_adpf_create_failed = false; // allow the next frame to recreate
#else
	(void)enabled;
#endif
}

void PerformanceMetrics::AdpfShutdown()
{
#if defined(__ANDROID__)
	std::lock_guard<std::mutex> lock(s_adpf_mutex);
	if (s_adpf_session)
	{
		s_adpf.closeSession(s_adpf_session);
		s_adpf_session = nullptr;
	}
	s_adpf_tids.clear();
	s_adpf_create_failed = false;
	s_adpf_work_accum_ns = 0;
#endif
	s_frame_work_start = 0;
}

#if defined(__ANDROID__)
// Reports the frame's work to PerformanceHintManager. The limiter waits twice per frame, so a
// frame is two segments: the first is held in s_adpf_work_accum_ns and the pair is reported at
// the frame's last wait. `segment_open` is false when no period was open at this wait (first
// frame, or just resumed from a pause).
static void AdpfEndWorkSegment(bool segment_open, u64 segment_ticks, bool frame_end)
{
	std::lock_guard<std::mutex> lock(s_adpf_mutex);
	if (!s_adpf_enabled)
		return;
	AdpfEnsureSession();
	if (!s_adpf_session || !segment_open)
	{
		// If this is the frame's first wait, its second segment alone would under-report the
		// frame, so skip it.
		s_adpf_work_accum_ns = frame_end ? 0 : -1;
		return;
	}
	const int64_t segment_ns =
		static_cast<int64_t>((static_cast<double>(segment_ticks) * 1.0e9) / static_cast<double>(GetTickFrequency()));
	if (!frame_end)
	{
		if (s_adpf_work_accum_ns >= 0)
			s_adpf_work_accum_ns += segment_ns;
		return;
	}
	if (s_adpf_work_accum_ns < 0)
	{
		s_adpf_work_accum_ns = 0;
		return;
	}
	const int64_t work_ns = s_adpf_work_accum_ns + segment_ns;
	s_adpf_work_accum_ns = 0;
	const int64_t target = AdpfTargetNs();
	if (target > 0 && target != s_adpf_target_ns)
	{
		s_adpf.updateTarget(s_adpf_session, target);
		s_adpf_target_ns = target;
	}
	// Drop absurd outliers (savestate load, renderer recreation, debugger stall): a period several
	// times the deadline is not a real frame and would spam a spurious max-frequency demand.
	if (work_ns <= 0 || (s_adpf_target_ns > 0 && work_ns > s_adpf_target_ns * 4))
		return;
	if (s_adpf_paused)
	{
		Console.WriteLn("ADPF: reporting resumed.");
		s_adpf_paused = false;
	}
	const int ret = s_adpf.reportActual(s_adpf_session, work_ns);
	if (ret != 0 && !s_adpf_report_warned)
	{
		s_adpf_report_warned = true;
		Console.Warning("ADPF: reportActualWorkDuration returned %d — driver is ignoring the hint.", ret);
	}
}
#endif

u64 PerformanceMetrics::OnFrameWorkSegmentComplete(u64 now_ticks, bool frame_end)
{
	// Not pure CPU compute: the EE can still block behind a full MTGS queue that is itself
	// stalled on presentation, so some present-wait leaks in. The cycle-rate sampler times the
	// EE thread's GS and VU1 waits separately and subtracts them.
	const bool segment_open = (s_frame_work_start != 0);
	const u64 segment_ticks = (segment_open && now_ticks > s_frame_work_start) ? (now_ticks - s_frame_work_start) : 0;

#if defined(__ANDROID__)
	AdpfEndWorkSegment(segment_open, segment_ticks, frame_end);
#else
	(void)frame_end;
#endif

	return segment_ticks;
}

void PerformanceMetrics::OnFrameWorkBegin()
{
	s_frame_work_start = GetCPUTicks();
}

void PerformanceMetrics::OnFrameWorkPaused()
{
	// Not frame-limiting (unlimited / host-vsync / interrupted) — invalidate the period so no
	// wall-time-with-wait duration is measured.
	s_frame_work_start = 0;

#if defined(__ANDROID__)
	// Edge-log, so a tester never sees "ACTIVE" while nothing is actually being submitted.
	std::lock_guard<std::mutex> lock(s_adpf_mutex);
	if (s_adpf_session && !s_adpf_paused)
	{
		Console.WriteLn("ADPF: reporting paused (unlimited / host-vsync / interrupted) — no durations submitted.");
		s_adpf_paused = true;
	}
	s_adpf_work_accum_ns = 0;
#endif
}

u64 PerformanceMetrics::GetCPUThreadCPUTime()
{
	return s_cpu_thread_handle.GetCPUTime();
}

void PerformanceMetrics::SetGSSWThreadCount(u32 count)
{
	s_gs_sw_threads.clear();
	s_gs_sw_threads.resize(count);
}

void PerformanceMetrics::SetGSSWThread(u32 index, Threading::ThreadHandle thread)
{
	s_gs_sw_threads[index].last_cpu_time = thread ? thread.GetCPUTime() : 0;
	s_gs_sw_threads[index].handle = std::move(thread);
}

void PerformanceMetrics::SetGSBackThread(Threading::ThreadHandle thread)
{
	std::unique_lock lock(s_gs_back_thread_mutex);
	// Rebase off the incoming handle, so the first window after a GSreopen respawn measures
	// this thread's time rather than its difference against the retired thread's total.
	s_last_gs_back_time = thread ? thread.GetCPUTime() : 0;
	s_gs_back_thread_handle = std::move(thread);
}

void PerformanceMetrics::SetIOPThread(Threading::ThreadHandle thread)
{
	std::unique_lock lock(s_iop_thread_mutex);
	// Rebase off the incoming handle, so the first window after a start measures this thread's time
	// and not its difference against the retired thread's total.
	s_last_iop_time = thread ? thread.GetCPUTime() : 0;
	s_iop_thread_handle = std::move(thread);
	if (!s_iop_thread_handle)
	{
		s_iop_thread_usage = 0.0f;
		s_iop_thread_time = 0.0f;
	}
}

u64 PerformanceMetrics::GetFrameNumber()
{
	return s_frame_number;
}

PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod()
{
	return s_internal_fps_method;
}

bool PerformanceMetrics::IsInternalFPSValid()
{
	return s_internal_fps_method != InternalFPSMethod::None;
}

float PerformanceMetrics::GetFPS()
{
	return s_fps;
}

float PerformanceMetrics::GetAvgVPS()
{
	return s_avg_vps.GetAvgFPS();
}

float PerformanceMetrics::GetInternalFPS()
{
	return s_internal_fps;
}

float PerformanceMetrics::GetSpeed()
{
	return (s_fps / VMManager::GetFrameRate()) * 100.0;
}

float PerformanceMetrics::GetAverageFrameTime()
{
	return s_average_frame_time;
}

float PerformanceMetrics::GetMinimumFrameTime()
{
	return s_minimum_frame_time;
}

float PerformanceMetrics::GetMaximumFrameTime()
{
	return s_maximum_frame_time;
}

double PerformanceMetrics::GetCPUThreadUsage()
{
	return s_cpu_thread_usage;
}

double PerformanceMetrics::GetCPUThreadAverageTime()
{
	return s_cpu_thread_time;
}

float PerformanceMetrics::GetGSThreadUsage()
{
	return s_gs_thread_usage;
}

float PerformanceMetrics::GetGSThreadAverageTime()
{
	return s_gs_thread_time;
}

bool PerformanceMetrics::HasGSBackThread()
{
	std::unique_lock lock(s_gs_back_thread_mutex);
	return static_cast<bool>(s_gs_back_thread_handle);
}

float PerformanceMetrics::GetGSBackThreadUsage()
{
	return s_gs_back_thread_usage;
}

float PerformanceMetrics::GetGSBackThreadAverageTime()
{
	return s_gs_back_thread_time;
}

float PerformanceMetrics::GetVUThreadUsage()
{
	return s_vu_thread_usage;
}

float PerformanceMetrics::GetVUThreadAverageTime()
{
	return s_vu_thread_time;
}

bool PerformanceMetrics::HasIOPThread()
{
	std::unique_lock lock(s_iop_thread_mutex);
	return static_cast<bool>(s_iop_thread_handle);
}

float PerformanceMetrics::GetIOPThreadUsage()
{
	return s_iop_thread_usage;
}

float PerformanceMetrics::GetIOPThreadAverageTime()
{
	return s_iop_thread_time;
}

u32 PerformanceMetrics::GetGSSWThreadCount()
{
	return static_cast<u32>(s_gs_sw_threads.size());
}

double PerformanceMetrics::GetGSSWThreadUsage(u32 index)
{
	return s_gs_sw_threads[index].usage;
}

double PerformanceMetrics::GetGSSWThreadAverageTime(u32 index)
{
	return s_gs_sw_threads[index].time;
}

float PerformanceMetrics::GetGPUUsage()
{
	return s_gpu_usage;
}

float PerformanceMetrics::GetGPUAverageTime()
{
	return s_average_gpu_time;
}

float PerformanceMetrics::GetLastGPUTime()
{
	return s_last_gpu_time;
}

double PerformanceMetrics::GetGPUAverageVSInvocations()
{
	return s_average_gpu_vs_invocations;
}

double PerformanceMetrics::GetGPUAveragePSInvocations()
{
	return s_average_gpu_ps_invocations;
}

const PerformanceMetrics::FrameTimeHistory& PerformanceMetrics::GetFrameTimeHistory()
{
	return s_frame_time_history;
}

u32 PerformanceMetrics::GetFrameTimeHistoryPos()
{
	return s_frame_time_history_pos;
}
