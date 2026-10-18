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
	// The GPU is the cause: resolution is the lever that moves the GPU's work most.
	constexpr AutoTuneLever kGpuOrder[] = {AutoTuneLever::Resolution, AutoTuneLever::GsBackThread,
		AutoTuneLever::DownloadMode, AutoTuneLever::Blending};

	double Mean(double sum, size_t n)
	{
		return n ? sum / static_cast<double>(n) : 0.0;
	}
} // namespace

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
	std::fill(std::begin(m_exhausted), std::end(m_exhausted), false);
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

AutoTuneBottleneck AutoTuneController::Classify(const AutoTuneWindow& m) const
{
	const double sat = m_policy.saturation_pct;
	const bool gs_cpu = m.mtgs_pct >= sat || m.gs_back_pct >= sat;
	const bool ee_blocked_on_gs = m.ee_wait_gs_ratio.has_value() && *m.ee_wait_gs_ratio > m_policy.ee_blocked_on_gs_ratio;

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
	if (m.iop_pct >= sat)
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

std::optional<double> AutoTuneController::TargetFor(AutoTuneLever lever, const AutoTuneLeverState& s) const
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
			const int floor = std::max(0, s.blending_floor);
			if (s.blending_level > floor)
				return static_cast<double>(s.blending_level - 1);
			return std::nullopt;
		}

		default:
			return std::nullopt;
	}
}

AutoTuneAction AutoTuneController::Diagnose(const AutoTuneLeverState& state)
{
	// Average the bad windows that triggered this.
	AutoTuneWindow mean;
	mean.has_gpu_timer = m_streak_windows.front().has_gpu_timer;
	double speed = 0, late = 0, ee = 0, mtgs = 0, gsb = 0, vu = 0, iop = 0, gpu = 0, wgs = 0, wvu = 0;
	size_t n_wgs = 0, n_wvu = 0;
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

	const AutoTuneLever* order = nullptr;
	size_t count = 0;
	switch (bottleneck)
	{
		case AutoTuneBottleneck::Ee:
			order = kEeOrder;
			count = std::size(kEeOrder);
			break;
		case AutoTuneBottleneck::GsCpu:
			order = kGsCpuOrder;
			count = std::size(kGsCpuOrder);
			break;
		default:
			order = kGpuOrder;
			count = std::size(kGpuOrder);
			break;
	}

	for (size_t i = 0; i < count; i++)
	{
		const std::optional<double> target = TargetFor(order[i], state);
		if (!target || IsRejected(order[i], *target, state))
			continue;

		m_trial = Trial();
		m_trial.lever = order[i];
		m_trial.value = *target;
		m_trial.speed_before = mean.speed_pct;
		m_trial.late_before = mean.late_frame_ratio;
		EnterSettle(Phase::SettlingTrial, SettleSecondsFor(order[i]));

		action.kind = AutoTuneAction::Kind::StartTrial;
		action.lever = order[i];
		action.value = *target;
		action.speed_before = mean.speed_pct;
		return action;
	}

	// Nothing left for this bottleneck. Said once; the session then stays quiet about it.
	bool& exhausted = m_exhausted[static_cast<size_t>(bottleneck)];
	if (exhausted)
		return action;
	exhausted = true;
	action.kind = AutoTuneAction::Kind::Exhausted;
	return action;
}

AutoTuneAction AutoTuneController::FinishTrial()
{
	const double speed_after = Mean(m_trial.speed_sum, m_trial.collected);
	const double late_after = Mean(m_trial.late_sum, m_trial.collected);
	const double base = std::max(m_trial.speed_before, 1.0);
	const double gain = (speed_after - m_trial.speed_before) / base;

	const bool improved = gain >= m_policy.gain_fraction - 1e-9;
	// From bad to fully good counts even when the gain is under the bar. A baseline that was already fast
	// and only had late frames does not get this for free: it needs the late frames gone.
	const bool was_good_speed = m_trial.speed_before >= m_policy.reached_speed_pct;
	const bool was_late = m_trial.late_before >= m_policy.late_frame_ratio_bad;
	const bool fixed = speed_after >= m_policy.reached_speed_pct && late_after < m_policy.late_frame_ratio_bad &&
	                   (!was_good_speed || was_late);

	AutoTuneAction action;
	action.lever = m_trial.lever;
	action.value = m_trial.value;
	action.bottleneck = m_last_bottleneck;
	action.speed_before = m_trial.speed_before;
	action.speed_after = speed_after;
	action.gain = gain;
	action.measured = true;

	if (improved || fixed)
	{
		action.kind = AutoTuneAction::Kind::Keep;
		// Nothing about the applied state changes for a keep, so only the hitch of persisting it is waited out.
		EnterSettle(Phase::SettlingAfter, m_policy.settle_seconds);
	}
	else
	{
		action.kind = AutoTuneAction::Kind::Revert;
		m_rejected.push_back({m_trial.lever, m_trial.value});
		EnterSettle(Phase::SettlingAfter, SettleSecondsFor(m_trial.lever));
	}

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

		case Phase::SettlingTrial:
			m_phase_seconds -= w.seconds;
			if (m_phase_seconds <= 0.0)
			{
				m_phase = Phase::Measuring;
				m_phase_seconds = 0.0;
			}
			return none;

		case Phase::Measuring:
			m_trial.speed_sum += w.speed_pct;
			m_trial.late_sum += w.late_frame_ratio;
			m_trial.collected++;
			if (m_trial.collected < m_policy.measure_windows)
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
