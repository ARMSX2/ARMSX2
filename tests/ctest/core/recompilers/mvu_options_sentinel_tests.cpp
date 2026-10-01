// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Does every setting the mVU emitters read reach the program cache's key?
//
// The on-disk cache decides whether a recorded program may be replayed from
// the options sentinel, so a setting that picks an emitted shape and is absent
// from the sentinel lets a program recorded under one setting hydrate into a
// run using the other. THREAD_VU1 and XgKickHack were both absent; the point
// of a table rather than a case each is that the next one fails here too.
//
// The list is what the arm64 mVU emitters actually read -- `EmuConfig.` and
// the CHECK_*/REC_*/THREAD_* macros over microVU*-arm64.{cpp,inl,h}. Settings
// read only by the dispatcher are not here: it is regenerated every reset and
// nothing persists it.

#include "harness/RecompilerTestEnvironment.h"

#include "Config.h"
#include "common/Pcsx2Defs.h"

#include <gtest/gtest.h>

#include <functional>

void mVUTestProbe_OptionsSentinel(int index, u64& lo, u64& hi);

namespace recompiler_tests {

namespace {

struct Gate
{
	const char* name;
	// Run before reading either sentinel: a setting that only reaches the
	// emitters through a composite needs the composite's other half on.
	std::function<void()> prepare;
	std::function<void()> flip;
};

u64 Sentinel(int index)
{
	u64 lo = 0, hi = 0;
	mVUTestProbe_OptionsSentinel(index, lo, hi);
	return lo;
}

} // namespace

TEST(MvuOptionsSentinel, EverySettingTheEmittersReadReachesTheCacheKey)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());

	auto& gf = EmuConfig.Gamefixes;
	auto& sh = EmuConfig.Speedhacks;
	auto& rc = EmuConfig.Cpu.Recompiler;

	const Gate gates[] = {
		{"Gamefixes.IbitHack",        nullptr, [&] { gf.IbitHack = !gf.IbitHack; }},
		{"Gamefixes.VUSyncHack",      nullptr, [&] { gf.VUSyncHack = !gf.VUSyncHack; }},
		{"Gamefixes.FullVU0SyncHack", nullptr, [&] { gf.FullVU0SyncHack = !gf.FullVU0SyncHack; }},
		{"Gamefixes.VuAddSubHack",    nullptr, [&] { gf.VuAddSubHack = !gf.VuAddSubHack; }},
		{"Gamefixes.VUOverflowHack",  nullptr, [&] { gf.VUOverflowHack = !gf.VUOverflowHack; }},
		{"Gamefixes.XgKickHack",      nullptr, [&] { gf.XgKickHack = !gf.XgKickHack; }},
		{"Speedhacks.vuFlagHack",     nullptr, [&] { sh.vuFlagHack = !sh.vuFlagHack; }},
		{"Speedhacks.vuThread",       nullptr, [&] { sh.vuThread = !sh.vuThread; }},
		{"Speedhacks.EECycleRate",    nullptr, [&] { sh.EECycleRate = static_cast<s8>(sh.EECycleRate + 1); }},
		{"Speedhacks.EECycleSkip",    nullptr, [&] { sh.EECycleSkip = static_cast<u8>(sh.EECycleSkip + 1); }},
		// Reaches the emitters only as THREAD_VU1's other half.
		{"Recompiler.EnableVU1",      [&] { sh.vuThread = true; }, [&] { rc.EnableVU1 = !rc.EnableVU1; }},
		{"Recompiler.vu0Overflow",      nullptr, [&] { rc.vu0Overflow = !rc.vu0Overflow; }},
		{"Recompiler.vu0ExtraOverflow", nullptr, [&] { rc.vu0ExtraOverflow = !rc.vu0ExtraOverflow; }},
		{"Recompiler.vu0SignOverflow",  nullptr, [&] { rc.vu0SignOverflow = !rc.vu0SignOverflow; }},
		{"Recompiler.vu0ExactMode",     nullptr, [&] { rc.vu0ExactMode = !rc.vu0ExactMode; }},
		{"Recompiler.vu1Overflow",      nullptr, [&] { rc.vu1Overflow = !rc.vu1Overflow; }},
		{"Recompiler.vu1ExtraOverflow", nullptr, [&] { rc.vu1ExtraOverflow = !rc.vu1ExtraOverflow; }},
		{"Recompiler.vu1SignOverflow",  nullptr, [&] { rc.vu1SignOverflow = !rc.vu1SignOverflow; }},
		{"Recompiler.vu1ExactMode",     nullptr, [&] { rc.vu1ExactMode = !rc.vu1ExactMode; }},
		{"Cpu.FPUFPCR",  nullptr, [&] { EmuConfig.Cpu.FPUFPCR.bitmask ^= 0x00800000u; }},
		{"Cpu.VU0FPCR",  nullptr, [&] { EmuConfig.Cpu.VU0FPCR.bitmask ^= 0x00800000u; }},
		{"Cpu.VU1FPCR",  nullptr, [&] { EmuConfig.Cpu.VU1FPCR.bitmask ^= 0x00800000u; }},
	};

	const Pcsx2Config saved = EmuConfig;
	for (const Gate& g : gates)
	{
		for (int index = 0; index < 2; index++)
		{
			EmuConfig = saved;
			if (g.prepare)
				g.prepare();
			const u64 before = Sentinel(index);
			g.flip();
			const u64 after = Sentinel(index);
			EXPECT_NE(before, after)
				<< g.name << " does not reach VU" << index << "'s options "
				<< "sentinel, so a program recorded under one value of it is "
				<< "replayable under the other.";
		}
	}

	// The probe rebuilds the live sentinel as a side effect; leave both VUs
	// holding the one the current config asks for.
	EmuConfig = saved;
	Sentinel(0);
	Sentinel(1);
}

} // namespace recompiler_tests
