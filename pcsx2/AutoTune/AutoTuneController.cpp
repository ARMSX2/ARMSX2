// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "AutoTune/AutoTuneController.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace
{
	constexpr double kValueEpsilon = 1e-6;
	// AccBlendLevel::Basic, the default. AutoTuneApplier.cpp static_asserts the numbering.
	constexpr int kBlendingBasic = 1;

	constexpr const char* kLeverNames[] = {
		"iop-thread",
		"ee-governor",
		"resolution",
		"gs-back-thread",
		"download-mode",
		"blending",
	};
	static_assert(std::size(kLeverNames) == static_cast<size_t>(AutoTuneLever::Count));

	constexpr const char* kBottleneckNames[] = {
		"unknown",
		"EE",
		"GS (CPU)",
		"GPU",
		"VU1",
		"IOP",
	};
	static_assert(std::size(kBottleneckNames) == static_cast<size_t>(AutoTuneBottleneck::Count));

	// The order a bottleneck's levers are tried in. Every lever is tried at most once per value, so a lever
	// that is not applicable (already on, forbidden, forced by the game database) is skipped.
	constexpr AutoTuneLever kEeOrder[] = {AutoTuneLever::IopThread, AutoTuneLever::EeGovernor};
	// The GS threads are the cause and the GPU is not: the back thread splits the CPU work, so it goes first.
	constexpr AutoTuneLever kGsCpuOrder[] = {AutoTuneLever::GsBackThread, AutoTuneLever::Resolution,
		AutoTuneLever::DownloadMode, AutoTuneLever::Blending};
	// The GPU is the cause: resolution is the lever that moves the GPU's work most. Then the download mode, ahead of
	// the back thread: a stall on readbacks reads as the GPU (the EE blocked on a GS that is not CPU-bound), the
	// mode is cheap to try (the light settle, no renderer reopen), and the back thread costs two renderer reopens
	// when it does not help (OutRun and Need for Speed on the MQ65: 18 s late for it).
	constexpr AutoTuneLever kGpuOrder[] = {AutoTuneLever::Resolution, AutoTuneLever::DownloadMode,
		AutoTuneLever::GsBackThread, AutoTuneLever::Blending};
	// The same two when the game is so far under full speed that a notch of resolution cannot close the gap
	// (AutoTunePolicy::cheap_levers_first_below_pct): blending and the download mode go before resolution.
	constexpr AutoTuneLever kGsCpuFarOrder[] = {AutoTuneLever::GsBackThread, AutoTuneLever::Blending,
		AutoTuneLever::DownloadMode, AutoTuneLever::Resolution};
	constexpr AutoTuneLever kGpuFarOrder[] = {AutoTuneLever::Blending, AutoTuneLever::DownloadMode,
		AutoTuneLever::Resolution, AutoTuneLever::GsBackThread};

	double Mean(double sum, size_t n)
	{
		return n ? sum / static_cast<double>(n) : 0.0;
	}
} // namespace

AutoTuneStat AutoTuneSummarize(std::vector<double> values)
{
	AutoTuneStat stat;
	stat.n = values.size();
	if (values.empty())
		return stat;

	const auto median = [](std::vector<double>& v) {
		std::sort(v.begin(), v.end());
		const size_t mid = v.size() / 2;
		return (v.size() % 2) ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
	};

	stat.centre = median(values);

	std::vector<double> deviations;
	deviations.reserve(values.size());
	for (const double v : values)
		deviations.push_back(std::fabs(v - stat.centre));
	const double sigma = 1.4826 * median(deviations);
	stat.error = 1.2533 * sigma / std::sqrt(static_cast<double>(values.size()));
	return stat;
}

AutoTuneGain AutoTuneJudgeGain(const AutoTuneStat& before, const AutoTuneStat& after, bool higher_is_better, const AutoTunePolicy& policy)
{
	AutoTuneGain g;
	const double base = std::max(std::fabs(before.centre), 1e-6);
	g.gain = (higher_is_better ? after.centre - before.centre : before.centre - after.centre) / base;

	const double error = std::hypot(before.error, after.error) / base;
	g.noise = policy.noise_z * std::max(error, policy.noise_floor_fraction);

	g.clear = g.gain >= policy.gain_fraction - 1e-9 && g.gain > g.noise;
	g.none = g.gain + g.noise < policy.gain_fraction;
	g.worse = g.gain <= -(policy.gain_fraction - 1e-9) && g.gain < -g.noise;
	return g;
}

