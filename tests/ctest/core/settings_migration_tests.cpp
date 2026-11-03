// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// VMManager::Internal::MigrateSettings: an existing settings file that was written before two IOP thread defaults changed
// is moved once, each move with a mark of its own.
//   - A lag of 24576 (the old default) becomes the one-frame default, on every host.
//   - On a host where the IOP thread now starts on (a performance tier of two or three cores, and ResolveMode can run
//     it), a saved iopThread of false becomes true. A host with four or more fast cores, or one that cannot run the
//     thread, keeps what the file says.
// A file made by SetDefaultSettings is already marked, and whatever the user chooses after a move stays. The tests say
// which device they are, so none of them depends on the machine that runs them.
//
// In its own file because VMManager.h brings in the Host namespace, which iop_thread_mode_tests.cpp's device helper of
// the same name would collide with.

#include <gtest/gtest.h>

#include "Config.h"
#include "IopThread/IopThreadMode.h"
#include "IopThread/IopThreadVm.h"
#include "VMManager.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

namespace
{
	Pcsx2Config::SpeedhackOptions LoadHacks(MemorySettingsInterface& si)
	{
		Pcsx2Config::SpeedhackOptions hacks;
		SettingsLoadWrapper wrap(si);
		hacks.LoadSave(wrap);
		return hacks;
	}

	IopThread::HostCpuInfo MakeHost(bool arm64, u32 total_cores, u32 performance_cores)
	{
		IopThread::HostCpuInfo host;
		host.is_arm64 = arm64;
		host.total_cores = total_cores;
		host.performance_cores = performance_cores;
		return host;
	}

	// A wide host (1 prime + 3 A77 + 4 A55): the thread does not start on here, so only the lag move can show.
	const IopThread::HostCpuInfo kSd865 = MakeHost(true, 8, 4);
	// Narrow hosts: two fast cores (RG 55G1: 2 A78 + 6 A55; the MQ65 with two cores offline: 2 A73 + 4 A53) and three.
	const IopThread::HostCpuInfo kRg55g1 = MakeHost(true, 8, 2);
	const IopThread::HostCpuInfo kMq65TwoFast = MakeHost(true, 6, 2);
	const IopThread::HostCpuInfo kThreeFast = MakeHost(true, 8, 3);

	constexpr const char* kSpeed = "EmuCore/Speedhacks";
	constexpr const char* kLagMark = "IopThreadLagMigrated";
	constexpr const char* kTierMark = "IopThreadNarrowTierMigrated";

	// A settings file made while the lag default was 24576 and the thread was off by default: SetDefaultSettings wrote
	// every key, so the IOP thread flag is in it too, off.
	void WriteOldFile(MemorySettingsInterface& si, int lag = 24576, bool iop_thread = false, bool mtvu = true)
	{
		si.SetUIntValue("UI", "SettingsVersion", 1);
		si.SetBoolValue(kSpeed, "iopThread", iop_thread);
		si.SetIntValue(kSpeed, "iopThreadLag", lag);
		si.SetBoolValue(kSpeed, "vuThread", mtvu);
	}

	bool Migrate(MemorySettingsInterface& si, const IopThread::HostCpuInfo& host = kSd865)
	{
		return VMManager::Internal::MigrateSettings(si, host);
	}
} // namespace

// ---------------------------------------------------------------- the lag

TEST(IopThreadSettingsMigration, AnOldFileGetsTheOneFrameLag)
{
	MemorySettingsInterface si;
	WriteOldFile(si);

	EXPECT_TRUE(Migrate(si)) << "it changed the file, so the caller saves it";

	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), IopThread::kDefaultSettingLag);
	EXPECT_EQ(LoadHacks(si).iopThreadLag, 4915200);
}

