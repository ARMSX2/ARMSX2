// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <deque>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#ifdef __linux__
// For reading /proc/self/statm (resident set size) on the per-frame path.
#include <fcntl.h>
#include <unistd.h>
#endif

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/CocoaTools.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/Renderers/Common/GSGPUProfile.h"
#include "pcsx2/GS/GSPerfMon.h"
#include "pcsx2/GS/GSFeDecode.h"
#include "pcsx2/GS/Renderers/HW/GSDrawLog.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

#include "GSLadder.h"
#include "GSReplayPayload.h"
#include "GSRunnerAffinity.h"
#include "RenderDocCapture.h"

#include "svnrev.h"

#ifdef __ANDROID__
// The core expects the frontend to provide these JNI bridges (native-lib.cpp
// does in the APK). A bare NDK executable has no JVM: the Java-backed paths
// (scoped-storage fallbacks, content:// fds, Java sound, pad rumble) cannot
// trigger under adb shell on plain filesystem paths, so they stub to failure.
namespace Common
{
	bool PlaySoundAsync(const char* path) { return false; }
}
namespace FileSystem
{
	int OpenFDFileContent(const char* filename) { return -1; }
	bool CreateDirectoryViaJava(const char* path) { return false; }
	bool CreateFileViaJava(const char* path) { return false; }
}
namespace Native
{
	void onPadRumble(int pad, int largeMotor, int smallMotor) {}
}

// Android renderer-Auto steering (GSUtil.cpp; the APK sets it from the
// GL_RENDERER string). This frontend is headless: the SW renderer's host
// present device must come up without a window system, which Vulkan
// surfaceless does and an EGL context under adb shell does not.
extern bool g_gs_android_prefer_vk;
static const bool s_android_prefer_vk_init = []() { g_gs_android_prefer_vk = true; return true; }();
#endif

// Down here because X11 has a lot of defines that can conflict
#if defined(__linux__) && defined(X11_API)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace GSRunner
{
	static void InitializeConsole();
	static bool InitializeConfig();
	static void SettingsOverride();
	static bool ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params);
	static void DumpStats();

	static bool CreatePlatformWindow();
	static void DestroyPlatformWindow();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void PumpPlatformMessages(bool forever = false);
	static void StopPlatformMessagePump();
} // namespace GSRunner

static constexpr u32 WINDOW_WIDTH = 640;
static constexpr u32 WINDOW_HEIGHT = 480;

static MemorySettingsInterface s_settings_interface;

static std::string s_output_prefix;
static s32 s_loop_count = 1;
static std::optional<bool> s_use_window;
static bool s_no_console = false;

// -gspin. The CPU set the GS thread was asked to run on, exactly as it was typed, and
// the set it turned out to be on once the pin was applied. Both travel to the stats
// JSON because a measurement of GS-thread CPU time is only comparable between runs if
// the thread sat on the same kind of core in both, and on a big.LITTLE device the
// scheduler decides that, not the run. Empty request means the flag was not given.
static std::string s_gs_pin_request;
static u64 s_gs_pin_mask = 0;
static std::string s_gs_pin_effective("none");
static const char* s_gs_pin_source = "none";

// The CPU set this process started with, captured before anything has pinned anything.
// It is the reference that tells "nobody narrowed this thread" apart from "something
// did": a read-back on its own cannot say which, and both answers are one comma list.
// Captured rather than derived from the online CPU count because Android runs the app
// inside a cpuset, where the inherited set is already narrower than the machine.
static u64 s_baseline_cpu_mask = 0;

#if defined(__ANDROID__)
// VMManager's compiled-in thread-placement mode. It is an app-facing knob -- only the
// Android app's JNI bridge writes it -- and its default has moved before now, which the
// headless runner then inherited without saying so anywhere. A measurement binary must
// not carry app policy silently, so the runner overrides it with a default of its own
// and records what it used.
extern int g_android_affinity_mode;
#endif

// -affinity. The thread-placement mode this run asked VMManager for, and where that
// number came from. The runner's own default is 0 (unpinned) on every platform that has
// the mode at all, whatever the app's default happens to be that month, because an
// unpinned run is the one whose numbers are comparable against every other unpinned run.
// s_affinity_mode is -1 where the platform compiles no affinity path, and the source is
// then "unsupported" -- there is no mode in effect to report.
static int s_affinity_mode = 0;
static const char* s_affinity_source = "runner-default";

// -renderdoc / -renderdoc-frame. Empty path means capture is not requested.
static std::string s_renderdoc_path;
static u32 s_renderdoc_start_frame = 1;
static u32 s_renderdoc_frame_count = 1;

// Owned by the GS thread.
static u32 s_dump_frame_number = 0;
static u32 s_loop_number = s_loop_count;

// Frames in one pass over the dump. Latched while the replayer is alive, because the
// stats JSON is written after VMManager::Shutdown() has already released the dump file.
// Zero when the run was not a dump replay.
static u32 s_dump_frames_per_loop = 0;
static double s_last_internal_draws = 0;
static double s_last_draws = 0;
static double s_last_render_passes = 0;
// Summed renderArea of those same passes, in pixels: on a tiler, the frame's tile load-and-store
// bill. The pair is the point -- a pass count alone cannot separate a title that broke its frame
// into hundreds of small passes from one that broke it into hundreds of full-surface ones.
static double s_last_render_pass_area_pixels = 0;
static double s_last_barriers = 0;
static double s_last_copies = 0;
static double s_last_uploads = 0;
static double s_last_readbacks = 0;
static double s_last_gpu_blocking_waits = 0;
static double s_last_depth_copies_rov = 0;
static double s_last_draws_rov = 0;
static double s_last_barriers_rov = 0;
static u64 s_total_internal_draws = 0;
static u64 s_total_draws = 0;
static u64 s_total_render_passes = 0;
static u64 s_total_render_pass_area_pixels = 0;
static u64 s_total_barriers = 0;
static u64 s_total_copies = 0;
static u64 s_total_uploads = 0;
static u64 s_total_readbacks = 0;
static u64 s_total_gpu_blocking_waits = 0;
static u64 s_total_copies_rov = 0;
static u64 s_total_draws_rov = 0;
static u64 s_total_barriers_rov = 0;
static u32 s_total_frames = 0;
static u32 s_total_drawn_frames = 0;
static std::vector<std::string> s_extended_stats_snapshot;

// The device's wait bill -- the same population as GpuBlockingWaits above, but split by cause and
// carrying wall time -- LATCHED while the device is still alive.
//
// It is latched rather than read where it is printed because DumpStats() runs after
// VMManager::Shutdown(), and whether g_gs_device still exists at that point is a teardown-ordering
// accident that differs between builds. It is null there on the handheld device build, which is
// precisely the configuration whose residual waits the split exists to attribute -- so the line
// went silently missing on the only machine that needed it, and the run it was needed for had to be
// re-taken. Latched at every present (GS thread, device certainly live) and once more at
// shutdown-begin, so no future round depends on that ordering again. The counters are monotonic,
// so a latch is a plain copy and the last one wins.
struct DeviceWaitBill
{
	u64 sync_ns = 0;
	u64 sync_calls = 0;
	u64 ring_ns = 0;
	u64 ring_calls = 0;
	// Neither of these is a fence wait. `submit` is time inside the queue-submit call and `map` is
	// the cache invalidate a readback needs after its drain has ended -- both were previously in no
	// counter at all. submit_calls is also the run's submit count, since the submit bill has one
	// site.
	u64 submit_ns = 0;
	u64 submit_calls = 0;
	u64 map_ns = 0;
	u64 map_calls = 0;
	// The same waits itemised by CALL SITE. The two bills above say what waiting cost; these say
	// what it was waiting for, and the sites of a bill sum to that bill exactly (the device books
	// both in one place). Names are latched too, because they are read after the device is gone --
	// they are string literals, so copying the pointer is copying the string.
	static constexpr u32 kMaxSites = 32;
	u32 site_count = 0;
	const char* site_names[kMaxSites] = {};
	const char* site_families[kMaxSites] = {};
	u64 site_ns[kMaxSites] = {};
	u64 site_calls[kMaxSites] = {};
};
static DeviceWaitBill s_device_wait_bill;

static void LatchDeviceWaitBill()
{
	if (!g_gs_device)
		return;
	s_device_wait_bill.sync_ns = g_gs_device->GetSyncWaitNs();
	s_device_wait_bill.sync_calls = g_gs_device->GetSyncWaitCalls();
	s_device_wait_bill.ring_ns = g_gs_device->GetRingWaitNs();
	s_device_wait_bill.ring_calls = g_gs_device->GetRingWaitCalls();
	s_device_wait_bill.submit_ns = g_gs_device->GetSubmitWaitNs();
	s_device_wait_bill.submit_calls = g_gs_device->GetSubmitWaitCalls();
	s_device_wait_bill.map_ns = g_gs_device->GetMapWaitNs();
	s_device_wait_bill.map_calls = g_gs_device->GetMapWaitCalls();

	const u32 nsites = std::min(g_gs_device->GetGpuWaitSiteCount(), DeviceWaitBill::kMaxSites);
	const u64* site_ns = g_gs_device->GetGpuWaitSiteNs();
	const u64* site_calls = g_gs_device->GetGpuWaitSiteCalls();
	if (nsites == 0 || !site_ns || !site_calls)
		return;
	// The names are string literals and never change, so they are copied once rather than on every
	// present -- this runs per frame and the counters are the only part that moves.
	const bool need_names = (s_device_wait_bill.site_count == 0);
	s_device_wait_bill.site_count = nsites;
	for (u32 i = 0; i < nsites; i++)
	{
		if (need_names)
		{
			s_device_wait_bill.site_names[i] = g_gs_device->GetGpuWaitSiteName(i);
			s_device_wait_bill.site_families[i] = g_gs_device->GetGpuWaitSiteFamily(i);
		}
		s_device_wait_bill.site_ns[i] = site_ns[i];
		s_device_wait_bill.site_calls[i] = site_calls[i];
	}
}

// Process resident set size in kB, or 0 where the platform has no procfs to ask.
//
// /proc/self/statm's second field is the resident page count, so the figure is pages
// times the page size -- which is not always 4 kB (this dev box runs 16 kB pages), so
// it is asked for rather than assumed. The descriptor is opened once and re-read with
// pread because this lands on the GS thread's per-frame path; one pread is a couple of
// microseconds against a frame measured in milliseconds.
static u64 ReadResidentSetKB()
{
#if defined(__linux__)
	static const long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	static const int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || page_kb <= 0)
		return 0;

	char buf[128];
	const ssize_t len = pread(fd, buf, sizeof(buf) - 1, 0);
	if (len <= 0)
		return 0;
	buf[len] = '\0';

	// "<total> <resident> <shared> ..." -- step over the first field.
	const char* resident = std::strchr(buf, ' ');
	if (!resident)
		return 0;

	return std::strtoull(resident + 1, nullptr, 10) * static_cast<u64>(page_kb);
#else
	return 0;
#endif
}

// Process minor page fault count, cumulative since process start, or 0 where the platform has
// no procfs to ask. /proc/[pid]/stat field 10 (minflt, see `man proc`) -- comm (field 2) is
// parenthesized and can itself contain spaces or parens, so the safe parse skips to the LAST
// ')' before splitting the remaining space-separated fields, same as every other /proc/[pid]/stat
// reader has to.
//
// ⚠️ Deliberately NOT /proc/self: that symlink resolves per CALLING THREAD, not per process, and
// the kernel only aggregates minflt across the whole thread group when the directory numerically
// names the group leader -- /proc/self/stat opened from any other thread (this runs on the GS
// thread, not main) reads back that one thread's own count, which sits near zero while every
// other thread's faults, and the RSS growth they cause, go uncounted. getpid() always returns the
// thread-group id regardless of which thread calls it, so the path is built from that instead of
// trusted to the symlink.
static u64 ReadMinorFaultsCumulative()
{
#if defined(__linux__)
	static const std::string path = "/proc/" + std::to_string(getpid()) + "/stat";
	static const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;

	char buf[512];
	const ssize_t len = pread(fd, buf, sizeof(buf) - 1, 0);
	if (len <= 0)
		return 0;
	buf[len] = '\0';

	const char* rparen = std::strrchr(buf, ')');
	if (!rparen)
		return 0;

	// From field 3 (state) onward: state, ppid, pgrp, session, tty_nr, tpgid, flags, minflt --
	// skip the first seven to land on minflt (field 10).
	unsigned long long minflt = 0;
	if (std::sscanf(rparen + 1, "%*s %*d %*d %*d %*d %*d %*u %llu", &minflt) != 1)
		return 0;

	return static_cast<u64>(minflt);
#else
	return 0;
#endif
}

// Per-frame statistics series. Run-aggregate min/avg/max cannot locate a spike, so
// every presented frame is recorded and written out as JSON at the end of the run.
// Counters are exact per-frame deltas; frame_ms is measured here rather than taken
// from PerformanceMetrics, whose values are window averages.
struct FrameSample
{
	u32 frame;
	/// The same frame's index WITHIN the dump. `frame` is monotonic across the whole
	/// run, so under -loop N it says nothing about where in the dump a sample sits;
	/// this one resets on every wrap, which is what separates the cold first loop from
	/// steady state.
	///
	/// It is the replayer's counter as published to the GS thread -- the identical
	/// value, read at the identical instant, that names the presented-frame PNG, so a
	/// sample and a frame dump join on it exactly. That publication rides the CPU
	/// thread's message pump, so at a loop boundary the reset can arrive one present
	/// late: cut the series where this DECREASES, not where it equals zero.
	u32 frame_in_dump;
	bool idle;
	float frame_ms;
	float gpu_ms;

	/// CPU time the GS thread itself burned producing this frame, in milliseconds.
	///
	/// This is the numerator of the ladder's absolute per-draw CPU budget, and it is
	/// deliberately not frame_ms: frame_ms is wall clock, so it carries the frame
	/// limiter, the GPU's pace and every other thread's contention, none of which the
	/// renderer's per-draw cost can be held responsible for. Thread CPU time carries
	/// only what this thread executed.
	float gs_cpu_ms;

	u64 prims;
	u64 draws; // PS2-level (GSPerfMon::Draw)
	u64 draw_calls;
	u64 render_passes;
	u64 render_pass_area_pixels;
	u64 barriers;
	u64 copies;
	u64 uploads;
	u64 readbacks;
	/// Times the GS thread blocked on the GPU out of turn (readback submit-and-wait, explicit
	/// sync). One per frame serializes the whole pipeline, so the number to read is whether it
	/// is zero, not whether it fell.
	u64 gpu_blocking_waits;

	/// This frame's share of the device's four wait bills, in nanoseconds. The run totals have
	/// always been there; the per-frame deltas are what says WHICH frames paid, and a run whose
	/// p95 moved while its total barely did is exactly the shape those totals cannot show.
	///
	/// sync and ring are fence waits, so their nanoseconds are off-CPU and do not appear in
	/// gs_cpu_ms. ⚠️ submit and map are not: a queue submit is a syscall and a cache invalidate is
	/// cache maintenance, so part of those nanoseconds is ALSO inside gs_cpu_ms -- on the M2,
	/// stuntman burns 235 ms inside vkQueueSubmit and its gs_cpu_ms sits within a millisecond of
	/// its frame_ms. Read those two beside the untracked residual, never subtracted from it.
	u64 sync_wait_ns;
	u64 ring_wait_ns;
	u64 submit_wait_ns;
	u64 map_wait_ns;
	/// Command buffers handed to the queue this frame. A frame that is GPU-synchronous pays a
	/// pipeline bubble per submit, so this is the multiplier on every submit-side cost.
	u64 submits;

	u64 copies_rov;
	u64 draw_calls_rov;
	u64 barriers_rov;
	u64 tc_source_hit;
	u64 tc_source_miss;
	u64 tc_target_hit;
	u64 tc_target_miss;
	u64 hash_cache_hit;
	u64 hash_cache_miss;
	u64 pipeline_switches;

	/// Process resident set size in kB at the end of this frame. Per frame rather than
	/// once at the end because the shape is the finding: a run that leaks and a run that
	/// merely started big have the same closing figure and different curves, and a
	/// 50-loop run is exactly where that difference shows up.
	u64 rss_kb;

