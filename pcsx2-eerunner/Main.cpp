// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// pcsx2-eerunner — headless standalone full-system runner for EE (R5900)
// JIT-vs-interpreter divergence triage.
//
// Modeled closely on pcsx2-gsrunner: it does a full VMManager init on a
// dedicated CPU thread and provides the complete Host:: implementation surface
// a standalone binary needs to link against libpcsx2. Unlike gsrunner it runs
// fully headless (Null GS renderer, no window, synchronous GS, no audio) so the
// run is deterministic frame-to-frame.
//
// --selfcheck loads a savestate, runs N frames under the EE interpreter twice
// from the same savestate, and proves the two per-frame fingerprint streams are
// byte-identical. That run-to-run determinism is the gate before any
// JIT-vs-interp diff (--localize / --repro) is meaningful.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <map>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/Perf.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/DebugTools/Debug.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Hw.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/IopThread/IopThreadVm.h"
#include "pcsx2/IopThread/OwnerAsserts.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/EECycleRate.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/VUmicro.h"

// --statereport decode targets
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/Counters.h"
#include "pcsx2/Gif_Unit.h"
#include "pcsx2/IopCounters.h"
#include "pcsx2/MTVU.h"
#include "pcsx2/R3000A.h"
#include "pcsx2/Vif_Dma.h"

#include "pcsx2/ee_divtrace.h"

#if defined(ARCH_ARM64)
#include "pcsx2/arm64/iR5900-arm64.h"
#include "pcsx2/arm64/vuaot/VuAotLive.h"
#endif

#include "svnrev.h"

#ifdef __ANDROID__
// The core expects the frontend to provide these JNI bridges (native-lib.cpp
// does in the APK). A bare NDK executable has no JVM: the Java-backed paths
// (scoped-storage fallbacks, content:// fds, Java sound, pad rumble) cannot
// trigger under adb shell on plain filesystem paths, so they stub to failure.
//
// The declaring headers are included rather than the signatures re-typed, so a
// signature that drifts is a compile error here instead of an unresolved
// external.
#include "pcsx2/Input/AndroidNativeRumble.h"

namespace Common
{
	bool PlaySoundAsync(const char* path) { return false; }
}
namespace FileSystem
{
	int OpenFDFileContent(const char* filename) { return -1; }
	std::vector<std::string> FindContentChdSiblings(const char* filename) { return {}; }
	bool CreateDirectoryViaJava(const char* path) { return false; }
	bool CreateFileViaJava(const char* path) { return false; }
}
namespace Native
{
	void onPadRumble(int pad, int largeMotor, int smallMotor) {}
}

// Android renderer-Auto steering (GSUtil.cpp; the APK sets it from the
// GL_RENDERER string). Even the Null GS opens a host device API, and EGL fails
// under adb shell, so take Vulkan.
extern bool g_gs_android_prefer_vk;
static const bool s_android_prefer_vk_init = []() { g_gs_android_prefer_vk = true; return true; }();

// VMManager's Android thread placement mode (default 7, Performance Cores, set
// by the app's settings). 0 is the unpinned mode: no mode picks cores for the
// emu threads, matching the Linux build. Note that 0 sets every emu thread's
// mask to all processors, which widens a mask inherited from taskset; pin
// per thread once the VM is up.
extern int g_android_affinity_mode;
static const bool s_android_affinity_init = []() { g_android_affinity_mode = 0; return true; }();
#endif

namespace EERunner
{
	static void InitializeConsole();
	static bool InitializeConfig();
	static void SettingsOverride();
	static bool ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params);
} // namespace EERunner

enum class RunMode
{
	None,
	SelfCheck,
	Localize,
	Repro,
	StepDiff,
	Vu0Diff,
	ContMem,
	SpeedhackDiff,
	LiveRun,
	Disasm,
	StateReport,
	TwinDump,
	TwinCompare,
	DumpConfig,
	MkState,
};

static MemorySettingsInterface s_settings_interface;

// Parsed command-line state, read by the CPU thread.
static RunMode s_mode = RunMode::None;
static std::string s_iso_path;
static std::string s_savestate_path;
static uint32_t s_frames = 300;
static bool s_no_console = false;
static bool s_vu0_interp = false; // --vu0-interp: VU0 on its interpreter (every --contmem pass, --liverun)
static bool s_vu1_interp = false; // --vu1-interp: VU1 on its interpreter (--liverun)
static uint32_t s_vuaot_trip_at = 0; // --vuaot-trip-at N: set the vu-aot sticky-STATUS tripwire off after frame N (--liverun)
static uint32_t s_reload_state_at = 0; // --reload-state-at N: load the --savestate again after frame N (--liverun)
static GSRendererType s_renderer = GSRendererType::Null; // --renderer (Null default; vk for Intel/headless)
static bool s_renderer_explicit = false; // user passed --renderer (an explicit null is honored in liverun)
static std::string s_memdump_prefix; // --memdump <prefix>: write <prefix>.{interp,jit}.bin at the last frame
static bool s_perf_jitdump = false; // --perf-jitdump: emit Linux perf jitdump for `perf inject --jit` (profiling)

// --gsdump: headless GS-dump capture from a --liverun. A .gs dump is a recording of
// the GIF command stream, so replaying one in pcsx2-gsrunner exercises the GS with
// ZERO emulator in front of it — which is the only way to A/B the GS across two
// builds honestly. Measuring the GS inside a live run cannot work: the MTGS ring and
// the SW rasterizer's job queue both spin while waiting, so a build with a faster EE
// changes the GS threads' instruction counts without changing a single pixel of GS
// work. Capture once here, then replay the same dump on both builds.
static std::string s_gsdump_path;         // --gsdump <path.png>: dump basename (extension replaced)
static uint32_t s_gsdump_frames = 0;      // number of frames to record
static uint32_t s_gsdump_at = 30;         // --gsdump-at F: start recording after frame F
static std::string s_shots_prefix;         // --shots <prefix>: per-frame PNG basename
static uint32_t s_shots_every = 1;        // --shots-every N: one PNG every N frames
static uint32_t s_shots_from = 0;         // --shots-from F: first frame to capture

// twindiff (cross-BUILD divergence finder) state — see the big comment above
// RunTwinDump. The same byte-identical runner is built in two trees (working
// vs broken); one dumps a trace, the other compares against it.
static std::string s_twin_prefix;          // --twindump <prefix>: trace + ram-dump output prefix
static std::string s_twin_other;           // --twincompare <file>: the other build's .twin trace
static std::string s_twin_label = "local"; // --twin-label <name>: stamped into the trace header
static int s_twin_ram_at = -1;             // --twin-ram-at <frame>: raw EE RAM+scratch dump at this frame
static std::string s_dumpcfg_path;         // --dump-config <path>: effective-EmuConfig INI dump
static std::string s_mkstate_path;         // --mkstate <out.p2s>: write a savestate after --frames
struct SetOverride
{
	std::string section, key, value;
};
static std::vector<SetOverride> s_set_overrides; // --set Section/Key=Value (repeatable)
static u32 s_iop_stats_every = 0;      // --iop-stats-every N: in a --liverun, log the IOP thread's Threaded counters every N frames
static bool s_stress = false;          // --iop-stress <seed>: a --liverun that does random lifecycle things between frames
static u32 s_stress_seed = 1;
static u32 s_pause_toggle_at = 0;      // --pause-toggle N: in a --liverun, flip the IOP thread setting while the VM is paused after frame N
static bool s_threadperf_frontend = false; // --threadperf-frontend: @THREADPERF2@ front-end counters on the CPU thread
static u32 s_churn_every = 0;          // --settings-churn N <kind>: apply a settings change every N frames of a twin pass
static std::vector<bool> s_no_vm_vuaot_flips; // --no-vm-vuaot-flips 0,1,0: EnableVUAot set to each value in turn and the settings applied with no VM running
static std::string s_churn_kind;       // "lockstep": flip the IOP thread between Off and Lockstep; "threaded": between Off and Threaded; "lag": flip the IOP thread lag; "vuaot": flip EnableVUAot
static u32 s_rec_fallback_groups = 0; // --rec-fallback <groups>: EE opcode groups forced to interp
#if defined(ARCH_ARM64)
static u32 s_rec_fallback_reg_masks[EERecFallback::kCop2MoveOpCount] = {~0u, ~0u, ~0u, ~0u}; // per-COP2-move-op register filters
static u64 s_rec_fallback_fpu_mask[EERecFallback::kFpuIdCount / 64] = {~0ull, ~0ull}; // per-FPU-op filter
static u64 s_rec_fallback_vu_mask[EERecFallback::kCop2VuIdCount / 64] = {~0ull, ~0ull, ~0ull, ~0ull}; // per-VU-macro-op filter
#endif

// --ee-rate-script / --ee-rate-hitch
//
// Drives EECycleRate::ApplyEffective from the runner's CPU thread between Execute() calls, never from inside guest
// execution. That is the seam the governor uses, so it exercises the real transition path: store the effective
// selector, reset the EE recompiler and microVU0, and nothing else.
//
// It checks two things:
//   * a transition must not reset the IOP recompiler, microVU1 or the VIF unpack dynarec. None of them bake the
//     EE selector, and under MTVU a VU1 reset reaches across a thread. The run fails if any of their generation
//     counters moves.
//   * how long the rebuild costs. --ee-rate-hitch N reports the median of the N frames before each transition and
//     the wall time of the N frames after it.
//
// The script uses absolute effective selectors and is bound by the governor's window: [configured - 2, configured],
// floored at -3.

struct EeRateStep
{
	uint32_t frame;
	s8       selector;
};

static std::vector<EeRateStep> s_ee_rate_script; // --ee-rate-script, sorted by frame
static uint32_t s_ee_rate_hitch = 0;             // --ee-rate-hitch N (0 = no timing)
static bool s_ee_rate_failed = false;            // a transition reset something it must not
static std::string s_ee_rate_trace_path;         // --ee-rate-trace <path>

// Bounds-check the script against the configured baseline. Callable only once
// the VM is up, since the baseline is settled by GameDB + settings.
static bool ValidateEeRateScript()
{
	if (s_ee_rate_script.empty())
		return true;

	const s8 configured = EECycleRate::GetConfigured();
	const s8 floor = EECycleRate::GetFloor();
	bool ok = true;
	for (const EeRateStep& step : s_ee_rate_script)
	{
		if (step.selector > configured || step.selector < floor)
		{
			Console.ErrorFmt("--ee-rate-script: frame {} asks for selector {}, outside the allowed [{}, {}] "
			                 "for configured rate {}",
				step.frame, static_cast<int>(step.selector), static_cast<int>(floor),
				static_cast<int>(configured), static_cast<int>(configured));
			ok = false;
		}
	}
	if (ok)
	{
		Console.WriteLn(fmt::format("EERATE: script of {} step(s) accepted; configured {}, allowed [{}, {}]",
			s_ee_rate_script.size(), static_cast<int>(configured), static_cast<int>(floor),
			static_cast<int>(configured)));
	}
	return ok;
}

// End-of-run census of the EE recompiler's charge sites: the numbers the EERATE transition line carries, for the
// state the run finished in, so a run with no transitions still reports its working set and the fraction of it
// a patch pass could not repair in place.
static void ReportEeChargeCensus(const char* when)
{
#if defined(ARCH_ARM64)
	const u32 blocks = recEeGetLiveBlockCount();
	const u32 sites = recEeGetChargeSiteCount();
	const u32 fc_sites = recEeGetFormChangeSiteCount();
	const u32 fc_blocks = recEeGetFormChangeBlockCount();
	const double site_pct = (sites != 0) ? (100.0 * fc_sites / sites) : 0.0;
	const double block_pct = (blocks != 0) ? (100.0 * fc_blocks / blocks) : 0.0;

	Console.WriteLn(fmt::format(
		"EERATE-CENSUS[{}]: {} live blocks, {} charge sites ({:.2f} per block); "
		"{} sites ({:.2f}%) in {} blocks ({:.2f}%) change encoding form across "
		"[configured-2, configured]",
		when, blocks, sites, (blocks != 0) ? (static_cast<double>(sites) / blocks) : 0.0,
		fc_sites, site_pct, fc_blocks, block_pct));
#else
	(void)when;
#endif
}

// Per-pass driver. One instance per Execute() loop, so --contmem's interpreter
// and JIT passes each get the same script from the same starting selector.
class EeRateDriver
{
public:
	explicit EeRateDriver(std::string pass)
		: m_pass(std::move(pass))
	{
	}

	void BeforeFrame(uint32_t frame)
	{
		for (const EeRateStep& step : s_ee_rate_script)
		{
			if (step.frame != frame)
				continue;
			Apply(frame, step.selector);
		}
	}

	void AfterFrame(double ms)
	{
		if (s_ee_rate_hitch == 0)
			return;

		if (m_collecting > 0)
		{
			m_after.push_back(ms);
			if (--m_collecting == 0)
				ReportHitch();
		}

		m_recent.push_back(ms);
		if (m_recent.size() > s_ee_rate_hitch)
			m_recent.erase(m_recent.begin());
	}

private:
	void Apply(uint32_t frame, s8 selector)
	{
		const EECycleRate::ResetGenerations before = EECycleRate::GetResetGenerations();
		const s8 old = EECycleRate::GetEffective();

		// Read before the transition: the EE reset clears these counts.
#if defined(ARCH_ARM64)
		const u32 live_blocks = recEeGetLiveBlockCount();
		const u32 charge_sites = recEeGetChargeSiteCount();
		const u32 fc_sites = recEeGetFormChangeSiteCount();
		const u32 fc_blocks = recEeGetFormChangeBlockCount();
#else
		const u32 live_blocks = 0, charge_sites = 0, fc_sites = 0, fc_blocks = 0;
#endif

		const auto t0 = std::chrono::steady_clock::now();
		const bool ok = EECycleRate::ApplyEffective(selector, "eerunner --ee-rate-script");
		const double apply_ms =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		const EECycleRate::ResetGenerations after = EECycleRate::GetResetGenerations();

		// Report the Cpu->Reset() call and the EE rebuild separately: the EE reset defers when called mid-execute,
		// so they differ for the governor (they coincide here, between Execute() calls). The rebuild belongs to this
		// transition when its generation stamp matches the post-transition EE generation, else to an earlier reset.
		const EECycleRate::TransitionCost cost = EECycleRate::GetLastTransitionCost();
		const EECycleRate::TransitionPath path = EECycleRate::GetLastTransitionPath();
		const double tick_ms = 1000.0 / static_cast<double>(GetTickFrequency());
		const bool rebuild_is_ours = (cost.ee_rebuild_generation == after.ee) && (after.ee != before.ee);

		// A request for the selector already in force returns before the path is recorded, so `path` still
		// describes an earlier transition; don't print it.
		const std::string path_text = (old == selector)
			? std::string("EE path unchanged (the selector was already there)")
			: fmt::format("EE path {} ({}), patch pass {:.3f} ms, {} sites patched, {} orphaned, "
			              "{} blocks invalidated ({} unpatchable)",
				  path.patched ? "PATCHED" : "RESET", path.reason,
				  static_cast<double>(path.ticks) * tick_ms, path.sites, path.sites_orphaned,
				  path.blocks_invalidated, path.blocks_unpatchable);

		Console.WriteLn(fmt::format(
			"EERATE[{}] frame {}: effective {} -> {} ({} in {:.3f} ms); {}; "
			"split Cpu->Reset() call {:.3f} ms, EE rebuild {:.3f} ms ({}), mVUreset(VU0) {:.3f} ms; "
			"working set {} live blocks / {} charge sites, of which {} sites in {} blocks change "
			"encoding form across the window; "
			"generations ee {}->{} vu0 {}->{} iop {}->{} vu1 {}->{} vif {}->{}",
			m_pass, frame, static_cast<int>(old), static_cast<int>(selector),
			ok ? "applied" : "REJECTED", apply_ms, path_text,
			static_cast<double>(cost.cpu_reset) * tick_ms,
			static_cast<double>(cost.ee_rebuild) * tick_ms,
			rebuild_is_ours ? "this transition" : "deferred, an earlier reset",
			static_cast<double>(cost.vu0_reset) * tick_ms,
			live_blocks, charge_sites, fc_sites, fc_blocks,
			before.ee, after.ee, before.vu0, after.vu0, before.iop, after.iop,
			before.vu1, after.vu1, before.vif, after.vif));

		if (!ok)
		{
			Console.ErrorFmt("EERATE[{}] frame {}: ApplyEffective({}) refused — script out of bounds",
				m_pass, frame, static_cast<int>(selector));
			s_ee_rate_failed = true;
			return;
		}

		// A transition that reports PATCHED must not have reset the EE recompiler. If the generation moved anyway,
		// the patch pass did not replace the reset and the timings are of the reset path.
		if (old != selector && path.patched && after.ee != before.ee)
		{
			Console.ErrorFmt("EERATE[{}] frame {}: the transition reported PATCHED but the EE recompiler "
			                 "generation still moved ({} -> {})",
				m_pass, frame, before.ee, after.ee);
			s_ee_rate_failed = true;
		}

		if (after.iop != before.iop || after.vu1 != before.vu1 || after.vif != before.vif)
		{
			Console.ErrorFmt("EERATE[{}] frame {}: a rate transition reset a provider it must not "
			                 "(iop {}->{}, vu1 {}->{}, vif {}->{})",
				m_pass, frame, before.iop, after.iop, before.vu1, after.vu1, before.vif, after.vif);
			s_ee_rate_failed = true;
		}

		if (s_ee_rate_hitch != 0 && old != selector)
		{
			m_median_before = Median(m_recent);
			m_have_median = !m_recent.empty();
			m_after.clear();
			m_collecting = static_cast<int>(s_ee_rate_hitch);
			m_hitch_frame = frame;
			m_hitch_from = old;
			m_hitch_to = selector;
		}
	}

	static double Median(std::vector<double> v)
	{
		if (v.empty())
			return 0.0;
		std::sort(v.begin(), v.end());
		const size_t n = v.size();
		return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
	}

	void ReportHitch()
	{
		if (!m_have_median)
		{
			Console.WriteLn(fmt::format("EERATE-HITCH[{}] frame {}: no preceding frames to take a median from",
				m_pass, m_hitch_frame));
			return;
		}

		double worst = 0.0;
		size_t worst_idx = 0;
		std::string cells;
		for (size_t i = 0; i < m_after.size(); i++)
		{
			const double excess = m_after[i] - m_median_before;
			if (excess > worst)
			{
				worst = excess;
				worst_idx = i;
			}
			cells += fmt::format(" {:.2f}", m_after[i]);
		}

		Console.WriteLn(fmt::format(
			"EERATE-HITCH[{}] frame {} ({} -> {}): median of preceding {} frames {:.2f} ms; "
			"next {} frames (ms):{}",
			m_pass, m_hitch_frame, static_cast<int>(m_hitch_from), static_cast<int>(m_hitch_to),
			m_recent.size(), m_median_before, m_after.size(), cells));
		Console.WriteLn(fmt::format(
			"EERATE-HITCH[{}] frame {}: worst excess {:.2f} ms at +{} (first frame excess {:.2f} ms)",
			m_pass, m_hitch_frame, worst, worst_idx,
			m_after.empty() ? 0.0 : m_after[0] - m_median_before));
	}

	std::string         m_pass;
	std::vector<double> m_recent;
	std::vector<double> m_after;
	int                 m_collecting = 0;
	double              m_median_before = 0.0;
	bool                m_have_median = false;
	uint32_t            m_hitch_frame = 0;
	s8                  m_hitch_from = 0;
	s8                  m_hitch_to = 0;
};

// Wall time of one Execute() call, in milliseconds.
struct EeRateFrameTimer
{
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	double Elapsed() const
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}
};

bool EERunner::InitializeConfig()
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

	// don't provide an ini path, or bother loading. settings are stored entirely in memory.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);

	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void Host::CommitBaseSettingChanges()
{
	// nothing to save, settings are entirely in memory
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
	// Headless — never present anything.
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	return wi;
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
	// Headless — the Null renderer doesn't need a surface.
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	// Headless — nothing to present.
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

// Work posted for the CPU thread (a hotkey that defers its action, a vibration the IOP thread hands over in Threaded
// mode). It runs from PumpMessagesOnCPUThread(), which the CPU thread calls at every vsync. A blocking post from
// the CPU thread itself runs at once, and one from another thread waits for the drain.
static std::mutex s_cpu_thread_tasks_mutex;
static std::condition_variable s_cpu_thread_tasks_done;
static std::deque<std::function<void()>> s_cpu_thread_tasks;

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	BLOCKING_HOST_CALL_ASSERT(block);
	if (block && VMManager::Internal::IsOnCPUThread())
	{
		function();
		return;
	}

	std::unique_lock lock(s_cpu_thread_tasks_mutex);
	s_cpu_thread_tasks.push_back(std::move(function));
	if (block)
		s_cpu_thread_tasks_done.wait(lock, [] { return s_cpu_thread_tasks.empty(); });
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

void Host::OnCoverDownloaderOpenRequested()
{
	// noop
}

void Host::OnCreateMemoryCardOpenRequested()
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

static void ApplySettingsChurn();
static bool s_churn_pending = false; // a settings change waits for the next vsync (the CPU thread pumps messages there)
static u32 s_change_disc_at = 0; // --change-disc N <image>: swap the disc at frame N of a liverun
static std::string s_change_disc_path;
static bool s_change_disc_pending = false; // the swap waits for the next vsync, where a player's menu would deliver it

// --host-action N <hotkey>: press and release a hotkey at frame N of a liverun, from the CPU thread, as the input
// manager does for a bound key. Several may be given. The hotkey's own handler runs, so the holds it takes are the
// ones a player's key press takes.
struct HostAction
{
	u32 frame;
	std::string hotkey;
};
static std::vector<HostAction> s_host_actions;
static std::vector<std::string> s_host_actions_pending; // delivered at the next vsync

// Work posted with Host::RunOnCPUThread. Tasks run outside the lock, so one that posts more cannot deadlock.
static void DrainCpuThreadTasks()
{
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
}

void Host::PumpMessagesOnCPUThread()
{
	// Headless: no platform message pump, apart from the --settings-churn change, delivered here as a settings
	// dialog would, from inside the EE's event test while the VM runs.
	if (s_churn_pending)
	{
		s_churn_pending = false;
		ApplySettingsChurn();
	}

	// --change-disc: what the frontend's "change disc" menu does, from the CPU thread, mid-run.
	if (s_change_disc_pending)
	{
		s_change_disc_pending = false;
		const bool ok = VMManager::ChangeDisc(CDVD_SourceType::Iso, s_change_disc_path);
		Console.WriteLn(fmt::format("eerunner: changed the disc to '{}': {}", s_change_disc_path, ok ? "ok" : "failed"));
	}

	if (!s_host_actions_pending.empty())
	{
		const std::vector<std::string> pending = std::exchange(s_host_actions_pending, {});

		// A liverun advances one frame at a time, so the VM is Paused whenever this pump runs, and a paused VM holds the
		// IOP for the pause. A key pressed in a running VM has no such hold, and the holds the hotkeys take for themselves
		// are the thing under test, so the pause hold is let go for the length of the actions.
		const bool paused = (VMManager::GetState() == VMState::Paused);
		if (paused)
			IopThread::OnVmPaused(false);
		for (const std::string& name : pending)
		{
			bool found = false;
			for (const HotkeyInfo* hotkey : InputManager::GetHotkeyList())
			{
				if (hotkey->name && name == hotkey->name)
				{
					hotkey->handler(1);
					hotkey->handler(0);
					found = true;
					break;
				}
			}
			Console.WriteLn(fmt::format("eerunner: hotkey '{}': {}", name, found ? "pressed" : "no such hotkey"));
		}
		// A hotkey that defers its action (the memory card swap) posted it; it runs here, still without the pause hold.
		DrainCpuThreadTasks();
		if (paused)
			IopThread::OnVmPaused(true);
	}

	DrainCpuThreadTasks();
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

static void PrintCommandLineVersion()
{
	std::fprintf(stderr, "PCSX2 EE Runner Version %s\n", GIT_REV);
	std::fprintf(stderr, "https://pcsx2.net/\n");
	std::fprintf(stderr, "\n");
}

static void PrintCommandLineHelp(const char* progname)
{
	PrintCommandLineVersion();
	std::fprintf(stderr, "Usage: %s [--stepdiff|--contmem|--liverun|--vu0diff|--localize|--repro|--selfcheck] --savestate <file> --frames N [--iso <file>] [<iso>]\n", progname);
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "  --stepdiff:  Checkpoint-anchored interp-vs-JIT diff (THE primary mode). Per frame, checkpoints\n");
	std::fprintf(stderr, "               the VM and runs one frame interp-twice + JIT-once from the IDENTICAL state, so a\n");
	std::fprintf(stderr, "               clean interp control + JIT divergence = a real EE JIT bug; then zooms to the block.\n");
	std::fprintf(stderr, "  --contmem:   Continuous-trajectory memory diff. Runs interp CONTINUOUSLY (x2, control) + JIT\n");
	std::fprintf(stderr, "               CONTINUOUSLY, diffs per-frame memory hashes. Catches ACCUMULATION bugs --stepdiff\n");
	std::fprintf(stderr, "               can't (it re-anchors to golden each frame). Add --vu0-interp to force VU0=interp in\n");
	std::fprintf(stderr, "               all passes (isolate EE-FPU/integer from VU0/COP2). Run on x86 too for cross-arch.\n");
	std::fprintf(stderr, "  --speedhack-diff: Speedhack-misfire differential. EE-jit throughout; baseline = all transparency-class\n");
	std::fprintf(stderr, "               speedhacks OFF (run twice for the determinism floor), then sweeps each speedhack on its\n");
	std::fprintf(stderr, "               own (WaitLoop/IntcStat/vuFlagHack/vu1Instant/fastCDVD) + all-on. A speedhack that claims\n");
	std::fprintf(stderr, "               to be architecturally transparent must NOT change the EE-RAM trajectory before the\n");
	std::fprintf(stderr, "               baseline control floor breaks; the first such divergence (with ReportMemDiff at it) is a\n");
	std::fprintf(stderr, "               misfire (e.g. the Burnout-3 WaitLoop timeout-loop skip). Diffs EE main RAM + scratchpad.\n");
	std::fprintf(stderr, "  --liverun:   Reproduce the in-game HANG headlessly: single EE-jit pass with the LIVE subsystems\n");
	std::fprintf(stderr, "               the diff modes suppress (real GS so GIF is consumed, MTVU on). A 10s no-frame-\n");
	std::fprintf(stderr, "               progress watchdog samples the live EE PC to fingerprint the spin loop, then exits 42.\n");
	std::fprintf(stderr, "  --vu0diff:   Pin EE interp in both passes, toggle only VU0 micro engine; diff COP2 read-streams.\n");
	std::fprintf(stderr, "  --localize / --repro: aliases of --stepdiff (the old jittery frame-boundary funnel was removed).\n");
	std::fprintf(stderr, "  --selfcheck: Characterize run-to-run determinism (interp only). Expected to flag benign ~10-cycle\n");
	std::fprintf(stderr, "               pause-point sampling jitter; use it to understand the noise floor, not as a gate.\n");
	std::fprintf(stderr, "  --rec-fallback <groups>: comma-separated EE opcode groups to force through the INTERPRETER\n");
	std::fprintf(stderr, "               instead of native codegen, with everything else still JIT. Bisects \"the EE JIT\n");
	std::fprintf(stderr, "               miscomputes something\" down to one emitter family without a rebuild per hypothesis:\n");
	std::fprintf(stderr, "               the group that makes the symptom disappear contains the bug. Groups: fpu, cop2,\n");
	std::fprintf(stderr, "               mmi, multdiv, shift, arith, loadstore, move, cop0, branch (plus all / none).\n");
	std::fprintf(stderr, "               cop2 narrows further into cop2move (QMFC2/CFC2/QMTC2/CTC2), cop2vu (the VU\n");
	std::fprintf(stderr, "               macro ops) and cop2ls (LQC2/SQC2). The four move ops take a ':<reg>' filter\n");
	std::fprintf(stderr, "               (ctc2:27, ctc2:16:21); cop2vu takes a ':<mnemonic>' one (cop2vu:vmaddaw,\n");
	std::fprintf(stderr, "               cop2vu:vmulax:vmaddaz) — cop2vu is ~100 emitters, so don't stop the funnel\n");
	std::fprintf(stderr, "               there. --mkstate prints a COP2VU CENSUS of the macro ops actually compiled;\n");
	std::fprintf(stderr, "               bisect over that list, since an op never compiled cannot be the bug.\n");
	std::fprintf(stderr, "               Pairs well with --mkstate --renderer sw for a visual oracle. arm64 only.\n");
	std::fprintf(stderr, "  --vu0-interp / --vu1-interp: run that VU on its interpreter with everything else\n");
	std::fprintf(stderr, "               left on the JIT. Under --contmem, --vu0-interp applies to every pass. Under\n");
	std::fprintf(stderr, "               --liverun, the pair attributes a trajectory move to one VU engine; turn MTVU\n");
	std::fprintf(stderr, "               off with EERUNNER_MTVU=0 when interpreting VU1.\n");
	std::fprintf(stderr, "  --gsdump <path>[:<frames>]: with --liverun, record a .gs dump of the GIF stream (default 1\n");
	std::fprintf(stderr, "               frame) starting after --gsdump-at frames (default 30, so the scene has settled).\n");
	std::fprintf(stderr, "               The dump is written as <path>.gs.zip: the .gs.zst, its driver report and a screenshot.\n");
	std::fprintf(stderr, "               Replaying that dump in pcsx2-gsrunner is the only honest way to A/B the GS across\n");
	std::fprintf(stderr, "               builds: it drives the GS with no emulator in front of it, so the MTGS ring and the\n");
	std::fprintf(stderr, "               SW job queue can't spin-wait a faster EE into a bogus GS instruction-count delta.\n");
	std::fprintf(stderr, "  --gsdump-at <frame>: frame after which --gsdump starts recording (default 30).\n");
	std::fprintf(stderr, "  --shots <prefix> [--shots-every N] [--shots-from F]: with --liverun and a real renderer,\n");
	std::fprintf(stderr, "               write <prefix>fNNNNN.png every N frames from frame F. A visual oracle for a\n");
	std::fprintf(stderr, "               bug whose only symptom is on screen: the PNG series is what a headless run\n");
	std::fprintf(stderr, "               can be judged on.\n");
	std::fprintf(stderr, "  --renderer <null|vk|ogl|sw>: GS renderer (default null). Use vk on Intel GPUs / boxes where\n");
	std::fprintf(stderr, "               the auto-check declines Vulkan and the surfaceless GL path fails to open GS.\n");
	std::fprintf(stderr, "  --statereport: load the savestate, print a field-level decode of every serialized timebase\n");
	std::fprintf(stderr, "               (EE cycle/COP0 Count pair, rcnt bases, vsync phase, EE<->IOP skew, IOP counters,\n");
	std::fprintf(stderr, "               CDVD RTC, GIF paths, VIF, DMA regs, MTVU) and exit. Diff two states with:\n");
	std::fprintf(stderr, "               diff <(... --statereport --savestate A) <(... --statereport --savestate B)\n");
	std::fprintf(stderr, "  --ee-rate-script <frame:selector,...>: apply ABSOLUTE effective EE cycle-rate selectors at\n");
	std::fprintf(stderr, "               the listed outer frame boundaries, through the production ApplyEffective path\n");
	std::fprintf(stderr, "               (store, then reset the EE recompiler and microVU0 only). Bounded by the same\n");
	std::fprintf(stderr, "               window the governor uses: [configured-2, configured], floored at -3. Applied to\n");
	std::fprintf(stderr, "               every pass of --contmem, so the interpreter and JIT trajectories are compared\n");
	std::fprintf(stderr, "               under the same rate schedule. The run FAILS if a transition resets the IOP\n");
	std::fprintf(stderr, "               recompiler, microVU1 or the VIF dynarec.\n");
	std::fprintf(stderr, "  --ee-rate-trace <path>: set EmuCore/Speedhacks/DynamicEECycleRateTrace for the run, so the\n");
	std::fprintf(stderr, "               governor's window trace and its lifecycle rows are written. The runner does not\n");
	std::fprintf(stderr, "               pace - the limiter is off and frames come from FrameAdvance - so every window is\n");
	std::fprintf(stderr, "               recorded as unmeasurable, which is correct and is what the trace will say. Use it\n");
	std::fprintf(stderr, "               to exercise the writer and the lifecycle path, not to judge the classifier.\n");
	std::fprintf(stderr, "  --ee-rate-hitch <N>: with --ee-rate-script, report the median wall time of the N frames\n");
	std::fprintf(stderr, "               before each transition and the wall time of the N frames after it, so the\n");
	std::fprintf(stderr, "               recompile hitch is a number rather than a guess.\n");
	std::fprintf(stderr, "  --threadperf-frontend: in a --liverun, also count L1 instruction cache and TLB refills, front-end stalls and\n");
	std::fprintf(stderr, "               branch mispredictions on the CPU thread (@THREADPERF2@).\n");
	std::fprintf(stderr, "  --savestate <file>: Savestate to load after Initialize (required).\n");
	std::fprintf(stderr, "  --frames N: Number of frames to run (default 300).\n");
	std::fprintf(stderr, "  --iso <file>: Game ISO/disc to mount (required so the savestate has its disc).\n");
	std::fprintf(stderr, "  --perf-jitdump: Emit a Linux perf jitdump (under EmuFolders::Cache) so `perf inject --jit`\n");
	std::fprintf(stderr, "               resolves EE_/VU0_/VU1_/IOP_/VIF_ JIT block symbols. Profiling only; with --liverun it\n");
	std::fprintf(stderr, "               also honors an explicit --renderer null. Requires a USE_PERF_JITDUMP build.\n");
	std::fprintf(stderr, "  --twindump <prefix>: twindiff golden side. Runs full-interp x2 (determinism control) + full-JIT\n");
	std::fprintf(stderr, "               free-running passes from the savestate, recording per-frame memhash/regfp/cycle to\n");
	std::fprintf(stderr, "               <prefix>.twin for a DIFFERENT build of this runner to compare against.\n");
	std::fprintf(stderr, "  --twincompare <file.twin>: twindiff test side. Runs the same three passes locally and diffs them\n");
	std::fprintf(stderr, "               against the other build's trace: cross-interp = shared-C++ drift; jit-vs-interp per\n");
	std::fprintf(stderr, "               tree; jit-vs-jit = end-to-end working-vs-broken. Exit 2 on real divergence.\n");
	std::fprintf(stderr, "  --twin-label <name>: label stamped into the .twin header (e.g. armsx2-master / jit-transplant).\n");
	std::fprintf(stderr, "  --twin-ram-at <frame>: also dump raw EE RAM+scratch at that frame (interp + jit passes) to\n");
	std::fprintf(stderr, "               <prefix>.f<N>.{interp,jit}.bin, for offline page/region diffing across builds.\n");
	std::fprintf(stderr, "  --dump-config <path>: boot far enough for GameDB to apply, write the EFFECTIVE EmuConfig INI,\n");
	std::fprintf(stderr, "               and exit. Diff the two trees' dumps FIRST - config drift is the cheapest bug class.\n");
	std::fprintf(stderr, "  --mkstate <out.p2s>: run --frames under the production JIT (from --savestate if given, else a\n");
	std::fprintf(stderr, "               cold disc boot) and write a savestate. Bootstrap twindiff states on the WORKING\n");
	std::fprintf(stderr, "               binary, positioned BEFORE the corrupting computation (e.g. before a level load).\n");
	std::fprintf(stderr, "  --settings-churn N <lockstep|threaded|lag|vuaot>: in the twin passes and the --liverun, apply a settings change through\n");
	std::fprintf(stderr, "               VMManager::ApplySettings every N frames, as a player changing a setting mid-game would.\n");
	std::fprintf(stderr, "               'lockstep' flips the IOP thread between Off and Lockstep (a mode change: the thread\n");
	std::fprintf(stderr, "               starts or stops and the EE recompiler is reset); 'lag' flips only the IOP thread lag\n");
	std::fprintf(stderr, "               (the same recompiler resets, no mode change), the control for it. Each pass starts from\n");
	std::fprintf(stderr, "               the settings it began the run with. Both must leave the fingerprint unchanged.\n");
	std::fprintf(stderr, "  --no-vm-vuaot-flips 0,1,0: before the VM boots, set EnableVUAot to each value in turn and apply the settings with no\n");
	std::fprintf(stderr, "               VM running (the desktop settings window does the same with no game); on to off clears what vu-aot saved.\n");
	std::fprintf(stderr, "  --change-disc N <image>: in a --liverun, swap the disc for <image> at frame N, from the CPU thread, as the\n");
	std::fprintf(stderr, "               frontend's change-disc menu does.\n");
	std::fprintf(stderr, "  --host-action N <hotkey>: in a --liverun, press and release the hotkey <hotkey> (by its name in the hotkey\n");
	std::fprintf(stderr, "               list: Mute, IncreaseVolume, DecreaseVolume, ToggleTurbo, ToggleSlowMotion, SwapMemCards ...) at\n");
	std::fprintf(stderr, "               frame N, from the CPU thread, as the input manager does for a bound key, with the pause hold of the\n");
	std::fprintf(stderr, "               frame-by-frame liverun let go, as a running VM has none. Repeatable.\n");
	std::fprintf(stderr, "  --iop-assert-log: an IOP thread ownership assert (a Devel build's IOP_SIDE_ASSERT/EE_SIDE_ASSERT) logs\n");
	std::fprintf(stderr, "               the first violation at each site and the run goes on, instead of stopping at the first. The\n");
	std::fprintf(stderr, "               IOPTHREAD: line at the end of the run says how many there were.\n");
	std::fprintf(stderr, "  --iop-stress <seed>: in a --liverun, do things a player does between frames at random (seeded): switch the IOP thread\n");
	std::fprintf(stderr, "               between Off and Threaded and change its lag bound from inside the event test, save and load a state,\n");
	std::fprintf(stderr, "               pause for a while, reset the VM, press hotkeys. Every operation is logged as STRESS: lines.\n");
	std::fprintf(stderr, "  --pause-toggle N: in a --liverun, flip the IOP thread setting (Off to Threaded, or back) while the VM is paused after\n");
	std::fprintf(stderr, "               frame N, as a settings dialog does on a paused VM, and log the mode and the thread before and after as PAUSE-TOGGLE:.\n");
	std::fprintf(stderr, "  --iop-min-slice N: Threaded IOP thread: S_min, the budget in EE cycles the IOP lets build up before it runs a\n");
	std::fprintf(stderr, "               slice (design 5.3). Default 0 (run whenever there is budget). A measurement knob, not a setting.\n");
	std::fprintf(stderr, "  --iop-spin-hint yield|isb: the instruction the Threaded spin loops use between looks (default yield).\n");
	std::fprintf(stderr, "  --iop-idle-spin-us N: Threaded IOP thread: how long an idle IOP watches the watermark before it sleeps, in\n");
	std::fprintf(stderr, "               microseconds (0 sleeps at once). Default: the built-in window. A measurement knob, not a setting.\n");
	std::fprintf(stderr, "  --iop-stats-every N: in a --liverun, log the Threaded IOP counters per frame every N frames. The IOPSTATS: line\n");
	std::fprintf(stderr, "               at the end of the run gives the whole-run figures.\n");
	std::fprintf(stderr, "  --set Section/Key=Value: base-layer settings override, applied after the harness pinning\n");
	std::fprintf(stderr, "               (repeatable). E.g. --set EmuCore/CPU/Recompiler/fpuFullMode=true. GameDB\n");
	std::fprintf(stderr, "               per-game keys still win; the twin passes own the EnableEE/VU0/VU1/IOP toggles.\n");
	std::fprintf(stderr, "  -help: Displays this information and exits.\n");
	std::fprintf(stderr, "  -version: Displays version information and exits.\n");
	std::fprintf(stderr, "\n");
}

void EERunner::InitializeConsole()
{
	const char* var = std::getenv("PCSX2_NOCONSOLE");
	s_no_console = (var && StringUtil::FromChars<bool>(var).value_or(false));
	if (!s_no_console)
		Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
}

bool EERunner::ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params)
{
	bool no_more_args = false;
	for (int i = 1; i < argc; i++)
	{
		if (!no_more_args)
		{
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

			if (CHECK_ARG("-help") || CHECK_ARG("--help"))
			{
				PrintCommandLineHelp(argv[0]);
				return false;
			}
			else if (CHECK_ARG("-version") || CHECK_ARG("--version"))
			{
				PrintCommandLineVersion();
				return false;
			}
			else if (CHECK_ARG("--selfcheck"))
			{
				s_mode = RunMode::SelfCheck;
				continue;
			}
			else if (CHECK_ARG("--localize"))
			{
				s_mode = RunMode::Localize;
				continue;
			}
			else if (CHECK_ARG("--repro"))
			{
				s_mode = RunMode::Repro;
				continue;
			}
			else if (CHECK_ARG("--stepdiff"))
			{
				s_mode = RunMode::StepDiff;
				continue;
			}
			else if (CHECK_ARG("--vu0diff"))
			{
				s_mode = RunMode::Vu0Diff;
				continue;
			}
			else if (CHECK_ARG("--contmem"))
			{
				s_mode = RunMode::ContMem;
				continue;
			}
			else if (CHECK_ARG("--speedhack-diff"))
			{
				s_mode = RunMode::SpeedhackDiff;
				continue;
			}
			else if (CHECK_ARG("--liverun"))
			{
				s_mode = RunMode::LiveRun;
				continue;
			}
			else if (CHECK_ARG("--threadperf-frontend"))
			{
				s_threadperf_frontend = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("--gsdump"))
			{
				const std::string_view spec(argv[++i]);
				const std::string_view::size_type at = spec.rfind(':');
				std::string_view path = spec;
				s_gsdump_frames = 1;
				// "<path>:<frames>" — the colon is optional, and a Windows-style
				// drive letter can't appear here, so rfind is unambiguous.
				if (at != std::string_view::npos)
				{
					if (const std::optional<u32> n = StringUtil::FromChars<u32>(spec.substr(at + 1), 10))
					{
						s_gsdump_frames = std::max<u32>(1, n.value());
						path = spec.substr(0, at);
					}
				}
				s_gsdump_path = path;
				// QueueSnapshot only honours a caller-supplied path when it ends in
				// ".png" (it strips the extension and appends the dump's own).
				if (!StringUtil::EndsWithNoCase(s_gsdump_path, ".png"))
					s_gsdump_path += ".png";
				continue;
			}
			else if (CHECK_ARG_PARAM("--gsdump-at"))
			{
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value())
				{
					Console.Error("--gsdump-at expects a frame number.");
					return false;
				}
				s_gsdump_at = n.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("--shots"))
			{
				// Written from the GS thread at the point --gsdump arms, so the
				// frame numbering is the one the run reports.
				s_shots_prefix = argv[++i];
				continue;
			}
			else if (CHECK_ARG_PARAM("--shots-every"))
			{
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value() || n.value() == 0)
				{
					Console.Error("--shots-every expects a frame count above zero.");
					return false;
				}
				s_shots_every = n.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("--shots-from"))
			{
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value())
				{
					Console.Error("--shots-from expects a frame number.");
					return false;
				}
				s_shots_from = n.value();
				continue;
			}
			else if (CHECK_ARG("--perf-jitdump"))
			{
				// Emit a Linux perf jitdump so `perf inject --jit` can resolve EE_/VU1_/...
				// JIT block symbols. Profiling only; honors explicit --renderer null (the
				// liverun null->VK force below is skipped when this is set). Requires a
				// USE_PERF_JITDUMP build (no-op otherwise).
				s_perf_jitdump = true;
				continue;
			}
			else if (CHECK_ARG("--disasm"))
			{
				// --disasm: load the savestate, then disassemble EE code in
				// [EERUNNER_DIS_LO, EERUNNER_DIS_HI] with the correct R5900 disassembler
				// (generic MIPS disassemblers garble R5900 COP2/MMI opcodes). No run.
				s_mode = RunMode::Disasm;
				continue;
			}
			else if (CHECK_ARG("--statereport"))
			{
				// --statereport: load the savestate and print a field-level decode of
				// every serialized timebase and transfer-engine state. No run.
				s_mode = RunMode::StateReport;
				continue;
			}
			else if (CHECK_ARG("--vu0-interp"))
			{
				s_vu0_interp = true;
				continue;
			}
			else if (CHECK_ARG("--vu1-interp"))
			{
				s_vu1_interp = true;
				continue;
			}
			else if (CHECK_ARG("--count-vu1-ins"))
			{
				// Count the user-mode instructions the thread that runs VU1 retires inside
				// its dispatch (recMicroVU1::Execute); printed as @VU1INS@ at the end of a
				// --liverun. Pin the process to the performance cores of a hybrid CPU.
#if defined(ARCH_ARM64) && defined(PCSX2_RECOMPILER_TESTS)
				mvu_test_hooks::EnableVu1InstructionCount(true);
#else
				Console.Warning("--count-vu1-ins needs an arm64 build with the recompiler test hooks.");
#endif
				continue;
			}
			else if (CHECK_ARG_PARAM("--vuaot-threshold"))
			{
				// With EmuCore/CPU/Recompiler/EnableVUAot on (--set), tier VU1
				// programs up after N dispatches instead of the shipped constant:
				// a low N puts almost every program through vu-aot for an identity run.
#if defined(ARCH_ARM64) && defined(PCSX2_RECOMPILER_TESTS)
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value() || n.value() == 0)
				{
					Console.Error("--vuaot-threshold expects a run count above zero.");
					return false;
				}
				vuaot::live::SetTierUpRuns(n.value());
