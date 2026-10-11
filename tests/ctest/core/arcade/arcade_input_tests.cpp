// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A controller on a Namco cabinet: what each generic binding does to the JVS board in each control mode,
// the player's own layout (remap) and its INI text, and the jobs a settings page lists.
//
// The board is the real one (ACJV); the tests read back what the mapping left on it. There is no CPU thread
// here, so coins and the test switch run where they are posted from.

#include <gtest/gtest.h>

#include "Arcade/ArcadeInput.h"
#include "DEV9/ACJV.h"

#include "common/ARCADE.h"
#include "common/MemorySettingsInterface.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace
{
	using GIB = GenericInputBinding;
	using ArcadeInput::Remap;

	constexpr float HELD = 1.0f;
	constexpr float LET_GO = 0.0f;

	// A game ID no table knows: the generic cabinet.
	constexpr const char* GENERIC_GAME = "NM00099";

	class ArcadeInputTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			ArcadeInput::SetBoardRunner([](std::function<void()> function) { function(); });
			Arcade::s_session = true;
			ArcadeInput::SetActiveRemap({});
			Boot(GENERIC_GAME);
		}

		void TearDown() override
		{
			ArcadeInput::SetActiveRemap({});
			ArcadeInput::SetBoardRunner({});
			Arcade::s_session = false;
		}

		// The game starting: what VMManager does to the board, and so to the mapping.
		static void Boot(const std::string& gameid, std::optional<JVS_MODE> forced_mode = std::nullopt)
		{
			ACJV::SetGameId(gameid);
			ACJV::SetMode(forced_mode ? *forced_mode : ACJV::ResolveModeFromGameId(gameid));
			for (u32 axis = 0; axis < 4; axis++)
				ACJV::SetWheelAxis(axis, 0.0f);
			ArcadeInput::Reset();
		}

		static bool Send(GIB binding, float value, u32 pad = 0)
		{
			return ArcadeInput::HandleBinding(pad, binding, value);
		}
		static void Press(GIB binding, u32 pad = 0) { EXPECT_TRUE(Send(binding, HELD, pad)); }
		static void Release(GIB binding, u32 pad = 0) { EXPECT_TRUE(Send(binding, LET_GO, pad)); }

		// The switches the board has down for a player.
		static u16 Switches(u32 player = 0) { return ACJV::GetButtonState(player); }

		// What pressing a button alone leaves on the board, and the board after letting go of it.
		static u16 PressedAlone(GIB binding, u32 pad = 0)
		{
			Press(binding, pad);
			const u16 down = Switches(pad);
			Release(binding, pad);
			EXPECT_EQ(Switches(pad), 0) << "still down after letting go";
			return down;
		}
	};

	Remap MakeRemap(std::initializer_list<std::pair<GIB, GIB>> pairs)
	{
		return Remap(pairs.begin(), pairs.end());
	}
} // namespace

// ----------------------------------------------------------------------------------------------------
// Taking the controller
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, NothingIsTakenWithoutAnArcadeGame)
{
	Arcade::s_session = false;
	EXPECT_FALSE(Send(GIB::Cross, HELD));
	EXPECT_EQ(Switches(), 0);
	EXPECT_FALSE(ArcadeInput::IsActive());
	EXPECT_EQ(ArcadeInput::GetGameId(), "");
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::DEFAULT);
}

TEST_F(ArcadeInputTest, TheTwoControllerPortsAreTheCabinetsAndASlotBeyondThemIsNot)
{
	EXPECT_TRUE(Send(GIB::Cross, HELD, 0));
	EXPECT_TRUE(Send(GIB::Cross, HELD, 1));
	// A multitap's slots are not the cabinet's: they stay with the pad.
	EXPECT_FALSE(Send(GIB::Cross, HELD, 2));
	EXPECT_FALSE(Send(GIB::Cross, HELD, 7));
	EXPECT_NE(Switches(0), 0);
	EXPECT_NE(Switches(1), 0);
}

TEST_F(ArcadeInputTest, AnEventWithNoGenericBindingIsTakenAndDoesNothing)
{
	EXPECT_TRUE(Send(GIB::Unknown, HELD));
	EXPECT_TRUE(Send(GIB::System, HELD));
	EXPECT_EQ(Switches(), 0);
}

TEST_F(ArcadeInputTest, TheSessionAndItsMode)
{
	EXPECT_TRUE(ArcadeInput::IsActive());
	EXPECT_EQ(ArcadeInput::GetGameId(), GENERIC_GAME);
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::DEFAULT);
	Boot("NM00004");
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::FIGHTING);
	// An .acgame's jvsmode wins over the game ID's.
	Boot("NM00004", JVS_MODE::DRUM);
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::DRUM);
}

// ----------------------------------------------------------------------------------------------------
// Control modes
// ----------------------------------------------------------------------------------------------------

TEST(ArcadeInputModes, EveryModeIsListedInOrderWithItsJvsmodeName)
{
	const auto modes = ArcadeInput::GetModes();
	ASSERT_EQ(modes.size(), 8u);
	for (size_t i = 0; i < modes.size(); i++)
	{
		EXPECT_EQ(static_cast<size_t>(modes[i].mode), i);
		EXPECT_NE(std::string(modes[i].display_name), "");
		EXPECT_NE(std::string(modes[i].hint), "");
		for (size_t j = 0; j < i; j++)
			EXPECT_NE(std::string(modes[i].name), modes[j].name);
	}
	EXPECT_EQ(std::string(ArcadeInput::GetModeInfo(JVS_MODE::DRIVE).name), "racing");
	EXPECT_EQ(std::string(ArcadeInput::GetModeInfo(JVS_MODE::LIGHTGUN).name), "lightgun");
	EXPECT_EQ(std::string(ArcadeInput::GetModeInfo(JVS_MODE::TWINSTICK).name), "twinstick");
	EXPECT_EQ(ArcadeInput::GetModeInfo(static_cast<JVS_MODE>(99)).mode, JVS_MODE::DEFAULT);
}

TEST(ArcadeInputModes, AGameIDGetsItsModeFromTheBoardTables)
{
	EXPECT_EQ(ArcadeInput::GetModeForGameId("NM00004"), JVS_MODE::FIGHTING);
	EXPECT_EQ(ArcadeInput::GetModeForGameId("NM00003"), JVS_MODE::LIGHTGUN);
	EXPECT_EQ(ArcadeInput::GetModeForGameId("NM00047"), JVS_MODE::DRIVE);
	EXPECT_EQ(ArcadeInput::GetModeForGameId("NM00023"), JVS_MODE::DRUM);
	EXPECT_EQ(ArcadeInput::GetModeForGameId("NM00014"), JVS_MODE::TOUCH);
	EXPECT_EQ(ArcadeInput::GetModeForGameId("NM00006"), JVS_MODE::STANDARD);
	EXPECT_EQ(ArcadeInput::GetModeForGameId("NM00016"), JVS_MODE::TWINSTICK);
	EXPECT_EQ(ArcadeInput::GetModeForGameId("NM00099"), JVS_MODE::DEFAULT);
}

