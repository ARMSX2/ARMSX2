// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Carries out what AutoTuneController asks for, on the CPU thread, and ties the pieces to the VM.
//
// Two layers:
//
//   AutoTuneApplier  does the actions: puts a trial value in the in-memory trial layer and applies the
//                    settings, moves a kept value into the game's settings file, takes a trial back, says
//                    so on screen. It reaches the VM only through AutoTuneEnv, so a test can run it
//                    against a fake.
//   the AutoTune::   functions below are what VMManager calls. They own the one real applier, poll the
//                    sampler once per frame, read the lever state off the running VM, run the controller
//                    and write the trace.
//
// A trial lives in a MemorySettingsInterface installed as LAYER_AUTOTUNE, above the game's file. Settings
// are applied with VMManager::ApplySettings(), which rebuilds EmuConfig from the layers, so the trial
// survives it. A kept value goes into the game's own settings file and is removed from the trial layer.

#pragma once

#include "AutoTune/AutoTuneController.h"
#include "AutoTune/AutoTuneSampler.h"
#include "AutoTune/AutoTuneStore.h"
#include "EECycleRate.h"
#include "IopThread/IopThreadMode.h"

#include "common/MemorySettingsInterface.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameDatabaseSchema
{
	struct GameEntry;
}

enum class GSHardwareDownloadMode : u8;
enum class RenderAPI;
struct GSBackThreadInputs;

/// Everything the applier needs from the VM.
class AutoTuneEnv
{
public:
	virtual ~AutoTuneEnv() = default;

	/// Rebuilds the configuration from the settings layers (VMManager::ApplySettings).
	virtual void ApplySettings() = 0;

	/// Loads the game's settings file, lets `edit` change it, and saves it. False when the game has no
	/// settings file name (no CRC) or the file could not be saved; `edit` may then not have run.
	virtual bool EditGameFile(const std::function<void(SettingsInterface&)>& edit) = 0;

	/// Re-reads the game's file into the game layer and applies the settings.
	virtual void ReloadGameSettings() = 0;

	/// Held while the trial layer is written, if it is installed where other threads read it.
	virtual std::unique_lock<std::mutex> LockSettings() { return {}; }

	virtual void ShowMessage(const std::string& text) = 0;
	virtual void Log(const std::string& text) = 0;
	virtual void NotifyCommitted(const char* section, const char* key, const std::string& value) = 0;
};

class AutoTuneApplier
{
public:
	explicit AutoTuneApplier(AutoTuneEnv& env);

	/// The in-memory layer a trial is written to. Install it as LAYER_AUTOTUNE.
	SettingsInterface* TrialLayer() { return &m_trial_layer; }

	/// A new game or session: drops any trial (without applying) and takes the steps earlier sessions
	/// found useless, which the controller is told about through AutoTuneLeverState::rejected.
	void Begin(std::vector<AutoTuneRejection> persisted_rejections);

	/// The session is over: drops any trial without applying anything.
	void End();

	void Execute(const AutoTuneAction& action);

	/// Takes back the trial in flight, if any, without remembering it as a failure. With `apply` the settings
	/// are applied again so the VM stops using it; without, the caller is about to apply them anyway.
	void CancelTrial(bool apply);

	bool TrialActive() const { return m_trial_active; }
	const std::vector<AutoTuneRejection>& Rejections() const { return m_rejected; }

private:
	void StartTrial(const AutoTuneAction& action);
	void Keep(const AutoTuneAction& action);
	void Revert(const AutoTuneAction& action);
	void RestoreTrialKey();

	AutoTuneEnv& m_env;
	MemorySettingsInterface m_trial_layer;
	std::vector<AutoTuneRejection> m_rejected;

	bool m_trial_active = false;
	AutoTuneLever m_trial_lever = AutoTuneLever::Count;
	double m_trial_value = 0.0;
	// What the trial layer held under the trial's key before the trial, so a revert puts it back.
	std::optional<std::string> m_trial_previous;
};

namespace AutoTune
{
	/// Set by a host that keeps its own copy of the per-game settings; null (nothing to tell) otherwise. Auto-tune
	/// calls it when it has moved one setting into the game's own settings file (section and key as in the file,
	/// value as text), on the CPU thread. The Android app regenerates that file from its own store and drops what
	/// it does not know, so it writes the key back to its store here. Set once at start-up, before any VM exists.
	extern void (*g_on_committed)(const char* section, const char* key, std::string_view value);

