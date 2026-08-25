// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/CocoaTools.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/Renderers/Common/GSTileSelectionPolicy.h"
#include "pcsx2/GS/GSPerfMon.h"
#include "pcsx2/GS/GSFeDecode.h"
#include "pcsx2/GS/Renderers/HW/GSDrawLog.h"
#include "pcsx2/GS/Renderers/Tile/GSTileOracle.h"
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

// -renderdoc / -renderdoc-frame. Empty path means capture is not requested.
static std::string s_renderdoc_path;
static u32 s_renderdoc_start_frame = 1;
static u32 s_renderdoc_frame_count = 1;

// Owned by the GS thread.
static u32 s_dump_frame_number = 0;
static u32 s_loop_number = s_loop_count;
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
	u64 oob_ns = 0;
	u64 oob_calls = 0;
	u64 ring_ns = 0;
	u64 ring_calls = 0;
	// Its own bucket, disjoint from sync_* -- but inside GpuBlockingWaits with it, unlike ring_*.
	u64 source_set_ns = 0;
	u64 source_set_calls = 0;
};
static DeviceWaitBill s_device_wait_bill;

static void LatchDeviceWaitBill()
{
	if (!g_gs_device)
		return;
	s_device_wait_bill.sync_ns = g_gs_device->GetSyncWaitNs();
	s_device_wait_bill.sync_calls = g_gs_device->GetSyncWaitCalls();
	s_device_wait_bill.oob_ns = g_gs_device->GetOobWaitNs();
	s_device_wait_bill.oob_calls = g_gs_device->GetOobWaitCalls();
	s_device_wait_bill.ring_ns = g_gs_device->GetRingWaitNs();
	s_device_wait_bill.ring_calls = g_gs_device->GetRingWaitCalls();
	s_device_wait_bill.source_set_ns = g_gs_device->GetSourceSetWaitNs();
	s_device_wait_bill.source_set_calls = g_gs_device->GetSourceSetWaitCalls();
}

// Per-frame statistics series. Run-aggregate min/avg/max cannot locate a spike, so
// every presented frame is recorded and written out as JSON at the end of the run.
// Counters are exact per-frame deltas; frame_ms is measured here rather than taken
// from PerformanceMetrics, whose values are window averages.
struct FrameSample
{
	u32 frame;
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
	/// Times the GS thread blocked on the GPU out of turn (readback submit-and-wait,
	/// out-of-band fence, explicit sync). One per frame serializes the whole pipeline, so the
	/// number to read is whether it is zero, not whether it fell.
	u64 gpu_blocking_waits;
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
	u64 tile_payload_bytes;
};
// Work posted from other threads (the PINE server) to run on the CPU thread.
static std::mutex s_cpu_thread_tasks_mutex;
static std::condition_variable s_cpu_thread_tasks_done;
static std::deque<std::function<void()>> s_cpu_thread_tasks;

static std::string s_stats_json_path;
static std::string s_drawlog_path;
static std::string s_tile_oracle_path;
static std::string s_fedump_path;
static std::string s_fediff_path;
static std::vector<FrameSample> s_frame_samples;
static std::string s_device_name;
static std::string s_driver_info;
static u64 s_frame_timer_last = 0;
static u64 s_gs_cpu_time_last = 0;
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
static double s_last_tile_payload_bytes = 0;
static u64 s_total_tile_payload_bytes = 0;
// The TileGpu per-frame depth-pass predictor's three counters. Zero on every run that is not
// running it, which is what lets a device gate read a run's arm out of stats.json rather than out of
// an emulog: merged frames, policy switches, and the metric's numerator (its denominator is `draws`,
// already here).
static double s_last_depth_merged_frames = 0;
static u64 s_total_depth_merged_frames = 0;
static double s_last_depth_policy_switches = 0;
static u64 s_total_depth_policy_switches = 0;
static double s_last_depth_passes_saved = 0;
static u64 s_total_depth_passes_saved = 0;
// The TileGpu scissor census. Counted on whichever scissor road the device took, so a run on either
// reports what the other would have cost: draws planned, draws the scissor rejects part of, and the
// indirect calls a per-call vkCmdSetScissor adds over the clip-plane road.
static double s_last_scissor_draws = 0;
static u64 s_total_scissor_draws = 0;
static double s_last_scissor_cuts = 0;
static u64 s_total_scissor_cuts = 0;
static double s_last_scissor_extra_calls = 0;
static u64 s_total_scissor_extra_calls = 0;
static double s_last_dualsrc_draws = 0;
static u64 s_total_dualsrc_draws = 0;
static double s_last_dualsrc_restore = 0;
static u64 s_total_dualsrc_restore = 0;
static double s_last_dualsrc_companions = 0;
static u64 s_total_dualsrc_companions = 0;
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

