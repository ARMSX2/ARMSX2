// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "AutoTune/AutoTuneApplier.h"

#include "Achievements.h"
#include "Config.h"
#include "EECycleRateSampler.h"
#include "GameDatabase.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSBackThreadPolicy.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "IopThread/IopThreadVm.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"
#include "Arcade/ArcadeInput.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Timer.h"

#include "IconsFontAwesome.h"
#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>

// The controller speaks in plain integers; they have to be the values the settings files hold.
static_assert(static_cast<int>(GSHardwareDownloadMode::Enabled) == AutoTuneDownloadMode::Accurate);
static_assert(static_cast<int>(GSHardwareDownloadMode::EnabledForceFull) == AutoTuneDownloadMode::AccurateForceFull);
static_assert(static_cast<int>(GSHardwareDownloadMode::NoReadbacks) == AutoTuneDownloadMode::NoReadbacks);
static_assert(static_cast<int>(GSHardwareDownloadMode::Unsynchronized) == AutoTuneDownloadMode::Unsynchronized);
static_assert(static_cast<int>(GSHardwareDownloadMode::Disabled) == AutoTuneDownloadMode::Disabled);
static_assert(static_cast<int>(GSHardwareDownloadMode::Asynchronous) == AutoTuneDownloadMode::Asynchronous);
static_assert(static_cast<int>(AccBlendLevel::Minimum) == 0 && static_cast<int>(AccBlendLevel::Basic) == 1 && static_cast<int>(AccBlendLevel::Maximum) == 5);

void (*AutoTune::g_on_committed)(const char* section, const char* key, std::string_view value) = nullptr;

namespace
{
	std::atomic<int> s_applying_depth{0};
}

bool AutoTune::IsApplyingSettings()
{
	return s_applying_depth.load(std::memory_order_relaxed) > 0;
}

AutoTune::ApplyScope::ApplyScope()
{
	s_applying_depth.fetch_add(1, std::memory_order_relaxed);
}

AutoTune::ApplyScope::~ApplyScope()
{
	s_applying_depth.fetch_sub(1, std::memory_order_relaxed);
}

namespace
{
	// ", target cost 127.00 -> 4.75 ms per frame (-96%)" when the cost the lever targets was measured on both sides.
	std::string CostText(const AutoTuneAction& a)
	{
		if (a.cost_before <= 0.0 || a.cost_after <= 0.0)
			return {};
		return fmt::format(", target cost {:.2f} -> {:.2f} ms per frame ({:+.0f}%)", a.cost_before, a.cost_after, -a.cost_gain * 100.0);
	}
} // namespace

// ================================================================================================
// AutoTuneApplier
// ================================================================================================

AutoTuneApplier::AutoTuneApplier(AutoTuneEnv& env)
	: m_env(env)
{
}

void AutoTuneApplier::Begin(std::vector<AutoTuneRejection> persisted_rejections)
{
	{
		auto lock = m_env.LockSettings();
		m_trial_layer.Clear();
	}
	m_trial_active = false;
	m_trial_previous.reset();
	m_rejected = std::move(persisted_rejections);
}

void AutoTuneApplier::End()
{
	auto lock = m_env.LockSettings();
	m_trial_layer.Clear();
	m_trial_active = false;
	m_trial_previous.reset();
}

void AutoTuneApplier::CancelTrial(bool apply)
{
	if (!m_trial_active)
		return;
	RestoreTrialKey();
	m_trial_active = false;
	if (apply)
		m_env.ApplySettings();
}

void AutoTuneApplier::RestoreTrialKey()
{
	const AutoTune::LeverKey k = AutoTune::KeyForLever(m_trial_lever);
	auto lock = m_env.LockSettings();
	if (m_trial_previous)
		m_trial_layer.SetStringValue(k.section, k.key, m_trial_previous->c_str());
	else
		m_trial_layer.DeleteValue(k.section, k.key);
	m_trial_previous.reset();
}

