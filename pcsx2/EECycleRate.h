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
// In static mode the two are equal by construction, so nothing changes: no
// extra load, branch or multiply reaches generated code, and the emitted charge
// sequence for a given selector is byte-identical to what it was before this
// split existed.
//
// A dynamic governor (later work) may lower EFFECTIVE below CONFIGURED to buy
// back host time on a machine that cannot hold the frame deadline. It may never
// raise it above CONFIGURED, and it may never write back into EmuConfig — a
// transient decision is not user intent and must not be persisted.
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
	// resets the EE recompiler and microVU0 — in that order, because the EE
	// reset defers itself to the next safe execution boundary while mVU0's is
	// immediate, and an EE block that calls into mVU0 in between only ever does
	// so through a C entry point.
	//
	// Nothing else is reset: not the IOP recompiler, not microVU1, not the VIF
	// unpack dynarec. None of them bake the EE selector into generated code.
	bool ApplyEffective(s8 selector, const char* reason);

	// How many times ApplyEffective has actually moved the selector this session.
	u32 GetTransitionCount();

	// What the last transition that actually moved the selector cost, in the
	// units GetCPUTicks() counts (divide by GetTickFrequency() for seconds).
	//
	// Split three ways because only one of the three is a candidate for
	// removal. The EE recompiler's reset rewrites an 88 MiB block-pointer LUT
	// and memsets a 32 MiB RAM snapshot; microVU0's reset is program-count
	// work with no bulk memset. A design that patches the EE charge immediates
	// in place instead of resetting has to know which of the two the measured
	// transition cost actually is.
	//
	// cpu_reset is the wall time of the `Cpu->Reset()` CALL, which is not the
	// same thing as the EE rebuild: recResetEE defers to the next safe
	// execution boundary when the EE is mid-execute, in which case the call
	// returns in microseconds and the rebuild happens later. ee_rebuild is the
	// EE recompiler's own measurement of the rebuild itself, whenever it last
	// ran; ee_rebuild_generation is the value of GetResetGenerations().ee at
	// that moment, so a reader can tell whether the rebuild it is looking at
	// belongs to this transition or to an earlier one.
	struct TransitionCost
	{
		u64 cpu_reset = 0;              // Cpu->Reset()
		u64 vu0_reset = 0;              // CpuVU0->Reset()
		u64 ee_rebuild = 0;             // the EE recompiler rebuild itself
		u32 ee_rebuild_generation = 0;  // which EE reset ee_rebuild measured
	};

	// Which way the last transition that moved the selector actually went.
	// The immediate-patching pass repairs the EE recompiler's baked charge
	// immediates in place; when it cannot, the caller falls back to the reset
	// this whole struct was written to measure.
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
		// the number that made it refuse, which is the only way to tell a cap
		// refusal apart from every other reason without a debugger.
		u32 blocks_unpatchable = 0;
	};

	TransitionCost GetLastTransitionCost();
	TransitionPath GetLastTransitionPath();

	// Published by the EE recompiler's patch pass at every exit, taken or not.
	// Separate from the cost struct because it is the recompiler that knows the
	// reason, and because a refused pass still has one worth reporting.
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

	// Called from the provider resets themselves. Trivially cheap and nowhere
	// near a hot path — a code-cache reset is a once-per-second event at worst.
	void NoteEeReset();
	void NoteIopReset();
	void NoteVuReset(int vu_index);
	void NoteVifReset();

	// Everything the "may the governor act at all" answer depends on, in one struct.
	//
	// It is a struct rather than a pile of reads inside the resolver because the rules
	// are the part worth testing and the VM is the part that makes testing them
	// impossible: hardcore mode, the limiter, a GS dump replay and a frame-advance
	// request are all global runtime state a unit test cannot stand up. Split this way
	// the rules are a pure function over thirteen fields, and ReadEligibilityInputs()
	// is the only piece that needs a running emulator.
	struct EligibilityInputs
	{
		// --- who chose the rate, and whether the governor was asked for ---

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

		// --- conditions that suspend the governor whatever the setting says ---

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

	// Why the resolver answered the way it did, for the log and for a test that wants
	// to prove which rule fired rather than only that the answer was right.
	struct EligibilityReasons
	{
		// Which precedence rule decided `enabled`.
		const char* enabled = "";
		// The first suspending condition found, or why there was none.
		const char* eligible = "";
	};

	// The rules, as a pure function. See KTD7: precedence is per-game dynamic key
	// first, then any fixed-rate claim (database or per-game), then the global switch.
	// Never value comparison — equal values can come from different layers.
	EECycleRateEligibility ResolveEligibility(const EligibilityInputs& in, EligibilityReasons* reasons = nullptr);

	// Reads the live config and VM state. CPU thread only, and only valid once every
	// settings and game-database mutation for this apply is complete.
	EligibilityInputs ReadEligibilityInputs();

	// ReadEligibilityInputs() + ResolveEligibility().
	EECycleRateEligibility ResolveEligibility();

	// Resolve and write one DevCon line saying what came out and why. Call once per
	// settings apply; the governor itself resolves per window and does not log.
	void LogResolvedEligibility();
} // namespace EECycleRate
