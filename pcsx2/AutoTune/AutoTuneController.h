// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Auto-tune's decision logic, with nothing else in it: no clock, no globals, no logging, no I/O.
//
// The caller feeds it one AutoTuneWindow per half second of play, together with a description of the
// settings as they are right now (AutoTuneLeverState), and gets back at most one AutoTuneAction per call.
// Time exists only as the `seconds` each window carries. That keeps it testable by replaying scripted
// windows, and keeps every threshold in AutoTunePolicy where a device round can change it.
//
// What it does, in order:
//   warm-up   the first policy.warmup_seconds of usable play after a boot or a state load are ignored.
//   watching  counts consecutive usable windows that are bad (speed under policy.bad_speed_pct, or late
//             frames). trigger_windows of them in a row start a diagnosis.
//   classify  from the averages of those windows, which part of the emulator is the bottleneck.
//   trial     applies the next countermeasure for that bottleneck (StartTrial), waits out the settle time,
//             measures measure_windows windows, and either keeps it (Keep) or undoes it (Revert) and
//             remembers that it did not help.
//   repeat    until the frames stop being late, or no countermeasure is left (Exhausted).
//
// Only usable windows count. An unusable window (paused, fast-forward, frame advance, a GS dump) neither
// advances a clock nor breaks a streak: it is as if it had not happened.
//
// The controller never undoes a kept change by itself. It only lowers settings, one step per trial.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

enum class AutoTuneLever : uint8_t
{
	IopThread, // run the IOP on its own thread
	EeGovernor, // the live EE cycle-rate governor (DynamicEECycleRate)
	Resolution, // upscale multiplier, one notch down
	GsBackThread, // GS multi-threading (a back thread draws while the front thread parses)
	DownloadMode, // hardware download mode, towards fewer readbacks
	Blending, // blending accuracy, one level down

	Count
};

enum class AutoTuneBottleneck : uint8_t
{
	None, // nothing identifiable
	Ee, // the EE thread
	GsCpu, // the MTGS thread or the GS back thread
	Gpu, // the GPU, or the EE blocked on a GS that is not CPU-bound
	Vu1, // the MTVU thread (report only)
	Iop, // the IOP thread (report only)

	Count
};

const char* AutoTuneLeverName(AutoTuneLever lever);
const char* AutoTuneBottleneckName(AutoTuneBottleneck bottleneck);
std::optional<AutoTuneLever> AutoTuneLeverFromName(const char* name);

/// Hardware download mode values as they are stored in the settings file (GSHardwareDownloadMode).
/// AutoTuneApplier.cpp static_asserts that these match the enum.
namespace AutoTuneDownloadMode
{
	inline constexpr int Accurate = 0;
	inline constexpr int AccurateForceFull = 1;
	inline constexpr int NoReadbacks = 2;
	inline constexpr int Unsynchronized = 3;
	inline constexpr int Disabled = 4;
	inline constexpr int Asynchronous = 5;
} // namespace AutoTuneDownloadMode

/// Every number that decides anything. The defaults are the first guess; device rounds tune them.
struct AutoTunePolicy
{
	/// A window is bad when its speed is under this (the OSD turns red at the same value)...
	double bad_speed_pct = 95.0;
	/// ...or when at least this share of its frames were late. A late frame is one that took two frame
	/// times or more, and costs the speed the same way, so 5% of a half-second window is about the
	/// 95% speed line.
	double late_frame_ratio_bad = 0.05;

	/// A thread at or above this share of one core is saturated.
	double saturation_pct = 90.0;
	/// GPU usage at or above this is a busy GPU (only where there is a GPU timer).
	double gpu_high_pct = 90.0;
	/// The EE waiting on the GS for more than this share of its time means the EE is blocked on the GS.
	/// Also the stand-in for "GPU-bound" where there is no GPU timer.
	double ee_blocked_on_gs_ratio = 0.25;
	/// The same for the IOP thread: an EE that spends more than this share of its time waiting for the IOP
	/// thread to catch up or to stop for a hold reads as saturated (it is spinning), but is bound by the IOP.
	double ee_blocked_on_iop_ratio = 0.25;