void AutoTuneApplier::Execute(const AutoTuneAction& action)
{
	using Kind = AutoTuneAction::Kind;
	switch (action.kind)
	{
		case Kind::None:
			break;

		case Kind::StartTrial:
			StartTrial(action);
			break;

		case Kind::Keep:
			Keep(action);
			break;

		case Kind::Revert:
			Revert(action);
			break;

		case Kind::ReportBottleneck:
		{
			const bool vu1 = action.bottleneck == AutoTuneBottleneck::Vu1;
			m_env.Log(fmt::format("{} is the bottleneck; none of the settings auto-tune changes can help", AutoTuneBottleneckName(action.bottleneck)));
			if (vu1)
				m_env.ShowMessage(std::string(TRANSLATE_SV("AutoTune", "Auto-tune: VU1 is the bottleneck. None of the settings it can change would help.")));
			else
				m_env.ShowMessage(std::string(TRANSLATE_SV("AutoTune", "Auto-tune: the IOP thread is the bottleneck. None of the settings it can change would help.")));
			break;
		}

		case Kind::Unclassified:
			m_env.Log("running slow, but no thread or GPU stands out; nothing to try");
			break;

		case Kind::Exhausted:
			m_env.Log(fmt::format("nothing left to try (bottleneck: {})", AutoTuneBottleneckName(action.bottleneck)));
			m_env.ShowMessage(fmt::format(TRANSLATE_FS("AutoTune", "Auto-tune: nothing left to try (bottleneck: {})"),
				AutoTuneBottleneckName(action.bottleneck)));
			break;
	}
}

void AutoTuneApplier::StartTrial(const AutoTuneAction& action)
{
	// A previous trial that was never resolved (it should not happen) is dropped first so its key is not stranded.
	if (m_trial_active)
		RestoreTrialKey();

	const AutoTune::LeverKey k = AutoTune::KeyForLever(action.lever);
	{
		auto lock = m_env.LockSettings();
		std::string previous;
		m_trial_previous = m_trial_layer.GetStringValue(k.section, k.key, &previous) ? std::optional<std::string>(previous) : std::nullopt;
		AutoTune::WriteLeverValue(m_trial_layer, action.lever, action.value);
	}
	m_trial_active = true;
	m_trial_lever = action.lever;
	m_trial_value = action.value;

	const std::string label = AutoTune::LeverLabel(action.lever, action.value);
	m_env.Log(fmt::format("trying {} (bottleneck {}, speed {:.1f}%{})", label, AutoTuneBottleneckName(action.bottleneck), action.speed_before,
		action.cost_before > 0.0 ? fmt::format(", {:.2f} ms per frame on the target", action.cost_before) : std::string()));
	m_env.ShowMessage(fmt::format(TRANSLATE_FS("AutoTune", "Auto-tune: trying {}..."), label));

	// The settle period starts after this returns: the controller waits out the windows that follow.
	m_env.ApplySettings();
}

void AutoTuneApplier::Keep(const AutoTuneAction& action)
{
	const std::string label = AutoTune::LeverLabel(action.lever, action.value);
	m_env.Log(fmt::format("keeping {}: speed {:.1f}% -> {:.1f}% ({:+.1f}%){}", label, action.speed_before, action.speed_after, action.gain * 100.0,
		CostText(action)));

	// A keep the full-speed rule earned on a game that was at full speed with late frames can have a speed change
	// a little under zero. What the player is told and what the game's file records is 0 at the least: a keep is
	// never shown as a loss. The log line above has the measured figure.
	const double gain = std::max(action.gain, 0.0);

	AutoTune::Change change;
	const bool persisted = m_env.EditGameFile([&](SettingsInterface& ini) {
		change = AutoTune::Commit(ini, action.lever, action.value, gain);
	});

	if (persisted)
	{
		// The file holds the value now. Drop the trial's copy; the game layer is re-read from the file below, so
		// the value never goes missing from the layers in between (nothing applies settings in this gap).
		m_trial_active = false;
		m_trial_previous.reset();
		{
			const AutoTune::LeverKey k = AutoTune::KeyForLever(action.lever);
			auto lock = m_env.LockSettings();
			m_trial_layer.DeleteValue(k.section, k.key);
		}
		m_env.NotifyCommitted(change.section.c_str(), change.key.c_str(), change.value);
		m_env.ReloadGameSettings();
		m_env.Log(fmt::format("saved {}/{} = {} in the game's settings", change.section, change.key, change.value));
	}
	else
	{
		// No settings file to write (no disc CRC, or it could not be saved). The value stays in the trial layer
		// and holds for this session only.
		m_trial_active = false;
		m_trial_previous.reset();
		m_env.Log(fmt::format("could not save {} in the game's settings; kept for this session only", label));
	}

	if (action.lever == AutoTuneLever::EeGovernor)
	{
		// The governor lowers the emulated EE's clock so the game keeps up with real time; the gain is game
		// speed, and a bare percentage reads like a frame-rate gain.
		m_env.ShowMessage(fmt::format(TRANSLATE_FS("AutoTune",
										  "Auto-tune: {} kept. The emulated CPU is underclocked to hold game speed (game speed {:+.0f}%, not more frames)."),
			label, gain * 100.0));
	}
	else
	{
		m_env.ShowMessage(fmt::format(TRANSLATE_FS("AutoTune", "Auto-tune: {} kept ({:+.0f}%)"), label, gain * 100.0));
	}
}