#else
				++i;
				Console.Warning("--vuaot-threshold needs an arm64 build with the recompiler test hooks.");
#endif
				continue;
			}
			else if (CHECK_ARG_PARAM("--vuaot-jump-threshold"))
			{
				// ... and after N JR/JALR slow-path entries (the programs a stub reaches only by
				// jumping) instead of the shipped constant.
#if defined(ARCH_ARM64) && defined(PCSX2_RECOMPILER_TESTS)
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value() || n.value() == 0)
				{
					Console.Error("--vuaot-jump-threshold expects a count above zero.");
					return false;
				}
				vuaot::live::SetJumpTierUpRuns(n.value());
#else
				++i;
				Console.Warning("--vuaot-jump-threshold needs an arm64 build with the recompiler test hooks.");
#endif
				continue;
			}
			else if (CHECK_ARG_PARAM("--vuaot-trip-at"))
			{
				// Sets the tier's sticky-STATUS tripwire off after frame N of a --liverun: the regions that
				// dropped the sticky byte are rebuilt in the middle of a game, which is what the trip does.
#if defined(ARCH_ARM64) && defined(PCSX2_RECOMPILER_TESTS)
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value() || n.value() == 0)
				{
					Console.Error("--vuaot-trip-at expects a frame number above zero.");
					return false;
				}
				s_vuaot_trip_at = n.value();
#else
				++i;
				Console.Warning("--vuaot-trip-at needs an arm64 build with the recompiler test hooks.");
#endif
				continue;
			}
			else if (CHECK_ARG_PARAM("--vuaot-worker-delay-us"))
			{
				// The vu-aot worker sleeps this long before each job: publication lags the
				// request as it does on a loaded or slow machine.
#if defined(ARCH_ARM64) && defined(PCSX2_RECOMPILER_TESTS)
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value())
				{
					Console.Error("--vuaot-worker-delay-us expects a number of microseconds.");
					return false;
				}
				vuaot::live::SetWorkerDelayUs(n.value());
#else
				++i;
				Console.Warning("--vuaot-worker-delay-us needs an arm64 build with the recompiler test hooks.");
#endif
				continue;
			}
			else if (CHECK_ARG("--vuaot-sync"))
			{
				// The vu-aot tier's deterministic mode: every background compile is waited
				// for and published where it was asked for (the synchronous tier's timing),
				// so two runs compile the same things at the same dispatches.
#if defined(ARCH_ARM64) && defined(PCSX2_RECOMPILER_TESTS)
				vuaot::live::SetSynchronous(true);
#else
				Console.Warning("--vuaot-sync needs an arm64 build with the recompiler test hooks.");
#endif
				continue;
			}
			else if (CHECK_ARG("--vuaot-no-install"))
			{
				// Loaded vu-aot sidecars are used as plans only: no saved region code is installed, the
				// worker compiles every region again (the plan-only half, for A/B against the install).
#if defined(ARCH_ARM64) && defined(PCSX2_RECOMPILER_TESTS)
				vuaot::live::SetInstallEnabled(false);
#else
				Console.Warning("--vuaot-no-install needs an arm64 build with the recompiler test hooks.");
#endif
				continue;
			}
			else if (CHECK_ARG_PARAM("--memdump"))
			{
				s_memdump_prefix = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("--twindump"))
			{
				// Standalone = golden-dump mode; alongside --twincompare it only
				// names the local trace/ram-dump prefix.
				if (s_mode != RunMode::TwinCompare)
					s_mode = RunMode::TwinDump;
				s_twin_prefix = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("--twincompare"))
			{
				s_mode = RunMode::TwinCompare;
				s_twin_other = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("--twin-label"))
			{
				s_twin_label = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("--twin-ram-at"))
			{
				const auto v = StringUtil::FromChars<uint32_t>(argv[++i]);
				if (!v.has_value())
				{
					Console.Error("Invalid --twin-ram-at value.");
					return false;
				}
				s_twin_ram_at = static_cast<int>(v.value());
				continue;
			}
			else if (CHECK_ARG_PARAM("--dump-config"))
			{
				s_mode = RunMode::DumpConfig;
				s_dumpcfg_path = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("--mkstate"))
			{
				s_mode = RunMode::MkState;
				s_mkstate_path = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("--set"))
			{
				const std::string kv(StringUtil::StripWhitespace(argv[++i]));
				const size_t eq = kv.find('=');
				const size_t slash = (eq == std::string::npos) ? std::string::npos : kv.rfind('/', eq);
				if (eq == std::string::npos || slash == std::string::npos || slash == 0 || slash + 1 >= eq)
				{
					Console.Error("--set expects Section/Key=Value (e.g. EmuCore/CPU/Recompiler/fpuFullMode=true).");
					return false;
				}
				s_set_overrides.push_back({kv.substr(0, slash), kv.substr(slash + 1, eq - slash - 1), kv.substr(eq + 1)});
				continue;
			}
			else if (CHECK_ARG("--settings-churn") && (i + 2) < argc)
			{
				const std::string_view n = StringUtil::StripWhitespace(argv[++i]);
				s_churn_kind = StringUtil::StripWhitespace(argv[++i]);
				const std::optional<u32> every = StringUtil::FromChars<u32>(n, 10);
				if (!every.has_value() || every.value() == 0 || (s_churn_kind != "lockstep" && s_churn_kind != "threaded" && s_churn_kind != "lag" && s_churn_kind != "vuaot"))
				{
					Console.Error("--settings-churn expects N (frames) and a kind: lockstep, threaded, lag or vuaot.");
					return false;
				}
				s_churn_every = every.value();
				continue;
			}
			else if (CHECK_ARG("--change-disc") && (i + 2) < argc)
			{
				const std::optional<u32> at = StringUtil::FromChars<u32>(StringUtil::StripWhitespace(argv[++i]), 10);
				if (!at.has_value() || at.value() == 0)
				{
					Console.Error("--change-disc expects a frame number and an image path.");
					return false;
				}
				s_change_disc_at = at.value();
				s_change_disc_path = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG("--host-action") && (i + 2) < argc)
			{
				const std::optional<u32> at = StringUtil::FromChars<u32>(StringUtil::StripWhitespace(argv[++i]), 10);
				if (!at.has_value() || at.value() == 0)
				{
					Console.Error("--host-action expects a frame number and a hotkey name.");
					return false;
				}
				s_host_actions.push_back({at.value(), std::string(StringUtil::StripWhitespace(argv[++i]))});
				continue;
			}
			else if (CHECK_ARG_PARAM("--no-vm-vuaot-flips"))
			{
				// A list of 0 and 1 separated by commas. Before the VM boots (and so with no VM running) the
				// runner sets EnableVUAot to each value in turn and applies the settings, as the desktop settings
				// window does with no game running; turning it off clears what vu-aot saved.
				for (const char* c = argv[++i]; *c; c++)
				{
					if (*c == '0' || *c == '1')
						s_no_vm_vuaot_flips.push_back(*c == '1');
					else if (*c != ',')
					{
						Console.Error("--no-vm-vuaot-flips expects 0s and 1s separated by commas.");
						return false;
					}
				}
				continue;
			}
			else if (CHECK_ARG("--iop-assert-log"))
			{
				IopThread::SetViolationHandler(&IopThread::LogViolation);
				continue;
			}
			else if (CHECK_ARG_PARAM("--iop-stress"))
			{
				const auto v = StringUtil::FromChars<u32>(argv[++i]);
				if (!v.has_value())
				{
					Console.Error("--iop-stress expects a seed.");
					return false;
				}
				s_stress = true;
				s_stress_seed = v.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("--pause-toggle"))
			{
				const auto v = StringUtil::FromChars<u32>(argv[++i]);
				if (!v.has_value() || v.value() == 0)
				{
					Console.Error("--pause-toggle expects a frame number, 1 or more.");
					return false;
				}
				s_pause_toggle_at = v.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("--iop-min-slice"))
			{
				const auto v = StringUtil::FromChars<s32>(argv[++i]);
				if (!v.has_value() || v.value() < 0)
				{
					Console.Error("--iop-min-slice expects a number of EE cycles, 0 or more.");
					return false;
				}
				IopThread::SetThreadedMinSlice(v.value());
				continue;
			}
			else if (CHECK_ARG_PARAM("--iop-idle-spin-us"))
			{
				const auto v = StringUtil::FromChars<u32>(argv[++i]);
				if (!v.has_value() || v.value() > 4'000'000)
				{
					Console.Error("--iop-idle-spin-us expects a number of microseconds, 0 to 4000000.");
					return false;
				}
				IopThread::SetThreadedIdleSpinNs(v.value() * 1000u);
				continue;
			}
			else if (CHECK_ARG_PARAM("--iop-spin-hint"))
			{
				const std::string_view kind = argv[++i];
				if (kind == "yield")
					IopThread::g_spin_hint.store(IopThread::SpinHintKind::Yield, std::memory_order_relaxed);
				else if (kind == "isb")
					IopThread::g_spin_hint.store(IopThread::SpinHintKind::Isb, std::memory_order_relaxed);
				else
				{
					Console.Error("--iop-spin-hint expects yield or isb.");
					return false;
				}
				continue;
			}
			else if (CHECK_ARG_PARAM("--iop-stats-every"))
			{
				const auto v = StringUtil::FromChars<u32>(argv[++i]);
				if (!v.has_value())
				{
					Console.Error("--iop-stats-every expects a number of frames.");
					return false;
				}
				s_iop_stats_every = v.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("--rec-fallback"))
			{
				const std::string_view list = StringUtil::StripWhitespace(argv[++i]);
#if defined(ARCH_ARM64)
				std::string err;
				u32 mask = 0;
				u32 reg_masks[EERecFallback::kCop2MoveOpCount] = {};
				u64 fpu_mask[EERecFallback::kFpuIdCount / 64] = {};
				u64 vu_mask[EERecFallback::kCop2VuIdCount / 64] = {};
				if (!EERecFallback::ParseGroups(list, &mask, reg_masks, fpu_mask, vu_mask, &err))
				{
					Console.Error(err.c_str());
					return false;
				}
				s_rec_fallback_groups = mask;
				std::memcpy(s_rec_fallback_reg_masks, reg_masks, sizeof(reg_masks));
				std::memcpy(s_rec_fallback_fpu_mask, fpu_mask, sizeof(fpu_mask));
				std::memcpy(s_rec_fallback_vu_mask, vu_mask, sizeof(vu_mask));
#else
				Console.Error("--rec-fallback is implemented for the arm64 EE recompiler only.");
				return false;
#endif
				continue;
			}
			else if (CHECK_ARG_PARAM("--renderer"))
			{
				const std::string_view r = StringUtil::StripWhitespace(argv[++i]);
				s_renderer_explicit = true;
				if (r == "null" || r == "Null")            s_renderer = GSRendererType::Null;
				else if (r == "vk" || r == "vulkan")       s_renderer = GSRendererType::VK;
				else if (r == "ogl" || r == "gl" || r == "opengl") s_renderer = GSRendererType::OGL;
				else if (r == "sw" || r == "software")     s_renderer = GSRendererType::SW;
				else
				{
					Console.Error("--renderer expects one of: null, vk, ogl, sw");
					return false;
				}
				continue;
			}
			else if (CHECK_ARG_PARAM("--iso"))
			{
				s_iso_path = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("--savestate"))
			{
				s_savestate_path = StringUtil::StripWhitespace(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("--reload-state-at"))
			{
				// A savestate load in the middle of a run resets everything, the VU recompilers among them.
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value() || n.value() == 0)
				{
					Console.Error("--reload-state-at expects a frame number above zero.");
					return false;
				}
				s_reload_state_at = n.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("--ee-rate-script"))
			{
				const std::string_view spec(argv[++i]);
				size_t pos = 0;
				bool bad = false;
				while (pos <= spec.size() && !bad)
				{
					const size_t comma = spec.find(',', pos);
					const std::string_view item = spec.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
					if (!item.empty())
					{
						const size_t colon = item.find(':');
						std::optional<u32> frame;
						std::optional<s32> sel;
						if (colon != std::string_view::npos)
						{
							frame = StringUtil::FromChars<u32>(item.substr(0, colon), 10);
							sel = StringUtil::FromChars<s32>(item.substr(colon + 1), 10);
						}
						if (!frame.has_value() || !sel.has_value() || sel.value() < -3 || sel.value() > 3)
						{
							std::fprintf(stderr, "--ee-rate-script: bad entry '%.*s' (want <frame>:<selector>, selector -3..3)\n",
								static_cast<int>(item.size()), item.data());
							bad = true;
							break;
						}
						s_ee_rate_script.push_back({frame.value(), static_cast<s8>(sel.value())});
					}
					if (comma == std::string_view::npos)
						break;
					pos = comma + 1;
				}
				if (bad)
					return false;
				std::sort(s_ee_rate_script.begin(), s_ee_rate_script.end(),
					[](const EeRateStep& a, const EeRateStep& b) { return a.frame < b.frame; });
				continue;
			}
			else if (CHECK_ARG_PARAM("--ee-rate-trace"))
			{
				s_ee_rate_trace_path = StringUtil::StripWhitespace(argv[++i]);
				if (s_ee_rate_trace_path.empty())
				{
					std::fprintf(stderr, "--ee-rate-trace: want a file path\n");
					return false;
				}
				continue;
			}
			else if (CHECK_ARG_PARAM("--ee-rate-hitch"))
			{
				const std::optional<u32> n = StringUtil::FromChars<u32>(std::string_view(argv[++i]), 10);
				if (!n.has_value() || n.value() == 0)
				{
					std::fprintf(stderr, "--ee-rate-hitch: want a positive frame count\n");
					return false;
				}
				s_ee_rate_hitch = n.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("--frames"))
			{
				const auto v = StringUtil::FromChars<uint32_t>(argv[++i]);
				if (!v.has_value() || v.value() == 0)
				{
					Console.Error("Invalid --frames value.");
					return false;
				}
				s_frames = v.value();
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

		// Positional argument = the ISO/disc.
		if (s_iso_path.empty())
			s_iso_path = argv[i];
		else
		{
			Console.Error("Unexpected extra positional argument: '%s'", argv[i]);
			return false;
		}
	}

	if (s_mode == RunMode::None)
	{
		Console.Error("No mode specified (use --stepdiff, --contmem, --speedhack-diff, --liverun, --vu0diff, --selfcheck, --localize, --repro, --twindump, --twincompare, or --dump-config).");
		return false;
	}

	if (s_savestate_path.empty())
	{
		// --dump-config only needs the disc mounted: GameDB keys off the serial,
		// which is known right after Initialize. --mkstate can cold-boot the
		// disc to CREATE the first state. Every other mode replays state.
		if (s_mode != RunMode::DumpConfig && s_mode != RunMode::MkState)
		{
			Console.Error("No savestate provided (use --savestate <file>).");
			return false;
		}
	}
	else if (!FileSystem::FileExists(s_savestate_path.c_str()))
	{
		Console.ErrorFmt("Savestate '{}' does not exist.", s_savestate_path);
		return false;
	}

	if (!s_gsdump_path.empty())
	{
		if (s_mode != RunMode::LiveRun)
		{
			Console.Error("--gsdump needs --liverun: the diff modes suppress the real GS, so there is no GIF stream to record.");
			return false;
		}
		if (s_renderer_explicit && s_renderer == GSRendererType::Null)
		{
			Console.Error("--gsdump cannot use --renderer null: Null drops GIF/PATH3 instead of consuming it.");
			return false;
		}
		// Recording is armed after frame s_gsdump_at and needs s_gsdump_frames more
		// vsyncs to close the file; a short --frames would truncate it silently.
		if (s_frames < s_gsdump_at + s_gsdump_frames + 1)
		{
			s_frames = s_gsdump_at + s_gsdump_frames + 1;
			Console.WarningFmt("--gsdump: raising --frames to {} so the dump can finish.", s_frames);
		}
	}

	if (!s_shots_prefix.empty())
	{
		if (s_mode != RunMode::LiveRun)
		{
			Console.Error("--shots needs --liverun: the diff modes suppress the real GS, so nothing is drawn.");
			return false;
		}
		if (s_renderer_explicit && s_renderer == GSRendererType::Null)
		{
			Console.Error("--shots cannot use --renderer null: Null draws nothing.");
			return false;
		}
	}

	if (s_iso_path.empty())
	{
		Console.Error("No ISO provided (use --iso <file>); the savestate needs its disc mounted.");
		return false;
	}
	if (!FileSystem::FileExists(s_iso_path.c_str()))
	{
		Console.ErrorFmt("ISO '{}' does not exist.", s_iso_path);
		return false;
	}

	params.filename = s_iso_path;
	return true;
}

