// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What auto-tune writes into a game's own settings file, and how it takes it back.
//
// A kept lever becomes an ordinary key in the game's file (so the game starts tuned next time, and the
// settings screens show it), and an `[AutoTune]` section records what was there before:
//
//   [AutoTune]
//   Change.0 = resolution|EmuCore/GS|upscale_multiplier|=3|2|0.1210
//   Change.1 = iop-thread|EmuCore/Speedhacks|iopThread|~|true|0.1830
//   Rejected.0 = blending@0
//
// A Change line is `lever|section|key|original|new|gain`. The original is `=<text>` when the file had the
// key and `~` when it did not (the game was following the global setting, and a reset must delete the key
// again, not freeze the global value into the file). The new value is the text the file holds. Rejected
// lines are `lever@value`: a step that was tried and did not help.
//
// Everything here works on a SettingsInterface, so it needs no VM.

#pragma once

#include "AutoTune/AutoTuneController.h"

#include <optional>
#include <string>
#include <vector>

class SettingsInterface;

namespace AutoTune
{
	inline constexpr const char* kStoreSection = "AutoTune";

	struct LeverKey
	{
		const char* section;
		const char* key;
	};
	LeverKey KeyForLever(AutoTuneLever lever);

	/// One key auto-tune set in the game's file.
	struct Change
	{
		AutoTuneLever lever = AutoTuneLever::Count;
		std::string section;
		std::string key;
		std::optional<std::string> original; // nullopt: the file did not have the key
		std::string value; // as the file holds it
		double gain = 0.0; // relative gain in speed measured when the lever was kept, compounded over every step kept on this key
	};

	/// True when `section`/`key` is a setting auto-tune writes (one of the keys KeyForLever returns).
	bool IsLeverKey(const char* section, const char* key);

	/// The player changed `section`/`key` by hand in a game's own settings (not the global ones). When it is a
	/// lever key and auto-tune is on for this game, turns it off for the game by writing EmuCore/AutoTune = false
	/// into `game_si`, so it does not move the setting the player just chose. `enabled_globally` is the global
	/// switch, which decides whether a game's file that says nothing is on. Returns true when it wrote. The caller
	/// saves the file. What auto-tune already changed, and the record of it, stay.
	bool DisableForManualChange(SettingsInterface& game_si, const char* section, const char* key, bool enabled_globally);

	void WriteLeverValue(SettingsInterface& si, AutoTuneLever lever, double value);

	std::vector<Change> ReadChanges(const SettingsInterface& si);
	std::vector<AutoTuneRejection> ReadRejected(const SettingsInterface& si);

	/// Sets the lever's key to `value` in `si` and records it. When the key was already one of ours the
	/// first original is kept and the gains compound, so a reset goes back to what the player had.
	Change Commit(SettingsInterface& si, AutoTuneLever lever, double value, double gain);

	/// Remembers that a step did not help. Idempotent.
	void RecordRejected(SettingsInterface& si, const AutoTuneRejection& rejection);

	/// A key a reset put back (or deleted) in the file. `value` is empty when the key was deleted.
	struct Restored
	{
		std::string section;
		std::string key;
		std::optional<std::string> value;
	};

	/// A recorded key a reset did not put back. The Change is what auto-tune wrote.
	struct LeftAlone : Change
	{
		/// The key is not in the file at all, as opposed to holding another value. A frontend that keeps the
		/// player's settings somewhere other than this file (the Android app) can tell from its own copy whether
		/// the key was removed on purpose or only dropped when the file was written again.
		bool absent = false;
	};

	struct ResetResult
	{
		bool had_record = false;
		/// Keys put back to their original value (or deleted, when there was none).
		std::vector<Restored> restored;
		/// Keys the player changed after auto-tune did, or removed from the file; left as they are.
		std::vector<LeftAlone> left_alone;
	};

	/// Puts every recorded key back, except those whose value is no longer the one auto-tune wrote, and
	/// removes the `[AutoTune]` section (the rejected steps too, so a later session may try them again).
	ResetResult Reset(SettingsInterface& si);

	/// One line per change, then one for the steps that did not help, in words the player can read.
	std::vector<std::string> Describe(const SettingsInterface& si);

	/// True when two settings texts mean the same value ("2" and "2.000000", "true" and "1").
	bool SameValue(const std::string& a, const std::string& b);

	/// "Resolution 3x" style label for the OSD and the list. `value` is the lever's value as the controller
	/// uses it.
	std::string LeverLabel(AutoTuneLever lever, double value);
} // namespace AutoTune