// ----------------------------------------------------------------------------------------------------
// Default cabinet
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, DefaultTheDpadIsTheLeverAndTheFaceButtonsAreTheSixButtons)
{
	EXPECT_EQ(PressedAlone(GIB::DPadUp), JVS_BTN_UP);
	EXPECT_EQ(PressedAlone(GIB::DPadDown), JVS_BTN_DOWN);
	EXPECT_EQ(PressedAlone(GIB::DPadLeft), JVS_BTN_LEFT);
	EXPECT_EQ(PressedAlone(GIB::DPadRight), JVS_BTN_RIGHT);

	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_1);
	EXPECT_EQ(PressedAlone(GIB::Triangle), JVS_BTN_2);
	EXPECT_EQ(PressedAlone(GIB::L1), JVS_BTN_3);
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_4);
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_5);
	EXPECT_EQ(PressedAlone(GIB::R1), JVS_BTN_6);
	EXPECT_EQ(PressedAlone(GIB::Start), JVS_BTN_START);

	// What has no place on the cabinet does nothing.
	EXPECT_EQ(PressedAlone(GIB::L2), 0);
	EXPECT_EQ(PressedAlone(GIB::R2), 0);
	EXPECT_EQ(PressedAlone(GIB::L3), 0);
	EXPECT_EQ(PressedAlone(GIB::R3), 0);
}

TEST_F(ArcadeInputTest, DefaultSeveralButtonsHoldTogetherAndLetGoOneAtATime)
{
	Press(GIB::DPadUp);
	Press(GIB::Cross);
	Press(GIB::Square);
	EXPECT_EQ(Switches(), JVS_BTN_UP | JVS_BTN_4 | JVS_BTN_1);
	Release(GIB::Cross);
	EXPECT_EQ(Switches(), JVS_BTN_UP | JVS_BTN_1);
	Release(GIB::DPadUp);
	Release(GIB::Square);
	EXPECT_EQ(Switches(), 0);
}

TEST_F(ArcadeInputTest, DefaultTheLeftStickIsTheLeverFromHalfWay)
{
	Send(GIB::LeftStickLeft, 0.4f);
	EXPECT_EQ(Switches(), 0);
	Send(GIB::LeftStickLeft, 0.6f);
	EXPECT_EQ(Switches(), JVS_BTN_LEFT);
	Send(GIB::LeftStickLeft, 0.0f);
	Send(GIB::LeftStickUp, 1.0f);
	Send(GIB::LeftStickRight, 1.0f);
	EXPECT_EQ(Switches(), JVS_BTN_UP | JVS_BTN_RIGHT);
	Send(GIB::LeftStickUp, 0.0f);
	Send(GIB::LeftStickRight, 0.0f);
	EXPECT_EQ(Switches(), 0);
	// The right stick is not the lever.
	Send(GIB::RightStickUp, 1.0f);
	EXPECT_EQ(Switches(), 0);
}

TEST_F(ArcadeInputTest, DefaultPressureBelowHalfIsNotAPress)
{
	Send(GIB::Cross, 0.49f);
	EXPECT_EQ(Switches(), 0);
	Send(GIB::Cross, 0.5f);
	EXPECT_EQ(Switches(), JVS_BTN_4);
}

TEST_F(ArcadeInputTest, TheSecondControllerPlaysThePlayer2Buttons)
{
	Press(GIB::Cross, 0);
	Press(GIB::Circle, 1);
	EXPECT_EQ(Switches(0), JVS_BTN_4);
	EXPECT_EQ(Switches(1), JVS_BTN_5);
	Release(GIB::Cross, 0);
	EXPECT_EQ(Switches(0), 0);
	EXPECT_EQ(Switches(1), JVS_BTN_5);
}

TEST_F(ArcadeInputTest, ResetLetsGoOfEverythingHeld)
{
	Press(GIB::Cross);
	Press(GIB::DPadLeft, 1);
	ACJV::SetGameId(GENERIC_GAME); // The board's side of a new game.
	ArcadeInput::Reset();
	EXPECT_EQ(Switches(0), 0);
	EXPECT_EQ(Switches(1), 0);
	// A button let go of after the reset is not a press.
	Release(GIB::Cross);
	EXPECT_EQ(Switches(0), 0);
}

// ----------------------------------------------------------------------------------------------------
// Coin, Service, Test
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, SelectInsertsACoinOnThePressNotWhileHeld)
{
	EXPECT_EQ(ACJV::coin[0], 0);
	Press(GIB::Select);
	EXPECT_EQ(ACJV::coin[0], 1);
	Send(GIB::Select, 0.9f); // Still held, a little harder.
	EXPECT_EQ(ACJV::coin[0], 1);
	Release(GIB::Select);
	Press(GIB::Select);
	EXPECT_EQ(ACJV::coin[0], 2);
	Release(GIB::Select);
	// Select is the coin only: no switch goes down with it.
	EXPECT_EQ(Switches(), 0);
	EXPECT_EQ(ACJV::coin[1], 0);
}

TEST_F(ArcadeInputTest, EachPlayersSelectIsThatPlayersCoin)
{
	Press(GIB::Select, 1);
	EXPECT_EQ(ACJV::coin[0], 0);
	EXPECT_EQ(ACJV::coin[1], 1);
}

TEST_F(ArcadeInputTest, CoinFunctionInsertsForThePlayerAndOnlyWhileAGameRuns)
{
	ArcadeInput::InsertCoin(0);
	ArcadeInput::InsertCoin(1);
	ArcadeInput::InsertCoin(1);
	ArcadeInput::InsertCoin(2); // There is no third slot.
	EXPECT_EQ(ACJV::coin[0], 1);
	EXPECT_EQ(ACJV::coin[1], 2);
	Arcade::s_session = false;
	ArcadeInput::InsertCoin(0);
	EXPECT_EQ(ACJV::coin[0], 1);
}

TEST_F(ArcadeInputTest, CoinGoesToTheBoardRunnerNotStraightIn)
{
	// The runner stands for the CPU thread: until it runs the posted change, the board has not changed.
	std::vector<std::function<void()>> posted;
	ArcadeInput::SetBoardRunner([&posted](std::function<void()> function) { posted.push_back(std::move(function)); });
	ArcadeInput::InsertCoin(0);
	ArcadeInput::ToggleTest();
	Press(GIB::Select);
	EXPECT_EQ(ACJV::coin[0], 0);
	EXPECT_FALSE(ArcadeInput::IsTestOn());
	ASSERT_EQ(posted.size(), 3u);
	for (auto& function : posted)
		function();
	EXPECT_EQ(ACJV::coin[0], 2);
	EXPECT_TRUE(ArcadeInput::IsTestOn());
	posted.clear();
	ArcadeInput::SetBoardRunner([](std::function<void()> function) { function(); });
	ArcadeInput::ToggleTest();
}

TEST_F(ArcadeInputTest, ServiceIsAHeldSwitchOfPlayer1)
{
	ArcadeInput::SetService(true);
	EXPECT_EQ(Switches(0), JVS_BTN_SERVICE);
	EXPECT_EQ(Switches(1), 0);
	// Held with the pad's own buttons, and not let go by them.
	Press(GIB::Cross);
	EXPECT_EQ(Switches(0), JVS_BTN_SERVICE | JVS_BTN_4);
	Release(GIB::Cross);
	EXPECT_EQ(Switches(0), JVS_BTN_SERVICE);
	ArcadeInput::SetService(false);
	EXPECT_EQ(Switches(0), 0);
}

