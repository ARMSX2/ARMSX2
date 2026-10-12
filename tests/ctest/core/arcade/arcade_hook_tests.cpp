// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The seam between the input layer and the cabinet: a controller binding that fires while an arcade game
// runs must reach the JVS board and not the emulated DualShock, and a console game must be untouched.
// These run the real InputManager binding path (a pad with its buttons bound to pointer buttons, since
// that source needs no device) into the real pad.

#include <gtest/gtest.h>

#include <string_view>

#include "Arcade/ArcadeInput.h"
#include "Config.h"
#include "DEV9/ACJV.h"
#include "Input/InputManager.h"
#include "LayeredSettingsInterface.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Pad/PadDualshock2.h"

#include "common/ARCADE.h"
#include "common/MemorySettingsInterface.h"

namespace
{
	using GIB = GenericInputBinding;

	// Pointer-0/Button0 is the pad's Cross, Button1 its Select, Button2 its Start, Button3 the right stick pushed
	// right. Buttons 0 to 2 are the mouse's left, right and middle, which work a light gun while one runs; 3 is
	// not the gun's, so it stands in for a controller input there.
	constexpr u32 CROSS_BUTTON = 0;
	constexpr u32 SELECT_BUTTON = 1;
	constexpr u32 START_BUTTON = 2;
	constexpr u32 RSTICK_RIGHT_BUTTON = 3;

	class ArcadeHookTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			ArcadeInput::SetBoardRunner([](std::function<void()> function) { function(); });

			EmuConfig.Pad.Ports[0].Type = Pad::ControllerType::DualShock2;
			// The pad's bindings sit in their own layer, not the base one: the base layer is also read for the
			// bindings that drive the menus, which needs the input sources a test does not start.
			m_pad.SetStringList("Pad1", "Cross", {"Pointer-0/Button0"});
			m_pad.SetStringList("Pad1", "Select", {"Pointer-0/Button1"});
			m_pad.SetStringList("Pad1", "Start", {"Pointer-0/Button2"});
			m_pad.SetStringList("Pad1", "RRight", {"Pointer-0/Button3"});
			m_settings.SetLayer(LayeredSettingsInterface::LAYER_CMDLINE, &m_pad);
			m_settings.SetLayer(LayeredSettingsInterface::LAYER_BASE, &m_base);
			Pad::LoadConfig(m_settings);
			ASSERT_NE(Pad::GetPad(0), nullptr);
			Reload();
		}

		void TearDown() override
		{
			Arcade::s_session = false;
			ArcadeInput::SetActiveRemap({});
			ArcadeInput::SetBoardRunner({});
			MemorySettingsInterface empty;
			LayeredSettingsInterface none;
			none.SetLayer(LayeredSettingsInterface::LAYER_BASE, &empty);
			InputManager::ReloadBindings(none, none, none, false, false);
			m_settings.SetLayer(LayeredSettingsInterface::LAYER_GAME, nullptr);
			Pad::Shutdown();
		}

		void Reload() { InputManager::ReloadBindings(m_settings, m_settings, m_settings, false, false); }

		static void StartArcadeGame(const char* gameid)
		{
			Arcade::s_session = true;
			ACJV::SetGameId(gameid);
			ACJV::SetMode(ACJV::ResolveModeFromGameId(gameid));
		}

		static void Click(u32 button, bool down)
		{
			InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), down ? 1.0f : 0.0f);
		}

		static bool DualShockHolds(PadDualshock2::Inputs input)
		{
			return Pad::GetPad(0)->GetEffectiveInput(input) > 0.5f;
		}

		MemorySettingsInterface m_pad;
		MemorySettingsInterface m_base;
		LayeredSettingsInterface m_settings;
	};
} // namespace

TEST_F(ArcadeHookTest, ABindingReachesTheDualShockOfAConsoleGame)
{
	Click(CROSS_BUTTON, true);
	EXPECT_TRUE(DualShockHolds(PadDualshock2::PAD_CROSS));
	EXPECT_EQ(ACJV::GetButtonState(0), 0);
	Click(CROSS_BUTTON, false);
	EXPECT_FALSE(DualShockHolds(PadDualshock2::PAD_CROSS));
}

TEST_F(ArcadeHookTest, ABindingReachesTheCabinetAndNotTheDualShockOfAnArcadeGame)
{
	StartArcadeGame("NM00099");
	Click(CROSS_BUTTON, true);
	EXPECT_FALSE(DualShockHolds(PadDualshock2::PAD_CROSS));
	EXPECT_EQ(ACJV::GetButtonState(0), JVS_BTN_4);
	Click(CROSS_BUTTON, false);
	EXPECT_EQ(ACJV::GetButtonState(0), 0);
	EXPECT_FALSE(DualShockHolds(PadDualshock2::PAD_CROSS));

	Click(SELECT_BUTTON, true);
	EXPECT_EQ(ACJV::coin[0], 1);
	EXPECT_FALSE(DualShockHolds(PadDualshock2::PAD_SELECT));
	Click(SELECT_BUTTON, false);
}

