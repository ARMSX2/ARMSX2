// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP-thread settings (three INI keys, one game database key) and IopThread::ResolveMode, the
// pure function that maps the requested mode plus host and VM conditions to Off, Lockstep or
// Threaded. A device is described by a HostCpuInfo struct here; nothing in this file runs a thread.

#include <gtest/gtest.h>

#include "Config.h"
#include "GameDatabase.h"
#include "IopThread/IopChannels.h"
#include "IopThread/IopLagRules.h"
#include "IopThread/IopThreadMode.h"
#include "PerGameOverrides.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

#include <climits>

namespace
{
	using IopThread::ClampLag;
	using IopThread::HostCpuInfo;
	using IopThread::ResolveMode;
	using IopThread::VmConditions;

	HostCpuInfo Host(bool arm64, u32 total_cores, u32 performance_cores)
	{
		HostCpuInfo host;
		host.is_arm64 = arm64;
		host.total_cores = total_cores;
		host.performance_cores = performance_cores;
		return host;
	}

	// One row per target device. performance_cores is what GetPerformanceCoresMask() counts on the
	// device: cores of at least 0.75 of the fastest.
	const HostCpuInfo kSd865 = Host(true, 8, 4); // 1 prime + 3 A77, 4 A55
	const HostCpuInfo kMq65 = Host(true, 8, 4); // 4 A73, 4 A53
	const HostCpuInfo kDimensity8300 = Host(true, 8, 4);
	const HostCpuInfo kM2 = Host(true, 8, 4); // 4 performance + 4 efficiency
	const HostCpuInfo kM2Max = Host(true, 12, 8); // 8 performance + 4 efficiency
	const HostCpuInfo kRk3562 = Host(true, 4, 4); // 4 A53, all alike
	const HostCpuInfo kSd662 = Host(true, 8, 4); // 4 A73 + 4 A53
	const HostCpuInfo kRg55g1 = Host(true, 8, 2); // 2 A78 + 6 A55
	const HostCpuInfo kUniform8 = Host(true, 8, 8); // 8 cores of one speed: every one is in the tier

	Pcsx2Config Requested(bool iop_thread, bool lockstep, bool mtvu)
	{
		Pcsx2Config config;
		config.Speedhacks.iopThread = iop_thread;
		config.Speedhacks.iopThreadLockstep = lockstep;
		config.Speedhacks.vuThread = mtvu;
		return config;
	}

	IopThreadMode Resolve(bool iop_thread, bool lockstep, bool mtvu, const HostCpuInfo& host, const VmConditions& vm = {})
	{
		return ResolveMode(Requested(iop_thread, lockstep, mtvu), host, vm);
	}

	Pcsx2Config::SpeedhackOptions LoadHacks(MemorySettingsInterface& si)
	{
		Pcsx2Config::SpeedhackOptions hacks;
		SettingsLoadWrapper wrap(si);
		hacks.LoadSave(wrap);
		return hacks;
	}

	int LoadLag(s32 value)
	{
		MemorySettingsInterface si;
		si.SetIntValue("EmuCore/Speedhacks", "iopThreadLag", value);
		return LoadHacks(si).iopThreadLag;
	}
} // namespace

TEST(IopThreadMode, TheDefaultIsOff)
{
	const Pcsx2Config config;
	EXPECT_FALSE(config.Speedhacks.iopThread);
	EXPECT_FALSE(config.Speedhacks.iopThreadLockstep);
	EXPECT_EQ(ResolveMode(config, kSd865, {}), IopThreadMode::Off);
}

TEST(IopThreadMode, AnEmptySettingsFileLoadsAsOff)
{
	MemorySettingsInterface si;
	const Pcsx2Config::SpeedhackOptions hacks = LoadHacks(si);
	EXPECT_FALSE(hacks.iopThread);
	EXPECT_FALSE(hacks.iopThreadLockstep);
	EXPECT_EQ(hacks.iopThreadLag, IopThread::kDefaultSettingLag);
}

TEST(IopThreadMode, TheDefaultOfTheLagSettingIsOneFrame)
{
	// One NTSC frame of EE cycles: 294,912,000 a second at 60 Hz.
	EXPECT_EQ(IopThread::kOneFrameLag, 294912000 / 60);
	EXPECT_EQ(IopThread::kDefaultSettingLag, 4915200);
	EXPECT_EQ(Pcsx2Config().Speedhacks.iopThreadLag, 4915200);
	EXPECT_EQ(Pcsx2Config::SpeedhackOptions().iopThreadLag, IopThread::kDefaultSettingLag);
	// The default is a value the core keeps as it is.
	EXPECT_EQ(IopThread::ClampLag(IopThread::kDefaultSettingLag), IopThread::kDefaultSettingLag);
}

