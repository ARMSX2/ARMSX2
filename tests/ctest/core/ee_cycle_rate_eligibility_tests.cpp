// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Who gets to decide the EE cycle rate, and when the dynamic governor is allowed to
// touch it at all.
//
// The rule these tests exist to pin is that a claim is KEY PRESENCE, never value
// equality. A per-game EECycleRate of 0 and a global EECycleRate of 0 are the same
// integer and completely different statements: one is a player saying this game runs
// at stock timing, the other is a player who never thought about it. Deciding by
// comparing the two numbers gets it wrong in exactly the case that matters, and it
// gets it wrong silently — the governor underclocks a game somebody had already
// pinned, and all that shows up is a compatibility report nobody can reproduce.
//
// EECycleRate::ResolveEligibility() takes a plain input struct for the same reason
// the controller is clock-free: hardcore mode, the limiter, a GS dump replay and a
// frame-advance request are global runtime state, and a unit test that had to stand
// up a VM to reach them would test none of this.

#include "EECycleRate.h"

#include "Config.h"
#include "GameDatabase.h"
#include "PerGameOverrides.h"

#include "common/MemorySettingsInterface.h"

#include <gtest/gtest.h>

namespace
{

	using Inputs = EECycleRate::EligibilityInputs;
	using Reasons = EECycleRate::EligibilityReasons;

	// A VM running normally with nothing suspending the governor, and nobody claiming
	// the rate. Every test below starts here and changes the one thing it is about, so
	// a failure names the field that caused it.
	Inputs Healthy()
	{
		Inputs in;
		in.dynamic_setting = false;
		in.pergame_claims_dynamic = false;
		in.pergame_claims_rate = false;
		in.gamedb_set_rate = false;
		in.configured = 0;

		in.hardcore = false;
		in.ee_cycle_skip = 0;
		in.limiter_nominal = true;
		in.paced_by_host_vsync = false;
		in.gs_dump_replay = false;
		in.frame_advancing = false;
		in.vm_running = true;
		return in;
	}

	// Stands in for a per-game INI, same class the real overrides are computed from.
	class GameFile
	{
	public:
		GameFile& Set(const char* section, const char* key, int value)
		{
			m_si.SetIntValue(section, key, value);
			return *this;
		}

		GameFile& Set(const char* section, const char* key, bool value)
		{
			m_si.SetBoolValue(section, key, value);
			return *this;
		}

		PerGameOverrides Overrides() const { return ComputePerGameOverrides(m_si); }

	private:
		MemorySettingsInterface m_si;
	};

	// The two claim bits as the real settings path derives them: from what the file
	// holds, not from what the values turned out to be.
	void ApplyClaims(Inputs& in, const PerGameOverrides& ov)
	{
		in.pergame_claims_rate = ov.Has(SpeedHack::EECycleRate);
		in.pergame_claims_dynamic = ov.Has(SpeedHack::DynamicEECycleRate);
	}

} // namespace

// ------------------------------------------------------------------- precedence

TEST(EECycleRateEligibility, AMissingKeyLeavesTheGovernorOff)
{
	const EECycleRateEligibility e = EECycleRate::ResolveEligibility(Healthy());

	EXPECT_FALSE(e.enabled);
	EXPECT_TRUE(e.eligible) << "nothing is suspending it; it was simply never asked for";
	EXPECT_EQ(e.baseline, 0);
}

TEST(EECycleRateEligibility, TheGlobalSwitchDecidesWhenNobodyClaimsTheRate)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;

	const EECycleRateEligibility e = EECycleRate::ResolveEligibility(in);

	EXPECT_TRUE(e.enabled);
	EXPECT_TRUE(e.eligible);
}

TEST(EECycleRateEligibility, AGameDatabaseRateKeepsTheSelectorFixed)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.gamedb_set_rate = true;
	in.configured = -1;

	Reasons why;
	const EECycleRateEligibility e = EECycleRate::ResolveEligibility(in, &why);

	EXPECT_FALSE(e.enabled) << "the database staked a compatibility claim on this game's timing";
	EXPECT_STREQ(why.enabled, "the game database sets this game's cycle rate");
	EXPECT_TRUE(e.eligible) << "fixed is not the same as suspended";
	EXPECT_EQ(e.baseline, -1);
}

// The whole reason provenance is tracked separately rather than inferred. A per-game
// key holding the same number as the global setting is still a decision, and a value
// comparison cannot see it.
TEST(EECycleRateEligibility, APerGameRateEqualToTheGlobalOneIsStillAClaim)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.configured = 0; // and the global EECycleRate is 0 as well

	ApplyClaims(in, GameFile().Set("EmuCore/Speedhacks", "EECycleRate", 0).Overrides());
	ASSERT_TRUE(in.pergame_claims_rate) << "presence, not value, is what makes it a claim";

	Reasons why;
	const EECycleRateEligibility e = EECycleRate::ResolveEligibility(in, &why);

	EXPECT_FALSE(e.enabled);
	EXPECT_STREQ(why.enabled, "the per-game file sets this game's cycle rate");
}

