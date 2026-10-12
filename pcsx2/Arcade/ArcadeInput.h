// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 cabinet controls. A retail PS2 reads a DualShock; an arcade cabinet reads its
// buttons, levers, wheel, drums and gun through the JVS I/O board (pcsx2/DEV9/ACJV). This file lays a
// controller's generic bindings (the buttons and sticks every pad type shares, GenericInputBinding) out on
// that board, for each kind of cabinet (JVS_MODE): which button is the lever, which are the cabinet's
// buttons, which is the trigger, the coin, and so on. The layout is the one the Android app has used since
// its arcade bridge, now over generic bindings, so both can run the same code.
//
// While an arcade game runs, input on controller ports 1 and 2 goes to the board and NOT to the emulated
// DualShock (an arcade board has none): InputManager hands each pad binding event to HandleBinding() first.
//
// Threads: every function may be called from any thread. Board changes that the game reads on the CPU
// thread (coins, the test switch) are posted there.
//
// The player's own layout is stored in the INI as one key:
//   [Arcade]
//   Controls = Cross=Square,Select=None
// in the global settings (every arcade game) and in a game's own settings file (that game only), a list of
// "<button>=<button>" pairs: <button> does the job <button> has by default, None for no job. Buttons are
// the names of GetRemappableButtons(). A game's pair overrides the global pair for the same button.

#pragma once

#include "Config.h"
#include "common/Pcsx2Types.h"

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class SettingsInterface;

// The cabinet's control mode, defined in DEV9/ACJV.h (include it for the values).
enum class JVS_MODE;

namespace ArcadeInput
{
	// ------------------------------------------------------------------------------------------------
	// The running game
	// ------------------------------------------------------------------------------------------------

	/// An arcade game is running (its board exists). Everything below that acts on the cabinet does
	/// nothing when this is false.
	bool IsActive();

	/// The running game's ID ("NM00004"), empty when no arcade game runs.
	std::string GetGameId();

	/// The control mode the running game plays in. The game's .acgame can force one (jvsmode), so this is not
	/// always GetModeForGameId(). JVS_MODE::DEFAULT when no arcade game runs.
	JVS_MODE GetMode();

	// ------------------------------------------------------------------------------------------------
	// Control modes
	// ------------------------------------------------------------------------------------------------

	struct ModeInfo
	{
		JVS_MODE mode;
		const char* name; // The .acgame's jvsmode value for it ("fighting", "racing"...); "default" for none.
		const char* display_name; // Translation context "Arcade".
		const char* hint; // One sentence on what the sticks and triggers do in the mode; context "Arcade".
	};

	/// Every control mode, in JVS_MODE order: GetModes()[static_cast<size_t>(mode)] is that mode's.
	std::span<const ModeInfo> GetModes();
	const ModeInfo& GetModeInfo(JVS_MODE mode);

	/// The control mode a game ID gets, before any .acgame override (ACJV::ResolveModeFromGameId).
	JVS_MODE GetModeForGameId(const std::string& gameid);

	// ------------------------------------------------------------------------------------------------
	// Coin, Service and Test
	// ------------------------------------------------------------------------------------------------

	/// A coin in player 1's (0) or player 2's (1) slot. The same as Select on that player's controller.
	void InsertCoin(u32 player);

	/// The cabinet's Service button, held while [pressed]: a credit without a coin, and the way through the
	/// test menus. A frontend that presses it for the player holds it for a moment (Android: 200 ms).
	void SetService(bool pressed);

	/// Flips the board's Test switch: on, the game goes to its test menu; off again to leave it.
	void ToggleTest();
	bool IsTestOn();

	// ------------------------------------------------------------------------------------------------
	// Controller input
	// ------------------------------------------------------------------------------------------------

	/// One pad binding event: controller port [pad_index] (0 is player 1, 1 is player 2), the generic binding
	/// it stands for (GenericInputBinding::Unknown for one with none), its value 0..1 (buttons are held from
	/// 0.5). True when an arcade game runs and the cabinet took the event, which the DualShock must then
	/// not see.
	bool HandleBinding(u32 pad_index, GenericInputBinding binding, float value);

	/// The host pointer (the mouse; the touchscreen) moved. A light gun or touch panel that the right stick
	/// had taken over aims with the pointer again.
	void OnPointerMoved();

	/// Presses on a light gun or touch panel that do not come from a controller: the mouse buttons, a
	/// touchscreen's gun layer. On a light gun: Trigger fires, ShootOffscreen fires beside the screen (Vampire
	/// Night reloads with it), A and B work the pedal (or, on a cabinet with none, the same reload), Start
	/// starts. On a touch panel: Trigger and ShootOffscreen touch it; A, B and C are buttons 1 to 3; Start.
	/// Coin inserts one for [player].
	enum class GunButton
	{
		A,
		B,
		C,
		Trigger,
		ShootOffscreen,
		Start,
		Coin,
	};
	void SetGunButton(u32 player, GunButton button, bool pressed);