TEST_F(ArcadeHookTest, TheDualShockIsBackWhenTheArcadeGameIsGone)
{
	StartArcadeGame("NM00099");
	Arcade::s_session = false;
	Click(CROSS_BUTTON, true);
	EXPECT_TRUE(DualShockHolds(PadDualshock2::PAD_CROSS));
	Click(CROSS_BUTTON, false);
}

TEST_F(ArcadeHookTest, ASettingsReloadBringsThePlayersLayoutInFromBothLayers)
{
	StartArcadeGame("NM00099");
	ArcadeInput::SaveRemap(m_base, {{GIB::Cross, GIB::Square}});
	Reload();
	Click(CROSS_BUTTON, true);
	EXPECT_EQ(ACJV::GetButtonState(0), JVS_BTN_1);
	Click(CROSS_BUTTON, false);

	// The game's own file says the cross is the cross again.
	MemorySettingsInterface game;
	ArcadeInput::SaveRemap(game, {{GIB::Cross, GIB::Cross}});
	m_settings.SetLayer(LayeredSettingsInterface::LAYER_GAME, &game);
	Reload();
	Click(CROSS_BUTTON, true);
	EXPECT_EQ(ACJV::GetButtonState(0), JVS_BTN_4);
	Click(CROSS_BUTTON, false);
	m_settings.SetLayer(LayeredSettingsInterface::LAYER_GAME, nullptr);
}

TEST_F(ArcadeHookTest, TheMouseMovingGivesTheGunBackToTheMouse)
{
	StartArcadeGame("NM00003");
	float x, y;
	Click(RSTICK_RIGHT_BUTTON, true);
	ASSERT_TRUE(ACJV::GetGunStickAim(0, &x, &y));

	// No movement, no change.
	InputManager::UpdatePointerAbsolutePosition(0, 0.0f, 0.0f);
	EXPECT_TRUE(ACJV::GetGunStickAim(0, &x, &y));
	InputManager::UpdatePointerAbsolutePosition(0, 40.0f, 25.0f);
	EXPECT_FALSE(ACJV::GetGunStickAim(0, &x, &y));

	Click(RSTICK_RIGHT_BUTTON, true);
	ASSERT_TRUE(ACJV::GetGunStickAim(0, &x, &y));
	InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::X, 3.0f);
	EXPECT_FALSE(ACJV::GetGunStickAim(0, &x, &y));
	Click(RSTICK_RIGHT_BUTTON, false);
}

// ----------------------------------------------------------------------------------------------------
// The mouse as the gun
// ----------------------------------------------------------------------------------------------------

// The fixture binds pointer buttons 0, 1 and 2 (left, right, middle) to the pad's Cross, Select and Start: a
// click that the gun takes must not also reach them.

TEST_F(ArcadeHookTest, AClickFiresTheLightGunAndDoesNotAlsoReachTheBindings)
{
	StartArcadeGame("NM00003");
	const GunMapping& gm = ACJV::GetGunMapping();

	Click(CROSS_BUTTON, true);
	EXPECT_EQ(ACJV::GetButtonState(0), gm.p1_trigger);
	Click(CROSS_BUTTON, false);
	EXPECT_EQ(ACJV::GetButtonState(0), 0);

	// The right button reloads. It is bound to Select, the coin, which it must not insert.
	Click(SELECT_BUTTON, true);
	EXPECT_EQ(ACJV::GetButtonState(0), gm.p1_trigger);
	EXPECT_TRUE(ACJV::GetGunForceOffscreen());
	EXPECT_EQ(ACJV::coin[0], 0);
	Click(SELECT_BUTTON, false);
	EXPECT_EQ(ACJV::GetButtonState(0), 0);
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());
	EXPECT_EQ(ACJV::coin[0], 0);

	// The middle button is the pedal, which on this cabinet reloads. It is bound to Start, which it must not press.
	Click(START_BUTTON, true);
	EXPECT_TRUE(ACJV::GetGunForceOffscreen());
	EXPECT_EQ(ACJV::GetButtonState(0), 0);
	Click(START_BUTTON, false);
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());

	// A button that is not the gun's still reaches its binding.
	float x, y;
	Click(RSTICK_RIGHT_BUTTON, true);
	EXPECT_TRUE(ACJV::GetGunStickAim(0, &x, &y));
	Click(RSTICK_RIGHT_BUTTON, false);
}

TEST_F(ArcadeHookTest, AClickOnATouchPanelTouchesIt)
{
	StartArcadeGame("NM00014");
	Click(CROSS_BUTTON, true);
	EXPECT_TRUE(ACJV::GetTouchPressed());
	EXPECT_EQ(ACJV::GetButtonState(0), 0); // Not the Cross binding's button 4 as well.
	Click(CROSS_BUTTON, false);
	EXPECT_FALSE(ACJV::GetTouchPressed());
}