TEST_F(ArcadeInputTest, TestFlipsTheBoardsTestSwitch)
{
	EXPECT_FALSE(ArcadeInput::IsTestOn());
	ArcadeInput::ToggleTest();
	EXPECT_TRUE(ArcadeInput::IsTestOn());
	EXPECT_TRUE(ACJV::GetDIPSwitchState(0));
	ArcadeInput::ToggleTest();
	EXPECT_FALSE(ArcadeInput::IsTestOn());
	Arcade::s_session = false;
	ArcadeInput::ToggleTest();
	EXPECT_FALSE(ACJV::GetDIPSwitchState(0));
}

// ----------------------------------------------------------------------------------------------------
// Fighting and standard cabinets: the game's own table of buttons
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, FightingTekkenUsesItsFourButtonsAndNothingElse)
{
	Boot("NM00004");
	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_1);
	EXPECT_EQ(PressedAlone(GIB::Triangle), JVS_BTN_2);
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_4);
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_5);
	// Not on the cabinet, though the six-button default has them.
	EXPECT_EQ(PressedAlone(GIB::L1), 0);
	EXPECT_EQ(PressedAlone(GIB::R1), 0);
	EXPECT_EQ(PressedAlone(GIB::DPadLeft), JVS_BTN_LEFT);
	EXPECT_EQ(PressedAlone(GIB::Start), JVS_BTN_START);
}

TEST_F(ArcadeInputTest, FightingSoulCaliburHasItsOwnOrder)
{
	Boot("NM00007");
	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_1); // Horizontal
	EXPECT_EQ(PressedAlone(GIB::Triangle), JVS_BTN_2); // Vertical
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_3); // Kick
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_4); // Guard
}

TEST_F(ArcadeInputTest, FightingCapcomFightingJamHasSixButtons)
{
	Boot("NM00018");
	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_1);
	EXPECT_EQ(PressedAlone(GIB::Triangle), JVS_BTN_2);
	EXPECT_EQ(PressedAlone(GIB::L1), JVS_BTN_3);
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_4);
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_5);
	EXPECT_EQ(PressedAlone(GIB::R1), JVS_BTN_6);
}

TEST_F(ArcadeInputTest, FightingTheLeftStickIsTheLeverToo)
{
	Boot("NM00004");
	Send(GIB::LeftStickDown, 1.0f);
	EXPECT_EQ(Switches(), JVS_BTN_DOWN);
}

TEST_F(ArcadeInputTest, StandardSmashCourtHasTwoButtonsAndQuizAnswersSitOnTheLever)
{
	Boot("NM00006");
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::STANDARD);
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_1);
	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_2);
	EXPECT_EQ(PressedAlone(GIB::Triangle), 0);

	// Quiz Suku Suku Inufuku 2: answers 1 to 4 are wired on the Up/Down/Left/Right switches.
	Boot("NM00037");
	EXPECT_EQ(PressedAlone(GIB::Triangle), JVS_BTN_UP);
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_DOWN);
	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_LEFT);
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_RIGHT);
}

// ----------------------------------------------------------------------------------------------------
// Driving
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, DriveTheLeftStickSteersAndTheTriggersAreThePedals)
{
	Boot("NM00047");
	Send(GIB::LeftStickRight, 0.5f);
	EXPECT_FLOAT_EQ(ACJV::GetSteer(), 0.5f);
	Send(GIB::LeftStickRight, 0.0f);
	Send(GIB::LeftStickLeft, 1.0f);
	EXPECT_FLOAT_EQ(ACJV::GetSteer(), -1.0f);
	Send(GIB::LeftStickLeft, 0.0f);
	EXPECT_FLOAT_EQ(ACJV::GetSteer(), 0.0f);

	Send(GIB::R2, 0.25f);
	Send(GIB::L2, 0.75f);
	EXPECT_FLOAT_EQ(ACJV::GetGas(), 0.25f);
	EXPECT_FLOAT_EQ(ACJV::GetBrake(), 0.75f);
	// The pedals are analog: they work no switch.
	EXPECT_EQ(Switches(), 0);
	Send(GIB::R2, 0.0f);
	Send(GIB::L2, 0.0f);
	EXPECT_FLOAT_EQ(ACJV::GetGas(), 0.0f);
	EXPECT_FLOAT_EQ(ACJV::GetBrake(), 0.0f);
}

TEST_F(ArcadeInputTest, DriveTheStickIsNotTheLeverButTheDpadIs)
{
	Boot("NM00047");
	Send(GIB::LeftStickUp, 1.0f);
	EXPECT_EQ(Switches(), 0);
	Send(GIB::LeftStickUp, 0.0f);
	EXPECT_EQ(PressedAlone(GIB::DPadUp), JVS_BTN_UP);
	EXPECT_EQ(PressedAlone(GIB::DPadDown), JVS_BTN_DOWN);
}

TEST_F(ArcadeInputTest, DriveRacingButtonsAndTheFreeFaceButtonsThatWorkTheTestMenus)
{
	Boot("NM00047");
	EXPECT_EQ(PressedAlone(GIB::R1), JVS_BTN_3); // Shift up
	EXPECT_EQ(PressedAlone(GIB::L1), JVS_BTN_4); // Shift down
	EXPECT_EQ(PressedAlone(GIB::Triangle), JVS_BTN_2 | JVS_BTN_9); // View change
	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_1); // Free: the test menu's ENTER
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_1);
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_5);
	EXPECT_EQ(PressedAlone(GIB::Start), JVS_BTN_START);
}

TEST_F(ArcadeInputTest, DriveBattleGear3KeepsItsButtonsOnTheLeverSwitches)
{
	Boot("NM00010");
	EXPECT_EQ(PressedAlone(GIB::R1), JVS_BTN_RIGHT); // Shift up
	EXPECT_EQ(PressedAlone(GIB::L1), JVS_BTN_1); // Shift down
	EXPECT_EQ(PressedAlone(GIB::Triangle), JVS_BTN_DOWN); // View
	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_LEFT); // Sidebrake
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_UP); // Hazard
	// Cross would be button 1, which Shift down has: no free button to give it.
	EXPECT_EQ(PressedAlone(GIB::Cross), 0);
}

TEST_F(ArcadeInputTest, DriveOneCabinetOnePlayer)
{
	Boot("NM00047");
	Press(GIB::R1, 1);
	Send(GIB::R2, 1.0f, 1);
	EXPECT_EQ(Switches(1), 0);
	EXPECT_FLOAT_EQ(ACJV::GetGas(), 0.0f);
}