void AutoTuneApplier::Revert(const AutoTuneAction& action)
{
	const std::string label = AutoTune::LeverLabel(action.lever, action.value);

	if (m_trial_active)
		RestoreTrialKey();
	m_trial_active = false;

	if (action.measured)
	{
		const AutoTuneRejection rejection{action.lever, action.value};
		m_rejected.push_back(rejection);
		m_env.EditGameFile([&](SettingsInterface& ini) { AutoTune::RecordRejected(ini, rejection); });
		m_env.Log(fmt::format("{} did not help (speed {:.1f}% -> {:.1f}%, {:+.1f}%){}; undone", label, action.speed_before, action.speed_after,
			action.gain * 100.0, CostText(action)));
		m_env.ShowMessage(fmt::format(TRANSLATE_FS("AutoTune", "Auto-tune: {} did not help, undone"), label));
	}
	else if (action.retry)
	{
		// The game changed while the lever was tried, so the loss is not the lever's to answer for. Undone and not
		// written down; the controller measures the step again from a fresh baseline.
		m_env.Log(fmt::format("{} was undone because the game changed while it was tried (speed {:.1f}% -> {:.1f}%, {:+.1f}%){}; not remembered, will be measured again",
			label, action.speed_before, action.speed_after, action.gain * 100.0, CostText(action)));
		m_env.ShowMessage(fmt::format(TRANSLATE_FS("AutoTune", "Auto-tune: the game changed while {} was tried, undone; measuring again"), label));
	}
	else if (action.inconclusive)
	{
		// Within the noise even after a longer look: undone, and not written to the game's file, because a result
		// that could be noise must not keep the same step from being tried for good.
		m_env.Log(fmt::format("{} was inconclusive (speed {:.1f}% -> {:.1f}%, {:+.1f}%){}; undone, not remembered", label, action.speed_before,
			action.speed_after, action.gain * 100.0, CostText(action)));
		m_env.ShowMessage(fmt::format(TRANSLATE_FS("AutoTune", "Auto-tune: {} made no clear difference, undone"), label));
	}
	else
	{
		m_env.Log(fmt::format("trial of {} cancelled", label));
	}

	m_env.ApplySettings();
}

// ================================================================================================
// Decisions that depend on the host and the game database
// ================================================================================================

AutoTune::GameDbForced AutoTune::ForcedByGameDb(const GameDatabaseSchema::GameEntry* entry)
{
	GameDbForced forced;
	if (!entry)
		return forced;

	for (const auto& [hack, value] : entry->speedHacks)
	{
		if (hack == SpeedHack::IOPThread)
			forced.iop_thread = true;
		else if (hack == SpeedHack::EECycleRate)
			forced.ee_governor = true;
	}

	for (const auto& [id, value] : entry->gsHWFixes)
	{
		if (id == GameDatabaseSchema::GSHWFixId::HWDownloadMode)
			forced.download_mode = true;
		else if (id == GameDatabaseSchema::GSHWFixId::MinimumBlendingLevel)
			forced.min_blending = std::max(forced.min_blending, static_cast<int>(value));
	}

	return forced;
}

GSBackThreadInputs AutoTune::BackThreadInputs(bool hardware_renderer, RenderAPI live_api, GSHardwareDownloadMode download_mode)
{
	GSBackThreadInputs in;
	in.requested = true;
	in.hardware_renderer = hardware_renderer;
	in.vulkan = live_api == RenderAPI::Vulkan;
	in.download_mode = download_mode;
	return in;
}

bool AutoTune::GsBackThreadOffered(const GSBackThreadInputs& inputs, bool would_share_busy_core)
{
	return GSDecideBackThread(inputs).on && !would_share_busy_core;
}