void EERunner::SettingsOverride()
{
	// Headless + deterministic: Null GS renderer, synchronous GS on the CPU
	// thread, no MTVU, no audio, no time-stretch. Keep wall-clock out of the
	// system clock so two runs from the same savestate produce identical
	// architectural state.

	// GS renderer. Default Null (no GS draws) — but Null is NOT self-contained: GS's
	// GetAPIForRenderer() has no Null case, so it falls through to GetPreferredRenderer()
	// for the HOST device API. On Asahi/AMD/NVIDIA that resolves to Vulkan (works with
	// the Surfaceless window we hand back); on an Intel box the auto-check declines Intel
	// Vulkan and picks OpenGL, which can't make a context for a Surfaceless window -> GS
	// fails to open. There, pass `--renderer vk` to force Vulkan (surfaceless-capable).
	// The renderer is irrelevant to EE/--contmem results (both passes on a box use the
	// same one, so GS cancels out in the jit-vs-interp diff).
	// LiveRun (the in-game hang repro) needs the live subsystems the deterministic
	// diff modes suppress: a REAL GS (so GIF/PATH3 is actually consumed, not dropped
	// like Null) and MTVU. Null GS is meaningless for it, so force VK if unset.
	const bool live = (s_mode == RunMode::LiveRun);
	GSRendererType rend = s_renderer;
	// Liverun normally needs a real GS (Null drops GIF/PATH3), so a DEFAULTED Null is
	// forced to VK. An EXPLICIT --renderer null (or --perf-jitdump profiling) is
	// honored: codegen_ab.sh depends on it for deterministic A/Bs on boxes whose
	// Vulkan is unusable from a scripted session (mq65 turnip reports 1.0 over ssh).
	if (live && rend == GSRendererType::Null && !s_perf_jitdump && !s_renderer_explicit)
		rend = GSRendererType::VK;
	s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(rend));

	// Run GS synchronously on the CPU thread (no MTGS). Key is "SynchronousMTGS"
	// (Pcsx2Config::GSOptions::SynchronousMTGS; DEVBUILD-only — Devel defines it).
	// Diff modes keep it forced true. For LiveRun, default true but gate on
	// EERUNNER_SYNCMTGS: real async play uses SyncMTGS=false, so this lets us test
	// whether the MTVU+SyncMTGS combo (which can't occur in normal play) is itself
	// the deadlock trigger (harness artifact) vs a genuine hang reproducible async.
	bool sync_mtgs = true;
	if (live)
	{
		if (const char* e = std::getenv("EERUNNER_SYNCMTGS"))
			sync_mtgs = (e[0] != '0');
	}
	s_settings_interface.SetBoolValue("EmuCore/GS", "SynchronousMTGS", sync_mtgs);

	// MTVU off for the deterministic diff modes; ON for LiveRun by default. Gate on
	// EERUNNER_MTVU so the wedge can be retested MTVU-off without a rebuild: if the
	// hang reproduces identically MTVU-off, it is NOT an MTVU/MTGS thread deadlock
	// (rules out "EE thread blocked") and is a genuine EE-rec cycle/event-test bug.
	bool live_mtvu = live;
	if (live)
	{
		if (const char* e = std::getenv("EERUNNER_MTVU"))
			live_mtvu = (e[0] != '0');
	}
	s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "vuThread", live_mtvu);

	// vu-aot is on by default in the product; the runner starts with it off so the twin passes and the identity
	// runs stay comparable. `--set EmuCore/CPU/Recompiler/EnableVUAot=true` (applied after this pinning) turns it
	// on, which also brings the VU program cache up (UseVUProgramCache).
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", false);

	// EERUNNER_EECYCLESKIP / EERUNNER_EECYCLERATE: apply the EE cycle
	// speedhacks. The Android Low-End preset ships EECycleSkip=1 and handheld
	// users add underclock on top; timing-poison triage needs the handheld's
	// clock shape reproducible on the desk.
	if (const char* e = std::getenv("EERUNNER_EECYCLESKIP"))
		s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleSkip", std::atoi(e));
	if (const char* e = std::getenv("EERUNNER_EECYCLERATE"))
		s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleRate", std::atoi(e));

	// The governor's development trace. The switch itself stays off: with no limiter there
	// is nothing for it to act on, and shadow mode is what writes the rows.
	if (!s_ee_rate_trace_path.empty())
		s_settings_interface.SetStringValue("EmuCore/Speedhacks", "DynamicEECycleRateTrace", s_ee_rate_trace_path.c_str());

	// EE recompiler ON by default for LiveRun; gate on EERUNNER_EE=interp (or 0) to
	// run the clean EE-interpreter control pass — for jit-vs-interp comparison of the
	// SAME live hang (interp clean, jit hangs). Diff modes leave EnableEE untouched.
	if (live)
	{
		bool ee_jit = true;
		if (const char* e = std::getenv("EERUNNER_EE"))
			ee_jit = !(e[0] == 'i' || e[0] == 'I' || e[0] == '0');
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", ee_jit);

		// --vu0-interp / --vu1-interp run that VU on its interpreter with
		// everything else left on the JIT, so a PCSX2_VU_TRAJ_OUT A/B attributes a
		// trajectory move to one VU engine. Turn MTVU off with EERUNNER_MTVU=0 when
		// interpreting VU1.
		if (s_vu0_interp)
			s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", false);
		if (s_vu1_interp)
			s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", false);
	}

	// EERUNNER_FPUFULL=1 forces CHECK_FPU_FULL (eeClampMode:3) so the EE-FPU JIT uses
	// the double-precision ADD/SUB/MUL/MADD paths that match the interp's fpuDouble()
	// math. In --liverun: decisive test of the "EE-FPU 1-ULP precision is the hang
	// cause" hypothesis (proven NEGATIVE — still hangs with FPUFULL on). In --stepdiff:
	// converges the benign mul/add 1-ULP so the lockstep differ walks past it to the
	// next (non-precision) divergence. Applied to ALL modes (set before VMManager
	// init). recDIV_S stays single even in FULL, so div divergences still surface.
	// EERUNNER_VU0FP=1 adds VU0 macro-visible state (VF00-31 + ACC + the
	// STATUS/MAC/CLIP flag VIs) to the divtrace fingerprint + snapshot diff, so
	// --stepdiff's zoom breaks at the EE block whose COP2 macro emission first
	// produced divergent VU0 state (default fingerprint only sees GPR/FPR/CP0,
	// which surfaces macro bugs downstream — e.g. the Jak 3 fMax vertex flood
	// showed up at an unrelated memcpy block). Opt-in because mVU-vs-interp has
	// by-design NaN/clamp corners the benign filters were not tuned for.
	if (const char* e = std::getenv("EERUNNER_VU0FP"))
		ee_divtrace::g_vu0_include = (e[0] != '0');
	// EERUNNER_VU0FULL=1 adds the rest of VU0 (every VI incl. VPU_STAT/TPC, its
	// clock and flags, data + micro memory) to the fingerprint. For --twindump /
	// --twincompare between two JIT builds whose VU0 timing must be identical;
	// JIT-vs-interp pairs differ on the VU0 clock by design.
	if (const char* e = std::getenv("EERUNNER_VU0FULL"))
		ee_divtrace::g_vu0_full = (e[0] != '0');
	// EERUNNER_SYSFULL=1 widens the per-frame fingerprint to the whole machine:
	// IOP RAM, registers and scheduler, both hardware-register pages, both
	// counter sets, VU1, and the EE timing registers the default hash skips.
	// For twin runs between two JIT builds that must keep emulated timing
	// identical.
	if (const char* e = std::getenv("EERUNNER_SYSFULL"))
		ee_divtrace::g_sys_full = (e[0] != '0');

	if (const char* e = std::getenv("EERUNNER_FPUFULL"))
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "fpuFullMode", e[0] != '0');

	// EERUNNER_NOFASTMEM=1 disables EE/VTLB fastmem (the 4 GB signal-backpatch fast
	// path). REQUIRED when running this binary under x86 emulation (FEX inside the
	// muvm 4K-page microVM, for single-machine cross-arch jit-vs-jit): PCSX2 fastmem
	// catches its OWN SIGSEGV to backpatch VTLB accesses, but FEX intercepts SIGSEGV
	// for its x86->arm64 translation, so the guest fault never reaches PCSX2's handler
	// -> unhandled SIGSEGV right after "Resetting fastmem mappings". Fastmem off routes
	// every load/store through the explicit VTLB call path: EE architectural state is
	// IDENTICAL (fastmem only changes speed), so memdump/contmem results are unaffected.
	// Native arm64 leaves it on (default). Applied to ALL modes (set before VM init).
	if (const char* e = std::getenv("EERUNNER_NOFASTMEM"))
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", e[0] == '0');

	// EERUNNER_DIVCHOP=1 forces EE FPU DIV.S/SQRT.S to round toward zero (chop) by
	// setting FPUDiv.Roundmode = ChopZero (3), making FPUDivFPCR == FPUFPCR so the
	// arm64 recDIV_S FPCR round-mode swap-to-Nearest becomes a no-op. Diagnostic for
	// the Burnout-3 hang: the JIT div rounds Nearest (default FPUDivFPCR), interp
	// rounds chop (ambient FPUFPCR), and one game div (0.1*2560) lands on the
	// 255.9999847/256.0 boundary -> cvt.w.s gives 255 vs 256, a 1-off count that
	// corrupts the GIF scratchpad buffer bookkeeping. Roundmode: 0=Nearest 3=ChopZero.
	if (const char* e = std::getenv("EERUNNER_DIVCHOP"))
		s_settings_interface.SetIntValue("EmuCore/CPU", "FPUDiv.Roundmode", e[0] != '0' ? 3 : 0);

	// EERUNNER_HWDL=<0..4> forces EmuCore/GS HWDownloadMode (0=Enabled 1=EnabledForceFull
	// 2=NoReadbacks 3=Unsynchronized 4=Disabled). Diagnostic for GPU-readback pipeline
	// serialization (OutRun 2006 SD865: fps ≈ 1/(GS_cpu + GPU) because GSDownloadTexture::
	// Flush() drains the GPU whenever the game reads back RT data mid-frame).
	// Unsynchronized keeps all CPU-side copy work but skips the fence wait — isolates the
	// stall; NoReadbacks/Disabled also skip the copies. 1-4 are NOT render-accurate; A/B only.
	if (const char* e = std::getenv("EERUNNER_HWDL"))
		s_settings_interface.SetIntValue("EmuCore/GS", "HWDownloadMode", atoi(e));

	// EERUNNER_SPINCPU=1 sets EmuCore/GS HWSpinCPUForReadbacks: fence waits at GPU
	// readbacks spin instead of sleeping, cutting the scheduler wake latency that
	// dominates each synchronous readback round-trip once the pipeline overlap fix
	// (mid-frame command buffer kick) has the GPU already caught up.
	if (const char* e = std::getenv("EERUNNER_SPINCPU"))
		s_settings_interface.SetBoolValue("EmuCore/GS", "HWSpinCPUForReadbacks", e[0] != '0');

	// No audio output, and no time-stretch sync (wall-clock-driven). Keys live in
	// Pcsx2Config::SPU2Options::LoadSave under [SPU2/Output]: Backend / SyncMode.
	s_settings_interface.SetStringValue("SPU2/Output", "Backend", "Null");
	s_settings_interface.SetStringValue("SPU2/Output", "SyncMode", "Disabled");

	// Profiling: drive the perf jitdump enable through the normal config path so
	// ApplySettings/LoadSettings (which re-applies Perf::SetJitDumpEnabled from this
	// bool) keeps it on for the whole run instead of resetting it to the default.
	s_settings_interface.SetBoolValue("EmuCore/Profiler", "EnablePerfDump", s_perf_jitdump);

	// No frameskip.
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameSkipEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "FramesToDraw", 1);
	s_settings_interface.SetIntValue("EmuCore/GS", "FramesToSkip", 0);

	// Don't limit speed (also set on the VM via SetLimiterMode after init).
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", 0);

	// Disable input sources; we drive nothing.
	s_settings_interface.SetBoolValue("InputSources", "SDL", false);
	s_settings_interface.SetBoolValue("InputSources", "XInput", false);
	Pad::ClearPortBindings(s_settings_interface, 0);
	s_settings_interface.ClearSection("Hotkeys");

	// Logging.
	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", !s_no_console);
	s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", false);
	s_settings_interface.SetBoolValue("Logging", "EnableVerbose", true);

	// Remove memory cards, so we don't have sharing violations.
	for (u32 i = 0; i < 2; i++)
	{
		s_settings_interface.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
		s_settings_interface.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
	}

	// --set Section/Key=Value overrides, applied LAST so they win over the
	// harness pinning above. Base-layer only: a GameDB per-game override of the
	// same key still wins (that's the production layering — useful for testing
	// a pending GameDB fix). The twin passes own the CPU-provider toggles
	// (EnableEE/VU0/VU1/IOP); a --set of those is clobbered per pass.
	for (const SetOverride& so : s_set_overrides)
	{
		s_settings_interface.SetStringValue(so.section.c_str(), so.key.c_str(), so.value.c_str());
		Console.WriteLn(fmt::format("SettingsOverride: --set [{}] {} = {}", so.section, so.key, so.value));
	}

#if defined(ARCH_ARM64)
	// --rec-fallback: route whole EE opcode groups through the interpreter. Not a
	// setting (it lives in the recompiler, not EmuConfig), so it is applied here,
	// before any block is compiled, and stays put for the whole run.
	EERecFallback::g_groups = s_rec_fallback_groups;
	std::memcpy(EERecFallback::g_cop2RegMask, s_rec_fallback_reg_masks, sizeof(s_rec_fallback_reg_masks));
	std::memcpy(EERecFallback::g_fpuMask, s_rec_fallback_fpu_mask, sizeof(s_rec_fallback_fpu_mask));
	std::memcpy(EERecFallback::g_cop2VuMask, s_rec_fallback_vu_mask, sizeof(s_rec_fallback_vu_mask));
	if (s_rec_fallback_groups != 0)
	{
		Console.WriteLn(fmt::format("EE REC FALLBACK: forcing to interpreter -> {}",
			EERecFallback::DescribeGroups(s_rec_fallback_groups)));
	}
#endif
}

// Snapshot live cpuRegs/fpuRegs into a FullSnap (frame-boundary capture; pc/cycle
// from the live registers).
static ee_divtrace::FullSnap CaptureFullSnap()
{
	ee_divtrace::FullSnap fs;
	std::memcpy(&fs.cpu, &cpuRegs, sizeof(cpuRegisters));
	std::memcpy(&fs.fpu, &fpuRegs, sizeof(fpuRegisters));
	std::memcpy(fs.vu0_vf, VU0.VF, 32 * 16);
	std::memcpy(fs.vu0_vf + 32 * 16, &VU0.ACC, 16);
	fs.vu0_vi_status = VU0.VI[REG_STATUS_FLAG].UL;
	fs.vu0_vi_mac    = VU0.VI[REG_MAC_FLAG].UL;
	fs.vu0_vi_clip   = VU0.VI[REG_CLIP_FLAG].UL;
	fs._pad0 = 0;
	fs.cycle = cpuRegs.cycle;
	fs.pc = cpuRegs.pc;
	fs._pad = 0;
	return fs;
}

// Per-frame selfcheck record: full register snapshot + the frame memory hash.
struct SelfCheckFrame
{
	ee_divtrace::FullSnap snap;
	uint64_t              memhash;
};

// Field-level diff of two FullSnaps (defined later); empty == identical regs.
static std::vector<std::string> DiffFullSnaps(const ee_divtrace::FullSnap& jit,
	const ee_divtrace::FullSnap& interp);

// Advance the loaded VM by `count` frames, discarding output (defined later).
static void AdvanceFrames(uint32_t count);

// Run s_frames frames in the current CPU mode, retaining a full register
// snapshot + memory hash per frame so selfcheck can field-diff the first
// divergent frame. (Snapshots are ~2 KB each; bounded by s_frames.)
static std::vector<SelfCheckFrame> RunAndSnapshot()
{
	std::vector<SelfCheckFrame> out;
	out.reserve(s_frames);
	for (uint32_t f = 0; f < s_frames && VMManager::GetState() != VMState::Shutdown; ++f)
	{
		VMManager::FrameAdvance(1);
		VMManager::Execute(); // returns after one frame (paused)
		out.push_back({CaptureFullSnap(), ee_divtrace::HashMemory()});
	}
	return out;
}

// Re-run from a fresh savestate load to the state captured at frame index
// `frame` (== AdvanceFrames(frame+1)), returning EE main RAM + scratchpad bytes
// for offline region diffing. Used to localize a store-path divergence.
static std::vector<u8> RunToFrameCaptureMem(uint32_t frame)
{
	Error error;
	if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
	{
		Console.ErrorFmt("RunToFrameCaptureMem: load failed: {}", error.GetDescription());
		return {};
	}
	AdvanceFrames(frame + 1);
	std::vector<u8> out(Ps2MemSize::MainRam + Ps2MemSize::Scratch);
	std::memcpy(out.data(), eeMem->Main, Ps2MemSize::MainRam);
	std::memcpy(out.data() + Ps2MemSize::MainRam, eeMem->Scratch, Ps2MemSize::Scratch);
	return out;
}

// Count of differing 4 KB pages / bytes between two EE-memory captures.
struct MemDiffCount
{
	size_t pages = 0;
	size_t bytes = 0;
};

// Diff two EE-memory captures; print the first few differing 4 KB pages with a
// little content from each side so the divergent region/device can be reasoned
// about (main RAM byte offset == EE physical address for the first 32 MB).
// Returns the total differing page/byte counts (used by --speedhack-diff to size
// a divergence against the baseline determinism floor). verbose=false suppresses
// the per-page detail + summary line (for the many quiet trajectory samples).
static MemDiffCount ReportMemDiff(const std::vector<u8>& a, const std::vector<u8>& b, bool verbose = true, size_t max_show = 8)
{
	if (a.size() != b.size() || a.empty())
	{
		if (verbose)
			Console.ErrorFmt("  mem capture size mismatch ({} vs {})", a.size(), b.size());
		return {};
	}
	const size_t pageSize = 0x1000;
	size_t shown = 0, diffPages = 0, diffBytes = 0;
	for (size_t off = 0; off < a.size(); off += pageSize)
	{
		const size_t end = std::min(off + pageSize, a.size());
		size_t firstDiff = SIZE_MAX, pageDiffBytes = 0;
		for (size_t i = off; i < end; ++i)
			if (a[i] != b[i])
			{
				if (firstDiff == SIZE_MAX)
					firstDiff = i;
				++pageDiffBytes;
			}
		if (firstDiff == SIZE_MAX)
			continue;
		++diffPages;
		diffBytes += pageDiffBytes;
		if (verbose && shown < max_show)
		{
			++shown;
			const char* region = (firstDiff < Ps2MemSize::MainRam) ? "Main" : "Scratch";
			const size_t addr = (firstDiff < Ps2MemSize::MainRam)
				? firstDiff : (firstDiff - Ps2MemSize::MainRam);
			Console.ErrorFmt("  {} @ {:#010x}: {} bytes differ in page; A=[{:02x} {:02x} {:02x} {:02x}] B=[{:02x} {:02x} {:02x} {:02x}]",
				region, addr, pageDiffBytes,
				a[firstDiff], a[firstDiff + 1 < a.size() ? firstDiff + 1 : firstDiff],
				a[firstDiff + 2 < a.size() ? firstDiff + 2 : firstDiff], a[firstDiff + 3 < a.size() ? firstDiff + 3 : firstDiff],
				b[firstDiff], b[firstDiff + 1 < b.size() ? firstDiff + 1 : firstDiff],
				b[firstDiff + 2 < b.size() ? firstDiff + 2 : firstDiff], b[firstDiff + 3 < b.size() ? firstDiff + 3 : firstDiff]);
		}
	}
	if (verbose)
		Console.ErrorFmt("  mem diff summary: {} pages, {} bytes differ (of {} captured)", diffPages, diffBytes, a.size());
	return {diffPages, diffBytes};
}

// Compare two per-frame snapshot streams; print the first few divergent frames
// with field detail. Returns true if identical. `la`/`lb` label the two streams.
static bool CompareStreams(const std::vector<SelfCheckFrame>& a,
	const std::vector<SelfCheckFrame>& b, const char* la, const char* lb)
{
	if (a.size() != b.size())
	{
		Console.ErrorFmt("  {} vs {}: frame count differs ({} vs {})", la, lb, a.size(), b.size());
		return false;
	}

	size_t first_regdiff = SIZE_MAX, first_memdiff = SIZE_MAX, first_pcdiff = SIZE_MAX;
	size_t diverged_frames = 0;
	for (size_t f = 0; f < a.size(); ++f)
	{
		const auto regdiffs = DiffFullSnaps(a[f].snap, b[f].snap);
		const bool memdiff = a[f].memhash != b[f].memhash;
		const bool pcdiff = a[f].snap.pc != b[f].snap.pc;
		if (regdiffs.empty() && !memdiff && !pcdiff)
			continue;

		++diverged_frames;
		if (!regdiffs.empty() && first_regdiff == SIZE_MAX)
			first_regdiff = f;
		if (memdiff && first_memdiff == SIZE_MAX)
			first_memdiff = f;
		if (pcdiff && first_pcdiff == SIZE_MAX)
			first_pcdiff = f;

		if (diverged_frames <= 4)
		{
			Console.ErrorFmt("  [{} vs {}] frame {}: pc {:#010x}/{:#010x} cycle {}/{} (dcyc={}) mem {}",
				la, lb, f, a[f].snap.pc, b[f].snap.pc, a[f].snap.cycle, b[f].snap.cycle,
				(int64_t)a[f].snap.cycle - (int64_t)b[f].snap.cycle,
				memdiff ? "DIFFERS" : "same");
			for (const auto& d : regdiffs)
				Console.ErrorFmt("    {}", d); // here "JIT="==la, "INTERP="==lb
		}
	}

	if (diverged_frames == 0)
	{
		Console.WriteLn(fmt::format("  {} vs {}: identical ({} frames)", la, lb, a.size()));
		return true;
	}

	Console.ErrorFmt("  {} vs {}: {}/{} frames diverged. first reg={} mem={} pc={}",
		la, lb, diverged_frames, a.size(),
		first_regdiff == SIZE_MAX ? -1 : (int64_t)first_regdiff,
		first_memdiff == SIZE_MAX ? -1 : (int64_t)first_memdiff,
		first_pcdiff == SIZE_MAX ? -1 : (int64_t)first_pcdiff);
	return false;
}

// --selfcheck: N interpreter passes from the same savestate must produce
// byte-identical per-frame snapshot streams. Running 3 passes (not 2) lets us
// distinguish a cold-cache/first-run artifact (B==C but A differs) from genuine
// per-run host nondeterminism (all three differ). Returns process exit code.
static int RunSelfCheck()
{
	Error error;
	const int kPasses = 3;
	std::vector<std::vector<SelfCheckFrame>> runs;

	for (int p = 0; p < kPasses; ++p)
	{
		if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
		{
			Console.ErrorFmt("Failed to load savestate (pass {}): {}", p, error.GetDescription());
			return EXIT_FAILURE;
		}
		Console.WriteLn(fmt::format("eerunner: pass {} — running {} frames (interp)...", p, s_frames));
		runs.push_back(RunAndSnapshot());
	}

	static const char* const names[3] = {"A", "B", "C"};
	bool all_match = true;
	for (int p = 1; p < kPasses; ++p)
		all_match &= CompareStreams(runs[p - 1], runs[p], names[p - 1], names[p]);

	if (all_match)
	{
		Console.WriteLn(fmt::format("SELFCHECK PASS ({} passes × {} frames identical)", kPasses, s_frames));
		return EXIT_SUCCESS;
	}

	// Localize the first store-path divergence between the two WARM runs (B,C):
	// these have no cold-cache asymmetry, so a memory diff there is genuine
	// per-run nondeterminism. Re-run twice to that frame and report the region.
	size_t warm_memdiff = SIZE_MAX;
	for (size_t f = 0; f < runs[1].size() && f < runs[2].size(); ++f)
		if (runs[1][f].memhash != runs[2][f].memhash)
		{
			warm_memdiff = f;
			break;
		}
	if (warm_memdiff != SIZE_MAX)
	{
		Console.ErrorFmt("warm-run (B,C) memory first diverges at frame {} — localizing region:", warm_memdiff);
		const auto m1 = RunToFrameCaptureMem((uint32_t)warm_memdiff);
		const auto m2 = RunToFrameCaptureMem((uint32_t)warm_memdiff);
		ReportMemDiff(m1, m2);
	}

	Console.Error("SELFCHECK FAIL — see per-pair divergence above.");
	Console.Error("  (B vs C identical but A differs => cold-cache/first-run artifact; warm up before the golden.)");
	Console.Error("  (all pairs differ => per-run host nondeterminism; hunt the wall-clock/thread source.)");
	return EXIT_FAILURE;
}

// ===========================================================================
// --localize : the three-level divergence funnel.
//
//   Level 1 (per-FRAME, whole run): run interp golden, then JIT, recording one
//     (regfp, memhash) per frame. First differing frame F is the divergent
//     frame. If only memhash differs there, it's a store-only divergence (a
//     bad memory write that no register has read back yet) — reported at frame
//     granularity; finer memory localization is intentionally out of scope (it
//     is a slice-tracking tarpit).
//   Level 2 (per-OP, frame F only): re-run interp (dense, one Sample/op) and
//     JIT (sparse, one Sample per block entry) for frame F with the capture
//     sites enabled. Align by walking the JIT block-entry stream against the
//     dense interp op-stream — EE rec blocks are single basic blocks, so the
//     next interp op with pc == jit[k].pc is unambiguously that block entry.
//     First fp mismatch localizes the offending JIT block (the one that ran
//     between the last matching entry and the mismatch).
//   Level 3 (full register snapshot): re-run frame F with a 1-entry detail
//     window at the divergent index on each side, and diff the full
//     cpuRegisters/fpuRegisters to name the exact divergent field(s).
// ===========================================================================

// Switch the EE core between interpreter (jit=false) and recompiler (jit=true).
// Consumed by the next VMManager::Execute() (cpu-impl-changed -> cache clear).
static void SetEeMode(bool jit)
{
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", jit);
	VMManager::ApplySettings();
}

// --vu0diff axis: pin the EE INTERPRETER (deterministic, identical in both
// passes) and toggle only the VU0 micro engine. The two passes then run EE-interp
// in lockstep until mVU0-jit first hands the EE a different result than the VU0
// interpreter — isolating a VU0-jit-vs-interp value bug with no EE-rec or
// cross-arch noise.
static void SetVu0Mode(bool vu0_jit)
{
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", false);
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", vu0_jit);
	VMManager::ApplySettings();
}

// Advance the (already savestate-loaded) VM by `count` frames, discarding output.
static void AdvanceFrames(uint32_t count)
{
	for (uint32_t f = 0; f < count && VMManager::GetState() != VMState::Shutdown; ++f)
	{
		VMManager::FrameAdvance(1);
		VMManager::Execute();
	}
}

// Checkpoint-anchored fine pass: run exactly ONE frame from the CURRENT VM state
// (a freshly-loaded checkpoint) with the dense/sparse capture sites enabled. No
// AdvanceFrames — the caller positioned the VM, so there is no cross-pass drift.
static std::vector<ee_divtrace::Sample> RunFineFpFromHere()
{
	ee_divtrace::Reset();
	ee_divtrace::ReserveStream(16u * 1024u * 1024u);
	ee_divtrace::ConfigureFullWindow(0, 0); // fingerprints only
	ee_divtrace::g_enabled.store(true, std::memory_order_release);
	VMManager::FrameAdvance(1);
	VMManager::Execute();
	ee_divtrace::g_enabled.store(false, std::memory_order_release);
	return ee_divtrace::g_stream;
}

// Checkpoint-anchored detail pass: one frame from the current VM state with a
// 1-entry full-snapshot window at stream index `idx`.
static ee_divtrace::FullSnap RunFineSnapAtFromHere(uint32_t idx)
{
	ee_divtrace::Reset();
	ee_divtrace::ReserveStream(16u * 1024u * 1024u);
	ee_divtrace::ConfigureFullWindow(idx, 1);
	ee_divtrace::g_enabled.store(true, std::memory_order_release);
	VMManager::FrameAdvance(1);
	VMManager::Execute();
	ee_divtrace::g_enabled.store(false, std::memory_order_release);
	if (ee_divtrace::g_snaps.empty())
		return ee_divtrace::FullSnap{};
	return ee_divtrace::g_snaps.front();
}

struct AlignResult
{
	bool     found = false;
	bool     control_flow = false; // true: JIT reached a block interp never did
	uint32_t jit_idx = 0;          // divergent JIT block-entry index
	uint32_t interp_idx = 0;       // matched interp op index
	uint32_t pc = 0;               // block entry where divergence was observed
	uint32_t prev_pc = 0;          // entry of the JIT block that produced it
};

// Walk the sparse JIT block-entry stream against the dense interp op-stream.
//
// Semantics: a JIT block-entry sample is (pc=block start, fp=state ABOUT TO
// execute that block). An interp sample is recorded AFTER each op with
// pc=cpuRegs.pc (== the NEXT op) and fp=state after the op == state about to
// execute that next pc. So an interp sample (pc=P, fp=S) means "about to execute
// P with state S" — directly comparable to a JIT block-entry (P, S).
//
// Exception: JIT block-entry #0 is the FRAME START (state == the shared
// checkpoint), and the interp stream has no pre-first-op sample for it (the first
// interp sample with pc==entry0 is one loop iteration later). Entry #0 is equal
// by construction, so we anchor on it without comparing, and begin the real
// divergence search at entry #1.
// Walk from a given resume point (start_k JIT entry, start_ii interp position,
// start_prev_pc the entry that precedes start_k). Returns the first divergence
// at or after start_k, or {found=false} if the streams agree to the end.
static AlignResult AlignFrom(const std::vector<ee_divtrace::Sample>& interp,
	const std::vector<ee_divtrace::Sample>& jit, size_t start_k, size_t start_ii, uint32_t start_prev_pc)
{
	size_t ii = start_ii;
	uint32_t prev_pc = start_prev_pc;
	for (size_t k = start_k; k < jit.size(); ++k)
	{
		size_t scan = ii;
		while (scan < interp.size() && interp[scan].pc != jit[k].pc)
			++scan;
		if (scan >= interp.size())
			return {true, true, static_cast<uint32_t>(k), static_cast<uint32_t>(ii), jit[k].pc, prev_pc};
		if (interp[scan].fp != jit[k].fp)
			return {true, false, static_cast<uint32_t>(k), static_cast<uint32_t>(scan), jit[k].pc, prev_pc};
		ii = scan + 1;
		prev_pc = jit[k].pc;
	}
	return {}; // streams agree across every JIT block entry
}

static AlignResult Align(const std::vector<ee_divtrace::Sample>& interp,
	const std::vector<ee_divtrace::Sample>& jit)
{
	return AlignFrom(interp, jit, 1, 0, jit.empty() ? 0 : jit[0].pc);
}

// After a data divergence at (div_k, div_ii) that the caller has classified
// benign (a cycle-derived timer read taints one or more GPRs), walk forward to
// where the two streams RE-CONVERGE — the first JIT block-entry whose pc matches
// the interp op-stream AND whose full fingerprint agrees again (the tainted
// value has been overwritten / washed out). From there a strict walk is sound.
struct ResyncResult
{
	bool     reconverged = false;
	uint32_t k = 0;        // resume start_k for AlignFrom
	uint32_t ii = 0;       // resume start_ii
	uint32_t prev_pc = 0;  // resume start_prev_pc
	uint32_t blind = 0;    // JIT block-entries skipped while contaminated (a blind window)
	bool     ran_off = false; // pc-match failed mid-window (timing perturbed control flow)
};

static ResyncResult ResyncAfter(const std::vector<ee_divtrace::Sample>& interp,
	const std::vector<ee_divtrace::Sample>& jit, uint32_t div_k, uint32_t div_ii)
{
	size_t ii = static_cast<size_t>(div_ii) + 1;
	uint32_t blind = 0;
	for (size_t k = div_k + 1; k < jit.size(); ++k)
	{
		size_t scan = ii;
		while (scan < interp.size() && interp[scan].pc != jit[k].pc)
			++scan;
		if (scan >= interp.size())
		{
			ResyncResult r;
			r.ran_off = true;
			r.blind = blind;
			return r; // JIT reached a block interp never did within the window
		}
		if (interp[scan].fp == jit[k].fp)
		{
			// Full state matches again — resume strict align AFTER this entry.
			ResyncResult r;
			r.reconverged = true;
			r.k = static_cast<uint32_t>(k) + 1;
			r.ii = static_cast<uint32_t>(scan) + 1;
			r.prev_pc = jit[k].pc;
			r.blind = blind;
			return r;
		}
		ii = scan + 1;
		++blind;
	}
	ResyncResult r; // walked to end-of-frame still divergent (taint never washed out)
	r.blind = blind;
	return r;
}

// True if any non-GPR architectural field differs (HI/LO, CP0 except the
// dispatcher counters, FPR, ACC, sa) — i.e. the divergence is more than just
// tainted GPRs, so it can't be dismissed as a pure timer-read artifact.
static bool NonGprDiffers(const ee_divtrace::FullSnap& jit, const ee_divtrace::FullSnap& interp)
{
	if (jit.cpu.HI.UD[0] != interp.cpu.HI.UD[0] || jit.cpu.HI.UD[1] != interp.cpu.HI.UD[1] ||
		jit.cpu.LO.UD[0] != interp.cpu.LO.UD[0] || jit.cpu.LO.UD[1] != interp.cpu.LO.UD[1])
		return true;
	for (int i = 0; i < 32; ++i)
	{
		if (i == 1 || i == 9 || i == 11 || i == 13 || i == 14) // +Cause/EPC: interrupt-phase noise
			continue;
		if (jit.cpu.CP0.r[i] != interp.cpu.CP0.r[i])
			return true;
	}
	if (!ee_divtrace::g_fp_exclude)
	{
		for (int i = 0; i < 32; ++i)
			if (jit.fpu.fpr[i].Word() != interp.fpu.fpr[i].Word())
				return true;
		if (jit.fpu.ACC.Word() != interp.fpu.ACC.Word())
			return true;
	}
	if (jit.cpu.sa != interp.cpu.sa)
		return true;
	return false;
}

// Field-level diff of two full snapshots, mirroring DiffEe's ignored set
// (CP0 Random/Count/Compare, FPU control regs, cycle bookkeeping).
static std::vector<std::string> DiffFullSnaps(const ee_divtrace::FullSnap& jit,
	const ee_divtrace::FullSnap& interp)
{
	std::vector<std::string> out;
	static const char* const gpr_names[32] = {
		"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
		"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
		"t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"};
	auto d64 = [&](const std::string& n, u64 a, u64 b) {
		if (a != b)
			out.push_back(fmt::format("{}: JIT={:#018x} INTERP={:#018x}", n, a, b));
	};
	auto d32 = [&](const std::string& n, u32 a, u32 b) {
		if (a != b)
			out.push_back(fmt::format("{}: JIT={:#010x} INTERP={:#010x}", n, a, b));
	};
	for (int i = 0; i < 32; ++i)
	{
		d64(std::string(gpr_names[i]) + ".lo", jit.cpu.GPR.r[i].UD[0], interp.cpu.GPR.r[i].UD[0]);
		d64(std::string(gpr_names[i]) + ".hi", jit.cpu.GPR.r[i].UD[1], interp.cpu.GPR.r[i].UD[1]);
	}
	d64("hi.lo", jit.cpu.HI.UD[0], interp.cpu.HI.UD[0]);
	d64("hi.hi", jit.cpu.HI.UD[1], interp.cpu.HI.UD[1]);
	d64("lo.lo", jit.cpu.LO.UD[0], interp.cpu.LO.UD[0]);
	d64("lo.hi", jit.cpu.LO.UD[1], interp.cpu.LO.UD[1]);
	for (int i = 0; i < 32; ++i)
	{
		if (i == 1 || i == 9 || i == 11 || i == 13 || i == 14) // +Cause/EPC: interrupt-phase noise
			continue;
		d32(fmt::format("cp0[{}]", i), jit.cpu.CP0.r[i], interp.cpu.CP0.r[i]);
	}
	if (!ee_divtrace::g_fp_exclude)
	{
		for (int i = 0; i < 32; ++i)
			d32(fmt::format("fpr[{}]", i), jit.fpu.fpr[i].Word(), interp.fpu.fpr[i].Word());
		d32("ACC", jit.fpu.ACC.Word(), interp.fpu.ACC.Word());
	}
	// VU0 macro state — reported only when it participates in alignment
	// (EERUNNER_VU0FP), so the default report set stays what the benign-class
	// filters were tuned on.
	if (ee_divtrace::g_vu0_include)
	{
		static const char lanes[4] = {'x', 'y', 'z', 'w'};
		for (int i = 0; i < 33; ++i)
		{
			u32 a[4], b[4];
			std::memcpy(a, jit.vu0_vf + i * 16, 16);
			std::memcpy(b, interp.vu0_vf + i * 16, 16);
			const std::string name = (i == 32) ? std::string("vu0.ACC") : fmt::format("vu0.vf{:02}", i);
			for (int l = 0; l < 4; ++l)
				d32(fmt::format("{}.{}", name, lanes[l]), a[l], b[l]);
		}
		d32("vu0.vi.STATUS", jit.vu0_vi_status, interp.vu0_vi_status);
		d32("vu0.vi.MAC", jit.vu0_vi_mac, interp.vu0_vi_mac);
		d32("vu0.vi.CLIP", jit.vu0_vi_clip, interp.vu0_vi_clip);
	}
	d32("sa", jit.cpu.sa, interp.cpu.sa);
	return out;
}