// ----------------------------------------------------------------------------------------------------
// Light guns
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, LightGunVampireNightTriggerStartAndPedalReload)
{
	Boot("NM00003");
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::LIGHTGUN);
	const GunMapping& gm = ACJV::GetGunMapping();
	ASSERT_NE(gm.p1_trigger, 0);

	EXPECT_EQ(PressedAlone(GIB::R2), gm.p1_trigger);
	EXPECT_EQ(PressedAlone(GIB::Cross), gm.p1_trigger);
	EXPECT_EQ(PressedAlone(GIB::Start), gm.p1_start);
	// The rest of the pad has no place on a gun: it owns only the trigger, the pedal and Start.
	EXPECT_EQ(PressedAlone(GIB::Square), 0);
	EXPECT_EQ(PressedAlone(GIB::DPadUp), 0);

	// Vampire Night has no pedal switch: the pedal button reports the camera lost, which reloads.
	ASSERT_EQ(gm.pedal, 0);
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());
	Press(GIB::L1);
	EXPECT_TRUE(ACJV::GetGunForceOffscreen());
	Release(GIB::L1);
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());
	Press(GIB::L2);
	EXPECT_TRUE(ACJV::GetGunForceOffscreen());
	Release(GIB::L2);
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());
}

TEST_F(ArcadeInputTest, LightGunTimeCrisis3HasAPedalSwitch)
{
	Boot("NM00012");
	const GunMapping& gm = ACJV::GetGunMapping();
	ASSERT_NE(gm.pedal, 0);
	EXPECT_EQ(PressedAlone(GIB::L1), gm.pedal);
	EXPECT_EQ(PressedAlone(GIB::L2), gm.pedal);
	EXPECT_EQ(PressedAlone(GIB::R2), gm.p1_trigger);
	EXPECT_EQ(PressedAlone(GIB::Start), JVS_BTN_START); // No start switch of its own.
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());
}

TEST_F(ArcadeInputTest, LightGunTheSensorBitIsTheBoardsAndSurvivesThePad)
{
	Boot("NM00021"); // Cobra: the sensor is the Right switch.
	const GunMapping& gm = ACJV::GetGunMapping();
	ASSERT_EQ(gm.sensor, JVS_BTN_RIGHT);
	ACJV::SetButtonState(0, gm.sensor, true); // What the aim code does when the gun is on screen.
	Press(GIB::R2);
	Release(GIB::R2);
	Press(GIB::DPadRight); // Not a switch of a gun's.
	Release(GIB::DPadRight);
	EXPECT_EQ(Switches(), gm.sensor);
	ACJV::SetButtonState(0, gm.sensor, false);
}

TEST_F(ArcadeInputTest, LightGunPlayer2HasItsOwnSwitches)
{
	Boot("NM00003");
	const GunMapping& gm = ACJV::GetGunMapping();
	EXPECT_EQ(PressedAlone(GIB::R2, 1), gm.p2_trigger);
	EXPECT_EQ(PressedAlone(GIB::Start, 1), gm.p2_start);
	// The pedal is player 1's.
	Press(GIB::L1, 1);
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());
	Release(GIB::L1, 1);
}

TEST_F(ArcadeInputTest, LightGunServiceStillReachesTheBoard)
{
	Boot("NM00003");
	ArcadeInput::SetService(true);
	EXPECT_EQ(Switches(0), JVS_BTN_SERVICE);
	ArcadeInput::SetService(false);
	EXPECT_EQ(Switches(0), 0);
}

TEST_F(ArcadeInputTest, LightGunTheRightStickAimsUntilThePointerMoves)
{
	Boot("NM00003");
	float x, y;
	// Player 1 starts on the pointer, player 2 on the stick with no aim yet.
	EXPECT_FALSE(ACJV::GetGunStickAim(0, &x, &y));
	EXPECT_FALSE(ACJV::GetGunStickAim(1, &x, &y));

	// Inside the dead zone nothing moves.
	Send(GIB::RightStickRight, 0.1f);
	EXPECT_FALSE(ACJV::GetGunStickAim(0, &x, &y));

	Send(GIB::RightStickRight, 1.0f);
	ASSERT_TRUE(ACJV::GetGunStickAim(0, &x, &y));
	EXPECT_FLOAT_EQ(x, 1.0f);
	EXPECT_FLOAT_EQ(y, 0.5f);
	Send(GIB::RightStickRight, 0.0f);
	Send(GIB::RightStickUp, 1.0f);
	ASSERT_TRUE(ACJV::GetGunStickAim(0, &x, &y));
	EXPECT_FLOAT_EQ(x, 0.5f);
	EXPECT_FLOAT_EQ(y, 0.0f);

	// The mouse moving gives the aim back to it, and the stick has to be moved again to take it.
	ArcadeInput::OnPointerMoved();
	EXPECT_FALSE(ACJV::GetGunStickAim(0, &x, &y));
	Send(GIB::RightStickUp, 1.0f);
	EXPECT_TRUE(ACJV::GetGunStickAim(0, &x, &y));

	// Player 2 has no pointer.
	Send(GIB::RightStickLeft, 1.0f, 1);
	ASSERT_TRUE(ACJV::GetGunStickAim(1, &x, &y));
	EXPECT_FLOAT_EQ(x, 0.0f);
	ArcadeInput::OnPointerMoved();
	EXPECT_TRUE(ACJV::GetGunStickAim(1, &x, &y));
}

TEST_F(ArcadeInputTest, LightGunGunLayerButtonsWorkTheSameSwitches)
{
	Boot("NM00003");
	const GunMapping& gm = ACJV::GetGunMapping();
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::Trigger, true);
	EXPECT_EQ(Switches(), gm.p1_trigger);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::Trigger, false);
	EXPECT_EQ(Switches(), 0);

	// A shot beside the screen is a shot, and reloads where the cabinet reloads by looking away.
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::ShootOffscreen, true);
	EXPECT_EQ(Switches(), gm.p1_trigger);
	EXPECT_TRUE(ACJV::GetGunForceOffscreen());
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::ShootOffscreen, false);
	EXPECT_FALSE(ACJV::GetGunForceOffscreen());

	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::A, true);
	EXPECT_TRUE(ACJV::GetGunForceOffscreen()); // No pedal switch on this cabinet: the same reload.
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::A, false);

	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::Start, true);
	EXPECT_EQ(Switches(), gm.p1_start);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::Start, false);

	ArcadeInput::SetGunButton(1, ArcadeInput::GunButton::Coin, true);
	EXPECT_EQ(ACJV::coin[1], 1);

	// A cabinet with a pedal switch gets it from A and B.
	Boot("NM00012");
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::B, true);
	EXPECT_EQ(Switches(), ACJV::GetGunMapping().pedal);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::B, false);
	EXPECT_EQ(Switches(), 0);
}