const char* AutoTuneLeverName(AutoTuneLever lever)
{
	const size_t i = static_cast<size_t>(lever);
	return i < std::size(kLeverNames) ? kLeverNames[i] : "?";
}

const char* AutoTuneBottleneckName(AutoTuneBottleneck bottleneck)
{
	const size_t i = static_cast<size_t>(bottleneck);
	return i < std::size(kBottleneckNames) ? kBottleneckNames[i] : "?";
}

std::optional<AutoTuneLever> AutoTuneLeverFromName(const char* name)
{
	for (size_t i = 0; i < std::size(kLeverNames); i++)
	{
		if (std::strcmp(kLeverNames[i], name) == 0)
			return static_cast<AutoTuneLever>(i);
	}
	return std::nullopt;
}

const char* AutoTuneController::PhaseName(Phase phase)
{
	switch (phase)
	{
		case Phase::WarmUp:
			return "warmup";
		case Phase::Watching:
			return "watching";
		case Phase::Baselining:
			return "baselining";
		case Phase::SettlingTrial:
			return "settling";
		case Phase::Measuring:
			return "measuring";
		case Phase::SettlingAfter:
			return "settling-after";
	}
	return "?";
}

AutoTuneController::AutoTuneController(const AutoTunePolicy& policy)
	: m_policy(policy)
{
	std::sort(m_policy.resolution_notches.begin(), m_policy.resolution_notches.end());
}

void AutoTuneController::Reset()
{
	m_phase = Phase::WarmUp;
	m_phase_seconds = 0.0;
	m_streak = 0;
	m_streak_windows.clear();
	m_trial = Trial();
	m_rejected.clear();
	m_retries.clear();
	m_exhausted_said = false;
	std::fill(std::begin(m_reported), std::end(m_reported), false);
	m_unclassified_cooldown = 0.0;
	m_last_bottleneck = AutoTuneBottleneck::None;
}

bool AutoTuneController::IsBad(const AutoTuneWindow& w) const
{
	return w.speed_pct < m_policy.bad_speed_pct || w.late_frame_ratio >= m_policy.late_frame_ratio_bad;
}

void AutoTuneController::EnterSettle(Phase phase, double seconds)
{
	m_phase = phase;
	m_phase_seconds = seconds;
	m_streak = 0;
	m_streak_windows.clear();
}

double AutoTuneController::SettleSecondsFor(AutoTuneLever lever) const
{
	switch (lever)
	{
		// These reset the recompilers or reopen the renderer, and the hitch that follows is longer.
		case AutoTuneLever::IopThread:
		case AutoTuneLever::EeGovernor:
		case AutoTuneLever::GsBackThread:
			return m_policy.settle_heavy_seconds;
		default:
			return m_policy.settle_seconds;
	}
}

double AutoTuneController::TrialSettleSeconds(AutoTuneLever lever, double speed_pct) const
{
	const double hitch = SettleSecondsFor(lever);
	if (lever != AutoTuneLever::EeGovernor)
		return hitch;

	// The governor's clocks run in game time (see AutoTunePolicy::governor_settle_game_seconds); the wall time
	// they take is that over the speed the game is running at.
	const double speed = std::clamp(speed_pct, m_policy.governor_min_speed_pct, 100.0) / 100.0;
	return hitch + m_policy.governor_settle_game_seconds / speed;
}

uint32_t AutoTuneController::MeasureWindowsFor(AutoTuneLever lever) const
{
	return lever == AutoTuneLever::EeGovernor ? m_policy.governor_measure_windows : m_policy.measure_windows;
}

std::optional<double> AutoTuneController::CostOf(AutoTuneBottleneck bottleneck, const AutoTuneWindow& w) const
{
	double ms = 0.0;
	switch (bottleneck)
	{
		case AutoTuneBottleneck::Ee:
			ms = w.ee_ms;
			break;
		case AutoTuneBottleneck::Gpu:
			ms = w.has_gpu_timer ? w.gpu_ms : std::max(w.mtgs_ms, w.gs_back_ms);
			break;
		case AutoTuneBottleneck::GsCpu:
			ms = std::max(w.mtgs_ms, w.gs_back_ms);
			break;
		default:
			return std::nullopt;
	}
	if (!(ms > 0.0))
		return std::nullopt;
	return ms;
}

