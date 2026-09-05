// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "EECycleRate.h"

#include "Achievements.h"
#include "Config.h"
#include "GSDumpReplayer.h"
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

EECycleRateEligibility EECycleRate::ResolveEligibility(const EligibilityInputs& in, EligibilityReasons* reasons)
{
	EligibilityReasons why;
	EECycleRateEligibility out;

	// The configured selector is the baseline whether the governor runs or not: it is
	// both the ceiling and what a restore goes back to.
	out.baseline = in.configured;

	// Precedence. A per-game dynamic key is the player answering this exact question
	// for this exact game, so it wins over everything, including a fixed-rate claim it
	// sits next to. Failing that, any fixed-rate claim — from the database or from the
	// per-game file — is a statement about this game's timing that a transient
	// underclock would contradict, so the governor stays out. Only when nobody has
	// claimed the rate does the global switch get to decide.
	if (in.pergame_claims_dynamic)
	{
		out.enabled = in.dynamic_setting;
		why.enabled = out.enabled ? "the per-game key opts this game in" : "the per-game key opts this game out";
	}
	else if (in.gamedb_set_rate)
	{
		out.enabled = false;
		why.enabled = "the game database sets this game's cycle rate";
	}
	else if (in.pergame_claims_rate)
	{
		out.enabled = false;
		why.enabled = "the per-game file sets this game's cycle rate";
	}
	else
	{
		out.enabled = in.dynamic_setting;
		why.enabled = out.enabled ? "on globally, and nothing claims this game's rate" : "off globally";
	}

	// Ordered so the reason names the thing a player would have to change first.
	if (!in.vm_running)
		why.eligible = "no VM is running";
	else if (in.hardcore)
		why.eligible = "hardcore mode forbids underclocking";
	else if (in.ee_cycle_skip != 0)
		why.eligible = "EE cycle skip is not zero";
	else if (!in.limiter_nominal)
		why.eligible = "the limiter is not at normal speed";
	else if (in.paced_by_host_vsync)
		why.eligible = "pacing comes from host vsync, not the limiter";
	else if (in.gs_dump_replay)
		why.eligible = "a GS dump is being replayed";
	else if (in.frame_advancing)
		why.eligible = "a frame advance is in progress";
	else
	{
		out.eligible = true;
		why.eligible = "nothing is suspending it";
	}

	if (reasons)
		*reasons = why;

	return out;
}

EECycleRate::EligibilityInputs EECycleRate::ReadEligibilityInputs()
{
	pxAssertMsg(VMManager::Internal::IsOnCPUThread(),
		"EECycleRate::ReadEligibilityInputs reads EmuConfig and VM state, which the CPU thread owns.");

	EligibilityInputs in;

	in.dynamic_setting = EmuConfig.Speedhacks.DynamicEECycleRate;
	in.pergame_claims_dynamic = EmuConfig.PerGameClaimsDynamicEECycleRate;
	in.pergame_claims_rate = EmuConfig.PerGameClaimsEECycleRate;
	in.gamedb_set_rate = EmuConfig.GameDBSetEECycleRate;
	in.configured = GetConfigured();

	in.hardcore = Achievements::IsHardcoreModeActive();
	in.ee_cycle_skip = EmuConfig.Speedhacks.EECycleSkip;
	in.limiter_nominal = (VMManager::GetLimiterMode() == LimiterModeType::Nominal);
	in.paced_by_host_vsync = VMManager::IsUsingVSyncForTiming();
	in.gs_dump_replay = GSDumpReplayer::IsReplayingDump();
	in.frame_advancing = VMManager::IsFrameAdvancing();
	in.vm_running = (VMManager::GetState() == VMState::Running);

	return in;
}

EECycleRateEligibility EECycleRate::ResolveEligibility()
{
	return ResolveEligibility(ReadEligibilityInputs());
}

void EECycleRate::LogResolvedEligibility()
{
	EligibilityReasons why;
	const EECycleRateEligibility e = ResolveEligibility(ReadEligibilityInputs(), &why);

	DevCon.WriteLn("EE cycle rate: dynamic is %s (%s); %s (%s); baseline %d",
		e.enabled ? "on" : "off", why.enabled,
		e.eligible ? "eligible" : "suspended", why.eligible,
		static_cast<int>(e.baseline));
}