bool GSRunner::InitializeConfig()
{
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
		return false;

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
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
		}

		// Same reasoning, every frame rather than once: the device's wait counters have to be read
		// somewhere the device certainly exists, and this is the only per-frame point that qualifies.
		LatchDeviceWaitBill();

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
		sample.tile_payload_bytes = update_stat(GSPerfMon::TilePayloadBytes, s_total_tile_payload_bytes, s_last_tile_payload_bytes);
		update_stat(GSPerfMon::TileGpuDepthMergedFrames, s_total_depth_merged_frames, s_last_depth_merged_frames);
		update_stat(GSPerfMon::TileGpuDepthPolicySwitches, s_total_depth_policy_switches, s_last_depth_policy_switches);
		update_stat(GSPerfMon::TileGpuDepthPassesSaved, s_total_depth_passes_saved, s_last_depth_passes_saved);
		update_stat(GSPerfMon::TileGpuScissorDraws, s_total_scissor_draws, s_last_scissor_draws);
		update_stat(GSPerfMon::TileGpuScissorCuts, s_total_scissor_cuts, s_last_scissor_cuts);
		update_stat(GSPerfMon::TileGpuScissorExtraCalls, s_total_scissor_extra_calls, s_last_scissor_extra_calls);
		update_stat(GSPerfMon::TileGpuDualSrcDraws, s_total_dualsrc_draws, s_last_dualsrc_draws);
		update_stat(GSPerfMon::TileGpuDualSrcRestore, s_total_dualsrc_restore, s_last_dualsrc_restore);
		update_stat(GSPerfMon::TileGpuDualSrcCompanions, s_total_dualsrc_companions, s_last_dualsrc_companions);

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

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
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
	std::fprintf(stderr, "  -variant <auto|classic|tile|tilegpu>: Sets the HW renderer variant (Vulkan only for "
						 "tile/tilegpu). Defaults to Auto.\n");
	std::fprintf(stderr, "  -swthreads <threads>: Sets the number of threads for the software renderer.\n");
	std::fprintf(stderr, "  -backthread <mode>: GS back-thread mode (0=off, 1=inline-records, 2=lockstep, 3=pipelined). Defaults to 0.\n");
	std::fprintf(stderr, "  -window: Forces a window to be displayed.\n");
	std::fprintf(stderr, "  -surfaceless: Disables showing a window.\n");
	std::fprintf(stderr, "  -logfile <filename>: Writes emu log to filename.\n");
	std::fprintf(stderr, "  -noshadercache: Disables the shader cache (useful for parallel runs).\n");
	std::fprintf(stderr, "  -perf: Enable frame timing performance stats.\n");
	std::fprintf(stderr, "  -drawlog <path.csv>: Record a per-draw ledger (PS2 register state + backend draw config).\n");
	std::fprintf(stderr, "  -tileoracle <path.csv>: Run the software rasterizer in lockstep against the Tile renderer's "
						 "native route and record every per-draw divergence. Tile only, and orders of magnitude "
						 "slower than a plain run.\n");
	std::fprintf(stderr, "  -fedump <path>: Record the front-end decode surface -- every draw, upload, move, CLUT "
						 "load, readback request and frame boundary the GIF decode hands the renderer, in stream "
						 "order, as canonical bytes. The recording of the shipping decode is what a replacement "
						 "front end is diffed against.\n");
	std::fprintf(stderr, "  -fediff <path>: Replay under the same arm and byte-compare against a -fedump recording. "
						 "Reports the FIRST divergence (event, record, field, byte) and exits non-zero. Record and "
						 "diff must use the same dump, the same renderer arm and the same -loop count.\n");
	std::fprintf(stderr, "  -tilepasssim: Score the GS-semantic minimum pass structure of the run (pass breaks, "
						 "snapshots, syncs a fully-GPU-timeline backend would be forced to take) plus GIF stream "
						 "volume; report at teardown. Tile and TileGpu; an attribution arm, never a timed one.\n");
	std::fprintf(stderr, "  -tilermw: TileGpu only, TEST SCAFFOLDING. Admit every draw whose blend, write mask or "
						 "destination-alpha test fixed-function cannot express to the declared in-pass destination "
						 "read, instead of only the classes whose accuracy repairs have landed. It exists to exercise "
						 "the read over the whole corpus; it is not a correctness or a performance mode.\n");
	std::fprintf(stderr, "  -stats-json <path>: Write per-frame and run-summary statistics as JSON. Combine with -perf "
						 "for frame/GPU timing.\n");
	std::fprintf(stderr, "  -set <Section/Key>=<value>: Override any setting, e.g. -set EmuCore/GS/AccurateBlendingUnit=3. "
						 "Repeatable.\n");
	std::fprintf(stderr, "  -vsync: Force vsync on (FIFO present mode). Workaround for libmali Wayland WSI which "
						 "advertises MAILBOX support but errors VK_ERROR_INITIALIZATION_FAILED on swapchain create.\n");
	std::fprintf(stderr, "  -no-fb-fetch: Disable Vulkan framebuffer fetch (VK_EXT_rasterization_order_attachment_access). "
						 "Use to A/B against drivers that mishandle subpass self-dependencies (e.g. libmali).\n");
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

