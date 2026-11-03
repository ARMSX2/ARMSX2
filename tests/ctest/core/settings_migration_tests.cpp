// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// VMManager::Internal::MigrateSettings: an existing settings file that was written while the IOP thread lag defaulted to
// 24576 gets the one-frame default once, a file made by SetDefaultSettings does not need it, and whatever the user chooses
// after the move stays. The IOP thread setting itself is off by default and is never moved.
//
// In its own file because VMManager.h brings in the Host namespace, which iop_thread_mode_tests.cpp's device helper of
// the same name would collide with.

#include <gtest/gtest.h>

#include "Config.h"
#include "IopThread/IopThreadMode.h"
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

	constexpr const char* kSpeed = "EmuCore/Speedhacks";

	// A settings file made while the lag default was 24576: SetDefaultSettings wrote every key, so the IOP thread flag is in
	// it too, off.
	void WriteOldFile(MemorySettingsInterface& si, int lag = 24576, bool iop_thread = false)
	{
		si.SetUIntValue("UI", "SettingsVersion", 1);
		si.SetBoolValue(kSpeed, "iopThread", iop_thread);
		si.SetIntValue(kSpeed, "iopThreadLag", lag);
	}
} // namespace

TEST(IopThreadSettingsMigration, AnOldFileGetsTheOneFrameLag)
{
	MemorySettingsInterface si;
	WriteOldFile(si);

	EXPECT_TRUE(VMManager::Internal::MigrateSettings(si)) << "it changed the file, so the caller saves it";

	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), IopThread::kDefaultSettingLag);
	EXPECT_EQ(LoadHacks(si).iopThreadLag, 4915200);
}

TEST(IopThreadSettingsMigration, TheThreadSettingItselfIsNeverMoved)
{
	// Off by default, and the lag move is no reason to turn it on: off stays off, and on (a user's choice) stays on.
	for (const bool before : {false, true})
	{
		MemorySettingsInterface si;
		WriteOldFile(si, 24576, before);
		VMManager::Internal::MigrateSettings(si);
		EXPECT_EQ(si.GetBoolValue(kSpeed, "iopThread", !before), before);
		EXPECT_EQ(LoadHacks(si).iopThread, before);
	}
}

TEST(IopThreadSettingsMigration, ItRunsOnceAndALagPickedAfterwardsStays)
{
	MemorySettingsInterface si;
	WriteOldFile(si);
	ASSERT_TRUE(VMManager::Internal::MigrateSettings(si));

	// The user picks 24576 on purpose.
	si.SetIntValue(kSpeed, "iopThreadLag", 24576);

	EXPECT_FALSE(VMManager::Internal::MigrateSettings(si)) << "it changed nothing the second time";
	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), 24576);
}

TEST(IopThreadSettingsMigration, AnyOtherLagTheUserPickedIsUntouched)
{
	// 6144 is the first default there was, and the Android picker offers it; 4915200 is the new default already.
	for (const int lag : {4096, 6144, 12288, 24575, 24577, 196608, 4915200, 9000000})
	{
		MemorySettingsInterface si;
		WriteOldFile(si, lag);
		VMManager::Internal::MigrateSettings(si);
		EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), lag) << lag;
	}
}

TEST(IopThreadSettingsMigration, KeysTheFileDoesNotHaveAreNotCreated)
{
	// A file that never named the lag loads it as the new default, so there is nothing to move. The file is still
	// marked, so it counts as done.
	MemorySettingsInterface si;
	si.SetUIntValue("UI", "SettingsVersion", 1);
	EXPECT_TRUE(VMManager::Internal::MigrateSettings(si));
	EXPECT_FALSE(si.ContainsValue(kSpeed, "iopThreadLag"));
	EXPECT_FALSE(si.ContainsValue(kSpeed, "iopThread"));
	EXPECT_FALSE(VMManager::Internal::MigrateSettings(si));

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
	VMManager::Internal::MigrateSettings(si);
	EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThreadLockstep", true));
	EXPECT_FALSE(si.GetBoolValue(kSpeed, "vuThread", true));
	EXPECT_EQ(si.GetIntValue(kSpeed, "EECycleRate", 0), 2);
}

TEST(IopThreadSettingsMigration, AFileMadeByTheDefaultsIsAlreadyDone)
{
	MemorySettingsInterface si;
	VMManager::SetDefaultSettings(si, false, true, false, false, false);

	// The current defaults, written to the file: the lag is a frame and the thread is off.
	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), IopThread::kDefaultSettingLag);
	EXPECT_FALSE(si.GetBoolValue(kSpeed, "iopThread", true));

	EXPECT_FALSE(VMManager::Internal::MigrateSettings(si));

	// So a user who then picks 24576 is not moved at the next start.
	si.SetIntValue(kSpeed, "iopThreadLag", 24576);
	EXPECT_FALSE(VMManager::Internal::MigrateSettings(si));
	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), 24576);
}

TEST(IopThreadSettingsMigration, DefaultingOnlyTheControllersDoesNotMarkTheFile)
{
	// The controller settings window defaults the pads and hotkeys of an existing file, and nothing else
	// (ControllerSettingsWindow.cpp). That says nothing about the core settings, so an old file that has not been moved
	// yet must still be moved afterwards.
	MemorySettingsInterface si;
	WriteOldFile(si);
	VMManager::SetDefaultSettings(si, false, false, true, true, false);
	EXPECT_TRUE(VMManager::Internal::MigrateSettings(si));
	EXPECT_EQ(si.GetIntValue(kSpeed, "iopThreadLag", 0), IopThread::kDefaultSettingLag);
}