// True for EE branch/jump primary opcodes (so we can stop disassembling a
// single basic block after its terminating branch + delay slot).
static bool IsEeBranchOpcode(u32 code)
{
	const u32 op = code >> 26;
	if (op == 0) // SPECIAL — JR (8) / JALR (9)
	{
		const u32 fn = code & 0x3f;
		return fn == 8 || fn == 9;
	}
	if (op == 1) // REGIMM — BLTZ/BGEZ/BLTZAL/...
		return true;
	if (op == 2 || op == 3) // J / JAL
		return true;
	if (op >= 4 && op <= 7) // BEQ/BNE/BLEZ/BGTZ
		return true;
	if (op >= 0x14 && op <= 0x17) // BEQL/BNEL/BLEZL/BGTZL
		return true;
	if (op == 0x10 || op == 0x11 || op == 0x12) // COP0/1/2 — may be BCxF/T
		return ((code >> 21) & 0x1f) == 0x08;
	return false;
}

// Disassemble a single EE basic block starting at `pc` to the console (stops a
// couple of instructions past the first branch, or at maxInsns). Read on the
// CPU thread where guest memory is live.
static void DisasmBlock(u32 pc, u32 maxInsns = 48)
{
	Console.WriteLn(fmt::format("    --- block disasm @ {:#010x} ---", pc));
	bool saw_branch = false;
	u32 after_branch = 0;
	for (u32 i = 0; i < maxInsns; ++i)
	{
		const u32 addr = pc + i * 4;
		const u32 code = memRead32(addr);
		std::string line;
		R5900::disR5900Fasm(line, code, addr, /*simplify=*/false);
		Console.WriteLn(fmt::format("    {:#010x}: {:08x}  {}", addr, code, line));
		if (saw_branch && ++after_branch >= 1) // include the delay slot, then stop
			break;
		if (IsEeBranchOpcode(code))
			saw_branch = true;
	}
}

// Recognize the cycle-derived-MMIO divergence class: a divergent GPR whose value
// originates — directly OR through arithmetic — from an EE timer COUNT register
// read inside the offending block. The EE timers (T0..T3 @ 0x1000_0000 / _0800 /
// _1000 / _1800, COUNT at +0) advance with cpuRegs.cycle, and the JIT vs interp
// differ by a few ticks at any mid-block read because they sync accumulated
// block-cycles at different granularity (per-block vs per-op). That is a TIMING
// artifact, not a codegen bug — a classic timing-taint trap — so we tag it rather
// than presenting it as a real bug.
//
// Two layers run over the block:
//   * const-prop (lui/ori/addiu chains) resolves each load's effective address,
//     so we can spot a load from a timer COUNT;
//   * taint-propagation tracks a per-GPR "cycle-derived" bit: set on a COUNT
//     load, propagated through pure-dataflow ALU ops (subu/addu/daddu/sll/and/…)
//     whose source is tainted, cleared on a non-timer load / lui / unmodeled
//     writer. This catches the common software-timer accumulator shape (read
//     COUNT, subtract prior COUNT for a delta, daddu it into a 64-bit virtual
//     clock) where the divergence flows into registers that were never the load
//     destination. Under-tainting is the SAFE failure mode (the divergence is
//     reported as real and a human looks); we only propagate through ops modeled
//     as pure dataflow, so we never mark a real-bug value as benign.
//
// Returns a human description for each divergent GPR proven cycle-derived (and
// pushes its index into classified_out), or empty.
static std::vector<std::string> ClassifyCycleDerivedLoads(u32 block_pc,
	const std::vector<int>& divergent_gprs, std::vector<int>* classified_out = nullptr,
	u32 end_pc = 0, const ee_divtrace::FullSnap* entry_snap = nullptr, u32 maxInsns = 48)
{
	std::vector<std::string> out;
	if (divergent_gprs.empty())
		return out;

	u32 regval[32] = {0};
	bool known[32] = {false};
	bool tainted[32] = {false};
	std::string taint_src[32]; // origin description carried with the taint
	known[0] = true; // $zero
	if (entry_snap)
	{
		// Seed the const-tracker with the interpreter's ACTUAL register file at
		// the offending block's entry. Without this, a load whose base address
		// was materialized in an EARLIER block (lui 0x1000 there, ori 0x800
		// here) can't be proven to hit a timer COUNT register and under-taints
		// — the common software-virtual-clock accumulator shape spans blocks.
		// Every unmodeled GPR-writing op below must clear known[] so a seeded
		// value can never go stale into an over-taint.
		for (int i = 1; i < 32; ++i)
		{
			regval[i] = static_cast<u32>(entry_snap->cpu.GPR.r[i].UD[0]);
			known[i] = true;
		}
	}

	auto setTaint = [&](u32 d, bool t, const std::string& src) {
		tainted[d] = t;
		taint_src[d] = t ? src : std::string();
	};

	bool saw_branch = false;
	for (u32 i = 0; i < maxInsns; ++i)
	{
		const u32 addr = block_pc + i * 4;
		// The divergence was OBSERVED entering end_pc — the offending block's
		// body is [block_pc, end_pc). Walking past the observation point (e.g.
		// through a following block's jr + delay slot when the offending block
		// ended at an ei) lets an untainted overwrite in code that never ran
		// erase taint that was live at the observation point.
		if (end_pc && addr >= end_pc)
			break;
		const u32 code = memRead32(addr);
		const u32 op = code >> 26;
		const u32 rs = (code >> 21) & 0x1f;
		const u32 rt = (code >> 16) & 0x1f;
		const u32 rd = (code >> 11) & 0x1f;
		const u32 fn = code & 0x3f;
		const s32 simm = static_cast<s16>(code & 0xffff);
		const u32 uimm = code & 0xffff;

		auto isTimerCount = [](u32 a) {
			// COUNT register of any of the four EE timers (offset 0 within the
			// 0x800-strided bank). Flag the whole COUNT word.
			return (a == 0x10000000 || a == 0x10000800 || a == 0x10001000 || a == 0x10001800);
		};
		auto isCyclePhaseMmio = [&](u32 a) {
			// Any EE hardware-register load whose value reflects transfer/counter
			// PROGRESS is cycle-phase-derived the same way a timer COUNT is: the
			// JIT and interp accumulate cycles at different granularity, so DMAC
			// CHCR/MADR/QWC/TADR tag fields, FIFO counts, GS CSR etc. legitimately
			// read back a different phase. The 0x1000_xxxx bank plus the GS
			// privileged bank. The ResyncAfter reconvergence gate downstream is
			// what keeps this broad class from masking a real codegen bug.
			return isTimerCount(a) || (a >= 0x10000000 && a < 0x10010000) ||
				   (a >= 0x12000000 && a < 0x12002000);
		};

		// Word/dword loads — a timer-COUNT load TAINTS rt; any other load gives rt
		// a fresh untainted value.
		if (op == 0x23 /*LW*/ || op == 0x27 /*LWU*/ || op == 0x37 /*LD*/ || op == 0x1e /*LQ*/)
		{
			if (known[rs] && isCyclePhaseMmio(regval[rs] + static_cast<u32>(simm)))
			{
				const u32 ea = regval[rs] + static_cast<u32>(simm);
				if (isTimerCount(ea))
				{
					const int timer = (ea - 0x10000000) / 0x800;
					setTaint(rt, true, fmt::format("EE Timer {} COUNT ({:#010x}) read at pc={:#010x}", timer, ea, addr));
				}
				else
				{
					setTaint(rt, true, fmt::format("cycle-phase MMIO ({:#010x}) read at pc={:#010x}", ea, addr));
				}
			}
			else
			{
				setTaint(rt, false, {});
			}
			known[rt] = false; // loaded value is not a tracked const
		}
		else if (op == 0x10 /*COP0*/ && rs == 0x00 /*MFC0*/ && rd == 9 /*Count*/)
		{
			// mfc0 rt, $9 reads the COP0 cycle counter directly into rt. Like the
			// EE-timer COUNT MMIO loads, its value differs between the JIT and interp
			// by the per-block-vs-per-op cycle-sync granularity, so it taints rt. The
			// shape here is a Count-based timeout busy-wait (mfc0 Count; dsll32/dsra32
			// sign-extend; sltu vs a deadline) - benign cycle phase, not a codegen bug.
			setTaint(rt, true, fmt::format("COP0 Count (mfc0 $9) read at pc={:#010x}", addr));
			known[rt] = false;
		}
		else if (op == 0x0f /*LUI*/)
		{
			regval[rt] = uimm << 16; known[rt] = true;
			setTaint(rt, false, {}); // immediate — untainted
		}
		else if (op == 0x0d /*ORI*/)
		{
			if (known[rs]) { regval[rt] = regval[rs] | uimm; known[rt] = true; } else known[rt] = false;
			setTaint(rt, tainted[rs], taint_src[rs]);
		}
		else if (op == 0x09 /*ADDIU*/ || op == 0x19 /*DADDIU*/ || op == 0x08 /*ADDI*/ || op == 0x18 /*DADDI*/)
		{
			if (known[rs]) { regval[rt] = regval[rs] + static_cast<u32>(simm); known[rt] = true; } else known[rt] = false;
			setTaint(rt, tainted[rs], taint_src[rs]);
		}
		else if (op == 0x0a /*SLTI*/ || op == 0x0b /*SLTIU*/ || op == 0x0c /*ANDI*/ || op == 0x0e /*XORI*/)
		{
			known[rt] = false; // not const-tracked, but taint flows from rs
			setTaint(rt, tainted[rs], taint_src[rs]);
		}
		else if (op == 0x00 /*SPECIAL*/)
		{
			// R-type pure-dataflow ALU: dst rd, tainted iff any source operand is.
			// Shift-immediate forms (sll/srl/sra/dsll*/dsrl*/dsra*) take only rt.
			const bool isShiftImm =
				(fn == 0x00 || fn == 0x02 || fn == 0x03 ||
				 fn == 0x38 || fn == 0x3a || fn == 0x3b ||
				 fn == 0x3c || fn == 0x3e || fn == 0x3f);
			const bool isAlu =
				(fn >= 0x20 && fn <= 0x2f) || // add/addu/sub/subu/and/or/xor/nor/slt/sltu/dadd..dsubu
				isShiftImm ||
				(fn == 0x04 || fn == 0x06 || fn == 0x07) || // sllv/srlv/srav
				(fn == 0x14 || fn == 0x16 || fn == 0x17);   // dsllv/dsrlv/dsrav
			if (isAlu)
			{
				const bool srcT = isShiftImm ? tainted[rt] : (tainted[rs] || tainted[rt]);
				const std::string& src = tainted[rs] ? taint_src[rs] : taint_src[rt];
				setTaint(rd, srcT, src);
				// Keep the existing OR const-prop; other R-ops invalidate rd's const.
				if (fn == 0x25 /*OR*/ && known[rs] && known[rt]) { regval[rd] = regval[rs] | regval[rt]; known[rd] = true; }
				else known[rd] = false;
			}
			else
			{
				// Unmodeled SPECIAL GPR writer (mfhi/mflo/movz/…): clear rd taint
				// AND const-knowledge (a seeded entry value must not survive an
				// unmodeled write). jr/jalr/sync have rd=0, harmless.
				setTaint(rd, false, {});
				known[rd] = false;
			}
		}
		else if (op == 0x20 || op == 0x21 || op == 0x22 || op == 0x24 || op == 0x25 ||
				 op == 0x26 || op == 0x1a || op == 0x1b)
		{
			// Sub-word / unaligned loads (lb/lh/lwl/lbu/lhu/lwr/ldl/ldr): rt gets
			// a fresh unmodeled value — clear const-knowledge and taint.
			setTaint(rt, false, {});
			known[rt] = false;
		}
		else if (op == 0x03 /*JAL*/)
		{
			setTaint(31, false, {});
			known[31] = false;
		}
		else if (op == 0x1c /*MMI*/)
		{
			setTaint(rd, false, {});
			known[rd] = false;
		}
		else if (op == 0x10 || op == 0x11 || op == 0x12)
		{
			// COPx moves (mfc/cfc land here when not the modeled MFC0 Count):
			// conservatively treat rt as freshly written.
			setTaint(rt, false, {});
			known[rt] = false;
		}
		// (Stores, branches write no GPR — taint and const-knowledge left intact.
		// Any other GPR-writing op we don't recognize is an under-taint, which is
		// the safe direction: the divergence is reported as real, not skipped.)

		if (saw_branch)
			break;
		if (IsEeBranchOpcode(code))
			saw_branch = true;
	}

	// Classify each divergent GPR whose FINAL value is cycle-derived.
	for (int dr : divergent_gprs)
	{
		if (tainted[dr])
		{
			out.push_back(fmt::format(
				"${} is cycle-derived from {} — JIT/interp cycle-sync granularity makes a few-tick delta "
				"EXPECTED, almost certainly NOT a codegen bug.",
				dr, taint_src[dr]));
			if (classified_out)
				classified_out->push_back(dr);
		}
	}
	return out;
}

// Returns the backward-branch target if the basic block at `block_pc` terminates
// in a PC-relative branch that loops back to at/near its own entry (a self-loop),
// else 0. Only PC-relative families (REGIMM, BEQ/BNE/BLEZ/BGTZ, their likely
// variants, COPx BCxF/T) encode a reachable backward target; J/JAL/JR/JALR are
// absolute and not treated as self-loops here. When found, `*branch_addr_out`
// receives the address of the terminating branch (so the caller can bound the
// loop body's pc range, delay slot included).
static u32 BlockBackwardBranchTarget(u32 block_pc, u32* branch_addr_out = nullptr, u32 maxInsns = 64)
{
	for (u32 i = 0; i < maxInsns; ++i)
	{
		const u32 addr = block_pc + i * 4;
		const u32 code = memRead32(addr);
		if (!IsEeBranchOpcode(code))
			continue;
		const u32 op = code >> 26;
		const bool pcrel = (op == 1) || (op >= 4 && op <= 7) || (op >= 0x14 && op <= 0x17) ||
			((op == 0x10 || op == 0x11 || op == 0x12) && ((code >> 21) & 0x1f) == 0x08);
		if (!pcrel)
			return 0; // first terminating branch is absolute — not a self-loop
		const s32 off = static_cast<s16>(code & 0xffff);
		const u32 target = addr + 4 + (static_cast<u32>(off) << 2);
		// Self-loop: backward branch whose target lands in this block's head
		// region (at the entry or a few words before/within it).
		if (target <= addr && target + 8 >= block_pc)
		{
			if (branch_addr_out)
				*branch_addr_out = addr;
			return target;
		}
		return 0; // first terminating branch is forward / out-of-block
	}
	return 0;
}

// Resync past an ENTIRE self-loop run, not one iteration at a time. The JIT
// records one block-entry per loop iteration (all at the loop head pc), so we
// skip every consecutive JIT entry whose pc is in the loop body range
// [loop_lo, loop_hi] to land on the loop EXIT entry — the first JIT entry past
// the loop, carrying the loop's final architectural state. We then find that
// exact exit in the dense interp stream by (pc AND fingerprint) match.
//
// The fingerprint match (not a pc-range skip) is essential AND is the soundness
// check: a branch's delay slot is sampled by the interpreter with pc = branch+8,
// which aliases the loop's fall-through exit pc and recurs EVERY iteration — so a
// pc-only scan can't tell a mid-loop delay slot from the real exit. Only the true
// exit carries the loop's final state, so pc+fp pins it unambiguously. A pure
// sampling-phase artifact reaches an exit state identical to the JIT's; a real
// loop-body codegen bug changes the exit state or trip count, so the JIT exit
// fingerprint never appears in interp → reported as a real lead. Collapses what
// iteration-at-a-time resync would spend the whole benign-skip budget on into a
// single jump.
static ResyncResult ResyncPastSelfLoop(const std::vector<ee_divtrace::Sample>& interp,
	const std::vector<ee_divtrace::Sample>& jit, uint32_t div_k, uint32_t div_ii,
	u32 loop_lo, u32 loop_hi)
{
	auto inLoop = [&](u32 pc) { return pc >= loop_lo && pc <= loop_hi; };
	uint32_t blind = 0;
	size_t k = div_k;
	while (k < jit.size() && inLoop(jit[k].pc)) { ++k; ++blind; }
	if (k >= jit.size())
	{
		ResyncResult r; // loop never exited within the frame
		r.blind = blind;
		return r;
	}
	const u32 exit_pc = jit[k].pc;
	const u64 exit_fp = jit[k].fp;
	size_t ii = div_ii;
	while (ii < interp.size() && !(interp[ii].pc == exit_pc && interp[ii].fp == exit_fp))
		++ii;
	if (ii >= interp.size())
	{
		ResyncResult r; // JIT's loop-exit state never appears in interp — real lead
		r.blind = blind;
		return r;
	}
	ResyncResult r;
	r.reconverged = true;
	r.k = static_cast<uint32_t>(k) + 1;
	r.ii = static_cast<uint32_t>(ii) + 1;
	r.prev_pc = exit_pc;
	r.blind = blind;
	return r;
}

// Checkpoint-anchored zoom: given a checkpoint file holding the state at the
// START of the divergent frame, localize the offending JIT block and the exact
// divergent register field(s). interp (dense per-op) and JIT (sparse per-block)
// both run ONE frame from the SAME checkpoint, so the alignment is clean — no
// cross-pass drift. Prints the block disasm + field diff + fixture next-step.
// Returns true if the walk found something worth STOPPING for (a real codegen
// divergence, a control-flow split, or an inconclusive lead), false if every
// divergence this frame was a benign cycle-derived timer artifact the walk could
// resync past — in which case the caller should keep scanning later frames.
static bool ZoomFromCheckpoint(const std::string& ckpt)
{
	Error error;
	auto reload = [&](bool jit) -> bool {
		if (!VMManager::LoadState(ckpt.c_str(), &error))
		{
			Console.ErrorFmt("zoom: load checkpoint failed: {}", error.GetDescription());
			return false;
		}
		SetEeMode(jit);
		return true;
	};

	Console.WriteLn("STEPDIFF zoom — dense interp pass (one frame from checkpoint)...");
	if (!reload(false))
		return true;
	const auto interp_fine = RunFineFpFromHere();

	Console.WriteLn("STEPDIFF zoom — sparse JIT pass (one frame from checkpoint)...");
	if (!reload(true))
		return true;
	const auto jit_fine = RunFineFpFromHere();

	Console.WriteLn(fmt::format("STEPDIFF zoom — interp {} ops, JIT {} block entries.",
		interp_fine.size(), jit_fine.size()));

	// Iteratively localize. Find the first divergence and classify it: a data
	// divergence whose ONLY differing fields are GPRs loaded from EE timer COUNT
	// registers is the cycle-sync timing artifact — tag it, resync past where the
	// tainted value washes out, and keep hunting. Anything else is a real lead and
	// stops the walk. A cap bounds the per-skip re-run cost.
	size_t k = 1, ii = 0;
	uint32_t prev_pc = jit_fine.empty() ? 0 : jit_fine[0].pc;
	// Two skip budgets. Timer skips each take TWO full-frame re-runs to snapshot
	// the divergent registers, so they are capped tightly. Self-loop phase skips
	// are pure fingerprint-stream walks (no re-run), so they get a far larger cap
	// — a single frame can legitimately contain dozens of short phase-misaligned
	// string/scan loops, and stopping at 32 would falsely report the 33rd.
	int timer_skipped = 0;
	int selfloop_skipped = 0;
	int interrupt_skipped = 0;
	// EERUNNER_CENSUS=1: instead of stopping at the first non-benign data
	// divergence, report it compactly, resync unconditionally, and keep
	// walking. Transient divergences (DMA-phase staging buffers, double-
	// buffered comm regions that reconverge within the frame) get logged and
	// skipped; the walk stops at the first PERSISTENT divergence — the one
	// whose effects never wash out of the register/VU0 fingerprint. Built for
	// the Jak 3 hunt, where the first-stop site was a shared-benign DMA-phase
	// memcpy and the real (arm64-only) VU0-macro corruption came later.
	const char* census_env = std::getenv("EERUNNER_CENSUS");
	const bool census = census_env && census_env[0] != '0';
	int census_sites = 0;
	const int kCensusCap = 64;
	const int kTimerCap = 32;
	const int kSelfLoopCap = 4096;
	const int kInterruptCap = 4096; // stream-only resync, like self-loops

	while (true)
	{
		const int benign_skipped = timer_skipped + selfloop_skipped + interrupt_skipped;
		const AlignResult ar = AlignFrom(interp_fine, jit_fine, k, ii, prev_pc);
		if (!ar.found)
		{
			if (benign_skipped > 0)
			{
				Console.WriteLn(fmt::format(
					"STEPDIFF zoom: no CODEGEN divergence this frame — walked past {} benign divergence(s) "
					"({} cycle-derived timer, {} self-loop phase, {} interrupt-handler phase); the streams "
					"otherwise agree to end-of-frame.",
					benign_skipped, timer_skipped, selfloop_skipped, interrupt_skipped));
				return false; // benign — caller keeps scanning
			}
			Console.WriteLn("STEPDIFF zoom: registers diverged at frame granularity but per-op alignment found "
			                "no block-entry mismatch. The divergence likely lands on state written after the "
			                "last block boundary (event-test / COP path) — inspect the frame-boundary diff.");
			return true; // inconclusive lead — stop for a human look
		}

		if (ar.control_flow)
		{
			// control_flow means jit[k].pc was not found in interp's op-stream
			// AFTER the alignment point. Disambiguate a genuine wrong-target branch
			// from a benign spin-wait PHASE offset: if that pc appears ANYWHERE in
			// interp's stream this frame, the interpreter DID execute it (just at a
			// different iteration of a producer/consumer poll loop — e.g. the GIF
			// double-buffer wait at 0x1f24e0) and the JIT is merely further ahead at
			// the vsync cutoff. Benign — the end-of-frame MEMORY gate (caller) has
			// already proven this frame's persisted state differs only by timer/phase
			// noise, so a poll-loop iteration imbalance here carries no real signal.
			// If the pc appears NOWHERE in interp's stream, the JIT branched to a
			// block the interpreter never reached → a real control-flow codegen bug.
			bool interp_reached = false;
			for (const auto& s : interp_fine)
				if (s.pc == ar.pc) { interp_reached = true; break; }
			if (interp_reached)
			{
				Console.WriteLn(fmt::format(
					"STEPDIFF zoom: spin-wait PHASE offset entering pc={:#010x} (offending block {:#010x}) — the "
					"interpreter DID reach this pc elsewhere this frame; the JIT sits at a different poll-loop "
					"iteration at the vsync cutoff (benign, JIT ran off the end of interp's stream). Continuing the hunt.",
					ar.pc, ar.prev_pc));
				return false; // benign phase — caller keeps scanning later frames
			}
			// Census mode: a control-flow split that RECONVERGES (JIT re-joins
			// interp's pc+fp stream) is a poll-phase artifact — e.g. a DMA-busy
			// flag poll the JIT entered because it reached the check at a
			// different cycle phase (Jak 3: the CHCR.STR-style wait at
			// 0x10df40). Log it and keep walking; a genuine wrong-target branch
			// perturbs state and will not reconverge.
			if (census && census_sites < kCensusCap)
			{
				const ResyncResult rs = ResyncAfter(interp_fine, jit_fine, ar.jit_idx, ar.interp_idx);
				if (rs.reconverged)
				{
					Console.WriteLn(fmt::format(
						"CENSUS site {}: CONTROL-FLOW split at pc={:#010x} (from block {:#010x}, JIT entry #{}) — "
						"TRANSIENT, reconverged after a {}-entry blind window (poll-phase class); continuing.",
						census_sites, ar.pc, ar.prev_pc, ar.jit_idx, rs.blind));
					++census_sites;
					k = rs.k;
					ii = rs.ii;
					prev_pc = rs.prev_pc;
					continue;
				}
				Console.WriteLn(fmt::format(
					"CENSUS site {}: CONTROL-FLOW split at pc={:#010x} — PERSISTENT (never reconverged, blind={}, "
					"ran_off={}). Full report follows.",
					census_sites, ar.pc, rs.blind, rs.ran_off));
				++census_sites;
				// Snapshot the JIT side at the split entry so the guest register
				// operands of the diverging test are visible (e.g. the busy-flag
				// address a poll loop is spinning on), plus both sides' view of
				// the polled word when it resolves to RAM.
				if (reload(true))
				{
					const auto csnap = RunFineSnapAtFromHere(ar.jit_idx);
					static const char* const gprn[32] = {
						"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
						"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
						"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
						"t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"};
					std::string regs;
					for (int gi : {2, 4, 5, 6, 16, 17, 18})
						regs += fmt::format("{}={:#x} ", gprn[gi], csnap.cpu.GPR.r[gi].UD[0]);
					Console.WriteLn(fmt::format("CENSUS split-entry JIT regs: {}", regs));
					const u32 s0addr = static_cast<u32>(csnap.cpu.GPR.r[16].UD[0]);
					if ((s0addr & 0xF0000000u) == 0 && (s0addr & 0x0FFFFFFFu) < Ps2MemSize::MainRam)
					{
						const u32 flagjit = *reinterpret_cast<const u32*>(
							reinterpret_cast<const u8*>(eeMem->Main) + (s0addr & 0x1ffffffu));
						Console.WriteLn(fmt::format(
							"CENSUS split: [s0={:#010x}] (RAM) = {:#010x} on the JIT side at entry.", s0addr, flagjit));
					}
					else
					{
						Console.WriteLn(fmt::format(
							"CENSUS split: s0={:#010x} is not a plain RAM address (HW/scratch/kseg) — poll target is MMIO.",
							s0addr));
					}
					// The re-run left the VM at END of the JIT frame — dump the SPR
					// DMA channel state (both directions) so a wedged channel's
					// CHCR/MADR/QWC/TADR are visible on the JIT side...
					Console.WriteLn(fmt::format(
						"CENSUS split JIT frame-end SPR1(toSPR): chcr={:#010x} madr={:#010x} qwc={:#x} tadr={:#010x} sadr={:#010x}",
						psHu32(0xD400), psHu32(0xD410), psHu32(0xD420), psHu32(0xD430), psHu32(0xD480)));
					Console.WriteLn(fmt::format(
						"CENSUS split JIT frame-end SPR0(fromSPR): chcr={:#010x} madr={:#010x} qwc={:#x} sadr={:#010x}",
						psHu32(0xD000), psHu32(0xD010), psHu32(0xD020), psHu32(0xD080)));
					// ...and the same after a fresh interp frame for comparison.
					if (reload(false))
					{
						AdvanceFrames(1);
						Console.WriteLn(fmt::format(
							"CENSUS split INTERP frame-end SPR1(toSPR): chcr={:#010x} madr={:#010x} qwc={:#x} tadr={:#010x} sadr={:#010x}",
							psHu32(0xD400), psHu32(0xD410), psHu32(0xD420), psHu32(0xD430), psHu32(0xD480)));
						Console.WriteLn(fmt::format(
							"CENSUS split INTERP frame-end SPR0(fromSPR): chcr={:#010x} madr={:#010x} qwc={:#x} sadr={:#010x}",
							psHu32(0xD000), psHu32(0xD010), psHu32(0xD020), psHu32(0xD080)));
					}
				}
			}
			Console.WriteLn(fmt::format(
				"STEPDIFF zoom: CONTROL-FLOW divergence — JIT dispatched to block pc={:#010x} the interpreter "
				"NEVER reached this frame. Offending JIT block: pc={:#010x} (terminating branch went to the wrong target).",
				ar.pc, ar.prev_pc));
			if (ar.prev_pc)
				DisasmBlock(ar.prev_pc);
			return true;
		}

		// Self-loop PHASE misalignment — checked FIRST and CHEAPLY (no snapshot).
		// The offending block is a tight backward self-loop; the JIT folds the
		// loop's first iteration into the preceding block, so its loop-head sample
		// runs one iteration ahead of the interpreter's dense per-op samples. Both
		// cores compute the SAME results — a sampling-phase artifact, not a codegen
		// bug. ResyncPastSelfLoop works on the fingerprint streams ALONE: it skips
		// the whole loop run and proves convergence at the loop EXIT (final state
		// identical). A real loop-body bug changes the exit state or trip count and
		// will NOT converge → it falls through to the snapshot + real report below.
		// Because this needs no re-run, it gets the large self-loop cap, so a frame
		// full of short scan loops doesn't exhaust the tight timer budget.
		{
			u32 loop_branch_addr = 0;
			// The self-loop can be the block being ENTERED (a re-entry sample), OR
			// the offending block just executed (ar.prev_pc): the interpreter
			// samples the loop's terminating-branch delay slot at branch+8, which
			// aliases the loop's fall-through EXIT pc every iteration — so the
			// divergence is reported "entering <exit>" with the loop sitting in
			// prev_pc. Check the entered block first, then the offending block.
			u32 loop_target = BlockBackwardBranchTarget(ar.pc, &loop_branch_addr);
			if (loop_target == 0 && ar.prev_pc)
				loop_target = BlockBackwardBranchTarget(ar.prev_pc, &loop_branch_addr);
			if (loop_target != 0 && selfloop_skipped < kSelfLoopCap)
			{
				const u32 loop_lo = loop_target;
				const u32 loop_hi = loop_branch_addr + 4; // include the branch's delay slot
				const ResyncResult rs = ResyncPastSelfLoop(
					interp_fine, jit_fine, ar.jit_idx, ar.interp_idx, loop_lo, loop_hi);
				if (rs.reconverged)
				{
					Console.WriteLn(fmt::format(
						"STEPDIFF zoom: skipping self-loop phase misalignment entering pc={:#010x} (loop {:#010x}..{:#010x}, "
						"branches back to {:#010x}). The JIT folds the loop's first iteration into the preceding block, so "
						"its loop-head sample runs one iteration ahead of interp; skipped the whole {}-entry loop run and "
						"converged at the exit. Continuing the hunt.",
						ar.pc, loop_lo, loop_hi, loop_target, rs.blind));
					++selfloop_skipped;
					k = rs.k;
					ii = rs.ii;
					prev_pc = rs.prev_pc;
					continue;
				}
				// Structural self-loop but the exit did NOT converge — not a mere
				// phase offset. Fall through to the snapshot + real-divergence report.
			}
		}

		// EXCEPTION/INTERRUPT-HANDLER PHASE — checked CHEAPLY (stream-only, no
		// snapshot) before the expensive data path. The divergence is entered inside
		// the EE kernel exception handler (kseg0, pc >= 0x8000_0000): the JIT and
		// interp took the same vblank/timer interrupt a few cycles apart in an idle
		// poll loop, so EPC differs by an instruction and that value propagates into
		// whatever scratch GPRs the handler touches (k0→t0→…). The handler dispatches
		// on Cause (not EPC), saves+restores the FULL user register context, and
		// ERETs to EPC — so on return to user code every user GPR is restored
		// IDENTICAL and only EPC (excluded from the fingerprint) differs. ResyncAfter
		// therefore reconverges the instant the handler returns. Gating on
		// reconvergence is the safety net: a genuine kernel-codegen bug changes the
		// post-return state and will NOT reconverge, falling through to the report.
		if ((ar.pc >= 0x80000000u || ar.prev_pc >= 0x80000000u) && interrupt_skipped < kInterruptCap)
		{
			const ResyncResult rs = ResyncAfter(interp_fine, jit_fine, ar.jit_idx, ar.interp_idx);
			if (rs.reconverged)
			{
				++interrupt_skipped;
				k = rs.k;
				ii = rs.ii;
				prev_pc = rs.prev_pc;
				continue;
			}
			// Did not reconverge within the frame — not a benign handler phase.
			// Fall through to the snapshot + real-divergence report.
		}

		// DATA divergence — snapshot both streams at the entry (per-stream indices:
		// interp dense vs JIT sparse) and classify.
		if (!reload(false))
			return true;
		const auto isnap = RunFineSnapAtFromHere(ar.interp_idx);
		if (!reload(true))
			return true;
		const auto jsnap = RunFineSnapAtFromHere(ar.jit_idx);

		std::vector<int> divergent_gprs;
		for (int i = 0; i < 32; ++i)
			if (jsnap.cpu.GPR.r[i].UD[0] != isnap.cpu.GPR.r[i].UD[0] ||
				jsnap.cpu.GPR.r[i].UD[1] != isnap.cpu.GPR.r[i].UD[1])
				divergent_gprs.push_back(i);

		// Seed the timer-taint classifier with the interpreter's register file at
		// the offending block's ENTRY (the last interp sample at prev_pc before
		// the divergence index) so load addresses materialized in earlier blocks
		// resolve. Costs one extra frame re-run per data divergence.
		ee_divtrace::FullSnap esnap{};
		bool have_esnap = false;
		if (ar.prev_pc)
		{
			for (u32 pi = ar.interp_idx; pi-- > 0;)
			{
				if (interp_fine[pi].pc == ar.prev_pc)
				{
					if (!reload(false))
						return true;
					esnap = RunFineSnapAtFromHere(pi);
					have_esnap = true;
					break;
				}
			}
		}

		std::vector<int> classified;
		const auto timer_notes = ClassifyCycleDerivedLoads(ar.prev_pc, divergent_gprs, &classified, ar.pc,
			have_esnap ? &esnap : nullptr);

		const bool fully_benign =
			!divergent_gprs.empty() &&
			classified.size() == divergent_gprs.size() &&
			!NonGprDiffers(jsnap, isnap);

		if (fully_benign && timer_skipped < kTimerCap)
		{
			const ResyncResult rs = ResyncAfter(interp_fine, jit_fine, ar.jit_idx, ar.interp_idx);
			if (rs.ran_off)
			{
				Console.WriteLn(fmt::format(
					"STEPDIFF zoom: benign cycle-derived timer divergence entering pc={:#010x} ({}), but control "
					"flow then perturbed within the blind window ({} entries) — the timer value propagated into a "
					"branch. Can't cleanly look past it here; re-run from a later savestate.",
					ar.pc, timer_notes.front(), rs.blind));
				if (ar.prev_pc)
					DisasmBlock(ar.prev_pc);
				return true;
			}
			if (!rs.reconverged)
			{
				Console.WriteLn(fmt::format(
					"STEPDIFF zoom: benign cycle-derived timer divergence entering pc={:#010x} ({}); state never "
					"re-converged before end-of-frame ({} entries stayed tainted). The timing difference persisted "
					"— re-run from a later savestate to look past it.",
					ar.pc, timer_notes.front(), rs.blind));
				return true;
			}
			Console.WriteLn(fmt::format(
				"STEPDIFF zoom: skipping benign timer divergence entering pc={:#010x} (offending block {:#010x}; {}). "
				"Resynced after a {}-entry blind window; continuing the hunt.",
				ar.pc, ar.prev_pc, timer_notes.front(), rs.blind));
			++timer_skipped;
			k = rs.k;
			ii = rs.ii;
			prev_pc = rs.prev_pc;
			continue;
		}

		// Census mode: log the site, resync unconditionally, continue while the
		// divergence proves transient. Falls through to the full report (stop)
		// on the first site that never reconverges, or at the cap.
		if (census && census_sites < kCensusCap)
		{
			const auto cdiffs = DiffFullSnaps(jsnap, isnap);
			Console.WriteLn(fmt::format(
				"CENSUS site {}: entering pc={:#010x} (offending block {:#010x}, JIT entry #{}), {} divergent field(s):",
				census_sites, ar.pc, ar.prev_pc, ar.jit_idx, cdiffs.size()));
			for (size_t di = 0; di < cdiffs.size() && di < 10; ++di)
				Console.WriteLn(fmt::format("    {}", cdiffs[di]));
			if (cdiffs.size() > 10)
				Console.WriteLn(fmt::format("    ... (+{} more)", cdiffs.size() - 10));
			const ResyncResult rs = ResyncAfter(interp_fine, jit_fine, ar.jit_idx, ar.interp_idx);
			if (rs.reconverged)
			{
				Console.WriteLn(fmt::format(
					"CENSUS site {}: TRANSIENT — reconverged after a {}-entry blind window; continuing.",
					census_sites, rs.blind));
				++census_sites;
				k = rs.k;
				ii = rs.ii;
				prev_pc = rs.prev_pc;
				continue;
			}
			Console.WriteLn(fmt::format(
				"CENSUS site {}: PERSISTENT — never reconverged (blind={}, ran_off={}). Full report follows.",
				census_sites, rs.blind, rs.ran_off));
			++census_sites;
			// fall through to the full report + stop
		}

		// REAL divergence (or benign-cap reached): full report + stop.
		Console.WriteLn(fmt::format(
			"STEPDIFF zoom: DATA divergence observed entering block pc={:#010x} (JIT block-entry #{}). "
			"Offending JIT block: entry pc={:#010x} — its body produced register state differing from interp.",
			ar.pc, ar.jit_idx, ar.prev_pc));
		if (ar.prev_pc)
			DisasmBlock(ar.prev_pc);

		// Offending-block ENTRY probe: dump BOTH streams' GPR file at prev_pc
		// (the offending block's head). Tells apart "entered already-diverged =
		// upstream culprit" from "entered identical, exits diverged = this block
		// is the bug", and hands a single-block fixture the real seed values.
		if (ar.prev_pc)
		{
			auto entry_snap = [&](const std::vector<ee_divtrace::Sample>& stream, uint32_t upto,
								   bool jit_side, ee_divtrace::FullSnap& out) -> bool {
				for (uint32_t pi = upto; pi-- > 0;)
				{
					if (stream[pi].pc == ar.prev_pc)
					{
						if (!reload(jit_side))
							return false;
						out = RunFineSnapAtFromHere(pi);
						return true;
					}
				}
				return false;
			};
			ee_divtrace::FullSnap ientry{}, jentry{};
			const bool hi = entry_snap(interp_fine, ar.interp_idx, false, ientry);
			const bool hj = entry_snap(jit_fine, ar.jit_idx, true, jentry);
			if (hi && hj)
			{
				Console.WriteLn(fmt::format("STEPDIFF zoom: offending-block ENTRY GPRs @ {:#010x} (interp | jit):", ar.prev_pc));
				for (int r = 0; r < 32; ++r)
				{
					const u64 iv = ientry.cpu.GPR.r[r].UD[0];
					const u64 jv = jentry.cpu.GPR.r[r].UD[0];
					if (iv != jv || (r >= 4 && r <= 7))
						Console.WriteLn(fmt::format("    r{:<2} {:#018x} | {:#018x}{}", r, iv, jv,
							iv != jv ? "   <-- DIFFERS" : ""));
				}
				Console.WriteLn("STEPDIFF zoom: offending-block ENTRY FPRs (interp | jit; f0-f3,f12 + diffs):");
				for (int f = 0; f < 32; ++f)
				{
					const u32 iv = ientry.fpu.fpr[f].Word();
					const u32 jv = jentry.fpu.fpr[f].Word();
					if (iv != jv || f <= 3 || f == 12)
						Console.WriteLn(fmt::format("    f{:<2} {:#010x} | {:#010x}{}", f, iv, jv,
							iv != jv ? "   <-- DIFFERS" : ""));
				}
			}
		}

		const auto diffs = DiffFullSnaps(jsnap, isnap);
		if (diffs.empty())
		{
			Console.WriteLn("STEPDIFF zoom: (detail re-run did not reproduce the field diff at the entry — the "
			                "divergence may be mid-block / in memory; the block disasm above is the lead.)");
		}
		else
		{
			Console.WriteLn(fmt::format("STEPDIFF zoom: divergent register field(s) entering block pc={:#010x}:", ar.pc));
			for (const auto& d : diffs)
				Console.WriteLn(fmt::format("    {}", d));
			if (!timer_notes.empty())
			{
				Console.WriteLn("STEPDIFF zoom: SUSPECTED TIMING (some divergent GPRs are cycle-derived timer reads, "
				                "but NOT all divergent state is — see above):");
				for (const auto& n : timer_notes)
					Console.WriteLn(fmt::format("    {}", n));
			}
		}
		if (timer_skipped >= kTimerCap)
			Console.WriteLn(fmt::format(
				"STEPDIFF zoom: NOTE — hit the timer-skip cap ({}); this report may itself be another timer "
				"artifact. Re-run from a later savestate if so.", kTimerCap));
		else if (timer_skipped + selfloop_skipped > 0)
			Console.WriteLn(fmt::format(
				"STEPDIFF zoom: (walked past {} benign divergence(s) — {} timer, {} self-loop phase — before "
				"reaching this one.)", timer_skipped + selfloop_skipped, timer_skipped, selfloop_skipped));
		Console.WriteLn(fmt::format(
			"STEPDIFF zoom: offending block pc={:#010x} — capture a single-block EE fixture there to pin the opcode.",
			ar.prev_pc));
		return true;
	}
}


