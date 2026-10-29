// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "EECycleRateController.h"

#include "common/Pcsx2Types.h"

// The EE cycle-rate selector has two values, not one.
//
//   CONFIGURED is EmuConfig.Speedhacks.EECycleRate, the fully resolved static
//   value after global settings, the Game Database and per-game overrides. It
//   is what the settings UI, the "you have speedhacks on" warning and the
//   overlay report, and it is user intent.
//
//   EFFECTIVE is what the emulator actually runs at. It is owned by the CPU
//   thread and is the value every runtime consumer reads: the EE interpreter's
//   per-block charge, the EE recompiler's baked-in charge immediate, the VU0
//   interpreter's and microVU0's nominal-rate adjustment, and microVU's
//   generated-code option sentinel.
//
// In static mode the two are equal, so no extra load, branch or multiply
// reaches generated code.
//
// A dynamic governor may lower EFFECTIVE below CONFIGURED to buy back host time
// on a machine that cannot hold the frame deadline. It never raises it above
// CONFIGURED and never writes back into EmuConfig: a transient decision is not
// user intent and must not be persisted.
namespace EECycleRate
{
	// The value all runtime consumers read. CPU-thread owned.
	s8 GetEffective();

	// EmuConfig.Speedhacks.EECycleRate — the configured baseline.
	s8 GetConfigured();

	// Effective := configured, with no cache reset. Call this wherever settings
	// are (re)applied and the caches are being thrown away anyway, so a settings
	// reload can never leave a stale effective value behind.
	void SyncToConfigured();

	// The floor a dynamic selector may reach: two steps below configured, never
	// past the -3 the selector itself bottoms out at.
	s8 GetFloor();

	// Set the effective selector and rebuild only the generated code whose
	// constants depend on it. CPU thread only.
	//
	// Rejects (returns false, resets nothing) a selector outside
	// [GetFloor(), GetConfigured()]. A selector equal to the current one
	// returns true and resets nothing. Otherwise it stores the new value, then
	// updates the EE recompiler (patching its charge immediates in place when it
	// can, else resetting it) and resets microVU0. The order matters when the EE
	// reset is taken: it defers itself to the next safe execution boundary while
	// mVU0's is immediate, and an EE block that calls into mVU0 in between only
	// ever does so through a C entry point.
	//
	// Nothing else is reset: not the IOP recompiler, not microVU1, not the VIF
	// unpack dynarec. None of them bake the EE selector into generated code.
	bool ApplyEffective(s8 selector, const char* reason);

	// How many times ApplyEffective has actually moved the selector this session.
	u32 GetTransitionCount();

	// What the last transition that actually moved the selector cost, in the
	// units GetCPUTicks() counts (divide by GetTickFrequency() for seconds).
	//
	// Split three ways because the EE recompiler's reset is bulk work (a
	// block-pointer LUT and a RAM snapshot) while microVU0's is program-count
	// work, and a caller deciding whether to avoid the EE reset needs to know
	// which one dominates.
	//
	// cpu_reset is the wall time of the `Cpu->Reset()` call, which is not the
	// same thing as the EE rebuild: recResetEE defers to the next safe
	// execution boundary when the EE is mid-execute, in which case the call
	// returns in microseconds and the rebuild happens later. ee_rebuild is the
	// EE recompiler's own measurement of the rebuild itself, whenever it last
	// ran; ee_rebuild_generation is GetResetGenerations().ee at that moment, so
	// a reader can tell whether the rebuild belongs to this transition or to an
	// earlier one.
	struct TransitionCost
	{
		u64 cpu_reset = 0;              // Cpu->Reset()
		u64 vu0_reset = 0;              // CpuVU0->Reset()
		u64 ee_rebuild = 0;             // the EE recompiler rebuild itself
		u32 ee_rebuild_generation = 0;  // which EE reset ee_rebuild measured
	};

	// Which way the last transition that moved the selector went. The patch pass
	// repairs the EE recompiler's baked charge immediates in place; when it
	// cannot, the caller falls back to a reset.
	struct TransitionPath
	{
		bool patched = false;       // true: the EE recompiler was NOT reset
		const char* reason = "";    // what the pass did, or why it would not
		u64 ticks = 0;              // duration of the patch pass itself
		u32 sites = 0;              // charge immediates rewritten
		// Recorded sites the pass stepped over because their code is orphaned:
		// the block was removed since, and the word the record points at is the
		// redirect stub that removal wrote, not a charge. Only a site AT a
		// block's entry can end up in this state, which is why it is recognised
		// rather than treated as the recompiler contradicting itself.
		u32 sites_orphaned = 0;
		u32 blocks_invalidated = 0; // blocks thrown away instead of patched
		// Blocks the pass found unrepairable, whether or not it went ahead.
		// Same as blocks_invalidated on a pass that ran; on a refused one it is
		// the count that made it refuse.
		u32 blocks_unpatchable = 0;
	};

