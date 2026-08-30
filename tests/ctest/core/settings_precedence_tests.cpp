// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Global settings < GameDB < per-game settings.
//
// The middle of that used to be the end of it: the database wrote into EmuConfig
// after the whole settings read, so a value the player set for one game was quietly
// replaced. What decides it now is whether the per-game file holds the key, since
// every settings screen deletes the key rather than writing a value for "use the
// global setting".
//
// That rule only holds while the key tables stay complete. A knob nobody mapped is a
// setting that silently goes back to being overridden, and it looks like nothing —
// no warning, no failure, just the old bug for that one setting. Hence the drift
// guards below, which are the real reason this file exists.

#include <gtest/gtest.h>

#include "Config.h"
#include "GameDatabase.h"
#include "PerGameOverrides.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

namespace
{

// Stands in for a per-game INI. Same class the reference configurations are built
// with, so values compare the way they will be stored.
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

	const MemorySettingsInterface& Get() const { return m_si; }
	PerGameOverrides Overrides() const { return ComputePerGameOverrides(m_si); }

private:
	MemorySettingsInterface m_si;
};

} // namespace

// ---------------------------------------------------------------- provenance

TEST(SettingsPrecedence, AnEmptyFileClaimsNothing)
{
	const PerGameOverrides ov = GameFile().Overrides();
	EXPECT_FALSE(ov.Any());
	EXPECT_EQ(ov.gs_fixes, 0u);
	EXPECT_EQ(ov.gs_hacks, 0u);
	EXPECT_EQ(ov.core, 0u);
	EXPECT_EQ(ov.speedhacks, 0u);
	EXPECT_TRUE(ov.gamefixes.none());
}

TEST(SettingsPrecedence, EveryGamefixKeyClaimsItsOwnFixAndNoOther)
{
	for (u32 i = GamefixId_FIRST; i < GamefixId_COUNT; i++)
	{
		const GamefixId id = static_cast<GamefixId>(i);
		const char* key = PerGameOverrideKeys::ForGamefix(id);
		ASSERT_NE(key, nullptr) << "gamefix " << i;

		GameFile file;
		file.Set("EmuCore/Gamefixes", key, false);
		const PerGameOverrides ov = file.Overrides();

		EXPECT_EQ(ov.gamefixes.count(), 1u) << key;
		EXPECT_TRUE(ov.Has(id)) << key;
		EXPECT_EQ(ov.core, 0u) << key;
		EXPECT_EQ(ov.speedhacks, 0u) << key;
	}
}

TEST(SettingsPrecedence, EverySpeedhackKeyClaimsItsOwnHackAndNoOther)
{
	for (u32 i = 0; i < static_cast<u32>(SpeedHack::MaxCount); i++)
	{
		const SpeedHack hack = static_cast<SpeedHack>(i);
		const char* key = PerGameOverrideKeys::ForSpeedHack(hack);
		ASSERT_NE(key, nullptr) << "speedhack " << i;

		GameFile file;
		file.Set("EmuCore/Speedhacks", key, 1);
		const PerGameOverrides ov = file.Overrides();

		EXPECT_EQ(ov.speedhacks, 1u << i) << key;
		EXPECT_TRUE(ov.gamefixes.none()) << key;
		EXPECT_EQ(ov.core, 0u) << key;
	}
}

// A clamp mode is one picker over three or four keys. The screens write and delete
// them together, so any one of them present has to claim the whole mode — otherwise
// the database rewrites the other bits and the player gets a mode they never chose.
TEST(SettingsPrecedence, AnyOneKeyOfAClampModeClaimsTheWholeMode)
{
	for (u32 i = 0; i < static_cast<u32>(CoreGameDBKnob::MaxCount); i++)
	{
		const CoreGameDBKnob knob = static_cast<CoreGameDBKnob>(i);
		const PerGameOverrideKeys::CoreKnobKeys keys = PerGameOverrideKeys::ForCoreKnob(knob);
		ASSERT_NE(keys.section, nullptr) << "knob " << i;
		ASSERT_GT(keys.count, 0u) << "knob " << i;

		for (u32 k = 0; k < keys.count; k++)
		{
			GameFile file;
			file.Set(keys.section, keys.keys[k], true);
			const PerGameOverrides ov = file.Overrides();

			EXPECT_EQ(ov.core, 1u << i) << keys.keys[k];
			EXPECT_TRUE(ov.gamefixes.none()) << keys.keys[k];
			EXPECT_EQ(ov.speedhacks, 0u) << keys.keys[k];
		}
	}
}