bool AutoTune::IopThreadPlaceable(bool vu_thread, bool lockstep, IopThread::HostCpuInfo host, const IopThread::VmConditions& vm,
	bool gs_back_thread_running)
{
	// Lockstep is a testing mode that runs the two threads in turns; it is not something to switch on for speed.
	if (lockstep)
		return false;

	// The GS back thread is a busy thread that needs a core of its own, and ResolveMode does not know about it.
	// Taking one performance core and one core away from the host has the same effect as counting it.
	if (gs_back_thread_running)
	{
		if (host.performance_cores > 0)
			host.performance_cores--;
		if (host.total_cores > 0)
			host.total_cores--;
	}

	Pcsx2Config hypothetical;
	hypothetical.Speedhacks.iopThread = true;
	hypothetical.Speedhacks.iopThreadLockstep = false;
	hypothetical.Speedhacks.vuThread = vu_thread;
	return IopThread::ResolveMode(hypothetical, host, vm) == IopThreadMode::Threaded;
}

bool AutoTune::GovernorAvailableFrom(const EECycleRate::GovernorStatus& status, bool dynamic_setting)
{
	if (!status.eligible)
		return false;

	// The floor is two steps under the player's rate and never under the selector's own minimum.
	if (status.floor >= status.configured)
		return false;

	// On globally but kept out of this game by a pinned rate: a trial would not change the configuration.
	if (dynamic_setting && !status.enabled)
		return false;

	return true;
}

bool AutoTune::EeGovernorAvailable()
{
	return GovernorAvailableFrom(EECycleRate::GetGovernorStatus(), EmuConfig.Speedhacks.DynamicEECycleRate);
}

bool AutoTune::EeGovernorEnabled()
{
	// The resolved answer rather than the raw switch: a pinned rate can keep the governor out of a game whose
	// switch is on.
	return EECycleRate::GetGovernorStatus().enabled;
}

bool AutoTune::TrialWasMeasured(AutoTuneLever lever, uint32_t governor_transitions_at_start, uint32_t governor_transitions_now)
{
	if (lever != AutoTuneLever::EeGovernor)
		return true;
	return governor_transitions_now != governor_transitions_at_start;
}

// ================================================================================================
// The VM-bound half
// ================================================================================================

namespace
{
	std::mutex s_file_mutex;

	// The game's settings file for the running game, or empty when there is none to name.
	std::string RunningGameFilePath()
	{
		const std::string serial = VMManager::GetSerialForGameSettings();
		u32 crc = VMManager::GetDiscCRC();
		if (crc == 0)
			crc = VMManager::GetCurrentCRC();
		if (crc == 0 && !(ArcadeInput::IsActive() && !serial.empty()))
			return {};

		std::string path = VMManager::GetGameSettingsPath(serial, crc);
		// A game whose settings still live in the old CRC-only file keeps them there: a new file under the
		// serial would take over from it and drop what the player had set.
		if (!FileSystem::FileExists(path.c_str()) && !ArcadeInput::IsActive())
		{
			std::string legacy = VMManager::GetGameSettingsPath({}, crc);
			if (FileSystem::FileExists(legacy.c_str()))
				path = std::move(legacy);
		}
		return path;
	}

	std::string FilePathFor(const std::string& serial, u32 crc)
	{
		std::string path = VMManager::GetGameSettingsPath(serial, crc);
		if (!FileSystem::FileExists(path.c_str()))
		{
			std::string legacy = VMManager::GetGameSettingsPath({}, crc);
			if (crc != 0 && FileSystem::FileExists(legacy.c_str()))
				path = std::move(legacy);
		}
		return path;
	}

	class VmEnv final : public AutoTuneEnv
	{
	public:
		void ApplySettings() override
		{
			AutoTune::ApplyScope applying;
			VMManager::ApplySettings();
		}

		bool EditGameFile(const std::function<void(SettingsInterface&)>& edit) override
		{
			const std::string path = RunningGameFilePath();
			if (path.empty())
				return false;

			std::lock_guard lock(s_file_mutex);
			INISettingsInterface ini(path);
			ini.Load(); // a game with no file yet starts from an empty one
			edit(ini);
			return ini.Save();
		}

		void ReloadGameSettings() override
		{
			AutoTune::ApplyScope applying;
			VMManager::ReloadGameSettings();
		}

