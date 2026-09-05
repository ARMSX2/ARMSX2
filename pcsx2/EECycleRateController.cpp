// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "EECycleRateController.h"

#include <algorithm>
#include <cmath>

namespace
{
	// The selector range the speedhack has always had.
	constexpr int kMinSelector = -3;
	constexpr int kMaxSelector = 3;

	// Ratios are dimensionless and mostly land under 2. This is not a plausibility
	// check, it is a "the sampler produced garbage" check: anything past it means a
	// counter wrapped or a divisor was near zero, and the window is thrown away
	// rather than repaired.
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

// A downshift that measurably bought nothing gives back one step, not the whole
// underclock: any step below it was judged on its own two windows and passed, and
// throwing those away would cost real time already proven to be there.
//
// The inhibit that follows is deliberately not a state. It drains through Observe
// and Cooldown alike, so the cooldown of this very upshift - or of a later one -
// cannot shorten it, and headroom keeps working throughout, so the controller can
// still climb back to the baseline while it runs.
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

	// Eligibility is the one thing that acts without a measurable window: whatever
	// suspended the governor - hardcore mode, turbo, a cycle-skip setting, a pause -
	// wants the player's configured rate back now, not two windows from now.
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
		// First eligible window of a session, or the first after a suspension.
		m_baseline = baseline;
		m_effective = baseline;
		ClearDwell();
		ClearEffectiveness();
		m_downshift_inhibit_seconds = 0.0;
		EnterWarmup();
	}
	else if (baseline != m_baseline)
	{
		// The configured rate moved underneath us - a settings apply, a game change.
		// The old effective selector is a decision about a baseline that no longer
		// exists, so it goes back and the evidence starts over.
		m_baseline = baseline;
		if (m_effective != m_baseline)
			return GoToBaseline(State::Warmup);

		ClearDwell();
		ClearEffectiveness();
		m_downshift_inhibit_seconds = 0.0;
		EnterWarmup();
	}

	// A window we could not measure is not evidence either way. It cannot move the
	// selector, and it advances none of the clocks - not the warm-up, not the
	// cooldown, not the downshift inhibit - but it does break a dwell run: two
	// overloaded windows either side of a gap are not two consecutive ones.
	if (!IsSampleUsable(sample))
	{
		ClearDwell();
		return EECycleRateDecision::None;
	}

	m_last_p90 = sample.p90_active_ratio;

	// The inhibit is read before it is drained, so the window that spends the last
	// of it is still inhibited - the same rule the timed states use, where the
	// window that fills the cooldown is consumed by it.
	const bool downshift_inhibited = m_downshift_inhibit_seconds > 0.0;
	if (downshift_inhibited)
		m_downshift_inhibit_seconds = std::max(0.0, m_downshift_inhibit_seconds - sample.target_seconds);

	// The timed states sit windows out entirely.
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

	// Did the last downshift buy anything? This runs before classification, so a
	// step that did not help is undone rather than followed by another one.
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

	// Four gates, and a downshift needs all four open together. Each of the last
	// three is a way an earlier attempt at this feature went wrong: host load that
	// was not deadline pressure, deadline pressure the EE thread did not cause, and
	// a GS or VU1 bottleneck answered by slowing the EE down.
	const bool over_deadline = sample.p90_active_ratio >= m_policy.overload_p90;
	const bool missing_frames = sample.late_ratio >= m_policy.overload_late;
	const bool ee_is_the_cost = sample.own_time_ratio >= m_policy.min_own_time;
	const bool not_blocked_elsewhere = sample.blocked_ratio <= m_policy.max_blocked;
	const bool overload = over_deadline && missing_frames && ee_is_the_cost && not_blocked_elsewhere;
	const bool headroom = sample.p90_active_ratio <= m_policy.headroom_p90 && sample.late_ratio <= 0.0f;

	// An overloaded window during the inhibit is not counted against the downshift
	// dwell - the last step already answered this classification and it did not
	// work. It falls through to the decay path instead. Headroom is untouched, so
	// the way back to the baseline stays open the whole time.
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
		// The deadband. Not evidence against the run, just a missing vote, so the
		// counters decay instead of clearing - that is what stops a workload sitting
		// on a threshold from alternating between the two decisions.
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