	/// Minor page faults since the previous sample (first sample reads as the whole run
	/// so far, same convention as sync_wait_ns). Exists to tell a loop-count-sensitive
	/// warm-up fault tax apart from real per-frame churn -- rss_kb alone can plateau
	/// while faults are still high if pages are being re-faulted without growing RSS,
	/// and can grow without a fault spike if the growth came from one large mmap.
	u64 minflt_delta;
};
// Work posted from other threads (the PINE server) to run on the CPU thread.
static std::mutex s_cpu_thread_tasks_mutex;
static std::condition_variable s_cpu_thread_tasks_done;
static std::deque<std::function<void()>> s_cpu_thread_tasks;

static std::string s_stats_json_path;
static std::string s_drawlog_path;
static std::string s_fedump_path;
static std::string s_fediff_path;
static std::vector<FrameSample> s_frame_samples;
static std::string s_device_name;
static std::string s_driver_info;
static std::string s_stream_ring_memory;
// Latched every frame for the same reason as the wait bill: the device is gone by the time the
// stats file is written, and these counters live on it.
static GSDevice::StreamRingFlushStats s_stream_ring_flush;
static u64 s_frame_timer_last = 0;
static u64 s_gs_cpu_time_last = 0;
static u64 s_minflt_last = 0;
// Previous frame's wait bill, so each sample carries its own delta rather than a running total.
static DeviceWaitBill s_device_wait_bill_last;
static bool s_saw_gs_back_thread_in_stats = false;
static double s_last_prims = 0;
static double s_last_tc_source_hit = 0;
static double s_last_tc_source_miss = 0;
static double s_last_tc_target_hit = 0;
static double s_last_tc_target_miss = 0;
static double s_last_hash_cache_hit = 0;
static double s_last_hash_cache_miss = 0;
static double s_last_pipeline_switches = 0;
static u64 s_total_pipeline_switches = 0;

// --- Per-frame byte census (SD662 tier).
//
// One table rather than a static pair per counter, because the point of the census is the
// per-class total and a table can be summed. `cls` is what the byte movement is, and it is
// the load-bearing field: on single-channel LPDDR4X read+write traffic runs 5.2x behind the
// SD865 at 16 MiB and 8.75x at 64 MiB while write-only traffic holds a flat ~3x, so two paths
// moving the same number of bytes do not cost the same and the class says which is which.
// Emitted as totals; bytes per frame is the total over drawn_frames, which is the figure the
// census tabulates.
struct ByteCounterDesc
{
	GSPerfMon::counter_t counter;
	const char* name;
	const char* cls; ///< "rw" read+write, "w" write-only, "r" read-only
};
static constexpr ByteCounterDesc s_byte_counters[] = {
	{GSPerfMon::BytesGifImageIn, "gif_image_in", "rw"},
	{GSPerfMon::BytesGifImageOut, "gif_image_out", "rw"},
	{GSPerfMon::BytesTexReadVmem, "tex_read_vmem", "rw"},
	{GSPerfMon::BytesTexExpandOut, "tex_expand_out", "rw"},
	{GSPerfMon::BytesHashReadVmem, "hash_read_vmem", "rw"},
	{GSPerfMon::BytesHashExpandOut, "hash_expand_out", "rw"},
	{GSPerfMon::BytesUploadRing, "upload_ring", "rw"},
	{GSPerfMon::BytesReadbackToVmem, "readback_to_vmem", "rw"},
	{GSPerfMon::BytesVertexStream, "vertex_stream", "w"},
	{GSPerfMon::BytesIndexStream, "index_stream", "w"},
	{GSPerfMon::BytesUniformStream, "uniform_stream", "w"},
	{GSPerfMon::BytesLocalMemClear, "local_mem_clear", "w"},
	{GSPerfMon::BytesSwSprite, "sw_sprite", "w"},
	{GSPerfMon::BytesGifPacket, "gif_packet", "rw"},
	{GSPerfMon::BytesHashed, "hashed", "r"},
};
static constexpr size_t NUM_BYTE_COUNTERS = std::size(s_byte_counters);
static u64 s_total_bytes[NUM_BYTE_COUNTERS] = {};
static double s_last_bytes[NUM_BYTE_COUNTERS] = {};
static u64 s_total_prims = 0;
static u64 s_total_tc_source_hit = 0;
static u64 s_total_tc_source_miss = 0;
static u64 s_total_tc_target_hit = 0;
static u64 s_total_tc_target_miss = 0;
static u64 s_total_hash_cache_hit = 0;
static u64 s_total_hash_cache_miss = 0;

static bool s_perf_enable = false;
static bool s_force_vsync = false;

// Console replay payload emission. This runs and exits before any VM or GS device is
// created -- the dump is a replay script rather than a recording, so turning one into
// something a PlayStation 2 can execute is close to a file transform.
static bool s_emit_payload = false;
static GSReplayPayload::Options s_payload_opts;
static GSLadder::Options s_ladder_opts;
static float s_perf_updates = 0.0f;
static float s_perf_sum_fps = 0.0f;
static float s_perf_sum_internal_fps = 0.0f;
static float s_perf_sum_cpu_thread_usage = 0.0f;
static float s_perf_sum_cpu_thread_time = 0.0f;
static float s_perf_sum_gs_thread_usage = 0.0f;
static float s_perf_sum_gs_thread_time = 0.0f;
static float s_perf_sum_gs_back_thread_usage = 0.0f;
static float s_perf_sum_gs_back_thread_time = 0.0f;
// Latched during the run: DumpStats() runs after VMManager::Shutdown(), by which point the
// back thread has joined and PerformanceMetrics would report it as never having existed.
static bool s_perf_saw_gs_back_thread = false;
static float s_perf_sum_gpu_time = 0.0f;
static float s_perf_sum_gpu_usage = 0.0f;

// Failures that happen before the log is usable are reported through here.
//
// Two things hide them otherwise. The file log only opens during VM startup, which is
// after argument parsing and after InitializeConfig, so nothing written before then ever
// reaches an emulog. And the measurement harnesses run with PCSX2_NOCONSOLE set, which
// puts the console sink at LOGLEVEL_NONE (see InitializeConsole), so Console.Error
// reaches nothing either. A run that died in either place left an empty emulog, an empty
// terminal and exit code 1 -- which is exactly what a crash looks like from outside. That
// is how a staged binary older than the flag its caller had just learned cost a device
// round: it rejected the argument and said nothing.
//
// stderr is written unconditionally. PCSX2_NOCONSOLE asks for a quiet log, not for a
// fatal error to be swallowed.
template <typename... T>
static void EarlyError(fmt::format_string<T...> format, T&&... args)
{
	const std::string message = fmt::format(format, std::forward<T>(args)...);
	std::fprintf(stderr, "pcsx2-gsrunner: %s\n", message.c_str());
	std::fflush(stderr);

	// Only when the console sink is off, otherwise the terminal gets the same line twice.
	// This is for the file and host sinks, on the chance one is already open.
	if (!Log::IsConsoleOutputEnabled())
		Console.Error(message);
}

// The same, plus the pointer to -help. Everything ParseCommandLineArgs rejects uses this,
// so a caller holding a wrong command line is told both what is wrong and where the list
// of accepted arguments is.
template <typename... T>
static void ArgError(fmt::format_string<T...> format, T&&... args)
{
	EarlyError(format, std::forward<T>(args)...);
	std::fprintf(stderr, "pcsx2-gsrunner: run with -help for the arguments this build accepts.\n");
	std::fflush(stderr);
}

// Parses a numeric flag argument, rejecting anything that is not entirely a number.
// These were FromChars<>(...).value_or(<default>), which silently substituted the default
// for a typo: '-loop tow' looped forever and '-swthreads x' ran with none, both without a
// word, and both looking from outside like the run that was asked for.
template <typename T>
static std::optional<T> ParseNumericArg(const char* flag, const std::string_view text)
{
	const std::string_view trimmed = StringUtil::StripWhitespace(text);
	std::string_view rest;
	std::optional<T> value;
	if constexpr (std::is_integral_v<T>)
		value = StringUtil::FromChars<T>(trimmed, 10, &rest);
	else
		value = StringUtil::FromChars<T>(trimmed, &rest);

	if (!value.has_value() || !rest.empty())
	{
		ArgError("{}: '{}' is not a number.", flag, text);
		return std::nullopt;
	}

	return value;
}