AutoTuneBottleneck AutoTuneController::Classify(const AutoTuneWindow& m) const
{
	const double sat = m_policy.saturation_pct;
	const bool gs_cpu = m.mtgs_pct >= sat || m.gs_back_pct >= sat;
	const bool ee_blocked_on_gs = m.ee_wait_gs_ratio.has_value() && *m.ee_wait_gs_ratio > m_policy.ee_blocked_on_gs_ratio;
	const bool ee_blocked_on_iop = m.ee_wait_iop_ratio.has_value() && *m.ee_wait_iop_ratio > m_policy.ee_blocked_on_iop_ratio;

	// A busy GPU wins over busy GS threads: the GS thread of a GPU-bound game spends its time feeding or
	// waiting on the GPU and reads just as busy, and the lever for that is the resolution, not a back thread.
	if (m.has_gpu_timer && m.gpu_pct >= m_policy.gpu_high_pct)
		return AutoTuneBottleneck::Gpu;

	// The GS threads are saturated and the GPU is not: they are the cause, whatever the EE shows (it spins
	// waiting for them).
	if (gs_cpu)
		return AutoTuneBottleneck::GsCpu;

	// The EE is stuck behind a GS that is not CPU-bound: a stall inside the GS.
	if (ee_blocked_on_gs)
		return AutoTuneBottleneck::Gpu;

	if (m.mtvu_pct >= sat)
		return AutoTuneBottleneck::Vu1;
	// The IOP thread is the cause when the EE is stuck waiting for it: the EE then reads as saturated too (it
	// spins), and the levers for an EE-bound game would not help. Its own CPU share is not evidence of that on
	// its own: it paces itself against the EE and spins while it waits, so it reads saturated whenever the EE
	// is, and the EE waiting 0% of its time for it says it keeps up. The CPU share is the answer only where the
	// wait could not be measured.
	const bool iop_bound = m.ee_wait_iop_ratio.has_value() ? ee_blocked_on_iop : m.iop_pct >= sat;
	if (iop_bound)
		return AutoTuneBottleneck::Iop;

	if (m.ee_pct >= sat)
		return AutoTuneBottleneck::Ee;

	// Slow, nothing saturated, and no way to see the GPU or the EE's waits: what is left is the GPU.
	if (!m.has_gpu_timer && !m.ee_wait_gs_ratio.has_value())
		return AutoTuneBottleneck::Gpu;

	return AutoTuneBottleneck::None;
}

bool AutoTuneController::IsRejected(AutoTuneLever lever, double value, const AutoTuneLeverState& state) const
{
	const auto matches = [&](const AutoTuneRejection& r) {
		return r.lever == lever && std::fabs(r.value - value) < kValueEpsilon;
	};
	return std::any_of(m_rejected.begin(), m_rejected.end(), matches) ||
	       std::any_of(state.rejected.begin(), state.rejected.end(), matches);
}

std::optional<double> AutoTuneController::TargetFor(AutoTuneLever lever, const AutoTuneLeverState& s, AutoTuneBottleneck bottleneck) const
{
	switch (lever)
	{
		case AutoTuneLever::IopThread:
			if (!s.iop_thread_on && s.iop_thread_placeable && !s.iop_thread_forced)
				return 1.0;
			return std::nullopt;

		case AutoTuneLever::EeGovernor:
			if (!s.ee_governor_on && s.ee_governor_eligible && !s.ee_governor_forced && !s.hardcore)
				return 1.0;
			return std::nullopt;

		case AutoTuneLever::Resolution:
		{
			if (!s.hw_renderer)
				return std::nullopt;
			// The largest notch below the current multiplier, never under the first notch (1x).
			// A multiplier already at or under the first notch has nowhere to go.
			const auto& notches = m_policy.resolution_notches;
			if (notches.empty() || s.resolution <= notches.front() + kValueEpsilon)
				return std::nullopt;
			double target = notches.front();
			for (const double notch : notches)
			{
				if (notch < s.resolution - kValueEpsilon)
					target = notch;
			}
			return target;
		}

		case AutoTuneLever::GsBackThread:
			if (!s.gs_back_thread_on && s.gs_back_thread_allowed)
				return 1.0;
			return std::nullopt;

		case AutoTuneLever::DownloadMode:
			if (!s.hw_renderer || s.download_mode_forced)
				return std::nullopt;
			// Only a mode that reads back has anything to give up. Accurate and Force Full both read back
			// and wait for it; Asynchronous reads back without making the EE wait; No Readbacks stops.
			if (s.download_mode == AutoTuneDownloadMode::Accurate || s.download_mode == AutoTuneDownloadMode::AccurateForceFull)
				return static_cast<double>(AutoTuneDownloadMode::Asynchronous);
			if (s.download_mode == AutoTuneDownloadMode::Asynchronous)
				return static_cast<double>(AutoTuneDownloadMode::NoReadbacks);
			return std::nullopt;

		case AutoTuneLever::Blending:
		{
			if (!s.hw_renderer)
				return std::nullopt;
			int floor = std::max(0, s.blending_floor);
			// Basic to Minimum is a speed lever only where the GS thread on the CPU is the limit: on a GPU-bound
			// game it measured +3.8% and -12% (SD865), and it is no help to a stall. Above Basic the steps are
			// offered to every bottleneck.
			if (bottleneck != AutoTuneBottleneck::GsCpu)
				floor = std::max(floor, kBlendingBasic);
			if (s.blending_level > floor)
				return static_cast<double>(s.blending_level - 1);
			return std::nullopt;
		}

		default:
			return std::nullopt;
	}
}

