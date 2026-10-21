// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Where auto-tune keeps what it is trying, and the setting that switches it off.

#include <gtest/gtest.h>

#include "Config.h"
#include "Host.h"
#include "LayeredSettingsInterface.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

namespace
{
	constexpr const char* GS = "EmuCore/GS";
	constexpr const char* UPSCALE = "upscale_multiplier";
} // namespace

// ---------------------------------------------------------------------------------------------- trial layer

TEST(AutoTuneTrialLayer, SitsAboveTheGameLayerAndBelowTheCommandLine)
{
	MemorySettingsInterface base, game, trial, cmdline;
	base.SetFloatValue(GS, UPSCALE, 4.0f);
	game.SetFloatValue(GS, UPSCALE, 3.0f);

	LayeredSettingsInterface layered;
	layered.SetLayer(LayeredSettingsInterface::LAYER_BASE, &base);
	layered.SetLayer(LayeredSettingsInterface::LAYER_GAME, &game);
	EXPECT_EQ(layered.GetFloatValue(GS, UPSCALE, 0.0f), 3.0f);

	layered.SetLayer(LayeredSettingsInterface::LAYER_AUTOTUNE, &trial);
	EXPECT_EQ(layered.GetFloatValue(GS, UPSCALE, 0.0f), 3.0f) << "an empty trial layer answers nothing";

	trial.SetFloatValue(GS, UPSCALE, 2.0f);
	EXPECT_EQ(layered.GetFloatValue(GS, UPSCALE, 0.0f), 2.0f) << "a trial beats the game's own file";

	cmdline.SetFloatValue(GS, UPSCALE, 1.0f);
	layered.SetLayer(LayeredSettingsInterface::LAYER_CMDLINE, &cmdline);
	EXPECT_EQ(layered.GetFloatValue(GS, UPSCALE, 0.0f), 1.0f) << "an explicit override beats a trial";
}

TEST(AutoTuneTrialLayer, ClearingAKeyShowsTheGameValueAgain)
{
	MemorySettingsInterface base, game, trial;
	game.SetBoolValue("EmuCore/Speedhacks", "iopThread", false);

	LayeredSettingsInterface layered;
	layered.SetLayer(LayeredSettingsInterface::LAYER_BASE, &base);
	layered.SetLayer(LayeredSettingsInterface::LAYER_GAME, &game);
	layered.SetLayer(LayeredSettingsInterface::LAYER_AUTOTUNE, &trial);

	trial.SetBoolValue("EmuCore/Speedhacks", "iopThread", true);
	EXPECT_TRUE(layered.GetBoolValue("EmuCore/Speedhacks", "iopThread", false));
	EXPECT_TRUE(layered.ContainsValue("EmuCore/Speedhacks", "iopThread"));

	trial.DeleteValue("EmuCore/Speedhacks", "iopThread");
	EXPECT_FALSE(layered.GetBoolValue("EmuCore/Speedhacks", "iopThread", true));
}

TEST(AutoTuneTrialLayer, SurvivesTheGameLayerBeingRebuilt)
{
	// ApplySettings re-reads every layer. The game layer is replaced by a new object when its file is
	// reloaded (VMManager::UpdateGameSettingsLayer); the trial layer is auto-tune's and is not touched.
	MemorySettingsInterface base, trial;
	auto game = std::make_unique<MemorySettingsInterface>();
	game->SetFloatValue(GS, UPSCALE, 3.0f);

	LayeredSettingsInterface layered;
	layered.SetLayer(LayeredSettingsInterface::LAYER_BASE, &base);
	layered.SetLayer(LayeredSettingsInterface::LAYER_GAME, game.get());
	layered.SetLayer(LayeredSettingsInterface::LAYER_AUTOTUNE, &trial);
	trial.SetFloatValue(GS, UPSCALE, 2.0f);

	// The game file is reloaded: new object, new contents.
	auto reloaded = std::make_unique<MemorySettingsInterface>();
	reloaded->SetFloatValue(GS, UPSCALE, 5.0f);
	layered.SetLayer(LayeredSettingsInterface::LAYER_GAME, reloaded.get());
	game.reset();
	EXPECT_EQ(layered.GetFloatValue(GS, UPSCALE, 0.0f), 2.0f);

	// The game has no file at all any more.
	layered.SetLayer(LayeredSettingsInterface::LAYER_GAME, nullptr);
	EXPECT_EQ(layered.GetFloatValue(GS, UPSCALE, 0.0f), 2.0f);
}