TEST(IopThreadSettingsMigration, TheLagMoveDoesNotTouchTheThreadSetting)
{
	// On a wide host the thread is off by default and the lag move is no reason to turn it on: off stays off, and on (a
	// user's choice) stays on.
	for (const bool before : {false, true})
	{
		MemorySettingsInterface si;
		WriteOldFile(si, 24576, before);
		Migrate(si);
		EXPECT_EQ(si.GetBoolValue(kSpeed, "iopThread", !before), before);
		EXPECT_EQ(LoadHacks(si).iopThread, before);
	}
}

TEST(IopThreadSettingsMigration, ItRunsOnceAndALagPickedAfterwardsStays)
{
	MemorySettingsInterface si;
	WriteOldFile(si);
	ASSERT_TRUE(Migrate(si));

	// The user picks 24576 on purpose.
	si.SetIntValue(kSpeed, "iopThreadLag", 24576);

	EXPECT_FALSE(Migrate(si)) << "it changed nothing the second time";
	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), 24576);
}

TEST(IopThreadSettingsMigration, AnyOtherLagTheUserPickedIsUntouched)
{
	// 6144 is the first default there was, and the Android picker offers it; 4915200 is the new default already.
	for (const int lag : {4096, 6144, 12288, 24575, 24577, 196608, 4915200, 9000000})
	{
		MemorySettingsInterface si;
		WriteOldFile(si, lag);
		Migrate(si);
		EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), lag) << lag;
	}
}

TEST(IopThreadSettingsMigration, KeysTheFileDoesNotHaveAreNotCreated)
{
	// A file that never named the lag loads it as the new default, so there is nothing to move. The file is still
	// marked, so it counts as done.
	MemorySettingsInterface si;
	si.SetUIntValue("UI", "SettingsVersion", 1);
	EXPECT_TRUE(Migrate(si));
	EXPECT_FALSE(si.ContainsValue(kSpeed, "iopThreadLag"));
	EXPECT_FALSE(si.ContainsValue(kSpeed, "iopThread"));
	EXPECT_FALSE(Migrate(si));

	const Pcsx2Config::SpeedhackOptions hacks = LoadHacks(si);
	EXPECT_FALSE(hacks.iopThread);
	EXPECT_EQ(hacks.iopThreadLag, IopThread::kDefaultSettingLag);
}

TEST(IopThreadSettingsMigration, OtherKeysAreNotTouched)
{
	MemorySettingsInterface si;
	WriteOldFile(si);
	si.SetBoolValue(kSpeed, "iopThreadLockstep", false);
	si.SetBoolValue(kSpeed, "vuThread", false);
	si.SetIntValue(kSpeed, "EECycleRate", 2);
	Migrate(si);
	EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThreadLockstep", true));
	EXPECT_FALSE(si.GetBoolValue(kSpeed, "vuThread", true));
	EXPECT_EQ(si.GetIntValue(kSpeed, "EECycleRate", 0), 2);
}

TEST(IopThreadSettingsMigration, AFileMadeByTheDefaultsIsAlreadyDone)
{
	MemorySettingsInterface si;
	VMManager::SetDefaultSettings(si, false, true, false, false, false);

	// The current defaults, written to the file, and both marks.
	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), IopThread::kDefaultSettingLag);
	EXPECT_TRUE(si.GetBoolValue("UI", kLagMark, false));
	EXPECT_TRUE(si.GetBoolValue("UI", kTierMark, false));

	// The IOP thread default is the host's: whatever the rule says for the machine running this test, with the MTVU
	// setting the same call wrote. (The hosts the rule picks on are described in the tests below.)
	ASSERT_TRUE(si.ContainsValue(kSpeed, "iopThread"));
	const bool mtvu = si.GetBoolValue(kSpeed, "vuThread", false);
	EXPECT_EQ(si.GetBoolValue(kSpeed, "iopThread", !IopThread::DefaultOnForHost(IopThread::CurrentHostInfo(), mtvu)),
		IopThread::DefaultOnForHost(IopThread::CurrentHostInfo(), mtvu));

	EXPECT_FALSE(Migrate(si, kRg55g1));

	// So a user who then picks 24576, or turns the thread off, is not moved at the next start.
	si.SetIntValue(kSpeed, "iopThreadLag", 24576);
	si.SetBoolValue(kSpeed, "iopThread", false);
	EXPECT_FALSE(Migrate(si, kRg55g1));
	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), 24576);
	EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThread", true));
}

