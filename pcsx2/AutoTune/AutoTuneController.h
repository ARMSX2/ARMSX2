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
//   baseline  takes baseline_extra_windows more windows right before the trial, so the "before" is
//             measured now and over enough windows to tell a steady figure from a hitch.
//   trial     applies the next countermeasure for that bottleneck (StartTrial), waits out the settle time,
//             measures measure_windows windows, and gives one of these verdicts: keep it (Keep), undo it
//             and remember that it did not help (Revert, measured), or undo it and remember nothing
//             (Revert, inconclusive) when the difference is within the noise. An inconclusive measurement
//             is first extended by a few windows. A speed that clearly fell is a measured rejection at the
//             first look. A rejection that the game changing also explains (see scene_shift_fraction) is
//             neither: it is undone, nothing is written down, and the step comes round again with a fresh
//             baseline (Revert, retry). The EE governor's trial settles and measures for longer,
//             in step with the governor's own clocks (policy.governor_*).
//   repeat    until the frames stop being late, or no countermeasure is left (Exhausted).
//
// A trial is judged on the medians of its windows, never the means: one hitch window (a shader compile,
// a state save) moves a mean of four windows by several percent and a median by nothing. Only the speed
// earns a keep (a clear gain, or full speed reached). The cost of the thing the lever targets (GPU ms per
// frame, GS thread ms per frame, or EE ms per frame; see AutoTuneController::CostOf) is the second figure,
// and it can only hold a verdict back: a cost that fell with a flat speed is looked at for longer before the
// trial is called a failure. A cost falls whenever work is split over more threads, whether or not anything
// downstream is faster, so it is not a reason to keep a setting or burn a core.
//
// Only usable windows count. An unusable window (paused, fast-forward, frame advance, a GS dump) neither
// advances a clock nor breaks a streak: it is as if it had not happened.
//
// The controller never undoes a kept change by itself. It only lowers settings, one step per trial.

#pragma once

#include "EECycleRateController.h"