TEST(AutoTuneTrialLayer, TheKeyValueListLetsATrialShadowTheGameFile)
{
	MemorySettingsInterface base, game, trial;
	game.SetFloatValue(GS, UPSCALE, 3.0f);
	trial.SetFloatValue(GS, UPSCALE, 2.0f);

	LayeredSettingsInterface layered;
	layered.SetLayer(LayeredSettingsInterface::LAYER_BASE, &base);
	layered.SetLayer(LayeredSettingsInterface::LAYER_GAME, &game);
	layered.SetLayer(LayeredSettingsInterface::LAYER_AUTOTUNE, &trial);

	const auto list = layered.GetKeyValueList(GS);
	ASSERT_EQ(list.size(), 1u);
	EXPECT_EQ(list[0].first, UPSCALE);
	EXPECT_EQ(std::stof(list[0].second), 2.0f);
}

TEST(AutoTuneTrialLayer, TheHostInstallsAndRemovesIt)
{
	MemorySettingsInterface trial;
	trial.SetStringValue("EmuCore", "AutoTuneProbe", "trial");

	{
		auto lock = Host::GetSettingsLock();
		ASSERT_EQ(Host::Internal::GetAutoTuneSettingsLayer(), nullptr);
		Host::Internal::SetAutoTuneSettingsLayer(&trial, lock);
		EXPECT_EQ(Host::Internal::GetAutoTuneSettingsLayer(), &trial);
	}
	EXPECT_EQ(Host::GetStringSettingValue("EmuCore", "AutoTuneProbe", "none"), "trial");

	{
		auto lock = Host::GetSettingsLock();
		Host::Internal::SetAutoTuneSettingsLayer(nullptr, lock);
	}
	EXPECT_EQ(Host::GetStringSettingValue("EmuCore", "AutoTuneProbe", "none"), "none");
}

// ---------------------------------------------------------------------------------------------- the switch

TEST(AutoTuneSetting, IsOffByDefaultAndReadFromEmuCore)
{
	Pcsx2Config config;
	EXPECT_FALSE(config.AutoTune);
	EXPECT_TRUE(config.AutoTuneTrace.empty());

	MemorySettingsInterface si;
	si.SetBoolValue("EmuCore", "AutoTune", true);
	si.SetStringValue("EmuCore", "AutoTuneTrace", "/tmp/trace.csv");
	SettingsLoadWrapper wrapper(si);
	config.LoadSaveCore(wrapper);
	EXPECT_TRUE(config.AutoTune);
	EXPECT_EQ(config.AutoTuneTrace, "/tmp/trace.csv");
}

TEST(AutoTuneSetting, IsNotPartOfAnythingTheCpuCachesDependOn)
{
	// CheckForCPUConfigChanges compares Cpu, Gamefixes, Speedhacks and Profiler and flushes the
	// recompiler caches when one differs. Toggling auto-tune must change none of them.
	Pcsx2Config on, off;
	on.AutoTune = true;
	EXPECT_TRUE(on.Cpu == off.Cpu);
	EXPECT_TRUE(on.Gamefixes == off.Gamefixes);
	EXPECT_TRUE(on.Speedhacks == off.Speedhacks);
	EXPECT_TRUE(on.Profiler == off.Profiler);
	EXPECT_TRUE(on.GS == off.GS);
}