// ===========================================================================
// --stepdiff : checkpoint-anchored per-frame interp-vs-JIT comparison.
//
// The frame-boundary funnel (--localize) runs interp and JIT as two separate
// full passes and diffs them at frame boundaries. That conflates two things:
// real codegen divergence, and the ~10-cycle async sampling jitter at the
// frame-advance pause point (proven by --selfcheck: two warm interp runs
// already disagree at the boundary, yet committed memory re-captures identical).
// Accumulated over a pass, that jitter looks exactly like a JIT bug — a
// diagnostic tarpit.
//
// --stepdiff removes the accumulation: at each frame it CHECKPOINTS the VM
// (in-flight savestate), then runs ONE frame three times from that IDENTICAL
// state — interp twice (a determinism control) and JIT once. From a shared
// checkpoint:
//   * interp-vs-interp divergence  => async sampling jitter (this frame is noisy)
//   * interp-vs-interp clean + interp-vs-JIT divergence => a REAL EE JIT bug
// The golden timeline is advanced one interp frame between checkpoints, so the
// scan walks the whole run while every comparison starts from a clean state.
// ===========================================================================
// --contmem : continuous-trajectory memory diff. The checkpoint-anchored
// --stepdiff re-anchors to the golden interp state every frame, so it can NEVER
// reproduce an ACCUMULATION bug (drift that builds over ~1s of CONTINUOUS JIT —
// the Burnout 3 physics-explosion shape); it also fights LoadState pause-point
// jitter. This instead runs interp CONTINUOUSLY for the whole window
// (deterministic, per --selfcheck) twice as a control + JIT CONTINUOUSLY once,
// then diffs the per-frame memory-hash trajectories. The first frame whose
// hashes differ WITH a clean interp control is where continuous JIT first
// deviates; on it the EE-RAM region is localized via a byte diff. Cross-arch:
// run the SAME invocation on x86 — if x86 also diverges early the divergence is
// shared benign timing, if x86 stays clean it's an arm64-specific EE-JIT bug.
// `--vu0-interp` forces VU0=interp in every pass so the only cross-pass
// difference stays the EE engine (isolates EE-COP1-FPU/integer from VU0/COP2).
static int RunContinuousMemTrajectory()
{
	Error error;
	if (!ValidateEeRateScript())
		return EXIT_FAILURE;

	const bool force_vu0_interp = s_vu0_interp;
	int pass_no = 0;
	auto runPass = [&](bool jit, std::vector<uint64_t>* cycles = nullptr) -> std::vector<uint64_t> {
		std::vector<uint64_t> hashes;
		if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
		{
			Console.ErrorFmt("contmem: load failed: {}", error.GetDescription());
			return hashes;
		}
		SetEeMode(jit);
		if (force_vu0_interp)
		{
			s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", false);
			VMManager::ApplySettings();
		}
		hashes.reserve(s_frames);
		EeRateDriver rate(fmt::format("contmem-{}{}", jit ? "jit" : "interp", ++pass_no));
		for (uint32_t f = 0; f < s_frames && VMManager::GetState() != VMState::Shutdown; ++f)
		{
			const EeRateFrameTimer timer; // starts before the transition: the reset is part of the frame that pays it
			rate.BeforeFrame(f);
			VMManager::FrameAdvance(1);
			VMManager::Execute();
			rate.AfterFrame(timer.Elapsed());
			hashes.push_back(ee_divtrace::HashMemory());
			if (cycles)
				cycles->push_back(static_cast<uint64_t>(cpuRegs.cycle));
		}
		return hashes;
	};

	// Cycle-drift trajectory: EE cpuRegs.cycle at each frame boundary, JIT vs interp.
	// This is DETERMINISTIC (unlike the chaotic memory diff): if the JIT's per-block
	// emitted cycle cost matches the interpreter's, the two cycle counts stay locked;
	// a growing |jit.cycle - interp.cycle| is the EE-JIT cycle-accounting drift that
	// shifts every cycle-clocked subsystem (DMA/VIF/timers) out of phase. Cross-arch:
	// if arm64's per-frame drift >> x86's, arm64 EE block-cycle costs are the bug.
	std::vector<uint64_t> ic, jc;
	Console.WriteLn("CONTMEM: interp pass 1 (continuous)...");
	const auto i1 = runPass(false, &ic);
	Console.WriteLn("CONTMEM: interp pass 2 (continuous, determinism control)...");
	const auto i2 = runPass(false);
	Console.WriteLn("CONTMEM: JIT pass (continuous)...");
	const auto j = runPass(true, &jc);

	// Cycle-drift report (deterministic; compare arm64's vs x86's numbers cross-arch).
	{
		const size_t cn = std::min(ic.size(), jc.size());
		int64_t maxabs = 0;
		size_t maxf = 0;
		for (size_t f = 0; f < cn; ++f)
		{
			const int64_t d = (int64_t)jc[f] - (int64_t)ic[f];
			if (std::llabs(d) > std::llabs(maxabs)) { maxabs = d; maxf = f; }
		}
		Console.WriteLn("CONTMEM CYCLE-DRIFT (EE cpuRegs.cycle, jit - interp, per frame):");
		for (size_t f = 0; f < cn; ++f)
		{
			const int64_t d = (int64_t)jc[f] - (int64_t)ic[f];
			if (f < 12 || f + 4 >= cn || std::llabs(d) == std::llabs(maxabs))
				Console.WriteLn(fmt::format("  frame {:3}: interp.cycle={} jit.cycle={} drift={:+d}", f, ic[f], jc[f], d));
		}
		Console.WriteLn(fmt::format("CONTMEM CYCLE-DRIFT SUMMARY: max |drift| = {:+d} EE cycles at frame {} (of {} frames).",
			maxabs, maxf, cn));
	}

	const size_t n = std::min({i1.size(), i2.size(), j.size()});
	int first_ctrl = -1, first_real = -1;
	for (size_t f = 0; f < n; ++f)
	{
		const bool ctrl_div = i1[f] != i2[f];
		const bool jit_div = i1[f] != j[f];
		if (ctrl_div && first_ctrl < 0)
			first_ctrl = (int)f;
		if (jit_div && !ctrl_div && first_real < 0)
			first_real = (int)f;
		Console.WriteLn(fmt::format("CONTMEM frame {:3}: interp1={:#018x} interp2={:#018x} jit={:#018x}  ctrl={} jit-vs-interp={}",
			f, i1[f], i2[f], j[f], ctrl_div ? "DIFF" : "ok", jit_div ? "DIFF" : "ok"));
	}
	Console.WriteLn(fmt::format(
		"CONTMEM SUMMARY: {} frames; interp determinism first breaks at frame {} ; "
		"continuous JIT-vs-interp memory first diverges (with clean interp control) at frame {}.",
		n, first_ctrl, first_real));
	// Capture EE main RAM + scratchpad after running `frame+1` continuous frames
	// in the given mode (honors --vu0-interp), for an interp-vs-JIT byte-region diff.
	auto capMem = [&](bool jit, uint32_t frame) -> std::vector<u8> {
		std::vector<u8> out;
		if (!VMManager::LoadState(s_savestate_path.c_str(), &error)) return out;
		SetEeMode(jit);
		if (force_vu0_interp)
		{
			s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", false);
			VMManager::ApplySettings();
		}
		AdvanceFrames(frame + 1);
		out.resize(Ps2MemSize::MainRam + Ps2MemSize::Scratch);
		std::memcpy(out.data(), eeMem->Main, Ps2MemSize::MainRam);
		std::memcpy(out.data() + Ps2MemSize::MainRam, eeMem->Scratch, Ps2MemSize::Scratch);
		return out;
	};

	if (first_real >= 0)
	{
		Console.WriteLn(fmt::format("CONTMEM: EE-RAM region @ FIRST clean-control JIT divergence (frame {}) — divergence onset:", first_real));
		ReportMemDiff(capMem(false, (uint32_t)first_real), capMem(true, (uint32_t)first_real));
	}
	// Magnitude trajectory: diff again at the LAST frame. A BOUNDED drift stays a
	// similar page/byte count to the onset frame (→ the divergence is benign timing
	// phase, the explosion is elsewhere); an EXPLOSION balloons to thousands of
	// pages of NaN/garbage physics (→ this divergence IS the corruption). Interp's
	// own late-frame nondeterminism adds only a small page-count floor, far below an
	// explosion's footprint.
	if (n >= 2)
	{
		const uint32_t lastf = (uint32_t)n - 1;
		Console.WriteLn(fmt::format("CONTMEM: interp-vs-interp CONTROL region @ LAST frame ({}) — nondeterminism noise floor:", lastf));
		ReportMemDiff(capMem(false, lastf), capMem(false, lastf));
		Console.WriteLn(fmt::format("CONTMEM: EE-RAM region @ LAST frame ({}) — JIT-vs-interp; subtract the control floor above:", lastf));
		ReportMemDiff(capMem(false, lastf), capMem(true, lastf));
	}

	// --memdump: write the raw EE main RAM + scratchpad of the interp and jit passes
	// at the LAST frame, for a cross-machine JIT-vs-JIT diff. Interp is bit-identical
	// cross-arch (deterministic IEEE) and EE cycles are locked, so diffing arm64's
	// .jit.bin vs x86's .jit.bin isolates the arm64-specific COMPUTATIONAL EE-JIT
	// divergence directly. Use a SMALL --frames (e.g. 2-3) so chaotic amplification
	// hasn't spread the seed yet. The .interp.bin pair should be byte-identical
	// cross-arch (a sanity check on cross-arch determinism).
	if (!s_memdump_prefix.empty() && n >= 1)
	{
		const uint32_t lastf = (uint32_t)n - 1;
		auto dump = [&](bool jit, const char* tag) {
			const std::vector<u8> m = capMem(jit, lastf);
			const std::string path = fmt::format("{}.{}.bin", s_memdump_prefix, tag);
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			if (!f) { Console.ErrorFmt("CONTMEM: failed to open {}", path); return; }
			f.write(reinterpret_cast<const char*>(m.data()), static_cast<std::streamsize>(m.size()));
			Console.WriteLn(fmt::format("CONTMEM: wrote {} ({} bytes, frame {}).", path, m.size(), lastf));
		};
		dump(false, "interp");
		dump(true, "jit");
	}
	ReportEeChargeCensus("contmem end");
	if (s_ee_rate_failed)
	{
		Console.Error("CONTMEM: FAILED — a scripted EE cycle-rate transition was refused or reset a "
		              "provider it must not. See the EERATE lines above.");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// --twindump / --twincompare : cross-BUILD divergence finder ("twindiff").
//
// Motivation: a JIT transplant regressed games the pre-transplant tree (its
// own independent JIT stack) runs correctly. The in-tree oracles compare a
// JIT against ITS OWN interpreter, so they are structurally blind to two
// classes: (a) shared-C++ semantic drift between the trees (both engines in
// one tree agree with each other AND with the bug), and (b) behavior the
// WORKING tree's JIT has that the interpreter also lacks (True Crime
// guard-bit adds: interp never masked, so jit-vs-interp reads "clean" while
// the game needs the masking).
//
// Design: the same byte-identical runner is built in BOTH trees. From one
// shared savestate each binary makes three free-running passes over --frames:
//   i1, i2  full interpreter (EE+VU0+VU1+IOP interp); i2 is the determinism
//           control. The interp cells are pure shared-C++.
//   j       the full production JIT stack of that tree.
// recording per frame: xxh3 of EE RAM+scratch (HashMemory), the architectural
// register fingerprint (FingerprintCpu), and cpuRegs.cycle. --twindump writes
// <prefix>.twin; --twincompare re-runs the passes locally and diffs:
//
//   pair XI  other.i1 vs local.i1  -> shared-C++ semantic drift (sharp: both
//                                     sides claim identical semantics AND
//                                     identical cycle accounting)
//   pair TJ  other.j  vs other.i1  -> the other tree's JIT-vs-interp envelope
//   pair OJ  local.j  vs local.i1  -> local JIT bug (hand to --stepdiff)
//   pair EE  other.j  vs local.j   -> end-to-end working-vs-broken headline
//
// TJ/OJ/EE carry benign divergence by construction: interp and each JIT use
// different cycle accounting, so timer-derived values land in RAM and drift
// the memhash. Adjudicate those with the RAM page-diff character (scattered
// small deltas = timing noise; structured buffers = value bug) and config A/B
// (--set). The guard-bit signature: XI clean + OJ clean-ish + TJ divergent +
// EE divergent. Memhash is primary; regfp is advisory (frame-pause register
// sampling jitters ~10 cycles). Free-running passes accumulate divergence, so
// keep --frames modest and the savestate near the symptom — the i1-vs-i2
// control reports where each tree's own determinism floor breaks.
// ---------------------------------------------------------------------------

namespace
{
	struct TwinRec
	{
		u64 mem;
		u64 reg;
		u64 cyc;
		// IOP-side hashes (ee_divtrace::FingerprintIop); recorded only with
		// EERUNNER_SYSFULL=1, zero otherwise and in files from before they existed.
		u64 spu2 = 0;
		u64 sif = 0;
		u64 sio = 0;
		u64 cdvd = 0;
	};

	struct TwinTrace
	{
		std::string label;
		std::string serial;
		uint32_t frames = 0;
		int vu0fp = 0;
		int nofp = 0;
		int vu0full = 0;
		int sysfull = 0;
		int iopfp = 0; // the records carry the IOP-side hashes
		std::vector<TwinRec> i1, i2, j;
	};
} // namespace

// Force the WHOLE CPU stack (EE, VU0, VU1, IOP) to interp or to this tree's
// JITs. The twin interp cells must be pure shared-C++: with only EnableEE
// toggled (what SetEeMode does for the in-tree modes), the two trees'
// DIFFERENT VU/IOP JITs would leak into the "interpreter" comparison and
// conflate divergence classes.
static void SetAllCpuMode(bool jit)
{
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", jit);
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", jit);
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", jit);
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", jit);
	VMManager::ApplySettings();
}

#ifdef PCSX2_RECOMPILER_TESTS
extern u64 g_twinEeEventTests;
extern u64 g_twinIopSlices;
namespace mvu_test_hooks
{
	u64 GetVu0ExecuteCount();
	u64 GetSpinBounceCount(int vu_index);
} // namespace mvu_test_hooks
#endif

// --settings-churn: the settings change a pass applies every N frames. The settings the run began with
// are remembered and put back at the start of each pass, so every pass sees the same sequence.
static void ApplyChurnBaseline(bool restore)
{
	struct Base
	{
		bool lockstep, iop_thread, vuaot;
		int lag;
	};
	static std::optional<Base> base;
	if (!base)
	{
		base = Base{s_settings_interface.GetBoolValue("EmuCore/Speedhacks", "iopThreadLockstep", false),
			s_settings_interface.GetBoolValue("EmuCore/Speedhacks", "iopThread", false),
			s_settings_interface.GetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", false),
			s_settings_interface.GetIntValue("EmuCore/Speedhacks", "iopThreadLag", 6144)};
	}
	if (!restore)
		return;
	s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "iopThreadLockstep", base->lockstep);
	s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "iopThread", base->iop_thread);
	s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", base->vuaot);
	s_settings_interface.SetIntValue("EmuCore/Speedhacks", "iopThreadLag", base->lag);
	VMManager::ApplySettings();
}

static u32 s_stress_lag_pick = 0;

static void ApplySettingsChurn()
{
	if (s_churn_kind == "lockstep")
	{
		const bool on = s_settings_interface.GetBoolValue("EmuCore/Speedhacks", "iopThreadLockstep", false);
		s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "iopThread", !on);
		s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "iopThreadLockstep", !on);
	}
	else if (s_churn_kind == "threaded")
	{
		const bool on = s_settings_interface.GetBoolValue("EmuCore/Speedhacks", "iopThread", false);
		s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "iopThread", !on);
		s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "iopThreadLockstep", false);
		if (s_stress)
			Console.WriteLn(fmt::format("STRESS: settings change: iopThread {} -> {}", on ? "on" : "off", on ? "off" : "on"));
	}
	else if (s_churn_kind == "vuaot")
	{
		const bool on = s_settings_interface.GetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", false);
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", !on);
		Console.WriteLn(fmt::format("CHURN: EnableVUAot {} -> {}", on ? "on" : "off", on ? "off" : "on"));
	}
	else if (s_stress && s_churn_kind == "lag-random")
	{
		static constexpr int kLags[] = {4096, 6144, 8192, 12288, 24576, 196608};
		const int lag = kLags[s_stress_lag_pick % (sizeof(kLags) / sizeof(kLags[0]))];
		s_settings_interface.SetIntValue("EmuCore/Speedhacks", "iopThreadLag", lag);
		Console.WriteLn(fmt::format("STRESS: settings change: iopThreadLag {}", lag));
	}
	else
	{
		const int lag = s_settings_interface.GetIntValue("EmuCore/Speedhacks", "iopThreadLag", 6144);
		s_settings_interface.SetIntValue("EmuCore/Speedhacks", "iopThreadLag", lag == 6144 ? 8192 : 6144);
	}
	VMManager::ApplySettings();
}

// One free-running pass from the start savestate. Optionally dumps raw EE
// RAM+scratch at --twin-ram-at to <prefix>.f<N>.{interp,jit}.bin (skipped for
// the i2 control twin).
static bool TwinRunPass(bool jit, const char* cell, std::vector<TwinRec>& out)
{
	Error error;
	out.clear();
	if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
	{
		Console.ErrorFmt("twin: load failed: {}", error.GetDescription());
		return false;
	}
	if (s_churn_every)
	{
		s_churn_pending = false;
		ApplyChurnBaseline(true);
	}
	SetAllCpuMode(jit);
	out.reserve(s_frames);
#ifdef PCSX2_RECOMPILER_TESTS
	const u64 executes0 = mvu_test_hooks::GetVu0ExecuteCount();
	const u64 bounces0 = mvu_test_hooks::GetSpinBounceCount(0);
	// The whole-system fingerprint mixes these in; count from this pass's start.
	g_twinEeEventTests = 0;
	g_twinIopSlices = 0;
#endif
	for (uint32_t f = 0; f < s_frames && VMManager::GetState() != VMState::Shutdown; ++f)
	{
		VMManager::FrameAdvance(1);
		VMManager::Execute();
		// A settings change applied during the frame resets the recompilers and leaves the execution
		// loop early, as it does for a player; carry on until the frame ends.
		while (s_churn_every && VMManager::GetState() == VMState::Running)
			VMManager::Execute();
		TwinRec r;
		r.mem = ee_divtrace::HashMemory();
		r.reg = ee_divtrace::FingerprintCpu();
		r.cyc = cpuRegs.cycle;
#ifdef PCSX2_RECOMPILER_TESTS
		if (ee_divtrace::g_sys_full)
		{
			const ee_divtrace::IopFingerprint io = ee_divtrace::FingerprintIop();
			r.spu2 = io.spu2;
			r.sif = io.sif;
			r.sio = io.sio;
			r.cdvd = io.cdvd;
		}
#endif
		out.push_back(r);
		if (s_churn_every && ((f + 1) % s_churn_every) == 0)
			s_churn_pending = true; // applied at the next frame's vsync, inside the event test
		if (s_twin_ram_at >= 0 && f == static_cast<uint32_t>(s_twin_ram_at) &&
			std::strcmp(cell, "i2") != 0 && !s_twin_prefix.empty())
		{
			const std::string path = fmt::format("{}.f{}.{}.bin", s_twin_prefix, f, jit ? "jit" : "interp");
			std::ofstream bf(path, std::ios::binary | std::ios::trunc);
			if (bf)
			{
				bf.write(reinterpret_cast<const char*>(eeMem->Main), Ps2MemSize::MainRam);
				bf.write(reinterpret_cast<const char*>(eeMem->Scratch), Ps2MemSize::Scratch);
				Console.WriteLn(fmt::format("twin: wrote {} ({} bytes).", path,
					static_cast<size_t>(Ps2MemSize::MainRam) + Ps2MemSize::Scratch));
			}
			else
				Console.ErrorFmt("twin: failed to open {}", path);
		}
	}
	Console.WriteLn(fmt::format("twin: pass {} done ({} frames).", cell, out.size()));
#ifdef PCSX2_RECOMPILER_TESTS
	// VU0 dispatches that reached recMicroVU0::Execute, and how many of those
	// Execute answered with its spin bounce. Zero on an interp pass.
	Console.WriteLn(fmt::format("twin: pass {} vu0 Execute calls {}, C++ spin bounces {}.", cell,
		mvu_test_hooks::GetVu0ExecuteCount() - executes0, mvu_test_hooks::GetSpinBounceCount(0) - bounces0));
	Console.WriteLn(fmt::format("twin: pass {} EE event tests {}, IOP slices {}.", cell, g_twinEeEventTests, g_twinIopSlices));
#endif
	return true;
}

static bool TwinRunAll(TwinTrace& t)
{
	t.label = s_twin_label;
	t.serial = VMManager::GetDiscSerial();
	t.frames = s_frames;
	t.vu0fp = ee_divtrace::g_vu0_include ? 1 : 0;
	t.nofp = ee_divtrace::g_fp_exclude ? 1 : 0;
	t.vu0full = ee_divtrace::g_vu0_full ? 1 : 0;
	t.sysfull = ee_divtrace::g_sys_full ? 1 : 0;
#ifdef PCSX2_RECOMPILER_TESTS
	t.iopfp = ee_divtrace::g_sys_full ? 1 : 0;
#endif
	return TwinRunPass(false, "i1", t.i1) &&
		   TwinRunPass(false, "i2", t.i2) &&
		   TwinRunPass(true, "j", t.j);
}

static bool WriteTwinTrace(const TwinTrace& t, const std::string& path)
{
	std::ofstream f(path, std::ios::trunc);
	if (!f)
	{
		Console.ErrorFmt("twin: failed to open {}", path);
		return false;
	}
	f << "# twindiff trace v1\n";
	f << fmt::format("meta label={} serial={} frames={} vu0fp={} nofp={} vu0full={} sysfull={} iopfp={} savestate={}\n",
		t.label, t.serial.empty() ? "?" : t.serial, t.frames, t.vu0fp, t.nofp, t.vu0full, t.sysfull, t.iopfp,
		Path::GetFileName(s_savestate_path));
	// With iopfp the line carries four more hashes after the cycle: spu2 sif sio cdvd.
	const auto cell = [&f, &t](const char* name, const std::vector<TwinRec>& v) {
		for (size_t i = 0; i < v.size(); ++i)
		{
			f << fmt::format("rec {} {} {:016x} {:016x} {}", i, name, v[i].mem, v[i].reg, v[i].cyc);
			if (t.iopfp)
				f << fmt::format(" {:016x} {:016x} {:016x} {:016x}", v[i].spu2, v[i].sif, v[i].sio, v[i].cdvd);
			f << '\n';
		}
	};
	cell("i1", t.i1);
	cell("i2", t.i2);
	cell("j", t.j);
	Console.WriteLn(fmt::format("twin: wrote {} ({} frames x 3 cells).", path, t.frames));
	return true;
}