// ----------------------------------------------------------------------------------------------------
// Drums
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, DrumFourHitsOfThePad)
{
	Boot("NM00023");
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::DRUM);
	// Player 1: Don left 0, Don right 3, Ka left 5, Ka right 4.
	Press(GIB::DPadUp);
	EXPECT_TRUE(ACJV::GetDrumHit(0));
	EXPECT_FALSE(ACJV::GetDrumHit(3));
	Press(GIB::DPadLeft);
	Release(GIB::DPadUp);
	EXPECT_TRUE(ACJV::GetDrumHit(0)); // Another d-pad direction still holds it.
	Release(GIB::DPadLeft);
	EXPECT_FALSE(ACJV::GetDrumHit(0));

	for (const GIB face : {GIB::Cross, GIB::Circle, GIB::Square, GIB::Triangle})
	{
		Press(face);
		EXPECT_TRUE(ACJV::GetDrumHit(3));
		Release(face);
		EXPECT_FALSE(ACJV::GetDrumHit(3));
	}
	for (const GIB rim : {GIB::L1, GIB::L2})
	{
		Press(rim);
		EXPECT_TRUE(ACJV::GetDrumHit(5));
		Release(rim);
		EXPECT_FALSE(ACJV::GetDrumHit(5));
	}
	for (const GIB rim : {GIB::R1, GIB::R2})
	{
		Press(rim);
		EXPECT_TRUE(ACJV::GetDrumHit(4));
		Release(rim);
		EXPECT_FALSE(ACJV::GetDrumHit(4));
	}
	// Drum hits are not switches.
	Press(GIB::Cross);
	EXPECT_EQ(Switches(), 0);
	Release(GIB::Cross);
	EXPECT_EQ(PressedAlone(GIB::Start), JVS_BTN_START);
}

TEST_F(ArcadeInputTest, DrumPlayer2HasTheOtherFourChannels)
{
	Boot("NM00023");
	Press(GIB::DPadUp, 1);
	EXPECT_TRUE(ACJV::GetDrumHit(2));
	EXPECT_FALSE(ACJV::GetDrumHit(0));
	Release(GIB::DPadUp, 1);
	Press(GIB::Cross, 1);
	EXPECT_TRUE(ACJV::GetDrumHit(7));
	Release(GIB::Cross, 1);
	Press(GIB::L1, 1);
	EXPECT_TRUE(ACJV::GetDrumHit(1));
	Release(GIB::L1, 1);
	Press(GIB::R1, 1);
	EXPECT_TRUE(ACJV::GetDrumHit(6));
	Release(GIB::R1, 1);
	for (u32 channel = 0; channel < JVS_DRUM_CHANNEL_MAX; channel++)
		EXPECT_FALSE(ACJV::GetDrumHit(channel)) << channel;
}

// ----------------------------------------------------------------------------------------------------
// Twin sticks
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, TwinStickTheSticksAreTheLeversAndTheTriggersAreTheTriggers)
{
	Boot("NM00016");
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::TWINSTICK);

	// Left lever: the D-pad or the left stick.
	EXPECT_EQ(PressedAlone(GIB::DPadUp), 0x0001);
	EXPECT_EQ(PressedAlone(GIB::DPadDown), 0x8000);
	EXPECT_EQ(PressedAlone(GIB::DPadLeft), 0x4000);
	EXPECT_EQ(PressedAlone(GIB::DPadRight), 0x2000);
	Send(GIB::LeftStickUp, 1.0f);
	EXPECT_EQ(Switches(), 0x0001);
	Send(GIB::LeftStickUp, 0.0f);

	// Right lever: the right stick.
	Send(GIB::RightStickUp, 1.0f);
	EXPECT_EQ(Switches(), 0x0010);
	Send(GIB::RightStickUp, 0.0f);
	Send(GIB::RightStickDown, 1.0f);
	EXPECT_EQ(Switches(), 0x0008);
	Send(GIB::RightStickDown, 0.0f);
	Send(GIB::RightStickLeft, 1.0f);
	EXPECT_EQ(Switches(), 0x0004);
	Send(GIB::RightStickLeft, 0.0f);
	Send(GIB::RightStickRight, 1.0f);
	EXPECT_EQ(Switches(), 0x0002);
	Send(GIB::RightStickRight, 0.0f);
	EXPECT_EQ(Switches(), 0);

	EXPECT_EQ(PressedAlone(GIB::L2), 0x0400);
	EXPECT_EQ(PressedAlone(GIB::R2), 0x1000);
	EXPECT_EQ(PressedAlone(GIB::L1), 0x0200);
	EXPECT_EQ(PressedAlone(GIB::R1), 0x0800);
	EXPECT_EQ(PressedAlone(GIB::Start), JVS_BTN_START);
	// The face buttons are not wired.
	EXPECT_EQ(PressedAlone(GIB::Cross), 0);
	EXPECT_EQ(PressedAlone(GIB::Square), 0);
}

TEST_F(ArcadeInputTest, TwinStickOneCabinetOnePlayer)
{
	Boot("NM00016");
	Press(GIB::DPadUp, 1);
	Press(GIB::Start, 1);
	EXPECT_EQ(Switches(1), 0);
}

// ----------------------------------------------------------------------------------------------------
// Touch panel
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, TouchLeverAndSixButtonsLikeTheDefaultAndR2Touches)
{
	Boot("NM00014");
	EXPECT_EQ(ArcadeInput::GetMode(), JVS_MODE::TOUCH);
	EXPECT_EQ(PressedAlone(GIB::DPadUp), JVS_BTN_UP);
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_4);
	EXPECT_EQ(PressedAlone(GIB::Start), JVS_BTN_START);

	EXPECT_FALSE(ACJV::GetTouchPressed());
	Press(GIB::R2);
	EXPECT_TRUE(ACJV::GetTouchPressed());
	EXPECT_EQ(Switches(), 0); // Touching is not a switch.
	Release(GIB::R2);
	EXPECT_FALSE(ACJV::GetTouchPressed());
}

TEST_F(ArcadeInputTest, TouchGunLayerTouchesAndHasThreeButtons)
{
	Boot("NM00014");
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::Trigger, true);
	EXPECT_TRUE(ACJV::GetTouchPressed());
	// The controller letting go does not let go of the mouse's touch.
	Press(GIB::R2);
	Release(GIB::R2);
	EXPECT_TRUE(ACJV::GetTouchPressed());
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::Trigger, false);
	EXPECT_FALSE(ACJV::GetTouchPressed());

	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::A, true);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::C, true);
	EXPECT_EQ(Switches(), JVS_BTN_1 | JVS_BTN_3);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::A, false);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::C, false);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::B, true);
	EXPECT_EQ(Switches(), JVS_BTN_2);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::B, false);
	EXPECT_EQ(Switches(), 0);
}

TEST_F(ArcadeInputTest, GunLayerButtonsDoNothingOnOtherCabinets)
{
	Boot("NM00004");
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::A, true);
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::Trigger, true);
	EXPECT_EQ(Switches(), 0);
	EXPECT_FALSE(ACJV::GetTouchPressed());
	// The coin is every cabinet's.
	ArcadeInput::SetGunButton(0, ArcadeInput::GunButton::Coin, true);
	EXPECT_EQ(ACJV::coin[0], 1);
}

// ----------------------------------------------------------------------------------------------------
// The player's own layout
// ----------------------------------------------------------------------------------------------------

TEST_F(ArcadeInputTest, RemapAButtonDoesTheOtherButtonsJobAndLosesItsOwn)
{
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::Cross, GIB::Square}}));
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_1); // Square's job
	EXPECT_EQ(PressedAlone(GIB::Square), JVS_BTN_1); // Still its own.
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_5);
	// Nothing does button 4's job now.
	EXPECT_EQ(PressedAlone(GIB::Triangle), JVS_BTN_2);
}