TEST_F(ArcadeHookTest, AClickKeepsItsBindingsOnOtherCabinetsAndInConsoleGames)
{
	StartArcadeGame("NM00099");
	Click(SELECT_BUTTON, true);
	EXPECT_EQ(ACJV::coin[0], 1); // The binding, as before.
	Click(SELECT_BUTTON, false);
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());

	Arcade::s_session = false;
	Click(CROSS_BUTTON, true);
	EXPECT_TRUE(DualShockHolds(PadDualshock2::PAD_CROSS));
	Click(CROSS_BUTTON, false);
}

TEST_F(ArcadeHookTest, AReleaseStillLetsGoOfTheGunWhenTheBindingsAreGone)
{
	StartArcadeGame("NM00003");
	Click(CROSS_BUTTON, true);
	EXPECT_NE(ACJV::GetButtonState(0), 0);

	// The player's bindings reloaded between the press and the release.
	MemorySettingsInterface empty;
	LayeredSettingsInterface none;
	none.SetLayer(LayeredSettingsInterface::LAYER_BASE, &empty);
	InputManager::ReloadBindings(none, none, none, false, false);
	Click(CROSS_BUTTON, false);
	EXPECT_EQ(ACJV::GetButtonState(0), 0);
}

// ----------------------------------------------------------------------------------------------------
// Hotkeys
// ----------------------------------------------------------------------------------------------------

namespace
{
	void FireHotkey(const char* name, s32 pressed)
	{
		for (const HotkeyInfo* hotkey : InputManager::GetHotkeyList())
		{
			if (std::string_view(hotkey->name) == name)
			{
				hotkey->handler(pressed);
				return;
			}
		}
		ADD_FAILURE() << "no hotkey " << name;
	}
} // namespace

TEST_F(ArcadeHookTest, TheArcadeHotkeysAreInTheSystemCategory)
{
	for (const char* name : {"ArcadeCoinP1", "ArcadeCoinP2", "ArcadeService", "ArcadeTest"})
	{
		bool found = false;
		for (const HotkeyInfo* hotkey : InputManager::GetHotkeyList())
		{
			if (std::string_view(hotkey->name) == name)
			{
				found = true;
				EXPECT_STREQ(hotkey->category, "System");
				EXPECT_NE(std::string_view(hotkey->display_name).find("Arcade"), std::string_view::npos);
			}
		}
		EXPECT_TRUE(found) << name;
	}
}

TEST_F(ArcadeHookTest, CoinHotkeysInsertACoinOnThePress)
{
	StartArcadeGame("NM00099");
	FireHotkey("ArcadeCoinP1", 1);
	EXPECT_EQ(ACJV::coin[0], 1);
	FireHotkey("ArcadeCoinP1", 0); // The release is not a second coin.
	EXPECT_EQ(ACJV::coin[0], 1);
	FireHotkey("ArcadeCoinP2", 1);
	FireHotkey("ArcadeCoinP2", 0);
	EXPECT_EQ(ACJV::coin[1], 1);
	FireHotkey("ArcadeCoinP2", -1); // Cancelled by a chord.
	EXPECT_EQ(ACJV::coin[1], 1);
}

TEST_F(ArcadeHookTest, ServiceHotkeyIsHeldWhilePressed)
{
	StartArcadeGame("NM00099");
	FireHotkey("ArcadeService", 1);
	EXPECT_EQ(ACJV::GetButtonState(0), JVS_BTN_SERVICE);
	FireHotkey("ArcadeService", 0);
	EXPECT_EQ(ACJV::GetButtonState(0), 0);
	FireHotkey("ArcadeService", 1);
	FireHotkey("ArcadeService", -1);
	EXPECT_EQ(ACJV::GetButtonState(0), 0);
}

TEST_F(ArcadeHookTest, TestHotkeyFlipsTheTestSwitch)
{
	StartArcadeGame("NM00099");
	EXPECT_FALSE(ArcadeInput::IsTestOn());
	FireHotkey("ArcadeTest", 1);
	FireHotkey("ArcadeTest", 0);
	EXPECT_TRUE(ArcadeInput::IsTestOn());
	FireHotkey("ArcadeTest", 1);
	EXPECT_FALSE(ArcadeInput::IsTestOn());
}

TEST_F(ArcadeHookTest, TheArcadeHotkeysDoNothingWithoutAnArcadeGame)
{
	FireHotkey("ArcadeCoinP1", 1);
	FireHotkey("ArcadeCoinP2", 1);
	FireHotkey("ArcadeService", 1);
	FireHotkey("ArcadeTest", 1);
	EXPECT_FALSE(ACJV::GetDIPSwitchState(0));
	EXPECT_EQ(ACJV::GetButtonState(0), 0);
}