static bool LoadTwinTrace(TwinTrace& t, const std::string& path)
{
	std::ifstream f(path);
	if (!f)
	{
		Console.ErrorFmt("twin: failed to open {}", path);
		return false;
	}
	std::string line;
	while (std::getline(f, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream ss(line);
		std::string tok;
		ss >> tok;
		if (tok == "meta")
		{
			std::string kv;
			while (ss >> kv)
			{
				const size_t eq = kv.find('=');
				if (eq == std::string::npos)
					continue;
				const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
				if (k == "label")
					t.label = v;
				else if (k == "serial")
					t.serial = v;
				else if (k == "frames")
					t.frames = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
				else if (k == "vu0fp")
					t.vu0fp = (v == "1");
				else if (k == "nofp")
					t.nofp = (v == "1");
				else if (k == "vu0full")
					t.vu0full = (v == "1");
				else if (k == "sysfull")
					t.sysfull = (v == "1");
				else if (k == "iopfp")
					t.iopfp = (v == "1");
			}
		}
		else if (tok == "rec")
		{
			size_t idx = 0;
			std::string cell, memh, regh;
			u64 cyc = 0;
			ss >> idx >> cell >> memh >> regh >> cyc;
			if (ss.fail() || (cell != "i1" && cell != "i2" && cell != "j"))
			{
				Console.ErrorFmt("twin: malformed rec line in {}: '{}'", path, line);
				return false;
			}
			TwinRec r;
			r.mem = std::strtoull(memh.c_str(), nullptr, 16);
			r.reg = std::strtoull(regh.c_str(), nullptr, 16);
			r.cyc = cyc;
			if (t.iopfp)
			{
				std::string spu2h, sifh, sioh, cdvdh;
				ss >> spu2h >> sifh >> sioh >> cdvdh;
				if (ss.fail())
				{
					Console.ErrorFmt("twin: malformed iop fields in {}: '{}'", path, line);
					return false;
				}
				r.spu2 = std::strtoull(spu2h.c_str(), nullptr, 16);
				r.sif = std::strtoull(sifh.c_str(), nullptr, 16);
				r.sio = std::strtoull(sioh.c_str(), nullptr, 16);
				r.cdvd = std::strtoull(cdvdh.c_str(), nullptr, 16);
			}
			auto& v = (cell == "i1") ? t.i1 : (cell == "i2") ? t.i2 : t.j;
			if (idx != v.size())
			{
				Console.ErrorFmt("twin: out-of-order rec index in {} (cell {} idx {}).", path, cell, idx);
				return false;
			}
			v.push_back(r);
		}
	}
	Console.WriteLn(fmt::format("twin: loaded {} (label={} serial={} i1/i2/j = {}/{}/{} frames).",
		path, t.label, t.serial, t.i1.size(), t.i2.size(), t.j.size()));
	return !t.i1.empty() && !t.i2.empty() && !t.j.empty();
}

// First frame index where the two cells' memhash (or regfp) differ; -1 = none.
static int TwinFirstDiff(const std::vector<TwinRec>& a, const std::vector<TwinRec>& b, bool reg)
{
	const size_t n = std::min(a.size(), b.size());
	for (size_t f = 0; f < n; ++f)
	{
		if (reg ? (a[f].reg != b[f].reg) : (a[f].mem != b[f].mem))
			return static_cast<int>(f);
	}
	return -1;
}

// First frame where any IOP-side hash (spu2 sif sio cdvd) differs; -1 = none.
static int TwinFirstIopDiff(const std::vector<TwinRec>& a, const std::vector<TwinRec>& b)
{
	const size_t n = std::min(a.size(), b.size());
	for (size_t f = 0; f < n; ++f)
	{
		if (a[f].spu2 != b[f].spu2 || a[f].sif != b[f].sif || a[f].sio != b[f].sio || a[f].cdvd != b[f].cdvd)
			return static_cast<int>(f);
	}
	return -1;
}

static s64 TwinMaxCycDrift(const std::vector<TwinRec>& a, const std::vector<TwinRec>& b, int* at)
{
	const size_t n = std::min(a.size(), b.size());
	s64 maxd = 0;
	*at = -1;
	for (size_t f = 0; f < n; ++f)
	{
		// cycle is a u32 counter widened at capture; diff as s64 of the u32s
		// (wrap artifacts only matter across ~14s spans, far beyond a window).
		const s64 d = static_cast<s64>(a[f].cyc) - static_cast<s64>(b[f].cyc);
		if (std::llabs(d) > std::llabs(maxd))
		{
			maxd = d;
			*at = static_cast<int>(f);
		}
	}
	return maxd;
}

static int RunTwinDump()
{
	if (s_twin_prefix.empty())
	{
		Console.Error("twin: --twindump needs a <prefix>.");
		return EXIT_FAILURE;
	}
	TwinTrace t;
	if (!TwinRunAll(t))
		return EXIT_FAILURE;
	if (!WriteTwinTrace(t, s_twin_prefix + ".twin"))
		return EXIT_FAILURE;

	// Local quick summary so a lone dump is already informative.
	const int ctrl = TwinFirstDiff(t.i1, t.i2, false);
	const int jvi = TwinFirstDiff(t.j, t.i1, false);
	int at = -1;
	const s64 drift = TwinMaxCycDrift(t.j, t.i1, &at);
	Console.WriteLn(fmt::format(
		"TWINDUMP SUMMARY [{}]: interp determinism floor breaks at frame {}; local jit-vs-interp mem first differs at frame {} (cycle-model noise expected); max |jit-interp cycle drift| = {:+d} @ frame {}.",
		t.label, ctrl, jvi, drift, at));
	return EXIT_SUCCESS;
}

static int RunTwinCompare()
{
	// Default the local prefix BEFORE the passes (the ram-at dumps use it).
	if (s_twin_prefix.empty())
		s_twin_prefix = s_twin_other + ".local";

	TwinTrace other;
	if (!LoadTwinTrace(other, s_twin_other))
		return EXIT_FAILURE;

	TwinTrace local;
	if (!TwinRunAll(local))
		return EXIT_FAILURE;
	WriteTwinTrace(local, s_twin_prefix + ".twin");

	if (other.vu0fp != local.vu0fp || other.nofp != local.nofp || other.vu0full != local.vu0full || other.sysfull != local.sysfull)
	{
		Console.ErrorFmt(
			"twin: fingerprint config mismatch (other vu0fp={} nofp={} vu0full={} sysfull={}, local vu0fp={} nofp={} vu0full={} sysfull={}) — set matching EERUNNER_VU0FP / EERUNNER_NOFP / EERUNNER_VU0FULL / EERUNNER_SYSFULL on both sides.",
			other.vu0fp, other.nofp, other.vu0full, other.sysfull, local.vu0fp, local.nofp, local.vu0full, local.sysfull);
		return EXIT_FAILURE;
	}
	if (!other.serial.empty() && other.serial != "?" && !local.serial.empty() && other.serial != local.serial)
		Console.ErrorFmt("twin: WARNING serial mismatch (other={} local={}) — comparing different games?", other.serial, local.serial);

	const size_t n = std::min({other.i1.size(), other.i2.size(), other.j.size(),
		local.i1.size(), local.i2.size(), local.j.size()});
	if (n == 0)
	{
		Console.Error("twin: no overlapping frames.");
		return EXIT_FAILURE;
	}

	// Determinism floors (per tree, interp-vs-interp).
	const int oc = TwinFirstDiff(other.i1, other.i2, false);
	const int lc = TwinFirstDiff(local.i1, local.i2, false);

	struct Pair
	{
		const char* tag;
		std::string desc;
		const std::vector<TwinRec>* a;
		const std::vector<TwinRec>* b;
	};
	const Pair pairs[] = {
		{"XI", fmt::format("cross-tree interp-vs-interp ({} vs {}) [SHARP]", other.label, local.label), &other.i1, &local.i1},
		{"TJ", fmt::format("{} jit-vs-interp envelope [cycle-model noisy]", other.label), &other.j, &other.i1},
		{"OJ", fmt::format("{} jit-vs-interp envelope [cycle-model noisy]", local.label), &local.j, &local.i1},
		{"EE", fmt::format("end-to-end jit-vs-jit ({} vs {}) [cycle-model noisy]", other.label, local.label), &other.j, &local.j},
	};

	Console.WriteLn(fmt::format(
		"TWINDIFF: other='{}' local='{}' frames={}; determinism floors: other frame {}, local frame {} (-1 = never broke).",
		other.label, local.label, n, oc, lc));

	int first_mem[4], first_reg[4], drift_at[4];
	s64 drift[4];
	for (int p = 0; p < 4; ++p)
	{
		first_mem[p] = TwinFirstDiff(*pairs[p].a, *pairs[p].b, false);
		first_reg[p] = TwinFirstDiff(*pairs[p].a, *pairs[p].b, true);
		drift[p] = TwinMaxCycDrift(*pairs[p].a, *pairs[p].b, &drift_at[p]);
		Console.WriteLn(fmt::format("  {}  {}: first mem-div frame {}, first regfp-div frame {} (advisory), max |cycle drift| {:+d} @ frame {}.",
			pairs[p].tag, pairs[p].desc, first_mem[p], first_reg[p], drift[p], drift_at[p]));
		// The IOP-side hashes (spu2 sif sio cdvd) are advisory too; they only exist in files taken with EERUNNER_SYSFULL=1.
		if (other.iopfp && local.iopfp)
			Console.WriteLn(fmt::format("      first iop-state-div frame {} (spu2/sif/sio/cdvd, advisory).",
				TwinFirstIopDiff(*pairs[p].a, *pairs[p].b)));
	}

	// XI is real only while BOTH trees' own determinism floors are still clean.
	const int xi = first_mem[0];
	const bool xi_real = xi >= 0 && (oc < 0 || xi < oc) && (lc < 0 || xi < lc);
	const int ee = first_mem[3];

	Console.WriteLn("TWINDIFF VERDICT:");
	if (xi_real)
	{
		Console.WriteLn(fmt::format(
			"  SHARED-C++ DRIFT: the two trees' full interpreters diverge at frame {} (identical cycle model on both sides — semantic merge delta, not JIT). Re-run both sides with --twin-ram-at {} and page-diff the .interp.bin pair; suspects = the shared pcsx2/ files changed between the trees.",
			xi, xi));
		if (drift[0] != 0 && drift_at[0] <= xi)
			Console.WriteLn("  (interp cycle trajectories also drift cross-tree — event-scheduling change, e.g. Counters — inspect that before trusting value-level diffs.)");
	}
	else if (xi >= 0)
	{
		Console.WriteLn(fmt::format(
			"  cross-tree interp divergence at frame {} is AT/BEYOND a determinism floor (other {}, local {}) — inconclusive; move the savestate closer or shorten --frames.",
			xi, oc, lc));
	}
	else
	{
		Console.WriteLn(fmt::format("  cross-tree interp timelines IDENTICAL for {} frames — shared C++ is clean here.", n));
	}
	if (ee >= 0)
	{
		Console.WriteLn(fmt::format(
			"  end-to-end jit-vs-jit mem first differs at frame {}. The jit pairs carry benign cycle-model divergence (timer-derived values land in RAM): adjudicate with the RAM page-diff character at that frame (--twin-ram-at {} on both sides; scattered small deltas = timing noise, structured buffers = value bug) and with config A/B (--set).",
			ee, ee));
		if (first_mem[1] >= 0 && first_mem[2] < 0)
			Console.WriteLn(fmt::format(
				"  SIGNATURE: {} JIT diverges from shared interp (frame {}) while {} JIT tracks interp — if the game is broken here, it likely DEPENDS on the other JIT's semantics (guard-bit class).",
				other.label, first_mem[1], local.label));
	}
	else
	{
		Console.WriteLn("  no end-to-end jit-vs-jit divergence in the window — CPU-state clean here; suspect GS-side or config (diff --dump-config outputs), or move the savestate closer to the symptom.");
	}

	return (xi_real || ee >= 0) ? 2 : EXIT_SUCCESS;
}

// --mkstate <out.p2s> : bootstrap a shared savestate for twindiff. Boots the
// disc (or continues from --savestate), runs --frames under the production
// JIT stack, writes a savestate, exits. Make twindiff states on the WORKING
// (gold) binary: a state taken from a broken run can already carry the
// corruption baked into RAM — True Crime's paletted textures are computed
// once at level load, so a mid-scene state from a broken build shows no
// guard-bit delta at all; the window has to CROSS the corrupting computation.
static int RunMkState()
{
	Error error;
	if (!s_savestate_path.empty())
	{
		if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
		{
			Console.ErrorFmt("mkstate: load failed: {}", error.GetDescription());
			return EXIT_FAILURE;
		}
	}
	AdvanceFrames(s_frames);
	bool ok = true;
	VMManager::SaveState(s_mkstate_path.c_str(), /*zip_on_thread=*/false, /*backup_old_state=*/false,
		[&](const std::string& e) { ok = false; Console.ErrorFmt("mkstate: save failed: {}", e); });
	VMManager::WaitForSaveStateFlush();
	Console.WriteLn(fmt::format("MKSTATE: {} after {} frames -> {}",
		ok ? "wrote state" : "FAILED", s_frames, s_mkstate_path));
#if defined(ARCH_ARM64)
	// Which VU macro ops this run actually compiled — the candidate list for a
	// `--rec-fallback cop2vu:<mnemonic>` bisect.
	Console.WriteLn(fmt::format("COP2VU CENSUS: {}", EERecFallback::DescribeCop2VuCensus()));
#endif
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

// --dump-config <path> : boot the VM far enough for GameDB to apply, then
// serialize the EFFECTIVE EmuConfig to an INI. The twindiff driver diffs the
// two trees' dumps FIRST — config/GameDB drift (True Crime: guard-bit emulation
// default-off at the time + three divergent GameIndex copies) explains a broken
// game far more cheaply than any trace. Savestate optional: config keys off the disc
// serial, known right after Initialize.
static int RunDumpConfig()
{
	INISettingsInterface ini(s_dumpcfg_path);
	{
		SettingsSaveWrapper wrap(ini);
		EmuConfig.LoadSave(wrap);
	}
	if (!ini.Save())
	{
		Console.ErrorFmt("dump-config: failed to write {}", s_dumpcfg_path);
		return EXIT_FAILURE;
	}
	Console.WriteLn(fmt::format("DUMPCONFIG: serial={} crc={:08x} -> {}",
		VMManager::GetDiscSerial(), VMManager::GetDiscCRC(), s_dumpcfg_path));
	return EXIT_SUCCESS;
}

// --speedhack-diff : speedhack-misfire differential.
//
// Speedhacks are silent, runtime-gated divergences: each one CLAIMS to skip only
// dead work (a spin loop, an intc_stat poll, a redundant flag update) and leave
// the EE/VU architectural result unchanged. When that claim is wrong — as the EE
// recompiler's WaitLoop timeout-loop skip was for Burnout 3's DMA-display-list
// build loop — the game corrupts state and hangs, with EE=interp clean and
// EE=jit broken. No existing state-diff tool catches this because they compare
// jit-vs-interp at ONE speedhack config; the bug lived in a config axis no test
// varied.
//
// This mode varies that axis. It runs the savestate forward in EE-jit throughout
// (the speedhacks are jit-gated), establishes a baseline with every
// transparency-class speedhack OFF (run twice, for the run-to-run determinism
// floor), then sweeps each speedhack on its own and all-on. A transparent
// speedhack must NOT change the per-frame EE-RAM hash before the baseline control
// floor breaks; the FIRST such clean-control divergence is a misfire, and
// ReportMemDiff at that frame localizes the corrupted region (e.g. the GIF DMA
// chain). No core instrumentation — reuses HashMemory/ReportMemDiff. This is the
// system-level positive-side coverage the unit tests can't give (a fired skip
// diverges from naive interp BY DESIGN; only equality-against-an-honest-baseline
// validates it).
//
// Excluded by construction: vuThread/MTVU (nondeterministic — thread races defeat
// an equality diff) and EECycleRate/EECycleSkip (deliberately lossy cycle scaling
// — they change results on purpose). Diffs EE main RAM + scratchpad (where
// DMA-chain / display-list corruption lands), the same surface as --contmem.
static int RunSpeedhackDiff()
{
	Error error;

	// The swept speedhacks. equalityClass = CLAIMS bit-exact equivalence (skips
	// provably-dead work); a sustained divergence from those is off-spec. The
	// others are deliberate timing approximations that legitimately change a few
	// async-phase bytes — only a runaway EXPLOSION flags them. Bit i = knob i ON.
	struct Knob { const char* name; const char* key; bool equalityClass; };
	static const Knob kKnobs[] = {
		{"WaitLoop", "WaitLoop", true},      // EE timeout/idle-loop skip (recSkipTimeoutLoop) — the Burnout-3 culprit
		{"IntcStat", "IntcStat", true},      // fast-forward through intc_stat poll waits (skip-to-event)
		{"vuFlagHack", "vuFlagHack", true},  // microVU status/mac flag elision (redundant-write skip)
		{"vu1Instant", "vu1Instant", false}, // run VU1 to completion instantly — lossy timing approximation
		{"fastCDVD", "fastCDVD", false},      // shorten CDVD access latency — lossy timing approximation
	};
	constexpr size_t kNumKnobs = std::size(kKnobs);
	const uint32_t kAllOn = (1u << kNumKnobs) - 1u;

	auto maskLabel = [&](uint32_t mask) -> std::string {
		if (mask == 0)
			return "baseline(all-off)";
		if (mask == kAllOn)
			return "all-on";
		std::string s;
		for (size_t k = 0; k < kNumKnobs; ++k)
			if ((mask >> k) & 1u)
				s += (s.empty() ? "" : "+") + std::string(kKnobs[k].name);
		return s;
	};

	auto applyConfig = [&](uint32_t mask) {
		for (size_t k = 0; k < kNumKnobs; ++k)
			s_settings_interface.SetBoolValue("EmuCore/Speedhacks", kKnobs[k].key, ((mask >> k) & 1u) != 0u);
		// Force the excluded knobs to their neutral / off state so they never
		// contaminate the differential.
		s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
		s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleRate", 0);
		s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleSkip", 0);
		// Speedhacks bite the EE recompiler; run it (not interp) in every pass.
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", true);
		VMManager::ApplySettings();
	};

	auto runPass = [&](uint32_t mask, std::vector<uint64_t>* cyc = nullptr) -> std::vector<uint64_t> {
		std::vector<uint64_t> hashes;
		if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
		{
			Console.ErrorFmt("speedhack-diff: load failed: {}", error.GetDescription());
			return hashes;
		}
		applyConfig(mask);
		hashes.reserve(s_frames);
		for (uint32_t f = 0; f < s_frames && VMManager::GetState() != VMState::Shutdown; ++f)
		{
			VMManager::FrameAdvance(1);
			VMManager::Execute();
			hashes.push_back(ee_divtrace::HashMemory());
			if (cyc)
				cyc->push_back(static_cast<uint64_t>(cpuRegs.cycle));
		}
		return hashes;
	};

	auto capMem = [&](uint32_t mask, uint32_t frame) -> std::vector<u8> {
		std::vector<u8> out;
		if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
			return out;
		applyConfig(mask);
		AdvanceFrames(frame + 1);
		out.resize(Ps2MemSize::MainRam + Ps2MemSize::Scratch);
		std::memcpy(out.data(), eeMem->Main, Ps2MemSize::MainRam);
		std::memcpy(out.data() + Ps2MemSize::MainRam, eeMem->Scratch, Ps2MemSize::Scratch);
		return out;
	};

	// Baseline determinism sanity: run all-off twice and report how long the
	// per-frame hash stays bit-identical. If this breaks immediately the
	// savestate/renderer setup is nondeterministic and the verdicts below are
	// unreliable (reduce --frames or check the setup).
	Console.WriteLn("SPEEDHACK-DIFF: baseline determinism pass A (all speedhacks OFF)...");
	const auto baseA = runPass(0);
	Console.WriteLn("SPEEDHACK-DIFF: baseline determinism pass B (all speedhacks OFF)...");
	const auto baseB = runPass(0);
	if (baseA.empty() || baseB.empty())
		return EXIT_FAILURE;
	{
		const size_t bn = std::min(baseA.size(), baseB.size());
		int floorFrame = -1;
		for (size_t f = 0; f < bn; ++f)
			if (baseA[f] != baseB[f]) { floorFrame = static_cast<int>(f); break; }
		Console.WriteLn(fmt::format(
			"SPEEDHACK-DIFF: baseline (all-off) stays run-to-run bit-identical through frame {} (of {}).",
			floorFrame < 0 ? static_cast<int>(bn) : floorFrame, bn));
	}

	// The verdict is NOT "any byte differs". Two distinct speedhack classes exist:
	//  - equality-class (WaitLoop/IntcStat/vuFlagHack): skip provably-dead work
	//    (a spin loop to its next event, a redundant flag) and CLAIM bit-exact
	//    equivalence. Any sustained divergence from all-off is suspect.
	//  - lossy-timing (vu1Instant/fastCDVD): deliberate approximations that DO
	//    change cycle timing (and therefore a few async-phase bytes) on purpose.
	//    Only a runaway EXPLOSION matters for these.
	// The clean cross-class discriminator for the corruption/hang family (the
	// Burnout-3 WaitLoop misfire) is GROWTH: corruption balloons across the
	// window; a legit timing offset stays bounded. So we sample the EE-RAM
	// divergence magnitude at several frames and compare its growth + absolute
	// size against the determinism floor (two independent all-off runs). Same
	// bounded-vs-explosion logic --contmem uses, swept over the speedhack axis.
	std::vector<uint32_t> samples;
	{
		const uint32_t N = s_frames;
		auto add = [&](uint32_t f) {
			if (f < N && std::find(samples.begin(), samples.end(), f) == samples.end())
				samples.push_back(f);
		};
		add(N >= 8 ? N / 4 : 0);
		add(N / 2);
		add((3u * N) / 4u);
		if (N >= 1)
			add(N - 1);
		std::sort(samples.begin(), samples.end());
	}

	// Reference (all-off run #1) and an independent all-off run #2 for the floor,
	// captured once per sample frame and reused across every config.
	std::vector<std::vector<u8>> baseCaps, floorCaps;
	std::vector<MemDiffCount> floorTraj;
	for (uint32_t sf : samples)
	{
		baseCaps.push_back(capMem(0, sf));
		floorCaps.push_back(capMem(0, sf));
		floorTraj.push_back(ReportMemDiff(baseCaps.back(), floorCaps.back(), /*verbose=*/false));
	}
	{
		std::string s;
		for (size_t i = 0; i < samples.size(); ++i)
			s += fmt::format("{}f={}p ", samples[i], floorTraj[i].pages);
		Console.WriteLn("SPEEDHACK-DIFF: determinism floor trajectory (all-off vs all-off): " + s);
	}
	const MemDiffCount floorLast = floorTraj.empty() ? MemDiffCount{} : floorTraj.back();

	// Is every speedhack in `mask` an equality-class one (and mask non-empty)?
	auto pureEqualityClass = [&](uint32_t mask) -> bool {
		if (mask == 0)
			return false;
		for (size_t k = 0; k < kNumKnobs; ++k)
			if (((mask >> k) & 1u) && !kKnobs[k].equalityClass)
				return false;
		return true;
	};

	struct Verdict { uint32_t mask; std::vector<MemDiffCount> traj; bool explosion; };
	std::vector<Verdict> verdicts;

	auto evalConfig = [&](uint32_t mask) {
		std::vector<MemDiffCount> traj;
		for (size_t i = 0; i < samples.size(); ++i)
			traj.push_back(ReportMemDiff(baseCaps[i], capMem(mask, samples[i]), /*verbose=*/false));
		const MemDiffCount first = traj.front();
		const MemDiffCount last = traj.back();

		// EXPLOSION = the corruption/hang signature: a page spread well past the
		// determinism floor that GREW across the window (a misfire's corruption
		// onsets mid-run and runs away, e.g. buggy WaitLoop 14p->10p->10p->206p).
		// This is class-agnostic and the ONLY automated verdict — every swept
		// speedhack perturbs SOME bounded state by design (a correct WaitLoop/IntcStat
		// leaves the abandoned spin counter + a few async-phase bytes;
		// vu1Instant/fastCDVD shift timing), so "diverges at all" is not a bug; only
		// runaway GROWTH is.
		//
		// Absolute magnitude alone cannot discriminate: chaos-amplifying 3D titles
		// (e.g. GTA San Andreas) amplify a 110-byte sub-ULP seed (the unavoidable
		// arm64 fmadd/-ffp-contract JIT-vs-interp difference) into a 576p intrinsic
		// chaos floor with NO speedhack at all (see --contmem); a 303p WaitLoop
		// divergence there is *below* that floor, same scattered-1-ULP-FP character,
		// and is NOT a misfire. Magnitude (absolute OR relative-to-floor) can't
		// separate that from a real misfire (Burnout-3 buggy 206p was only 1.2x its
		// 166p fixed floor); the late-onset GROWTH shape can:
		// GTA's flat-high 282->303 fails `grew`, Burnout's 14->206 passes it.
		// Empirically validated: Burnout-3 buggy WaitLoop flagged, fixed not;
		// GTA SA / R&C UYA WaitLoop+IntcStat correctly NOT flagged.
		const size_t floorPad = floorLast.pages * 4 + 16;
		const bool grew = last.pages >= first.pages * 4;
		const bool explosion = last.pages >= 32 && last.pages > floorPad && grew;

		std::string trajStr;
		for (size_t i = 0; i < samples.size(); ++i)
			trajStr += fmt::format("{}f={}p/{}b ", samples[i], traj[i].pages, traj[i].bytes);
		// Only an EQUALITY-class config (WaitLoop/IntcStat/vuFlagHack — claims
		// bit-exactness) exploding is an actionable misfire. Lossy-class configs
		// (vu1Instant/fastCDVD/all-on) are DESIGNED to diverge — fastCDVD shortens
		// disc latency, so on a slot that's actively streaming it legitimately loads
		// MBs of assets earlier (seen as a multi-hundred-page explosion). That is
		// expected, not a bug, so it must not read as "investigate as misfire".
		const bool isEquality = (mask != 0) && pureEqualityClass(mask);
		const char* cls = (mask == 0) ? "-" : (isEquality ? "equality" : "lossy/mixed");
		const char* verdictStr = !explosion ? "bounded (expected)"
			: (isEquality ? "EXPLOSION (likely misfire/corruption)"
			              : "EXPLOSION (expected — lossy timing class, not a misfire)");
		Console.WriteLn(fmt::format("SPEEDHACK-DIFF [{}] class={}: {}", maskLabel(mask), cls, verdictStr));
		Console.WriteLn("  trajectory (baseline-all-off vs config): " + trajStr
			+ fmt::format("(floor_last={}p)", floorLast.pages));
		if (explosion)
		{
			Console.WriteLn(fmt::format("  EE-RAM region @ frame {} — baseline(all-off) vs [{}]:",
				samples.back(), maskLabel(mask)));
			ReportMemDiff(baseCaps.back(), capMem(mask, samples.back()), /*verbose=*/true);
		}
		verdicts.push_back({mask, std::move(traj), explosion});
	};

	// Sweep each speedhack on its own, then all-on (interaction check).
	for (size_t k = 0; k < kNumKnobs; ++k)
		evalConfig(1u << k);
	evalConfig(kAllOn);

	// Summary.
	Console.WriteLn(fmt::format("SPEEDHACK-DIFF SUMMARY (floor_last = {} pages / {} bytes @ frame {}):",
		floorLast.pages, floorLast.bytes, samples.empty() ? 0u : samples.back()));
	int misfires = 0, lossyExplosions = 0;
	for (const auto& v : verdicts)
	{
		const bool isEquality = (v.mask != 0) && pureEqualityClass(v.mask);
		const bool misfire = v.explosion && isEquality;
		misfires += misfire ? 1 : 0;
		lossyExplosions += (v.explosion && !isEquality) ? 1 : 0;
		const MemDiffCount last = v.traj.empty() ? MemDiffCount{} : v.traj.back();
		const char* tag = !v.explosion ? "bounded"
			: (misfire ? "EXPLOSION <-- likely misfire" : "EXPLOSION (expected lossy)");
		Console.WriteLn(fmt::format("  {:<28} last {:>6} pages / {:>8} bytes   {}",
			maskLabel(v.mask), last.pages, last.bytes, tag));
	}
	Console.WriteLn(fmt::format(
		"SPEEDHACK-DIFF: {} equality-class MISFIRE(s) — the actionable signal — plus {} expected lossy-class explosion(s).",
		misfires, lossyExplosions));
	return EXIT_SUCCESS;
}

static int RunStepDiff()
{
	Error error;
	const std::string ckpt = Path::Combine(EmuFolders::Cache, "eerunner_stepdiff.p2s");

	auto saveCkpt = [&]() -> bool {
		bool ok = true;
		VMManager::SaveState(ckpt.c_str(), /*zip_on_thread=*/false, /*backup_old_state=*/false,
			[&](const std::string& e) { ok = false; Console.ErrorFmt("stepdiff: save failed: {}", e); });
		VMManager::WaitForSaveStateFlush();
		return ok;
	};
	auto loadCkpt = [&]() -> bool {
		if (!VMManager::LoadState(ckpt.c_str(), &error))
		{
			Console.ErrorFmt("stepdiff: load checkpoint failed: {}", error.GetDescription());
			return false;
		}
		return true;
	};
	// Run one frame in the given mode from the just-loaded checkpoint, returning
	// the end-of-frame full snapshot + memory hash.
	auto runOne = [&](bool jit, ee_divtrace::FullSnap& snap, uint64_t& memhash) -> bool {
		if (!loadCkpt())
			return false;
		SetEeMode(jit);
		AdvanceFrames(1);
		snap = CaptureFullSnap();
		memhash = ee_divtrace::HashMemory();
		return true;
	};

	if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
	{
		Console.ErrorFmt("stepdiff: initial load failed: {}", error.GetDescription());
		return EXIT_FAILURE;
	}
	SetEeMode(false); // golden timeline is interp

	int benign_frames = 0;
	for (uint32_t f = 0; f < s_frames && VMManager::GetState() != VMState::Shutdown; ++f)
	{
		if (!saveCkpt())
			return EXIT_FAILURE;

		ee_divtrace::FullSnap i1, i2, j;
		uint64_t i1m = 0, i2m = 0, jm = 0;
		if (!runOne(false, i1, i1m) || !runOne(false, i2, i2m) || !runOne(true, j, jm))
			return EXIT_FAILURE;

		const auto ii = DiffFullSnaps(i1, i2);
		const bool ii_clean = ii.empty() && i1m == i2m && i1.pc == i2.pc;
		const auto ij = DiffFullSnaps(j, i1); // labels: JIT=j, INTERP=i1
		const bool ij_diverged = !ij.empty() || (i1m != jm) || (i1.pc != j.pc);

		if (ij_diverged)
		{
			Console.WriteLn(fmt::format(
				"STEPDIFF frame {}: interp-vs-JIT DIVERGES (pc interp={:#010x} jit={:#010x}, mem {}); "
				"interp-vs-interp control = {}",
				f, i1.pc, j.pc, (i1m != jm) ? "DIFFERS" : "same",
				ii_clean ? "CLEAN" : "ALSO DIVERGES (async jitter)"));
			for (const auto& d : ij)
				Console.WriteLn(fmt::format("    {}", d));
			if (ii_clean && i1m == jm)
			{
				// End-of-frame MEMORY identical — only live registers / pc differ.
				// The EE is parked in a producer/consumer spin-wait (the GIF double-
				// buffer poll at 0x1f24e0) and the two cores are sampled at different
				// iteration counts at the vsync boundary. No architectural state
				// PERSISTED differently, so this CANNOT be the divergence we hunt: a
				// real EE-JIT computational bug stores its wrong value, which would
				// show as a memory difference. Skip the zoom (it would only re-find
				// the benign spin phase) and keep scanning.
				Console.WriteLn(fmt::format(
					"  => frame {}: end-of-frame MEMORY identical (only live regs/pc differ) — benign spin-wait "
					"phase at the vsync boundary; continuing scan.", f));
				++benign_frames;
			}
			else if (ii_clean)
			{
				// Candidate real divergence (clean interp control, MEMORY differs).
				// Zoom in INLINE to classify: the checkpoint still holds this frame's
				// start (the zoom only LoadState-reads it). If the zoom resolves it
				// all to benign cycle-derived timer reads / spin phase, keep scanning
				// later frames; otherwise it's a real lead and we stop here.
				Console.Error("  => candidate REAL EE JIT divergence (clean interp control, MEMORY differs) — zooming to classify...");
				// Byte-level store localization first: re-run this frame once per
				// mode from the same checkpoint capturing EE RAM+scratch, and report
				// the differing regions. Single-frame from an identical checkpoint
				// with a clean interp control, so every differing byte is a real JIT
				// store difference (no cross-frame accumulation, no async jitter).
				{
					auto runOneMem = [&](bool jit_mode, std::vector<u8>& mem) -> bool {
						if (!loadCkpt())
							return false;
						SetEeMode(jit_mode);
						AdvanceFrames(1);
						mem.resize(Ps2MemSize::MainRam + Ps2MemSize::Scratch);
						std::memcpy(mem.data(), eeMem->Main, Ps2MemSize::MainRam);
						std::memcpy(mem.data() + Ps2MemSize::MainRam, eeMem->Scratch, Ps2MemSize::Scratch);
						return true;
					};
					std::vector<u8> mi, mj;
					if (runOneMem(false, mi) && runOneMem(true, mj))
					{
						Console.Error("  frame-local EE-RAM diff (A=interp, B=JIT):");
						ReportMemDiff(mi, mj, /*verbose=*/true, /*max_show=*/64);
					}
				}
				const bool stop = ZoomFromCheckpoint(ckpt);
				if (stop)
				{
					FileSystem::DeleteFilePath(ckpt.c_str());
					return EXIT_FAILURE;
				}
				++benign_frames;
				Console.WriteLn(fmt::format(
					"  => frame {}: the frame-boundary divergence resolved to benign timing only; continuing scan.", f));
			}
			else
			{
				Console.WriteLn("  => discounted: interp control also diverges, so this is sampling jitter, "
				                "not a codegen bug.");
				for (const auto& d : ii)
					Console.WriteLn(fmt::format("      [ctrl] {}", d));
			}
		}

		// Advance the golden interp timeline by one frame for the next checkpoint.
		// (After an inline zoom the VM is wherever the last re-run left it; loadCkpt
		// restores this frame's start, then AdvanceFrames steps to the next.)
		if (!loadCkpt())
			return EXIT_FAILURE;
		SetEeMode(false);
		AdvanceFrames(1);
	}

	FileSystem::DeleteFilePath(ckpt.c_str());
	if (benign_frames > 0)
		Console.WriteLn(fmt::format(
			"STEPDIFF: no real JIT divergence over {} frames ({} frame(s) had benign cycle-derived timer "
			"divergences that the zoom walked past).", s_frames, benign_frames));
	else
		Console.WriteLn(fmt::format("STEPDIFF: no real JIT divergence over {} frames (interp control clean throughout).",
			s_frames));
	return EXIT_SUCCESS;
}

// ===================================================================
// --vu0diff : live VU0-jit-vs-interp COP2-read value diff
// ===================================================================
// Pin the EE INTERPRETER in both passes and toggle ONLY the VU0 micro engine.
// Each pass loads the same checkpoint, runs one frame, and records every COP2
// read (QMFC2 reads VF[fs], CFC2 reads VI[fs]) the EE interpreter performs of
// VU0 state, in execution order. Diffing the two read-streams pins the FIRST
// VU0 program output the micro JIT computes differently from the VU0
// interpreter — live, with the real EE<->VU0 interleave the offline
// capture-replay harness can't reproduce.
//
// CAVEAT (read first): this is SINGLE-ARCH jit-vs-interp. It is the right tool
// for an arithmetic VALUE bug (the JIT computes a wrong number), but a
// divergence in the flag / Q / cycle pipeline INSTANCE is usually
// shared-with-x86 and NOT arch-specific — the FMAND-flag and cycle-bubble red
// herrings of the Burnout 3 hunt all collapsed this way. Confirm any lead from
// here with an arm64-jit-vs-x86-jit diff before trusting it.

// Capture hooks live in pcsx2/VU0.cpp; null in production. They only EXIST
// under PCSX2_RECOMPILER_TESTS (ENABLE_RECOMPILER_TEST_HOOKS=ON) — production
// configs (build-rocknix) compile eerunner without them, so every reference
// here must share the gate or the link breaks.
#ifdef PCSX2_RECOMPILER_TESTS
typedef void (*Cop2ReadHook)(u32 ee_pc, u32 op, u32 fs, const u32* lanes);
typedef void (*Cop2StateHook)(u32 tpc, u32 q, u32 mac, u32 status, u32 clip);
extern Cop2ReadHook g_cop2ReadHook;
extern Cop2StateHook g_cop2StateHook;
#endif

namespace
{
struct Cop2Read
{
	u32 ee_pc, op, fs;
	u32 lanes[4];
	u32 tpc, q, mac, status, clip;
};
std::vector<Cop2Read> s_cop2_sink;

#ifdef PCSX2_RECOMPILER_TESTS
void Cop2ReadCapture(u32 ee_pc, u32 op, u32 fs, const u32* lanes)
{
	Cop2Read r{};
	r.ee_pc = ee_pc;
	r.op = op;
	r.fs = fs;
	// QMFC2 (op 0) reads a full 128-bit VF; CFC2 (op 1) reads one 32-bit VI.
	r.lanes[0] = lanes[0];
	r.lanes[1] = (op == 0) ? lanes[1] : 0;
	r.lanes[2] = (op == 0) ? lanes[2] : 0;
	r.lanes[3] = (op == 0) ? lanes[3] : 0;
	s_cop2_sink.push_back(r);
}

void Cop2StateCapture(u32 tpc, u32 q, u32 mac, u32 status, u32 clip)
{
	if (s_cop2_sink.empty())
		return;
	Cop2Read& r = s_cop2_sink.back();
	r.tpc = tpc;
	r.q = q;
	r.mac = mac;
	r.status = status;
	r.clip = clip;
}
#endif // PCSX2_RECOMPILER_TESTS
} // namespace

// Diff two per-COP2-read VU0-value streams (a=interp golden, b=jit candidate) and
// report the FIRST read whose VU0 value differs. This pins the exact COP2 read — and
// thus the VU0 program output — where the micro JIT first diverges from the interp,
// upstream of the geometry-buffer corruption. VI flag regs read by CFC2 (16/17/18/
// 22/23/26) are stopping-point/flag noise (shared block-overshoot); they're shown but
// the FIRST-QMFC2-divergence line is the actionable signal.
static void ReportCop2ReadDiff(const std::vector<Cop2Read>& a, const std::vector<Cop2Read>& b)
{
	auto asf = [](u32 u) { float f; std::memcpy(&f, &u, 4); return f; };
	auto is_flag_vi = [](u32 fs) {
		return fs == 16 || fs == 17 || fs == 18 || fs == 22 || fs == 23 || fs == 26;
	};
	const size_t n = std::min(a.size(), b.size());
	Console.WriteLn(fmt::format("  COP2-read streams: interp={} reads, jit={} reads", a.size(), b.size()));
	int shown = 0;
	bool first_qmfc2 = false;
	for (size_t i = 0; i < n; ++i)
	{
		const Cop2Read& x = a[i];
		const Cop2Read& y = b[i];
		if (x.op != y.op || x.fs != y.fs || x.ee_pc != y.ee_pc)
		{
			Console.WriteLn(fmt::format(
				"  read #{}: STRUCTURAL divergence  interp(pc={:#010x} op={} fs={})  jit(pc={:#010x} op={} fs={})",
				i, x.ee_pc, x.op, x.fs, y.ee_pc, y.op, y.fs));
			break; // control flow split — everything after is unaligned
		}
		const bool diff = x.lanes[0] != y.lanes[0] || x.lanes[1] != y.lanes[1] ||
			x.lanes[2] != y.lanes[2] || x.lanes[3] != y.lanes[3];
		if (!diff)
			continue;
		const char* tag = (x.op == 0) ? "QMFC2 VF" : (is_flag_vi(x.fs) ? "CFC2  VI(flag)" : "CFC2  VI");
		if (x.op == 0)
		{
			if (!first_qmfc2)
			{
				Console.WriteLn(fmt::format(
					"  >>> FIRST QMFC2 VF divergence at read #{} (pc={:#010x} VF{:02}):", i, x.ee_pc, x.fs));
				first_qmfc2 = true;
			}
			Console.WriteLn(fmt::format(
				"  read #{} {} {:02}  pc={:#010x}\n"
				"      interp {:08x}_{:08x}_{:08x}_{:08x}  ({:g} {:g} {:g} {:g})\n"
				"      jit    {:08x}_{:08x}_{:08x}_{:08x}  ({:g} {:g} {:g} {:g})",
				i, tag, x.fs, x.ee_pc,
				x.lanes[0], x.lanes[1], x.lanes[2], x.lanes[3], asf(x.lanes[0]), asf(x.lanes[1]), asf(x.lanes[2]), asf(x.lanes[3]),
				y.lanes[0], y.lanes[1], y.lanes[2], y.lanes[3], asf(y.lanes[0]), asf(y.lanes[1]), asf(y.lanes[2]), asf(y.lanes[3])));
		}
		else
		{
			Console.WriteLn(fmt::format(
				"  read #{} {} {:02}  pc={:#010x}  interp={:08x}  jit={:08x}",
				i, tag, x.fs, x.ee_pc, x.lanes[0], y.lanes[0]));
		}
		if (++shown >= 40)
		{
			Console.WriteLn("  ... (40 divergent reads shown; truncating)");
			break;
		}
	}
	if (a.size() != b.size())
		Console.WriteLn(fmt::format("  NOTE: read-count differs (interp={} jit={}) — VU0-jit drove a different COP2 path",
			a.size(), b.size()));
}

static int RunVu0Diff()
{
#ifndef PCSX2_RECOMPILER_TESTS
	Console.Error("--vu0diff needs the COP2 capture hooks: rebuild with ENABLE_RECOMPILER_TEST_HOOKS=ON.");
	return EXIT_FAILURE;
#endif
	Error error;
	const std::string ckpt = Path::Combine(EmuFolders::Cache, "eerunner_vu0diff.p2s");

	auto saveCkpt = [&]() -> bool {
		bool ok = true;
		VMManager::SaveState(ckpt.c_str(), /*zip_on_thread=*/false, /*backup_old_state=*/false,
			[&](const std::string& e) { ok = false; Console.ErrorFmt("vu0diff: save failed: {}", e); });
		VMManager::WaitForSaveStateFlush();
		return ok;
	};
	// One pass: load the checkpoint, set VU0 mode, capture the COP2 read-stream
	// for exactly one frame from the just-loaded checkpoint.
	auto runOne = [&](bool vu0_jit, std::vector<Cop2Read>& out) -> bool {
		if (!VMManager::LoadState(ckpt.c_str(), &error))
		{
			Console.ErrorFmt("vu0diff: load checkpoint failed: {}", error.GetDescription());
			return false;
		}
		SetVu0Mode(vu0_jit);
		s_cop2_sink.clear();
#ifdef PCSX2_RECOMPILER_TESTS
		g_cop2ReadHook = &Cop2ReadCapture;
		g_cop2StateHook = &Cop2StateCapture;
		AdvanceFrames(1);
		g_cop2ReadHook = nullptr;
		g_cop2StateHook = nullptr;
#endif
		out = s_cop2_sink;
		return true;
	};

	if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
	{
		Console.ErrorFmt("vu0diff: initial load failed: {}", error.GetDescription());
		return EXIT_FAILURE;
	}
	SetVu0Mode(false); // golden timeline = VU0 interp

	for (uint32_t f = 0; f < s_frames && VMManager::GetState() != VMState::Shutdown; ++f)
	{
		if (!saveCkpt())
			return EXIT_FAILURE;

		std::vector<Cop2Read> interp, jit;
		if (!runOne(false, interp) || !runOne(true, jit))
			return EXIT_FAILURE;

		// Did any QMFC2 VF value diverge? (The actionable signal; flag-VI noise
		// is reported by ReportCop2ReadDiff but doesn't gate the per-frame header.)
		bool qmfc2_diverged = false;
		const size_t n = std::min(interp.size(), jit.size());
		for (size_t i = 0; i < n && !qmfc2_diverged; ++i)
		{
			const Cop2Read& x = interp[i];
			const Cop2Read& y = jit[i];
			if (x.op == 0 && (x.lanes[0] != y.lanes[0] || x.lanes[1] != y.lanes[1] ||
				x.lanes[2] != y.lanes[2] || x.lanes[3] != y.lanes[3]))
				qmfc2_diverged = true;
		}

		Console.WriteLn(fmt::format("VU0DIFF frame {}: {} ({} interp / {} jit COP2 reads)", f,
			qmfc2_diverged ? "QMFC2 VF DIVERGES" : "no QMFC2 value divergence", interp.size(), jit.size()));
		if (qmfc2_diverged || interp.size() != jit.size())
			ReportCop2ReadDiff(interp, jit);

		// Advance the golden interp timeline by one frame for the next checkpoint.
		if (!VMManager::LoadState(ckpt.c_str(), &error))
			return EXIT_FAILURE;
		SetVu0Mode(false);
		AdvanceFrames(1);
	}

	FileSystem::DeleteFilePath(ckpt.c_str());
	Console.WriteLn(fmt::format("VU0DIFF: scanned {} frame(s).", s_frames));
	return EXIT_SUCCESS;
}

// --liverun: reproduce the in-game HANG headlessly. Unlike the deterministic diff
// modes, this enables the live subsystems (real GS so GIF is consumed, MTVU on) and
// runs a single straight EE-jit pass. If the EE wedges in a spin/sync loop (the
// frozen-frame + looping-audio symptom), VMManager::Execute() never returns for a
// frame; a watchdog thread notices the stalled frame counter, samples the live EE
// PC (cpuRegs.pc) to fingerprint the spin loop, prints a PC histogram, and exits
// with code 42. A clean completion (all frames, code 0) means we did NOT reproduce
// the hang in this configuration.
// --disasm: load the savestate and disassemble EE code in [EERUNNER_DIS_LO,
// EERUNNER_DIS_HI] (default 0x100000..0x100040) with the correct R5900 disassembler.
// Generic MIPS disassemblers garble R5900 COP2/MMI ops; this tool does not.
static int RunDisasm()
{
	Error error;
	if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
	{
		Console.ErrorFmt("disasm: load failed: {}", error.GetDescription());
		return EXIT_FAILURE;
	}
	u32 lo = 0x100000, hi = 0x100040;
	if (const char* e = std::getenv("EERUNNER_DIS_LO")) lo = (u32)strtoul(e, nullptr, 0);
	if (const char* e = std::getenv("EERUNNER_DIS_HI")) hi = (u32)strtoul(e, nullptr, 0);
	Console.WriteLn(fmt::format("DISASM 0x{:08x}..0x{:08x}:", lo, hi));
	for (u32 a = lo; a <= hi; a += 4)
	{
		const u32 code = memRead32(a);
		std::string line;
		R5900::disR5900Fasm(line, code, a, false);
		Console.WriteLn(fmt::format("    {:#010x}: {:08x}  {}", a, code, line));
	}
	return EXIT_SUCCESS;
}

// --statereport: load the savestate and print a legible, field-level decode of every
// serialized timebase and transfer-engine state, then exit without running a frame.
// Built for A/B-diffing a poisoned state against a clean one:
//   diff <(eerunner --statereport --savestate A ...) <(eerunner --statereport --savestate B ...)
// A stuck-timer bug lives in the RELATIONSHIP between clocks that normally advance in
// lockstep (cycle vs lastCOP0Cycle, rcnt startCycle bases, EE vs IOP, vsync phase), so
// every line prints the stored baseline AND the value derived the way the guest would
// read it. Decoding goes through the emulator's own thaw path (VMManager::LoadState),
// so this report can never drift from the on-disk format.
static int RunStateReport()
{
	Error error;
	if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
	{
		Console.ErrorFmt("statereport: load failed: {}", error.GetDescription());
		return EXIT_FAILURE;
	}

	constexpr double EE_HZ = 294912000.0;
	constexpr double IOP_HZ = 36864000.0;
	const u64 cyc = cpuRegs.cycle;

	Console.WriteLn(fmt::format("== STATE REPORT: {} ==", Path::GetFileName(s_savestate_path)));

	Console.WriteLn(fmt::format(
		"[ee]      pc={:#010x} cycle={} ({:.6f}s) nextEvent=+{} lastEvent={} interrupt={:#010x} branch={}",
		cpuRegs.pc, cyc, cyc / EE_HZ, (s64)(cpuRegs.nextEventCycle - cyc),
		(s64)(cpuRegs.lastEventCycle - cyc), cpuRegs.interrupt, cpuRegs.branch));

	// The guest-visible COP0 Count is CP0.Count + (cycle - lastCOP0Cycle) at the moment
	// of the MFC0 (COP0.cpp) — a stale lastCOP0Cycle in a state file therefore warps the
	// game's realtime clock even though Count itself looks plausible.
	Console.WriteLn(fmt::format(
		"[ee.cop0] Count={} lastCOP0Cycle={} (lag={}) mfc0_now={} Compare={} Status={:#010x} Cause={:#010x}",
		(u32)cpuRegs.CP0.n.Count, cpuRegs.lastCOP0Cycle, (s64)(cyc - cpuRegs.lastCOP0Cycle),
		(u32)(cpuRegs.CP0.n.Count + (u32)(cyc - cpuRegs.lastCOP0Cycle)),
		(u32)cpuRegs.CP0.n.Compare, cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause));

	Console.WriteLn(fmt::format(
		"[ee.intc] STAT={:#010x} MASK={:#010x} pending&unmasked={:#010x} | DMAC STAT={:#06x} MASK={:#06x}",
		psHu32(INTC_STAT), psHu32(INTC_MASK), psHu32(INTC_STAT) & psHu32(INTC_MASK),
		psHu16(0xe012), psHu16(0xe010)));

	for (int n = 0; n < 32; n++)
	{
		if (!(cpuRegs.interrupt & (1u << n)))
			continue;
		const u64 deadline = cpuRegs.sCycle[n] + cpuRegs.eCycle[n];
		Console.WriteLn(fmt::format("[ee.int{:02d}] sCycle={} eCycle={} deadline={} ({})",
			n, cpuRegs.sCycle[n], cpuRegs.eCycle[n], deadline,
			(s64)(deadline - cyc) <= 0 ? std::string("DUE") : fmt::format("+{}", (s64)(deadline - cyc))));
	}

	// EE hardware counters: eff~ is derived the way rcntRcount derives it, i.e. what the
	// game would read back, not just the stored baseline.
	for (int i = 0; i < 4; i++)
	{
		const Counter& c = counters[i];
		const u64 age = cyc - c.startCycle;
		const u64 eff = c.mode.IsCounting ? (c.count + (c.rate ? (age / c.rate) : 0)) : c.count;
		Console.WriteLn(fmt::format(
			"[rcnt{}]   mode={:#06x} src={} counting={} gate={}/{} count={} target={} hold={} rate={} startCycle={} age={} eff~{} ({:.6f}s)",
			i, c.modeval & 0xffff, (u32)c.mode.ClockSource, (u32)c.mode.IsCounting,
			(u32)c.mode.EnableGate, (u32)c.mode.GateMode,
			c.count, c.target, c.hold, c.rate, c.startCycle, age, eff,
			(eff * (double)c.rate) / EE_HZ));
	}

	Console.WriteLn(fmt::format(
		"[vsync]   mode={} startCycle={} (age={}) delta={} | hsync mode={} startCycle={} (age={}) delta={}",
		vsyncCounter.Mode, vsyncCounter.startCycle, (s64)(cyc - vsyncCounter.startCycle), vsyncCounter.deltaCycles,
		hsyncCounter.Mode, hsyncCounter.startCycle, (s64)(cyc - hsyncCounter.startCycle), hsyncCounter.deltaCycles));
	Console.WriteLn(fmt::format(
		"[sched]   nextDeltaCounter={} nextStartCounter={} fires_at={} ({})",
		nextDeltaCounter, nextStartCounter, nextStartCounter + (u64)nextDeltaCounter,
		((s64)(nextStartCounter + (u64)nextDeltaCounter - cyc) <= 0) ? "DUE" : "future"));

	// EE<->IOP: the two cores nominally hold cycle_EE == 8 * cycle_IOP; skew is the drift
	// a poisoned state would carry between the two serialized clock domains.
	Console.WriteLn(fmt::format(
		"[ee-iop]  EEsCycle={} EEoCycle={} iop.cycle={} ee/8={} skew={} | ee={:.6f}s iop={:.6f}s d={:+.6f}s",
		EEsCycle, EEoCycle, psxRegs.cycle, cyc / 8, (s64)(cyc / 8 - psxRegs.cycle),
		cyc / EE_HZ, psxRegs.cycle / IOP_HZ, cyc / EE_HZ - psxRegs.cycle / IOP_HZ));

	Console.WriteLn(fmt::format(
		"[iop]     pc={:#010x} cycle={} interrupt={:#010x} psxNextDelta={} psxNextStart={}",
		psxRegs.pc, psxRegs.cycle, psxRegs.interrupt, psxNextDeltaCounter, psxNextStartCounter));

	for (int i = 0; i < NUM_COUNTERS; i++)
	{
		const psxCounter& c = psxCounters[i];
		const u64 age = psxRegs.cycle - c.startCycle;
		const u64 eff = c.count + (c.rate ? (age / c.rate) : 0);
		Console.WriteLn(fmt::format(
			"[iop.rc{}] mode={:#06x} count={} target={} rate={} startCycle={} age={} delta={} eff~{}",
			i, c.mode.modeval & 0xffff, c.count, c.target, c.rate, c.startCycle, age, c.deltaCycles, eff));
	}

	// RTC bytes are stored raw (BCD-ish, cdvd.cpp itob/btoi) — print hex, don't interpret.
	Console.WriteLn(fmt::format(
		"[cdvd]    rtc={:02x}-{:02x}-{:02x} {:02x}:{:02x}:{:02x} status={:#04x}",
		cdvd.RTC.year, cdvd.RTC.month, cdvd.RTC.day,
		cdvd.RTC.hour, cdvd.RTC.minute, cdvd.RTC.second, cdvd.RTC.status));

	for (int i = 0; i < 3; i++)
	{
		Gif_Path& p = gifUnit.gifPath[i];
		Console.WriteLn(fmt::format(
			"[gif.p{}]  state={} curSize={} curOffset={} readAmount={} done={}",
			i + 1, (int)p.state, p.curSize, p.curOffset, p.readAmount.load(), p.isDone() ? 1 : 0));
	}
	Console.WriteLn(fmt::format(
		"[gif]     GIF_STAT={:#010x} signal.queued={} finish fired={} pending={}",
		psHu32(0x3020), gifUnit.gsSIGNAL.queued ? 1 : 0,
		gifUnit.gsFINISH.gsFINISHFired ? 1 : 0, gifUnit.gsFINISH.gsFINISHPending ? 1 : 0));

	for (int i = 0; i < 2; i++)
	{
		const vifStruct& v = (i == 0) ? vif0 : vif1;
		Console.WriteLn(fmt::format(
			"[vif{}]    cmd={:#04x} done={} waitforvu={} inprogress={} dmamode={} irq={} queued_program={} queued_pc={}",
			i, (u32)v.cmd & 0xff, v.done ? 1 : 0, v.waitforvu ? 1 : 0, v.inprogress, v.dmamode,
			v.irq, v.queued_program ? 1 : 0, v.queued_pc));
	}

	static constexpr struct { const char* name; u32 base; } kDmaCh[] = {
		{"vif0", 0x8000}, {"vif1", 0x9000}, {"gif", 0xa000}, {"ipu0", 0xb000}, {"ipu1", 0xb400},
		{"sif0", 0xc000}, {"sif1", 0xc400}, {"sif2", 0xc800}, {"spr0", 0xd000}, {"spr1", 0xd400}};
	for (const auto& ch : kDmaCh)
	{
		Console.WriteLn(fmt::format(
			"[dma.{:<4}] chcr={:#010x} madr={:#010x} qwc={} tadr={:#010x}",
			ch.name, psHu32(ch.base), psHu32(ch.base + 0x10),
			psHu32(ch.base + 0x20) & 0xffff, psHu32(ch.base + 0x30)));
	}

	Console.WriteLn(fmt::format(
		"[mtvu]    vuCycles=[{},{},{},{}] idx={} interrupts={:#x} gsSignal={:#x} gsLabel={:#x}",
		vu1Thread.vuCycles[0].load(), vu1Thread.vuCycles[1].load(),
		vu1Thread.vuCycles[2].load(), vu1Thread.vuCycles[3].load(),
		vu1Thread.vuCycleIdx, vu1Thread.mtvuInterrupts.load(),
		vu1Thread.gsSignal.load(), vu1Thread.gsLabel.load()));

	return EXIT_SUCCESS;
}

static std::atomic<uint32_t> s_liverun_frame{0};
static u32 s_liverun_first_frame = 0; // g_FrameCount when the liverun's frames began, for per-frame figures
static std::atomic<bool>     s_liverun_done{false};

// Step-0 wedge classifier: snapshot the EE event/interrupt scheduler state. The
// recompiler and interpreter SHARE all of _cpuEventTest_Shared / intc / dmac /
// scheduling — the only jit-specific variables are (a) when a block re-enters the
// event test and (b) the cpuRegs.cycle value it has accumulated. So a "this IRQ
// never fires under jit" wedge is one of three modes, distinguishable here:
//   A  cycle FROZEN between snapshots          -> spin block costs 0 cycles / RECCYCLE stuck
//   B  cycle advances, INTC/DMAC pending+unmasked but never serviced -> arm64 codegen (event-test/x25)
//   C  cycle advances, nothing pending          -> IRQ never scheduled (IOP/SIF/DMA upstream)
// Racy reads of globals from the watchdog thread — fine for a stuck loop.
static void DumpEeEventState(int snap)
{
	const u64 cyc   = cpuRegs.cycle;
	const u64 nextE = cpuRegs.nextEventCycle;
	const u64 lastE = cpuRegs.lastEventCycle;
	const u32 ints  = cpuRegs.interrupt;
	const u32 stat  = cpuRegs.CP0.n.Status.val;
	const u32 intcS = psHu32(INTC_STAT);
	const u32 intcM = psHu32(INTC_MASK);
	const u16 dmacS = psHu16(0xe012);
	const u16 dmacM = psHu16(0xe010);
	Console.Error(fmt::format(
		"  [snap {}] cycle={} nextEvent={} (dEvt={}) lastEvent={} | interrupt=0x{:08x} branch={}",
		snap, cyc, nextE, (s64)(nextE - cyc), lastE, ints, cpuRegs.branch));
	Console.Error(fmt::format(
		"           CP0.Status=0x{:08x} (EIE={} ERL={} EXL={} IE={} IM_INTC={} IM_DMAC={})",
		stat, (stat >> 16) & 1, (stat >> 2) & 1, (stat >> 1) & 1, stat & 1,
		(stat >> 10) & 1, (stat >> 11) & 1));
	Console.Error(fmt::format(
		"           INTC_STAT=0x{:08x} INTC_MASK=0x{:08x} (pending&unmasked=0x{:08x}) | DMAC_STAT=0x{:04x} DMAC_MASK=0x{:04x} (pend=0x{:04x})",
		intcS, intcM, intcS & intcM, dmacS, dmacM, (u16)(dmacS & dmacM)));
	// Per-source scheduled EE interrupts: which channels have a deadline, and has cycle passed it?
	if (ints)
	{
		std::string sched;
		for (int n = 0; n < 32; n++)
		{
			if (!(ints & (1u << n)))
				continue;
			const u64 deadline = cpuRegs.sCycle[n] + cpuRegs.eCycle[n];
			sched += fmt::format(" int[{}]: sCycle={} eCycle={} deadline={} ({}); ",
				n, cpuRegs.sCycle[n], cpuRegs.eCycle[n], deadline,
				(s64)(deadline - cyc) <= 0 ? "DUE" : "future");
		}
		Console.Error(fmt::format("           scheduled:{}", sched));
	}
}

#ifdef __linux__
// Per-thread hardware counters over the liverun frame window (attached after
// savestate load, read at shutdown next to @THREADCPU@). The @THREADPERF@
// "CPU Thread" line gives codegen_ab.sh an EE-thread-SCOPED deterministic
// metric: whole-process `perf stat` dilutes an EE-only codegen delta by the
// GS/MTVU/worker threads' share (~50% measured on SotC), and the time-based
// @THREADCPU@ line is clock-sensitive. Counters attach per-tid, so boot and
// savestate-load work is excluded (JIT compile of the scene's blocks is not —
// it runs on the EE thread inside the window, identically for base/new).
struct ThreadPerfCounter
{
	std::string comm;
	int fd_ins = -1;
	int fd_cyc = -1;
	bool user_only = false;
};
static std::vector<ThreadPerfCounter> s_thread_perf;

static int OpenThreadPerfCounter(pid_t tid, u64 config, bool* user_only)
{
	perf_event_attr attr = {};
	attr.type = PERF_TYPE_HARDWARE;
	attr.size = sizeof(attr);
	attr.config = config;
	attr.exclude_hv = 1;
	int fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0));
	if (fd < 0 && errno == EACCES)
	{
		// perf_event_paranoid >= 2 (unprivileged dev box): user-space-only counting.
		attr.exclude_kernel = 1;
		fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0));
		if (fd >= 0)
			*user_only = true;
	}
	return fd;
}

