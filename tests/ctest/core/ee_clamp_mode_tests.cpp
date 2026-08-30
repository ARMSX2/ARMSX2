// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The EE clamp mode is stored as four ordered bools and used as a number, in
// three places that have to agree: Get/SetEEClampMode, the GameDatabase's own
// unpacking of eeClampMode, and ApplySanityCheck, which rejects a mode whose
// lower bits are not set.
//
// Mode 4 is the reason these are pinned: adding a mode on top of three that
// had gone unasserted is the change that leaves a `>= 3` where a `>= 4` was
// meant.

#include <gtest/gtest.h>

#include "Config.h"
#include "GameDatabase.h"

namespace
{

struct ModeBits
{
	bool overflow, extra, full, exact;
};

ModeBits BitsOf(const Pcsx2Config::RecompilerOptions& r)
{
	return {r.fpuOverflow, r.fpuExtraOverflow, r.fpuFullMode, r.fpuExactMode};
}

// What each mode number means as a set of bits. This is the table the GameDB,
// the INI and the picker all encode and decode through.
constexpr ModeBits kModes[] = {
	{false, false, false, false}, // 0 none
	{true, false, false, false},  // 1 normal
	{true, true, false, false},   // 2 extra + preserve sign
	{true, true, true, false},    // 3 full
	{true, true, true, true},     // 4 exact
};

bool Same(const ModeBits& a, const ModeBits& b)
{
	return a.overflow == b.overflow && a.extra == b.extra && a.full == b.full &&
		   a.exact == b.exact;
}

} // namespace

TEST(EeClampMode, SetAndGetRoundTripEveryMode)
{
	for (u32 mode = 0; mode < std::size(kModes); mode++)
	{
		Pcsx2Config::RecompilerOptions r;
		r.SetEEClampMode(mode);
		EXPECT_TRUE(Same(BitsOf(r), kModes[mode])) << "mode " << mode;
		EXPECT_EQ(r.GetEEClampMode(), mode);
	}
}

TEST(EeClampMode, TheGameDatabaseUnpacksTheSameBits)
{
	for (u32 mode = 0; mode < std::size(kModes); mode++)
	{
		GameDatabaseSchema::GameEntry entry;
		entry.eeClampMode = static_cast<GameDatabaseSchema::ClampMode>(mode);

		Pcsx2Config config;
		// Something other than the mode under test, so a no-op apply fails.
		config.Cpu.Recompiler.SetEEClampMode(mode == 1 ? 4 : 1);
		entry.applyGameFixes(config, true);

		EXPECT_EQ(config.Cpu.Recompiler.GetEEClampMode(), mode);
		EXPECT_TRUE(Same(BitsOf(config.Cpu.Recompiler), kModes[mode]))
			<< "mode " << mode;
	}
}

// Undefined is the "no entry" value and must leave the config alone, or every
// game without a clampModes block would be forced to whatever 0 means.
TEST(EeClampMode, AnUndefinedGameDbEntryDoesNotMoveTheMode)
{
	GameDatabaseSchema::GameEntry entry;
	ASSERT_EQ(entry.eeClampMode, GameDatabaseSchema::ClampMode::Undefined);

	Pcsx2Config config;
	config.Cpu.Recompiler.SetEEClampMode(2);
	entry.applyGameFixes(config, true);
	EXPECT_EQ(config.Cpu.Recompiler.GetEEClampMode(), 2u);
}

// ApplySanityCheck's contract: a whole mode survives, and a set of bits that is
// not one is thrown away rather than half-honoured. The default is mode 1, so
// the rejected configs land there.
TEST(EeClampMode, SanityCheckKeepsWholeModesAndRejectsGaps)
{
	for (u32 mode = 0; mode < std::size(kModes); mode++)
	{
		Pcsx2Config::RecompilerOptions r;
		r.SetEEClampMode(mode);
		r.ApplySanityCheck();
		EXPECT_EQ(r.GetEEClampMode(), mode) << "whole mode " << mode;
	}

	const u32 fallback = Pcsx2Config::RecompilerOptions().GetEEClampMode();
	struct Gap
	{
		ModeBits bits;
		const char* what;
	};
	static const Gap kGaps[] = {
		{{false, true, false, false}, "extra without overflow"},
		{{true, false, true, false}, "full without extra"},
		{{true, true, false, true}, "exact without full"},
		{{false, false, false, true}, "exact with nothing under it"},
	};
	for (const Gap& g : kGaps)
	{
		Pcsx2Config::RecompilerOptions r;
		r.fpuOverflow = g.bits.overflow;
		r.fpuExtraOverflow = g.bits.extra;
		r.fpuFullMode = g.bits.full;
		r.fpuExactMode = g.bits.exact;
		r.ApplySanityCheck();
		EXPECT_EQ(r.GetEEClampMode(), fallback) << g.what;
	}
}