bool AutoTuneController::CheapLeversHaveSomethingToGive(double speed_pct, const AutoTuneLeverState& s) const
{
	// Blending only gives a lot from above Basic; Basic to Minimum is a small, scene-dependent step (and the
	// only one left at the default), so it does not justify putting the cheap levers before the resolution.
	if (s.blending_level <= kBlendingBasic)
		return false;

	// With a notch to take: one notch from r removes (r / next)^2 of the pixels, so it can give that much speed
	// at most. If that gets to full speed, the resolution goes first.
	const std::optional<double> next = TargetFor(AutoTuneLever::Resolution, s, AutoTuneBottleneck::Gpu);
	if (!next || !(*next > 0.0))
		return true; // no notch (native, or the software renderer): there is no resolution step to put them before
	const double pixels = (s.resolution / *next) * (s.resolution / *next);
	return speed_pct * pixels < 100.0;
}

AutoTuneAction AutoTuneController::Diagnose(const AutoTuneLeverState& state)
{
	// Average the bad windows that triggered this.
	AutoTuneWindow mean;
	mean.has_gpu_timer = m_streak_windows.front().has_gpu_timer;
	double speed = 0, late = 0, ee = 0, mtgs = 0, gsb = 0, vu = 0, iop = 0, gpu = 0, wgs = 0, wvu = 0, wiop = 0;
	size_t n_wgs = 0, n_wvu = 0, n_wiop = 0;
	for (const AutoTuneWindow& w : m_streak_windows)
	{
		speed += w.speed_pct;
		late += w.late_frame_ratio;
		ee += w.ee_pct;
		mtgs += w.mtgs_pct;
		gsb += w.gs_back_pct;
		vu += w.mtvu_pct;
		iop += w.iop_pct;
		gpu += w.gpu_pct;
		if (w.ee_wait_gs_ratio)
		{
			wgs += *w.ee_wait_gs_ratio;
			n_wgs++;
		}
		if (w.ee_wait_vu1_ratio)
		{
			wvu += *w.ee_wait_vu1_ratio;
			n_wvu++;
		}
		if (w.ee_wait_iop_ratio)
		{
			wiop += *w.ee_wait_iop_ratio;
			n_wiop++;
		}
	}
	const size_t n = m_streak_windows.size();
	mean.speed_pct = Mean(speed, n);
	mean.late_frame_ratio = Mean(late, n);
	mean.ee_pct = Mean(ee, n);
	mean.mtgs_pct = Mean(mtgs, n);
	mean.gs_back_pct = Mean(gsb, n);
	mean.mtvu_pct = Mean(vu, n);
	mean.iop_pct = Mean(iop, n);
	mean.gpu_pct = Mean(gpu, n);
	if (n_wgs)
		mean.ee_wait_gs_ratio = Mean(wgs, n_wgs);
	if (n_wvu)
		mean.ee_wait_vu1_ratio = Mean(wvu, n_wvu);
	if (n_wiop)
		mean.ee_wait_iop_ratio = Mean(wiop, n_wiop);

	const AutoTuneBottleneck bottleneck = Classify(mean);
	m_last_bottleneck = bottleneck;

	AutoTuneAction action;
	action.bottleneck = bottleneck;

	if (bottleneck == AutoTuneBottleneck::None)
	{
		if (m_unclassified_cooldown > 0.0)
			return action;
		m_unclassified_cooldown = m_policy.unclassified_cooldown_seconds;
		action.kind = AutoTuneAction::Kind::Unclassified;
		return action;
	}

	if (bottleneck == AutoTuneBottleneck::Vu1 || bottleneck == AutoTuneBottleneck::Iop)
	{
		bool& reported = m_reported[static_cast<size_t>(bottleneck)];
		if (reported)
			return action;
		reported = true;
		action.kind = AutoTuneAction::Kind::ReportBottleneck;
		return action;
	}

	// So far under full speed that one notch of resolution cannot close the gap: see the policy.
	std::vector<double> streak_speeds;
	for (const AutoTuneWindow& w : m_streak_windows)
		streak_speeds.push_back(w.speed_pct);
	const double centre_speed = AutoTuneSummarize(streak_speeds).centre;
	const bool far_below = m_policy.cheap_levers_first_below_pct > 0.0 && centre_speed < m_policy.cheap_levers_first_below_pct &&
	                       CheapLeversHaveSomethingToGive(centre_speed, state);

	const AutoTuneLever* order = nullptr;
	size_t count = 0;
	switch (bottleneck)
	{
		case AutoTuneBottleneck::Ee:
			order = kEeOrder;
			count = std::size(kEeOrder);
			break;
		case AutoTuneBottleneck::GsCpu:
			order = far_below ? kGsCpuFarOrder : kGsCpuOrder;
			count = far_below ? std::size(kGsCpuFarOrder) : std::size(kGsCpuOrder);
			break;
		default:
			order = far_below ? kGpuFarOrder : kGpuOrder;
			count = far_below ? std::size(kGpuFarOrder) : std::size(kGpuOrder);
			break;
	}

	for (size_t i = 0; i < count; i++)
	{
		const std::optional<double> target = TargetFor(order[i], state, bottleneck);
		if (!target || IsRejected(order[i], *target, state))
			continue;

		// The windows that triggered this are the start of the baseline; the trial starts once it has the rest.
		m_pending = Pending();
		m_pending.lever = order[i];
		m_pending.value = *target;
		m_pending.bottleneck = bottleneck;
		m_pending.baseline = m_streak_windows;
		m_pending.extra_wanted = m_policy.baseline_extra_windows;
		m_phase = Phase::Baselining;
		m_phase_seconds = 0.0;
		m_streak = 0;
		m_streak_windows.clear();
		if (m_pending.extra_wanted == 0)
			return StartPendingTrial(state);
		return action;
	}

	// Nothing left for this bottleneck. Said once; the session then stays quiet about it, for every class.
	if (m_exhausted_said)
		return action;
	m_exhausted_said = true;
	action.kind = AutoTuneAction::Kind::Exhausted;
	return action;
}