bool GSRunner::InitializeConfig()
{
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory())
	{
		EarlyError("resources directory '{}' is missing (looked for it under the application root '{}'). "
				   "A staged tree copied without dereferencing symlinks lands here.",
			EmuFolders::Resources, EmuFolders::AppRoot);
		return false;
	}

	Error data_error;
	if (!EmuFolders::SetDataDirectory(&data_error))
	{
		EarlyError("could not create the data directory '{}' or its inis subdirectory: {}", EmuFolders::DataRoot,
			data_error.GetDescription());
		return false;
	}

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
	{
		// Those messages are written for a dialog box and carry embedded newlines. Flatten
		// them, so the reason is one line a harness log can be grepped for.
		std::string text(error ? error : "no reason given");
		std::replace(text.begin(), text.end(), '\n', ' ');
		EarlyError("early hardware check failed: {}", text);
		return false;
	}

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			EarlyError("could not read the font '{}' (resources directory '{}').", roboto_path, EmuFolders::Resources);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);

		ImGuiManager::SetFonts(std::move(fonts));
	}

	// don't provide an ini path, or bother loading. we'll store everything in memory.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);

	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void Host::CommitBaseSettingChanges()
{
	// nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	// not running any UI, so no settings requests will come in
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

bool Host::LocaleCircleConfirm()
{
	// not running any UI, so no settings requests will come in
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
	// noop
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
	// noop
}

void Host::EndTextInput()
{
	// noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	// Before anything else: this is the GS thread, at the boundary where the frame's
	// GS work is submitted but not yet presented, which is where a RenderDoc capture
	// has to open and close.
	RenderDocCapture::OnPresentFrame(s_dump_frame_number);

	if (s_loop_number == 0 && !s_output_prefix.empty())
	{
		// when we wrap around, don't race other files
		GSJoinSnapshotThreads();

		// queue dumping of this frame
		std::string dump_path(fmt::format("{}_frame{:05}.png", s_output_prefix, s_dump_frame_number));
		GSQueueSnapshot(dump_path);
	}

	if (GSIsHardwareRenderer())
	{
		// Captured here rather than at shutdown: this runs on the GS thread with the
		// device definitely live, and it is the axis that decides whether a settings
		// A/B was even applied (several GS features are force-overridden per-driver).
		if (s_device_name.empty() && g_gs_device)
		{
			s_device_name = g_gs_device->GetName();
			s_driver_info = g_gs_device->GetDriverInfo();
			// Which host-visible memory the backend's stream rings landed in, in the same words the
			// device banner uses. A round's numbers are only readable against the road the device
			// took, and reconstructing that from a log after the fact is how a round gets
			// attributed to the wrong one.
			s_stream_ring_memory = g_gs_device->GetStreamRingMemoryDescription();
		}

		// Same reasoning, every frame rather than once: the device's wait counters have to be read
		// somewhere the device certainly exists, and this is the only per-frame point that qualifies.
		LatchDeviceWaitBill();
		if (g_gs_device)
			s_stream_ring_flush = g_gs_device->GetStreamRingFlushStats();

		const u32 last_draws = s_total_internal_draws;

		// Returns this frame's delta as well as accumulating it, so the per-frame
		// series and the run totals stay derived from one source.
		static constexpr auto update_stat = [](GSPerfMon::counter_t counter, u64& dst, double& last) -> u64 {
			// perfmon resets every 32 frames to zero
			const double val = g_perfmon.GetCounter(counter);
			const u64 delta = static_cast<u64>((val < last) ? val : (val - last));
			dst += delta;
			last = val;
			return delta;
		};

		FrameSample sample = {};
		sample.frame = s_total_frames;
		sample.frame_in_dump = s_dump_frame_number;
		sample.prims = update_stat(GSPerfMon::Prim, s_total_prims, s_last_prims);
		sample.draws = update_stat(GSPerfMon::Draw, s_total_internal_draws, s_last_internal_draws);
		sample.draw_calls = update_stat(GSPerfMon::DrawCalls, s_total_draws, s_last_draws);
		sample.render_passes = update_stat(GSPerfMon::RenderPasses, s_total_render_passes, s_last_render_passes);
		sample.render_pass_area_pixels = update_stat(
			GSPerfMon::RenderPassAreaPixels, s_total_render_pass_area_pixels, s_last_render_pass_area_pixels);
		sample.barriers = update_stat(GSPerfMon::Barriers, s_total_barriers, s_last_barriers);
		sample.copies = update_stat(GSPerfMon::TextureCopies, s_total_copies, s_last_copies);
		sample.uploads = update_stat(GSPerfMon::TextureUploads, s_total_uploads, s_last_uploads);
		sample.readbacks = update_stat(GSPerfMon::Readbacks, s_total_readbacks, s_last_readbacks);
		sample.gpu_blocking_waits =
			update_stat(GSPerfMon::GpuBlockingWaits, s_total_gpu_blocking_waits, s_last_gpu_blocking_waits);
		sample.copies_rov = update_stat(GSPerfMon::TextureCopiesROV, s_total_copies_rov, s_last_depth_copies_rov);
		sample.draw_calls_rov = update_stat(GSPerfMon::DrawCallsROV, s_total_draws_rov, s_last_draws_rov);
		sample.barriers_rov = update_stat(GSPerfMon::BarriersROV, s_total_barriers_rov, s_last_barriers_rov);
		sample.tc_source_hit = update_stat(GSPerfMon::TCSourceHit, s_total_tc_source_hit, s_last_tc_source_hit);
		sample.tc_source_miss = update_stat(GSPerfMon::TCSourceMiss, s_total_tc_source_miss, s_last_tc_source_miss);
		sample.tc_target_hit = update_stat(GSPerfMon::TCTargetHit, s_total_tc_target_hit, s_last_tc_target_hit);
		sample.tc_target_miss = update_stat(GSPerfMon::TCTargetMiss, s_total_tc_target_miss, s_last_tc_target_miss);
		sample.hash_cache_hit = update_stat(GSPerfMon::HashCacheHit, s_total_hash_cache_hit, s_last_hash_cache_hit);
		sample.hash_cache_miss = update_stat(GSPerfMon::HashCacheMiss, s_total_hash_cache_miss, s_last_hash_cache_miss);
		sample.pipeline_switches = update_stat(GSPerfMon::PipelineSwitches, s_total_pipeline_switches, s_last_pipeline_switches);
		for (size_t bc = 0; bc < NUM_BYTE_COUNTERS; bc++)
			update_stat(s_byte_counters[bc].counter, s_total_bytes[bc], s_last_bytes[bc]);

		// A frame is drawn if it carried PS2 draws. The upstream heuristic also counted a
		// frame with only texture uploads as drawn; under Tile every present-only frame
		// carries one upload (the floor's framebuffer reaching the display texture), so
		// that definition made half of a Tile run's frames "drawn" and put ~1 ms
		// present-only frames into the same percentile as 18 ms drawn ones -- Tile's p50
		// read as 1.0 ms while its drawn frames were 18. Draws are the honest test.
		const bool idle_frame = s_total_frames && (last_draws == s_total_internal_draws);

		if (!idle_frame)
			s_total_drawn_frames++;

		s_total_frames++;

		if (!s_stats_json_path.empty())
		{
			const u64 now = Common::Timer::GetCurrentValue();
			sample.idle = idle_frame;
			// First frame has no predecessor to measure against.
			sample.frame_ms = s_frame_timer_last ?
			                      static_cast<float>(Common::Timer::ConvertValueToMilliseconds(now - s_frame_timer_last)) :
			                      0.0f;
			s_frame_timer_last = now;
			sample.gpu_ms = PerformanceMetrics::GetLastGPUTime();

			// Thread CPU time, sampled here on the GS thread itself, so the frame's
			// delta is what this thread executed between two presents. Under a
			// GSBackThreadMode above Off the back thread carries part of the work and
			// is not sampled here; the run summary says so, because a per-draw figure
			// taken from half the work would read as a win.
			const u64 gs_cpu_now = MTGS::GetThreadHandle().GetCPUTime();
			sample.gs_cpu_ms = (s_gs_cpu_time_last && gs_cpu_now > s_gs_cpu_time_last) ?
			                       static_cast<float>(static_cast<double>(gs_cpu_now - s_gs_cpu_time_last) * 1000.0 /
			                                          static_cast<double>(Threading::GetThreadTicksPerSecond())) :
			                       0.0f;
			s_gs_cpu_time_last = gs_cpu_now;

			// The device's counters are monotonic and were latched a few lines above this block,
			// on this thread, with the device live -- so the delta since the last present is this
			// frame's share exactly. First sample has no predecessor and reads as the whole run so
			// far, which is the same convention frame_ms already uses for frame zero.
			const DeviceWaitBill& w = s_device_wait_bill;
			const DeviceWaitBill& wprev = s_device_wait_bill_last;
			sample.sync_wait_ns = w.sync_ns - wprev.sync_ns;
			sample.ring_wait_ns = w.ring_ns - wprev.ring_ns;
			sample.submit_wait_ns = w.submit_ns - wprev.submit_ns;
			sample.map_wait_ns = w.map_ns - wprev.map_ns;
			sample.submits = w.submit_calls - wprev.submit_calls;
			s_device_wait_bill_last = w;

			sample.rss_kb = ReadResidentSetKB();

			// Same "first sample reads as the whole run so far" convention as the wait bill
			// above: the pre-frame-0 warm-up burst (process start through the first present)
			// is exactly the kind of thing this counter exists to catch, so frame 0 is not
			// zeroed. A read failure comes back as 0 from ReadMinorFaultsCumulative, which a
			// live process's monotonic count can never legitimately return to once it has
			// already advanced past zero -- treated as a dropped sample rather than recorded
			// as a fabricated delta, and s_minflt_last is left alone so the next successful
			// read still deltas against the last known-good value.
			const u64 minflt_now = ReadMinorFaultsCumulative();
			if (minflt_now == 0 && s_minflt_last != 0)
			{
				sample.minflt_delta = 0;
			}
			else
			{
				sample.minflt_delta = minflt_now - s_minflt_last;
				s_minflt_last = minflt_now;
			}

			s_saw_gs_back_thread_in_stats |= PerformanceMetrics::HasGSBackThread();
			s_frame_samples.push_back(sample);
		}

		std::atomic_thread_fence(std::memory_order_release);
	}
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
	if (s_perf_enable)
	{
		s_perf_updates += 1.0f;
		s_perf_sum_fps += PerformanceMetrics::GetFPS();
		s_perf_sum_internal_fps += PerformanceMetrics::GetInternalFPS();
		s_perf_sum_cpu_thread_usage += PerformanceMetrics::GetCPUThreadUsage();
		s_perf_sum_cpu_thread_time += PerformanceMetrics::GetCPUThreadAverageTime();
		s_perf_sum_gs_thread_usage += PerformanceMetrics::GetGSThreadUsage();
		s_perf_sum_gs_thread_time += PerformanceMetrics::GetGSThreadAverageTime();
		s_perf_sum_gs_back_thread_usage += PerformanceMetrics::GetGSBackThreadUsage();
		s_perf_sum_gs_back_thread_time += PerformanceMetrics::GetGSBackThreadAverageTime();
		s_perf_saw_gs_back_thread |= PerformanceMetrics::HasGSBackThread();
		s_perf_sum_gpu_time += PerformanceMetrics::GetGPUAverageTime();
		s_perf_sum_gpu_usage += PerformanceMetrics::GetGPUUsage();
	}
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	// Queued here and drained in PumpMessagesOnCPUThread(). Previously a hard
	// pxFailRel, which meant any PINE command that marshals to the CPU thread
	// (settings apply, savestates, frame advance) aborted the whole run.
	std::unique_lock lock(s_cpu_thread_tasks_mutex);
	s_cpu_thread_tasks.push_back(std::move(function));

	if (!block)
		return;

	// Wait for the drain to reach our task. The generation counter is bumped once
	// per drain, so waiting for the queue to empty is enough.
	s_cpu_thread_tasks_done.wait(lock, []() { return s_cpu_thread_tasks.empty(); });
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
	// noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	// noop
}

bool Host::HasNativeAchievementNotifications() { return false; }
void Host::OnAchievementNotification(const char*, float, const char*, const char*, const char*) {}

void Host::OnAchievementsRefreshed()
{
	// noop
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void PrintCommandLineVersion()
{
	std::fprintf(stderr, "PCSX2 GS Runner Version %s\n", GIT_REV);
	std::fprintf(stderr, "https://pcsx2.net/\n");
	std::fprintf(stderr, "\n");
}

static void PrintCommandLineHelp(const char* progname)
{
	PrintCommandLineVersion();
	std::fprintf(stderr, "Usage: %s [parameters] [--] [filename]\n", progname);
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "  -help: Displays this information and exits.\n");
	std::fprintf(stderr, "  -version: Displays version information and exits.\n");
	std::fprintf(stderr, "  -dumpdir <dir>: Frame dump directory (will be dumped as filename_frameN.png).\n");
	std::fprintf(stderr, "  -dumpdirhw <dir>: Directory for the hardware renderer's -dump output. Defaults to -dumpdir.\n");
	std::fprintf(stderr, "  -dumpdirsw <dir>: Directory for the software renderer's -dump output. Defaults to -dumpdir.\n");
	std::fprintf(stderr, "  -dump [rt|tex|z|f|a|i|tr|ds|fs|hw]: Enabling dumping of render target, texture, z buffer, frame, "
		"alphas, and info (context, vertices, list of transfers), transfers images, draw stats, frame stats, HW config, respectively, per draw. Generates lots of data.\n");
	std::fprintf(stderr, "  -dumprange N[,L,B]: Start dumping from draw N (base 0), stops after L draws, and only "
		"those draws that are multiples of B (intersection of -dumprange and -dumprangef used)."
		"Defaults to 0,-1,1 (all draws). Only used if -dump used.\n");
	std::fprintf(stderr, "  -dumprangef NF[,LF,BF]: Start dumping from frame NF (base 0), stops after LF frames, "
		"and only those frames that are multiples of BF (intersection of -dumprange and -dumprangef used).\n"
		"Defaults to 0,-1,1 (all frames). Only used if -dump is used.\n");
	std::fprintf(stderr, "  -loop <count>: Loops dump playback N times. Defaults to 1. 0 will loop infinitely.\n");
	std::fprintf(stderr, "  -renderdoc <path>: Capture GS work with RenderDoc, writing <path>_frameN.rdc. gsrunner "
						 "triggers the capture itself, so no F12 and no RenderDoc UI are needed -- but RenderDoc must "
						 "already be in the process, so either launch from qrenderdoc/renderdoccmd or prefix the command "
						 "with LD_PRELOAD=/path/to/librenderdoc.so. Hardware renderers only. Prefer '-renderer vulkan "
						 "-surfaceless': RenderDoc's Vulkan capture drops VK_KHR_wayland_surface, so a windowed Vulkan "
						 "run cannot even create an instance under it.\n");
	std::fprintf(stderr, "  -renderdoc-frame N[,C]: Capture dump frame N (base 0, minimum 1) and the C-1 frames after it, "
						 "one .rdc each. Defaults to 1,1. Only used if -renderdoc is used.\n");
	std::fprintf(stderr, "  -renderer <renderer>: Sets the graphics renderer. Defaults to Auto.\n");
	std::fprintf(stderr, "  -variant <auto|classic>: Selects the HW renderer variant. This build has only the "
						 "classic renderer, so both values are the same run; any other value is an error.\n");
	std::fprintf(stderr, "  -swthreads <threads>: Sets the number of threads for the software renderer.\n");
	std::fprintf(stderr, "  -upscale <multiplier>: Sets the upscale multiplier, e.g. 1 for native or 2 for 2x. Minimum 0.5.\n");
	std::fprintf(stderr, "  -renderhacks [af|cpufb|dds|dpi|dsf|tinrt|plf]: Enable user hacks -- auto flush, CPU framebuffer "
						 "conversion, disable depth support, disable partial invalidation, disable safe features, texture "
						 "inside render target, preload frame with GS data, respectively.\n");
	std::fprintf(stderr, "  -ini <path>: Load the [EmuCore/GS] section of an INI file as settings overrides. Applied in "
						 "command-line order, so a later -set wins.\n");
	std::fprintf(stderr, "  -backthread <mode>: GS back-thread mode (0=off, 1=inline-records, 2=lockstep, 3=pipelined). Defaults to 0.\n");
	std::fprintf(stderr, "  -window: Forces a window to be displayed.\n");
	std::fprintf(stderr, "  -surfaceless: Disables showing a window.\n");
	std::fprintf(stderr, "  -logfile <filename>: Writes emu log to filename.\n");
	std::fprintf(stderr, "  -noshadercache: Disables the shader cache (useful for parallel runs).\n");
	std::fprintf(stderr, "  -debugdevice: Enable the graphics API debug device (Vulkan validation layers / GL debug output). "
						 "Slow; for diagnosing API misuse, not for measurement.\n");
	std::fprintf(stderr, "  -perf: Enable frame timing performance stats.\n");
	std::fprintf(stderr, "  -drawlog <path.csv>: Record a per-draw ledger (PS2 register state + backend draw config).\n");
	std::fprintf(stderr, "  -fedump <path>: Record the front-end decode surface -- every draw, upload, move, CLUT "
						 "load, readback request and frame boundary the GIF decode hands the renderer, in stream "
						 "order, as canonical bytes. The recording of the shipping decode is what a replacement "
						 "front end is diffed against.\n");
	std::fprintf(stderr, "  -fediff <path>: Replay under the same arm and byte-compare against a -fedump recording. "
						 "Reports the FIRST divergence (event, record, field, byte) and exits non-zero. Record and "
						 "diff must use the same dump, the same renderer arm and the same -loop count.\n");
	std::fprintf(stderr, "  -gspin <cpu[,cpu...]>: Pin the GS thread to these CPUs, e.g. '-gspin 4' or '-gspin 0,1,2,3'. "
						 "On a big.LITTLE device an unpinned GS thread migrates between core types mid-run, which moves "
						 "its CPU time without anything in the renderer changing. The pin is read back afterwards and "
						 "both the request and the result are written to -stats-json; a pin that did not take warns and "
						 "the run continues.\n");
	std::fprintf(stderr, "  -affinity <0-7>: Thread-placement mode handed to VMManager before the VM boots. "
						 "0 = unpinned (every emu thread gets every processor), 1-6 = explicit per-core placements by "
						 "EE/VU/GS priority, 7 = Performance Cores (confine the emu threads to the big tier). The "
						 "runner defaults to 0 regardless of what the app build defaults to, because an app policy "
						 "inherited silently makes two rounds incomparable without either of them saying so. Written "
						 "to -stats-json as affinity_mode / affinity_source, alongside inherited_cpu_mask, the CPU set "
						 "this process started with. No effect on platforms with no affinity path (a notice is "
						 "logged).\n");
	std::fprintf(stderr, "  -emit-payload <path>: Transform the dump into a console replay payload and exit. Needs no VM, "
						 "no GS device and no window -- the dump already carries the freeze and the packet stream.\n");
	std::fprintf(stderr, "  -payload-frames <count>: Stop the emitted payload after this many dump frames. 0 (the default) "
						 "means all of them. Only used with -emit-payload.\n");
	std::fprintf(stderr, "  -payload-readback bp,bw,psm,w,h | bp,bw,psm,x,y,w,h: The region every payload checkpoint reads "
						 "back. Left alone it comes from the freeze's context-0 FRAME, which is wrong for a dump that "
						 "renders somewhere other than where it displays. Only used with -emit-payload.\n");
	std::fprintf(stderr, "  -payload-ladder <n>: Emit a payload checkpoint every n draws as well as at frame boundaries. "
						 "Only used with -emit-payload.\n");
	std::fprintf(stderr, "  -ladder bp,bw,psm,x,y,w,h: Read back this window at every rung of a host-side ladder run, the "
						 "arm the console payload is compared against. Keep the window small: a rung is only useful if "
						 "hundreds of them fit.\n");
	std::fprintf(stderr, "  -ladder-every <n>: Take a ladder rung every n draws. Only used if -ladder is used.\n");
	std::fprintf(stderr, "  -ladder-out <path>: Where to write the ladder rungs. Only used if -ladder is used.\n");
	std::fprintf(stderr, "  -stats-json <path>: Write per-frame and run-summary statistics as JSON. Combine with -perf "
						 "for frame/GPU timing.\n");
	std::fprintf(stderr, "  -set <Section/Key>=<value>: Override any setting, e.g. -set EmuCore/GS/AccurateBlendingUnit=3. "
						 "Repeatable.\n");
	std::fprintf(stderr, "  -vsync: Force vsync on (FIFO present mode). Workaround for libmali Wayland WSI which "
						 "advertises MAILBOX support but errors VK_ERROR_INITIALIZATION_FAILED on swapchain create.\n");
	std::fprintf(stderr, "  -no-fb-fetch: Disable Vulkan framebuffer fetch (VK_EXT_rasterization_order_attachment_access). "
						 "Use to A/B against drivers that mishandle subpass self-dependencies (e.g. libmali).\n");
	std::fprintf(stderr, "  -no-dual-source: Report no dual-source blend unit, the way every Mali Vulkan blob does. "
						 "Makes GSRendererHW take the SRC1 substitution and SW-blend fallbacks, so a Mali-only blending "
						 "bug reproduces on a desktop GPU.\n");
	std::fprintf(stderr, "  -broken-blend-constant: Report the driver as ignoring the Vulkan blend constant, the way Mesa "
						 "Turnip does on some draws. GSRendererHW then sends a fixed (AFIX) blend factor through the "
						 "second fragment output instead of vkCmdSetBlendConstants, so that road can be A/B'd on a "
						 "machine whose driver is fine.\n");
	std::fprintf(stderr, "  -no-vs-expand: Disable vertex-shader point/line/sprite expansion (storage-buffer path). "
						 "Falls back to hardware/geometry expansion.\n");
	std::fprintf(stderr, "  -no-tex-barriers: Force OverrideTextureBarriers=0. Disables the texture-barrier render-pass pattern "
						 "and the framebuffer-fetch / depth-feedback paths that build on it.\n");
	std::fprintf(stderr, "  -accblend <0-5>: Force accurate blending unit (0=Minimum, 1=Basic, 2=Medium, 3=High, 4=Full, 5=Maximum). "
						 "Overrides the game/global default; use to exercise the SW-blend / fb-fetch (ROV) path headlessly.\n");
	std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
						 "    parameters make up the filename. Use when the filename contains\n"
						 "    spaces or starts with a dash.\n");
	std::fprintf(stderr, "\n");
}

void GSRunner::InitializeConsole()
{
	const char* var = std::getenv("PCSX2_NOCONSOLE");
	s_no_console = (var && StringUtil::FromChars<bool>(var).value_or(false));
	if (!s_no_console)
		Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
}

// Renders a CPU-affinity mask as the comma list the stats JSON carries: 4, or 0,1,2,3.
static std::string FormatCpuMask(u64 mask)
{
	std::string out;
	for (u32 i = 0; i < 64; i++)
	{
		if (!(mask & (static_cast<u64>(1) << i)))
			continue;
		if (!out.empty())
			out.push_back(',');
		out += std::to_string(i);
	}
	return out;
}

// Parses the -gspin argument, a comma-separated list of CPU indices, into an affinity
// mask. Rejects anything that is not a number, but tolerates an index this mask cannot
// express (>= 64) by dropping it -- whether the CPU exists is the kernel's answer to
// give at pin time, not something to guess at parse time.
static bool ParseCpuList(const std::string_view list, u64* mask)
{
	*mask = 0;
	size_t pos = 0;
	while (pos <= list.size())
	{
		const size_t comma = list.find(',', pos);
		const std::string_view tok =
			StringUtil::StripWhitespace(list.substr(pos, (comma == std::string_view::npos) ? std::string_view::npos : (comma - pos)));
		if (tok.empty())
			return false;

		const std::optional<u32> cpu = StringUtil::FromChars<u32>(tok);
		if (!cpu.has_value())
			return false;
		if (cpu.value() < 64)
			*mask |= (static_cast<u64>(1) << cpu.value());

		if (comma == std::string_view::npos)
			break;
		pos = comma + 1;
	}
	return true;
}