#include <algorithm>
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
	/// Time to ignore after a setting changes, before measuring. A resolution, blending or download mode change
	/// shows its new figure in the first full window, so only the partial window is skipped. A stray hitch window
	/// that outlasts this is the median's and the extension's to absorb.
	double settle_seconds = 1.5;
	/// The same for the levers that reset the recompilers or reopen the renderer, whose hitch is longer.
	double settle_heavy_seconds = 6.0;

	uint32_t trigger_windows = 4;
	uint32_t measure_windows = 4;

	/// The EE governor keeps its clocks in game time (warm-up, overload windows, cooldown), which take
	/// proportionally longer on the wall for a slow game. A trial of it settles for this much game time spread
	/// over the wall at the game's speed, on top of the usual settle, so it is measured after the governor has
	/// taken its steps; measuring earlier would reject a governor that had not yet acted.
	/// Derived from the governor's own policy so the two cannot drift apart.
	double governor_settle_game_seconds = GovernorSettleGameSeconds(EECycleRatePolicy());
	/// The slowest speed assumed when spreading it over the wall: a game at 5% does not get 130 seconds.
	double governor_min_speed_pct = 20.0;
	/// The governor steps down and back up on its own once it has headroom; a longer look averages over a cycle
	/// of that instead of landing in one half of it.
	uint32_t governor_measure_windows = 8;

	/// Game time from the switch to the governor's last possible step down: the warm-up, then for each step
	/// the windows of overload it needs and (between steps) the cooldown, then a window of slack because the
	/// governor's clocks advance in whole windows. With the default policy: 2 + 2 x 1 + 1 x 2 + 0.5 = 6.5 s.
	static double GovernorSettleGameSeconds(const EECycleRatePolicy& governor)
	{
		const double steps = std::max<double>(governor.max_steps_down, 1.0);
		const double dwell = static_cast<double>(governor.downshift_dwell) * governor.window_seconds;
		return governor.warmup_seconds + steps * dwell + (steps - 1.0) * governor.cooldown_seconds + governor.window_seconds;
	}

	/// A trial is kept when the median of its speed improved by at least this fraction of the median before it,
	/// and by more than the noise (below)...
	double gain_fraction = 0.05;
	/// ...or when it went from bad to this speed with no late frames. The cost the lever targets never earns a
	/// keep: a lever that lowers it with the speed flat did not help, because something else is the limit and
	/// the game is no faster. It does hold a flat speed back from an early rejection (see extend_windows).
	double reached_speed_pct = 99.0;

	/// The noise a gain has to clear, as this many standard errors of the difference between the two
	/// medians (each estimated from the spread of its own windows, robustly). A gain that reaches
	/// gain_fraction but not the noise is inconclusive, not a keep.
	double noise_z = 2.0;
	/// The standard error is never taken to be smaller than this fraction of the baseline: the windows of
	/// one scene are not independent draws (the scene moves, the clocks drift), and a run of steady windows
	/// would otherwise claim a precision it does not have. With noise_z that is a 3% floor on the gain.
	double noise_floor_fraction = 0.015;

	/// Windows taken right before a trial, added to the ones that triggered the diagnosis, so the baseline
	/// is measured now and not borrowed from an earlier trial or from before a hitch.
	uint32_t baseline_extra_windows = 4;

	/// A measurement that is neither a clear gain nor clearly nothing is extended by this many windows, up
	/// to max_extensions times, and then undone without being remembered as "did not help". A flat speed
	/// with a cost that clearly fell is extended the same way, and if the speed is still flat afterwards it
	/// is remembered as "did not help".
	uint32_t extend_windows = 4;
	uint32_t max_extensions = 2;

	/// Windows with a figure for the targeted cost needed on both sides before the cost is used to judge.
	uint32_t min_cost_windows = 3;

	/// A trial is only judged against its baseline if the game stayed the same between them. Two tests say it
	/// did not. The first and second half of the trial's windows differ in speed by more than the noise (only
	/// from this many windows on, since halves of two windows prove nothing; the EE governor is exempt, it steps
	/// down and back up by design). Or the cost of a resource the lever does not act on moved by more than
	/// scene_shift_fraction of its baseline: the EE time per frame for resolution and blending; the GS and GPU
	/// time for the IOP thread and the governor; the GPU time for the GS back thread and the download mode (which
	/// act on the GS threads and on how long the EE waits for them, so the EE time moves with them). A rejection
	/// that either test explains is not written down: the trial is undone and measured again from a fresh
	/// baseline, at most max_extensions times for one step.
	uint32_t scene_halves_min_windows = 8;
	double scene_shift_fraction = 0.10;

	/// Below this speed the game can be so far from full speed that one notch of resolution cannot close the gap
	/// (a notch removes at most (r / (r - 1))^2 of the pixels: 1.3x at 8x, 1.7x at 4x), and the levers whose
	/// effect is not bounded that way are tried before resolution: blending accuracy and the download mode.
	/// 0 turns the reordering off.
	///
	/// It applies only where both of these hold. The blending is above Basic: the one step from Basic to Minimum
	/// is not that kind of lever. And one notch cannot plausibly reach full speed: speed x (r / next notch)^2 is
	/// under 100, where r is the current multiplier and the next notch is the one the resolution lever would
	/// step to (at 1x there is no notch).
	double cheap_levers_first_below_pct = 50.0;

	/// After a diagnosis that found nothing to act on, wait this long before saying so again.
	double unclassified_cooldown_seconds = 30.0;

	/// The resolutions a trial steps through, ascending. A step goes to the largest notch below the
	/// current multiplier; there is no notch under the first entry. These are the whole multipliers
	/// the settings screens offer (the quarter steps between them are skipped on purpose: a trial costs
	/// seconds, and a quarter notch rarely moves the speed enough to keep).
	std::vector<double> resolution_notches = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
};

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
	/// What each thread costs per frame, in milliseconds (CPU time per frame; GPU time per frame): the currency
	/// a trial is judged in. 0 = not measured, or no such thread.
	double ee_ms = 0.0;
	double mtgs_ms = 0.0;
	double gs_back_ms = 0.0;
	double gpu_ms = 0.0;
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
	/// The GS back thread would actually run if asked (Vulkan for the hardware renderer, and not Unsynchronized
	/// downloads) and is worth asking for: it would not have to share a processor with another busy thread.
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
	/// StartTrial, Keep and Revert: the median speed before the trial and during it, and the relative change.
	/// `gain` is always the speed's: it is what the player is told and what the game's file records as the
	/// lever's gain in speed. It is under zero only for a Keep earned by the full-speed rule on a game that was
	/// at full speed with late frames. The cost has its own figures below.
	double speed_before = 0.0;
	double speed_after = 0.0;
	double gain = 0.0;
	/// The median cost the lever targets before and during the trial (ms per frame), and its relative
	/// improvement (positive is cheaper). 0 when the cost could not be measured on both sides.
	double cost_before = 0.0;
	double cost_after = 0.0;
	double cost_gain = 0.0;
	/// Revert only: false when the trial was cut short (the scene changed, the user changed a setting)
	/// or was inconclusive, instead of being measured to a clear answer. Only a measured failure is
	/// remembered as "did not help"; the controller still does not offer the same step again this session.
	bool measured = false;
	/// Revert only: the measurement stayed within the noise even after being extended.
	bool inconclusive = false;
	/// Revert only: the game changed while the lever was tried (a rejection that is also explained by the scene).
	/// The step is not written off and not held back for the session; it comes round again with a fresh baseline.
	bool retry = false;
};