TEST(SettingsPrecedence, AGraphicsKeyClaimsItsFixAndItsLegacyHackBit)
{
	GameFile file;
	file.Set("EmuCore/GS", "UserHacks_align_sprite_X", true);
	const PerGameOverrides ov = file.Overrides();

	EXPECT_TRUE(ov.Has(GameDatabaseSchema::GSHWFixId::AlignSprite));
	// The narrower mask matters too: MaskUserHacks() strips an unclaimed hack before
	// the database is ever consulted, so without this bit the value is already gone.
	EXPECT_EQ(ov.gs_hacks, 1u << static_cast<u32>(GSUserHackOverride::AlignSprite));
}

// Mipmapping was never a "user hack", so it has no bit in the legacy mask. It still
// has to be claimable, because it is one of the settings people actually change.
TEST(SettingsPrecedence, ASettingThatWasNeverAUserHackIsStillClaimable)
{
	GameFile file;
	file.Set("EmuCore/GS", "hw_mipmap", false);
	const PerGameOverrides ov = file.Overrides();

	EXPECT_TRUE(ov.Has(GameDatabaseSchema::GSHWFixId::Mipmap));
	EXPECT_EQ(ov.gs_hacks, 0u);
}

// One control, two database fixes. The database clamps the blend level from both
// ends, so claiming the setting has to silence both or the player still gets moved.
TEST(SettingsPrecedence, TheBlendingSettingClaimsBothOfItsClamps)
{
	GameFile file;
	file.Set("EmuCore/GS", "accurate_blending_unit", 2);
	const PerGameOverrides ov = file.Overrides();

	EXPECT_TRUE(ov.Has(GameDatabaseSchema::GSHWFixId::MinimumBlendingLevel));
	EXPECT_TRUE(ov.Has(GameDatabaseSchema::GSHWFixId::MaximumBlendingLevel));
}

// ---------------------------------------------------------------- drift guards

TEST(SettingsPrecedenceDrift, EveryGamefixHasASettingsKey)
{
	for (u32 i = GamefixId_FIRST; i < GamefixId_COUNT; i++)
	{
		const char* key = PerGameOverrideKeys::ForGamefix(static_cast<GamefixId>(i));
		ASSERT_NE(key, nullptr) << "gamefix " << i << " has no settings key, so it can never be claimed";
		EXPECT_STRNE(key, "") << "gamefix " << i;
	}
}

TEST(SettingsPrecedenceDrift, EverySpeedhackHasASettingsKey)
{
	for (u32 i = 0; i < static_cast<u32>(SpeedHack::MaxCount); i++)
	{
		const char* key = PerGameOverrideKeys::ForSpeedHack(static_cast<SpeedHack>(i));
		ASSERT_NE(key, nullptr) << "speedhack " << i << " has no settings key, so it can never be claimed";
	}
}

// The six exceptions are deliberate and are the whole list: three renderer routine
// selectors with no setting and no UI, and three that only raise a recommendation and
// write no config field. Anything else reaching this list is a setting that has gone
// back to being silently overridden.
TEST(SettingsPrecedenceDrift, OnlyTheSixUnsettableGraphicsFixesLackAKey)
{
	using GSHWFixId = GameDatabaseSchema::GSHWFixId;
	static constexpr GSHWFixId kExpected[] = {
		GSHWFixId::RecommendedBlendingLevel,
		GSHWFixId::RecommendedAccurateAlphaTest,
		GSHWFixId::RecommendedHWAA1,
		GSHWFixId::GetSkipCount,
		GSHWFixId::BeforeDraw,
		GSHWFixId::MoveHandler,
	};

	for (u32 i = 0; i < static_cast<u32>(GSHWFixId::Count); i++)
	{
		const GSHWFixId id = static_cast<GSHWFixId>(i);
		if (PerGameOverrideKeys::ForGSHWFix(id) != nullptr)
			continue;

		bool expected = false;
		for (const GSHWFixId allowed : kExpected)
			expected |= (allowed == id);

		EXPECT_TRUE(expected) << "GS hardware fix " << i << " has no settings key, so the player cannot claim it";
	}
}