TEST_F(ArcadeInputTest, RemapToNothingSilencesTheButton)
{
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::Cross, GIB::Unknown}, {GIB::Select, GIB::Unknown}}));
	EXPECT_EQ(PressedAlone(GIB::Cross), 0);
	Press(GIB::Select);
	EXPECT_EQ(ACJV::coin[0], 0); // No coin from Select now.
	Release(GIB::Select);
}

TEST_F(ArcadeInputTest, RemapAnotherButtonToSelectInsertsTheCoin)
{
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::L3, GIB::Select}}));
	Press(GIB::L3);
	EXPECT_EQ(ACJV::coin[0], 1);
	Release(GIB::L3);
	EXPECT_EQ(Switches(), 0);
}

TEST_F(ArcadeInputTest, RemapSwapsTwoButtons)
{
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::Cross, GIB::Circle}, {GIB::Circle, GIB::Cross}}));
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_5);
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_4);
}

TEST_F(ArcadeInputTest, RemapTwoButtonsOnOneJobAndEitherHoldsIt)
{
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::Cross, GIB::Square}}));
	Press(GIB::Cross);
	Press(GIB::Square);
	Release(GIB::Cross);
	EXPECT_EQ(Switches(), JVS_BTN_1); // Square is still down.
	Release(GIB::Square);
	EXPECT_EQ(Switches(), 0);
}

TEST_F(ArcadeInputTest, RemapOnTheDpadMovesTheLever)
{
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::L2, GIB::DPadUp}}));
	EXPECT_EQ(PressedAlone(GIB::L2), JVS_BTN_UP);
	EXPECT_EQ(PressedAlone(GIB::DPadUp), JVS_BTN_UP);
}

TEST_F(ArcadeInputTest, RemapDoesNotMoveTheSticks)
{
	// The sticks are analog axes of the pad, not buttons a layout can give another job.
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::DPadLeft, GIB::DPadRight}}));
	Send(GIB::LeftStickLeft, 1.0f);
	EXPECT_EQ(Switches(), JVS_BTN_LEFT);
	Send(GIB::LeftStickLeft, 0.0f);
	EXPECT_EQ(PressedAlone(GIB::DPadLeft), JVS_BTN_RIGHT);
}

TEST_F(ArcadeInputTest, RemapNeverMovesADrivingCabinetsPedals)
{
	Boot("NM00047");
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::R2, GIB::Cross}, {GIB::L2, GIB::Cross}}));
	Send(GIB::R2, 1.0f);
	Send(GIB::L2, 1.0f);
	EXPECT_FLOAT_EQ(ACJV::GetGas(), 1.0f);
	EXPECT_FLOAT_EQ(ACJV::GetBrake(), 1.0f);
	EXPECT_EQ(Switches(), 0);
}

TEST_F(ArcadeInputTest, RemapOnAFightingCabinetUsesTheJobsOfItsTable)
{
	Boot("NM00004");
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::L1, GIB::Square}, {GIB::R1, GIB::Triangle}}));
	EXPECT_EQ(PressedAlone(GIB::L1), JVS_BTN_1); // Left punch
	EXPECT_EQ(PressedAlone(GIB::R1), JVS_BTN_2); // Right punch
}

TEST_F(ArcadeInputTest, ChangingTheLayoutLetsGoOfWhatIsHeld)
{
	Press(GIB::Cross);
	EXPECT_EQ(Switches(), JVS_BTN_4);
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::Cross, GIB::Square}}));
	EXPECT_EQ(Switches(), 0);
	// The button still held is not held any more as far as the cabinet knows: letting go is quiet.
	Release(GIB::Cross);
	EXPECT_EQ(Switches(), 0);
	EXPECT_EQ(ACJV::coin[0], 0);
}

TEST_F(ArcadeInputTest, TheSameLayoutAgainLeavesWhatIsHeld)
{
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::L3, GIB::Square}}));
	Press(GIB::Cross);
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::L3, GIB::Square}}));
	EXPECT_EQ(Switches(), JVS_BTN_4);
	Release(GIB::Cross);
}

TEST_F(ArcadeInputTest, ASelectHeldThroughALayoutChangeIsNoNewCoin)
{
	Press(GIB::Select);
	EXPECT_EQ(ACJV::coin[0], 1);
	ArcadeInput::SetActiveRemap(MakeRemap({{GIB::L3, GIB::Square}}));
	Press(GIB::Select);
	EXPECT_EQ(ACJV::coin[0], 2); // Let go by the change, so this is a press.
	Release(GIB::Select);
}

// ----------------------------------------------------------------------------------------------------
// Remap text and settings
// ----------------------------------------------------------------------------------------------------

TEST(ArcadeRemapText, RoundTrip)
{
	const Remap remap = MakeRemap({{GIB::Cross, GIB::Square}, {GIB::Select, GIB::Unknown}, {GIB::DPadUp, GIB::L3}});
	const std::string text = ArcadeInput::RemapToString(remap);
	// In the settings page's order.
	EXPECT_EQ(text, "Up=L3,Cross=Square,Select=None");
	EXPECT_EQ(ArcadeInput::RemapFromString(text), remap);
}

TEST(ArcadeRemapText, EmptyIsEmpty)
{
	EXPECT_EQ(ArcadeInput::RemapToString({}), "");
	EXPECT_TRUE(ArcadeInput::RemapFromString("").empty());
	EXPECT_TRUE(ArcadeInput::RemapFromString(" , ,").empty());
}

TEST(ArcadeRemapText, HandEditedTextIsReadLeniently)
{
	// Case and spaces do not matter.
	EXPECT_EQ(ArcadeInput::RemapFromString(" cross = SQUARE ,select=none"),
		MakeRemap({{GIB::Cross, GIB::Square}, {GIB::Select, GIB::Unknown}}));
	// A pair that names no button, or has no equals sign, is dropped; the rest stand.
	EXPECT_EQ(ArcadeInput::RemapFromString("Banana=Cross,Cross=Banana,Cross,Circle=Square"),
		MakeRemap({{GIB::Circle, GIB::Square}}));
	// The last of a repeated button stands.
	EXPECT_EQ(ArcadeInput::RemapFromString("Cross=Square,Cross=Circle"), MakeRemap({{GIB::Cross, GIB::Circle}}));
	// The sticks are not buttons.
	EXPECT_TRUE(ArcadeInput::RemapFromString("LeftStickUp=Cross,Cross=LeftStickUp").empty());
}

TEST(ArcadeRemapText, AButtonWithItselfIsKeptForAGameToCancelTheGlobalOne)
{
	const Remap game = ArcadeInput::RemapFromString("Cross=Cross");
	EXPECT_EQ(game, MakeRemap({{GIB::Cross, GIB::Cross}}));
	EXPECT_EQ(ArcadeInput::RemapToString(game), "Cross=Cross");
}