	/// True while auto-tune is applying settings itself (a trial, an undo, a keep, a reset). Each apply would
	/// show the game database's hints (a recommended blending level, AA1) and the unsafe settings warning
	/// again, and a convergence is several applies in a row, so those stay quiet. A settings change by the
	/// player still shows them. CPU thread.
	bool IsApplyingSettings();

	/// Marks the settings applies made while it is alive as auto-tune's own. Nests.
	class ApplyScope
	{
	public:
		ApplyScope();
		~ApplyScope();
		ApplyScope(const ApplyScope&) = delete;
		ApplyScope& operator=(const ApplyScope&) = delete;
	};

	/// Settings the game database fixes for this game, in the terms the controller uses.
	struct GameDbForced
	{
		bool iop_thread = false;
		bool ee_governor = false; // any EE cycle rate entry: the governor moves the cycle rate
		bool download_mode = false;
		int min_blending = 0;
	};
	GameDbForced ForcedByGameDb(const GameDatabaseSchema::GameEntry* entry);

	/// Whether the GS back thread is worth offering as a lever: the setting would be granted for this renderer
	/// and download mode (GSDecideBackThread), and the thread would not have to share a processor with another busy
	/// emu thread (`would_share_busy_core`, VMManager::Internal::GSBackThreadWouldShareBusyCore). On a host with
	/// four performance cores and the IOP thread on, the back thread lands on one of them and costs speed.
	bool GsBackThreadOffered(const GSBackThreadInputs& inputs, bool would_share_busy_core);

	/// What the setting would be granted for: the renderer kind, the API of the device that is open (not the
	/// configured renderer, which reads Auto after a renderer reopen) and the download mode.
	GSBackThreadInputs BackThreadInputs(bool hardware_renderer, RenderAPI live_api, GSHardwareDownloadMode download_mode);

	/// Whether IopThread::ResolveMode would give an IOP thread a place on this host. The GS back thread, when it
	/// is running, takes one of the performance cores and one of the cores overall, so it is counted as busy:
	/// an IOP thread on a host that has room for it only without the back thread would not be placed.
	bool IopThreadPlaceable(bool vu_thread, bool lockstep, IopThread::HostCpuInfo host, const IopThread::VmConditions& vm,
		bool gs_back_thread_running);

	/// The live EE cycle-rate governor as a lever. Both read the governor's own resolver (EECycleRate::GetGovernorStatus),
	/// so auto-tune and the governor cannot disagree about whether it runs. CPU thread.
	///
	/// Available: the governor would run in this game if asked, and a trial of it would change something. That
	/// needs nothing suspending it (hardcore mode, a cycle skip, a limiter off normal speed, host-vsync
	/// pacing) and room to lower the player's rate (the floor is under it). It also excludes a global switch
	/// that is already on but held off by a pinned rate: the trial would write the same "on", leave the
	/// configuration unchanged, and the sampler would never be told to start.
	/// Enabled: the governor is asked for in this game and nothing pins the rate.
	bool GovernorAvailableFrom(const EECycleRate::GovernorStatus& status, bool dynamic_setting);
	bool EeGovernorAvailable();
	bool EeGovernorEnabled();

	/// Whether a finished trial says anything about its lever. Only the governor can fail to be tried: it acts
	/// by lowering the EE rate, and if it never did during the trial the game was measured with a governor that
	/// had not touched it (the game was not overloaded, or had not yet warmed up). That is not "did not help",
	/// which the per-game file remembers for good; a later session may find the game overloaded.
	bool TrialWasMeasured(AutoTuneLever lever, uint32_t governor_transitions_at_start, uint32_t governor_transitions_now);

	// What VMManager calls, on the CPU thread.
	void OnVMStarted();
	/// The VM is shutting down: the trial layer is emptied and removed.
	void OnVMShutdown();
	void OnGameChanged();
	/// A savestate was loaded or the VM was reset: the scene is not the one being measured.
	void OnSceneChanged();
	void OnResumed();
	/// VMManager::ApplySettings() finished. Applies auto-tune did itself are told apart from the rest.
	void OnSettingsApplied();
	/// Once per frame, at vsync start.
	void OnVSync();

	// The per-game record: any thread, no VM needed.

	/// Puts back what auto-tune changed in this game's settings file (except keys the player has changed since)
	/// and forgets what it learned. When the game is the one running, the change is applied on the CPU thread.
	ResetResult ResetForGame(const std::string& serial, u32 crc);

	std::vector<std::string> DescribeForGame(const std::string& serial, u32 crc);
} // namespace AutoTune