TEST(EECycleRateEligibility, APerGameDynamicKeyOutranksAFixedRateClaim)
{
	Inputs in = Healthy();
	// Global off, and the player pinned a rate for this game as well — both of which
	// would say no on their own. The per-game dynamic key beats both, because it is
	// this player answering this question for this game.
	in.dynamic_setting = true;
	in.configured = -1;
	in.gamedb_set_rate = true;

	ApplyClaims(in, GameFile()
						.Set("EmuCore/Speedhacks", "EECycleRate", -1)
						.Set("EmuCore/Speedhacks", "DynamicEECycleRate", true)
						.Overrides());

	const EECycleRateEligibility e = EECycleRate::ResolveEligibility(in);

	EXPECT_TRUE(e.enabled);
	EXPECT_EQ(e.baseline, -1) << "the resolved per-game rate is the baseline, and the ceiling";
}

TEST(EECycleRateEligibility, APerGameDynamicFalseBeatsTheGlobalTrue)
{
	Inputs in = Healthy();
	in.dynamic_setting = false; // what the per-game file resolved to
	ApplyClaims(in, GameFile().Set("EmuCore/Speedhacks", "DynamicEECycleRate", false).Overrides());

	Reasons why;
	const EECycleRateEligibility e = EECycleRate::ResolveEligibility(in, &why);

	EXPECT_FALSE(e.enabled);
	EXPECT_STREQ(why.enabled, "the per-game key opts this game out");
}

TEST(EECycleRateEligibility, TheBaselineIsAlwaysTheConfiguredSelector)
{
	for (int rate = Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE;
		rate <= Pcsx2Config::SpeedhackOptions::MAX_EE_CYCLE_RATE; rate++)
	{
		Inputs in = Healthy();
		in.configured = static_cast<s8>(rate);
		EXPECT_EQ(EECycleRate::ResolveEligibility(in).baseline, rate);
	}
}

// ------------------------------------------------------------------- suspension

TEST(EECycleRateEligibility, HardcoreModeSuspendsTheGovernor)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.hardcore = true;

	Reasons why;
	const EECycleRateEligibility e = EECycleRate::ResolveEligibility(in, &why);

	EXPECT_FALSE(e.eligible);
	EXPECT_STREQ(why.eligible, "hardcore mode forbids underclocking");
}

TEST(EECycleRateEligibility, ANonzeroCycleSkipSuspendsTheGovernor)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.ee_cycle_skip = 1;

	Reasons why;
	const EECycleRateEligibility e = EECycleRate::ResolveEligibility(in, &why);

	EXPECT_TRUE(e.enabled) << "the setting is still on; it is the moment that is wrong";
	EXPECT_FALSE(e.eligible);
	EXPECT_STREQ(why.eligible, "EE cycle skip is not zero");
}

TEST(EECycleRateEligibility, AnythingButTheNormalLimiterSuspendsTheGovernor)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.limiter_nominal = false; // turbo, slow motion or unlimited

	EXPECT_FALSE(EECycleRate::ResolveEligibility(in).eligible);
}

TEST(EECycleRateEligibility, HostVSyncPacingSuspendsTheGovernor)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.paced_by_host_vsync = true;

	Reasons why;
	EXPECT_FALSE(EECycleRate::ResolveEligibility(in, &why).eligible);
	EXPECT_STREQ(why.eligible, "pacing comes from host vsync, not the limiter");
}

TEST(EECycleRateEligibility, AGSDumpReplaySuspendsTheGovernor)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.gs_dump_replay = true;

	EXPECT_FALSE(EECycleRate::ResolveEligibility(in).eligible);
}

TEST(EECycleRateEligibility, AFrameAdvanceSuspendsTheGovernor)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.frame_advancing = true;

	EXPECT_FALSE(EECycleRate::ResolveEligibility(in).eligible);
}

TEST(EECycleRateEligibility, NoRunningVMSuspendsTheGovernor)
{
	Inputs in = Healthy();
	in.dynamic_setting = true;
	in.vm_running = false;

	Reasons why;
	EXPECT_FALSE(EECycleRate::ResolveEligibility(in, &why).eligible);
	EXPECT_STREQ(why.eligible, "no VM is running");
}

// ------------------------------------------------------------------- provenance

