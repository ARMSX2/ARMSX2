// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "EECycleRateController.h"

#include <algorithm>
#include <cmath>

namespace
{
	// Selector range of the EE cycle-rate speedhack.
	constexpr int kMinSelector = -3;
	constexpr int kMaxSelector = 3;

	// Ratios are dimensionless and normally under 2. A ratio past this means a counter
	// wrapped or a divisor was near zero; the window is discarded, not repaired.
	constexpr float kMaxRatio = 1.0e4f;

	s8 ClampSelector(s8 selector)
	{
		return static_cast<s8>(std::clamp<int>(selector, kMinSelector, kMaxSelector));
	}
} // namespace

EECycleRateController::EECycleRateController(const EECycleRatePolicy& policy)
	: m_policy(policy)
{
}

void EECycleRateController::Reset(s8 baseline)
{
	m_baseline = ClampSelector(baseline);
	m_effective = m_baseline;
	ClearDwell();
	ClearEffectiveness();
	m_downshift_inhibit_seconds = 0.0;
	m_last_p90 = 0;
	m_transitions = 0;
	m_ineffective_count = 0;
	EnterWarmup();
}

void EECycleRateController::OnApplied(s8 new_effective)
{
	m_effective = static_cast<s8>(std::clamp<int>(new_effective, GetFloor(), m_baseline));
}

s8 EECycleRateController::GetFloor() const
{
	const int floor = std::max<int>(kMinSelector, static_cast<int>(m_baseline) - static_cast<int>(m_policy.max_steps_down));
	return static_cast<s8>(std::min<int>(floor, m_baseline));
}

EECycleRateController::Snapshot EECycleRateController::GetSnapshot() const
{
	Snapshot s;
	s.state = m_state;
	s.baseline = m_baseline;
	s.effective = m_effective;
	s.overload_dwell = m_overload_dwell;
	s.headroom_dwell = m_headroom_dwell;
	s.clock_seconds = m_clock_seconds;
	s.downshift_inhibit_seconds = m_downshift_inhibit_seconds;
	s.last_p90 = m_last_p90;
	s.transitions = m_transitions;
	s.ineffective_count = m_ineffective_count;
	return s;
}

bool EECycleRateController::IsSampleUsable(const EECycleRateWindowSample& sample)
{
	if (!sample.valid || sample.frames == 0)
		return false;

	if (!std::isfinite(sample.target_seconds) || sample.target_seconds <= 0.0)
		return false;

	const float ratios[] = {
		sample.p90_active_ratio,
		sample.late_ratio,
		sample.own_time_ratio,
		sample.cpu_time_ratio,
		sample.blocked_ratio,
	};

	for (const float ratio : ratios)
	{
		if (!std::isfinite(ratio) || ratio < 0.0f || ratio > kMaxRatio)
			return false;
	}

	return true;
}

void EECycleRateController::ClearDwell()
{
	m_overload_dwell = 0;
	m_headroom_dwell = 0;
}

void EECycleRateController::ClearEffectiveness()
{
	m_effectiveness_armed = false;
	m_pre_downshift_p90 = 0;
	m_effectiveness_seen = 0;
	m_effectiveness_p90_sum = 0.0;
}

void EECycleRateController::EnterWarmup()
{
	m_state = State::Warmup;
	m_clock_seconds = 0.0;
}

void EECycleRateController::EnterObserve()
{
	m_state = State::Observe;
	m_clock_seconds = 0.0;
	ClearDwell();
}

EECycleRateDecision EECycleRateController::GoToBaseline(State next_state)
{
	m_effective = m_baseline;
	++m_transitions;
	ClearDwell();
	ClearEffectiveness();
	m_downshift_inhibit_seconds = 0.0;
	m_state = next_state;
	m_clock_seconds = 0.0;
	return EECycleRateDecision::RestoreBaseline;
}

// Gives back one step, not the whole underclock: earlier steps were each judged on
// their own windows and passed.
//
// The downshift inhibit that starts here is not a state. It drains through Observe
// and Cooldown alike, so no cooldown can shorten it, and headroom still works while
// it runs.
EECycleRateDecision EECycleRateController::UndoIneffectiveStep()
{
	++m_ineffective_count;
	m_downshift_inhibit_seconds = m_policy.ineffective_hold_seconds;
	ClearDwell();

	if (m_effective >= m_baseline)
		return EECycleRateDecision::None;

	++m_effective;
	++m_transitions;
	m_state = State::Cooldown;
	m_clock_seconds = 0.0;
	return EECycleRateDecision::StepUp;
}