		std::unique_lock<std::mutex> LockSettings() override { return Host::GetSettingsLock(); }

		void ShowMessage(const std::string& text) override
		{
			Host::AddIconOSDMessage("AutoTune", ICON_FA_GAUGE_HIGH, text, Host::OSD_INFO_DURATION);
		}

		void Log(const std::string& text) override { Console.WriteLn("AutoTune: %s", text.c_str()); }

		void NotifyCommitted(const char* section, const char* key, const std::string& value) override
		{
			if (AutoTune::g_on_committed)
				AutoTune::g_on_committed(section, key, value);
		}
	};

	const char* ActionName(const AutoTuneAction& a)
	{
		using Kind = AutoTuneAction::Kind;
		switch (a.kind)
		{
			case Kind::None:
				return "";
			case Kind::StartTrial:
				return "start";
			case Kind::Keep:
				return "keep";
			case Kind::Revert:
				return a.measured ? "revert" : (a.retry ? "retry" : (a.inconclusive ? "inconclusive" : "cancel"));
			case Kind::ReportBottleneck:
				return "report";
			case Kind::Unclassified:
				return "unclassified";
			case Kind::Exhausted:
				return "exhausted";
		}
		return "";
	}

	class Runtime
	{
	public:
		Runtime()
			: m_applier(m_env)
		{
		}

		VmEnv m_env;
		AutoTuneApplier m_applier;
		AutoTuneController m_controller;
		AutoTuneSampler m_sampler;

		bool m_active = false;
		bool m_layer_installed = false;
		bool m_pending_scene = false;
		bool m_pending_settings = false;
		bool m_pending_resume = false;
		// EECycleRate::GetTransitionCount() when the governor's trial began, to tell a trial in which it never acted.
		uint32_t m_governor_transitions_at_trial = 0;
		AutoTune::GameDbForced m_forced;
		Common::Timer m_session_timer;
		std::unique_ptr<std::FILE, void (*)(std::FILE*)> m_trace{nullptr, [](std::FILE* f) { if (f) std::fclose(f); }};

		void Begin();
		void Deactivate(bool apply);
		void Shutdown();
		void Handle(const std::optional<AutoTuneAction>& action);
		AutoTuneLeverState Gather() const;
		void OnWindow(const AutoTuneWindow& window);
		bool LeverTookEffect(AutoTuneLever lever, double value) const;
		void OpenTrace();
		void WriteTrace(const AutoTuneWindow& w, const AutoTuneLeverState& s, const AutoTuneAction& a);
	};

	Runtime& Rt()
	{
		static Runtime runtime;
		return runtime;
	}

	void Runtime::Begin()
	{
		m_pending_scene = m_pending_settings = m_pending_resume = false;

		std::vector<AutoTuneRejection> persisted;
		if (const std::string path = RunningGameFilePath(); !path.empty() && FileSystem::FileExists(path.c_str()))
		{
			std::lock_guard lock(s_file_mutex);
			INISettingsInterface ini(path);
			if (ini.Load())
				persisted = AutoTune::ReadRejected(ini);
		}
		m_applier.Begin(std::move(persisted));

		if (!m_layer_installed)
		{
			auto lock = Host::GetSettingsLock();
			Host::Internal::SetAutoTuneSettingsLayer(m_applier.TrialLayer(), lock);
			m_layer_installed = true;
		}

		m_forced = AutoTune::ForcedByGameDb(GameDatabase::findGame(VMManager::GetDiscSerial()));
		m_controller.Reset();
		// The waits and the dropped frames come from the governor's sampler, which only keeps them when asked.
		// Asked before the baseline is read, so the first window is measured from a reading that was kept.
		EECycleRateSampler::RequestTally(true);
		m_sampler.ResetLive();
		m_session_timer.Reset();
		OpenTrace();
		m_active = true;

		m_env.Log(fmt::format("watching {} (CRC {:08X}); {} step(s) already known not to help", VMManager::GetDiscSerial(),
			VMManager::GetDiscCRC(), m_applier.Rejections().size()));
	}

	void Runtime::Deactivate(bool apply)
	{
		if (!m_active)
			return;
		m_applier.CancelTrial(apply);
		m_controller.Reset();
		m_trace.reset();
		EECycleRateSampler::RequestTally(false);
		m_active = false;
	}

