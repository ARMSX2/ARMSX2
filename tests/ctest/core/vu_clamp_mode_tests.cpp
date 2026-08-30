// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU clamp modes are stored as four ordered bools per VU and used as a
// number, in the two places that have to agree: the GameDatabase's unpacking of
// vuClampMode / vu0ClampMode / vu1ClampMode, and ApplySanityCheck, which
// rejects a mode whose lower bits are not set.
//
// The EE ladder's own pins are in ee_clamp_mode_tests.cpp; this is the same
// table on the other side, where the fourth rung is new.

#include <gtest/gtest.h>

#include "Config.h"
#include "GameDatabase.h"

namespace
{

struct ModeBits
{
	bool overflow, extra, sign, exact;
};

ModeBits Vu0BitsOf(const Pcsx2Config::RecompilerOptions& r)
{
	return {r.vu0Overflow, r.vu0ExtraOverflow, r.vu0SignOverflow, r.vu0ExactMode};
}

ModeBits Vu1BitsOf(const Pcsx2Config::RecompilerOptions& r)
{
	return {r.vu1Overflow, r.vu1ExtraOverflow, r.vu1SignOverflow, r.vu1ExactMode};
}

void SetVu0(Pcsx2Config::RecompilerOptions& r, const ModeBits& b)
{
	r.vu0Overflow = b.overflow;
	r.vu0ExtraOverflow = b.extra;
	r.vu0SignOverflow = b.sign;
	r.vu0ExactMode = b.exact;
}

void SetVu1(Pcsx2Config::RecompilerOptions& r, const ModeBits& b)
{
	r.vu1Overflow = b.overflow;
	r.vu1ExtraOverflow = b.extra;
	r.vu1SignOverflow = b.sign;
	r.vu1ExactMode = b.exact;
}

constexpr ModeBits kModes[] = {
	{false, false, false, false}, // 0 none
	{true, false, false, false},  // 1 normal
	{true, true, false, false},   // 2 extra
	{true, true, true, false},    // 3 extra + preserve sign
	{true, true, true, true},     // 4 exact
};

bool Same(const ModeBits& a, const ModeBits& b)
{
	return a.overflow == b.overflow && a.extra == b.extra && a.sign == b.sign &&
		   a.exact == b.exact;
}

} // namespace

TEST(VuClampMode, TheGameDatabaseUnpacksTheSameBits)
{
	for (u32 mode = 0; mode < std::size(kModes); mode++)
	{
		// vu0ClampMode and vu1ClampMode reach one VU each.
		GameDatabaseSchema::GameEntry split;
		split.vu0ClampMode = static_cast<GameDatabaseSchema::ClampMode>(mode);
		split.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(
			(mode + 1) % std::size(kModes));

		Pcsx2Config config;
		split.applyGameFixes(config, true);
		EXPECT_TRUE(Same(Vu0BitsOf(config.Cpu.Recompiler), kModes[mode]))
			<< "vu0 mode " << mode;
		EXPECT_TRUE(Same(Vu1BitsOf(config.Cpu.Recompiler),
			kModes[(mode + 1) % std::size(kModes)]))
			<< "vu1 mode " << mode;
		EXPECT_EQ(config.Cpu.Recompiler.GetVUClampMode(), mode);

		// vuClampMode reaches both.
		GameDatabaseSchema::GameEntry both;
		both.vu0ClampMode = static_cast<GameDatabaseSchema::ClampMode>(mode);
		both.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(mode);

		Pcsx2Config shared;
		both.applyGameFixes(shared, true);
		EXPECT_TRUE(Same(Vu0BitsOf(shared.Cpu.Recompiler), kModes[mode]))
			<< "shared vu0 mode " << mode;
		EXPECT_TRUE(Same(Vu1BitsOf(shared.Cpu.Recompiler), kModes[mode]))
			<< "shared vu1 mode " << mode;
	}
}

// Undefined is the "no entry" value and must leave the config alone, or every
// game without a clampModes block would be forced to whatever 0 means.
TEST(VuClampMode, AnUndefinedGameDbEntryDoesNotMoveTheMode)
{
	GameDatabaseSchema::GameEntry entry;
	ASSERT_EQ(entry.vu0ClampMode, GameDatabaseSchema::ClampMode::Undefined);
	ASSERT_EQ(entry.vu1ClampMode, GameDatabaseSchema::ClampMode::Undefined);

	Pcsx2Config config;
	SetVu0(config.Cpu.Recompiler, kModes[2]);
	SetVu1(config.Cpu.Recompiler, kModes[4]);
	entry.applyGameFixes(config, true);
	EXPECT_TRUE(Same(Vu0BitsOf(config.Cpu.Recompiler), kModes[2]));
	EXPECT_TRUE(Same(Vu1BitsOf(config.Cpu.Recompiler), kModes[4]));
}

// ApplySanityCheck's contract: a whole mode survives, and a set of bits that is
// not one is thrown away rather than half-honoured. The default is mode 1, so
// the rejected configs land there. Each VU is checked on its own, since they
// are two independent chains.
TEST(VuClampMode, SanityCheckKeepsWholeModesAndRejectsGaps)
{
	Pcsx2Config::RecompilerOptions defaults;
	const ModeBits fallback0 = Vu0BitsOf(defaults);
	const ModeBits fallback1 = Vu1BitsOf(defaults);

	for (u32 mode = 0; mode < std::size(kModes); mode++)
	{
		Pcsx2Config::RecompilerOptions r;
		SetVu0(r, kModes[mode]);
		SetVu1(r, kModes[mode]);
		r.ApplySanityCheck();
		EXPECT_TRUE(Same(Vu0BitsOf(r), kModes[mode])) << "whole vu0 mode " << mode;
		EXPECT_TRUE(Same(Vu1BitsOf(r), kModes[mode])) << "whole vu1 mode " << mode;
	}

	struct Gap
	{
		ModeBits bits;
		const char* what;
	};
	static const Gap kGaps[] = {
		{{false, true, false, false}, "extra without overflow"},
		{{true, false, true, false}, "preserve sign without extra"},
		{{true, true, false, true}, "exact without preserve sign"},
		{{false, false, false, true}, "exact with nothing under it"},
	};
	for (const Gap& g : kGaps)
	{
		Pcsx2Config::RecompilerOptions r0;
		SetVu0(r0, g.bits);
		r0.ApplySanityCheck();
		EXPECT_TRUE(Same(Vu0BitsOf(r0), fallback0)) << "vu0 " << g.what;

		Pcsx2Config::RecompilerOptions r1;
		SetVu1(r1, g.bits);
		r1.ApplySanityCheck();
		EXPECT_TRUE(Same(Vu1BitsOf(r1), fallback1)) << "vu1 " << g.what;
	}
}