TEST(SettingsPrecedenceDrift, EveryMappedKeyIsRecognisedAsAClaim)
{
	for (u32 i = GamefixId_FIRST; i < GamefixId_COUNT; i++)
	{
		EXPECT_TRUE(PerGameOverrideKeys::ClaimsAGameDBSetting(
			"EmuCore/Gamefixes", PerGameOverrideKeys::ForGamefix(static_cast<GamefixId>(i))))
			<< "gamefix " << i;
	}

	for (u32 i = 0; i < static_cast<u32>(SpeedHack::MaxCount); i++)
	{
		EXPECT_TRUE(PerGameOverrideKeys::ClaimsAGameDBSetting(
			"EmuCore/Speedhacks", PerGameOverrideKeys::ForSpeedHack(static_cast<SpeedHack>(i))))
			<< "speedhack " << i;
	}

	for (u32 i = 0; i < static_cast<u32>(CoreGameDBKnob::MaxCount); i++)
	{
		const PerGameOverrideKeys::CoreKnobKeys keys = PerGameOverrideKeys::ForCoreKnob(static_cast<CoreGameDBKnob>(i));
		for (u32 k = 0; k < keys.count; k++)
			EXPECT_TRUE(PerGameOverrideKeys::ClaimsAGameDBSetting(keys.section, keys.keys[k])) << keys.keys[k];
	}

	EXPECT_FALSE(PerGameOverrideKeys::ClaimsAGameDBSetting("EmuCore/GS", "Renderer"));
	EXPECT_FALSE(PerGameOverrideKeys::ClaimsAGameDBSetting("EmuCore", "EnableCheats"));
}

// ---------------------------------------------------------------- the apply side