enum class AutoTuneDisturbance : uint8_t
{
	SceneChange, // boot, state load, reset, a new game: the whole warm-up again
	SettingsChange, // somebody else changed a setting: the numbers around it mean nothing
	Resumed, // back from a pause: the bad-window streak is cut; a trial in flight carries on
};

/// The centre of some windows' figures and how well it is known: the median, and the standard error of the
/// median estimated from the median absolute deviation (1.4826 * MAD is a standard deviation for a normal
/// spread, and the median of n has 1.2533 times the error of the mean). Both ignore a hitch window.
struct AutoTuneStat
{
	double centre = 0.0;
	double error = 0.0;
	size_t n = 0;
};
AutoTuneStat AutoTuneSummarize(std::vector<double> values);

/// How much a figure changed between two sets of windows, and whether that is a clear gain, clearly nothing, or
/// neither. `gain` is the relative improvement of the median (positive is better); `noise` is what it has to
/// beat. A gain is clear when it reaches policy.gain_fraction and exceeds the noise. It is clearly nothing
/// when even the gain plus the noise stays under policy.gain_fraction. A loss is clear (`worse`) when it is as
/// large as a clear gain would be. Anything between is inconclusive.
struct AutoTuneGain
{
	double gain = 0.0;
	double noise = 0.0;
	bool clear = false;
	bool none = false;
	/// The mirror of `clear`: the figure got worse by at least policy.gain_fraction and by more than the noise.
	bool worse = false;
};
AutoTuneGain AutoTuneJudgeGain(const AutoTuneStat& before, const AutoTuneStat& after, bool higher_is_better, const AutoTunePolicy& policy);

class AutoTuneController
{
public:
	enum class Phase : uint8_t
	{
		WarmUp,
		Watching,
		Baselining, // a lever is chosen; taking the windows its trial is compared with
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
		AutoTuneBottleneck bottleneck = AutoTuneBottleneck::None;
		AutoTuneStat speed_before;
		AutoTuneStat cost_before;
		double late_before = 0.0;
		// The costs of the resources the lever does not act on: the baseline for the check that the scene
		// stayed the same (see AutoTunePolicy::scene_shift_fraction).
		AutoTuneStat ee_before;
		AutoTuneStat gs_before;
		AutoTuneStat gpu_before;
		std::vector<double> speeds;
		std::vector<double> costs;
		std::vector<double> ee_ms;
		std::vector<double> gs_ms;
		std::vector<double> gpu_ms;
		double late_sum = 0.0;
		uint32_t collected = 0;
		uint32_t windows = 0; // how many to collect before judging
		uint32_t extensions = 0;
	};