TEST(IopThreadSettingsMigration, DefaultingOnlyTheControllersDoesNotMarkTheFile)
{
	// The controller settings window defaults the pads and hotkeys of an existing file, and nothing else
	// (ControllerSettingsWindow.cpp). That says nothing about the core settings, so an old file that has not been moved
	// yet must still be moved afterwards, in both ways.
	MemorySettingsInterface si;
	WriteOldFile(si);
	VMManager::SetDefaultSettings(si, false, false, true, true, false);
	EXPECT_FALSE(si.ContainsValue("UI", kLagMark));
	EXPECT_FALSE(si.ContainsValue("UI", kTierMark));
	EXPECT_TRUE(Migrate(si, kRg55g1));
	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), IopThread::kDefaultSettingLag);
	EXPECT_TRUE(si.GetBoolValue(kSpeed, "iopThread", false));
}

// ---------------------------------------------------------------- the thread on a narrow tier

TEST(IopThreadNarrowTierMigration, ASavedOffBecomesOnOnAHostWithTwoOrThreeFastCores)
{
	for (const IopThread::HostCpuInfo& host : {kRg55g1, kMq65TwoFast, kThreeFast})
	{
		MemorySettingsInterface si;
		WriteOldFile(si);
		EXPECT_TRUE(Migrate(si, host)) << host.performance_cores << " fast cores";
		EXPECT_TRUE(si.GetBoolValue(kSpeed, "iopThread", false)) << host.performance_cores << " fast cores";
		EXPECT_TRUE(LoadHacks(si).iopThread);
	}
}

TEST(IopThreadNarrowTierMigration, ASavedOffStaysOffOnAHostWithFourOrMoreFastCores)
{
	// SD865, MQ65 with all cores, Dimensity 8300, M2 Max, and eight cores of one speed: each has a tier of four or more.
	for (const IopThread::HostCpuInfo& host : {kSd865, MakeHost(true, 12, 8), MakeHost(true, 8, 8), MakeHost(true, 16, 5)})
	{
		MemorySettingsInterface si;
		WriteOldFile(si);
		Migrate(si, host);
		EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThread", true)) << host.performance_cores << " fast cores";
		EXPECT_FALSE(LoadHacks(si).iopThread);
	}
}

TEST(IopThreadNarrowTierMigration, AHostWithOneFastCoreOrNoneIsLeftAlone)
{
	for (const IopThread::HostCpuInfo& host : {MakeHost(true, 8, 1), MakeHost(true, 8, 0), MakeHost(true, 2, 1)})
	{
		MemorySettingsInterface si;
		WriteOldFile(si);
		Migrate(si, host);
		EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThread", true)) << host.performance_cores << " fast cores";
	}
}

TEST(IopThreadNarrowTierMigration, AHostThatCannotRunTheThreadIsLeftAlone)
{
	// Two fast cores, but not ARM64, or too few cores in total: ResolveMode would say Off, so the setting is not turned on.
	for (const IopThread::HostCpuInfo& host : {MakeHost(false, 12, 2), MakeHost(true, 4, 2), MakeHost(true, 3, 2)})
	{
		MemorySettingsInterface si;
		WriteOldFile(si, 24576, false, false);
		Migrate(si, host);
		EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThread", true)) << host.total_cores << " cores, arm64=" << host.is_arm64;
	}
}

TEST(IopThreadNarrowTierMigration, TheFilesMtvuSettingCountsInTheHostRule)
{
	// Five cores: three threads (EE, GS, IOP) need five, four with MTVU need six.
	const IopThread::HostCpuInfo five_cores = MakeHost(true, 5, 2);
	MemorySettingsInterface without_mtvu;
	WriteOldFile(without_mtvu, 24576, false, false);
	Migrate(without_mtvu, five_cores);
	EXPECT_TRUE(without_mtvu.GetBoolValue(kSpeed, "iopThread", false));

	MemorySettingsInterface with_mtvu;
	WriteOldFile(with_mtvu, 24576, false, true);
	Migrate(with_mtvu, five_cores);
	EXPECT_FALSE(with_mtvu.GetBoolValue(kSpeed, "iopThread", true));
}