AutoTuneAction AutoTuneController::StartPendingTrial(const AutoTuneLeverState& state)
{
	AutoTuneAction none;
	none.bottleneck = m_pending.bottleneck;

	// The state may have moved since the diagnosis (somebody changed a setting, or the step is no longer there).
	const std::optional<double> target = TargetFor(m_pending.lever, state, m_pending.bottleneck);
	if (!target || std::fabs(*target - m_pending.value) > kValueEpsilon || IsRejected(m_pending.lever, *target, state))
	{
		m_phase = Phase::Watching;
		m_phase_seconds = 0.0;
		m_pending = Pending();
		return none;
	}

	std::vector<double> speeds, costs, ee_ms, gs_ms, gpu_ms;
	double late = 0.0;
	for (const AutoTuneWindow& w : m_pending.baseline)
	{
		speeds.push_back(w.speed_pct);
		late += w.late_frame_ratio;
		if (const std::optional<double> cost = CostOf(m_pending.bottleneck, w))
			costs.push_back(*cost);
		if (w.ee_ms > 0.0)
			ee_ms.push_back(w.ee_ms);
		if (const double gs = std::max(w.mtgs_ms, w.gs_back_ms); gs > 0.0)
			gs_ms.push_back(gs);
		if (w.has_gpu_timer && w.gpu_ms > 0.0)
			gpu_ms.push_back(w.gpu_ms);
	}

	m_trial = Trial();
	m_trial.lever = m_pending.lever;
	m_trial.value = m_pending.value;
	m_trial.bottleneck = m_pending.bottleneck;
	m_trial.speed_before = AutoTuneSummarize(speeds);
	m_trial.cost_before = AutoTuneSummarize(costs);
	m_trial.ee_before = AutoTuneSummarize(ee_ms);
	m_trial.gs_before = AutoTuneSummarize(gs_ms);
	m_trial.gpu_before = AutoTuneSummarize(gpu_ms);
	m_trial.late_before = Mean(late, m_pending.baseline.size());
	m_trial.windows = MeasureWindowsFor(m_pending.lever);
	EnterSettle(Phase::SettlingTrial, TrialSettleSeconds(m_pending.lever, m_trial.speed_before.centre));

	AutoTuneAction action;
	action.kind = AutoTuneAction::Kind::StartTrial;
	action.lever = m_pending.lever;
	action.value = m_pending.value;
	action.bottleneck = m_pending.bottleneck;
	action.speed_before = m_trial.speed_before.centre;
	action.cost_before = m_trial.cost_before.n >= m_policy.min_cost_windows ? m_trial.cost_before.centre : 0.0;
	m_pending = Pending();
	return action;
}

