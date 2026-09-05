// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The decision half of the dynamic EE cycle-rate governor.
//
// This class is pure: no clock, no VM, no globals, no EmuConfig, no logging. It is
// fed one completed window sample plus the eligibility answer for that window, and
// returns at most one selector step. Everything it needs to know about the passage
// of time arrives as the target-time length of the windows it is given, so a test
// can drive years of governor behaviour from a fixed array of structs.
//
// That split is deliberate. The signal model and the actuation path are both hard
// to test in place: sampling needs a paced frontend and actuation needs a running
// recompiler. The hysteresis rules are the part that decides whether the feature
// oscillates, walks to the floor, or reacts to a bottleneck it cannot move, and
// those rules are exactly the part that can be settled offline.
//
// Bounds: the configured selector is the baseline and the upper bound. The
// governor only ever underclocks, by at most EECycleRatePolicy::max_steps_down
// steps, and never past the -3 end of the selector range.

#pragma once

#include "common/Pcsx2Defs.h"

// One completed sampling window, as measured on the CPU thread at the frame
// boundary. All the ratios are dimensionless; "target" is the frame deadline the
// limiter is pacing to, so a ratio of 1.0 means the work exactly filled its budget.
struct EECycleRateWindowSample
{
	// False means the sampler could not measure this window - the limiter was not
	// a nominal sleep-based one, execution was interrupted, a counter wrapped. An
	// invalid window clears both dwell counters and cannot cause a transition.
	bool valid = false;

	// Frames the window covers. Zero is treated as invalid: there is nothing to
	// take a percentile of.
	u32 frames = 0;

	// Window length measured in target (deadline) time, not wall time. This is the
	// only clock the controller has: warm-up, cooldown and the downshift inhibit are
	// all measured by summing this across valid windows.
	double target_seconds = 0.0;

	// p90 across the window's frames of (active wall time / target frame time).
	// Active wall time runs from the limiter's post-sleep return to the next frame
	// boundary, so it excludes the limiter sleep itself.
	float p90_active_ratio = 0;

	// Fraction of the window's frames whose active wall time exceeded target.
	float late_ratio = 0;

	// (Sum of active wall time - time blocked on the GS or VU1 threads) / sum of
	// target time. This is the "the EE thread is the cost" signal: a GS-bound frame
	// burns wall time without the EE having anything to give back.
	float own_time_ratio = 0;

	// CPU thread's own CPU time over the window / sum of target time. Cross-check
	// against preemption and host thermal throttling, which look like deadline
	// pressure but are not fixed by underclocking the guest.
	float cpu_time_ratio = 0;

	// Time blocked on the GS or VU1 threads / active wall time. Its own ceiling, so
	// a VU1 or GS bottleneck is not answered by slowing the EE down.
	float blocked_ratio = 0;
};

// The answer to "may the governor act at all right now", resolved by the caller
// after every settings, game-database and lifecycle mutation is complete.
struct EECycleRateEligibility
{
	// The dynamic setting as resolved for this game.
	bool enabled = false;

	// No suspending condition: nominal sleep-based limiter, EECycleSkip == 0, not
	// hardcore mode, not a GS dump replay, not frame-advancing.
	bool eligible = false;

	// The configured selector. The floor is max(-3, baseline - max_steps_down) and
	// the ceiling is the baseline itself.
	s8 baseline = 0;
};

enum class EECycleRateDecision : u8
{
	// Nothing to do this window.
	None,
	// Lower the effective selector by one step.
	StepDown,
	// Raise the effective selector by one step, toward the baseline.
	StepUp,
	// Go to the baseline now, in one act. Not one step: a two-step-down controller
	// that loses eligibility restores the baseline directly.
	RestoreBaseline,
};

// The KTD6 tuning constants, in one place so a trace and a test can name the same
// numbers. Change them from captured traces, not from taste.
struct EECycleRatePolicy
{
	// Target-time to sit out after a lifecycle reset, covering boot, state load and
	// JIT warm-up.
	double warmup_seconds = 2.0;

	// Target-time to sit out after any transition, covering the EE and microVU0
	// cache rebuild the transition costs.
	double cooldown_seconds = 2.0;

	// A downshift needs all four of these, together, for downshift_dwell windows.
	float overload_p90 = 0.95f;
	float overload_late = 0.20f;
	float min_own_time = 0.70f;
	float max_blocked = 0.25f;
	u32 downshift_dwell = 2;