	TransitionCost GetLastTransitionCost();
	TransitionPath GetLastTransitionPath();

	// Published by the EE recompiler's patch pass at every exit, taken or not;
	// a refused pass still reports its reason.
	void NoteEePatchPass(const TransitionPath& path);

	// Published by the EE recompiler's own reset with the duration it measured
	// for itself. Separate from NoteEeReset() because the x86 recompiler bumps
	// the generation counter without measuring anything.
	void NoteEeRebuildCost(u64 ticks);

	// Per-provider code-cache reset counts, so a test or a diagnostic run can
	// assert that a rate transition touched the EE and VU0 caches and nothing
	// else. Plain CPU-thread counters bumped from each provider's reset.
	struct ResetGenerations
	{
		u32 ee;
		u32 iop;
		u32 vu0;
		u32 vu1;
		u32 vif;
	};

	ResetGenerations GetResetGenerations();

	// Called from each provider's reset.
	void NoteEeReset();
	void NoteIopReset();
	void NoteVuReset(int vu_index);
	void NoteVifReset();

	// Everything the "may the governor act at all" answer depends on, in one struct.
	// Hardcore mode, the limiter, a GS dump replay and a frame-advance request are global
	// runtime state a unit test cannot stand up, so the rules are a pure function over these
	// fields and ReadEligibilityInputs() is the only piece that needs a running emulator.
	struct EligibilityInputs
	{
		// EmuConfig.Speedhacks.DynamicEECycleRate as finally resolved: the global
		// value, overlaid by whatever the per-game file says.
		bool dynamic_setting = false;

		// The per-game file holds DynamicEECycleRate. Presence, not value: the whole
		// point is that a key equal to the global value is still a decision.
		bool pergame_claims_dynamic = false;

		// The per-game file holds EECycleRate. Presence, not value, same reason.
		bool pergame_claims_rate = false;

		// The game database set EECycleRate for this game.
		bool gamedb_set_rate = false;

		// EmuConfig.Speedhacks.EECycleRate — the baseline, and the ceiling.
		s8 configured = 0;

		// Hardcore mode forbids underclocking, which is the only thing the governor
		// does. The settings clamp already forces the switch off; this is the second
		// line, because the switch and the clamp are applied at different times.
		bool hardcore = false;

		// A nonzero cycle skip is a second timing knob acting on the same guest, and
		// v1 does not try to classify the interaction.
		u8 ee_cycle_skip = 0;

		// Turbo, slow-motion and unlimited all move the deadline the governor measures
		// against, so there is nothing to hold.
		bool limiter_nominal = false;

		// Host-vsync pacing: Throttle() returns before it measures or waits, so there
		// is no window to sample.
		bool paced_by_host_vsync = false;

		bool gs_dump_replay = false;
		bool frame_advancing = false;
		bool vm_running = false;
	};

	// Why the resolver answered the way it did, for the log and for tests that check
	// which rule fired.
	struct EligibilityReasons
	{
		// Which precedence rule decided `enabled`.
		const char* enabled = "";
		// The first suspending condition found, or why there was none.
		const char* eligible = "";
	};

	// The rules, as a pure function. Precedence is per-game dynamic key first, then any
	// fixed-rate claim (database or per-game), then the global switch. Never value
	// comparison: equal values can come from different layers.
	EECycleRateEligibility ResolveEligibility(const EligibilityInputs& in, EligibilityReasons* reasons = nullptr);

	// Reads the live config and VM state. CPU thread only, and only valid once every
	// settings and game-database mutation for this apply is complete.
	EligibilityInputs ReadEligibilityInputs();

	EECycleRateEligibility ResolveEligibility();

	// Where the governor stands right now, for other consumers. Resolved afresh on each
	// call from the live config and VM state; CPU thread only.
	//
	// `enabled` is whether it was asked for in this game, `eligible` is whether nothing is
	// suspending it at the moment, and `active` is both. The reasons are static strings
	// naming the rule that decided each answer, the same ones the log line carries.
	struct GovernorStatus
	{
		bool enabled = false;
		bool eligible = false;
		bool active = false;
		s8 configured = 0;
		s8 effective = 0;
		s8 floor = 0;
		const char* enabled_reason = "";
		const char* eligible_reason = "";
	};

	GovernorStatus GetGovernorStatus();

	// The same answer as a pure function of the inputs and the effective selector, so the
	// mapping can be tested without a VM.
	GovernorStatus ResolveGovernorStatus(const EligibilityInputs& in, s8 effective);

	// Resolve and write one DevCon line saying what came out and why. Call once per
	// settings apply; the governor itself resolves per window and does not log.
	void LogResolvedEligibility();
} // namespace EECycleRate