	double warmup_seconds = 10.0;
	/// Windows to ignore after a setting changes, before measuring.
	double settle_seconds = 3.0;
	/// The same for the levers that reset the recompilers or reopen the renderer, whose hitch is longer.
	double settle_heavy_seconds = 6.0;

	uint32_t trigger_windows = 4;
	uint32_t measure_windows = 4;

	/// A trial is kept when the mean speed rose by at least this fraction of the mean before it...
	double gain_fraction = 0.05;
	/// ...or when it went from bad to this speed with no late frames.
	double reached_speed_pct = 99.0;

	/// After a diagnosis that found nothing to act on, wait this long before saying so again.
	double unclassified_cooldown_seconds = 30.0;

	/// The resolutions a trial steps through, ascending. A step goes to the largest notch below the
	/// current multiplier; there is no notch under the first entry. These are the whole multipliers
	/// the settings screens offer (the quarter steps between them are skipped on purpose: a trial costs
	/// seconds, and a quarter notch rarely moves the speed enough to keep).
	std::vector<double> resolution_notches = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
};

/// What the sampler measured over one window.
struct AutoTuneWindow
{
	/// False while the window cannot say anything about steady play (paused, not at the nominal limiter,
	/// frame advance, a GS dump).
	bool usable = false;
	double seconds = 0.5;
	double speed_pct = 100.0; // percent of the nominal frame rate
	double late_frame_ratio = 0.0; // late frames / frames at the limiter in this window
	double ee_pct = 0.0; // the EE thread, percent of one core
	double mtgs_pct = 0.0;
	double gs_back_pct = 0.0; // 0 when there is no back thread
	double mtvu_pct = 0.0; // 0 when MTVU is off
	double iop_pct = 0.0; // 0 when the IOP has no thread of its own
	double gpu_pct = 0.0;
	bool has_gpu_timer = false;
	/// The share of its time the EE spent waiting for the GS, for VU1, and for the IOP thread. Unknown when
	/// they could not be measured over the window (see AutoTuneSampler); the controller works without them.
	std::optional<double> ee_wait_gs_ratio;
	std::optional<double> ee_wait_vu1_ratio;
	std::optional<double> ee_wait_iop_ratio;
};

/// A trial that did not help. `value` is what was tried (lever-specific, see AutoTuneLeverState).
struct AutoTuneRejection
{
	AutoTuneLever lever;
	double value;
};

/// The settings as they are now, plus what is allowed. Passed on every call; the controller keeps none of it.
struct AutoTuneLeverState
{
	/// Hardware renderer. Resolution, download mode and blending do nothing for the software renderer.
	bool hw_renderer = true;

	double resolution = 1.0; // the upscale multiplier in effect

	bool gs_back_thread_on = false;
	/// The GS back thread would actually run if asked: Vulkan for the hardware renderer, and not
	/// Unsynchronized downloads.
	bool gs_back_thread_allowed = false;

	int download_mode = AutoTuneDownloadMode::Accurate; // AutoTuneDownloadMode::*
	int blending_level = 0; // AccBlendLevel as an integer, 0 = Minimum

	bool iop_thread_on = false;
	/// IopThread::ResolveMode would give the thread a place on this host (with the GS back thread counted as a
	/// busy core).
	bool iop_thread_placeable = false;

	bool ee_governor_on = false;
	bool ee_governor_eligible = false;

	/// Settings the game database fixes for this game. A forced lever is never touched, and the blending
	/// level never goes under the database's minimum.
	bool iop_thread_forced = false;
	bool ee_governor_forced = false;
	bool download_mode_forced = false;
	int blending_floor = 0;

	/// Achievements hardcore mode: no EE governor.
	bool hardcore = false;

	/// Trials that did not help in an earlier session (the per-game file remembers them).
	std::vector<AutoTuneRejection> rejected;
};

struct AutoTuneAction
{
	enum class Kind : uint8_t
	{
		None,
		StartTrial, // apply `lever` = `value` and measure
		Keep, // the trial helped: make `lever` = `value` permanent
		Revert, // undo the trial of `lever` = `value`
		ReportBottleneck, // Vu1 or Iop: nothing to try, tell the user
		Unclassified, // slow, but no thread stands out: nothing to try
		Exhausted, // every lever for `bottleneck` is used up
	};

