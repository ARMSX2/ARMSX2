// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The seam to the emulator: whether the GPU can use ASTC packs, and turning texture replacement on
// after an install. There is no VM and no graphics device in this binary, so these check what is
// decided without one; the reload itself runs on the GS thread and is exercised by hand (see the
// service's report).

#include "Catalog/TexturePackEngine.h"

#include "Host.h"
#include "LayeredSettingsInterface.h"

#include "common/MemorySettingsInterface.h"

#include <gtest/gtest.h>

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