TEST(EECycleRateProvenance, ADatabaseCycleRateIsRecordedWhenItLands)
{
	GameDatabaseSchema::GameEntry entry;
	entry.speedHacks.emplace_back(SpeedHack::EECycleRate, -1);

	Pcsx2Config config;
	ASSERT_FALSE(config.GameDBSetEECycleRate);

	entry.applyGameFixes(config, true, {}, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_EQ(config.Speedhacks.EECycleRate, -1);
	EXPECT_TRUE(config.GameDBSetEECycleRate);
}

// The flag has to follow the value. When the player has claimed the rate the database
// stands aside, so there is no database claim to record, and the governor is then free
// to act on the per-game dynamic key.
TEST(EECycleRateProvenance, AClaimedCycleRateLeavesNoDatabaseClaimBehind)
{
	GameDatabaseSchema::GameEntry entry;
	entry.speedHacks.emplace_back(SpeedHack::EECycleRate, -1);

	const PerGameOverrides ov = GameFile().Set("EmuCore/Speedhacks", "EECycleRate", 0).Overrides();

	Pcsx2Config config;
	config.Speedhacks.EECycleRate = 0;

	entry.applyGameFixes(config, true, ov, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_EQ(config.Speedhacks.EECycleRate, 0);
	EXPECT_FALSE(config.GameDBSetEECycleRate);
}

TEST(EECycleRateProvenance, ADatabaseEntryWithoutACycleRateRecordsNothing)
{
	GameDatabaseSchema::GameEntry entry;
	entry.speedHacks.emplace_back(SpeedHack::MTVU, 1);

	Pcsx2Config config;
	entry.applyGameFixes(config, true, {}, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_TRUE(config.Speedhacks.vuThread);
	EXPECT_FALSE(config.GameDBSetEECycleRate);
}

TEST(EECycleRateProvenance, TheDynamicKeyClaimsItselfAndNotTheRate)
{
	const PerGameOverrides ov = GameFile().Set("EmuCore/Speedhacks", "DynamicEECycleRate", true).Overrides();

	EXPECT_TRUE(ov.Has(SpeedHack::DynamicEECycleRate));
	EXPECT_FALSE(ov.Has(SpeedHack::EECycleRate)) << "opting in to the governor is not pinning a rate";
}

// ---------------------------------------------------------------- the two keys

TEST(EECycleRateSettings, HardcoreModeForcesTheGovernorOffAlongWithTheUnderclock)
{
	Pcsx2Config::SpeedhackOptions hacks;
	hacks.DynamicEECycleRate = true;
	hacks.EECycleRate = -2;
	hacks.EECycleSkip = 2;

	hacks.ClampForHardcoreMode();

	EXPECT_FALSE(hacks.DynamicEECycleRate) << "the governor only ever underclocks, and does it unasked";
	EXPECT_EQ(hacks.EECycleRate, 0);
	EXPECT_EQ(hacks.EECycleSkip, 0);
}

TEST(EECycleRateSettings, HardcoreModeStillAllowsOverclocking)
{
	Pcsx2Config::SpeedhackOptions hacks;
	hacks.EECycleRate = 2;

	hacks.ClampForHardcoreMode();

	EXPECT_EQ(hacks.EECycleRate, 2);
}

// CheckForCPUConfigChanges() asks SpeedhackOptions::operator== before it throws the EE
// and microVU code caches away. Where a development trace file goes changes no
// generated code, so it must not be part of that answer — otherwise switching the
// trace on recompiles everything, and the first windows the trace records are
// measuring the reset the trace itself caused.
TEST(EECycleRateSettings, ATracePathChangeIsNotACPUConfigChange)
{
	Pcsx2Config::SpeedhackOptions before;
	Pcsx2Config::SpeedhackOptions after = before;
	after.DynamicEECycleRateTrace = "/tmp/ee-cycle-rate.csv";

	EXPECT_TRUE(before == after);
}

TEST(EECycleRateSettings, TheGovernorSwitchItselfIsACPUConfigChange)
{
	Pcsx2Config::SpeedhackOptions before;
	Pcsx2Config::SpeedhackOptions after = before;
	after.DynamicEECycleRate = !before.DynamicEECycleRate;

	EXPECT_FALSE(before == after) << "turning the governor on changes what the effective selector may become";
}

TEST(EECycleRateSettings, TheGovernorIsOffInAStockConfiguration)
{
	const Pcsx2Config::SpeedhackOptions stock;

	EXPECT_FALSE(stock.DynamicEECycleRate);
	EXPECT_TRUE(stock.DynamicEECycleRateTrace.empty());
	EXPECT_FALSE(Pcsx2Config::SpeedhackOptions().DisableAll().DynamicEECycleRate);
}