TEST(IopThreadMode, TheTightBoundStaysTwentyFourThousandFiveHundredSeventySixCycles)
{
	// Eight times kEeWaitCycles, the longest the EE runs between event tests. It is no longer the default of the
	// setting, but the lag rules fall back to it while the EE waits on the IOP, and a bound at or below it
	// turns the rules off, so it must not move with the setting's default.
	EXPECT_EQ(IopThread::kDefaultLag, 24576);
	EXPECT_EQ(IopThread::LagRulesConfig().tight, 24576);
	EXPECT_LT(IopThread::kDefaultLag, IopThread::kDefaultSettingLag);
}

TEST(IopThreadMode, DisableAllPutsTheLagBackToTheDefaultOfTheSetting)
{
	Pcsx2Config::SpeedhackOptions hacks;
	hacks.iopThreadLag = IopThread::kMinLag;
	hacks.DisableAll();
	EXPECT_EQ(hacks.iopThreadLag, IopThread::kDefaultSettingLag);
}

TEST(IopThreadMode, TheEnableFlagAloneGivesThreaded)
{
	EXPECT_EQ(Resolve(true, false, true, kSd865), IopThreadMode::Threaded);
}

TEST(IopThreadMode, TheLockstepFlagWithTheEnableFlagGivesLockstep)
{
	EXPECT_EQ(Resolve(true, true, true, kSd865), IopThreadMode::Lockstep);
}

TEST(IopThreadMode, TheLockstepFlagWithoutTheEnableFlagIsOff)
{
	// Lockstep is a way of running the thread. It is not a feature of its own.
	EXPECT_EQ(Resolve(false, true, true, kSd865), IopThreadMode::Off);
	EXPECT_EQ(Resolve(false, false, true, kSd865), IopThreadMode::Off);
}

TEST(IopThreadMode, PS1ModeForcesOff)
{
	VmConditions vm;
	vm.ps1_mode = true;
	EXPECT_EQ(Resolve(true, false, true, kSd865, vm), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, true, true, kSd865, vm), IopThreadMode::Off);
}

TEST(IopThreadMode, AnArcadeTitleForcesOff)
{
	VmConditions vm;
	vm.arcade = true;
	EXPECT_EQ(Resolve(true, false, true, kSd865, vm), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, true, true, kSd865, vm), IopThreadMode::Off);
}

TEST(IopThreadMode, ReplayingAGSDumpForcesOff)
{
	VmConditions vm;
	vm.gs_dump_replay = true;
	EXPECT_EQ(Resolve(true, false, true, kSd865, vm), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, true, true, kSd865, vm), IopThreadMode::Off);
}

TEST(IopThreadMode, AnIopBreakpointForcesOff)
{
	VmConditions vm;
	vm.iop_breakpoints = true;
	EXPECT_EQ(Resolve(true, false, true, kSd865, vm), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, true, true, kSd865, vm), IopThreadMode::Off);
}

TEST(IopThreadMode, TheRecompilerTestHarnessForcesOff)
{
	VmConditions vm;
	vm.recompiler_test_harness = true;
	EXPECT_EQ(Resolve(true, false, true, kSd865, vm), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, true, true, kSd865, vm), IopThreadMode::Off);
}

TEST(IopThreadMode, ANonArm64HostForcesThreadedOff)
{
	// A host with plenty of cores, so that only the architecture can be the reason.
	const HostCpuInfo x86_desktop = Host(false, 16, 8);
	EXPECT_EQ(Resolve(true, false, true, x86_desktop), IopThreadMode::Off);
}

TEST(IopThreadMode, ANonArm64HostForcesLockstepOffToo)
{
	// The x86 IOP JIT has not been audited for running on another thread, and x86 is not a target
	// for this feature. Neither mode resolves there, whatever the core count.
	const HostCpuInfo x86_desktop = Host(false, 16, 8);
	EXPECT_EQ(Resolve(true, true, true, x86_desktop), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, true, false, x86_desktop), IopThreadMode::Off);
	// A host that can say nothing about itself is no different.
	EXPECT_EQ(Resolve(true, true, true, Host(false, 0, 0)), IopThreadMode::Off);
}

TEST(IopThreadMode, TheTestHarnessFlagIsOffUnlessSomeoneSetsIt)
{
	// This binary may be built with PCSX2_RECOMPILER_TESTS, as eerunner is. The define must not be what
	// says the harness is running, or eerunner could not run Lockstep: only the gtest recompiler
	// environment calls SetRecompilerTestHarness().
	EXPECT_FALSE(IopThread::IsRecompilerTestHarness());

	IopThread::SetRecompilerTestHarness(true);
	EXPECT_TRUE(IopThread::IsRecompilerTestHarness());
	IopThread::SetRecompilerTestHarness(false);
	EXPECT_FALSE(IopThread::IsRecompilerTestHarness());
}

