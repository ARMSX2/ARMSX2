// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The vu-aot setting (EmuCore/CPU/Recompiler EnableVUAot) is on by default, and turns the VU program cache
// on with it; the program cache's own setting (EnableVUProgramCache) stays off by default. The setting is
// in the shared configuration, so a build without the arm64 recompilers reads and writes it too, and
// nothing there looks at it.

#include <gtest/gtest.h>

#include "Config.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

namespace
{
	Pcsx2Config::RecompilerOptions Load(MemorySettingsInterface& si)
	{
		Pcsx2Config::RecompilerOptions rc;
		SettingsLoadWrapper wrap(si);
		rc.LoadSave(wrap);
		return rc;
	}
} // namespace

TEST(VuAotSetting, DefaultsOnAndTheProgramCacheSettingStaysOff)
{
	const Pcsx2Config::RecompilerOptions rc;
	EXPECT_TRUE(rc.EnableVUAot);
	EXPECT_FALSE(rc.EnableVUProgramCache);
	EXPECT_TRUE(rc.UseVUProgramCache()) << "vu-aot is bound to the cache";
}

TEST(VuAotSetting, ASettingsFileWithoutTheKeyGetsTheDefault)
{
	MemorySettingsInterface si;
	EXPECT_TRUE(Load(si).EnableVUAot);
	EXPECT_FALSE(Load(si).EnableVUProgramCache);
}

TEST(VuAotSetting, ReadsAnExplicitValueEitherWay)
{
	for (const bool on : {false, true})
	{
		MemorySettingsInterface si;
		si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", on);
		const Pcsx2Config::RecompilerOptions rc = Load(si);
		EXPECT_EQ(rc.EnableVUAot, on);
		EXPECT_EQ(rc.UseVUProgramCache(), on) << "off with the program cache setting off: no cache";
	}
}

TEST(VuAotSetting, OffWithTheProgramCacheSettingOnKeepsTheCache)
{
	MemorySettingsInterface si;
	si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", false);
	si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVUProgramCache", true);
	const Pcsx2Config::RecompilerOptions rc = Load(si);
	EXPECT_FALSE(rc.EnableVUAot);
	EXPECT_TRUE(rc.UseVUProgramCache());
}

TEST(VuAotSetting, IsWrittenUnderItsKey)
{
	for (const bool on : {false, true})
	{
		Pcsx2Config::RecompilerOptions rc;
		rc.EnableVUAot = on;
		MemorySettingsInterface si;
		SettingsSaveWrapper wrap(si);
		rc.LoadSave(wrap);

		bool saved = !on;
		ASSERT_TRUE(si.GetBoolValue("EmuCore/CPU/Recompiler", "EnableVUAot", &saved));
		EXPECT_EQ(saved, on);
	}
}

TEST(VuAotSetting, ChangingItIsAChangeOfTheCpuOptions)
{
	// (VMManager::CheckForCPUConfigChanges resets the VU recompilers when the options differ)
	Pcsx2Config::RecompilerOptions a, b;
	EXPECT_TRUE(a == b);
	b.EnableVUAot = !a.EnableVUAot;
	EXPECT_TRUE(a != b);
}