	/// A host mouse button went down or up ([button_index] is InputManager's pointer button: 0 left, 1 right,
	/// 2 middle). While an arcade game runs as a light gun or a touch panel the mouse is player 1's gun layer,
	/// and these are its buttons: left is Trigger, right ShootOffscreen (the reload, in the games that reload by
	/// pointing away from the screen), middle A (the pedal; button 1 on a touch panel). Player 2 aims with the
	/// right stick, so the mouse is never hers. Any other button is not the gun's.
	///
	/// Returns true when the cabinet took the click, which the pad bindings must then not see as well. A press is
	/// not taken when no gun or touch panel game runs, or when [ui_has_pointer] (a menu is under the pointer:
	/// clicking Resume must not fire a shot). A release is taken, and let go, whenever its press was, whatever
	/// the game or mode has become since, so nothing stays held.
	bool HandlePointerButton(u32 button_index, bool pressed, bool ui_has_pointer = false);

	/// The mouse buttons work the cabinet now: a light gun or touch panel game runs. A frontend that gives
	/// a mouse click another meaning (a double click that toggles full screen) leaves it alone while this holds.
	bool UsesPointerButtons();

	/// A new arcade game is up: nothing is held, player 1 aims with the pointer and player 2 with the right
	/// stick. Called when the board starts (ACJV::SetGameId); the player's layout stays.
	void Reset();

	// ------------------------------------------------------------------------------------------------
	// The player's own layout (remap)
	// ------------------------------------------------------------------------------------------------

	/// A pad button that can be given another button's job.
	struct ButtonInfo
	{
		GenericInputBinding binding;
		const char* name; // The name in the INI: "Up", "Cross", "L1", "Select"...
		const char* display_name; // Translation context "Arcade".
	};

	/// The buttons of the pad that a layout can change, in the order a settings page lists them. The sticks and
	/// the analog pedals (R2 and L2 on a driving cabinet) stay where they are.
	std::span<const ButtonInfo> GetRemappableButtons();

	/// A button: the button whose job it does instead. GenericInputBinding::Unknown for none. A button that is
	/// not in the map keeps its own job.
	using Remap = std::map<GenericInputBinding, GenericInputBinding>;

	constexpr const char* REMAP_SECTION = "Arcade";
	constexpr const char* REMAP_KEY = "Controls";

	/// "Cross=Square,Select=None". A button with itself ("Cross=Cross") is kept: in a game's remap it puts
	/// back a button the global remap moved.
	std::string RemapToString(const Remap& remap);

	/// The other way. A pair that is not two known buttons is dropped, and the last of a repeated button
	/// stands, as hand-edited INIs go wrong.
	Remap RemapFromString(std::string_view text);

	/// The remap in one settings file: the global settings, or one game's. Empty when it has none.
	Remap LoadRemap(const SettingsInterface& si);

	/// Writes the remap to [si] (the caller saves the file), or removes the key when it is empty.
	void SaveRemap(SettingsInterface& si, const Remap& remap);

	/// Back to the cabinet's own layout in [si]: the global settings reset every arcade game, a game's own
	/// settings that game (the global layout then applies to it again).
	void ResetRemap(SettingsInterface& si);

	/// What a game plays with: the global remap with the game's over it, button by button. A button with itself
	/// is no change, so it is not in the result.
	Remap MergeRemaps(const Remap& global, const Remap& game);

	/// The remap the running cabinet plays with, from the settings (InputManager::ReloadBindings passes both
	/// files on every settings reload, game start included; [game] is null for a game with no settings file).
	/// What is held is let go, as it was held under the old layout.
	void ReloadSettings(const SettingsInterface* base, const SettingsInterface* game);

	/// The same from a remap in hand: the one a settings page just changed, or a test's.
	void SetActiveRemap(const Remap& merged);
	Remap GetActiveRemap();

	// ------------------------------------------------------------------------------------------------
	// A game's controls, for a settings page
	// ------------------------------------------------------------------------------------------------

	/// A job the pad does on a cabinet. Its buttons are the ones that do it by default, the first standing in
	/// for the job when another button is given it. A fixed job is an analog pedal's, which stays on its trigger.
	struct Job
	{
		std::string label; // Translated: "Lever up", "Button 3", the cabinet's own name ("Left Punch").
		std::vector<GenericInputBinding> buttons;
		bool fixed = false;

		GenericInputBinding Key() const { return buttons.front(); }
	};

	struct Layout
	{
		JVS_MODE mode;
		std::vector<Job> jobs;
	};

	/// The jobs of a game ID's cabinet. The mode is the running game's if it is that game, else the one its ID
	/// gets. Works with no game running.
	Layout GetLayout(const std::string& gameid);

	/// The job [button] does under the global and the game's remaps, null for none.
	const Job* GetJobOf(const Layout& layout, const Remap& global, const Remap& game, GenericInputBinding button);

	/// Gives [button] the [job] (null: none) in the game's remap. The job the global remap already gives it is
	/// no change of the game's own, and removes the pair.
	void SetJob(Remap& game, const Remap& global, const Layout& layout, GenericInputBinding button, const Job* job);

	/// Makes [button] act as [target] in every game (Unknown: none; itself: its own, which removes the pair).
	void SetGlobalTarget(Remap& global, GenericInputBinding button, GenericInputBinding target);

	// ------------------------------------------------------------------------------------------------
	// Testing
	// ------------------------------------------------------------------------------------------------

	/// Where the board changes made for InsertCoin and ToggleTest run. Host::RunOnCPUThread unless replaced,
	/// which only tests do (they have no CPU thread). An empty function restores the default.
	void SetBoardRunner(std::function<void(std::function<void()>)> runner);
} // namespace ArcadeInput
