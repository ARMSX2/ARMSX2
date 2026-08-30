// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Per-game settings precedence over the game database. Feature contributed by
// bmdhacks (PR #593).

#pragma once

#include "Config.h"

#include <bitset>
#include <optional>

class SettingsInterface;

// Opaque-enum declaration rather than including GameDatabase.h, which includes this
// header back for applyGameFixes()'s parameter. A fixed underlying type makes the
// name a complete type, so it can be cast without the enumerator list.
namespace GameDatabaseSchema
{
	enum class GSHWFixId : u32;
}

/// The core (non-graphics) settings the game database can write, one entry per
/// control a player can actually reach. A clamp mode gets one entry rather than one
/// per boolean it expands to, because every settings screen writes and deletes all
/// of a mode's keys together.
enum class CoreGameDBKnob : u8
{
	EERoundMode,
	EEDivRoundMode,
	VU0RoundMode,
	VU1RoundMode,
	EEClampMode,
	VU0ClampMode,
	VU1ClampMode,

	MaxCount
};

/// What a player deliberately chose for one specific game.
///
/// "Deliberately" is decided by presence. Every settings screen represents "use the
/// global setting" by deleting the key — see SettingsWindow::setBoolSettingValue and
/// the Draw*Setting helpers in the fullscreen UI — so a key sitting in a per-game
/// file was put there on purpose. That is what lets the game database be overruled
/// one setting at a time instead of by the all-or-nothing EnableGameFixes and
/// ManualUserHacks switches, which throw away every other fix the game had.
struct PerGameOverrides
{
	/// Bit per GSHWFixId. This is what applyGSHardwareFixes() consults. It is a u64
	/// because there are more graphics fixes than the 32-bit UserHackOverrides mask
	/// below can name, and that mask's width is a persisted INI format.
	u64 gs_fixes = 0;
	/// Bit per GSUserHackOverride, for GSOptions::UserHackOverrides. Narrower than
	/// gs_fixes on purpose: it exists so MaskUserHacks() keeps sparing a claimed hack
	/// and so a mask already written by the iOS frontend still composes.
	u32 gs_hacks = 0;
	u32 core = 0; ///< bit per CoreGameDBKnob
	u32 speedhacks = 0; ///< bit per SpeedHack
	std::bitset<GamefixId_COUNT> gamefixes;

	bool Has(GameDatabaseSchema::GSHWFixId id) const { return (gs_fixes & (u64(1) << static_cast<u64>(id))) != 0; }
	bool Has(CoreGameDBKnob knob) const { return (core & (1u << static_cast<u32>(knob))) != 0; }
	bool Has(SpeedHack hack) const { return (speedhacks & (1u << static_cast<u32>(hack))) != 0; }
	bool Has(GamefixId id) const { return gamefixes.test(static_cast<size_t>(id)); }

	bool Any() const { return gs_fixes != 0 || gs_hacks != 0 || core != 0 || speedhacks != 0 || gamefixes.any(); }
};

/// Reads one per-game settings file — the game layer alone, never the layered stack,
/// because the whole point is to tell that layer's keys apart from the global ones.
PerGameOverrides ComputePerGameOverrides(const SettingsInterface& game_layer);

/// The settings keys behind each knob. Exposed so the tests can prove the tables
/// cover every enumerator: an unmapped knob is a database setting that silently keeps
/// overriding the player, which is the bug this whole mechanism exists to fix.
namespace PerGameOverrideKeys
{
	/// A knob's section, and the keys that together make it up. A clamp mode has
	/// three or four; everything else has one.
	struct CoreKnobKeys
	{
		const char* section;
		const char* keys[4];
		u32 count;
	};

	CoreKnobKeys ForCoreKnob(CoreGameDBKnob knob);

	/// ⚠️ NOT the same string as GamefixOptions::GetGameFixName(), which returns the
	/// game database's YAML name. The database says "FpuMul"; the settings key is
	/// "FpuMulHack". Both tables are needed and neither can stand in for the other.
	const char* ForGamefix(GamefixId id);

	const char* ForSpeedHack(SpeedHack hack);

	/// The `EmuCore/GS` key a player sets to claim this fix, or nullptr when the fix
	/// has no key. Three of them are renderer routine selectors with no setting and no
	/// UI, and three more only raise a recommendation and write nothing, so for those
	/// six the database rightly keeps the last word.
	const char* ForGSHWFix(GameDatabaseSchema::GSHWFixId id);

	/// Whether writing this key into a per-game file claims anything the game database
	/// contends. For frontends that keep a stored claim mask in step as the player
	/// edits, rather than deriving it on load.
	bool ClaimsAGameDBSetting(const char* section, const char* key);
} // namespace PerGameOverrideKeys
