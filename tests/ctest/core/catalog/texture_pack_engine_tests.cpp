// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The seam to the emulator: whether the GPU can use ASTC packs, and turning texture replacement on
// after an install. There is no VM and no graphics device in this binary, so these check what is
// decided without one; the reload itself runs on the GS thread and is exercised by hand (see the
// service's report).

#include "Catalog/TexturePackEngine.h"

#include "catalog_test_support.h"

#include "Host.h"
#include "INISettingsInterface.h"
#include "LayeredSettingsInterface.h"

#include "common/MemorySettingsInterface.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace Catalog::TexturePackEngine;

namespace
{
	constexpr const char* SECTION = "EmuCore/GS";
	constexpr const char* KEY = "LoadTextureReplacements";
} // namespace

class TexturePackEngineTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// Another test in this binary may already have given the host a settings layer; use
		// whichever is there. Only the layer this fixture installed is taken away again.
		auto lock = Host::GetSettingsLock();
		if (!Host::Internal::GetBaseSettingsLayer())
		{
			Host::Internal::SetBaseSettingsLayer(&m_settings);
			m_installed = true;
		}
	}

	void TearDown() override
	{
		Host::RemoveBaseSettingValue(SECTION, KEY);
		if (m_installed)
		{
			auto lock = Host::GetSettingsLock();
			static_cast<LayeredSettingsInterface*>(Host::GetSettingsInterface())->SetLayer(LayeredSettingsInterface::LAYER_BASE, nullptr);
		}
	}

	MemorySettingsInterface m_settings;
	bool m_installed = false;
};

TEST_F(TexturePackEngineTest, AnInstallTurnsTextureReplacementOn)
{
	Host::RemoveBaseSettingValue(SECTION, KEY);
	ASSERT_FALSE(Host::GetBaseBoolSettingValue(SECTION, KEY, false));

	ActivateAfterInstall();
	EXPECT_TRUE(Host::GetBaseBoolSettingValue(SECTION, KEY, false));
}

TEST_F(TexturePackEngineTest, AnExplicitOffIsTurnedOnToo)
{
	Host::SetBaseBoolSettingValue(SECTION, KEY, false);
	ActivateAfterInstall();
	EXPECT_TRUE(Host::GetBaseBoolSettingValue(SECTION, KEY, false));
}

TEST_F(TexturePackEngineTest, ItStaysOnWhenItAlreadyWas)
{
	Host::SetBaseBoolSettingValue(SECTION, KEY, true);
	ActivateAfterInstall();
	ActivateAfterInstall();
	EXPECT_TRUE(Host::GetBaseBoolSettingValue(SECTION, KEY, false));
}

TEST(TexturePackEngineGame, AGameThatTurnsReplacementOffGetsItTurnedOn)
{
	MemorySettingsInterface game;
	game.SetBoolValue(SECTION, KEY, false);
	game.SetIntValue("EmuCore/GS", "upscale_multiplier", 3);

	EXPECT_TRUE(EnableInGameLayer(game));
	EXPECT_TRUE(game.GetBoolValue(SECTION, KEY, false));
	EXPECT_EQ(game.GetIntValue("EmuCore/GS", "upscale_multiplier", 0), 3); // nothing else is touched
}

TEST(TexturePackEngineGame, AGameThatSaysNothingFollowsTheGlobalSetting)
{
	MemorySettingsInterface game;
	EXPECT_FALSE(EnableInGameLayer(game));
	EXPECT_FALSE(game.ContainsValue(SECTION, KEY));
}

TEST(TexturePackEngineGame, AGameThatTurnsReplacementOnIsLeftAlone)
{
	MemorySettingsInterface game;
	game.SetBoolValue(SECTION, KEY, true);
	EXPECT_FALSE(EnableInGameLayer(game));
	EXPECT_TRUE(game.GetBoolValue(SECTION, KEY, false));
}

TEST(TexturePackEngineGame, TheSettingsFileOfAGameIsEditedInPlace)
{
	CatalogTest::ScopedTempDir dir;
	const std::string path = dir.File("SLUS-21410_ABCDEF01.ini");
	{
		INISettingsInterface ini(path);
		ini.SetBoolValue(SECTION, KEY, false);
		ini.SetBoolValue("EmuCore/GS", "FXAA", true);
		ASSERT_TRUE(ini.Save());
	}

	EXPECT_TRUE(EnableForGame(path)); // no VM here, so nothing is reloaded

	INISettingsInterface after(path);
	ASSERT_TRUE(after.Load());
	EXPECT_TRUE(after.GetBoolValue(SECTION, KEY, false));
	EXPECT_TRUE(after.GetBoolValue("EmuCore/GS", "FXAA", false));

	EXPECT_FALSE(EnableForGame(path)); // already on
}

TEST(TexturePackEngineGame, AGameWithoutASettingsFileIsNotGivenOne)
{
	CatalogTest::ScopedTempDir dir;
	const std::string path = dir.File("none.ini");
	EXPECT_FALSE(EnableForGame(path));
	EXPECT_FALSE(EnableForGame(std::string()));
	EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(TexturePackEngineTest, ReloadingWithoutAGameDoesNothing)
{
	ReloadRunningGame(); // no VM: must neither crash nor queue anything
	SUCCEED();
}

TEST(TexturePackEngineAstc, IsUnknownWithoutAGraphicsDevice)
{
	EXPECT_EQ(GetAstcSupportOnGSThread(), AstcSupport::Unknown);
}

TEST(TexturePackEngineAstc, TheAsynchronousQueryAnswersAtOnceWhenNoGameIsRunning)
{
	int calls = 0;
	AstcSupport answer = AstcSupport::Supported;
	QueryAstcSupport([&](AstcSupport s) {
		calls++;
		answer = s;
	});
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(answer, AstcSupport::Unknown);

	QueryAstcSupport({}); // an empty callback is ignored rather than called
}