bool GSRunner::ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params)
{
	std::string dumpdir; // Save from argument -dumpdir for creating sub-directories
	bool no_more_args = false;
	for (int i = 1; i < argc; i++)
	{
		// A flag that takes a parameter but was given none used to fail its CHECK_ARG_PARAM
		// test, fall through the whole chain, and come out of the unknown-argument branch at
		// the bottom -- reported as an unknown flag, which sends the reader hunting a typo
		// that is not there. The name is recorded here instead, and a branch just above that
		// one says what is actually wrong.
		const char* missing_param = nullptr;
		if (!no_more_args)
		{
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && (((i + 1) < argc) ? true : ((missing_param = (str)), false)))

			if (CHECK_ARG("-help"))
			{
				PrintCommandLineHelp(argv[0]);
				return false;
			}
			else if (CHECK_ARG("-version"))
			{
				PrintCommandLineVersion();
				return false;
			}
			else if (CHECK_ARG_PARAM("-dumpdir"))
			{
				dumpdir = s_output_prefix = StringUtil::StripWhitespace(argv[++i]);
				if (s_output_prefix.empty())
				{
					ArgError("-dumpdir: the directory name is empty.");
					return false;
				}

				if (!FileSystem::DirectoryExists(s_output_prefix.c_str()) && !FileSystem::CreateDirectoryPath(s_output_prefix.c_str(), false))
				{
					ArgError("-dumpdir: could not create the output directory '{}'.", s_output_prefix);
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-dump"))
			{
				std::string str(argv[++i]);

				s_settings_interface.SetBoolValue("EmuCore/GS", "DumpGSData", true);

				if (str.find("rt") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveRT", true);
				if (str.find("f") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveFrame", true);
				if (str.find("tex") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveTexture", true);
				if (str.find("z") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveDepth", true);
				if (str.find("a") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveAlpha", true);
				if (str.find("i") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveInfo", true);
				if (str.find("tr") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveTransferImages", true);
				if (str.find("ds") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveDrawStats", true);
				if (str.find("fs") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveFrameStats", true);
				if (str.find("hw") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveHWConfig", true);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprange"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					const std::optional<int> v = ParseNumericArg<int>("-dumprange", split[0]);
					if (!v.has_value())
						return false;
					start = v.value();
				}
				if (split.size() > 1)
				{
					const std::optional<int> v = ParseNumericArg<int>("-dumprange", split[1]);
					if (!v.has_value())
						return false;
					num = v.value();
				}
				if (split.size() > 2)
				{
					const std::optional<int> v = ParseNumericArg<int>("-dumprange", split[2]);
					if (!v.has_value())
						return false;
					by = std::max(1, v.value());
				}
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawStart", start);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawCount", num);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprangef"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					const std::optional<int> v = ParseNumericArg<int>("-dumprangef", split[0]);
					if (!v.has_value())
						return false;
					start = v.value();
				}
				if (split.size() > 1)
				{
					const std::optional<int> v = ParseNumericArg<int>("-dumprangef", split[1]);
					if (!v.has_value())
						return false;
					num = v.value();
				}
				if (split.size() > 2)
				{
					const std::optional<int> v = ParseNumericArg<int>("-dumprangef", split[2]);
					if (!v.has_value())
						return false;
					by = std::max(1, v.value());
				}
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameStart", start);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameCount", num);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderdoc"))
			{
				s_renderdoc_path = StringUtil::StripWhitespace(argv[++i]);
				if (s_renderdoc_path.empty())
				{
					ArgError("-renderdoc: the capture path is empty.");
					return false;
				}
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderdoc-frame"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				if (split.size() > 0)
				{
					const std::optional<u32> v = ParseNumericArg<u32>("-renderdoc-frame", split[0]);
					if (!v.has_value())
						return false;
					s_renderdoc_start_frame = v.value();
				}
				if (split.size() > 1)
				{
					const std::optional<u32> v = ParseNumericArg<u32>("-renderdoc-frame", split[1]);
					if (!v.has_value())
						return false;
					s_renderdoc_frame_count = std::max(1u, v.value());
				}
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirhw"))
			{
				s_settings_interface.SetStringValue("EmuCore/GS", "HWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirsw"))
			{
				s_settings_interface.SetStringValue("EmuCore/GS", "SWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-loop"))
			{
				// A typo here used to become 0, which is the spelling of "loop forever".
				const std::optional<s32> count = ParseNumericArg<s32>("-loop", argv[++i]);
				if (!count.has_value())
					return false;
				s_loop_count = count.value();
				Console.WriteLn("Looping dump playback %d times.", s_loop_count);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderer"))
			{
				const char* rname = argv[++i];

				GSRendererType type = GSRendererType::Auto;
				if (StringUtil::Strcasecmp(rname, "Auto") == 0)
					type = GSRendererType::Auto;
#ifdef _WIN32
				else if (StringUtil::Strcasecmp(rname, "dx11") == 0)
					type = GSRendererType::DX11;
				else if (StringUtil::Strcasecmp(rname, "dx12") == 0)
					type = GSRendererType::DX12;
#endif
#ifdef ENABLE_OPENGL
				else if (StringUtil::Strcasecmp(rname, "gl") == 0)
					type = GSRendererType::OGL;
#endif
#ifdef ENABLE_VULKAN
				else if (StringUtil::Strcasecmp(rname, "vulkan") == 0)
					type = GSRendererType::VK;
#endif
#ifdef __APPLE__
				else if (StringUtil::Strcasecmp(rname, "metal") == 0)
					type = GSRendererType::Metal;
#endif
				else if (StringUtil::Strcasecmp(rname, "sw") == 0)
					type = GSRendererType::SW;
				else
				{
					ArgError("-renderer: unknown renderer '{}'.", rname);
					return false;
				}

				Console.WriteLn("Using %s renderer.", Pcsx2Config::GSOptions::GetRendererName(type));
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(type));
				continue;
			}
			else if (CHECK_ARG_PARAM("-variant"))
			{
				// This tree has exactly one hardware renderer, so the only variant it can run is
				// the one it already runs. The flag exists so a measurement harness that names the
				// arm on the command line works here unchanged -- and so that an arm this tree
				// cannot run FAILS THE RUN instead of quietly being measured as Classic under
				// another name, which is how a comparison ends up reporting two identical arms.
				const char* vname = argv[++i];
				if (StringUtil::Strcasecmp(vname, "auto") != 0 && StringUtil::Strcasecmp(vname, "classic") != 0)
				{
					ArgError("-variant: unknown HW renderer variant '{}' -- this build has only the classic "
							 "renderer (accepted: auto, classic).",
						vname);
					return false;
				}

				Console.WriteLn("Using classic HW renderer variant.");
				continue;
			}
			else if (CHECK_ARG_PARAM("-backthread"))
			{
				const char* mode_arg = argv[++i];
				const std::optional<int> parsed = ParseNumericArg<int>("-backthread", mode_arg);
				if (!parsed.has_value())
					return false;
				const int mode = parsed.value();
				if (mode < 0 || mode > 3)
				{
					ArgError("-backthread: mode '{}' is out of range (0=off, 1=inline-records, 2=lockstep, "
							 "3=pipelined).",
						mode_arg);
					return false;
				}

				Console.WriteLn("Setting GS back-thread mode to %d.", mode);
				s_settings_interface.SetIntValue("EmuCore/GS", "GSBackThreadMode", mode);
				continue;
			}
			else if (CHECK_ARG_PARAM("-swthreads"))
			{
				const std::optional<int> parsed = ParseNumericArg<int>("-swthreads", argv[++i]);
				if (!parsed.has_value())
					return false;
				const int swthreads = parsed.value();
				if (swthreads < 0)
				{
					ArgError("-swthreads: {} is negative.", swthreads);
					return false;
				}
				
				Console.WriteLn(fmt::format("Setting number of software threads to {}", swthreads));
				// The INI key is "extrathreads"; SWExtraThreads is the C++ member it loads
				// into (Pcsx2Config.cpp, SettingsWrapBitfieldEx). Writing the member name
				// wrote a key nothing reads, so this flag silently did nothing -- and it
				// is a flag used to CONTROL for software-rasterizer threading, so it
				// reported success while leaving the variable it claimed to pin unchanged.
				s_settings_interface.SetIntValue("EmuCore/GS", "extrathreads", swthreads);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderhacks"))
			{
				std::string str(argv[++i]);

				s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);

				if (str.find("af") != std::string::npos)
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_AutoFlushLevel", 1);
				if (str.find("cpufb") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_CPU_FB_Conversion", true);
				if (str.find("dds") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", true);
				if (str.find("dpi") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisablePartialInvalidation", true);
				if (str.find("dsf") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_Disable_Safe_Features", true);
				if (str.find("tinrt") != std::string::npos)
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", 1);
				if (str.find("plf") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "preload_frame_with_gs_data", true);

				continue;
			}
			else if (CHECK_ARG_PARAM("-ini"))
			{
				std::string path = std::string(StringUtil::StripWhitespace(argv[++i]));
				if (!FileSystem::FileExists(path.c_str()))
				{
					ArgError("-ini: no such file '{}'.", path);
					return false;
				}

				INISettingsInterface si_ini(path);

				if (!si_ini.Load())
				{
					ArgError("-ini: could not load settings from '{}'.", path);
					return false;
				}

				for (const auto& [key, value] : si_ini.GetKeyValueList("EmuCore/GS"))
					s_settings_interface.SetStringValue("EmuCore/GS", key.c_str(), value.c_str());

				continue;
			}
			else if (CHECK_ARG_PARAM("-upscale"))
			{
				const std::optional<float> parsed = ParseNumericArg<float>("-upscale", argv[++i]);
				if (!parsed.has_value())
					return false;
				const float upscale = parsed.value();
				if (upscale < 0.5f)
				{
					ArgError("-upscale: multiplier {} is below the minimum of 0.5.", upscale);
					return false;
				}

				Console.WriteLn(fmt::format("Setting upscale multiplier to {}", upscale));
				s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale);
				continue;
			}
			else if (CHECK_ARG_PARAM("-logfile"))
			{
				const char* logfile = argv[++i];
				if (std::strlen(logfile) > 0)
				{
					// disable timestamps, since we want to be able to diff the logs
					Console.WriteLn("Logging to %s...", logfile);
					VMManager::Internal::SetFileLogPath(logfile);
					s_settings_interface.SetBoolValue("Logging", "EnableFileLogging", true);
					s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", false);
				}

				continue;
			}
			else if (CHECK_ARG("-noshadercache"))
			{
				Console.WriteLn("Disabling shader cache");
				s_settings_interface.SetBoolValue("EmuCore/GS", "DisableShaderCache", true);
				continue;
			}
			else if (CHECK_ARG("-window"))
			{
				Console.WriteLn("Creating window");
				s_use_window = true;
				continue;
			}
			else if (CHECK_ARG("-surfaceless"))
			{
				Console.WriteLn("Running surfaceless");
				s_use_window = false;
				continue;
			}
			else if (CHECK_ARG("-perf"))
			{
				Console.WriteLn("Enable performance stats");
				s_perf_enable = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("-drawlog"))
			{
				s_drawlog_path = argv[++i];
				s_settings_interface.SetBoolValue("EmuCore/GS", "DumpDrawLog", true);
				Console.WriteLn(fmt::format("Recording per-draw ledger to {}", s_drawlog_path));
				continue;
			}
			else if (CHECK_ARG_PARAM("-fedump"))
			{
				s_fedump_path = argv[++i];
				Console.WriteLn(fmt::format("Recording the front-end decode surface to {}", s_fedump_path));
				continue;
			}
			else if (CHECK_ARG_PARAM("-fediff"))
			{
				s_fediff_path = argv[++i];
				Console.WriteLn(fmt::format("Diffing the front-end decode surface against {}", s_fediff_path));
				continue;
			}
			else if (CHECK_ARG_PARAM("-gspin"))
			{
				const std::string cpus(StringUtil::StripWhitespace(argv[++i]));
				if (!ParseCpuList(cpus, &s_gs_pin_mask))
				{
					ArgError("-gspin: '{}' is not a CPU list (expected e.g. 4 or 0,1,2,3).", cpus);
					return false;
				}
				s_gs_pin_request = cpus;
				Console.WriteLn(fmt::format("Pinning the GS thread to CPU(s) {}", s_gs_pin_request));
				continue;
			}
			else if (CHECK_ARG_PARAM("-affinity"))
			{
				const char* mode_arg = argv[++i];
				const std::optional<int> mode = GSRunnerAffinity::ParseMode(mode_arg);
				if (!mode.has_value())
				{
					ArgError("-affinity: invalid mode '{}' -- expected an integer {}-{} (0 unpinned, 1-6 explicit "
							 "per-core placements, 7 Performance Cores).",
						mode_arg, GSRunnerAffinity::MODE_MIN, GSRunnerAffinity::MODE_MAX);
					return false;
				}
				s_affinity_mode = mode.value();
				s_affinity_source = "flag";
				continue;
			}
			else if (CHECK_ARG_PARAM("-stats-json"))
			{
				s_stats_json_path = argv[++i];
				Console.WriteLn(fmt::format("Writing per-frame stats to {}", s_stats_json_path));
				continue;
			}
			else if (CHECK_ARG_PARAM("-set"))
			{
				// Generic settings override: -set <Section/Key>=<value>. Retires the need
				// for a bespoke flag per experiment and makes a sweep driver trivial.
				const std::string_view arg(argv[++i]);
				const std::string_view::size_type eq = arg.find('=');
				const std::string_view::size_type slash = arg.rfind('/', eq);
				if (eq == std::string_view::npos || slash == std::string_view::npos || slash == 0)
				{
					ArgError("-set: malformed override '{}', expected <Section/Key>=<value>.", arg);
					return false;
				}

				const std::string section(arg.substr(0, slash));
				const std::string key(arg.substr(slash + 1, eq - slash - 1));
				const std::string value(arg.substr(eq + 1));
				if (key.empty())
				{
					ArgError("-set: malformed override '{}', the key is empty.", arg);
					return false;
				}

				// Stored as a string; SettingsWrapper coerces on read, so this works for
				// bool/int/float keys alike.
				s_settings_interface.SetStringValue(section.c_str(), key.c_str(), value.c_str());
				Console.WriteLn(fmt::format("Override: [{}] {} = {}", section, key, value));
				continue;
			}
			else if (CHECK_ARG("-vsync"))
			{
				Console.WriteLn("Forcing vsync on (FIFO present mode). Use on libmali Wayland where MAILBOX errors VK_ERROR_INITIALIZATION_FAILED.");
				s_force_vsync = true;
				continue;
			}
			else if (CHECK_ARG("-no-fb-fetch"))
			{
				Console.WriteLn("Disabling framebuffer fetch (VK_EXT_rasterization_order_attachment_access)");
				s_settings_interface.SetBoolValue("EmuCore/GS", "DisableFramebufferFetch", true);
				continue;
			}
			else if (CHECK_ARG("-no-dual-source"))
			{
				Console.WriteLn("Disabling dual-source blending (pretend to be a Mali blob)");
				s_settings_interface.SetBoolValue("EmuCore/GS", "DisableDualSourceBlend", true);
				continue;
			}
			else if (CHECK_ARG("-broken-blend-constant"))
			{
				Console.WriteLn("Pretending the driver ignores the blend constant (pretend to be Turnip)");
				// Not a setting: the driver-bug database is keyed on device identity, and this
				// forces one of its bits on for a device that does not have it. Read when the
				// device resolves its profile, which happens after argument parsing.
				GpuProfileDetector::SetForcedBugs(GpuProfileDetector::GetForcedBugs() |
												  GpuProfileDetector::BugMask(DriverBug::BrokenBlendConstant));
				continue;
			}
			else if (CHECK_ARG("-no-vs-expand"))
			{
				Console.WriteLn("Disabling vertex-shader point/line/sprite expansion");
				s_settings_interface.SetBoolValue("EmuCore/GS", "DisableVertexShaderExpand", true);
				continue;
			}
			else if (CHECK_ARG("-no-tex-barriers"))
			{
				Console.WriteLn("Forcing texture barriers off (OverrideTextureBarriers=0)");
				s_settings_interface.SetIntValue("EmuCore/GS", "OverrideTextureBarriers", 0);
				continue;
			}
			else if (CHECK_ARG_PARAM("-accblend"))
			{
				const char* level_arg = argv[++i];
				const std::optional<int> level = ParseNumericArg<int>("-accblend", level_arg);
				if (!level.has_value())
					return false;
				if (level.value() < 0 || level.value() > 5)
				{
					ArgError("-accblend: level '{}' is out of range (expected 0=Minimum .. 5=Maximum).", level_arg);
					return false;
				}
				Console.WriteLn(fmt::format("Forcing accurate blending unit = {}", level.value()));
				s_settings_interface.SetIntValue("EmuCore/GS", "accurate_blending_unit", level.value());
				continue;
			}
			else if (CHECK_ARG_PARAM("-emit-payload"))
			{
				s_payload_opts.output_path = StringUtil::StripWhitespace(argv[++i]);
				if (s_payload_opts.output_path.empty())
				{
					ArgError("-emit-payload: the output path is empty.");
					return false;
				}
				s_emit_payload = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("-payload-frames"))
			{
				const std::optional<u32> frames = ParseNumericArg<u32>("-payload-frames", argv[++i]);
				if (!frames.has_value())
					return false;
				s_payload_opts.frame_limit = frames.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("-payload-readback"))
			{
				// bp,bw,psm,w,h -- the region every checkpoint reads back. Left alone it
				// comes from the freeze's context-0 FRAME, which is right for most dumps
				// and wrong for any that render somewhere other than where they display.
				// Five fields is the whole target; seven adds an origin and is the same
				// shape as -ladder, so a window can be copied between the two arms
				// verbatim rather than re-typed in a different order.
				const std::vector<std::string_view> parts = StringUtil::SplitString(argv[++i], ',', true);
				if (parts.size() != 5 && parts.size() != 7)
				{
					ArgError("-payload-readback: got {} fields, wants bp,bw,psm,w,h or bp,bw,psm,x,y,w,h.",
						parts.size());
					return false;
				}
				std::vector<u32> rb;
				rb.reserve(parts.size());
				for (const std::string_view& part : parts)
				{
					const std::optional<u32> v = ParseNumericArg<u32>("-payload-readback", part);
					if (!v.has_value())
						return false;
					rb.push_back(v.value());
				}
				s_payload_opts.rb_bp = rb[0];
				s_payload_opts.rb_bw = rb[1];
				s_payload_opts.rb_psm = rb[2];
				if (rb.size() == 7)
				{
					s_payload_opts.rb_x = rb[3];
					s_payload_opts.rb_y = rb[4];
				}
				s_payload_opts.rb_w = rb[rb.size() - 2];
				s_payload_opts.rb_h = rb[rb.size() - 1];
				s_payload_opts.rb_explicit = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("-payload-ladder"))
			{
				const std::optional<u32> every = ParseNumericArg<u32>("-payload-ladder", argv[++i]);
				if (!every.has_value())
					return false;
				s_payload_opts.ladder_every = every.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("-ladder"))
			{
				// bp,bw,psm,x,y,w,h -- the window read back at every rung. Small on
				// purpose: a rung is only useful if hundreds of them fit, and the
				// console's buffer is the binding constraint on both arms.
				const std::vector<std::string_view> parts = StringUtil::SplitString(argv[++i], ',', true);
				if (parts.size() != 7)
				{
					ArgError("-ladder: got {} fields, wants bp,bw,psm,x,y,w,h.", parts.size());
					return false;
				}
				u32 rung[7] = {};
				for (size_t p = 0; p < parts.size(); p++)
				{
					const std::optional<u32> v = ParseNumericArg<u32>("-ladder", parts[p]);
					if (!v.has_value())
						return false;
					rung[p] = v.value();
				}
				s_ladder_opts.bp = rung[0];
				s_ladder_opts.bw = rung[1];
				s_ladder_opts.psm = rung[2];
				s_ladder_opts.x = rung[3];
				s_ladder_opts.y = rung[4];
				s_ladder_opts.w = rung[5];
				s_ladder_opts.h = rung[6];
				continue;
			}
			else if (CHECK_ARG_PARAM("-ladder-every"))
			{
				const std::optional<u32> every = ParseNumericArg<u32>("-ladder-every", argv[++i]);
				if (!every.has_value())
					return false;
				s_ladder_opts.every = every.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("-ladder-out"))
			{
				s_ladder_opts.output_path = StringUtil::StripWhitespace(argv[++i]);
				if (s_ladder_opts.output_path.empty())
				{
					ArgError("-ladder-out: the output path is empty.");
					return false;
				}
				continue;
			}
			else if (CHECK_ARG("-debugdevice"))
			{
				Console.WriteLn("Enable debug device");
				s_settings_interface.SetBoolValue("EmuCore/GS", "UseDebugDevice", true);
				continue;
			}
			else if (CHECK_ARG("--"))
			{
				no_more_args = true;
				continue;
			}
			else if (missing_param)
			{
				ArgError("{}: needs a parameter, and it was the last argument on the command line.", missing_param);
				return false;
			}
			else if (argv[i][0] == '-')
			{
				ArgError("unknown argument '{}'.", argv[i]);
				return false;
			}

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
		}

		if (!params.filename.empty())
			params.filename += ' ';
		params.filename += argv[i];
	}

	if (params.filename.empty())
	{
		ArgError("no GS dump filename was given.");
		return false;
	}

	if (!VMManager::IsGSDumpFileName(params.filename))
	{
		ArgError("'{}' is not a GS dump (expected .gs, .gs.xz or .gs.zst).", params.filename);
		return false;
	}

	if (s_settings_interface.GetBoolValue("EmuCore/GS", "DumpGSData") && !dumpdir.empty())
	{
		if (s_settings_interface.GetStringValue("EmuCore/GS", "HWDumpDirectory").empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "HWDumpDirectory", dumpdir.c_str());
		if (s_settings_interface.GetStringValue("EmuCore/GS", "SWDumpDirectory").empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "SWDumpDirectory", dumpdir.c_str());
		
		// Disable saving frames with SaveSnapshotToMemory()
		// Instead we save more "raw" snapshots when using -dump.
		s_output_prefix = "";
	}

	// set up the frame dump directory
	if (!s_output_prefix.empty())
	{
		// strip off all extensions
		std::string_view title(Path::GetFileTitle(params.filename));
		if (StringUtil::EndsWithNoCase(title, ".gs"))
			title = Path::GetFileTitle(title);

		s_output_prefix = Path::Combine(s_output_prefix, StringUtil::StripWhitespace(title));
		Console.WriteLn(fmt::format("Saving dumps as {}_frameN.png", s_output_prefix));
	}

	return true;
}

void GSRunner::SettingsOverride()
{
	// complete as quickly as possible
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", s_force_vsync);
	s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", s_force_vsync);
	// -vsync needs DisableMailboxPresentation too: GetEffectiveVSyncMode() returns
	// Mailbox when VsyncEnable=true unless this is set.
	if (s_force_vsync)
		s_settings_interface.SetBoolValue("EmuCore/GS", "DisableMailboxPresentation", true);

	// Force screenshot quality settings to something more performant, overriding any defaults good for users.
	s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotQuality", 10);

	// ensure all input sources are disabled, we're not using them
	s_settings_interface.SetBoolValue("InputSources", "SDL", false);
	s_settings_interface.SetBoolValue("InputSources", "XInput", false);

	// we don't need any sound output
	s_settings_interface.SetStringValue("SPU2/Output", "OutputModule", "nullout");

	// none of the bindings are going to resolve to anything
	Pad::ClearPortBindings(s_settings_interface, 0);
	s_settings_interface.ClearSection("Hotkeys");

	// force logging
	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", !s_no_console);
	s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", true);
	s_settings_interface.SetBoolValue("Logging", "EnableVerbose", true);

	// and show some stats :)
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowFPS", true);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowResolution", true);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowGSStats", true);

	// remove memory cards, so we don't have sharing violations
	for (u32 i = 0; i < 2; i++)
	{
		s_settings_interface.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
		s_settings_interface.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
	}
}

static double Ratio(u64 num, u64 den)
{
	return den ? (100.0 * static_cast<double>(num) / static_cast<double>(den)) : 0.0;
}

// Nearest-rank percentile over an already-sorted vector.
static float Percentile(const std::vector<float>& sorted, double p)
{
	if (sorted.empty())
		return 0.0f;

	const size_t idx = std::min(sorted.size() - 1,
		static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size())) - 1.0));
	return sorted[idx];
}

// Writes the per-frame series plus a run summary. Emitted by hand rather than via a
// JSON library because gsrunner links none, and the schema is fixed.
static void WriteStatsJson(const std::string& path)
{
	auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb");
	if (!fp)
	{
		Console.Error(fmt::format("Failed to open '{}' for writing stats", path));
		return;
	}

	// Percentiles are computed over drawn frames only; idle frames are present-only
	// and would drag the distribution toward zero.
	std::vector<float> frame_times;
	frame_times.reserve(s_frame_samples.size());
	for (const FrameSample& s : s_frame_samples)
	{
		if (!s.idle && s.frame_ms > 0.0f)
			frame_times.push_back(s.frame_ms);
	}
	std::sort(frame_times.begin(), frame_times.end());

	// The absolute per-draw CPU budget: GS-thread CPU per PS2 draw, over drawn frames.
	// Summed before dividing rather than averaged per frame, so a frame with three
	// draws does not weigh the same as one with a thousand. Two denominators because
	// they answer different questions -- per PS2 draw is the number a renderer is held
	// to (both variants see the same draws), per draw call is what the backend was
	// actually asked to submit. The first frame's sample is zero by construction and
	// is skipped with the idle ones.
	double gs_cpu_ms_total = 0.0;
	u64 gs_cpu_draws = 0, gs_cpu_draw_calls = 0;
	std::vector<float> gs_cpu_per_draw_us;
	gs_cpu_per_draw_us.reserve(s_frame_samples.size());
	for (const FrameSample& s : s_frame_samples)
	{
		if (s.idle || s.gs_cpu_ms <= 0.0f || s.draws == 0)
			continue;
		gs_cpu_ms_total += s.gs_cpu_ms;
		gs_cpu_draws += s.draws;
		gs_cpu_draw_calls += s.draw_calls;
		gs_cpu_per_draw_us.push_back(static_cast<float>(s.gs_cpu_ms * 1000.0 / static_cast<double>(s.draws)));
	}
	std::sort(gs_cpu_per_draw_us.begin(), gs_cpu_per_draw_us.end());

	// Where the frame went that nothing accounted for. A frame is wall clock; of that, gs_cpu_ms is
	// what the GS thread executed and the fence bills are what it spent blocked. What is left over
	// is time the runner cannot name, and on a GPU-synchronous title that residual is the whole
	// question -- ac3 on Mali showed 2.5 ms a frame with every counter, every frame hash and every
	// tracked wait site identical between two builds.
	//
	// ⚠️ Only the two FENCE bills are subtracted. sync and ring are blocked time, so they are
	// disjoint from thread CPU by construction and the remainder cannot go negative. The submit
	// and map bills are not: measured on the M2, stuntman spends 235 ms inside vkQueueSubmit
	// across ten submits and its gs_cpu_ms is within a millisecond of its frame_ms, so that time
	// is CPU the thread burned rather than time it waited. Subtracting it as well took the same
	// four frames to a p50 of -70 ms, which is not a number a report can show.
	//
	// So submit and map are reported BESIDE this figure rather than inside it. Where a submit is
	// genuinely off-CPU -- which is the ac3-on-Mali hypothesis -- its nanoseconds are absent from
	// gs_cpu_ms and therefore land here, and submit_wait_ns per frame is what says whether the
	// residual is submit-shaped.
	std::vector<float> untracked_ms;
	double untracked_ms_total = 0.0;
	untracked_ms.reserve(s_frame_samples.size());
	for (const FrameSample& s : s_frame_samples)
	{
		if (s.idle || s.frame_ms <= 0.0f)
			continue;
		const double blocked_ms = static_cast<double>(s.sync_wait_ns + s.ring_wait_ns) / 1e6;
		const double residual = static_cast<double>(s.frame_ms) - static_cast<double>(s.gs_cpu_ms) - blocked_ms;
		untracked_ms.push_back(static_cast<float>(residual));
		untracked_ms_total += residual;
	}
	std::sort(untracked_ms.begin(), untracked_ms.end());

	const double gs_cpu_us_per_draw = gs_cpu_draws ? (gs_cpu_ms_total * 1000.0 / static_cast<double>(gs_cpu_draws)) : 0.0;
	const double gs_cpu_us_per_draw_call = gs_cpu_draw_calls ? (gs_cpu_ms_total * 1000.0 / static_cast<double>(gs_cpu_draw_calls)) : 0.0;

	// Resident set size across the run. first-to-last is the leak test; max is there
	// because a transient peak sits between the two endpoints and neither one sees it.
	// The baseline is the first frame that actually drew -- the frames before it are
	// still paging in shaders and textures, so they would understate the starting point
	// and turn ordinary warm-up into a reported leak.
	u64 rss_kb_first = 0, rss_kb_last = 0, rss_kb_max = 0;
	for (const FrameSample& s : s_frame_samples)
	{
		if (s.rss_kb == 0)
			continue;
		if (rss_kb_first == 0 && !s.idle)
			rss_kb_first = s.rss_kb;
		rss_kb_last = s.rss_kb;
		rss_kb_max = std::max(rss_kb_max, s.rss_kb);
	}

	u32 worst_frame = 0;
	float worst_ms = 0.0f;
	for (const FrameSample& s : s_frame_samples)
	{
		if (!s.idle && s.frame_ms > worst_ms)
		{
			worst_ms = s.frame_ms;
			worst_frame = s.frame;
		}
	}

	// GetDriverInfo() is multi-line on Vulkan, and neither string is JSON-safe as-is.
	const auto json_escape = [](const std::string& in) {
		std::string out;
		out.reserve(in.size());
		for (const char c : in)
		{
			if (c == '\n' || c == '\r' || c == '\t')
				out.push_back(' ');
			else if (c == '"' || c == '\\')
				out.push_back('\'');
			else
				out.push_back(c);
		}
		return out;
	};

	std::fprintf(fp.get(), "{\n  \"run\": {\n");
	std::fprintf(fp.get(), "    \"device_name\": \"%s\",\n    \"driver_info\": \"%s\",\n",
		json_escape(s_device_name).c_str(), json_escape(s_driver_info).c_str());
	std::fprintf(fp.get(), "    \"stream_ring_memory\": \"%s\",\n",
		json_escape(s_stream_ring_memory.empty() ? std::string("n/a") : s_stream_ring_memory).c_str());
	// What the non-coherent road cost, and what it would have cost per commit. Zero on every
	// coherent ring, which is every device that does not take that road -- so a non-zero
	// stream_ring_flush_commits with a far smaller stream_ring_flushes is the deferral working,
	// and equal numbers would be the old per-commit behaviour.
	std::fprintf(fp.get(),
		"    \"stream_ring_flushes\": %" PRIu64 ",\n    \"stream_ring_flush_commits\": %" PRIu64 ",\n"
		"    \"stream_ring_flush_bytes\": %" PRIu64 ",\n",
		s_stream_ring_flush.calls, s_stream_ring_flush.commits, s_stream_ring_flush.bytes);
	std::fprintf(fp.get(), "    \"frames\": %u,\n    \"drawn_frames\": %u,\n", s_total_frames, s_total_drawn_frames);
	// What the run was asked to replay, so a reader can cut the frame series into loops.
	// loop_count is the -loop value verbatim (1 when the flag was absent, 0 meaning
	// "until stopped"); frames_per_loop is the dump's own frame count. Without the pair,
	// the first loop -- shader compiles, cold caches, first upload of every texture --
	// sits in the same percentile as steady state and there is no way to take it out.
	std::fprintf(fp.get(), "    \"loop_count\": %d,\n    \"frames_per_loop\": %u,\n",
		s_loop_count, s_dump_frames_per_loop);
	std::fprintf(fp.get(), "    \"prims\": %" PRIu64 ",\n    \"draws\": %" PRIu64 ",\n    \"draw_calls\": %" PRIu64 ",\n",
		s_total_prims, s_total_internal_draws, s_total_draws);
	std::fprintf(fp.get(), "    \"render_passes\": %" PRIu64 ",\n    \"barriers\": %" PRIu64 ",\n", s_total_render_passes, s_total_barriers);
	std::fprintf(fp.get(), "    \"render_pass_area_pixels\": %" PRIu64 ",\n", s_total_render_pass_area_pixels);
	std::fprintf(fp.get(), "    \"copies\": %" PRIu64 ",\n    \"uploads\": %" PRIu64 ",\n    \"readbacks\": %" PRIu64 ",\n",
		s_total_copies, s_total_uploads, s_total_readbacks);
	std::fprintf(fp.get(), "    \"copies_rov\": %" PRIu64 ",\n    \"draw_calls_rov\": %" PRIu64 ",\n    \"barriers_rov\": %" PRIu64 ",\n",
		s_total_copies_rov, s_total_draws_rov, s_total_barriers_rov);
	std::fprintf(fp.get(), "    \"tc_source_hit\": %" PRIu64 ",\n    \"tc_source_miss\": %" PRIu64 ",\n",
		s_total_tc_source_hit, s_total_tc_source_miss);
	std::fprintf(fp.get(), "    \"tc_target_hit\": %" PRIu64 ",\n    \"tc_target_miss\": %" PRIu64 ",\n",
		s_total_tc_target_hit, s_total_tc_target_miss);
	std::fprintf(fp.get(), "    \"hash_cache_hit\": %" PRIu64 ",\n    \"hash_cache_miss\": %" PRIu64 ",\n",
		s_total_hash_cache_hit, s_total_hash_cache_miss);
	std::fprintf(fp.get(), "    \"pipeline_switches\": %" PRIu64 ",\n", s_total_pipeline_switches);
	// Every path is emitted, zeros included, so a reader diffing two runs never has to decide
	// whether a missing key means the path moved nothing or means the instrument was absent.
	std::fprintf(fp.get(), "    \"bytes\": {");
	for (size_t bc = 0; bc < NUM_BYTE_COUNTERS; bc++)
	{
		std::fprintf(fp.get(), "%s\n      \"%s\": {\"class\": \"%s\", \"total\": %" PRIu64 "}",
			(bc > 0) ? "," : "", s_byte_counters[bc].name, s_byte_counters[bc].cls, s_total_bytes[bc]);
	}
	std::fprintf(fp.get(), "\n    },\n");
	std::fprintf(fp.get(), "    \"gpu_blocking_waits\": %" PRIu64 ",\n", s_total_gpu_blocking_waits);
	// The same population, split by cause, so an attribution round needs no teardown-ordering print
	// to survive. Wall time in nanoseconds because that is the unit the device counts in; a reader
	// that wants milliseconds can divide, and one that wants to sum two causes cannot un-round.
	std::fprintf(fp.get(), "    \"sync_wait_ns\": %" PRIu64 ",\n    \"sync_wait_calls\": %" PRIu64 ",\n",
		s_device_wait_bill.sync_ns, s_device_wait_bill.sync_calls);
	std::fprintf(fp.get(), "    \"ring_wait_ns\": %" PRIu64 ",\n    \"ring_wait_calls\": %" PRIu64 ",\n",
		s_device_wait_bill.ring_ns, s_device_wait_bill.ring_calls);
	// The two bills that are not fence waits. submit_wait_calls is also the run's submit count.
	std::fprintf(fp.get(), "    \"submit_wait_ns\": %" PRIu64 ",\n    \"submit_wait_calls\": %" PRIu64 ",\n",
		s_device_wait_bill.submit_ns, s_device_wait_bill.submit_calls);
	std::fprintf(fp.get(), "    \"map_wait_ns\": %" PRIu64 ",\n    \"map_wait_calls\": %" PRIu64 ",\n",
		s_device_wait_bill.map_ns, s_device_wait_bill.map_calls);
	// The same population a THIRD way: by the call site that paid, which is the only cut that names
	// a mechanism. Every site is emitted, zeros included, so a reader diffing two runs never has to
	// decide whether a missing key means zero or means the instrument was absent. The families sum
	// to the two figures above exactly.
	std::fprintf(fp.get(), "    \"wait_sites\": {");
	for (u32 i = 0; i < s_device_wait_bill.site_count; i++)
	{
		std::fprintf(fp.get(), "%s\n      \"%s\": {\"family\": \"%s\", \"ns\": %" PRIu64 ", \"calls\": %" PRIu64 "}",
			(i > 0) ? "," : "", s_device_wait_bill.site_names[i], s_device_wait_bill.site_families[i],
			s_device_wait_bill.site_ns[i], s_device_wait_bill.site_calls[i]);
	}
	std::fprintf(fp.get(), "%s},\n", s_device_wait_bill.site_count ? "\n    " : "");
	std::fprintf(fp.get(), "    \"gs_cpu_ms\": %.3f,\n    \"gs_cpu_us_per_draw\": %.3f,\n    \"gs_cpu_us_per_draw_call\": %.3f,\n",
		gs_cpu_ms_total, gs_cpu_us_per_draw, gs_cpu_us_per_draw_call);
	std::fprintf(fp.get(), "    \"gs_cpu_us_per_draw_p50\": %.3f,\n    \"gs_cpu_us_per_draw_p95\": %.3f,\n",
		Percentile(gs_cpu_per_draw_us, 0.50), Percentile(gs_cpu_per_draw_us, 0.95));
	std::fprintf(fp.get(), "    \"gs_cpu_partial\": %s,\n", s_saw_gs_back_thread_in_stats ? "true" : "false");
	// Frame time nothing accounted for: wall clock less GS-thread CPU less the two fence bills, over
	// drawn frames. The submit and map bills are deliberately not subtracted -- see where it is
	// computed -- so submit_wait_ns is the thing to read next to this, not something already in it.
	std::fprintf(fp.get(), "    \"untracked_ms_p50\": %.3f,\n    \"untracked_ms_p95\": %.3f,\n    \"untracked_ms_total\": %.3f,\n",
		Percentile(untracked_ms, 0.50), Percentile(untracked_ms, 0.95), untracked_ms_total);
	// Where the GS thread was asked to sit, where it actually sat, and who put it there.
	// requested is "none" when the flag was absent; effective is always the read-back, and
	// is "unsupported" only when the platform cannot be asked.
	std::fprintf(fp.get(), "    \"gs_pin_requested\": \"%s\",\n    \"gs_pin_effective\": \"%s\",\n",
		s_gs_pin_request.empty() ? "none" : json_escape(s_gs_pin_request).c_str(), json_escape(s_gs_pin_effective).c_str());
	std::fprintf(fp.get(), "    \"gs_pin_source\": \"%s\",\n", s_gs_pin_source);
	// The thread-placement mode VMManager ran under, who chose it, and the CPU set this
	// process inherited before anything narrowed it. affinity_mode is -1 with source
	// "unsupported" on a build with no affinity path. inherited_cpu_mask is a hex mask
	// because that is how taskset's argument is written, and it is 0x0 where the platform
	// does not answer the question at all.
	std::fprintf(fp.get(), "    \"affinity_mode\": %d,\n    \"affinity_source\": \"%s\",\n", s_affinity_mode,
		s_affinity_source);
	std::fprintf(fp.get(), "    \"inherited_cpu_mask\": \"0x%" PRIx64 "\",\n", s_baseline_cpu_mask);
	std::fprintf(fp.get(), "    \"rss_kb_first\": %" PRIu64 ",\n    \"rss_kb_last\": %" PRIu64 ",\n    \"rss_kb_max\": %" PRIu64 ",\n",
		rss_kb_first, rss_kb_last, rss_kb_max);
	std::fprintf(fp.get(), "    \"frame_ms_p50\": %.3f,\n    \"frame_ms_p95\": %.3f,\n    \"frame_ms_p99\": %.3f,\n",
		Percentile(frame_times, 0.50), Percentile(frame_times, 0.95), Percentile(frame_times, 0.99));
	std::fprintf(fp.get(), "    \"frame_ms_worst\": %.3f,\n    \"frame_worst_index\": %u\n  },\n", worst_ms, worst_frame);

	std::fprintf(fp.get(), "  \"frames\": [\n");
	for (size_t i = 0; i < s_frame_samples.size(); i++)
	{
		const FrameSample& s = s_frame_samples[i];
		std::fprintf(fp.get(),
			"    {\"frame\":%u,\"frame_in_dump\":%u,\"idle\":%s,\"frame_ms\":%.3f,\"gpu_ms\":%.3f,\"gs_cpu_ms\":%.3f,"
			"\"prims\":%" PRIu64 ",\"draws\":%" PRIu64 ",\"draw_calls\":%" PRIu64 ","
			"\"render_passes\":%" PRIu64 ",\"render_pass_area_pixels\":%" PRIu64 ","
			"\"barriers\":%" PRIu64 ",\"copies\":%" PRIu64 ","
			"\"uploads\":%" PRIu64 ",\"readbacks\":%" PRIu64 ","
			"\"copies_rov\":%" PRIu64 ",\"draw_calls_rov\":%" PRIu64 ",\"barriers_rov\":%" PRIu64 ","
			"\"tc_source_hit\":%" PRIu64 ",\"tc_source_miss\":%" PRIu64 ","
			"\"tc_target_hit\":%" PRIu64 ",\"tc_target_miss\":%" PRIu64 ","
			"\"hash_cache_hit\":%" PRIu64 ",\"hash_cache_miss\":%" PRIu64 ","
			"\"pipeline_switches\":%" PRIu64 ",\"gpu_blocking_waits\":%" PRIu64 ","
			"\"sync_wait_ns\":%" PRIu64 ",\"ring_wait_ns\":%" PRIu64 ","
			"\"submit_wait_ns\":%" PRIu64 ",\"map_wait_ns\":%" PRIu64 ",\"submits\":%" PRIu64 ","
			"\"rss_kb\":%" PRIu64 ",\"minflt_delta\":%" PRIu64 "}%s\n",
			s.frame, s.frame_in_dump, s.idle ? "true" : "false", s.frame_ms, s.gpu_ms, s.gs_cpu_ms,
			s.prims, s.draws, s.draw_calls,
			s.render_passes, s.render_pass_area_pixels, s.barriers, s.copies,
			s.uploads, s.readbacks,
			s.copies_rov, s.draw_calls_rov, s.barriers_rov,
			s.tc_source_hit, s.tc_source_miss,
			s.tc_target_hit, s.tc_target_miss,
			s.hash_cache_hit, s.hash_cache_miss,
			s.pipeline_switches, s.gpu_blocking_waits,
			s.sync_wait_ns, s.ring_wait_ns, s.submit_wait_ns, s.map_wait_ns, s.submits,
			s.rss_kb, s.minflt_delta,
			(i + 1 < s_frame_samples.size()) ? "," : "");
	}
	std::fprintf(fp.get(), "  ]\n}\n");

	Console.WriteLn(fmt::format("Wrote {} frame samples to {}", s_frame_samples.size(), path));
}

void GSRunner::DumpStats()
{
	std::atomic_thread_fence(std::memory_order_acquire);
	Console.WriteLn(fmt::format("======= HW STATISTICS FOR {} ({}) FRAMES ========", s_total_frames, s_total_drawn_frames));
	Console.WriteLn(fmt::format("@HWSTAT@ Prims: {} (avg {})", s_total_prims, static_cast<u64>(std::ceil(s_total_prims / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Draws: {} (avg {})", s_total_internal_draws, static_cast<u64>(std::ceil(s_total_internal_draws / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Draw Calls: {} (avg {})", s_total_draws, static_cast<u64>(std::ceil(s_total_draws / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Render Passes: {} (avg {})", s_total_render_passes, static_cast<u64>(std::ceil(s_total_render_passes / static_cast<double>(s_total_drawn_frames)))));
	// The same passes weighed rather than counted: megapixels of renderArea a drawn frame loads and
	// stores, which is what a pass costs on a tiler.
	Console.WriteLn(fmt::format("@HWSTAT@ Render Pass Area Mpx: {:.2f} (avg {:.2f}/frame)",
		s_total_render_pass_area_pixels / 1e6,
		s_total_render_pass_area_pixels / 1e6 / static_cast<double>(s_total_drawn_frames)));
	Console.WriteLn(fmt::format("@HWSTAT@ Pipeline Switches: {} (avg {})", s_total_pipeline_switches, static_cast<u64>(std::ceil(s_total_pipeline_switches / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers: {} (avg {})", s_total_barriers, static_cast<u64>(std::ceil(s_total_barriers / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies: {} (avg {})", s_total_copies, static_cast<u64>(std::ceil(s_total_copies / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Uploads: {} (avg {})", s_total_uploads, static_cast<u64>(std::ceil(s_total_uploads / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Readbacks: {} (avg {})", s_total_readbacks, static_cast<u64>(std::ceil(s_total_readbacks / static_cast<double>(s_total_drawn_frames)))));
	// Not a duplicate of Readbacks: that counts copies that reach the device, this counts the times
	// the GS thread BLOCKED for one. Zero is the target; any nonzero value costs the frame
	// min(cpu, gpu) whatever the magnitude.
	Console.WriteLn(fmt::format("@HWSTAT@ GPU Blocking Waits: {} (avg {:.2f}/frame)", s_total_gpu_blocking_waits,
		s_total_gpu_blocking_waits / static_cast<double>(s_total_drawn_frames)));
	// Unconditional, off the latch: the device is gone by now on some builds, and a split that
	// disappears exactly where the waits are is worse than no split at all.
	{
		const DeviceWaitBill& w = s_device_wait_bill;
		Console.WriteLn(fmt::format("@HWSTAT@ GPU Blocking Wait ms: {:.3f} over {} waits; "
									"ring backpressure {:.3f} ms over {} waits",
			w.sync_ns / 1e6, w.sync_calls, w.ring_ns / 1e6, w.ring_calls));
		// The two bills that are not fence waits, on their own line so they cannot be read as part
		// of the blocking-wait population above. The submit count is the run's command-buffer
		// count, which is the multiplier on every submit-side cost.
		Console.WriteLn(fmt::format("@HWSTAT@ Queue Submit ms: {:.3f} over {} submits; "
									"readback map {:.3f} ms over {} invalidates",
			w.submit_ns / 1e6, w.submit_calls, w.map_ns / 1e6, w.map_calls));
		// ...and the same bill by SITE. Nonzero sites only, with each family's sum appended so the
		// reconciliation against the line above is a read rather than an arithmetic exercise. A run
		// that blocked nowhere prints "none", which is a reading and not a blank.
		std::string sites;
		u64 sum_ns[4] = {}, sum_calls[4] = {};
		static constexpr const char* kFamilies[4] = {"sync", "ring", "submit", "map"};
		for (u32 i = 0; i < w.site_count; i++)
		{
			for (u32 f = 0; f < 4; f++)
			{
				if (std::strcmp(w.site_families[i], kFamilies[f]) != 0)
					continue;
				sum_ns[f] += w.site_ns[i];
				sum_calls[f] += w.site_calls[i];
				break;
			}
			if (w.site_calls[i] == 0)
				continue;
			sites += fmt::format("  {}[{}] {} @ {:.3f} ms", w.site_names[i], w.site_families[i], w.site_calls[i],
				w.site_ns[i] / 1e6);
		}
		Console.WriteLn(fmt::format("@HWSTAT@ GPU Wait Sites:{}   [sums: sync {:.3f} ms over {}; "
									"ring {:.3f} ms over {}; submit {:.3f} ms over {}; map {:.3f} ms over {}]",
			sites.empty() ? std::string("  none") : sites, sum_ns[0] / 1e6, sum_calls[0], sum_ns[1] / 1e6,
			sum_calls[1], sum_ns[2] / 1e6, sum_calls[2], sum_ns[3] / 1e6, sum_calls[3]));
	}
	Console.WriteLn(fmt::format("@HWSTAT@ Copies (ROV): {} (avg {})", s_total_copies_rov, static_cast<u64>(std::ceil(s_total_copies_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Draws Calls (ROV): {} (avg {})", s_total_draws_rov, static_cast<u64>(std::ceil(s_total_draws_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers (ROV): {} (avg {})", s_total_barriers_rov, static_cast<u64>(std::ceil(s_total_barriers_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ TC Source Hit/Miss: {}/{} ({:.1f}% hit)", s_total_tc_source_hit, s_total_tc_source_miss,
		Ratio(s_total_tc_source_hit, s_total_tc_source_hit + s_total_tc_source_miss)));
	Console.WriteLn(fmt::format("@HWSTAT@ TC Target Hit/Miss: {}/{} ({:.1f}% hit)", s_total_tc_target_hit, s_total_tc_target_miss,
		Ratio(s_total_tc_target_hit, s_total_tc_target_hit + s_total_tc_target_miss)));
	Console.WriteLn(fmt::format("@HWSTAT@ Hash Cache Hit/Miss: {}/{} ({:.1f}% hit)", s_total_hash_cache_hit, s_total_hash_cache_miss,
		Ratio(s_total_hash_cache_hit, s_total_hash_cache_hit + s_total_hash_cache_miss)));
	{
		const double frames = std::max(1.0, static_cast<double>(s_total_drawn_frames));
		double per_class[3] = {};
		for (size_t bc = 0; bc < NUM_BYTE_COUNTERS; bc++)
		{
			const double per_frame = static_cast<double>(s_total_bytes[bc]) / frames;
			Console.WriteLn(fmt::format("@HWSTAT@ Bytes {} [{}]: {} total, {:.1f} KiB/frame",
				s_byte_counters[bc].name, s_byte_counters[bc].cls, s_total_bytes[bc], per_frame / 1024.0));
			const std::string_view cls(s_byte_counters[bc].cls);
			per_class[(cls == "rw") ? 0 : ((cls == "w") ? 1 : 2)] += per_frame;
		}
		Console.WriteLn(fmt::format("@HWSTAT@ Bytes by class KiB/frame: rw {:.1f}, w {:.1f}, r {:.1f}",
			per_class[0] / 1024.0, per_class[1] / 1024.0, per_class[2] / 1024.0));
	}
	if (s_perf_enable)
	{
		Console.WriteLn(fmt::format("@HWSTAT@ Minimum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMinimumFrameTime(), 1000.0f / PerformanceMetrics::GetMinimumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Average Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetAverageFrameTime(), 1000.0f / PerformanceMetrics::GetAverageFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Maximum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMaximumFrameTime(), 1000.0f / PerformanceMetrics::GetMaximumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ CPU Thread Usage: {:.3f} %", s_perf_sum_cpu_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GS Thread Usage: {:.3f} %", s_perf_sum_gs_thread_usage / s_perf_updates));
		// Only emitted under GSBackThreadMode >= Lockstep. Omitted rather than reported as a
		// flat zero, so a comparison across the two configurations doesn't read as a GS win
		// that is really work moved onto an unlisted thread.
		if (s_perf_saw_gs_back_thread)
			Console.WriteLn(fmt::format("@HWSTAT@ GS Back Thread Usage: {:.3f} %", s_perf_sum_gs_back_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GPU Usage: {:.3f} %", s_perf_sum_gpu_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average CPU Thread Time: {:.3f} ms", s_perf_sum_cpu_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GS Thread Time: {:.3f} ms", s_perf_sum_gs_thread_time / s_perf_updates));
		if (s_perf_saw_gs_back_thread)
			Console.WriteLn(fmt::format("@HWSTAT@ Average GS Back Thread Time: {:.3f} ms", s_perf_sum_gs_back_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GPU Time: {:.3f} ms", s_perf_sum_gpu_time / s_perf_updates));
	}
	if (!s_stats_json_path.empty())
	{
		// Percentiles come from the measured per-frame series, which only exists when
		// -stats-json is active. Run-aggregate min/avg/max cannot locate a spike.
		std::vector<float> frame_times;
		frame_times.reserve(s_frame_samples.size());
		for (const FrameSample& s : s_frame_samples)
		{
			if (!s.idle && s.frame_ms > 0.0f)
				frame_times.push_back(s.frame_ms);
		}
		std::sort(frame_times.begin(), frame_times.end());

		Console.WriteLn(fmt::format("@HWSTAT@ Frame Time p50/p95/p99: {:.3f} / {:.3f} / {:.3f} ms",
			Percentile(frame_times, 0.50), Percentile(frame_times, 0.95), Percentile(frame_times, 0.99)));

		// The absolute per-draw CPU line (same arithmetic as the JSON summary). A ratio
		// to Classic can only say "not worse"; this says how much a draw costs.
		double gs_cpu_ms_total = 0.0;
		u64 gs_cpu_draws = 0;
		for (const FrameSample& s : s_frame_samples)
		{
			if (s.idle || s.gs_cpu_ms <= 0.0f || s.draws == 0)
				continue;
			gs_cpu_ms_total += s.gs_cpu_ms;
			gs_cpu_draws += s.draws;
		}
		if (gs_cpu_draws)
		{
			Console.WriteLn(fmt::format("@HWSTAT@ GS Thread CPU per draw: {:.3f} us ({:.3f} ms over {} draws{})",
				gs_cpu_ms_total * 1000.0 / static_cast<double>(gs_cpu_draws), gs_cpu_ms_total, gs_cpu_draws,
				s_saw_gs_back_thread_in_stats ? ", front thread only" : ""));
		}

		// What the frame cost that nothing named: wall clock less GS-thread CPU less the two fence
		// bills. Same arithmetic as untracked_ms_* in the JSON summary; the submit and map bills
		// are partly CPU time gs_cpu_ms already counted, so they sit on their own line above
		// rather than inside this one.
		std::vector<float> untracked_ms;
		double untracked_ms_total = 0.0;
		u64 submits_total = 0;
		untracked_ms.reserve(s_frame_samples.size());
		for (const FrameSample& s : s_frame_samples)
		{
			if (s.idle || s.frame_ms <= 0.0f)
				continue;
			const double blocked_ms = static_cast<double>(s.sync_wait_ns + s.ring_wait_ns) / 1e6;
			const double residual = static_cast<double>(s.frame_ms) - static_cast<double>(s.gs_cpu_ms) - blocked_ms;
			untracked_ms.push_back(static_cast<float>(residual));
			untracked_ms_total += residual;
			submits_total += s.submits;
		}
		std::sort(untracked_ms.begin(), untracked_ms.end());
		if (!untracked_ms.empty())
		{
			Console.WriteLn(fmt::format("@HWSTAT@ Untracked Frame Time p50/p95: {:.3f} / {:.3f} ms "
										"({:.3f} ms over {} drawn frames; fence bills only)",
				Percentile(untracked_ms, 0.50), Percentile(untracked_ms, 0.95), untracked_ms_total,
				untracked_ms.size()));
			Console.WriteLn(fmt::format("@HWSTAT@ Submits per drawn frame: {:.2f} ({} over {} frames)",
				static_cast<double>(submits_total) / static_cast<double>(untracked_ms.size()), submits_total,
				untracked_ms.size()));
		}
	}
	for (const std::string& line : s_extended_stats_snapshot)
		Console.WriteLn(fmt::format("@HWSTAT@ {}", line));
	Console.WriteLn("============================================");

	if (!s_stats_json_path.empty())
		WriteStatsJson(s_stats_json_path);

	if (!s_drawlog_path.empty())
		GSDrawLog::WriteCSV(s_drawlog_path);
}

#ifdef _WIN32
// We can't handle unicode in filenames if we don't use wmain on Win32.
#define main real_main
#endif

// Hands VMManager the thread-placement mode this run is to use, and says so.
//
// This has to run before VMManager::Initialize: SetEmuThreadAffinities is called from
// inside it, so a mode written afterwards would not reach the first placement, and on a
// big.LITTLE device the first placement is the one the whole run happens under.
//
// The runner overrides the compiled-in default rather than accepting it. That default is
// the Android app's policy, pushed from the app's settings before the VM boots, and a
// headless measurement binary has no app to push anything -- so it silently keeps
// whatever the last app-side change left behind. That is not hypothetical: 07bf25a171
// moved the default from 0 to 7, and on the RG477V an ac3 dump's frame p95 went 14.2 ->
// 17.2 ms with GS-thread CPU time flat, which took a four-arm bisect to attribute
// because no artifact of either round named the mode it ran under. Hence 0 here and the
// three keys in the stats JSON.
static void ApplyAffinityMode()
{
#if defined(__ANDROID__)
	g_android_affinity_mode = s_affinity_mode;
#else
	// No affinity path compiled in on this platform, so there is no mode to be in. Say so
	// once if the flag was given, and record -1 rather than a number that did nothing.
	if (std::strcmp(s_affinity_source, "flag") == 0)
	{
		std::fprintf(stderr,
			"pcsx2-gsrunner: -affinity %d: this build has no thread-placement path, so the flag does nothing.\n",
			s_affinity_mode);
	}
	s_affinity_mode = -1;
	s_affinity_source = "unsupported";
#endif
}

// Pins the GS thread where -gspin asked, if it asked, and then reads back where the
// thread actually ended up -- always, flag or no flag.
//
// The read-back is unconditional because a request is not a result and, more to the
// point, because the run with no flag is not an unpinned run. VMManager pins the GS
// thread itself during Initialize whenever EmuCore/EnableThreadPinning is on, which it
// is by default, so on a big.LITTLE device a core has already been chosen by the time
// anything here runs. That choice decides what the run's GS-thread CPU time means, so
// it belongs in the record whether we made it or not.
//
// gs_pin_source names who did it, because the same comma list means different things:
//   flag       -- the read-back is exactly what -gspin asked for
//   vmmanager  -- something narrowed the thread and it was not us (in practice
//                 VMManager's own pinning, including when -gspin asked and missed)
//   none       -- the read-back is the set the process started with, so nothing pinned
//                 anything, or the platform cannot be asked at all
//
// A pin that did not take warns once on stderr and the run continues. The harness reads
// the JSON, and a run whose thread went elsewhere is still a valid run -- just not a
// placement-controlled one, which is exactly what these three keys let it work out.
static void ApplyGSThreadPin()
{
	// Reported here rather than where the mode is applied, so the run's whole thread
	// placement -- the mode VMManager was given, where that number came from, and the CPU
	// set the process was handed before anything narrowed it -- reads as one block next to
	// the GS pin line below. The suite runs the runner under taskset, so the inherited mask
	// is the thing mode 0 is measured against: mode 0 hands every emu thread an empty mask,
	// which means every processor, so it widens past whatever taskset asked for.
	Console.WriteLn(fmt::format("Affinity mode {} (source: {}), inherited CPU mask 0x{:x}", s_affinity_mode,
		s_affinity_source, s_baseline_cpu_mask));

	const Threading::ThreadHandle& gs_thread = MTGS::GetThreadHandle();
	if (!gs_thread)
	{
		s_gs_pin_effective = "unsupported";
		s_gs_pin_source = "none";
		if (!s_gs_pin_request.empty())
			std::fprintf(stderr, "pcsx2-gsrunner: -gspin %s: there is no GS thread to pin.\n", s_gs_pin_request.c_str());
		return;
	}

	// A zero mask means "every processor" to SetAffinity, so a request naming only CPUs
	// the mask cannot express must not be passed through: that would un-pin the thread
	// while reporting that a pin was asked for.
	if (!s_gs_pin_request.empty() && s_gs_pin_mask != 0)
		gs_thread.SetAffinity(s_gs_pin_mask);

	const u64 effective = gs_thread.GetAffinity();
	if (effective == 0)
	{
		// No per-thread affinity introspection on this platform (Darwin and Windows both
		// return 0 here, and Darwin cannot pin at all), so there is nothing to report but
		// that the platform does not answer the question.
		s_gs_pin_effective = "unsupported";
		s_gs_pin_source = "none";
		if (!s_gs_pin_request.empty())
		{
			std::fprintf(stderr, "pcsx2-gsrunner: -gspin %s: this platform does not support pinning a thread.\n",
				s_gs_pin_request.c_str());
		}
		return;
	}

	s_gs_pin_effective = FormatCpuMask(effective);

	const bool took = (!s_gs_pin_request.empty() && effective == s_gs_pin_mask);
	if (took)
		s_gs_pin_source = "flag";
	else if (s_baseline_cpu_mask != 0 && effective == s_baseline_cpu_mask)
		s_gs_pin_source = "none";
	else
		s_gs_pin_source = "vmmanager";

	if (took)
	{
		Console.WriteLn(fmt::format("GS thread pinned to CPU(s) {}", s_gs_pin_effective));
	}
	else if (!s_gs_pin_request.empty())
	{
		std::fprintf(stderr,
			"pcsx2-gsrunner: -gspin %s did not take -- the GS thread may run on %s. The run continues, but its "
			"GS-thread CPU time is not placement-controlled.\n",
			s_gs_pin_request.c_str(), s_gs_pin_effective.c_str());
	}
	else
	{
		Console.WriteLn(fmt::format("GS thread runs on CPU(s) {} (pinned by: {})", s_gs_pin_effective, s_gs_pin_source));
	}
}

static void CPUThreadMain(VMBootParameters* params, std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);

	if (VMManager::Internal::CPUThreadInitialize())
	{
		// apply new settings (e.g. pick up renderer change)
		VMManager::ApplySettings();
		GSDumpReplayer::SetIsDumpRunner(true);

		if (VMManager::Initialize(*params) == VMBootResult::StartupSuccess)
		{
			// The GS thread exists from here on, and this is the last quiet moment before
			// frames start, so the pin lands before the first one is timed.
			//
			// It also has to land AFTER VMManager::Initialize, not before: with
			// EmuCore/EnableThreadPinning on -- which it is by default -- Initialize pins
			// the GS thread itself, to whichever processor its frequency sort ranked next.
			// Moving this call any earlier means that pin quietly overwrites -gspin, and
			// the only visible symptom would be that the flag stops doing anything.
			ApplyGSThreadPin();

			// run until end
			GSDumpReplayer::SetLoopCount(s_loop_count);
			// Read here, not in WriteStatsJson: by the time that runs, VMManager::Shutdown()
			// has released the dump file and the answer would always be zero.
			s_dump_frames_per_loop = GSDumpReplayer::GetDumpFrameCount();
			// Armed before the first packet, so rung zero is the state the freeze left
			// and every later rung is named by the packet it follows.
			GSLadder::Begin(s_ladder_opts);
			// The front-end decode instrument arms here for the same reason: the
			// stream must start at the first record the run produces, and nothing
			// before this point emits one (the dump's initial state arrives as a
			// savestate defrost, not as GIF traffic).
			if (!s_fedump_path.empty())
				GSFeDecode::BeginRecord(s_fedump_path, GIT_REV);
			else if (!s_fediff_path.empty())
				GSFeDecode::BeginDiff(s_fediff_path);
			// The ledger's join key to the ladder. Only paid for when a ledger is being
			// written, because it costs a queued store per packet.
			GSDumpReplayer::SetPublishPacketMarks(!s_drawlog_path.empty());
			VMManager::SetState(VMState::Running);
			// gsrunner is diagnostic-by-design; always collect extended stats so DumpStats has data.
			if (g_gs_device)
				g_gs_device->EnableExtendedStats(true);
			if (s_perf_enable)
			{
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
				g_gs_device->SetGPUTimingEnabled(true);
			}
			while (VMManager::GetState() == VMState::Running)
				VMManager::Execute();
			// Before Shutdown: the last rungs are still queued on the GS thread, and
			// Finish drains them. After teardown there is no local memory to read.
			GSLadder::Finish();
			// Snapshot backend-specific stats before the GS device is destroyed. The wait bill rides
			// the same snapshot so it also carries whatever was paid after the last present.
			if (g_gs_device)
				s_extended_stats_snapshot = g_gs_device->GetExtendedStats();
			LatchDeviceWaitBill();
			VMManager::Shutdown(false);
			GSRunner::DumpStats();
			// After Shutdown, so the last records are in: End() also reports a
			// recording the replay never caught up with. A divergence is a failing
			// run, not a note in the log -- this is a gate, so it must be readable
			// from the exit code alone.
			GSFeDecode::End();
			ret->store(GSFeDecode::Diverged() ? EXIT_FAILURE : EXIT_SUCCESS);
		}
	}

	VMManager::Internal::CPUThreadShutdown();
	GSRunner::StopPlatformMessagePump();
}

// Set by the SIGINT/SIGTERM handlers (async-signal-safe: just an atomic store)
// and consumed on the CPU thread in PumpMessagesOnCPUThread(), which issues the
// actual VMManager::SetState(Stopping). Calling SetState() from signal context
// is not async-signal-safe — it can assert/log, take mutexes, and WaitGS/WaitVU.
static std::atomic<bool> s_signal_stop_requested{false};

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	GSRunner::InitializeConsole();

	// Before the VM, and so before either VMManager's thread pinning or -gspin, which is
	// the only moment this reads as the untouched inherited set.
	s_baseline_cpu_mask = Threading::ThreadHandle::GetForCallingThread().GetAffinity();

	// Clean SIGINT/SIGTERM → VM stop, so DumpStats() still fires on ^C or SIGTERM during -loop 0.
	// Defer the actual stop to the CPU thread (see s_signal_stop_requested).
	std::signal(SIGINT, [](int) { s_signal_stop_requested.store(true); });
	std::signal(SIGTERM, [](int) { s_signal_stop_requested.store(true); });

	if (!GSRunner::InitializeConfig())
	{
		// Each failing step in there names itself and the path it was looking at. This line
		// is the backstop, so a future early return that forgets to say anything still
		// leaves something on the terminal rather than an exit code on its own.
		EarlyError("startup configuration failed, cannot continue.");
		return EXIT_FAILURE;
	}

	VMBootParameters params;
	if (!GSRunner::ParseCommandLineArgs(argc, argv, params))
		return EXIT_FAILURE;

	// Before the CPU thread is started, and so before VMManager::Initialize calls
	// SetEmuThreadAffinities for the first time.
	ApplyAffinityMode();

	// Emitting a console replay payload needs no VM, no GS device and no window: the
	// dump already carries the freeze and the packet stream, so this is a transform on
	// the file. Do it here and leave, before anything expensive is stood up.
	if (s_emit_payload)
		return GSReplayPayload::Emit(params.filename, s_payload_opts) ? EXIT_SUCCESS : EXIT_FAILURE;

	// Must happen before the GS device is created on the CPU thread: RenderDoc
	// installs its graphics-API hooks when its library loads, so a standalone run
	// has to get it in ahead of libEGL/libvulkan.
	if (!s_renderdoc_path.empty() &&
		!RenderDocCapture::Initialize(s_renderdoc_path, s_renderdoc_start_frame, s_renderdoc_frame_count))
	{
		// RenderDocCapture reports the reason to stderr itself; Console output does
		// not reach the terminal this early in startup.
		return EXIT_FAILURE;
	}

	if (s_use_window.value_or(true) && !GSRunner::CreatePlatformWindow())
	{
		EarlyError("could not create the host window. A headless device run wants -surfaceless.");
		return EXIT_FAILURE;
	}

	// Override settings that shouldn't be picked up from defaults or INIs.
	GSRunner::SettingsOverride();

	std::atomic<int> thread_ret;
	std::thread cputhread(CPUThreadMain, &params, &thread_ret);
	GSRunner::PumpPlatformMessages(/*forever=*/true);
	cputhread.join();

	RenderDocCapture::Shutdown();
	GSRunner::DestroyPlatformWindow();

	return thread_ret.load();
}

void Host::PumpMessagesOnCPUThread()
{
	// Honor a pending ^C / SIGTERM here, on the CPU thread, where SetState() is
	// safe to call. exchange() makes the transition fire exactly once.
	if (s_signal_stop_requested.exchange(false))
		VMManager::SetState(VMState::Stopping);

	// Drain work posted by Host::RunOnCPUThread (PINE commands). Tasks run outside
	// the lock so one that posts more work cannot deadlock.
	for (;;)
	{
		std::function<void()> task;
		{
			std::unique_lock lock(s_cpu_thread_tasks_mutex);
			if (s_cpu_thread_tasks.empty())
			{
				s_cpu_thread_tasks_done.notify_all();
				break;
			}
			task = std::move(s_cpu_thread_tasks.front());
			s_cpu_thread_tasks.pop_front();
		}
		task();
	}

	// update GS thread copy of frame number
	MTGS::RunOnGSThread([frame_number = GSDumpReplayer::GetFrameNumber()]() { s_dump_frame_number = frame_number; });
	MTGS::RunOnGSThread([loop_number = GSDumpReplayer::GetLoopCount()]() { s_loop_number = loop_number; });
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, 2, count_str.view());
	}

	return ret;
}

//////////////////////////////////////////////////////////////////////////
// Platform specific code
//////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

static constexpr LPCWSTR WINDOW_CLASS_NAME = L"PCSX2GSRunner";
static HWND s_hwnd = NULL;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool GSRunner::CreatePlatformWindow()
{
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.hIcon = NULL;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszMenuName = NULL;
	wc.lpszClassName = WINDOW_CLASS_NAME;
	wc.hIconSm = NULL;

	if (!RegisterClassExW(&wc))
	{
		Console.Error("Window registration failed.");
		return false;
	}

	s_hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, L"PCSX2 GS Runner",
		WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU | WS_SIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH,
		WINDOW_HEIGHT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!s_hwnd)
	{
		Console.Error("CreateWindowEx failed.");
		return false;
	}

	ShowWindow(s_hwnd, SW_SHOW);
	UpdateWindow(s_hwnd);

	// make sure all messages are processed before returning
	PumpPlatformMessages();
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (!s_hwnd)
		return;

	PumpPlatformMessages();
	DestroyWindow(s_hwnd);
	s_hwnd = {};
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;

	if (s_hwnd)
	{
		RECT rc = {};
		GetWindowRect(s_hwnd, &rc);
		wi.surface_width = static_cast<u32>(rc.right - rc.left);
		wi.surface_height = static_cast<u32>(rc.bottom - rc.top);
		wi.surface_scale = 1.0f;
		wi.type = WindowInfo::Type::Win32;
		wi.window_handle = s_hwnd;
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
	}

	return wi;
}

static constexpr int SHUTDOWN_MSG = WM_APP + 0x100;
static DWORD MainThreadID;

void GSRunner::PumpPlatformMessages(bool forever)
{
	MSG msg;
	while (true)
	{
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == SHUTDOWN_MSG)
				return;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!forever)
			return;
		WaitMessage();
	}
}

void GSRunner::StopPlatformMessagePump()
{
	PostThreadMessageW(MainThreadID, SHUTDOWN_MSG, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int wmain(int argc, wchar_t** argv)
{
	std::vector<std::string> u8_args;
	u8_args.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; i++)
		u8_args.push_back(StringUtil::WideStringToUTF8String(argv[i]));

	std::vector<char*> u8_argptrs;
	u8_argptrs.reserve(u8_args.size());
	for (int i = 0; i < argc; i++)
		u8_argptrs.push_back(u8_args[i].data());
	u8_argptrs.push_back(nullptr);

	MainThreadID = GetCurrentThreadId();

	return real_main(argc, u8_argptrs.data());
}

#elif defined(__APPLE__)

static void* s_window;
static WindowInfo s_wi;

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_window, "Tried to create window when there already was one!");
	s_window = CocoaTools::CreateWindow("PCSX2 GS Runner", WINDOW_WIDTH, WINDOW_HEIGHT);
	CocoaTools::GetWindowInfoFromWindow(&s_wi, s_window);
	PumpPlatformMessages();
	return s_window;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_window) {
		CocoaTools::DestroyWindow(s_window);
		s_window = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_window)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	CocoaTools::RunCocoaEventLoop(forever);
}

void GSRunner::StopPlatformMessagePump()
{
	CocoaTools::StopMainThreadEventLoop();
}

#elif defined(__linux__) && defined(WAYLAND_API)
// Wayland frontend for gsrunner. Used on handheld targets where the GPU's
// libmali variant is built for Wayland WSI (vkCreateWaylandSurfaceKHR) and
// VK_KHR_display is half-implemented (returns present_supported=false on the
// sole queue family). Runs as a normal Wayland client alongside the running
// compositor — no need to stop sway/weston.

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <cstring>
#include <poll.h>

static wl_display* s_display = nullptr;
static wl_registry* s_registry = nullptr;
static wl_compositor* s_compositor = nullptr;
static xdg_wm_base* s_wm_base = nullptr;
static wl_surface* s_surface = nullptr;
static xdg_surface* s_xdg_surface = nullptr;
static xdg_toplevel* s_xdg_toplevel = nullptr;
static WindowInfo s_wi;
static std::atomic<bool> s_shutdown_requested{false};
static bool s_initial_configure_received = false;

static void wl_wm_base_ping(void*, xdg_wm_base* wm_base, uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}
static const xdg_wm_base_listener s_wm_base_listener = {wl_wm_base_ping};

static void wl_xdg_surface_configure(void*, xdg_surface* xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	s_initial_configure_received = true;
}
static const xdg_surface_listener s_xdg_surface_listener = {wl_xdg_surface_configure};

static void wl_xdg_toplevel_configure(void*, xdg_toplevel*, int32_t width, int32_t height, wl_array*)
{
	if (width > 0 && height > 0)
	{
		s_wi.surface_width = static_cast<u32>(width);
		s_wi.surface_height = static_cast<u32>(height);
	}
}
static void wl_xdg_toplevel_close(void*, xdg_toplevel*)
{
	s_shutdown_requested.store(true);
}
// Stubs for the newer xdg_toplevel_listener slots. These struct members exist
// only when the wayland-scanner-generated header was built against a new enough
// xdg-shell (configure_bounds: protocol v4 / wayland-protocols >= 1.20;
// wm_capabilities: v5 / >= 1.26). Guard both the stubs and their initializer
// slots on the matching SINCE_VERSION macros so the aggregate initializer always
// matches the generated struct's member count — without the guards this is a hard
// "too many initializers" build break on older protocol headers.
#ifdef XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION
static void wl_xdg_toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
#endif
#ifdef XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION
static void wl_xdg_toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
#endif
static const xdg_toplevel_listener s_xdg_toplevel_listener = {
	wl_xdg_toplevel_configure,
	wl_xdg_toplevel_close,
#ifdef XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION
	wl_xdg_toplevel_configure_bounds,
#endif
#ifdef XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION
	wl_xdg_toplevel_wm_capabilities,
#endif
};

static void wl_registry_global(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
	if (std::strcmp(interface, wl_compositor_interface.name) == 0)
	{
		s_compositor = static_cast<wl_compositor*>(
			wl_registry_bind(registry, name, &wl_compositor_interface, std::min<uint32_t>(version, 4u)));
	}
	else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0)
	{
		s_wm_base = static_cast<xdg_wm_base*>(
			wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min<uint32_t>(version, 4u)));
		xdg_wm_base_add_listener(s_wm_base, &s_wm_base_listener, nullptr);
	}
}
static void wl_registry_global_remove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener s_registry_listener = {wl_registry_global, wl_registry_global_remove};

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_display && !s_surface, "Tried to create window when there already was one!");

	s_display = wl_display_connect(nullptr);
	if (!s_display)
	{
		Console.Error("wl_display_connect failed (check $WAYLAND_DISPLAY)");
		return false;
	}

	s_registry = wl_display_get_registry(s_display);
	wl_registry_add_listener(s_registry, &s_registry_listener, nullptr);
	wl_display_roundtrip(s_display);

	if (!s_compositor || !s_wm_base)
	{
		Console.Error("Wayland compositor missing wl_compositor or xdg_wm_base");
		DestroyPlatformWindow();
		return false;
	}

	s_surface = wl_compositor_create_surface(s_compositor);
	s_xdg_surface = xdg_wm_base_get_xdg_surface(s_wm_base, s_surface);
	xdg_surface_add_listener(s_xdg_surface, &s_xdg_surface_listener, nullptr);
	s_xdg_toplevel = xdg_surface_get_toplevel(s_xdg_surface);
	xdg_toplevel_add_listener(s_xdg_toplevel, &s_xdg_toplevel_listener, nullptr);
	xdg_toplevel_set_title(s_xdg_toplevel, "PCSX2 GS Runner");
	xdg_toplevel_set_app_id(s_xdg_toplevel, "net.pcsx2.gsrunner");

	wl_surface_commit(s_surface);
	// Round-trip until the compositor acks our initial configure, so the
	// Vulkan WSI sees a properly-sized surface from the first swapchain.
	while (!s_initial_configure_received)
	{
		if (wl_display_dispatch(s_display) < 0)
		{
			Console.Error("wl_display_dispatch failed during initial configure");
			DestroyPlatformWindow();
			return false;
		}
	}

	s_wi.type = WindowInfo::Type::Wayland;
	s_wi.display_connection = s_display;
	s_wi.window_handle = s_surface;
	if (s_wi.surface_width == 0)
		s_wi.surface_width = WINDOW_WIDTH;
	if (s_wi.surface_height == 0)
		s_wi.surface_height = WINDOW_HEIGHT;
	s_wi.surface_scale = 1.0f;
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_xdg_toplevel) { xdg_toplevel_destroy(s_xdg_toplevel); s_xdg_toplevel = nullptr; }
	if (s_xdg_surface)  { xdg_surface_destroy(s_xdg_surface);   s_xdg_surface = nullptr; }
	if (s_surface)      { wl_surface_destroy(s_surface);        s_surface = nullptr; }
	if (s_wm_base)      { xdg_wm_base_destroy(s_wm_base);       s_wm_base = nullptr; }
	if (s_compositor)   { wl_compositor_destroy(s_compositor);  s_compositor = nullptr; }
	if (s_registry)     { wl_registry_destroy(s_registry);      s_registry = nullptr; }
	if (s_display)      { wl_display_disconnect(s_display);     s_display = nullptr; }
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_display && s_surface)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	if (!s_display)
		return;

	if (!forever)
	{
		wl_display_flush(s_display);
		wl_display_dispatch_pending(s_display);
		return;
	}

	const int fd = wl_display_get_fd(s_display);
	while (!s_shutdown_requested.load())
	{
		// Everything below has to stay non-blocking, because the only thing that ends this loop is
		// the shutdown flag being noticed on the next iteration. wl_display_dispatch() would read
		// the queued events and then *wait* for more, and a window nobody is drawing to gets no
		// further events, so the flag would never be re-tested and the process would never exit.
		while (wl_display_prepare_read(s_display) != 0)
		{
			if (wl_display_dispatch_pending(s_display) < 0)
				return;
		}

		wl_display_flush(s_display);

		pollfd pfd = {fd, POLLIN, 0};
		const int p = poll(&pfd, 1, 16); // cap so we keep checking shutdown
		if (p > 0 && (pfd.revents & POLLIN))
		{
			if (wl_display_read_events(s_display) < 0)
				return;
		}
		else
		{
			wl_display_cancel_read(s_display);
		}

		if (wl_display_dispatch_pending(s_display) < 0)
			return;
	}
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}

#elif defined(__linux__) && defined(X11_API)
static Display* s_display = nullptr;
static Window s_window = None;
static WindowInfo s_wi;
static std::atomic<bool> s_shutdown_requested{false};

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_display && s_window == None, "Tried to create window when there already was one!");

	s_display = XOpenDisplay(nullptr);
	if (!s_display)
	{
		Console.Error("Failed to open X11 display");
		return false;
	}

	int screen = DefaultScreen(s_display);
	Window root = RootWindow(s_display, screen);

	s_window = XCreateSimpleWindow(s_display, root, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 1,
		BlackPixel(s_display, screen), WhitePixel(s_display, screen));

	if (s_window == None)
	{
		Console.Error("Failed to create X11 window");
		XCloseDisplay(s_display);
		s_display = nullptr;
		return false;
	}

	XStoreName(s_display, s_window, "PCSX2 GS Runner");
	XSelectInput(s_display, s_window, StructureNotifyMask);
	XMapWindow(s_display, s_window);

	s_wi.type = WindowInfo::Type::X11;
	s_wi.display_connection = s_display;
	s_wi.window_handle = reinterpret_cast<void*>(s_window);
	s_wi.surface_width = WINDOW_WIDTH;
	s_wi.surface_height = WINDOW_HEIGHT;
	s_wi.surface_scale = 1.0f;

	XFlush(s_display);
	PumpPlatformMessages();
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_display && s_window != None)
	{
		XDestroyWindow(s_display, s_window);
		s_window = None;
	}

	if (s_display)
	{
		XCloseDisplay(s_display);
		s_display = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_display && s_window != None)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	if (!s_display)
		return;

	do
	{
		while (XPending(s_display) > 0)
		{
			XEvent event;
			XNextEvent(s_display, &event);

			switch (event.type)
			{
				case ConfigureNotify:
				{
					const XConfigureEvent& configure = event.xconfigure;
					s_wi.surface_width = static_cast<u32>(configure.width);
					s_wi.surface_height = static_cast<u32>(configure.height);
					break;
				}
				case DestroyNotify:
					return;
				default:
					break;
			}
		}

		if (s_shutdown_requested.load())
			return;

		if (forever)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	} while (forever && !s_shutdown_requested.load());
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}

#elif defined(__linux__)
// No X11/Wayland on this build (handheld kmsdrm target). Vulkan VK_KHR_display
// owns the screen; VulkanDirect is reported with the requested resolution and
// the GS device's display backend enumerates the monitor itself. Mirrors
// pcsx2-sdl/Main.cpp::BuildWindowInfo.
static std::atomic<bool> s_shutdown_requested{false};

bool GSRunner::CreatePlatformWindow()
{
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_use_window.value_or(true))
	{
		wi.type = WindowInfo::Type::VulkanDirect;
		wi.surface_width = WINDOW_WIDTH;
		wi.surface_height = WINDOW_HEIGHT;
		wi.surface_scale = 1.0f;
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
	}
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	if (!forever)
		return;

	while (!s_shutdown_requested.load())
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}

#endif // _WIN32 / __APPLE__ / __linux__