	void Runtime::Shutdown()
	{
		m_applier.CancelTrial(false);
		m_applier.End();
		if (m_layer_installed)
		{
			auto lock = Host::GetSettingsLock();
			Host::Internal::SetAutoTuneSettingsLayer(nullptr, lock);
			m_layer_installed = false;
		}
		m_controller.Reset();
		m_trace.reset();
		EECycleRateSampler::RequestTally(false);
		m_active = false;
		m_pending_scene = m_pending_settings = m_pending_resume = false;
	}

	void Runtime::Handle(const std::optional<AutoTuneAction>& action)
	{
		if (action)
			m_applier.Execute(*action);
	}

	AutoTuneLeverState Runtime::Gather() const
	{
		AutoTuneLeverState s;

		const GSRendererType renderer = GSGetCurrentRenderer();
		s.hw_renderer = GSIsHardwareRenderer() && renderer != GSRendererType::Null;
		s.resolution = EmuConfig.GS.UpscaleMultiplier;

		s.gs_back_thread_on = EmuConfig.GS.BackThread;
		// Ask the device, not the configuration: after a renderer reopen the configured renderer reads Auto.
		const GSBackThreadInputs back = AutoTune::BackThreadInputs(s.hw_renderer, GSGetCurrentRenderAPI(), EmuConfig.GS.HWDownloadMode);
		// The placement is read off the topology, so it is only asked for when it matters: the thread is off and
		// the setting would be granted.
		s.gs_back_thread_allowed = GSDecideBackThread(back).on &&
		                           (s.gs_back_thread_on || AutoTune::GsBackThreadOffered(back, VMManager::Internal::GSBackThreadWouldShareBusyCore()));

		s.download_mode = static_cast<int>(EmuConfig.GS.HWDownloadMode);
		s.download_mode_forced = m_forced.download_mode;
		s.blending_level = static_cast<int>(EmuConfig.GS.AccurateBlendingUnit);
		s.blending_floor = m_forced.min_blending;

		s.iop_thread_on = EmuConfig.Speedhacks.iopThread;
		s.iop_thread_forced = m_forced.iop_thread;
		if (!s.iop_thread_on && !s.iop_thread_forced)
		{
			s.iop_thread_placeable = AutoTune::IopThreadPlaceable(EmuConfig.Speedhacks.vuThread, EmuConfig.Speedhacks.iopThreadLockstep,
				IopThread::CurrentHostInfo(), IopThread::CurrentVmConditions(), PerformanceMetrics::HasGSBackThread());
		}

		s.ee_governor_on = AutoTune::EeGovernorEnabled();
		s.ee_governor_eligible = AutoTune::EeGovernorAvailable();
		s.ee_governor_forced = m_forced.ee_governor;
		s.hardcore = Achievements::IsHardcoreModeActive();

		s.rejected = m_applier.Rejections();
		return s;
	}

	bool Runtime::LeverTookEffect(AutoTuneLever lever, double value) const
	{
		switch (lever)
		{
			case AutoTuneLever::IopThread:
				return EmuConfig.Speedhacks.iopThread == (value != 0.0);
			case AutoTuneLever::Resolution:
				return std::fabs(EmuConfig.GS.UpscaleMultiplier - static_cast<float>(value)) < 1e-3f;
			case AutoTuneLever::GsBackThread:
				return EmuConfig.GS.BackThread == (value != 0.0);
			case AutoTuneLever::DownloadMode:
				return static_cast<int>(EmuConfig.GS.HWDownloadMode) == static_cast<int>(std::lround(value));
			case AutoTuneLever::Blending:
				return static_cast<int>(EmuConfig.GS.AccurateBlendingUnit) == static_cast<int>(std::lround(value));
			case AutoTuneLever::EeGovernor:
			{
				// Asked for and not suspended. A pinned rate or a suspension (hardcore mode, a cycle skip) would
				// leave the governor off and the trial measuring nothing.
				const EECycleRate::GovernorStatus status = EECycleRate::GetGovernorStatus();
				return status.enabled == (value != 0.0) && (value == 0.0 || status.eligible);
			}
			default:
				return true;
		}
	}