TEST(ArcadeRemapText, EveryRemappableButtonHasAUniqueNameThatReadsBack)
{
	const auto buttons = ArcadeInput::GetRemappableButtons();
	ASSERT_EQ(buttons.size(), 16u);
	for (size_t i = 0; i < buttons.size(); i++)
	{
		EXPECT_NE(buttons[i].binding, GIB::Unknown);
		for (size_t j = 0; j < i; j++)
		{
			EXPECT_NE(buttons[i].binding, buttons[j].binding);
			EXPECT_NE(std::string(buttons[i].name), buttons[j].name);
		}
		const Remap one = MakeRemap({{buttons[i].binding, buttons[(i + 1) % buttons.size()].binding}});
		EXPECT_EQ(ArcadeInput::RemapFromString(ArcadeInput::RemapToString(one)), one) << buttons[i].name;
	}
}

TEST(ArcadeRemapSettings, SaveAndLoadThroughAnIniKey)
{
	MemorySettingsInterface si;
	EXPECT_TRUE(ArcadeInput::LoadRemap(si).empty());

	const Remap remap = MakeRemap({{GIB::Cross, GIB::Square}, {GIB::L3, GIB::Unknown}});
	ArcadeInput::SaveRemap(si, remap);
	EXPECT_EQ(si.GetStringValue("Arcade", "Controls", ""), "Cross=Square,L3=None");
	EXPECT_EQ(ArcadeInput::LoadRemap(si), remap);

	// An empty layout removes the key, and so does resetting it.
	ArcadeInput::SaveRemap(si, {});
	EXPECT_FALSE(si.ContainsValue("Arcade", "Controls"));
	EXPECT_TRUE(ArcadeInput::LoadRemap(si).empty());

	ArcadeInput::SaveRemap(si, remap);
	ASSERT_TRUE(si.ContainsValue("Arcade", "Controls"));
	ArcadeInput::ResetRemap(si);
	EXPECT_FALSE(si.ContainsValue("Arcade", "Controls"));
	EXPECT_TRUE(ArcadeInput::LoadRemap(si).empty());
}

TEST(ArcadeRemapSettings, AGamesPairsGoOverTheGlobalOnesButtonByButton)
{
	const Remap global = MakeRemap({{GIB::Cross, GIB::Square}, {GIB::Circle, GIB::Triangle}});
	const Remap game = MakeRemap({{GIB::Circle, GIB::L1}, {GIB::R3, GIB::Start}});
	EXPECT_EQ(ArcadeInput::MergeRemaps(global, game),
		MakeRemap({{GIB::Cross, GIB::Square}, {GIB::Circle, GIB::L1}, {GIB::R3, GIB::Start}}));
	EXPECT_EQ(ArcadeInput::MergeRemaps(global, {}), global);
	EXPECT_EQ(ArcadeInput::MergeRemaps({}, game), game);
}

TEST(ArcadeRemapSettings, AGameCancelsAGlobalPairWithTheButtonItself)
{
	const Remap global = MakeRemap({{GIB::Cross, GIB::Square}});
	const Remap game = MakeRemap({{GIB::Cross, GIB::Cross}});
	EXPECT_TRUE(ArcadeInput::MergeRemaps(global, game).empty());
}

TEST_F(ArcadeInputTest, SettingsReloadTakesTheGlobalAndTheGamesLayoutsTogether)
{
	MemorySettingsInterface base, game;
	ArcadeInput::SaveRemap(base, MakeRemap({{GIB::Cross, GIB::Square}, {GIB::Circle, GIB::Triangle}}));
	ArcadeInput::SaveRemap(game, MakeRemap({{GIB::Circle, GIB::Circle}}));

	ArcadeInput::ReloadSettings(&base, &game);
	EXPECT_EQ(ArcadeInput::GetActiveRemap(), MakeRemap({{GIB::Cross, GIB::Square}}));
	EXPECT_EQ(PressedAlone(GIB::Cross), JVS_BTN_1);
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_5);

	// A game with no settings file plays on the global layout.
	ArcadeInput::ReloadSettings(&base, nullptr);
	EXPECT_EQ(ArcadeInput::GetActiveRemap(), MakeRemap({{GIB::Cross, GIB::Square}, {GIB::Circle, GIB::Triangle}}));
	EXPECT_EQ(PressedAlone(GIB::Circle), JVS_BTN_2);

	ArcadeInput::ReloadSettings(nullptr, nullptr);
	EXPECT_TRUE(ArcadeInput::GetActiveRemap().empty());
}

// ----------------------------------------------------------------------------------------------------
// The jobs of a cabinet
// ----------------------------------------------------------------------------------------------------

namespace
{
	std::vector<std::string> Labels(const ArcadeInput::Layout& layout)
	{
		std::vector<std::string> labels;
		for (const ArcadeInput::Job& job : layout.jobs)
			labels.push_back(job.label);
		return labels;
	}

	const ArcadeInput::Job* FindJob(const ArcadeInput::Layout& layout, const std::string& label)
	{
		for (const ArcadeInput::Job& job : layout.jobs)
		{
			if (job.label == label)
				return &job;
		}
		return nullptr;
	}
} // namespace

TEST(ArcadeLayout, TheDefaultCabinet)
{
	const ArcadeInput::Layout layout = ArcadeInput::GetLayout("NM00099");
	EXPECT_EQ(layout.mode, JVS_MODE::DEFAULT);
	EXPECT_EQ(Labels(layout), (std::vector<std::string>{"Lever up", "Lever down", "Lever left", "Lever right", "Button 1", "Button 2",
								  "Button 3", "Button 4", "Button 5", "Button 6", "Start", "Coin"}));
	ASSERT_NE(FindJob(layout, "Button 4"), nullptr);
	EXPECT_EQ(FindJob(layout, "Button 4")->Key(), GIB::Cross);
	EXPECT_EQ(FindJob(layout, "Coin")->Key(), GIB::Select);
	for (const ArcadeInput::Job& job : layout.jobs)
		EXPECT_FALSE(job.fixed) << job.label;
}

TEST(ArcadeLayout, AFightingGamesButtonsAreNamedAsItsCabinetNamesThem)
{
	const ArcadeInput::Layout layout = ArcadeInput::GetLayout("NM00004");
	EXPECT_EQ(layout.mode, JVS_MODE::FIGHTING);
	EXPECT_EQ(Labels(layout), (std::vector<std::string>{"Lever up", "Lever down", "Lever left", "Lever right", "Left Punch", "Right Punch",
								  "Left Kick", "Right Kick", "Start", "Coin"}));
	EXPECT_EQ(FindJob(layout, "Left Kick")->Key(), GIB::Cross);
}

TEST(ArcadeLayout, ADrivingCabinetHasFixedPedalsAndTheFreeButtonsOfItsTable)
{
	const ArcadeInput::Layout layout = ArcadeInput::GetLayout("NM00047");
	EXPECT_EQ(layout.mode, JVS_MODE::DRIVE);
	ASSERT_NE(FindJob(layout, "Accelerate"), nullptr);
	ASSERT_NE(FindJob(layout, "Brake"), nullptr);
	EXPECT_TRUE(FindJob(layout, "Accelerate")->fixed);
	EXPECT_TRUE(FindJob(layout, "Brake")->fixed);
	EXPECT_EQ(FindJob(layout, "Accelerate")->Key(), GIB::R2);
	EXPECT_EQ(FindJob(layout, "Shift Up")->Key(), GIB::R1);
	// Button 1 is what Square and Cross both do, and 5 what Circle does.
	EXPECT_EQ(FindJob(layout, "Button 1")->buttons, (std::vector<GIB>{GIB::Square, GIB::Cross}));
	EXPECT_EQ(FindJob(layout, "Button 5")->buttons, (std::vector<GIB>{GIB::Circle}));

	// Battle Gear 3 has no free button.
	const ArcadeInput::Layout bg3 = ArcadeInput::GetLayout("NM00010");
	EXPECT_EQ(FindJob(bg3, "Button 1"), nullptr);
	EXPECT_EQ(FindJob(bg3, "Button 5"), nullptr);
	EXPECT_NE(FindJob(bg3, "Sidebrake"), nullptr);
}