TEST(SettingsPrecedenceApply, AnUnclaimedCoreKnobStillTakesTheDatabaseValue)
{
	GameDatabaseSchema::GameEntry entry;
	entry.eeClampMode = static_cast<GameDatabaseSchema::ClampMode>(3);
	entry.gameFixes.push_back(Fix_EETiming);
	entry.speedHacks.emplace_back(SpeedHack::MTVU, 1);

	Pcsx2Config config;
	config.Cpu.Recompiler.SetEEClampMode(1);
	config.Gamefixes.Set(Fix_EETiming, false);
	config.Speedhacks.vuThread = false;

	entry.applyGameFixes(config, true, {}, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_EQ(config.Cpu.Recompiler.GetEEClampMode(), 3u);
	EXPECT_TRUE(config.Gamefixes.Get(Fix_EETiming));
	EXPECT_TRUE(config.Speedhacks.vuThread);
}

TEST(SettingsPrecedenceApply, AClaimedCoreKnobKeepsThePlayersValue)
{
	GameDatabaseSchema::GameEntry entry;
	entry.eeClampMode = static_cast<GameDatabaseSchema::ClampMode>(3);
	entry.gameFixes.push_back(Fix_EETiming);
	entry.speedHacks.emplace_back(SpeedHack::MTVU, 1);

	const PerGameOverrides ov = GameFile()
									.Set("EmuCore/CPU/Recompiler", "fpuFullMode", false)
									.Set("EmuCore/Gamefixes", "EETimingHack", false)
									.Set("EmuCore/Speedhacks", "vuThread", false)
									.Overrides();

	Pcsx2Config config;
	config.Cpu.Recompiler.SetEEClampMode(1);
	config.Gamefixes.Set(Fix_EETiming, false);
	config.Speedhacks.vuThread = false;

	entry.applyGameFixes(config, true, ov, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_EQ(config.Cpu.Recompiler.GetEEClampMode(), 1u);
	EXPECT_FALSE(config.Gamefixes.Get(Fix_EETiming));
	EXPECT_FALSE(config.Speedhacks.vuThread);
}

// Claiming one setting must not cost the game every other fix it had. That was the
// whole failing of the two all-or-nothing switches this replaces.
TEST(SettingsPrecedenceApply, ClaimingOneKnobLeavesTheRestOfTheEntryAlone)
{
	GameDatabaseSchema::GameEntry entry;
	entry.eeClampMode = static_cast<GameDatabaseSchema::ClampMode>(3);
	entry.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(2);
	entry.gameFixes.push_back(Fix_EETiming);

	const PerGameOverrides ov = GameFile().Set("EmuCore/CPU/Recompiler", "fpuOverflow", true).Overrides();

	Pcsx2Config config;
	config.Cpu.Recompiler.SetEEClampMode(1);
	config.Cpu.Recompiler.vu1Overflow = false;
	config.Cpu.Recompiler.vu1ExtraOverflow = false;
	config.Cpu.Recompiler.vu1SignOverflow = false;
	config.Gamefixes.Set(Fix_EETiming, false);

	entry.applyGameFixes(config, true, ov, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_EQ(config.Cpu.Recompiler.GetEEClampMode(), 1u) << "the claimed one";
	EXPECT_TRUE(config.Cpu.Recompiler.vu1Overflow) << "an unclaimed one";
	EXPECT_TRUE(config.Cpu.Recompiler.vu1ExtraOverflow) << "an unclaimed one";
	EXPECT_TRUE(config.Gamefixes.Get(Fix_EETiming)) << "an unclaimed one";
}

TEST(SettingsPrecedenceApply, AClaimedGraphicsFixKeepsThePlayersValue)
{
	GameDatabaseSchema::GameEntry entry;
	entry.gsHWFixes.emplace_back(GameDatabaseSchema::GSHWFixId::Mipmap, 1);
	entry.gsHWFixes.emplace_back(GameDatabaseSchema::GSHWFixId::TextureInsideRT, 1);

	Pcsx2Config::GSOptions gs;
	gs.HWMipmap = false;
	gs.UserHacks_TextureInsideRt = GSTextureInRtMode::Disabled;

	const PerGameOverrides ov = GameFile().Set("EmuCore/GS", "hw_mipmap", false).Overrides();
	entry.applyGSHardwareFixes(gs, ov, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_FALSE(gs.HWMipmap) << "claimed, so the database stands aside";
	EXPECT_EQ(gs.UserHacks_TextureInsideRt, GSTextureInRtMode::InsideTargets) << "unclaimed, so it still applies";
}

// ---------------------------------------------------------------- the copy filter

namespace
{

// Everything CopyConfiguration would write, as key counts per section.
size_t CountKeys(const MemorySettingsInterface& si, const char* section)
{
	return si.GetKeyValueList(section).size();
}

} // namespace

TEST(SettingsCopyFilter, CopyingAnUntouchedConfigurationWritesNothing)
{
	MemorySettingsInterface source;
	{
		Pcsx2Config defaults;
		SettingsSaveWrapper wrapper(source);
		defaults.LoadSaveCore(wrapper);
	}

	MemorySettingsInterface dest;
	Pcsx2Config::CopyConfiguration(&dest, source, std::string_view());

	EXPECT_EQ(CountKeys(dest, "EmuCore/GS"), 0u);
	EXPECT_EQ(CountKeys(dest, "EmuCore/Gamefixes"), 0u);
	EXPECT_EQ(CountKeys(dest, "EmuCore/Speedhacks"), 0u);
	EXPECT_EQ(CountKeys(dest, "EmuCore/CPU/Recompiler"), 0u);
	EXPECT_TRUE(dest.IsEmpty()) << "a copy of stock settings is not a set of decisions";
}

// The other half of the filter, and the one that keeps the copy button from
// suppressing fixes: a value the database is going to set anyway is not a decision,
// so writing it down would only create a claim against the fix it agrees with.
//
// Driven through the wrapper rather than CopyConfiguration because building the real
// second reference means loading the game database off disk, which a unit test has no
// business doing. The reference here is what a database entry would have produced.
TEST(SettingsCopyFilter, AValueTheDatabaseWouldSetAnywayIsNotWritten)
{
	MemorySettingsInterface defaults_si;
	{
		Pcsx2Config defaults;
		SettingsSaveWrapper wrapper(defaults_si);
		defaults.LoadSaveCore(wrapper);
	}

	MemorySettingsInterface database_si;
	{
		// Stands in for a game whose entry carries `gameFixes: [EETiming]`.
		Pcsx2Config database;
		database.Gamefixes.Set(Fix_EETiming, true);
		SettingsSaveWrapper wrapper(database_si);
		database.LoadSaveCore(wrapper);
	}

	// The player turned the same fix on globally, and separately turned another one on
	// that the database says nothing about.
	Pcsx2Config source;
	source.Gamefixes.Set(Fix_EETiming, true);
	source.Gamefixes.Set(Fix_XGKick, true);

	MemorySettingsInterface dest;
	{
		SettingsSaveDeviationsWrapper wrapper(dest, {&defaults_si, &database_si});
		source.LoadSaveCore(wrapper);
	}

	EXPECT_FALSE(dest.ContainsValue("EmuCore/Gamefixes", "EETimingHack"))
		<< "the database sets this one, so claiming it would suppress the fix it agrees with";
	EXPECT_TRUE(dest.ContainsValue("EmuCore/Gamefixes", "XgKickHack"))
		<< "the database says nothing about this one, so it is a real decision";
	EXPECT_EQ(CountKeys(dest, "EmuCore/Gamefixes"), 1u);
}

TEST(SettingsCopyFilter, OnlyTheChangedValueIsWritten)
{
	MemorySettingsInterface source;
	{
		Pcsx2Config config;
		config.Gamefixes.Set(Fix_EETiming, true);
		SettingsSaveWrapper wrapper(source);
		config.LoadSaveCore(wrapper);
	}

	MemorySettingsInterface dest;
	Pcsx2Config::CopyConfiguration(&dest, source, std::string_view());

	EXPECT_EQ(CountKeys(dest, "EmuCore/Gamefixes"), 1u);
	EXPECT_TRUE(dest.ContainsValue("EmuCore/Gamefixes", "EETimingHack"));
	EXPECT_EQ(CountKeys(dest, "EmuCore/GS"), 0u);
}