	// An upshift needs both of these for upshift_dwell windows. It does not need
	// guest demand: returning to the player's configured rate is the neutral act.
	float headroom_p90 = 0.75f;
	u32 upshift_dwell = 4;

	// After a downshift's cooldown expires, the next effectiveness_windows valid
	// windows have to show a mean p90 at or below the pre-downshift p90 scaled by
	// (1 - effectiveness_margin). If they do not, that step bought nothing and it
	// is given back - one step, not the whole underclock, because any earlier step
	// was judged on its own evidence and passed.
	//
	// Giving it back also starts a downshift inhibit of ineffective_hold_seconds.
	// This is the guard against walking to the floor when the bottleneck is
	// somewhere the EE selector cannot reach.
	float effectiveness_margin = 0.05f;
	u32 effectiveness_windows = 2;
	double ineffective_hold_seconds = 10.0;

	// Steps below the baseline the governor may take.
	u32 max_steps_down = 2;
};

class EECycleRateController
{
public:
	enum class State : u8
	{
		// Not running: dynamic mode off, or suspended, or never started.
		Disabled,
		// Sitting out warmup_seconds of target time after a lifecycle reset.
		Warmup,
		// Classifying windows and counting dwell.
		Observe,
		// Sitting out cooldown_seconds of target time after a transition.
		Cooldown,
	};

	// Diagnostics only. A copy, so a caller can publish it without holding a
	// reference into controller state.
	struct Snapshot
	{
		State state;
		s8 baseline;
		s8 effective;
		u32 overload_dwell;
		u32 headroom_dwell;
		// Target-time accumulated in the current timed state (warm-up or cooldown).
		// Zero in Observe and Disabled.
		double clock_seconds;
		// Target-time still owed on the downshift inhibit. Not a state: it drains
		// through Observe and Cooldown alike, so an upshift taken while it is
		// running cannot shorten it.
		double downshift_inhibit_seconds;
		float last_p90;
		u32 transitions;
		u32 ineffective_count;
	};

	explicit EECycleRateController(const EECycleRatePolicy& policy = {});

	// Lifecycle reset: VM init, reset, shutdown, save-state load, settings apply,
	// limiter-mode change. Clears every counter, believes the baseline is applied,
	// and starts warm-up. The caller is expected to restore the baseline itself at
	// the same seam; the controller does not emit a decision from here.
	void Reset(s8 baseline);

	// One completed window. Returns at most one step.
	EECycleRateDecision Update(const EECycleRateWindowSample& sample, const EECycleRateEligibility& eligibility);

	// The integration reports what it actually applied. The controller updates its
	// belief optimistically when it returns a decision, so this is the correction
	// channel for a request that was dropped, clamped or coalesced.
	void OnApplied(s8 new_effective);

	// What the controller believes is applied.
	s8 GetEffective() const { return m_effective; }

	s8 GetBaseline() const { return m_baseline; }

	// Lowest selector this baseline allows: max(-3, baseline - max_steps_down).
	s8 GetFloor() const;

	State GetState() const { return m_state; }

	const EECycleRatePolicy& GetPolicy() const { return m_policy; }

	Snapshot GetSnapshot() const;

	// Whether a window is measurable at all. Exposed so a trace writer can label a
	// rejected window without duplicating the rules.
	static bool IsSampleUsable(const EECycleRateWindowSample& sample);

private:
	void ClearDwell();
	void ClearEffectiveness();
	void EnterWarmup();
	void EnterObserve();
	EECycleRateDecision GoToBaseline(State next_state);
	EECycleRateDecision UndoIneffectiveStep();

	EECycleRatePolicy m_policy;

	State m_state = State::Disabled;
	s8 m_baseline = 0;
	s8 m_effective = 0;

	u32 m_overload_dwell = 0;
	u32 m_headroom_dwell = 0;

	// Target-time accumulated in the current timed state.
	double m_clock_seconds = 0.0;

	// Armed by a downshift, evaluated once the cooldown expires.
	bool m_effectiveness_armed = false;
	float m_pre_downshift_p90 = 0;
	u32 m_effectiveness_seen = 0;
	double m_effectiveness_p90_sum = 0.0;

	// Target-time still owed before another downshift may be considered. Set by an
	// ineffective verdict, drained by every usable window in any state.
	double m_downshift_inhibit_seconds = 0.0;

	float m_last_p90 = 0;
	u32 m_transitions = 0;
	u32 m_ineffective_count = 0;
};