	Kind kind = Kind::None;
	AutoTuneLever lever = AutoTuneLever::Count;
	double value = 0.0;
	AutoTuneBottleneck bottleneck = AutoTuneBottleneck::None;
	/// Keep and Revert: the mean speed before the trial and during it, and the relative change.
	double speed_before = 0.0;
	double speed_after = 0.0;
	double gain = 0.0;
	/// Revert only: false when the trial was cut short (the scene changed, the user changed a setting)
	/// instead of being measured. Only a measured failure is remembered as "did not help".
	bool measured = false;
};

enum class AutoTuneDisturbance : uint8_t
{
	SceneChange, // boot, state load, reset, a new game: the whole warm-up again
	SettingsChange, // somebody else changed a setting: the numbers around it mean nothing
	Resumed, // back from a pause: the bad-window streak is cut; a trial in flight carries on
};

class AutoTuneController
{
public:
	enum class Phase : uint8_t
	{
		WarmUp,
		Watching,
		SettlingTrial, // a trial is applied, waiting out the hitch
		Measuring,
		SettlingAfter, // a keep or revert was applied, waiting out the hitch
	};

	explicit AutoTuneController(const AutoTunePolicy& policy = AutoTunePolicy());

	const AutoTunePolicy& GetPolicy() const { return m_policy; }

	/// One window. At most one action comes back; the caller carries it out before the next call.
	AutoTuneAction OnWindow(const AutoTuneWindow& window, const AutoTuneLeverState& state);

	/// Something outside changed the situation. Returns a Revert when a trial was running.
	std::optional<AutoTuneAction> OnDisturbance(AutoTuneDisturbance kind);

	/// The caller could not apply the trial it was told to start. The lever counts as tried and failed.
	void NoteApplyFailed();

	/// A new session: warm-up again, and forget what this session learned (rejections, exhaustion, reports).
	void Reset();

	Phase GetPhase() const { return m_phase; }
	static const char* PhaseName(Phase phase);
	bool TrialActive() const { return m_phase == Phase::SettlingTrial || m_phase == Phase::Measuring; }
	AutoTuneBottleneck LastBottleneck() const { return m_last_bottleneck; }
	uint32_t BadStreak() const { return m_streak; }

	/// Which part of the emulator the averages of `w` point at. Exposed for the trace and the tests.
	AutoTuneBottleneck Classify(const AutoTuneWindow& mean) const;

private:
	struct Trial
	{
		AutoTuneLever lever = AutoTuneLever::Count;
		double value = 0.0;
		double speed_before = 0.0;
		double late_before = 0.0;
		double speed_sum = 0.0;
		double late_sum = 0.0;
		uint32_t collected = 0;
	};

	bool IsBad(const AutoTuneWindow& w) const;
	AutoTuneAction Diagnose(const AutoTuneLeverState& state);
	std::optional<double> TargetFor(AutoTuneLever lever, const AutoTuneLeverState& state) const;
	bool IsRejected(AutoTuneLever lever, double value, const AutoTuneLeverState& state) const;
	double SettleSecondsFor(AutoTuneLever lever) const;
	AutoTuneAction FinishTrial();
	void EnterSettle(Phase phase, double seconds);

	AutoTunePolicy m_policy;
	Phase m_phase = Phase::WarmUp;
	double m_phase_seconds = 0.0; // warm-up elapsed, or settle remaining
	uint32_t m_streak = 0;
	std::vector<AutoTuneWindow> m_streak_windows;
	Trial m_trial;
	std::vector<AutoTuneRejection> m_rejected; // this session
	bool m_exhausted[static_cast<size_t>(AutoTuneBottleneck::Count)] = {};
	bool m_reported[static_cast<size_t>(AutoTuneBottleneck::Count)] = {};
	double m_unclassified_cooldown = 0.0;
	AutoTuneBottleneck m_last_bottleneck = AutoTuneBottleneck::None;
};