	void Runtime::OpenTrace()
	{
		m_trace.reset();
		if (EmuConfig.AutoTuneTrace.empty())
			return;

		std::FILE* f = FileSystem::OpenCFile(EmuConfig.AutoTuneTrace.c_str(), "ab");
		if (!f)
		{
			Console.Warning("AutoTune: cannot open the trace file '%s'", EmuConfig.AutoTuneTrace.c_str());
			return;
		}
		m_trace.reset(f);

		std::fseek(f, 0, SEEK_END);
		if (std::ftell(f) == 0)
		{
			std::fputs("t,seq,usable,seconds,speed,late,ee,mtgs,gsb,mtvu,iop,gpu,gpu_timer,wait_gs,wait_vu1,phase,streak,"
					   "bottleneck,action,lever,value,gain,res,back_thread,download,blending,iop_thread,governor,wait_iop,ee_rate,ee_ms,gs_ms,gsb_ms,gpu_ms\n",
				f);
		}
		std::fprintf(f, "# session %s crc %08X\n", VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC());
		std::fflush(f);
	}

	void Runtime::WriteTrace(const AutoTuneWindow& w, const AutoTuneLeverState& s, const AutoTuneAction& a)
	{
		std::FILE* f = m_trace.get();
		if (!f)
			return;

		const auto opt = [](const std::optional<double>& v) { return v ? fmt::format("{:.3f}", *v) : std::string(); };
		const std::string line = fmt::format("{:.2f},{},{},{:.3f},{:.1f},{:.3f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{},{},{},{},{},{},{},{},{},{:.3f},{:g},{},{},{},{},{},{},{},{:.2f},{:.2f},{:.2f},{:.2f}\n",
			m_session_timer.GetTimeSeconds(), m_sampler.LastSequence(), w.usable ? 1 : 0, w.seconds, w.speed_pct, w.late_frame_ratio, w.ee_pct,
			w.mtgs_pct, w.gs_back_pct, w.mtvu_pct, w.iop_pct, w.gpu_pct, w.has_gpu_timer ? 1 : 0, opt(w.ee_wait_gs_ratio),
			opt(w.ee_wait_vu1_ratio), AutoTuneController::PhaseName(m_controller.GetPhase()), m_controller.BadStreak(),
			AutoTuneBottleneckName(m_controller.LastBottleneck()), ActionName(a),
			a.kind == AutoTuneAction::Kind::StartTrial || a.kind == AutoTuneAction::Kind::Keep || a.kind == AutoTuneAction::Kind::Revert ?
				AutoTuneLeverName(a.lever) :
				"",
			a.kind == AutoTuneAction::Kind::StartTrial || a.kind == AutoTuneAction::Kind::Keep || a.kind == AutoTuneAction::Kind::Revert ?
				fmt::format("{:g}", a.value) :
				std::string(),
			a.gain, s.resolution, s.gs_back_thread_on ? 1 : 0, s.download_mode, s.blending_level, s.iop_thread_on ? 1 : 0,
			s.ee_governor_on ? 1 : 0, opt(w.ee_wait_iop_ratio), static_cast<int>(EECycleRate::GetEffective()), w.ee_ms, w.mtgs_ms,
			w.gs_back_ms, w.gpu_ms);
		std::fputs(line.c_str(), f);
		std::fflush(f);
	}

	void Runtime::OnWindow(const AutoTuneWindow& w)
	{
		if (m_pending_scene)
		{
			Handle(m_controller.OnDisturbance(AutoTuneDisturbance::SceneChange));
			m_sampler.ResetLive();
		}
		else if (m_pending_settings)
		{
			Handle(m_controller.OnDisturbance(AutoTuneDisturbance::SettingsChange));
		}
		else if (m_pending_resume)
		{
			m_controller.OnDisturbance(AutoTuneDisturbance::Resumed);
		}
		m_pending_scene = m_pending_settings = m_pending_resume = false;

		const AutoTuneLeverState state = Gather();
		AutoTuneAction action = m_controller.OnWindow(w, state);

		// A governor that never lowered the rate during its trial was not tried. The controller has noted it for
		// this session, so it is not offered again straight away, but it is not written to the game's file as
		// "did not help": the next session may find the game overloaded when this one did not.
		if (action.kind == AutoTuneAction::Kind::Revert && action.measured &&
			!AutoTune::TrialWasMeasured(action.lever, m_governor_transitions_at_trial, EECycleRate::GetTransitionCount()))
		{
			m_env.Log("the EE governor never lowered the EE rate during its trial, so it was not measured; not counted against it");
			action.measured = false;
		}
		WriteTrace(w, state, action);

		m_applier.Execute(action);
		if (action.kind == AutoTuneAction::Kind::StartTrial && action.lever == AutoTuneLever::EeGovernor)
			m_governor_transitions_at_trial = EECycleRate::GetTransitionCount();

		// A trial whose value did not reach the configuration (the game database or a clamp overruled it) would
		// measure nothing. Call it a failed trial and take it back.
		if (action.kind == AutoTuneAction::Kind::StartTrial && !LeverTookEffect(action.lever, action.value))
		{
			m_env.Log(fmt::format("{} did not take effect; abandoning it", AutoTune::LeverLabel(action.lever, action.value)));
			m_controller.NoteApplyFailed();
			AutoTuneAction cancel = action;
			cancel.kind = AutoTuneAction::Kind::Revert;
			cancel.measured = false;
			m_applier.Execute(cancel);
		}
	}
} // namespace

