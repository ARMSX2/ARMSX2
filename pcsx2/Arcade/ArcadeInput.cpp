// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// See ArcadeInput.h. The mapping below is a port of the Android arcade bridge (native-lib.cpp:
// ArcadeApplyPad, ArcadeGunButton, ArcadeJobs), keyed on GenericInputBinding instead of Android keycodes.
// Where the two differ, a comment says so. GetLayout() mirrors ApplyPad(): keep the two in step.

#include "Arcade/ArcadeInput.h"

#include "DEV9/ACJV.h"
#include "Host.h"

#include "common/ARCADE.h"
#include "common/SettingsInterface.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace
{
	using GIB = GenericInputBinding;
	using ArcadeInput::Remap;

	constexpr size_t NUM_GENERIC = static_cast<size_t>(GIB::Count);
	constexpr u32 NUM_PLAYERS = JVS_PLAYER_COUNT;

	// A button counts as held from here, and a stick as pushed from here (the lever).
	constexpr float PRESS_THRESHOLD = 0.5f;
	// The right stick takes over a light gun's or touch panel's aim from the pointer past here.
	constexpr float AIM_DEADZONE = 0.2f;

	// The twin-stick cabinet's switch bits that are not a lever: its two triggers, two buttons and Start.
	constexpr u16 TWINSTICK_NON_LEVER_BITS = 0x0400 | 0x1000 | 0x0200 | 0x0800 | JVS_BTN_START;

	constexpr size_t Idx(GIB b)
	{
		return static_cast<size_t>(b);
	}

	struct Player
	{
		float value[NUM_GENERIC] = {}; // The last value of each generic binding, 0..1.
		bool held[NUM_GENERIC] = {}; // value >= PRESS_THRESHOLD, for the remappable buttons: before the layout.
		bool coin_held = false; // The coin job was held at the last ApplyPad, for its rising edge.
		u16 pad_bits = 0; // JVS switches the controller holds.
		u16 ext_bits = 0; // JVS switches the gun layer and the Service button hold.
		bool stick_aim = false; // The right stick took the aim over from the pointer.
		bool pad_reload = false; // Vampire Night: the pedal button forces the camera-lost report.
		bool touch_offscreen = false; // A shot fired beside the screen.
		bool touch_pad = false; // The controller touches the touch panel (R2).
		bool touch_ext = false; // A mouse button or the gun layer does.
	};

	std::mutex s_mutex; // Guards the three below.
	Player s_players[NUM_PLAYERS];
	Remap s_remap;

	std::mutex s_runner_mutex;
	std::function<void(std::function<void()>)> s_board_runner;

	// ------------------------------------------------------------------------------------------------
	// Tables
	// ------------------------------------------------------------------------------------------------

	constexpr ArcadeInput::ModeInfo s_modes[] = {
		{JVS_MODE::DEFAULT, "default", TRANSLATE_NOOP("Arcade", "Generic"),
			TRANSLATE_NOOP("Arcade", "The left stick works the lever too.")},
		{JVS_MODE::LIGHTGUN, "lightgun", TRANSLATE_NOOP("Arcade", "Light gun"),
			TRANSLATE_NOOP("Arcade", "The right stick or the mouse aims. R2 or Cross fires, L1 or L2 is the pedal.")},
		{JVS_MODE::FIGHTING, "fighting", TRANSLATE_NOOP("Arcade", "Fighting"),
			TRANSLATE_NOOP("Arcade", "The left stick works the lever too.")},
		{JVS_MODE::DRIVE, "racing", TRANSLATE_NOOP("Arcade", "Driving"),
			TRANSLATE_NOOP("Arcade", "The left stick steers, R2 accelerates and L2 brakes.")},
		{JVS_MODE::DRUM, "drum", TRANSLATE_NOOP("Arcade", "Drums"),
			TRANSLATE_NOOP("Arcade", "The D-pad hits the left of the drum, the face buttons the right; L1 and L2 hit the left rim, R1 and R2 the right rim.")},
		{JVS_MODE::TOUCH, "touch", TRANSLATE_NOOP("Arcade", "Touch panel"),
			TRANSLATE_NOOP("Arcade", "The right stick or the mouse points, R2 touches. The left stick works the lever too.")},
		{JVS_MODE::STANDARD, "standard", TRANSLATE_NOOP("Arcade", "Standard"),
			TRANSLATE_NOOP("Arcade", "The left stick works the lever too.")},
		{JVS_MODE::TWINSTICK, "twinstick", TRANSLATE_NOOP("Arcade", "Twin sticks"),
			TRANSLATE_NOOP("Arcade", "The two sticks are the cabinet's levers.")},
	};

	// The order of Android's list, and so of its settings page.
	constexpr ArcadeInput::ButtonInfo s_buttons[] = {
		{GIB::DPadUp, "Up", TRANSLATE_NOOP("Arcade", "D-Pad Up")},
		{GIB::DPadDown, "Down", TRANSLATE_NOOP("Arcade", "D-Pad Down")},
		{GIB::DPadLeft, "Left", TRANSLATE_NOOP("Arcade", "D-Pad Left")},
		{GIB::DPadRight, "Right", TRANSLATE_NOOP("Arcade", "D-Pad Right")},
		{GIB::Cross, "Cross", TRANSLATE_NOOP("Arcade", "Cross")},
		{GIB::Circle, "Circle", TRANSLATE_NOOP("Arcade", "Circle")},
		{GIB::Square, "Square", TRANSLATE_NOOP("Arcade", "Square")},
		{GIB::Triangle, "Triangle", TRANSLATE_NOOP("Arcade", "Triangle")},
		{GIB::L1, "L1", TRANSLATE_NOOP("Arcade", "L1")},
		{GIB::R1, "R1", TRANSLATE_NOOP("Arcade", "R1")},
		{GIB::L2, "L2", TRANSLATE_NOOP("Arcade", "L2")},
		{GIB::R2, "R2", TRANSLATE_NOOP("Arcade", "R2")},
		{GIB::L3, "L3", TRANSLATE_NOOP("Arcade", "L3")},
		{GIB::R3, "R3", TRANSLATE_NOOP("Arcade", "R3")},
		{GIB::Start, "Start", TRANSLATE_NOOP("Arcade", "Start")},
		{GIB::Select, "Select", TRANSLATE_NOOP("Arcade", "Select")},
	};

	const ArcadeInput::ButtonInfo* FindButton(GIB binding)
	{
		for (const ArcadeInput::ButtonInfo& bi : s_buttons)
		{
			if (bi.binding == binding)
				return &bi;
		}
		return nullptr;
	}

	const ArcadeInput::ButtonInfo* FindButton(std::string_view name)
	{
		for (const ArcadeInput::ButtonInfo& bi : s_buttons)
		{
			if (StringUtil::compareNoCase(bi.name, name))
				return &bi;
		}
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------------
	// Posting to the CPU thread
	// ------------------------------------------------------------------------------------------------

	// The game takes coins off the same counter, on the CPU thread, so a coin and the test switch are
	// changed there.
	void PostToBoard(std::function<void()> function)
	{
		std::function<void(std::function<void()>)> runner;
		{
			std::lock_guard lock(s_runner_mutex);
			runner = s_board_runner;
		}
		if (runner)
			runner(std::move(function));
		else
			Host::RunOnCPUThread(std::move(function));
	}

	void PostCoin(u32 player)
	{
		PostToBoard([player]() {
			if (Arcade::IsActive())
				ACJV::InsertCoin(player);
		});
	}

	// ------------------------------------------------------------------------------------------------
	// The mapping
	// ------------------------------------------------------------------------------------------------

	// The analog pedals of a driving cabinet stay on their triggers; no layout moves them.
	bool IsFixedButton(JVS_MODE mode, GIB button)
	{
		return mode == JVS_MODE::DRIVE && (button == GIB::L2 || button == GIB::R2);
	}

	// Which jobs the controller's buttons do: each held button does its own job, or the one the player's
	// layout gives it. A job is named by the button that does it by default. Two buttons given one job both
	// hold it (Android lets the later press win).
	void ComputeJobs(const Player& p, JVS_MODE mode, bool (&jobs)[NUM_GENERIC])
	{
		std::fill(std::begin(jobs), std::end(jobs), false);
		for (const ArcadeInput::ButtonInfo& bi : s_buttons)
		{
			if (!p.held[Idx(bi.binding)])
				continue;
			GIB job = bi.binding;
			if (!IsFixedButton(mode, bi.binding))
			{
				const auto it = s_remap.find(bi.binding);
				if (it != s_remap.end())
					job = it->second;
			}
			if (job != GIB::Unknown)
				jobs[Idx(job)] = true;
		}
	}

	// The switches this player's controls own in the current game: everything, except on a light gun
	// game, where the gun's sensor bit belongs to ACJV's aim code and the rest of the word is unwired.
	u16 OwnedBits(u32 player)
	{
		if (ACJV::GetMode() != JVS_MODE::LIGHTGUN)
			return 0xFFFF;
		const GunMapping& gm = ACJV::GetGunMapping();
		const u16 start = (player == 0) ? gm.p1_start : gm.p2_start;
		// ARMSX2: Service is the player's too (Android leaves it out, so Service does nothing on a gun game).
		return static_cast<u16>(((player == 0) ? (gm.p1_trigger | gm.pedal) : gm.p2_trigger) |
								(start ? start : static_cast<u16>(JVS_BTN_START)) | JVS_BTN_SERVICE);
	}

	void Commit(u32 player)
	{
		const Player& p = s_players[player];
		const JVS_MODE mode = ACJV::GetMode();
		const u16 owned = OwnedBits(player);
		const u16 want = p.pad_bits | p.ext_bits;
		ACJV::SetButtonState(player, owned & want, true);
		ACJV::SetButtonState(player, owned & static_cast<u16>(~want), false);
		if (mode == JVS_MODE::LIGHTGUN && player == 0)
			ACJV::SetGunForceOffscreen(s_players[0].pad_reload || s_players[0].touch_offscreen);
		if (mode == JVS_MODE::TOUCH)
		{
			ACJV::SetTouchPressed(s_players[0].touch_pad || s_players[0].touch_ext || s_players[1].touch_ext);
		}
	}

	// Recomputes the cabinet controls one player's controller holds.
	void ApplyPad(u32 player)
	{
		Player& p = s_players[player];
		const JVS_MODE mode = ACJV::GetMode();
		bool jobs[NUM_GENERIC];
		ComputeJobs(p, mode, jobs);

		const auto held = [&jobs](GIB b) { return jobs[Idx(b)]; };
		const auto pushed = [&p](GIB stick) { return p.value[Idx(stick)] >= PRESS_THRESHOLD; };
		u16 bits = 0;

		// The lever: the D-pad, and the left stick past half way when the cabinet has one lever.
		const auto lever = [&](bool left_stick) {
			u16 dir = 0;
			if (held(GIB::DPadUp) || (left_stick && pushed(GIB::LeftStickUp)))
				dir |= JVS_BTN_UP;
			if (held(GIB::DPadDown) || (left_stick && pushed(GIB::LeftStickDown)))
				dir |= JVS_BTN_DOWN;
			if (held(GIB::DPadLeft) || (left_stick && pushed(GIB::LeftStickLeft)))
				dir |= JVS_BTN_LEFT;
			if (held(GIB::DPadRight) || (left_stick && pushed(GIB::LeftStickRight)))
				dir |= JVS_BTN_RIGHT;
			return dir;
		};
		// A game's own table of buttons: each binding's generic mapping says which button does it.
		const auto table = [&](std::span<const InputBindingInfo> buttons) {
			u16 sw = 0;
			for (const InputBindingInfo& bi : buttons)
			{
				if (bi.generic_mapping != GIB::Unknown && held(bi.generic_mapping))
					sw |= bi.bind_index;
			}
			return sw;
		};
		// The default cabinet: six buttons, laid out like a PS2 port of an arcade fighter.
		const auto six_buttons = [&]() {
			u16 sw = 0;
			if (held(GIB::Square))
				sw |= JVS_BTN_1;
			if (held(GIB::Triangle))
				sw |= JVS_BTN_2;
			if (held(GIB::L1))
				sw |= JVS_BTN_3;
			if (held(GIB::Cross))
				sw |= JVS_BTN_4;
			if (held(GIB::Circle))
				sw |= JVS_BTN_5;
			if (held(GIB::R1))
				sw |= JVS_BTN_6;
			return sw;
		};
		// The right stick as a position across the whole picture: -1..1 on each axis.
		const float aim_x = p.value[Idx(GIB::RightStickRight)] - p.value[Idx(GIB::RightStickLeft)];
		const float aim_y = p.value[Idx(GIB::RightStickDown)] - p.value[Idx(GIB::RightStickUp)];
		const bool aim_moved = std::abs(aim_x) > AIM_DEADZONE || std::abs(aim_y) > AIM_DEADZONE;

		switch (mode)
		{
			case JVS_MODE::LIGHTGUN:
			{
				const GunMapping& gm = ACJV::GetGunMapping();
				const u16 trigger = (player == 0) ? gm.p1_trigger : gm.p2_trigger;
				const u16 start = (player == 0) ? gm.p1_start : gm.p2_start;
				if (held(GIB::R2) || held(GIB::Cross))
					bits |= trigger;
				if (held(GIB::Start))
					bits |= start ? start : static_cast<u16>(JVS_BTN_START);
				const bool pedal = held(GIB::L1) || held(GIB::L2);
				if (player == 0)
				{
					if (gm.pedal)
						bits |= pedal ? gm.pedal : 0;
					else
						p.pad_reload = pedal;
				}
				// The right stick aims across the whole picture. Once moved it keeps the aim until the pointer
				// takes it back (OnPointerMoved).
				if (!p.stick_aim && aim_moved)
				{
					p.stick_aim = true;
					ACJV::SetGunAimSource(player, true);
				}
				if (p.stick_aim)
					ACJV::SetGunRelativeAim(player, 0.5f + aim_x * 0.5f, 0.5f + aim_y * 0.5f);
			}
			break;

			case JVS_MODE::DRIVE:
			{
				// One player per cabinet.
				if (player != 0)
					break;
				ACJV::SetWheelAxis(0, p.value[Idx(GIB::LeftStickRight)]);
				ACJV::SetWheelAxis(1, p.value[Idx(GIB::LeftStickLeft)]);
				ACJV::SetWheelAxis(2, p.value[Idx(GIB::R2)]);
				ACJV::SetWheelAxis(3, p.value[Idx(GIB::L2)]);
				const std::span<const InputBindingInfo> buttons = ACJV::GetRacingButtons();
				bits |= lever(false) | (buttons.empty() ? six_buttons() : table(buttons));
				if (held(GIB::Start))
					bits |= JVS_BTN_START;
				// A test menu's ENTER switch is a cabinet button no racing control uses: in Ace Driver 3 the
				// lever moves the highlight, and neither Start nor View enters. So the face buttons the game's
				// racing table leaves free work the cabinet's free buttons: Square and Cross button 1 (Ridge
				// Racer V's board takes ENTER from Square in PCSX2x6), Circle button 5.
				if (!buttons.empty())
				{
					u16 taken = 0;
					for (const InputBindingInfo& bi : buttons)
						taken |= static_cast<u16>(bi.bind_index);
					const auto free_button = [&](GIB pad, u16 bit) {
						const bool used = std::any_of(buttons.begin(), buttons.end(),
							[pad](const InputBindingInfo& bi) { return bi.generic_mapping == pad; });
						if (!used && !(taken & bit) && held(pad))
							bits |= bit;
					};
					free_button(GIB::Square, JVS_BTN_1);
					free_button(GIB::Cross, JVS_BTN_1);
					free_button(GIB::Circle, JVS_BTN_5);
				}
			}
			break;

			case JVS_MODE::TWINSTICK:
			{
				// One player per cabinet (versus is between cabinets). The D-pad and the left stick are the left
				// lever, the right stick the right one.
				if (player != 0)
					break;
				if (held(GIB::DPadUp) || pushed(GIB::LeftStickUp))
					bits |= 0x0001;
				if (held(GIB::DPadDown) || pushed(GIB::LeftStickDown))
					bits |= 0x8000;
				if (held(GIB::DPadLeft) || pushed(GIB::LeftStickLeft))
					bits |= 0x4000;
				if (held(GIB::DPadRight) || pushed(GIB::LeftStickRight))
					bits |= 0x2000;
				if (pushed(GIB::RightStickUp))
					bits |= 0x0010;
				if (pushed(GIB::RightStickDown))
					bits |= 0x0008;
				if (pushed(GIB::RightStickLeft))
					bits |= 0x0004;
				if (pushed(GIB::RightStickRight))
					bits |= 0x0002;
				bits |= table(ACJV::GetTwinstickBindings()) & TWINSTICK_NON_LEVER_BITS;
			}
			break;

			case JVS_MODE::DRUM:
			{
				// 1P: Don left/right on channels 0/3, Ka left/right on 5/4. 2P: 2/7 and 1/6 (ACJV_Inputs.h).
				const bool p1 = (player == 0);
				ACJV::SetDrumHit(p1 ? 0 : 2, held(GIB::DPadUp) || held(GIB::DPadDown) || held(GIB::DPadLeft) || held(GIB::DPadRight));
				ACJV::SetDrumHit(p1 ? 3 : 7, held(GIB::Cross) || held(GIB::Circle) || held(GIB::Square) || held(GIB::Triangle));
				ACJV::SetDrumHit(p1 ? 5 : 1, held(GIB::L1) || held(GIB::L2));
				ACJV::SetDrumHit(p1 ? 4 : 6, held(GIB::R1) || held(GIB::R2));
				if (held(GIB::Start))
					bits |= JVS_BTN_START;
			}
			break;

			default: // Fighting, standard, touch panel and unknown games.
			{
				std::span<const InputBindingInfo> buttons;
				if (mode == JVS_MODE::FIGHTING)
					buttons = ACJV::GetFightingButtons();
				else if (mode == JVS_MODE::STANDARD)
					buttons = ACJV::GetStandardButtons();
				bits |= lever(true) | (buttons.empty() ? six_buttons() : table(buttons));
				if (held(GIB::Start))
					bits |= JVS_BTN_START;

				// ARMSX2: a touch panel has no touchscreen to work it here (Android's gun layer is that). R2 touches
				// it, and the right stick points, like the light gun's.
				if (mode == JVS_MODE::TOUCH && player == 0)
				{
					p.touch_pad = held(GIB::R2);
					if (!p.stick_aim && aim_moved)
					{
						p.stick_aim = true;
						ACJV::SetTouchRelativeActive(true);
					}
					if (p.stick_aim)
					{
						ACJV::SetTouchRelativeAxis(0, p.value[Idx(GIB::RightStickLeft)]);
						ACJV::SetTouchRelativeAxis(1, p.value[Idx(GIB::RightStickRight)]);
						ACJV::SetTouchRelativeAxis(2, p.value[Idx(GIB::RightStickUp)]);
						ACJV::SetTouchRelativeAxis(3, p.value[Idx(GIB::RightStickDown)]);
					}
				}
			}
			break;
		}

		// Select is the coin, on the press.
		const bool coin = held(GIB::Select);
		if (coin && !p.coin_held)
			PostCoin(player);
		p.coin_held = coin;

		p.pad_bits = bits;
		Commit(player);
	}

	void ApplyAll()
	{
		for (u32 player = 0; player < NUM_PLAYERS; player++)
			ApplyPad(player);
	}

	// What is held is let go, as it was held under the old layout. The sticks and pedals stay as they are.
	void ReleaseButtons()
	{
		for (Player& p : s_players)
		{
			std::fill(std::begin(p.held), std::end(p.held), false);
			p.coin_held = false;
		}
	}

	// ------------------------------------------------------------------------------------------------
	// The jobs of a cabinet
	// ------------------------------------------------------------------------------------------------

	std::string Tr(const char* msg)
	{
		return Host::TranslateToString("Arcade", msg);
	}

	void AddJob(std::vector<ArcadeInput::Job>& jobs, std::string label, std::vector<GIB> buttons, bool fixed = false)
	{
		if (!buttons.empty())
			jobs.push_back({std::move(label), std::move(buttons), fixed});
	}

	// The cabinet's own name for a button, without the "P1 " the twin-lever table starts with.
	std::string CabinetName(const InputBindingInfo& bi)
	{
		std::string name = Host::TranslateToString("JVS", bi.display_name ? bi.display_name : bi.name);
		if (name.rfind("P1 ", 0) == 0)
			name.erase(0, 3);
		return name;
	}

	// A game's own table: a job for each button the pad can do, named as the cabinet names it.
	void AddTableJobs(std::vector<ArcadeInput::Job>& jobs, std::span<const InputBindingInfo> buttons, u16 mask = 0xFFFF)
	{
		for (const InputBindingInfo& bi : buttons)
		{
			if (bi.generic_mapping != GIB::Unknown && FindButton(bi.generic_mapping) && (bi.bind_index & mask) != 0)
				AddJob(jobs, CabinetName(bi), {bi.generic_mapping});
		}
	}
} // namespace

// ----------------------------------------------------------------------------------------------------
// The running game
// ----------------------------------------------------------------------------------------------------

bool ArcadeInput::IsActive()
{
	return Arcade::IsActive();
}

std::string ArcadeInput::GetGameId()
{
	return Arcade::IsActive() ? ACJV::GetGameId() : std::string();
}

JVS_MODE ArcadeInput::GetMode()
{
	return Arcade::IsActive() ? ACJV::GetMode() : JVS_MODE::DEFAULT;
}

// ----------------------------------------------------------------------------------------------------
// Control modes
// ----------------------------------------------------------------------------------------------------

std::span<const ArcadeInput::ModeInfo> ArcadeInput::GetModes()
{
	return s_modes;
}

const ArcadeInput::ModeInfo& ArcadeInput::GetModeInfo(JVS_MODE mode)
{
	const size_t index = static_cast<size_t>(mode);
	return (index < std::size(s_modes)) ? s_modes[index] : s_modes[0];
}

JVS_MODE ArcadeInput::GetModeForGameId(const std::string& gameid)
{
	return ACJV::ResolveModeFromGameId(gameid);
}

// ----------------------------------------------------------------------------------------------------
// Coin, Service and Test
// ----------------------------------------------------------------------------------------------------

void ArcadeInput::InsertCoin(u32 player)
{
	if (!Arcade::IsActive() || player >= NUM_PLAYERS)
		return;
	PostCoin(player);
}

void ArcadeInput::SetService(bool pressed)
{
	if (!Arcade::IsActive())
		return;
	std::lock_guard lock(s_mutex);
	Player& p = s_players[0];
	if (pressed)
		p.ext_bits |= JVS_BTN_SERVICE;
	else
		p.ext_bits &= static_cast<u16>(~JVS_BTN_SERVICE);
	Commit(0);
}

void ArcadeInput::ToggleTest()
{
	if (!Arcade::IsActive())
		return;
	PostToBoard([]() {
		if (Arcade::IsActive())
			ACJV::ToggleDIPSwitchState(0);
	});
}

bool ArcadeInput::IsTestOn()
{
	return Arcade::IsActive() && ACJV::GetDIPSwitchState(0);
}

// ----------------------------------------------------------------------------------------------------
// Controller input
// ----------------------------------------------------------------------------------------------------

bool ArcadeInput::HandleBinding(u32 pad_index, GenericInputBinding binding, float value)
{
	// A cabinet's players are on its JVS board and nothing is plugged into the PS2 controller ports, so a
	// press goes to the board only: also sending it to the emulated DualShock would give the game a second
	// input the real board never has. Slots past the two ports (a multitap's) are not the cabinet's.
	if (!Arcade::IsActive() || pad_index >= NUM_PLAYERS)
		return false;

	const size_t index = Idx(binding);
	std::lock_guard lock(s_mutex);
	Player& p = s_players[pad_index];
	if (index < NUM_GENERIC)
	{
		p.value[index] = value;
		p.held[index] = value >= PRESS_THRESHOLD;
	}
	ApplyPad(pad_index);
	return true;
}

void ArcadeInput::OnPointerMoved()
{
	if (!Arcade::IsActive())
		return;

	std::lock_guard lock(s_mutex);
	Player& p = s_players[0];
	if (!p.stick_aim)
		return;
	p.stick_aim = false;
	if (ACJV::GetMode() == JVS_MODE::TOUCH)
		ACJV::SetTouchRelativeActive(false);
	else
		ACJV::SetGunAimSource(0, false);
}

void ArcadeInput::SetGunButton(u32 player, GunButton button, bool pressed)
{
	if (!Arcade::IsActive() || player >= NUM_PLAYERS)
		return;

	if (button == GunButton::Coin)
	{
		if (pressed)
			PostCoin(player);
		return;
	}

	std::lock_guard lock(s_mutex);
	Player& p = s_players[player];
	const auto ext = [&p](u16 bit, bool on) {
		p.ext_bits = on ? static_cast<u16>(p.ext_bits | bit) : static_cast<u16>(p.ext_bits & ~bit);
	};

	const JVS_MODE mode = ACJV::GetMode();
	if (mode == JVS_MODE::TOUCH)
	{
		switch (button)
		{
			case GunButton::Trigger:
			case GunButton::ShootOffscreen:
				p.touch_ext = pressed;
				break;
			case GunButton::A:
				ext(JVS_BTN_1, pressed);
				break;
			case GunButton::B:
				ext(JVS_BTN_2, pressed);
				break;
			case GunButton::C:
				ext(JVS_BTN_3, pressed);
				break;
			case GunButton::Start:
				ext(JVS_BTN_START, pressed);
				break;
			default:
				break;
		}
	}
	else if (mode == JVS_MODE::LIGHTGUN)
	{
		const GunMapping& gm = ACJV::GetGunMapping();
		const u16 trigger = (player == 0) ? gm.p1_trigger : gm.p2_trigger;
		const u16 start = (player == 0) ? gm.p1_start : gm.p2_start;
		switch (button)
		{
			case GunButton::Trigger:
				ext(trigger, pressed);
				break;
			case GunButton::ShootOffscreen:
				// Fired beside the screen: what Vampire Night reloads with. The other boards read the aim,
				// which a shot at the edge still has on the picture, so it is an ordinary shot there.
				ext(trigger, pressed);
				p.touch_offscreen = pressed;
				break;
			case GunButton::A:
			case GunButton::B:
				if (player == 0 && gm.pedal)
					ext(gm.pedal, pressed);
				else if (player == 0)
					p.touch_offscreen = pressed;
				break;
			case GunButton::Start:
				ext(start ? start : static_cast<u16>(JVS_BTN_START), pressed);
				break;
			default:
				break;
		}
	}
	else
	{
		return;
	}
	Commit(player);
}

void ArcadeInput::Reset()
{
	std::lock_guard lock(s_mutex);
	for (Player& p : s_players)
		p = Player();
	// Player 1 aims with the pointer until the right stick moves; player 2 has no pointer, only the stick.
	ACJV::SetGunAimSource(0, false);
	ACJV::SetGunAimSource(1, true);
	ACJV::SetGunRelativeAim(0, -1.0f, -1.0f);
	ACJV::SetGunRelativeAim(1, -1.0f, -1.0f);
	ACJV::SetGunForceOffscreen(false);
	ACJV::SetTouchPressBound(true);
	ACJV::SetTouchPressed(false);
	ACJV::SetTouchRelativeActive(false);
}

// ----------------------------------------------------------------------------------------------------
// The player's own layout
// ----------------------------------------------------------------------------------------------------

std::span<const ArcadeInput::ButtonInfo> ArcadeInput::GetRemappableButtons()
{
	return s_buttons;
}

std::string ArcadeInput::RemapToString(const Remap& remap)
{
	std::string out;
	for (const ButtonInfo& from : s_buttons)
	{
		const auto it = remap.find(from.binding);
		if (it == remap.end())
			continue;

		const char* to = "None";
		if (it->second != GIB::Unknown)
		{
			const ButtonInfo* target = FindButton(it->second);
			if (!target)
				continue;
			to = target->name;
		}
		if (!out.empty())
			out += ',';
		out += from.name;
		out += '=';
		out += to;
	}
	return out;
}

ArcadeInput::Remap ArcadeInput::RemapFromString(std::string_view text)
{
	Remap remap;
	for (const std::string_view pair : StringUtil::SplitString(text, ',', true))
	{
		const size_t equals = pair.find('=');
		if (equals == std::string_view::npos)
			continue;

		const ButtonInfo* from = FindButton(StringUtil::StripWhitespace(pair.substr(0, equals)));
		if (!from)
			continue;

		const std::string_view to_name = StringUtil::StripWhitespace(pair.substr(equals + 1));
		GIB to = GIB::Unknown;
		if (!StringUtil::compareNoCase(to_name, "None"))
		{
			const ButtonInfo* target = FindButton(to_name);
			if (!target)
				continue;
			to = target->binding;
		}
		remap[from->binding] = to;
	}
	return remap;
}

ArcadeInput::Remap ArcadeInput::LoadRemap(const SettingsInterface& si)
{
	return RemapFromString(si.GetStringValue(REMAP_SECTION, REMAP_KEY, ""));
}

void ArcadeInput::SaveRemap(SettingsInterface& si, const Remap& remap)
{
	const std::string text = RemapToString(remap);
	if (text.empty())
		si.DeleteValue(REMAP_SECTION, REMAP_KEY);
	else
		si.SetStringValue(REMAP_SECTION, REMAP_KEY, text.c_str());
}

void ArcadeInput::ResetRemap(SettingsInterface& si)
{
	si.DeleteValue(REMAP_SECTION, REMAP_KEY);
}

ArcadeInput::Remap ArcadeInput::MergeRemaps(const Remap& global, const Remap& game)
{
	Remap merged = global;
	for (const auto& [from, to] : game)
		merged[from] = to;
	// A game's pair of a button with itself cancels the global one; nothing is left to do for it.
	for (auto it = merged.begin(); it != merged.end();)
	{
		if (it->first == it->second)
			it = merged.erase(it);
		else
			++it;
	}
	return merged;
}

void ArcadeInput::ReloadSettings(const SettingsInterface* base, const SettingsInterface* game)
{
	SetActiveRemap(MergeRemaps(base ? LoadRemap(*base) : Remap(), game ? LoadRemap(*game) : Remap()));
}

void ArcadeInput::SetActiveRemap(const Remap& merged)
{
	std::lock_guard lock(s_mutex);
	// Settings reload for many reasons; a held button is only let go when the layout changed.
	if (merged == s_remap)
		return;
	s_remap = merged;
	ReleaseButtons();
	if (Arcade::IsActive())
		ApplyAll();
}

ArcadeInput::Remap ArcadeInput::GetActiveRemap()
{
	std::lock_guard lock(s_mutex);
	return s_remap;
}

// ----------------------------------------------------------------------------------------------------
// A game's controls, for a settings page
// ----------------------------------------------------------------------------------------------------

ArcadeInput::Layout ArcadeInput::GetLayout(const std::string& gameid)
{
	using GIBs = std::vector<GIB>;

	// The running game's mode is the one it was started in (its .acgame can force one).
	const JVS_MODE mode = (Arcade::IsActive() && ACJV::GetGameId() == gameid) ? ACJV::GetMode() : ACJV::ResolveModeFromGameId(gameid);
	Layout layout;
	layout.mode = mode;
	std::vector<Job>& jobs = layout.jobs;

	const auto add_lever = [&jobs]() {
		AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Lever up")), {GIB::DPadUp});
		AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Lever down")), {GIB::DPadDown});
		AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Lever left")), {GIB::DPadLeft});
		AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Lever right")), {GIB::DPadRight});
	};
	const auto numbered = [](int n) {
		return fmt::format(TRANSLATE_FS("Arcade", "Button {}"), n);
	};
	const auto add_six_buttons = [&]() {
		AddJob(jobs, numbered(1), {GIB::Square});
		AddJob(jobs, numbered(2), {GIB::Triangle});
		AddJob(jobs, numbered(3), {GIB::L1});
		AddJob(jobs, numbered(4), {GIB::Cross});
		AddJob(jobs, numbered(5), {GIB::Circle});
		AddJob(jobs, numbered(6), {GIB::R1});
	};
	const std::string start = Tr(TRANSLATE_NOOP("Arcade", "Start"));

	switch (mode)
	{
		case JVS_MODE::LIGHTGUN:
			AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Trigger")), {GIB::R2, GIB::Cross});
			AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Pedal")), {GIB::L1, GIB::L2});
			AddJob(jobs, start, {GIB::Start});
			break;

		case JVS_MODE::DRIVE:
		{
			const std::span<const InputBindingInfo> buttons = ACJV::GetRacingButtons(gameid);
			add_lever();
			if (buttons.empty())
			{
				add_six_buttons();
			}
			else
			{
				AddTableJobs(jobs, buttons);
				// The face buttons the racing controls leave free work the cabinet's free buttons 1 and 5.
				u16 taken = 0;
				for (const InputBindingInfo& bi : buttons)
					taken |= static_cast<u16>(bi.bind_index);
				const auto free = [&buttons, taken](GIB pad, u16 bit) {
					return !(taken & bit) && std::none_of(buttons.begin(), buttons.end(),
												 [pad](const InputBindingInfo& bi) { return bi.generic_mapping == pad; });
				};
				GIBs one;
				if (free(GIB::Square, JVS_BTN_1))
					one.push_back(GIB::Square);
				if (free(GIB::Cross, JVS_BTN_1))
					one.push_back(GIB::Cross);
				AddJob(jobs, numbered(1), std::move(one));
				if (free(GIB::Circle, JVS_BTN_5))
					AddJob(jobs, numbered(5), {GIB::Circle});
			}
			AddJob(jobs, start, {GIB::Start});
			AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Accelerate")), {GIB::R2}, true);
			AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Brake")), {GIB::L2}, true);
		}
		break;

		case JVS_MODE::DRUM:
			AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Don (left)")), {GIB::DPadUp, GIB::DPadDown, GIB::DPadLeft, GIB::DPadRight});
			AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Don (right)")), {GIB::Cross, GIB::Circle, GIB::Square, GIB::Triangle});
			AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Ka (left)")), {GIB::L1, GIB::L2});
			AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Ka (right)")), {GIB::R1, GIB::R2});
			AddJob(jobs, start, {GIB::Start});
			break;

		case JVS_MODE::TWINSTICK:
			add_lever();
			AddTableJobs(jobs, ACJV::GetTwinstickBindings(), TWINSTICK_NON_LEVER_BITS);
			break;

		default: // Fighting, standard, touch panel and unknown games.
		{
			std::span<const InputBindingInfo> buttons;
			if (mode == JVS_MODE::FIGHTING)
				buttons = ACJV::GetFightingButtons(gameid);
			else if (mode == JVS_MODE::STANDARD)
				buttons = ACJV::GetStandardButtons(gameid);
			add_lever();
			if (buttons.empty())
				add_six_buttons();
			else
				AddTableJobs(jobs, buttons);
			AddJob(jobs, start, {GIB::Start});
			// ARMSX2: the touch panel's touch, which Android's touchscreen does.
			if (mode == JVS_MODE::TOUCH)
				AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Touch")), {GIB::R2});
		}
		break;
	}
	AddJob(jobs, Tr(TRANSLATE_NOOP("Arcade", "Coin")), {GIB::Select});
	return layout;
}

const ArcadeInput::Job* ArcadeInput::GetJobOf(const Layout& layout, const Remap& global, const Remap& game, GenericInputBinding button)
{
	GIB target = button;
	if (const auto it = game.find(button); it != game.end())
		target = it->second;
	else if (const auto it = global.find(button); it != global.end())
		target = it->second;

	if (target == GIB::Unknown)
		return nullptr;
	for (const Job& job : layout.jobs)
	{
		if (std::find(job.buttons.begin(), job.buttons.end(), target) != job.buttons.end())
			return &job;
	}
	return nullptr;
}

void ArcadeInput::SetJob(Remap& game, const Remap& global, const Layout& layout, GenericInputBinding button, const Job* job)
{
	// The job the global remap already gives the button is no change of the game's own. A pair of the button
	// with itself still is one, when the global remap moved it: it puts it back for this game.
	const auto inherited_it = global.find(button);
	const GIB inherited = (inherited_it != global.end()) ? inherited_it->second : button;
	const Job* inherited_job = nullptr;
	if (inherited != GIB::Unknown)
	{
		for (const Job& candidate : layout.jobs)
		{
			if (std::find(candidate.buttons.begin(), candidate.buttons.end(), inherited) != candidate.buttons.end())
			{
				inherited_job = &candidate;
				break;
			}
		}
	}

	if (job == inherited_job)
		game.erase(button);
	else
		game[button] = job ? job->Key() : GIB::Unknown;
}

void ArcadeInput::SetGlobalTarget(Remap& global, GenericInputBinding button, GenericInputBinding target)
{
	if (target == button)
		global.erase(button);
	else
		global[button] = target;
}

// ----------------------------------------------------------------------------------------------------
// Testing
// ----------------------------------------------------------------------------------------------------

void ArcadeInput::SetBoardRunner(std::function<void(std::function<void()>)> runner)
{
	std::lock_guard lock(s_runner_mutex);
	s_board_runner = std::move(runner);
}