	// A lever chosen by a diagnosis, waiting for its baseline.
	struct Pending
	{
		AutoTuneLever lever = AutoTuneLever::Count;
		double value = 0.0;
		AutoTuneBottleneck bottleneck = AutoTuneBottleneck::None;
		std::vector<AutoTuneWindow> baseline;
		uint32_t extra_wanted = 0;
	};

	bool IsBad(const AutoTuneWindow& w) const;
	AutoTuneAction Diagnose(const AutoTuneLeverState& state);
	/// The value a trial of `lever` would try now, or nullopt when there is none. `bottleneck` is what the trial
	/// is for: the step from Basic blending to Minimum is only offered against the GS on the CPU.
	std::optional<double> TargetFor(AutoTuneLever lever, const AutoTuneLeverState& state, AutoTuneBottleneck bottleneck) const;
	/// Whether blending accuracy and the download mode are worth trying before the resolution at this speed.
	bool CheapLeversHaveSomethingToGive(double speed_pct, const AutoTuneLeverState& state) const;
	bool IsRejected(AutoTuneLever lever, double value, const AutoTuneLeverState& state) const;
	/// The settle after a change is applied or undone: the hitch of whatever the change resets.
	double SettleSecondsFor(AutoTuneLever lever) const;
	/// The settle before a trial of `lever` is measured, for a game that was running at `speed_pct`.
	double TrialSettleSeconds(AutoTuneLever lever, double speed_pct) const;
	uint32_t MeasureWindowsFor(AutoTuneLever lever) const;
	/// What a trial of a lever against `bottleneck` is judged on, in ms per frame; nullopt if the window has no
	/// figure for it. EE: the EE thread. GPU with a GPU timer: the GPU. Otherwise the GS: the busier of the
	/// MTGS thread and the back thread (the back thread splits the work, so the sum would not fall with it).
	std::optional<double> CostOf(AutoTuneBottleneck bottleneck, const AutoTuneWindow& w) const;
	AutoTuneAction StartPendingTrial(const AutoTuneLeverState& state);
	AutoTuneAction FinishTrial();
	/// Whether the game itself changed between the trial's baseline and its windows, which makes a loss look like
	/// the lever's doing. `noise` is the speed judgement's, relative to the baseline.
	bool SceneChangedDuringTrial(double noise) const;
	/// How many times this step has been undone for a changed scene and measured again this session.
	uint32_t& RetriesFor(AutoTuneLever lever, double value);
	void EnterSettle(Phase phase, double seconds);

	AutoTunePolicy m_policy;
	Phase m_phase = Phase::WarmUp;
	double m_phase_seconds = 0.0; // warm-up elapsed, or settle remaining
	uint32_t m_streak = 0;
	std::vector<AutoTuneWindow> m_streak_windows;
	Trial m_trial;
	Pending m_pending;
	std::vector<AutoTuneRejection> m_rejected; // this session
	struct Retries
	{
		AutoTuneLever lever;
		double value;
		uint32_t count;
	};
	std::vector<Retries> m_retries; // this session
	// "Nothing left to try" is said once per session, whatever the class: a thread on the saturation line flips
	// the class between GS (CPU) and GPU, and both have nothing left.
	bool m_exhausted_said = false;
	bool m_reported[static_cast<size_t>(AutoTuneBottleneck::Count)] = {};
	double m_unclassified_cooldown = 0.0;
	AutoTuneBottleneck m_last_bottleneck = AutoTuneBottleneck::None;
};