uint32_t& AutoTuneController::RetriesFor(AutoTuneLever lever, double value)
{
	for (Retries& r : m_retries)
	{
		if (r.lever == lever && std::fabs(r.value - value) < kValueEpsilon)
			return r.count;
	}
	m_retries.push_back({lever, value, 0});
	return m_retries.back().count;
}

bool AutoTuneController::SceneChangedDuringTrial(double noise) const
{
	// The two halves of the measurement tell different stories. A trial's windows are one state of the game,
	// the lever's; when the first half and the second half differ by more than the noise, something moved
	// while they were taken. Two lumps of windows also make the spread-based noise estimate too small, which
	// is how a loss inside the lumps looked certain. The governor ramps down and back by design.
	const size_t n = m_trial.speeds.size();
	if (n >= m_policy.scene_halves_min_windows && n >= 4 && m_trial.lever != AutoTuneLever::EeGovernor)
	{
		const size_t half = n / 2;
		const std::vector<double> first(m_trial.speeds.begin(), m_trial.speeds.begin() + static_cast<std::ptrdiff_t>(half));
		const std::vector<double> second(m_trial.speeds.end() - static_cast<std::ptrdiff_t>(half), m_trial.speeds.end());
		const double base = std::max(std::fabs(m_trial.speed_before.centre), 1e-6);
		if (std::fabs(AutoTuneSummarize(first).centre - AutoTuneSummarize(second).centre) / base > noise)
			return true;
	}

	// A resource the lever does not act on cost something else per frame in the trial than before it: the game
	// is doing something else now.
	const auto moved = [&](const AutoTuneStat& before, const std::vector<double>& windows) {
		if (before.n < m_policy.min_cost_windows || windows.size() < m_policy.min_cost_windows || !(before.centre > 0.0))
			return false;
		const double now = AutoTuneSummarize(windows).centre;
		return std::fabs(now - before.centre) / before.centre > m_policy.scene_shift_fraction;
	};
	switch (m_trial.lever)
	{
		case AutoTuneLever::IopThread:
		case AutoTuneLever::EeGovernor:
			return moved(m_trial.gs_before, m_trial.gs_ms) || moved(m_trial.gpu_before, m_trial.gpu_ms);
		case AutoTuneLever::Resolution:
		case AutoTuneLever::Blending:
			return moved(m_trial.ee_before, m_trial.ee_ms);
		case AutoTuneLever::GsBackThread:
		case AutoTuneLever::DownloadMode:
			return moved(m_trial.gpu_before, m_trial.gpu_ms);
		default:
			return false;
	}
}