EECycleRateDecision EECycleRateController::Update(const EECycleRateWindowSample& sample, const EECycleRateEligibility& eligibility)
{
	const s8 baseline = ClampSelector(eligibility.baseline);

	// Loss of eligibility (hardcore mode, turbo, a cycle-skip setting, a pause) acts
	// without waiting for a measurable window: the configured rate is restored now.
	if (!eligibility.enabled || !eligibility.eligible)
	{
		if (m_effective != m_baseline)
			return GoToBaseline(State::Disabled);

		ClearDwell();
		ClearEffectiveness();
		m_downshift_inhibit_seconds = 0.0;
		m_state = State::Disabled;
		m_clock_seconds = 0.0;
		return EECycleRateDecision::None;
	}

	if (m_state == State::Disabled)
	{
		m_baseline = baseline;
		m_effective = baseline;
		ClearDwell();
		ClearEffectiveness();
		m_downshift_inhibit_seconds = 0.0;
		EnterWarmup();
	}
	else if (baseline != m_baseline)
	{
		// The configured rate changed (settings apply, game change). The effective
		// selector was chosen for the old baseline, so restore and start over.
		m_baseline = baseline;
		if (m_effective != m_baseline)
			return GoToBaseline(State::Warmup);

		ClearDwell();
		ClearEffectiveness();
		m_downshift_inhibit_seconds = 0.0;
		EnterWarmup();
	}

	// An unmeasurable window is no evidence either way: it cannot move the selector
	// and advances no clock (warm-up, cooldown, downshift inhibit), but it does break
	// a dwell run, since windows either side of a gap are not consecutive.
	if (!IsSampleUsable(sample))
	{
		ClearDwell();
		return EECycleRateDecision::None;
	}

	m_last_p90 = sample.p90_active_ratio;

	// Read before draining, so the window that spends the last of the inhibit is
	// still inhibited, as the window that fills a cooldown is consumed by it.
	const bool downshift_inhibited = m_downshift_inhibit_seconds > 0.0;
	if (downshift_inhibited)
		m_downshift_inhibit_seconds = std::max(0.0, m_downshift_inhibit_seconds - sample.target_seconds);

	if (m_state != State::Observe)
	{
		double limit = 0.0;
		switch (m_state)
		{
			case State::Warmup:
				limit = m_policy.warmup_seconds;
				break;
			case State::Cooldown:
				limit = m_policy.cooldown_seconds;
				break;
			default:
				break;
		}

		m_clock_seconds += sample.target_seconds;
		if (m_clock_seconds >= limit)
			EnterObserve();

		return EECycleRateDecision::None;
	}

	// Runs before classification so an ineffective downshift is undone rather than
	// followed by another one.
	if (m_effectiveness_armed)
	{
		++m_effectiveness_seen;
		m_effectiveness_p90_sum += static_cast<double>(sample.p90_active_ratio);

		if (m_effectiveness_seen >= m_policy.effectiveness_windows)
		{
			const double mean = m_effectiveness_p90_sum / static_cast<double>(m_effectiveness_seen);
			const double wanted = static_cast<double>(m_pre_downshift_p90) * (1.0 - static_cast<double>(m_policy.effectiveness_margin));
			ClearEffectiveness();

			if (mean > wanted)
				return UndoIneffectiveStep();
		}
	}

	// A downshift needs all four gates open. The last three exclude host load that is
	// not deadline pressure, pressure the EE thread did not cause, and a GS or VU1
	// bottleneck that slowing the EE cannot fix.
	const bool over_deadline = sample.p90_active_ratio >= m_policy.overload_p90;
	const bool missing_frames = sample.late_ratio >= m_policy.overload_late;
	const bool ee_is_the_cost = sample.own_time_ratio >= m_policy.min_own_time;
	const bool not_blocked_elsewhere = sample.blocked_ratio <= m_policy.max_blocked;
	const bool overload = over_deadline && missing_frames && ee_is_the_cost && not_blocked_elsewhere;
	const bool headroom = sample.p90_active_ratio <= m_policy.headroom_p90 && sample.late_ratio <= 0.0f;

	// An overloaded window during the inhibit does not count toward the downshift
	// dwell (the last step already answered it and did not help); it falls through
	// to the decay path. Headroom is unaffected, so the way back to the baseline
	// stays open.
	if (overload && !downshift_inhibited)
	{
		m_headroom_dwell = 0;
		if (m_overload_dwell < m_policy.downshift_dwell)
			++m_overload_dwell;
	}
	else if (headroom && !overload)
	{
		m_overload_dwell = 0;
		if (m_headroom_dwell < m_policy.upshift_dwell)
			++m_headroom_dwell;
	}
	else
	{
		// Deadband: a missing vote, not evidence against the run. The counters decay
		// rather than clear so a workload sitting on a threshold does not flip-flop.
		if (m_overload_dwell > 0)
			--m_overload_dwell;
		if (m_headroom_dwell > 0)
			--m_headroom_dwell;
	}

	if (m_overload_dwell >= m_policy.downshift_dwell && m_effective > GetFloor())
	{
		--m_effective;
		++m_transitions;
		ClearDwell();
		ClearEffectiveness();
		if (m_policy.effectiveness_windows > 0)
		{
			m_effectiveness_armed = true;
			m_pre_downshift_p90 = sample.p90_active_ratio;
		}
		m_state = State::Cooldown;
		m_clock_seconds = 0.0;
		return EECycleRateDecision::StepDown;
	}

	if (m_headroom_dwell >= m_policy.upshift_dwell && m_effective < m_baseline)
	{
		++m_effective;
		++m_transitions;
		ClearDwell();
		ClearEffectiveness();
		m_state = State::Cooldown;
		m_clock_seconds = 0.0;
		return EECycleRateDecision::StepUp;
	}

	return EECycleRateDecision::None;
}
