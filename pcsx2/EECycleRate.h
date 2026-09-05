// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

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
} // namespace EECycleRate