TEST(IopThreadNarrowTierMigration, ASavedOnStaysOnAndMissingKeysAreFilledInOnlyOnANarrowHost)
{
	// On: stays on, on any host.
	for (const IopThread::HostCpuInfo& host : {kRg55g1, kSd865})
	{
		MemorySettingsInterface si;
		WriteOldFile(si, 24576, true);
		Migrate(si, host);
		EXPECT_TRUE(si.GetBoolValue(kSpeed, "iopThread", false));
	}

	// A file from before the setting existed loads as off. On a narrow host that is the stored default too, so it is
	// moved; on a wide host nothing is written.
	MemorySettingsInterface narrow;
	narrow.SetUIntValue("UI", "SettingsVersion", 1);
	narrow.SetBoolValue(kSpeed, "vuThread", true);
	Migrate(narrow, kRg55g1);
	EXPECT_TRUE(narrow.GetBoolValue(kSpeed, "iopThread", false));

	MemorySettingsInterface wide;
	wide.SetUIntValue("UI", "SettingsVersion", 1);
	wide.SetBoolValue(kSpeed, "vuThread", true);
	Migrate(wide, kSd865);
	EXPECT_FALSE(wide.ContainsValue(kSpeed, "iopThread"));
}

TEST(IopThreadNarrowTierMigration, ItRunsOnceAndALaterOffSticks)
{
	MemorySettingsInterface si;
	WriteOldFile(si);
	ASSERT_TRUE(Migrate(si, kRg55g1));
	ASSERT_TRUE(si.GetBoolValue(kSpeed, "iopThread", false));

	// The user turns it off again.
	si.SetBoolValue(kSpeed, "iopThread", false);

	EXPECT_FALSE(Migrate(si, kRg55g1)) << "nothing left to move";
	EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThread", true));
	EXPECT_TRUE(si.GetBoolValue("UI", kTierMark, false));
}

TEST(IopThreadNarrowTierMigration, ItIsMarkedOnAWideHostToo)
{
	// A file moved on a wide host is done: if the same file is later read by a narrow host (a shared card, a restored
	// backup), the saved off is respected, not moved.
	MemorySettingsInterface si;
	WriteOldFile(si);
	Migrate(si, kSd865);
	EXPECT_TRUE(si.GetBoolValue("UI", kTierMark, false));
	EXPECT_FALSE(Migrate(si, kRg55g1));
	EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThread", true));
}

TEST(IopThreadNarrowTierMigration, TheTwoMovesAreIndependent)
{
	// A file that had the lag moved (an earlier build) and not the tier mark: only the tier move runs.
	MemorySettingsInterface lag_done;
	WriteOldFile(lag_done, 24576, false);
	lag_done.SetBoolValue("UI", kLagMark, true);
	EXPECT_TRUE(Migrate(lag_done, kRg55g1));
	EXPECT_EQ(lag_done.GetIntValue(kSpeed, "iopThreadLag", 0), 24576) << "its lag mark was set, so the lag is a choice now";
	EXPECT_TRUE(lag_done.GetBoolValue(kSpeed, "iopThread", false));

	// The other way round: the tier mark set, the lag mark not.
	MemorySettingsInterface tier_done;
	WriteOldFile(tier_done, 24576, false);
	tier_done.SetBoolValue("UI", kTierMark, true);
	EXPECT_TRUE(Migrate(tier_done, kRg55g1));
	EXPECT_EQ(tier_done.GetIntValue(kSpeed, "iopThreadLag", 0), IopThread::kDefaultSettingLag);
	EXPECT_FALSE(tier_done.GetBoolValue(kSpeed, "iopThread", true));
}