AutoTuneAction AutoTuneController::FinishTrial()
{
	const AutoTuneStat speed_after = AutoTuneSummarize(m_trial.speeds);
	const AutoTuneStat cost_after = AutoTuneSummarize(m_trial.costs);
	const double late_after = Mean(m_trial.late_sum, m_trial.collected);

	const AutoTuneGain speed = AutoTuneJudgeGain(m_trial.speed_before, speed_after, true, m_policy);

	// The cost the lever targets counts only where both sides have enough windows with a figure for it.
	const bool have_cost = m_trial.cost_before.n >= m_policy.min_cost_windows && cost_after.n >= m_policy.min_cost_windows;
	AutoTuneGain cost;
	if (have_cost)
		cost = AutoTuneJudgeGain(m_trial.cost_before, cost_after, false, m_policy);

	// From bad to fully good counts even when the gain is under the bar. A baseline that was already fast
	// and only had late frames does not get this for free: it needs the late frames gone.
	const bool was_good_speed = m_trial.speed_before.centre >= m_policy.reached_speed_pct;
	const bool was_late = m_trial.late_before >= m_policy.late_frame_ratio_bad;
	const bool fixed = speed_after.centre >= m_policy.reached_speed_pct && late_after < m_policy.late_frame_ratio_bad &&
	                   (!was_good_speed || was_late);

	// Keep: the speed clearly rose, or reached full speed. Only the speed earns a keep. A cost falls whenever
	// work is split over more threads, whether or not the game is faster (the GS back thread on God of War II:
	// -14% GS cost, speed 39.7% -> 39.5%, the EE still the limit), so a lower cost on its own is not a reason to
	// give up a core or a setting.
	const bool keep = fixed || speed.clear;
	// Reject, and remember it: the speed clearly fell, or it shows no gain even allowing for the noise and the
	// cost shows none either. Anything else is looked at for longer, and then inconclusive.
	//
	// The speed clearly fell, whatever the cost did: the lever made things worse, and that is as good a
	// measurement as there is. It ends the trial at the first look instead of keeping the player at the lower
	// speed for another eight windows.
	const bool hurt = speed.worse;
	// A cost that clearly fell is the one thing that holds a flat speed back from a rejection at the first look:
	// the speed may follow it. If it does not, the longer look ends in a rejection (below).
	const bool cost_fell = have_cost && cost.clear;
	const bool reject = hurt || (speed.none && (!have_cost || cost.none));

	AutoTuneAction action;
	action.lever = m_trial.lever;
	action.value = m_trial.value;
	action.bottleneck = m_last_bottleneck;
	action.speed_before = m_trial.speed_before.centre;
	action.speed_after = speed_after.centre;
	action.gain = speed.gain;
	if (have_cost)
	{
		action.cost_before = m_trial.cost_before.centre;
		action.cost_after = cost_after.centre;
		action.cost_gain = cost.gain;
	}

	if (keep)
	{
		action.kind = AutoTuneAction::Kind::Keep;
		action.measured = true;
		// Nothing about the applied state changes for a keep, so only the hitch of persisting it is waited out.
		EnterSettle(Phase::SettlingAfter, m_policy.settle_seconds);
		m_trial = Trial();
		return action;
	}

	if (!reject)
	{
		// Neither a clear gain nor clearly nothing, or the speed is flat but the cost fell. Look a little longer
		// before giving up.
		if (m_trial.extensions < m_policy.max_extensions && m_policy.extend_windows > 0)
		{
			m_trial.extensions++;
			m_trial.windows += m_policy.extend_windows;
			return AutoTuneAction();
		}
	}

	// The look is over. The speed is flat after all that, and the cost that fell did not take it along: the work
	// moved between threads and the game did not get faster. That did not help.
	const bool did_not_help = reject || (speed.none && cost_fell);

	// A rejection is only as good as the baseline it is measured against. If the game changed while the trial
	// ran, the loss may be the scene's: undo the trial without writing it down, and measure the step again from
	// a fresh baseline. Allowed a couple of times for one step; a scene that keeps moving gets the
	// inconclusive answer below.
	const bool scene_changed = did_not_help && SceneChangedDuringTrial(speed.noise);
	if (scene_changed)
	{
		uint32_t& retries = RetriesFor(m_trial.lever, m_trial.value);
		if (retries < m_policy.max_extensions)
		{
			retries++;
			action.kind = AutoTuneAction::Kind::Revert;
			action.measured = false;
			action.inconclusive = true;
			action.retry = true;
			EnterSettle(Phase::SettlingAfter, SettleSecondsFor(m_trial.lever));
			m_trial = Trial();
			return action;
		}
	}

	if (!did_not_help || scene_changed)
	{
		// Still within the noise, or too unsure about the scene: undo it and move on, without writing "did not
		// help" for good. This session does not offer the same step again.
		action.kind = AutoTuneAction::Kind::Revert;
		action.measured = false;
		action.inconclusive = true;
		m_rejected.push_back({m_trial.lever, m_trial.value});
		EnterSettle(Phase::SettlingAfter, SettleSecondsFor(m_trial.lever));
		m_trial = Trial();
		return action;
	}

	action.kind = AutoTuneAction::Kind::Revert;
	action.measured = true;
	m_rejected.push_back({m_trial.lever, m_trial.value});
	EnterSettle(Phase::SettlingAfter, SettleSecondsFor(m_trial.lever));
	m_trial = Trial();
	return action;
}

