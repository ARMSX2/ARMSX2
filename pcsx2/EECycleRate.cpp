// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "EECycleRate.h"

#include "Config.h"
#include "R5900.h"
#include "VMManager.h"
#include "VUmicro.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <algorithm>

namespace
{
	// CPU-thread owned. Plain scalars: every writer and every reader that
	// matters is the CPU thread. Anything cross-thread (the overlay) gets a
	// published snapshot instead of reaching in here.
	s8  s_effective = 0;
	u32 s_transitions = 0;

	EECycleRate::ResetGenerations s_generations = {};
} // namespace

s8 EECycleRate::GetEffective()
{
	return s_effective;
}

s8 EECycleRate::GetConfigured()
{
	return EmuConfig.Speedhacks.EECycleRate;
}

void EECycleRate::SyncToConfigured()
{
	s_effective = GetConfigured();
}

s8 EECycleRate::GetFloor()
{
	return static_cast<s8>(std::max<int>(Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE, GetConfigured() - 2));
}

bool EECycleRate::ApplyEffective(s8 selector, const char* reason)
{
	pxAssertMsg(VMManager::Internal::IsOnCPUThread(),
		"EECycleRate::ApplyEffective must run on the CPU thread — it resets code caches.");

	const s8 configured = GetConfigured();
	if (selector > configured || selector < GetFloor())
	{
		DevCon.WriteLn("EE cycle rate: rejecting effective %d (configured %d, floor %d), reason '%s'",
			static_cast<int>(selector), static_cast<int>(configured), static_cast<int>(GetFloor()),
			reason ? reason : "");
		return false;
	}

	if (selector == s_effective)
		return true;

	const s8 old = s_effective;
	s_effective = selector;
	s_transitions++;

	// Order matters. recResetEE defers to the next safe execution boundary when
	// the EE is mid-execute; mVUreset is immediate. Between the two, a live EE
	// block that reaches VU0 does so through a C entry point, so it meets the
	// fresh mVU0 rather than a half-torn-down one.
	Cpu->Reset();
	CpuVU0->Reset();

	DevCon.WriteLn("EE cycle rate: effective %d -> %d (configured %d), reason '%s'",
		static_cast<int>(old), static_cast<int>(selector), static_cast<int>(configured),
		reason ? reason : "");
	return true;
}

u32 EECycleRate::GetTransitionCount()
{
	return s_transitions;
}

EECycleRate::ResetGenerations EECycleRate::GetResetGenerations()
{
	return s_generations;
}

void EECycleRate::NoteEeReset()
{
	s_generations.ee++;
}

void EECycleRate::NoteIopReset()
{
	s_generations.iop++;
}

void EECycleRate::NoteVuReset(int vu_index)
{
	if (vu_index == 0)
		s_generations.vu0++;
	else
		s_generations.vu1++;
}

void EECycleRate::NoteVifReset()
{
	s_generations.vif++;
}