// ================================================================================================
// VMManager's entry points
// ================================================================================================

void AutoTune::OnVMStarted()
{
	// Everything is set up on the first vsync that finds the VM running; nothing to do until then.
	Rt().m_active = false;
}

void AutoTune::OnVMShutdown()
{
	Rt().Shutdown();
}

void AutoTune::OnGameChanged()
{
	// Called before the new game's settings are applied. A trial of the old game must not reach them.
	Runtime& rt = Rt();
	rt.m_applier.CancelTrial(false);
	rt.m_applier.End();
	rt.m_active = false;
}

void AutoTune::OnSceneChanged()
{
	Rt().m_pending_scene = true;
}

void AutoTune::OnResumed()
{
	Rt().m_pending_resume = true;
}

void AutoTune::OnSettingsApplied()
{
	Runtime& rt = Rt();
	if (!IsApplyingSettings())
		rt.m_pending_settings = true;
}

void AutoTune::OnVSync()
{
	Runtime& rt = Rt();

	if (!EmuConfig.AutoTune || !VMManager::HasValidVM() || VMManager::GetState() != VMState::Running)
	{
		// Switched off, or the VM is not running: take back a trial that is in flight.
		if (rt.m_active && !EmuConfig.AutoTune && VMManager::HasValidVM())
			rt.Deactivate(true);
		return;
	}

	if (!rt.m_active)
	{
		if (GSDumpReplayer::IsReplayingDump())
			return;
		rt.Begin();
	}

	AutoTuneWindow window;
	if (!rt.m_sampler.PollLive(&window))
		return;

	// The steady-state checks that cost more than a flag read run once per window, not once per frame.
	if (GSDumpReplayer::IsReplayingDump() || IopThread::CurrentVmConditions().ps1_mode)
	{
		rt.Deactivate(true);
		return;
	}

	rt.OnWindow(window);
}

// ================================================================================================
// The per-game record
// ================================================================================================

AutoTune::ResetResult AutoTune::ResetForGame(const std::string& serial, u32 crc)
{
	ResetResult result;
	const std::string path = FilePathFor(serial, crc);

	{
		std::lock_guard lock(s_file_mutex);
		if (!FileSystem::FileExists(path.c_str()))
			return result;

		INISettingsInterface ini(path);
		if (!ini.Load())
			return result;

		result = Reset(ini);
		if (result.had_record)
			ini.Save();
	}

	if (result.had_record && VMManager::HasValidVM() && VMManager::GetDiscSerial() == serial && VMManager::GetDiscCRC() == crc)
	{
		// This game is running: its game layer holds the old file and its session has learned from it.
		Host::RunOnCPUThread([]() {
			Runtime& rt = Rt();
			rt.m_applier.CancelTrial(false);
			rt.m_active = false; // starts over at the next vsync, reading the rejected steps from the cleaned file
			AutoTune::ApplyScope applying;
			VMManager::ReloadGameSettings();
		});
	}

	return result;
}

std::vector<std::string> AutoTune::DescribeForGame(const std::string& serial, u32 crc)
{
	const std::string path = FilePathFor(serial, crc);
	std::lock_guard lock(s_file_mutex);
	if (!FileSystem::FileExists(path.c_str()))
		return {};

	INISettingsInterface ini(path);
	if (!ini.Load())
		return {};
	return Describe(ini);
}