// --threadperf-frontend: the CPU thread's L1 instruction cache refills, L1 instruction TLB refills, front-end stalls and branch
// mispredictions, from the core PMU's named events (/sys/bus/event_source/devices/<pmu>/events/), printed as @THREADPERF2@ beside
// @THREADPERF@. The counting is the one in the EE AOT campaign's R0 harness (ee-aot-r0 4ca814aa28) without the rest of that
// harness. Measurement only.
struct FrontendCounter
{
	std::string name;
	int fd;
};
static std::vector<FrontendCounter> s_fe_counters;
static std::string s_fe_pmu;

static bool ReadSmallFile(const std::string& path, std::string* out)
{
	std::ifstream f(path);
	if (!f)
		return false;
	std::getline(f, *out);
	return true;
}

// Whether `cpu` is in a sysfs CPU list such as "0-3,5".
static bool CpuInList(const std::string& list, int cpu)
{
	for (const std::string_view tok : StringUtil::SplitString(list, ','))
	{
		const std::string t(tok);
		int lo = 0, hi = 0;
		if (std::sscanf(t.c_str(), "%d-%d", &lo, &hi) == 2)
		{
			if (cpu >= lo && cpu <= hi)
				return true;
		}
		else if (std::sscanf(t.c_str(), "%d", &lo) == 1 && cpu == lo)
			return true;
	}
	return false;
}

static void OpenFrontendCounters(pid_t tid)
{
	// The PMU of the first CPU the thread may run on (the runs are confined to one cluster).
	cpu_set_t set;
	CPU_ZERO(&set);
	int cpu = 0;
	if (sched_getaffinity(tid, sizeof(set), &set) == 0)
	{
		for (int c = 0; c < CPU_SETSIZE; c++)
		{
			if (CPU_ISSET(c, &set))
			{
				cpu = c;
				break;
			}
		}
	}
	std::string dev;
	u32 type = 0;
	for (const auto& e : std::filesystem::directory_iterator("/sys/bus/event_source/devices"))
	{
		std::string cpus, t;
		if (!ReadSmallFile((e.path() / "cpus").string(), &cpus) || !CpuInList(cpus, cpu) || !ReadSmallFile((e.path() / "type").string(), &t))
			continue;
		dev = e.path().filename().string();
		type = static_cast<u32>(std::strtoul(t.c_str(), nullptr, 10));
		break;
	}
	s_fe_pmu = dev;
	static const char* const kWant[] = {"l1i_cache_refill", "l1i_tlb_refill", "stall_frontend", "br_mis_pred"};
	for (const char* w : kWant)
	{
		std::string ev;
		if (dev.empty() || !ReadSmallFile("/sys/bus/event_source/devices/" + dev + "/events/" + w, &ev))
			continue;
		const auto eq = ev.find("0x");
		if (eq == std::string::npos)
			continue;
		perf_event_attr a = {};
		a.size = sizeof(a);
		a.type = type;
		a.config = std::strtoull(ev.c_str() + eq + 2, nullptr, 16);
		a.exclude_hv = 1;
		a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
		int fd = static_cast<int>(syscall(SYS_perf_event_open, &a, tid, -1, -1, 0));
		if (fd < 0 && errno == EACCES)
		{
			a.exclude_kernel = 1;
			fd = static_cast<int>(syscall(SYS_perf_event_open, &a, tid, -1, -1, 0));
		}
		if (fd >= 0)
			s_fe_counters.push_back({w, fd});
	}
}

static void ReportFrontendCounters()
{
	if (!s_threadperf_frontend)
		return;
	if (s_fe_counters.empty())
	{
		Console.WriteLn(fmt::format("@THREADPERF2@ CPU Thread: no front-end counters on PMU '{}'", s_fe_pmu));
		return;
	}
	std::string line = fmt::format("@THREADPERF2@ CPU Thread pmu {}:", s_fe_pmu);
	for (FrontendCounter& c : s_fe_counters)
	{
		u64 v[3] = {};
		if (read(c.fd, v, sizeof(v)) != sizeof(v))
			v[0] = v[1] = v[2] = 0;
		close(c.fd);
		line += fmt::format(" {}={}", c.name, v[0]);
		if (v[1] != v[2])
			line += fmt::format("(multiplexed {}/{})", v[2], v[1]);
	}
	Console.WriteLn(line);
	s_fe_counters.clear();
}

static void StartThreadPerfCounters()
{
	for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task"))
	{
		const pid_t tid = static_cast<pid_t>(std::strtol(entry.path().filename().c_str(), nullptr, 10));
		std::ifstream comm_file(entry.path() / "comm");
		std::string comm;
		std::getline(comm_file, comm);
		ThreadPerfCounter c;
		c.comm = comm.empty() ? entry.path().filename().string() : comm;
		c.fd_ins = OpenThreadPerfCounter(tid, PERF_COUNT_HW_INSTRUCTIONS, &c.user_only);
		c.fd_cyc = OpenThreadPerfCounter(tid, PERF_COUNT_HW_CPU_CYCLES, &c.user_only);
		if (s_threadperf_frontend && comm == "CPU Thread")
			OpenFrontendCounters(tid);
		if (c.fd_ins >= 0 || c.fd_cyc >= 0)
			s_thread_perf.push_back(std::move(c));
	}
	if (s_thread_perf.empty())
		Console.Warning("liverun: perf_event_open failed for all threads; no @THREADPERF@ lines (perf_event_paranoid?)");
}