AutoTuneAction AutoTuneController::OnWindow(const AutoTuneWindow& w, const AutoTuneLeverState& state)
{
	AutoTuneAction none;

	// Unusable windows are as if they had not happened.
	if (!w.usable)
		return none;

	if (m_unclassified_cooldown > 0.0)
		m_unclassified_cooldown = std::max(0.0, m_unclassified_cooldown - w.seconds);

	switch (m_phase)
	{
		case Phase::WarmUp:
			m_phase_seconds += w.seconds;
			if (m_phase_seconds >= m_policy.warmup_seconds)
			{
				m_phase = Phase::Watching;
				m_phase_seconds = 0.0;
				m_streak = 0;
				m_streak_windows.clear();
			}
			return none;

		case Phase::Watching:
			if (!IsBad(w))
			{
				m_streak = 0;
				m_streak_windows.clear();
				return none;
			}
			m_streak++;
			m_streak_windows.push_back(w);
			if (m_streak < m_policy.trigger_windows)
				return none;
			{
				const AutoTuneAction action = Diagnose(state);
				// A diagnosis that started a trial has moved to SettlingTrial and cleared the streak itself.
				if (m_phase == Phase::Watching)
				{
					m_streak = 0;
					m_streak_windows.clear();
				}
				return action;
			}

		case Phase::Baselining:
			// The problem has to still be there: a window that is fine ends the diagnosis, and the next bad
			// streak starts over.
			if (!IsBad(w))
			{
				m_phase = Phase::Watching;
				m_phase_seconds = 0.0;
				m_pending = Pending();
				return none;
			}
			m_pending.baseline.push_back(w);
			if (m_pending.baseline.size() < m_pending.extra_wanted + m_policy.trigger_windows)
				return none;
			return StartPendingTrial(state);

		case Phase::SettlingTrial:
			m_phase_seconds -= w.seconds;
			if (m_phase_seconds <= 0.0)
			{
				m_phase = Phase::Measuring;
				m_phase_seconds = 0.0;
			}
			return none;

		case Phase::Measuring:
			m_trial.speeds.push_back(w.speed_pct);
			if (const std::optional<double> cost = CostOf(m_trial.bottleneck, w))
				m_trial.costs.push_back(*cost);
			if (w.ee_ms > 0.0)
				m_trial.ee_ms.push_back(w.ee_ms);
			if (const double gs = std::max(w.mtgs_ms, w.gs_back_ms); gs > 0.0)
				m_trial.gs_ms.push_back(gs);
			if (w.has_gpu_timer && w.gpu_ms > 0.0)
				m_trial.gpu_ms.push_back(w.gpu_ms);
			m_trial.late_sum += w.late_frame_ratio;
			m_trial.collected++;
			if (m_trial.collected < m_trial.windows)
				return none;
			return FinishTrial();

		case Phase::SettlingAfter:
			m_phase_seconds -= w.seconds;
			if (m_phase_seconds <= 0.0)
			{
				m_phase = Phase::Watching;
				m_phase_seconds = 0.0;
				m_streak = 0;
				m_streak_windows.clear();
			}
			return none;
	}

	return none;
}

std::optional<AutoTuneAction> AutoTuneController::OnDisturbance(AutoTuneDisturbance kind)
{
	// A pause leaves a running trial alone: the paused windows were ignored already, and the figures
	// around the pause are not wrong, only discontinuous.
	if (kind == AutoTuneDisturbance::Resumed)
	{
		m_streak = 0;
		m_streak_windows.clear();
		return std::nullopt;
	}

	std::optional<AutoTuneAction> result;

	// A trial in flight can no longer be measured: the scene changed, or somebody else changed a setting.
	if (TrialActive())
	{
		AutoTuneAction revert;
		revert.kind = AutoTuneAction::Kind::Revert;
		revert.lever = m_trial.lever;
		revert.value = m_trial.value;
		revert.bottleneck = m_last_bottleneck;
		revert.measured = false;
		result = revert;
		m_trial = Trial();
	}

	if (kind == AutoTuneDisturbance::SceneChange)
	{
		m_phase = Phase::WarmUp;
		m_phase_seconds = 0.0;
		m_streak = 0;
		m_streak_windows.clear();
	}
	else if (m_phase == Phase::SettlingAfter)
	{
		// Already settling: wait for whichever is longer.
		m_phase_seconds = std::max(m_phase_seconds, m_policy.settle_seconds);
		m_streak = 0;
		m_streak_windows.clear();
	}
	else if (m_phase != Phase::WarmUp)
	{
		EnterSettle(Phase::SettlingAfter, m_policy.settle_seconds);
	}

	return result;
}

void AutoTuneController::NoteApplyFailed()
{
	if (!TrialActive())
		return;
	m_rejected.push_back({m_trial.lever, m_trial.value});
	m_trial = Trial();
	EnterSettle(Phase::SettlingAfter, m_policy.settle_seconds);
}