TEST(ArcadeLayout, LightGunDrumAndTwinStickJobs)
{
	const ArcadeInput::Layout gun = ArcadeInput::GetLayout("NM00003");
	EXPECT_EQ(Labels(gun), (std::vector<std::string>{"Trigger", "Pedal", "Start", "Coin"}));
	EXPECT_EQ(FindJob(gun, "Trigger")->buttons, (std::vector<GIB>{GIB::R2, GIB::Cross}));
	EXPECT_EQ(FindJob(gun, "Pedal")->buttons, (std::vector<GIB>{GIB::L1, GIB::L2}));

	const ArcadeInput::Layout drum = ArcadeInput::GetLayout("NM00023");
	EXPECT_EQ(Labels(drum), (std::vector<std::string>{"Don (left)", "Don (right)", "Ka (left)", "Ka (right)", "Start", "Coin"}));
	EXPECT_EQ(FindJob(drum, "Don (left)")->buttons.size(), 4u);

	const ArcadeInput::Layout twin = ArcadeInput::GetLayout("NM00016");
	EXPECT_EQ(Labels(twin), (std::vector<std::string>{"Lever up", "Lever down", "Lever left", "Lever right", "Left Trigger", "Right Trigger",
								"Left Button", "Right Button", "Start", "Coin"}));
	EXPECT_EQ(FindJob(twin, "Left Trigger")->Key(), GIB::L2);
}

TEST_F(ArcadeInputTest, TheRunningGamesModeIsTheOneItWasStartedIn)
{
	// NM00004 forced to drums by its .acgame: its jobs are the drums', not a fighter's.
	Boot("NM00004", JVS_MODE::DRUM);
	EXPECT_EQ(ArcadeInput::GetLayout("NM00004").mode, JVS_MODE::DRUM);
	// Another game's jobs are its own.
	EXPECT_EQ(ArcadeInput::GetLayout("NM00003").mode, JVS_MODE::LIGHTGUN);
	Arcade::s_session = false;
	EXPECT_EQ(ArcadeInput::GetLayout("NM00004").mode, JVS_MODE::FIGHTING);
}

TEST(ArcadeLayout, AButtonsJobIsItsOwnUntilALayoutChangesIt)
{
	const ArcadeInput::Layout layout = ArcadeInput::GetLayout("NM00099");
	const Remap none;
	EXPECT_EQ(ArcadeInput::GetJobOf(layout, none, none, GIB::Cross)->label, "Button 4");
	EXPECT_EQ(ArcadeInput::GetJobOf(layout, none, none, GIB::L3), nullptr); // No job by default.
	EXPECT_EQ(ArcadeInput::GetJobOf(layout, none, none, GIB::Select)->label, "Coin");

	const Remap global = MakeRemap({{GIB::Cross, GIB::Square}, {GIB::Circle, GIB::Unknown}});
	EXPECT_EQ(ArcadeInput::GetJobOf(layout, global, none, GIB::Cross)->label, "Button 1");
	EXPECT_EQ(ArcadeInput::GetJobOf(layout, global, none, GIB::Circle), nullptr);

	// The game's pair comes first.
	const Remap game = MakeRemap({{GIB::Cross, GIB::Triangle}, {GIB::Circle, GIB::Circle}});
	EXPECT_EQ(ArcadeInput::GetJobOf(layout, global, game, GIB::Cross)->label, "Button 2");
	EXPECT_EQ(ArcadeInput::GetJobOf(layout, global, game, GIB::Circle)->label, "Button 5");
}

TEST(ArcadeLayout, SettingAGamesJobKeepsOnlyWhatDiffersFromTheGlobalLayout)
{
	const ArcadeInput::Layout layout = ArcadeInput::GetLayout("NM00099");
	const ArcadeInput::Job* button1 = FindJob(layout, "Button 1");
	const ArcadeInput::Job* button4 = FindJob(layout, "Button 4");
	const ArcadeInput::Job* coin = FindJob(layout, "Coin");

	// No global layout: the job a button has anyway is no pair.
	Remap game;
	const Remap none;
	ArcadeInput::SetJob(game, none, layout, GIB::Cross, button4);
	EXPECT_TRUE(game.empty());
	ArcadeInput::SetJob(game, none, layout, GIB::Cross, button1);
	EXPECT_EQ(game, MakeRemap({{GIB::Cross, GIB::Square}}));
	ArcadeInput::SetJob(game, none, layout, GIB::Cross, nullptr);
	EXPECT_EQ(game, MakeRemap({{GIB::Cross, GIB::Unknown}}));
	ArcadeInput::SetJob(game, none, layout, GIB::Cross, button4);
	EXPECT_TRUE(game.empty());
	ArcadeInput::SetJob(game, none, layout, GIB::L3, coin);
	EXPECT_EQ(game, MakeRemap({{GIB::L3, GIB::Select}}));
	ArcadeInput::SetJob(game, none, layout, GIB::L3, nullptr); // L3 has no job anyway.
	EXPECT_TRUE(game.empty());

	// A global layout moved Cross to button 1: the game choosing button 1 changes nothing, choosing its
	// own button 4 back is a pair of the button with itself.
	const Remap global = MakeRemap({{GIB::Cross, GIB::Square}});
	game.clear();
	ArcadeInput::SetJob(game, global, layout, GIB::Cross, button1);
	EXPECT_TRUE(game.empty());
	ArcadeInput::SetJob(game, global, layout, GIB::Cross, button4);
	EXPECT_EQ(game, MakeRemap({{GIB::Cross, GIB::Cross}}));
	EXPECT_TRUE(ArcadeInput::MergeRemaps(global, game).empty());
	EXPECT_EQ(ArcadeInput::GetJobOf(layout, global, game, GIB::Cross)->label, "Button 4");
}

TEST(ArcadeLayout, SettingAGlobalTarget)
{
	Remap global;
	ArcadeInput::SetGlobalTarget(global, GIB::Cross, GIB::Circle);
	EXPECT_EQ(global, MakeRemap({{GIB::Cross, GIB::Circle}}));
	ArcadeInput::SetGlobalTarget(global, GIB::L3, GIB::Unknown);
	EXPECT_EQ(global, MakeRemap({{GIB::Cross, GIB::Circle}, {GIB::L3, GIB::Unknown}}));
	ArcadeInput::SetGlobalTarget(global, GIB::Cross, GIB::Cross);
	EXPECT_EQ(global, MakeRemap({{GIB::L3, GIB::Unknown}}));
}