static void ReportThreadPerfCounters()
{
	for (ThreadPerfCounter& c : s_thread_perf)
	{
		u64 ins = 0, cyc = 0;
		if (c.fd_ins >= 0)
		{
			if (read(c.fd_ins, &ins, sizeof(ins)) != sizeof(ins))
				ins = 0;
			close(c.fd_ins);
		}
		if (c.fd_cyc >= 0)
		{
			if (read(c.fd_cyc, &cyc, sizeof(cyc)) != sizeof(cyc))
				cyc = 0;
			close(c.fd_cyc);
		}
		Console.WriteLn(fmt::format("@THREADPERF@ {}: instructions={} cycles={}{}",
			c.comm, ins, cyc, c.user_only ? " (user-only)" : ""));
	}
	s_thread_perf.clear();
	ReportFrontendCounters();
}
#endif

#ifdef __linux__
// The voluntary and involuntary context switches of the thread named `comm` (/proc/self/task/<tid>/status). A voluntary
// switch means the thread slept in the kernel: for the IOP thread that counts real sleeps (its parks are mostly spins,
// WorkSema spins for SPIN_TIME_NS before it sleeps); for the CPU thread, which must never sleep on the emulation path,
// it shows that it did not.
static bool ReadContextSwitches(const char* comm, u64* voluntary, u64* involuntary)
{
	for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task"))
	{
		std::ifstream comm_file(entry.path() / "comm");
		std::string name;
		std::getline(comm_file, name);
		if (name != comm)
			continue;
		std::ifstream status(entry.path() / "status");
		std::string line;
		bool got = false;
		while (std::getline(status, line))
		{
			if (line.rfind("voluntary_ctxt_switches:", 0) == 0)
			{
				*voluntary = std::strtoull(line.c_str() + 24, nullptr, 10);
				got = true;
			}
			else if (line.rfind("nonvoluntary_ctxt_switches:", 0) == 0)
			{
				*involuntary = std::strtoull(line.c_str() + 27, nullptr, 10);
			}
		}
		return got;
	}
	return false;
}
#endif

// --iop-stress: the things a player does between frames, at random (a fixed seed repeats the sequence): turn the IOP thread
// off and on and change its lag bound from inside the event test, save the state and load it, pause for a while (the IOP is
// held and goes to sleep), reset the VM, press a hotkey. These are the lifecycle paths the IOP thread has to stay
// deadlock-free across.
static struct StressCounts
{
	u32 switches = 0, lags = 0, saves = 0, loads = 0, load_failures = 0, pauses = 0, resets = 0, hotkeys = 0;
} s_stress_counts;

// --pause-toggle: the settings change a player makes on a paused VM. Between two frames of a liverun the VM is Paused, so the
// change goes through VMManager::ApplySettings() in that state (the pause hold is live), unlike --settings-churn, which delivers it
// from the message pump inside the EE's event test.
static void PauseToggle(u32 frame)
{
	const auto describe = [] {
		const char* const names[] = {"Off", "Lockstep", "Threaded"};
		return fmt::format("mode={} thread={} state={}", names[static_cast<int>(IopThread::GetMode())],
			IopThread::g_thread.IsRunning() ? "running" : "none", static_cast<int>(VMManager::GetState()));
	};
	const std::string before = describe();
	s_churn_kind = "threaded";
	ApplySettingsChurn();
	Console.WriteLn(fmt::format("PAUSE-TOGGLE@{}: [{}] -> [{}] {}", frame, before, describe(), VMManager::Internal::GetThreadPlacementDebug()));
}

static void StressStep(u32 frame)
{
	static u32 rng = 0;
	static bool saved = false;
	if (rng == 0)
	{
		rng = s_stress_seed * 2654435761u + 12345u;
		if (rng == 0)
			rng = 1;
	}
	const auto next = [] {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return rng;
	};
	const std::string path = EmuFolders::DataRoot + "/iop-stress.p2s";
	const u32 r = next() % 1000;
	if (r < 20)
	{
		s_churn_kind = "threaded";
		s_churn_pending = true; // applied at the next vsync, inside the event test, as a settings dialog would
		s_stress_counts.switches++;
	}
	else if (r < 35)
	{
		s_churn_kind = "lag-random";
		s_stress_lag_pick = next();
		s_churn_pending = true;
		s_stress_counts.lags++;
	}
	else if (r < 39)
	{
		bool ok = true;
		VMManager::SaveState(path.c_str(), /*zip_on_thread=*/false, /*backup_old_state=*/false, [&](const std::string& e) {
			ok = false;
			Console.ErrorFmt("STRESS: save failed: {}", e);
		});
		VMManager::WaitForSaveStateFlush();
		saved = saved || ok;
		s_stress_counts.saves++;
		Console.WriteLn(fmt::format("STRESS: frame {} saved a state: {} (IOP thread mode {})", frame, ok ? "ok" : "FAILED", static_cast<int>(IopThread::GetMode())));
	}
	else if (r < 43 && saved)
	{
		Error error;
		const bool ok = VMManager::LoadState(path.c_str(), &error);
		s_stress_counts.loads++;
		if (!ok)
			s_stress_counts.load_failures++;
		Console.WriteLn(fmt::format("STRESS: frame {} loaded the state: {}{}", frame, ok ? "ok" : "FAILED ", ok ? "" : error.GetDescription()));
	}
	else if (r < 58)
	{
		const u32 ms = next() % 40;
		Console.WriteLn(fmt::format("STRESS: frame {} paused for {} ms", frame, ms));
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		s_stress_counts.pauses++;
	}
	else if (r < 60)
	{
		Console.WriteLn(fmt::format("STRESS: frame {} reset the VM", frame));
		VMManager::Reset();
		s_stress_counts.resets++;
	}
	else if (r < 75)
	{
		static constexpr const char* kKeys[] = {"Mute", "IncreaseVolume", "DecreaseVolume", "ToggleSlowMotion", "ToggleTurbo"};
		s_host_actions_pending.push_back(kKeys[next() % (sizeof(kKeys) / sizeof(kKeys[0]))]);
		s_stress_counts.hotkeys++;
	}
}

static int RunLiveRun()
{
	Error error;
	if (!ValidateEeRateScript())
		return EXIT_FAILURE;

	if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
	{
		Console.ErrorFmt("liverun: load failed: {}", error.GetDescription());
		return EXIT_FAILURE;
	}

#ifdef __linux__
	StartThreadPerfCounters();
#endif
	s_liverun_first_frame = g_FrameCount;

	Console.WriteLn(fmt::format(
		"LIVERUN: EE={}, VU0={}, VU1={}, MTVU={}, real GS — running up to {} frames (10s no-progress watchdog)...",
		EmuConfig.Cpu.Recompiler.EnableEE ? "jit" : "interp",
		EmuConfig.Cpu.Recompiler.EnableVU0 ? "jit" : "interp",
		EmuConfig.Cpu.Recompiler.EnableVU1 ? "jit" : "interp",
		EmuConfig.Speedhacks.vuThread ? "on" : "off", s_frames));

	std::thread watchdog([]() {
		uint32_t last = 0;
		int stalled = 0;
		while (!s_liverun_done.load(std::memory_order_relaxed))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			if (s_liverun_done.load(std::memory_order_relaxed))
				return;
			const uint32_t cur = s_liverun_frame.load(std::memory_order_relaxed);
			if (cur != last)
			{
				last = cur;
				stalled = 0;
				continue;
			}
			if (++stalled < 20) // 20 * 500ms = ~10s of no frame progress
				continue;

			// Wedged: the EE has not finished a frame in ~10s. Sample the live EE PC
			// to fingerprint the spin/sync loop (racy read of a global u32 — fine for
			// a stuck loop whose PC sits in a tiny range).
			std::map<u32, int> hist;
			u32 pcmin = ~0u, pcmax = 0;
			const int N = 4000;
			for (int i = 0; i < N; i++)
			{
				const u32 pc = cpuRegs.pc;
				hist[pc]++;
				pcmin = std::min(pcmin, pc);
				pcmax = std::max(pcmax, pc);
				std::this_thread::sleep_for(std::chrono::microseconds(250));
			}
			Console.Error(fmt::format(
				"LIVERUN WEDGE: no frame completed past frame {} for ~10s. "
				"EE spin PC range [0x{:08x} .. 0x{:08x}], {} distinct PCs over {} samples:",
				last, pcmin, pcmax, hist.size(), N));
			std::vector<std::pair<u32, int>> top(hist.begin(), hist.end());
			std::sort(top.begin(), top.end(),
				[](const auto& a, const auto& b) { return a.second > b.second; });
			for (size_t i = 0; i < top.size() && i < 16; i++)
				Console.Error(fmt::format("    pc=0x{:08x}  {:5d}  ({:4.1f}%)",
					top[i].first, top[i].second, 100.0 * top[i].second / N));
			// Disassemble the dominant spin block so the poll/branch + the awaited
			// memory operand are visible. The EE is stuck looping in this one block,
			// so its code bytes are stable to read from here.
			if (!top.empty())
				DisasmBlock(top[0].first);

			// Step-0 wedge classifier: two snapshots of the EE event/interrupt
			// scheduler ~200ms apart. Whether cpuRegs.cycle MOVES between them, and
			// whether an INTC/DMAC source is pending-but-unserviced, classifies the
			// wedge into mode A (cycle frozen), B (codegen: pending never delivered),
			// or C (never scheduled — upstream). See DumpEeEventState().
			Console.Error("LIVERUN WEDGE: EE event/interrupt scheduler state:");
			DumpEeEventState(0);
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			DumpEeEventState(1);

			std::fflush(stdout);
			std::fflush(stderr);
			std::_Exit(42); // distinct from EXIT_FAILURE; bypass GS teardown deliberately
		}
	});

	// EERUNNER_EXITSTORM=<period_us>: fire Cpu->ExitExecution() from a foreign
	// thread at randomized intervals averaging ~period_us, mimicking the Android
	// JNI pause/suspend churn (native-lib pause() calls ExitExecution cross-thread
	// against a running EE). Hunts clock corruption at the delta-pinning seams:
	// recSafeExitExecution writes cpuRegs.nextEventCycle=0 while JIT blocks hold
	// RECCYCLE = cycle - nextEventCycle, so a mid-chain hit skews the next flush.
	std::atomic<bool> storm_stop{false};
	std::thread stormthread;
	u32 storm_period = 0;
	if (const char* e = std::getenv("EERUNNER_EXITSTORM"))
		storm_period = static_cast<u32>(strtoul(e, nullptr, 0));
	if (storm_period)
	{
		Console.WriteLn(fmt::format("LIVERUN: EXITSTORM armed, ~{}us between cross-thread ExitExecution calls", storm_period));
		stormthread = std::thread([&storm_stop, storm_period]() {
			u32 lcg = 0x12345678;
			while (!storm_stop.load(std::memory_order_relaxed))
			{
				lcg = lcg * 1664525u + 1013904223u;
				std::this_thread::sleep_for(std::chrono::microseconds(storm_period / 2 + (lcg % storm_period)));
				if (Cpu)
					Cpu->ExitExecution();
			}
		});
	}

	// Soft-freeze probe: the frame-count watchdog can't see this hang, because the EE
	// keeps ticking vblank so frames KEEP completing (frozen frames). Instead watch a
	// guest EE-RAM word (EERUNNER_WATCH_ADDR) that should keep changing during normal
	// play; if it stays constant for many frames WHILE the frame counter advances, the
	// game is frozen even though the harness "completes" frames. For Burnout 3 the
	// idle-loop consumer pointer is 0x4e2838 (waits to equal 0x4e283c == addr+4).
	u32 watch_addr = 0;
	if (const char* e = std::getenv("EERUNNER_WATCH_ADDR"))
		watch_addr = static_cast<u32>(strtoul(e, nullptr, 0));
	auto eeR32 = [](u32 paddr) -> u32 {
		return *reinterpret_cast<const u32*>(reinterpret_cast<const char*>(eeMem->Main) + (paddr & 0x1ffffffu));
	};
	u32 watch_prev = 0;
	bool watch_init = false;
	int watch_stuck = 0;

	EeRateDriver rate("liverun");

	for (uint32_t f = 0; f < s_frames && VMManager::GetState() != VMState::Shutdown; ++f)
	{
		const EeRateFrameTimer frame_timer; // starts before the transition: the reset is part of the frame that pays it
		rate.BeforeFrame(f);
		VMManager::FrameAdvance(1);
		VMManager::Execute(); // blocks for one frame; if the EE wedges, never returns
		// A settings change delivered inside the frame (--settings-churn, --iop-stress) resets the recompilers and leaves the
		// execution loop early, as it does for a player; carry on until the frame ends.
		while ((s_churn_every || s_stress) && VMManager::GetState() == VMState::Running)
			VMManager::Execute();
		rate.AfterFrame(frame_timer.Elapsed());
		if (s_churn_every && ((f + 1) % s_churn_every) == 0)
			s_churn_pending = true;
		if (s_stress)
			StressStep(f + 1);
		s_liverun_frame.store(f + 1, std::memory_order_relaxed);
#if defined(ARCH_ARM64) && defined(PCSX2_RECOMPILER_TESTS)
		if (s_vuaot_trip_at && (f + 1) == s_vuaot_trip_at)
		{
			Console.WriteLn(fmt::format("LIVERUN: setting the vu-aot sticky-STATUS tripwire off after frame {}", f + 1));
			vuaot::live::TripStickyForTest();
		}
#endif
		if (s_reload_state_at && (f + 1) == s_reload_state_at)
		{
			Console.WriteLn(fmt::format("LIVERUN: loading the savestate again after frame {}", f + 1));
			Error error;
			if (!VMManager::LoadState(s_savestate_path.c_str(), &error))
			{
				Console.ErrorFmt("LIVERUN: the reload failed: {}", error.GetDescription());
				return 1;
			}
		}

		// Where the threads sit, once the scene has settled: the placement the VM chose (the affinity masks) and the core
		// each thread last ran on. The IOP thread is in it while it runs.
		if ((f + 1) == 100)
			Console.WriteLn(fmt::format("PLACEMENT@{}: {}", f + 1, VMManager::Internal::GetThreadPlacementDebug()));
		if (s_pause_toggle_at && (f + 1) == s_pause_toggle_at)
			PauseToggle(f + 1);
		if (s_iop_stats_every && ((f + 1) % s_iop_stats_every) == 0)
		{
			const std::string line = IopThread::OsdStatsLine();
			if (!line.empty())
				Console.WriteLn(fmt::format("IOPSTATS@{}: {}", f + 1, line));
		}
		if (s_change_disc_at && (f + 1) == s_change_disc_at)
			s_change_disc_pending = true;
		for (const HostAction& action : s_host_actions)
		{
			if (action.frame == f + 1)
				s_host_actions_pending.push_back(action.hotkey);
		}

		// Arm the GS dump once the scene has settled. Recording stops on its own
		// after s_gsdump_frames vsyncs (GSRenderer counts m_dump_frames down), so
		// the run just has to outlast it — hence the frame-budget check below.
		if (!s_gsdump_path.empty() && (f + 1) == s_gsdump_at)
		{
			Console.WriteLn(fmt::format("GSDUMP: recording {} frame(s) from frame {} to '{}'",
				s_gsdump_frames, s_gsdump_at, s_gsdump_path));
			const std::string path = s_gsdump_path;
			const u32 nframes = s_gsdump_frames;
			MTGS::RunOnGSThread([path, nframes]() { GSQueueSnapshot(path, nframes); });
		}

		if (!s_shots_prefix.empty() && (f + 1) >= s_shots_from && ((f + 1) - s_shots_from) % s_shots_every == 0)
		{
			const std::string path = fmt::format("{}f{:05d}.png", s_shots_prefix, f + 1);
			MTGS::RunOnGSThread([path]() { GSQueueSnapshot(path, 0); });
		}

		if (watch_addr)
		{
			const u32 v = eeR32(watch_addr);
			if (watch_init && v == watch_prev)
			{
				if (++watch_stuck == 120) // ~2s of frozen game while frames advanced
				{
					const u32 vb = eeR32(watch_addr + 4);
					Console.Error(fmt::format(
						"LIVERUN SOFT-FREEZE: watch[0x{:08x}]=0x{:08x} unchanged for {} frames "
						"(frame counter reached {}); neighbor[+4]=0x{:08x}. Game frozen, vblank still ticking.",
						watch_addr, v, watch_stuck, f + 1, vb));
					Console.Error("LIVERUN SOFT-FREEZE: EE event/interrupt scheduler state:");
					DumpEeEventState(0);
					// Fingerprint the EE spin PCs (game idle loop).
					std::map<u32, int> hist;
					for (int i = 0; i < 2000; i++)
					{
						hist[cpuRegs.pc]++;
						std::this_thread::sleep_for(std::chrono::microseconds(200));
					}
					std::vector<std::pair<u32, int>> top(hist.begin(), hist.end());
					std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
					for (size_t i = 0; i < top.size() && i < 8; i++)
						Console.Error(fmt::format("    pc=0x{:08x}  {:5d}", top[i].first, top[i].second));
					// Disasm a window around the hottest spin PC — the loop the game
					// is actually stuck in — so this is game-agnostic. Override the
					// range with EERUNNER_DIS_LO..EERUNNER_DIS_HI when chasing a
					// divergence whose deciding branch sits outside the spin window.
					{
						const u32 center = top.empty() ? cpuRegs.pc : top[0].first;
						u32 lo = center - 0x40, hi = center + 0x40;
						if (const char* e = std::getenv("EERUNNER_DIS_LO")) lo = (u32)strtoul(e, nullptr, 0);
						if (const char* e = std::getenv("EERUNNER_DIS_HI")) hi = (u32)strtoul(e, nullptr, 0);
						Console.Error(fmt::format("LIVERUN SOFT-FREEZE: disasm 0x{:08x}..0x{:08x}:", lo, hi));
						for (u32 a = lo; a <= hi; a += 4)
						{
							const u32 code = memRead32(a);
							std::string line;
							R5900::disR5900Fasm(line, code, a, false);
							Console.Error(fmt::format("    {:#010x}: {:08x}  {}", a, code, line));
						}
					}
					std::fflush(stdout);
					std::fflush(stderr);
					std::_Exit(43); // distinct from 42 (frame-count wedge)
				}
			}
			else
			{
				watch_stuck = 0;
			}
			watch_prev = v;
			watch_init = true;
		}
	}

	s_liverun_done.store(true, std::memory_order_relaxed);
	storm_stop.store(true, std::memory_order_relaxed);
	if (stormthread.joinable())
		stormthread.join();
	watchdog.join();

	// Per-frame GS stats (averaged over g_perfmon's last 32-frame window): draws, render
	// passes, readbacks (RB), texture copies/uploads. Read cross-thread while the VM is
	// still up — racy u64 reads, cosmetic-only. @GSSTAT@ so scripts can grep it.
	{
		SmallString gs_stats;
		GSgetStats(gs_stats);
		Console.WriteLn(fmt::format("@GSSTAT@ per-frame: {}", gs_stats.view()));
	}

#ifdef __linux__
	ReportThreadPerfCounters();

	// Per-thread CPU seconds (utime+stime) while the VM threads are still alive.
	// Wallclock A/B on the SD865 is too noisy for sub-ms/frame codegen deltas; the
	// per-thread number isolates e.g. GS-thread cost from GPU/sync/scheduler jitter.
	// comm can contain spaces ("CPU Thread") — parse /proc stat around the ')'.
	{
		const long tck = sysconf(_SC_CLK_TCK);
		for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task"))
		{
			std::ifstream stat_file(entry.path() / "stat");
			std::string line;
			if (!std::getline(stat_file, line))
				continue;
			const size_t rp = line.rfind(')');
			if (rp == std::string::npos)
				continue;
			const std::string comm = line.substr(line.find('(') + 1, rp - line.find('(') - 1);
			std::istringstream rest(line.substr(rp + 2));
			std::string field;
			u64 utime = 0, stime = 0;
			for (int i = 1; rest >> field; i++) // field 1 here = stat field 3 (state)
			{
				if (i == 12)
					utime = std::strtoull(field.c_str(), nullptr, 10);
				else if (i == 13)
				{
					stime = std::strtoull(field.c_str(), nullptr, 10);
					break;
				}
			}
			Console.WriteLn(fmt::format("@THREADCPU@ {}: {:.2f} s", comm,
				static_cast<double>(utime + stime) / static_cast<double>(tck)));
		}
	}
#endif

#if defined(ARCH_ARM64)
	if (vuaot::live::IsEnabled())
		Console.WriteLn(fmt::format("@VUAOT@ {}", vuaot::live::FormatStats()));
#if defined(PCSX2_RECOMPILER_TESTS)
	{
		u64 ins = 0, disp = 0;
		mvu_test_hooks::GetVu1InstructionCount(ins, disp);
		if (disp)
			Console.WriteLn(fmt::format("@VU1INS@ dispatches={} instructions={}", disp, ins));
		Console.WriteLn(fmt::format("@VU1STOPS@ budget_stops={}", mvu_test_hooks::GetVu1Stops()));
	}
#endif
#endif
	Console.WriteLn(fmt::format(
		"LIVERUN: completed {} frames with NO wedge — this config did not reproduce the hang.",
		s_frames));
	ReportEeChargeCensus("liverun end");
	if (s_ee_rate_failed)
	{
		Console.Error("LIVERUN: FAILED — a scripted EE cycle-rate transition was refused or reset a "
		              "provider it must not. See the EERATE lines above.");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

#ifdef _WIN32
// Unicode filenames require wmain on Win32; use the ascii main() with this workaround.
#define main real_main
#endif

static void CPUThreadMain(VMBootParameters* params, std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);

	if (VMManager::Internal::CPUThreadInitialize())
	{
		// Profiling: set the jitdump output dir before any JIT block compiles (the
		// first compile happens during the first FrameAdvance, well after this). Dir =
		// EmuFolders::Cache so the 100s-of-MB dump avoids /tmp/tmpfs, matching the
		// production rationale in common/Perf.cpp. The ENABLE flag is driven through the
		// normal settings path instead (EmuCore/Profiler EnablePerfDump, set in the
		// harness config) — ApplySettings() below calls LoadSettings() which re-applies
		// Perf::SetJitDumpEnabled(EnablePerfDump), so a manual enable here would just get
		// reset to the config default (false). No-op on non-jitdump builds.
		// EERUNNER_LEGACY_CORE: building against a pre-transplant emu core (ARMSX2
		// master) whose common/Perf.h has no SetJitDumpDir — the dump lands in
		// the default dir there, which is fine for the twin golden side.
#ifndef EERUNNER_LEGACY_CORE
		if (s_perf_jitdump)
			Perf::SetJitDumpDir(EmuFolders::Cache);
#endif

		// apply new settings (e.g. pick up renderer change)
		VMManager::ApplySettings();

		for (const bool on : s_no_vm_vuaot_flips)
		{
			Console.WriteLn(fmt::format("NOVM-FLIP: EnableVUAot -> {}, settings applied with no VM running", on ? "on" : "off"));
			s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", on);
			VMManager::ApplySettings();
		}

		if (VMManager::Initialize(*params) == VMBootResult::StartupSuccess)
		{
			// Run unlimited — the runner steps frame-by-frame and needs no
			// wall-clock pacing.
			VMManager::SetLimiterMode(LimiterModeType::Unlimited);
			VMManager::SetState(VMState::Paused);

			int code = EXIT_FAILURE;
			switch (s_mode)
			{
				case RunMode::SelfCheck:
					code = RunSelfCheck();
					break;

				// --localize / --repro / --stepdiff all run the robust
				// checkpoint-anchored comparison. The frame-boundary pass
				// conflates codegen bugs with the ~10-cycle async pause-point
				// sampling jitter (see --selfcheck, which characterizes that
				// jitter). --repro is the fast iteration verb (point it at a
				// savestate already narrowed to the bug);
				// --localize/--stepdiff are aliases.
				case RunMode::Localize:
				case RunMode::Repro:
				case RunMode::StepDiff:
					code = RunStepDiff();
					break;

				case RunMode::Vu0Diff:
					code = RunVu0Diff();
					break;

				case RunMode::ContMem:
					code = RunContinuousMemTrajectory();
					break;

				case RunMode::TwinDump:
					code = RunTwinDump();
					break;

				case RunMode::TwinCompare:
					code = RunTwinCompare();
					break;

				case RunMode::DumpConfig:
					code = RunDumpConfig();
					break;

				case RunMode::MkState:
					code = RunMkState();
					break;

				case RunMode::SpeedhackDiff:
					code = RunSpeedhackDiff();
					break;

				case RunMode::LiveRun:
					code = RunLiveRun();
					break;

				case RunMode::Disasm:
					code = RunDisasm();
					break;

				case RunMode::StateReport:
					code = RunStateReport();
					break;

				default:
					break;
			}

			if (s_mode == RunMode::LiveRun)
				Console.WriteLn(fmt::format("PLACEMENT-END: {}", VMManager::Internal::GetThreadPlacementDebug()));
			// What the IOP thread did over the run, for the Lockstep cost record (all zero in Off).
			{
				const IopThread::Stats st = IopThread::GetStats();
				const char* const mode_names[] = {"Off", "Lockstep", "Threaded"};
				Console.WriteLn(fmt::format("IOPTHREAD: mode={} handoffs={} parks={} wakes_sent={} slices={} items={} violations={} holds={}",
					mode_names[static_cast<int>(IopThread::GetMode())], st.handoffs, st.parks, st.wakes_sent, st.slices,
					st.items_applied, IopThread::ViolationCount(), st.hold_requests + st.hold_markers));
				if (s_mode == RunMode::LiveRun && s_stress)
				{
					Console.WriteLn(fmt::format("STRESS-SUMMARY: seed={} iop_thread_switches={} lag_changes={} saves={} loads={} (failed {}) pauses={} resets={} hotkeys={}",
						s_stress_seed, s_stress_counts.switches, s_stress_counts.lags, s_stress_counts.saves, s_stress_counts.loads,
						s_stress_counts.load_failures, s_stress_counts.pauses, s_stress_counts.resets, s_stress_counts.hotkeys));
				}
				if (s_mode == RunMode::LiveRun)
				{
					// What the EE and the IOP did per frame, in every mode, for the "sane against Off" check of the Threaded runs: EE event
					// tests, and IOP slices (Off and Lockstep count them on the EE side; Threaded is the thread's own count).
					const double frames = static_cast<double>(g_FrameCount - s_liverun_first_frame);
					const u64 slices = IopThread::GetMode() == IopThreadMode::Threaded ? st.slices : g_twinIopSlices;
					Console.WriteLn(fmt::format("COUNTS: mode={} frames={} ee_event_tests={} ({:.0f}/frame) iop_slices={} ({:.0f}/frame)",
						mode_names[static_cast<int>(IopThread::GetMode())], static_cast<u32>(frames), g_twinEeEventTests, g_twinEeEventTests / std::max(frames, 1.0),
						slices, slices / std::max(frames, 1.0)));
				}
				if (IopThread::GetMode() == IopThreadMode::Threaded && s_mode == RunMode::LiveRun)
				{
					const double frames = static_cast<double>(g_FrameCount - s_liverun_first_frame);
					Console.WriteLn(fmt::format("IOPSTATS: frames={} {}", static_cast<u32>(frames), IopThread::FormatStats(st, frames)));
					Console.WriteLn(fmt::format(
						"IOPSTATS-RAW: lag_spins={} lag_spin_iterations={} lag_stall_ns={} lag_spins_parked={} lag_parked_stall_ns={} emu_hold_requests={} emu_hold_stall_ns={} "
						"hold_requests={} hold_markers={} sif_ee_pumps={} calls_posted={} calls_served={} i2e_drained={} slices={} items_applied={} parks={} wakes_sent={} wakes_dropped={} "
						"holds_entered={} kicks_applied={} i2e_full_waits={} e2i_full_spins={} hold_sleeps={} lag_spins_idle={} lag_idle_stall_ns={} idle_waits={} idle_ns={} "
						"sleep_barrier_membarrier={}",
						st.lag_spins, st.lag_spin_iterations, st.lag_stall_ns, st.lag_spins_parked, st.lag_parked_stall_ns, st.emu_hold_requests, st.emu_hold_stall_ns,
						st.hold_requests, st.hold_markers, st.sif_ee_pumps, st.calls_posted, st.calls_served, st.i2e_drained, st.slices, st.items_applied, st.parks, st.wakes_sent,
						st.wakes_dropped, st.holds_entered, st.kicks_applied, st.i2e_full_waits, st.e2i_full_spins, st.hold_sleeps, st.lag_spins_idle, st.lag_idle_stall_ns,
						st.idle_waits, st.idle_ns, st.sleep_barrier_membarrier));
#ifdef __linux__
					u64 iop_vol = 0, iop_invol = 0, ee_vol = 0, ee_invol = 0;
					const bool have_iop = ReadContextSwitches("IOP", &iop_vol, &iop_invol);
					const bool have_ee = ReadContextSwitches("CPU Thread", &ee_vol, &ee_invol);
					if (have_iop && have_ee)
						Console.WriteLn(fmt::format("IOPSTATS-CTXT: iop_thread_sleeps={} iop_thread_preemptions={} ee_thread_sleeps={} ee_thread_preemptions={} "
													"(whole process; the EE thread's sleeps include the savestate load and the frame-by-frame pause)",
							iop_vol, iop_invol, ee_vol, ee_invol));
#endif
				}
			}

			VMManager::Shutdown(false);
			ret->store(code);
		}
		else
		{
			Console.Error("eerunner: VMManager::Initialize failed.");
		}
	}

	VMManager::Internal::CPUThreadShutdown();
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	EERunner::InitializeConsole();

	std::signal(SIGINT, [](int) { VMManager::SetState(VMState::Stopping); });
	std::signal(SIGTERM, [](int) { VMManager::SetState(VMState::Stopping); });

	if (!EERunner::InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return EXIT_FAILURE;
	}

	VMBootParameters params;
	if (!EERunner::ParseCommandLineArgs(argc, argv, params))
		return EXIT_FAILURE;

	// EERUNNER_LEGACY_CORE: pre-transplant cores allocate VM memory inside
	// VMManager::Internal::CPUThreadInitialize (SysMemory::Allocate) instead of
	// exposing a frontend-driven reservation.
#ifndef EERUNNER_LEGACY_CORE
	SysMemory::ReserveMemory();
#endif

	// --selfcheck and --vu0diff force the EE interpreter. Must be set BEFORE
	// VMManager::Initialize. (--vu0diff toggles only VU0; the EE stays interp in
	// both passes so the only moving part is the VU0 micro engine.)
	if (s_mode == RunMode::SelfCheck || s_mode == RunMode::Vu0Diff)
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", false);

	// The checkpoint-anchored modes emit the per-block divtrace hook into every
	// JIT block prologue (used by the zoom's sparse JIT stream). g_emit_block_hook
	// is read at block-compile time, so it must be set before the recompiler is
	// initialized; it stays true for the whole process (the interp passes simply
	// never compile EE blocks, and the hook is a g_enabled-gated no-op otherwise).
	// Production builds never set it, so they emit nothing.
	if (s_mode == RunMode::Localize || s_mode == RunMode::Repro || s_mode == RunMode::StepDiff)
		ee_divtrace::g_emit_block_hook = true;

	// Mirror the JIT's recSYSCALL FlushCache/iFlushCache skip in the golden interp
	// passes so both timelines stay bit-identical across that ABI-benign divergence
	// (otherwise the JIT skip vs interp-runs-handler shows up as a register + kernel-
	// stack diff that masks real bugs downstream). Harmless for --selfcheck (both
	// interp runs skip identically), so we set it for every mode the runner uses.
	ee_divtrace::g_skip_flushcache_syscall = true;

	// EERUNNER_NOFP=1: drop the FPU register file + ACC from the alignment
	// fingerprint and the diff helpers, so the zoom walks PAST FP-register
	// divergences to hunt a non-FP (integer / control-flow) divergence. Use when the
	// FP path is known benign (Burnout 3: hang persists with the EE-FPU fully
	// converged to interp — the real bug is the integer cond_b/pointer math, and the
	// pervasive 1-ULP div.s noise was masking it). Combine with EERUNNER_FPUFULL=1 to
	// also converge mul/add and minimize FP laundered into GPRs via store/reload.
	if (const char* e = std::getenv("EERUNNER_NOFP"))
		ee_divtrace::g_fp_exclude = (e[0] != '0');

	// Override settings that shouldn't be picked up from defaults or INIs.
	EERunner::SettingsOverride();

	std::atomic<int> thread_ret;
	std::thread cputhread(CPUThreadMain, &params, &thread_ret);
	cputhread.join();

	return thread_ret.load();
}

#ifdef _WIN32

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

	return real_main(argc, u8_argptrs.data());
}

#endif // _WIN32