TEST(IopThreadMode, EachForcingConditionIsSufficientAloneAndNoneStaysOnItsOwn)
{
	// With every condition clear the same inputs resolve On, so the Off results above are
	// the conditions' doing and not a side effect of the host row or the flags chosen.
	EXPECT_EQ(Resolve(true, false, true, kSd865, VmConditions()), IopThreadMode::Threaded);
	EXPECT_EQ(Resolve(true, true, true, kSd865, VmConditions()), IopThreadMode::Lockstep);
}

TEST(IopThreadMode, AllTheForcingConditionsAtOnceIsOff)
{
	VmConditions vm;
	vm.ps1_mode = vm.arcade = vm.gs_dump_replay = vm.iop_breakpoints = vm.recompiler_test_harness = true;
	EXPECT_EQ(Resolve(true, true, true, Host(false, 4, 4), vm), IopThreadMode::Off);
}

TEST(IopThreadModeDevices, TheSd865IsOn)
{
	EXPECT_EQ(Resolve(true, false, true, kSd865), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, TheMq65IsOn)
{
	EXPECT_EQ(Resolve(true, false, true, kMq65), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, TheDimensity8300IsOn)
{
	EXPECT_EQ(Resolve(true, false, true, kDimensity8300), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, TheM2IsOn)
{
	EXPECT_EQ(Resolve(true, false, true, kM2), IopThreadMode::Threaded);
	EXPECT_EQ(Resolve(true, false, true, kM2Max), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, TheRk3562ResolvesThreadedOff)
{
	// Four cores, all alike: 4 performance cores are enough for four threads, but there are no
	// two spare cores for the OS, the GS back thread and the audio callback.
	EXPECT_EQ(Resolve(true, false, true, kRk3562), IopThreadMode::Off);
	// Without MTVU it needs 3 performance cores and 5 in total, and still has 4.
	EXPECT_EQ(Resolve(true, false, false, kRk3562), IopThreadMode::Off);
}

TEST(IopThreadModeDevices, TheRk3562StillRunsLockstep)
{
	// The core rule protects a game from a thread it has no core for. Lockstep is not played on;
	// it runs the two threads in strict turns, so it needs no spare core.
	EXPECT_EQ(Resolve(true, true, true, kRk3562), IopThreadMode::Lockstep);
	EXPECT_EQ(Resolve(true, true, false, kRk3562), IopThreadMode::Lockstep);
}

TEST(IopThreadModeDevices, TheSd662ClassIsOnButUntested)
{
	// It passes the core rule, so Threaded is allowed; it has not been run on that hardware.
	EXPECT_EQ(Resolve(true, false, true, kSd662), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, TheCoreRuleCountsTheVuThread)
{
	// threads = EE + GS + IOP, plus one when MTVU is on, and two cores more for the rest.
	const HostCpuInfo five_cores = Host(true, 5, 3);
	EXPECT_EQ(Resolve(true, false, false, five_cores), IopThreadMode::Threaded); // needs 5 cores in total
	EXPECT_EQ(Resolve(true, false, true, five_cores), IopThreadMode::Off); // needs 6 in total
	EXPECT_EQ(Resolve(true, false, true, Host(true, 6, 3)), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, ATierNarrowerThanTheThreadsDoesNotSwitchItOff)
{
	// The RG 55G1: two big cores and six little ones. The IOP thread runs on the little ones (SlowCoresMask),
	// so the big cores are not needed for it, with MTVU on or off.
	EXPECT_EQ(Resolve(true, false, true, kRg55g1), IopThreadMode::Threaded);
	EXPECT_EQ(Resolve(true, false, false, kRg55g1), IopThreadMode::Threaded);
	// A tier of three, MTVU on: four threads on three fast cores.
	EXPECT_EQ(Resolve(true, false, true, Host(true, 8, 3)), IopThreadMode::Threaded);
	// Two fast cores is the narrowest tier that is enough.
	EXPECT_EQ(Resolve(true, false, true, Host(true, 8, 2)), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, TheTierNeedsAtLeastTwoPerformanceCores)
{
	EXPECT_EQ(IopThread::kMinPerformanceCores, 2u);

	// One fast core: the EE and GS threads cannot both have one. Off with MTVU on or off, on a host with cores to spare.
	EXPECT_EQ(Resolve(true, false, true, Host(true, 8, 1)), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, false, Host(true, 8, 1)), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, true, Host(true, 16, 1)), IopThreadMode::Off);
	// The same host with a second fast core resolves.
	EXPECT_EQ(Resolve(true, false, true, Host(true, 8, 2)), IopThreadMode::Threaded);
	EXPECT_EQ(Resolve(true, false, false, Host(true, 8, 2)), IopThreadMode::Threaded);
	// Nothing to count is no tier either.
	EXPECT_EQ(Resolve(true, false, false, Host(true, 8, 0)), IopThreadMode::Off);
}

TEST(IopThreadModeDevices, TheTwoCoreTierDoesNotWaiveTheTotalCoreRule)
{
	// Two fast cores and too few cores in total is still Off: the two rules are independent.
	EXPECT_EQ(Resolve(true, false, false, Host(true, 4, 2)), IopThreadMode::Off); // 3 threads need 5 cores
	EXPECT_EQ(Resolve(true, false, true, Host(true, 5, 2)), IopThreadMode::Off); // 4 threads need 6 cores
	EXPECT_EQ(Resolve(true, false, true, Host(true, 6, 2)), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, TheLockstepModeIgnoresTheTwoCoreTierToo)
{
	EXPECT_EQ(Resolve(true, true, true, Host(true, 8, 1)), IopThreadMode::Lockstep);
	EXPECT_EQ(Resolve(true, true, true, Host(true, 2, 1)), IopThreadMode::Lockstep);
}

TEST(IopThreadModeDevices, AHostWhoseCoresAreAllAlikeIsJudgedByItsCoreCountAlone)
{
	// GetPerformanceCoresMask() puts every core of at least 0.75 of the fastest into the tier, so on a part with one
	// core speed the tier is every core. It is only 0 when no core's speed can be read.
	//
	// RK3566 (4 x A55) and RK3562 (4 x A53): the tier holds four cores, but four cores leave nothing over once the
	// threads have theirs (3 threads need 5 cores, 4 need 6).
	EXPECT_EQ(Resolve(true, false, true, kRk3562), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, false, kRk3562), IopThreadMode::Off);
	// Five alike cores: enough without MTVU, not with it.
	EXPECT_EQ(Resolve(true, false, false, Host(true, 5, 5)), IopThreadMode::Threaded);
	EXPECT_EQ(Resolve(true, false, true, Host(true, 5, 5)), IopThreadMode::Off);
	// Six and eight alike cores resolve, with MTVU on or off. The tier holds all four threads, so nothing is placed
	// outside it.
	EXPECT_EQ(Resolve(true, false, true, Host(true, 6, 6)), IopThreadMode::Threaded);
	EXPECT_EQ(Resolve(true, false, true, kUniform8), IopThreadMode::Threaded);
	EXPECT_EQ(Resolve(true, false, false, kUniform8), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, ANarrowTierStillNeedsTheCoresForTheRest)
{
	// Two big and two little cores: the tier is fine to read, but four cores leave nothing for the OS, the GS back
	// thread and the audio callback.
	EXPECT_EQ(Resolve(true, false, false, Host(true, 4, 2)), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, false, Host(true, 5, 2)), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, TheCoreRuleNeedsTwoSpareCoresBeyondTheThreads)
{
	EXPECT_EQ(Resolve(true, false, true, Host(true, 5, 4)), IopThreadMode::Off); // 4 threads need 6 cores
	EXPECT_EQ(Resolve(true, false, true, Host(true, 6, 4)), IopThreadMode::Threaded);
	EXPECT_EQ(Resolve(true, false, false, Host(true, 4, 3)), IopThreadMode::Off); // 3 threads need 5 cores
	EXPECT_EQ(Resolve(true, false, false, Host(true, 5, 3)), IopThreadMode::Threaded);
}

TEST(IopThreadModeDevices, AHostThatCannotReadItsCoreSpeedsIsOff)
{
	// GetPerformanceCoresMask() returns 0 when no core's speed can be read (and on a host with no capacity source, such
	// as macOS or Windows). Cores that merely look alike are not this case: see the test above.
	EXPECT_EQ(Resolve(true, false, true, Host(true, 8, 0)), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, true, Host(true, 0, 0)), IopThreadMode::Off);
}

TEST(IopThreadModeDevices, LockstepIgnoresTheCoreRuleOnEveryHost)
{
	// Lockstep is a reference mode, so neither the core count nor the core type switches it off.
	// The first three rows resolve either way.
	EXPECT_EQ(Resolve(true, true, true, kSd865), IopThreadMode::Lockstep);
	EXPECT_EQ(Resolve(true, true, true, kM2Max), IopThreadMode::Lockstep);
	EXPECT_EQ(Resolve(true, true, true, kM2), IopThreadMode::Lockstep);
	// The rows below are Off for Threaded and must not be for Lockstep.
	EXPECT_EQ(Resolve(true, true, true, Host(true, 4, 4)), IopThreadMode::Lockstep); // RK3562 shape
	EXPECT_EQ(Resolve(true, true, true, Host(true, 5, 4)), IopThreadMode::Lockstep); // one core short of 6
	EXPECT_EQ(Resolve(true, true, true, Host(true, 8, 0)), IopThreadMode::Lockstep); // cores not told apart
	EXPECT_EQ(Resolve(true, true, true, Host(true, 1, 1)), IopThreadMode::Lockstep); // a single core
	EXPECT_EQ(Resolve(true, true, true, Host(true, 0, 0)), IopThreadMode::Lockstep); // topology unreadable
}

TEST(IopThreadModeDevices, TheSameHostsAreOffForThreaded)
{
	// The counterpart of the test above: the hosts Lockstep accepts anyway really do fail the rule
	// for Threaded, so the difference between the two modes is the mode and nothing else.
	EXPECT_EQ(Resolve(true, false, true, Host(true, 4, 4)), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, true, Host(true, 5, 4)), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, true, Host(true, 8, 0)), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, true, Host(true, 1, 1)), IopThreadMode::Off);
	EXPECT_EQ(Resolve(true, false, true, Host(true, 0, 0)), IopThreadMode::Off);
}

TEST(IopThreadModeDevices, EveryForcingConditionStillTurnsLockstepOffOnAnyHost)
{
	// Lockstep escapes the core rules only. A condition about the VM is a reason the IOP cannot
	// run on its own thread at all, and it holds on the hosts Lockstep is otherwise free on.
	const HostCpuInfo hosts[] = {kSd865, kRk3562, Host(false, 16, 8), Host(true, 0, 0)};
	for (const HostCpuInfo& host : hosts)
	{
		VmConditions vm;
		vm.ps1_mode = true;
		EXPECT_EQ(Resolve(true, true, true, host, vm), IopThreadMode::Off);

		vm = {};
		vm.arcade = true;
		EXPECT_EQ(Resolve(true, true, true, host, vm), IopThreadMode::Off);

		vm = {};
		vm.gs_dump_replay = true;
		EXPECT_EQ(Resolve(true, true, true, host, vm), IopThreadMode::Off);

		vm = {};
		vm.iop_breakpoints = true;
		EXPECT_EQ(Resolve(true, true, true, host, vm), IopThreadMode::Off);

		vm = {};
		vm.recompiler_test_harness = true;
		EXPECT_EQ(Resolve(true, true, true, host, vm), IopThreadMode::Off);

		// And with none of them it resolves on an ARM64 host, so the Off results above are the
		// conditions' doing. A non-ARM64 host is Off either way (the architecture rule).
		EXPECT_EQ(Resolve(true, true, true, host, VmConditions()),
			host.is_arm64 ? IopThreadMode::Lockstep : IopThreadMode::Off);
	}
}

TEST(IopThreadModeDevices, LockstepStillNeedsTheEnableFlag)
{
	// Escaping the host rules does not make Lockstep a setting of its own.
	EXPECT_EQ(Resolve(false, true, true, kRk3562), IopThreadMode::Off);
	EXPECT_EQ(Resolve(false, true, true, Host(false, 1, 1)), IopThreadMode::Off);
}

TEST(IopThreadModeLag, TheBoundsAreStrictlyAboveTheEventTestSpacing)
{
	// The EE checks the lag once per event test, and event tests are at most kEeWaitCycles apart.
	// A bound at or below that finds the IOP behind on nearly every check.
	static_assert(IopThread::kMinLag > IopThread::kEeWaitCycles);
	static_assert(IopThread::kDefaultLag > IopThread::kEeWaitCycles);
	static_assert(IopThread::kMinLag <= IopThread::kDefaultLag);
	static_assert(IopThread::kDefaultLag <= IopThread::kMaxLag);
	EXPECT_EQ(IopThread::kEeWaitCycles, 3072);
}

TEST(IopThreadModeLag, TheMinimumLeavesRoomForTheOvershootOfBothThreads)
{
	// 90th-percentile overshoot on the SD865 and MQ65: an event test lands up to 81 cycles past its
	// target, and the IOP ends its last block up to 57 cycles past what it was given. The floor keeps
	// a margin of at least seven times their sum.
	constexpr int kEeOvershootP90 = 81;
	constexpr int kIopOvershootP90 = 57;
	EXPECT_GE(IopThread::kMinLag - IopThread::kEeWaitCycles, 7 * (kEeOvershootP90 + kIopOvershootP90));
}

TEST(IopThreadModeLag, TheMaximumIsTwoFramesAndTheEeToIopQueueHoldsAWindowOfPosts)
{
	// The maximum is two NTSC frames of EE cycles. A window of that length holds the blanking posts of
	// the busiest title seen (1052 per frame on OutRun) plus a few dozen SIF items per frame, and the
	// queue has to hold all of them with room to spare: the lag keeps the EE from running further ahead
	// than the window, and a full queue stalls the EE.
	constexpr double kEeCyclesPerFrame = 294912000.0 / 60.0;
	constexpr double kPostsPerFrame = 1052.0 + 64.0;
	constexpr double kQueueEntries = static_cast<double>(IopThread::kE2IRingCapacity);
	EXPECT_EQ(IopThread::kMaxLag, 3200 * IopThread::kEeWaitCycles);
	EXPECT_DOUBLE_EQ(IopThread::kMaxLag / kEeCyclesPerFrame, 2.0);
	EXPECT_LT(IopThread::kMaxLag / kEeCyclesPerFrame * kPostsPerFrame, kQueueEntries / 3.0);
	// The largest delay before the EE sees an IOP effect is the lag plus one event-test spacing.
	EXPECT_LT((IopThread::kMaxLag + IopThread::kEeWaitCycles) / 294912000.0, 0.034);
}

TEST(IopThreadModeLag, TheMaximumLeavesS32Headroom)
{
	// The lag is an s32 and is added to cycle differences (EEsCycle is an s32 too).
	EXPECT_EQ(IopThread::kMaxLag % IopThread::kEeWaitCycles, 0);
	EXPECT_LT(static_cast<s64>(IopThread::kMaxLag) * 64, static_cast<s64>(INT32_MAX));
}

TEST(IopThreadModeLag, ValuesAtOrBelowTheEventTestSpacingAreRaisedToTheMinimum)
{
	EXPECT_EQ(ClampLag(IopThread::kEeWaitCycles), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(IopThread::kEeWaitCycles + 1), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(3200), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(IopThread::kMinLag - 1), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(512), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(1), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(0), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(-1), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(INT_MIN), IopThread::kMinLag);
}

TEST(IopThreadModeLag, NoInputBelowTheMinimumSurvivesAsALagAtOrBelowTheEventTestSpacing)
{
	for (int requested = -2; requested <= IopThread::kMinLag + 1; requested++)
	{
		const int lag = ClampLag(requested);
		ASSERT_GT(lag, IopThread::kEeWaitCycles) << "requested " << requested;
		ASSERT_GE(lag, IopThread::kMinLag) << "requested " << requested;
	}
}

TEST(IopThreadModeLag, ValuesInRangeAreKept)
{
	EXPECT_EQ(ClampLag(IopThread::kMinLag), IopThread::kMinLag);
	EXPECT_EQ(ClampLag(IopThread::kMinLag + 1), IopThread::kMinLag + 1);
	EXPECT_EQ(ClampLag(IopThread::kDefaultLag), 24576);
	EXPECT_EQ(ClampLag(IopThread::kDefaultSettingLag), 4915200);
	EXPECT_EQ(ClampLag(12288), 12288);
	EXPECT_EQ(ClampLag(196608), 196608); // the old maximum is an ordinary value now
	EXPECT_EQ(ClampLag(4900000), 4900000); // about one frame
	EXPECT_EQ(ClampLag(IopThread::kMaxLag - 1), IopThread::kMaxLag - 1);
	EXPECT_EQ(ClampLag(IopThread::kMaxLag), IopThread::kMaxLag);
}

TEST(IopThreadModeLag, ValuesAboveTheMaximumAreLowered)
{
	EXPECT_EQ(ClampLag(IopThread::kMaxLag + 1), IopThread::kMaxLag);
	EXPECT_EQ(ClampLag(IopThread::kMaxLag * 2), IopThread::kMaxLag);
	EXPECT_EQ(ClampLag(20000000), IopThread::kMaxLag);
	EXPECT_EQ(ClampLag(INT_MAX), IopThread::kMaxLag);
}

TEST(IopThreadModeLag, TheSettingsFileIsClampedWhenItIsRead)
{
	EXPECT_EQ(LoadLag(6144), 6144);
	EXPECT_EQ(LoadLag(12288), 12288);
	EXPECT_EQ(LoadLag(3072), IopThread::kMinLag);
	EXPECT_EQ(LoadLag(0), IopThread::kMinLag);
	EXPECT_EQ(LoadLag(-5), IopThread::kMinLag);
	EXPECT_EQ(LoadLag(196608), 196608);
	EXPECT_EQ(LoadLag(INT_MAX), IopThread::kMaxLag);
}

TEST(IopThreadModeLag, ALagTheFileDoesNotNameIsTheDefault)
{
	MemorySettingsInterface si;
	si.SetBoolValue("EmuCore/Speedhacks", "iopThread", true);
	EXPECT_EQ(LoadHacks(si).iopThreadLag, IopThread::kDefaultSettingLag);
}

TEST(IopThreadModeLag, ALagTheFileNamesStaysWhateverTheDefaultIs)
{
	// A settings file written while 24576 was the default has that value in it, and loads as 24576.
	EXPECT_EQ(LoadLag(24576), 24576);
}

TEST(IopThreadSetting, TheKeysLiveInTheSpeedhacksSectionAndRoundTrip)
{
	Pcsx2Config::SpeedhackOptions hacks;
	hacks.iopThread = true;
	hacks.iopThreadLockstep = true;
	hacks.iopThreadDeferSif = true;
	hacks.iopThreadLag = 9000;

	MemorySettingsInterface si;
	{
		SettingsSaveWrapper wrap(si);
		hacks.LoadSave(wrap);
	}

	bool enabled = false, lockstep = false, defer_sif = false;
	s32 lag = 0;
	ASSERT_TRUE(si.GetBoolValue("EmuCore/Speedhacks", "iopThread", &enabled));
	ASSERT_TRUE(si.GetBoolValue("EmuCore/Speedhacks", "iopThreadLockstep", &lockstep));
	ASSERT_TRUE(si.GetBoolValue("EmuCore/Speedhacks", "iopThreadDeferSif", &defer_sif));
	ASSERT_TRUE(si.GetIntValue("EmuCore/Speedhacks", "iopThreadLag", &lag));
	EXPECT_TRUE(enabled);
	EXPECT_TRUE(lockstep);
	EXPECT_TRUE(defer_sif);
	EXPECT_EQ(lag, 9000);

	const Pcsx2Config::SpeedhackOptions loaded = LoadHacks(si);
	EXPECT_TRUE(loaded.iopThread);
	EXPECT_TRUE(loaded.iopThreadLockstep);
	EXPECT_TRUE(loaded.iopThreadDeferSif);
	EXPECT_EQ(loaded.iopThreadLag, 9000);
}

TEST(IopThreadSetting, TheKeysAreReadFromTheFile)
{
	MemorySettingsInterface si;
	si.SetBoolValue("EmuCore/Speedhacks", "iopThread", true);
	si.SetBoolValue("EmuCore/Speedhacks", "iopThreadLockstep", true);
	si.SetBoolValue("EmuCore/Speedhacks", "iopThreadDeferSif", true);
	const Pcsx2Config::SpeedhackOptions hacks = LoadHacks(si);
	EXPECT_TRUE(hacks.iopThread);
	EXPECT_TRUE(hacks.iopThreadLockstep);
	EXPECT_TRUE(hacks.iopThreadDeferSif);
}

TEST(IopThreadSetting, ASetFlagThatTheFileLeavesOutStaysSetLikeTheOtherSpeedhacks)
{
	// Every SettingsWrapBitBool key keeps the value it had when the key is missing. This is the
	// behaviour of vuThread as well, and the reason a settings load starts from a fresh config.
	Pcsx2Config::SpeedhackOptions hacks;
	hacks.iopThread = true;
	MemorySettingsInterface si;
	SettingsLoadWrapper wrap(si);
	hacks.LoadSave(wrap);
	EXPECT_TRUE(hacks.iopThread);
}

TEST(IopThreadSetting, EachKeyTakesPartInEquality)
{
	const Pcsx2Config::SpeedhackOptions base;

	Pcsx2Config::SpeedhackOptions a = base;
	a.iopThread = true;
	EXPECT_TRUE(a != base);

	Pcsx2Config::SpeedhackOptions b = base;
	b.iopThreadLockstep = true;
	EXPECT_TRUE(b != base);

	Pcsx2Config::SpeedhackOptions d = base;
	d.iopThreadDeferSif = true;
	EXPECT_TRUE(d != base);

	Pcsx2Config::SpeedhackOptions c = base;
	c.iopThreadLag = base.iopThreadLag + 1;
	EXPECT_TRUE(c != base);

	EXPECT_TRUE(Pcsx2Config::SpeedhackOptions() == base);
}

TEST(IopThreadSetting, DisableAllTurnsTheFeatureOff)
{
	Pcsx2Config::SpeedhackOptions hacks;
	hacks.iopThread = true;
	hacks.iopThreadLockstep = true;
	hacks.iopThreadDeferSif = true;
	hacks.DisableAll();
	EXPECT_FALSE(hacks.iopThread);
	EXPECT_FALSE(hacks.iopThreadLockstep);
	EXPECT_FALSE(hacks.iopThreadDeferSif);
}

TEST(IopThreadSetting, NoOtherSpeedhackBitMoved)
{
	// JIT code tests these bits of Speedhacks.bitset by position (microVU-arm64.cpp, the
	// EECycleSkip block of the VU1 epilogue tests bit 4 for vuThread). The new flags must sit after
	// every existing one.
	auto only = [](auto set) {
		Pcsx2Config::SpeedhackOptions hacks;
		hacks.DisableAll();
		set(hacks);
		return hacks.bitset;
	};

	EXPECT_EQ(only([](auto& h) { h.fastCDVD = true; }), 1u << 0);
	EXPECT_EQ(only([](auto& h) { h.IntcStat = true; }), 1u << 1);
	EXPECT_EQ(only([](auto& h) { h.WaitLoop = true; }), 1u << 2);
	EXPECT_EQ(only([](auto& h) { h.vuFlagHack = true; }), 1u << 3);
	EXPECT_EQ(only([](auto& h) { h.vuThread = true; }), 1u << 4);
	EXPECT_EQ(only([](auto& h) { h.vu1Instant = true; }), 1u << 5);
	EXPECT_EQ(only([](auto& h) { h.iopThread = true; }), 1u << 6);
	EXPECT_EQ(only([](auto& h) { h.iopThreadLockstep = true; }), 1u << 7);
	EXPECT_EQ(only([](auto& h) { h.DynamicEECycleRate = true; }), 1u << 8);
	EXPECT_EQ(only([](auto& h) { h.iopThreadDeferSif = true; }), 1u << 9);
}

TEST(IopThreadSetting, TheSifDeferralIsOffByDefaultAndHasNoGameDatabaseName)
{
	EXPECT_FALSE(Pcsx2Config::SpeedhackOptions().iopThreadDeferSif);
	// INI only: not one of the SpeedHack ids a game entry or a per-game override can carry.
	EXPECT_FALSE(Pcsx2Config::SpeedhackOptions::ParseSpeedHackName("iopThreadDeferSif").has_value());
}

TEST(IopThreadGameDB, TheHackHasAnIniNameAndAGameDatabaseName)
{
	EXPECT_STREQ(Pcsx2Config::SpeedhackOptions::GetSpeedHackName(SpeedHack::IOPThread), "iopThread");
	const std::optional<SpeedHack> parsed = Pcsx2Config::SpeedhackOptions::ParseSpeedHackName("iopThread");
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed.value(), SpeedHack::IOPThread);
	EXPECT_STREQ(PerGameOverrideKeys::ForSpeedHack(SpeedHack::IOPThread), "iopThread");
}

TEST(IopThreadGameDB, SetWritesTheEnableFlagOnly)
{
	Pcsx2Config::SpeedhackOptions hacks;
	hacks.Set(SpeedHack::IOPThread, 1);
	EXPECT_TRUE(hacks.iopThread);
	EXPECT_FALSE(hacks.iopThreadLockstep);
	EXPECT_EQ(hacks.iopThreadLag, IopThread::kDefaultSettingLag);
	hacks.Set(SpeedHack::IOPThread, 0);
	EXPECT_FALSE(hacks.iopThread);
}

TEST(IopThreadGameDB, AnEntryCanOptATitleOut)
{
	GameDatabaseSchema::GameEntry entry;
	entry.speedHacks.emplace_back(SpeedHack::IOPThread, 0);

	Pcsx2Config config = Requested(true, false, true);
	ASSERT_EQ(ResolveMode(config, kSd865, {}), IopThreadMode::Threaded);

	entry.applyGameFixes(config, true, {}, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_FALSE(config.Speedhacks.iopThread);
	EXPECT_EQ(ResolveMode(config, kSd865, {}), IopThreadMode::Off);
}

TEST(IopThreadGameDB, APlayersPerGameChoiceBeatsTheEntry)
{
	GameDatabaseSchema::GameEntry entry;
	entry.speedHacks.emplace_back(SpeedHack::IOPThread, 0);

	MemorySettingsInterface game_file;
	game_file.SetBoolValue("EmuCore/Speedhacks", "iopThread", true);
	const PerGameOverrides ov = ComputePerGameOverrides(game_file);
	ASSERT_TRUE(ov.Has(SpeedHack::IOPThread));

	Pcsx2Config config = Requested(true, false, true);
	entry.applyGameFixes(config, true, ov, GameDatabaseSchema::ApplyMode::Hypothetical);

	EXPECT_TRUE(config.Speedhacks.iopThread);
}

TEST(IopThreadGameDB, OnlyTheEnableKeyIsClaimedByAPerGameFile)
{
	// The lock-step and lag keys are not game database settings.
	MemorySettingsInterface game_file;
	game_file.SetBoolValue("EmuCore/Speedhacks", "iopThreadLockstep", true);
	game_file.SetIntValue("EmuCore/Speedhacks", "iopThreadLag", 9000);
	EXPECT_FALSE(ComputePerGameOverrides(game_file).Any());
}