bool GSRunner::ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params)
{
	std::string dumpdir; // Save from argument -dumpdir for creating sub-directories
	bool no_more_args = false;
	for (int i = 1; i < argc; i++)
	{
		if (!no_more_args)
		{
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

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
					Console.Error("Invalid dump directory specified.");
					return false;
				}

				if (!FileSystem::DirectoryExists(s_output_prefix.c_str()) && !FileSystem::CreateDirectoryPath(s_output_prefix.c_str(), false))
				{
					Console.Error("Failed to create output directory");
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
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
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
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
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
					Console.Error("Invalid RenderDoc capture path specified.");
					return false;
				}
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderdoc-frame"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				if (split.size() > 0)
					s_renderdoc_start_frame = StringUtil::FromChars<u32>(split[0]).value_or(1);
				if (split.size() > 1)
					s_renderdoc_frame_count = std::max(1u, StringUtil::FromChars<u32>(split[1]).value_or(1));
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
				s_loop_count = StringUtil::FromChars<s32>(argv[++i]).value_or(0);
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
					Console.Error("Unknown renderer '%s'", rname);
					return false;
				}

				Console.WriteLn("Using %s renderer.", Pcsx2Config::GSOptions::GetRendererName(type));
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(type));
				continue;
			}
			else if (CHECK_ARG_PARAM("-variant"))
			{
				// Which hardware-renderer implementation runs on the (Vulkan) device —
				// GSTileSelectionPolicy.h owns the decision, this only sets the input.
				const char* vname = argv[++i];

				GSHWRendererVariant variant = GSHWRendererVariant::Auto;
				if (StringUtil::Strcasecmp(vname, "auto") == 0)
					variant = GSHWRendererVariant::Auto;
				else if (StringUtil::Strcasecmp(vname, "classic") == 0)
					variant = GSHWRendererVariant::Classic;
				else if (StringUtil::Strcasecmp(vname, "tile") == 0)
					variant = GSHWRendererVariant::Tile;
				else if (StringUtil::Strcasecmp(vname, "tilegpu") == 0)
					variant = GSHWRendererVariant::TileGpu;
				else
				{
					Console.Error("Unknown HW renderer variant '%s' (auto, classic, tile, tilegpu)", vname);
					return false;
				}

				Console.WriteLn("Using %s HW renderer variant.", GSHWRendererVariantName(variant));
				s_settings_interface.SetIntValue("EmuCore/GS", "HWRendererVariant", static_cast<int>(variant));
				continue;
			}
			else if (CHECK_ARG_PARAM("-backthread"))
			{
				const int mode = StringUtil::FromChars<int>(argv[++i]).value_or(-1);
				if (mode < 0 || mode > 3)
				{
					Console.Error("Invalid GS back-thread mode (0=off, 1=inline-records, 2=lockstep, 3=pipelined)");
					return false;
				}

				Console.WriteLn("Setting GS back-thread mode to %d.", mode);
				s_settings_interface.SetIntValue("EmuCore/GS", "GSBackThreadMode", mode);
				continue;
			}
			else if (CHECK_ARG_PARAM("-swthreads"))
			{
				const int swthreads = StringUtil::FromChars<int>(argv[++i]).value_or(0);
				if (swthreads < 0)
				{
					Console.WriteLn("Invalid number of software threads");
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
					Console.ErrorFmt("INI file {} does not exit.", path);
					return false;
				}

				INISettingsInterface si_ini(path);

				if (!si_ini.Load())
				{
					Console.ErrorFmt("Unable to load INI settings from {}.", path);
					return false;
				}

				for (const auto& [key, value] : si_ini.GetKeyValueList("EmuCore/GS"))
					s_settings_interface.SetStringValue("EmuCore/GS", key.c_str(), value.c_str());

				continue;
			}
			else if (CHECK_ARG_PARAM("-upscale"))
			{
				const float upscale = StringUtil::FromChars<float>(argv[++i]).value_or(0.0f);
				if (upscale < 0.5f)
				{
					Console.WriteLn("Invalid upscale multiplier");
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
			else if (CHECK_ARG_PARAM("-tileoracle"))
			{
				s_tile_oracle_path = argv[++i];
				s_settings_interface.SetBoolValue("EmuCore/GS", "TileDrawOracle", true);
				Console.WriteLn(fmt::format("Recording per-draw lockstep divergences to {}", s_tile_oracle_path));
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
			else if (CHECK_ARG("-tilepasssim"))
			{
				// One flag, both variants that carry the instrument: only one renderer runs, so
				// the key the other reads is inert.
				s_settings_interface.SetBoolValue("EmuCore/GS", "TilePassSim", true);
				s_settings_interface.SetBoolValue("EmuCore/GS", "TileGpuPassSim", true);
				Console.WriteLn("Scoring GS-semantic minimum pass structure (report at teardown)");
				continue;
			}
			else if (CHECK_ARG("-tilermw"))
			{
				s_settings_interface.SetBoolValue("EmuCore/GS", "TileGpuForceSelfRead", true);
				Console.WriteLn("Admitting every inexpressible draw to the TileGpu in-pass destination read");
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
					Console.Error(fmt::format("Malformed -set '{}', expected <Section/Key>=<value>", arg));
					return false;
				}

				const std::string section(arg.substr(0, slash));
				const std::string key(arg.substr(slash + 1, eq - slash - 1));
				const std::string value(arg.substr(eq + 1));
				if (key.empty())
				{
					Console.Error(fmt::format("Malformed -set '{}', empty key", arg));
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
				const std::optional<int> level = StringUtil::FromChars<int>(argv[++i]);
				if (!level.has_value() || level.value() < 0 || level.value() > 5)
				{
					Console.Error("Invalid -accblend level (expected 0=Minimum .. 5=Maximum)");
					return false;
				}
				Console.WriteLn(fmt::format("Forcing accurate blending unit = {}", level.value()));
				s_settings_interface.SetIntValue("EmuCore/GS", "accurate_blending_unit", level.value());
				continue;
			}
			else if (CHECK_ARG_PARAM("-emit-payload"))
			{
				s_payload_opts.output_path = StringUtil::StripWhitespace(argv[++i]);
				s_emit_payload = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("-payload-frames"))
			{
				s_payload_opts.frame_limit = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
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
					Console.Error("-payload-readback wants bp,bw,psm,w,h or bp,bw,psm,x,y,w,h");
					return false;
				}
				s_payload_opts.rb_bp = StringUtil::FromChars<u32>(parts[0]).value_or(0);
				s_payload_opts.rb_bw = StringUtil::FromChars<u32>(parts[1]).value_or(0);
				s_payload_opts.rb_psm = StringUtil::FromChars<u32>(parts[2]).value_or(0);
				if (parts.size() == 7)
				{
					s_payload_opts.rb_x = StringUtil::FromChars<u32>(parts[3]).value_or(0);
					s_payload_opts.rb_y = StringUtil::FromChars<u32>(parts[4]).value_or(0);
				}
				s_payload_opts.rb_w = StringUtil::FromChars<u32>(parts[parts.size() - 2]).value_or(0);
				s_payload_opts.rb_h = StringUtil::FromChars<u32>(parts[parts.size() - 1]).value_or(0);
				s_payload_opts.rb_explicit = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("-payload-ladder"))
			{
				s_payload_opts.ladder_every = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
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
					Console.Error("-ladder wants bp,bw,psm,x,y,w,h");
					return false;
				}
				s_ladder_opts.bp = StringUtil::FromChars<u32>(parts[0]).value_or(0);
				s_ladder_opts.bw = StringUtil::FromChars<u32>(parts[1]).value_or(0);
				s_ladder_opts.psm = StringUtil::FromChars<u32>(parts[2]).value_or(0);
				s_ladder_opts.x = StringUtil::FromChars<u32>(parts[3]).value_or(0);
				s_ladder_opts.y = StringUtil::FromChars<u32>(parts[4]).value_or(0);
				s_ladder_opts.w = StringUtil::FromChars<u32>(parts[5]).value_or(0);
				s_ladder_opts.h = StringUtil::FromChars<u32>(parts[6]).value_or(0);
				continue;
			}
			else if (CHECK_ARG_PARAM("-ladder-every"))
			{
				s_ladder_opts.every = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
				continue;
			}
			else if (CHECK_ARG_PARAM("-ladder-out"))
			{
				s_ladder_opts.output_path = StringUtil::StripWhitespace(argv[++i]);
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
			else if (argv[i][0] == '-')
			{
				Console.Error("Unknown parameter: '%s'", argv[i]);
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
		Console.Error("No dump filename provided.");
		return false;
	}

	if (!VMManager::IsGSDumpFileName(params.filename))
	{
		Console.Error("Provided filename is not a GS dump.");
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
	const double gs_cpu_us_per_draw = gs_cpu_draws ? (gs_cpu_ms_total * 1000.0 / static_cast<double>(gs_cpu_draws)) : 0.0;
	const double gs_cpu_us_per_draw_call = gs_cpu_draw_calls ? (gs_cpu_ms_total * 1000.0 / static_cast<double>(gs_cpu_draw_calls)) : 0.0;

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
	std::fprintf(fp.get(), "    \"frames\": %u,\n    \"drawn_frames\": %u,\n", s_total_frames, s_total_drawn_frames);
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
	std::fprintf(fp.get(), "    \"tile_payload_bytes\": %" PRIu64 ",\n", s_total_tile_payload_bytes);
	std::fprintf(fp.get(), "    \"tilegpu_depth_merged_frames\": %" PRIu64 ",\n", s_total_depth_merged_frames);
	std::fprintf(fp.get(), "    \"tilegpu_depth_policy_switches\": %" PRIu64 ",\n", s_total_depth_policy_switches);
	std::fprintf(fp.get(), "    \"tilegpu_depth_passes_saved\": %" PRIu64 ",\n", s_total_depth_passes_saved);
	std::fprintf(fp.get(), "    \"tilegpu_scissor_draws\": %" PRIu64 ",\n", s_total_scissor_draws);
	std::fprintf(fp.get(), "    \"tilegpu_scissor_cuts\": %" PRIu64 ",\n", s_total_scissor_cuts);
	std::fprintf(fp.get(), "    \"tilegpu_scissor_extra_calls\": %" PRIu64 ",\n", s_total_scissor_extra_calls);
	std::fprintf(fp.get(), "    \"tilegpu_dualsrc_draws\": %" PRIu64 ",\n", s_total_dualsrc_draws);
	std::fprintf(fp.get(), "    \"tilegpu_dualsrc_restore\": %" PRIu64 ",\n", s_total_dualsrc_restore);
	std::fprintf(fp.get(), "    \"tilegpu_dualsrc_companions\": %" PRIu64 ",\n", s_total_dualsrc_companions);
	std::fprintf(fp.get(), "    \"gpu_blocking_waits\": %" PRIu64 ",\n", s_total_gpu_blocking_waits);
	// The same population, split by cause, so an attribution round needs no teardown-ordering print
	// to survive. Wall time in nanoseconds because that is the unit the device counts in; a reader
	// that wants milliseconds can divide, and one that wants to sum two causes cannot un-round.
	std::fprintf(fp.get(), "    \"sync_wait_ns\": %" PRIu64 ",\n    \"sync_wait_calls\": %" PRIu64 ",\n",
		s_device_wait_bill.sync_ns, s_device_wait_bill.sync_calls);
	std::fprintf(fp.get(), "    \"oob_wait_ns\": %" PRIu64 ",\n    \"oob_wait_calls\": %" PRIu64 ",\n",
		s_device_wait_bill.oob_ns, s_device_wait_bill.oob_calls);
	std::fprintf(fp.get(), "    \"ring_wait_ns\": %" PRIu64 ",\n    \"ring_wait_calls\": %" PRIu64 ",\n",
		s_device_wait_bill.ring_ns, s_device_wait_bill.ring_calls);
	// gpu_blocking_waits == sync_wait_calls + oob_wait_calls + source_set_wait_calls. ring_wait_* is
	// the one class outside it.
	std::fprintf(fp.get(), "    \"source_set_wait_ns\": %" PRIu64 ",\n    \"source_set_wait_calls\": %" PRIu64 ",\n",
		s_device_wait_bill.source_set_ns, s_device_wait_bill.source_set_calls);
	std::fprintf(fp.get(), "    \"gs_cpu_ms\": %.3f,\n    \"gs_cpu_us_per_draw\": %.3f,\n    \"gs_cpu_us_per_draw_call\": %.3f,\n",
		gs_cpu_ms_total, gs_cpu_us_per_draw, gs_cpu_us_per_draw_call);
	std::fprintf(fp.get(), "    \"gs_cpu_us_per_draw_p50\": %.3f,\n    \"gs_cpu_us_per_draw_p95\": %.3f,\n",
		Percentile(gs_cpu_per_draw_us, 0.50), Percentile(gs_cpu_per_draw_us, 0.95));
	std::fprintf(fp.get(), "    \"gs_cpu_partial\": %s,\n", s_saw_gs_back_thread_in_stats ? "true" : "false");
	std::fprintf(fp.get(), "    \"frame_ms_p50\": %.3f,\n    \"frame_ms_p95\": %.3f,\n    \"frame_ms_p99\": %.3f,\n",
		Percentile(frame_times, 0.50), Percentile(frame_times, 0.95), Percentile(frame_times, 0.99));
	std::fprintf(fp.get(), "    \"frame_ms_worst\": %.3f,\n    \"frame_worst_index\": %u\n  },\n", worst_ms, worst_frame);

	std::fprintf(fp.get(), "  \"frames\": [\n");
	for (size_t i = 0; i < s_frame_samples.size(); i++)
	{
		const FrameSample& s = s_frame_samples[i];
		std::fprintf(fp.get(),
			"    {\"frame\":%u,\"idle\":%s,\"frame_ms\":%.3f,\"gpu_ms\":%.3f,\"gs_cpu_ms\":%.3f,"
			"\"prims\":%" PRIu64 ",\"draws\":%" PRIu64 ",\"draw_calls\":%" PRIu64 ","
			"\"render_passes\":%" PRIu64 ",\"render_pass_area_pixels\":%" PRIu64 ","
			"\"barriers\":%" PRIu64 ",\"copies\":%" PRIu64 ","
			"\"uploads\":%" PRIu64 ",\"readbacks\":%" PRIu64 ","
			"\"copies_rov\":%" PRIu64 ",\"draw_calls_rov\":%" PRIu64 ",\"barriers_rov\":%" PRIu64 ","
			"\"tc_source_hit\":%" PRIu64 ",\"tc_source_miss\":%" PRIu64 ","
			"\"tc_target_hit\":%" PRIu64 ",\"tc_target_miss\":%" PRIu64 ","
			"\"hash_cache_hit\":%" PRIu64 ",\"hash_cache_miss\":%" PRIu64 ","
			"\"pipeline_switches\":%" PRIu64 ",\"tile_payload_bytes\":%" PRIu64 ","
			"\"gpu_blocking_waits\":%" PRIu64 "}%s\n",
			s.frame, s.idle ? "true" : "false", s.frame_ms, s.gpu_ms, s.gs_cpu_ms,
			s.prims, s.draws, s.draw_calls,
			s.render_passes, s.render_pass_area_pixels, s.barriers, s.copies,
			s.uploads, s.readbacks,
			s.copies_rov, s.draw_calls_rov, s.barriers_rov,
			s.tc_source_hit, s.tc_source_miss,
			s.tc_target_hit, s.tc_target_miss,
			s.hash_cache_hit, s.hash_cache_miss,
			s.pipeline_switches, s.tile_payload_bytes,
			s.gpu_blocking_waits,
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
	Console.WriteLn(fmt::format("@HWSTAT@ Tile Payload Bytes: {} (avg {})", s_total_tile_payload_bytes, static_cast<u64>(std::ceil(s_total_tile_payload_bytes / static_cast<double>(s_total_drawn_frames)))));
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
		Console.WriteLn(fmt::format("@HWSTAT@ GPU Blocking Wait ms: {:.3f} (sync {:.3f} over {} + out-of-band {:.3f} "
									"over {} + source-set wrap {:.3f} over {}); ring backpressure {:.3f} ms over "
									"{} waits",
			(w.sync_ns + w.oob_ns + w.source_set_ns) / 1e6, w.sync_ns / 1e6, w.sync_calls, w.oob_ns / 1e6, w.oob_calls,
			w.source_set_ns / 1e6, w.source_set_calls, w.ring_ns / 1e6, w.ring_calls));
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
	}
	for (const std::string& line : s_extended_stats_snapshot)
		Console.WriteLn(fmt::format("@HWSTAT@ {}", line));
	Console.WriteLn("============================================");

	if (!s_stats_json_path.empty())
		WriteStatsJson(s_stats_json_path);

	if (!s_drawlog_path.empty())
		GSDrawLog::WriteCSV(s_drawlog_path);

	if (!s_tile_oracle_path.empty())
	{
		Console.WriteLn(fmt::format("GSTileOracle: {} draws compared, {} divergent{}", GSTileOracle::GetRowCount(),
			GSTileOracle::GetDivergentCount(), GSTileOracle::WasTruncated() ? " (TRUNCATED)" : ""));
		GSTileOracle::WriteCSV(s_tile_oracle_path);
	}
}

#ifdef _WIN32
// We can't handle unicode in filenames if we don't use wmain on Win32.
#define main real_main
#endif

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
			// run until end
			GSDumpReplayer::SetLoopCount(s_loop_count);
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

	// Clean SIGINT/SIGTERM → VM stop, so DumpStats() still fires on ^C or SIGTERM during -loop 0.
	// Defer the actual stop to the CPU thread (see s_signal_stop_requested).
	std::signal(SIGINT, [](int) { s_signal_stop_requested.store(true); });
	std::signal(SIGTERM, [](int) { s_signal_stop_requested.store(true); });

	if (!GSRunner::InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return EXIT_FAILURE;
	}

	VMBootParameters params;
	if (!GSRunner::ParseCommandLineArgs(argc, argv, params))
		return EXIT_FAILURE;

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
		Console.Error("Failed to create window.");
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
