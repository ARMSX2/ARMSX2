// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Reading an .acgame, the INI file that describes a Namco System 246/256 game, for the places that list games
// (the game list, an arcade setup page). Booting one is VMManager::OpenArcadeGame(), which reads every key.

#pragma once

#include <optional>
#include <string>
#include <string_view>

class SettingsInterface;

namespace Arcade
{
	struct AcGameInfo
	{
		std::string gameid; // [game] gameid: NM and five digits.
		std::string name; // [game] name; empty when the file gives none.

		// Where the board's files are, resolved as VMManager::OpenArcadeGame() does: the absolute path a key
		// gives, else under the game folder. The files may not exist (yet).
		std::string dir;
		std::string base_dir; // The game folder: [data] subdir beside the .acgame (the game ID by default, none for the .acgame's own).
		std::string elf_path; // [data] elf, boot.elf by default.
		std::string media_path; // [data] mediasrc, GAMEID.chd by default.
		std::string sram_path; // [data] sram, sram.bin by default.

		// Memory card files, as names in the memory cards folder (OpenArcadeGame() looks nowhere else).
		std::string dongle; // [data] dongle, GAMEID.ps2 by default.
		std::string card; // [data] card, the second card (Soul Calibur II's Conquest card); empty for none.
	};

	/// True when [name] is one file name: no path separator of either kind, no drive letter, not "." or "..". The
	/// dongle and card keys must be, since they are joined to the memory cards folder and written there.
	bool IsPlainFileName(std::string_view name);

	/// True when [path] stays inside the folder it is joined to: relative, with no ".." component (either kind of
	/// separator) and no drive. The sram and subdir keys must be, since the core writes the SRAM there.
	bool IsPathInsideFolder(std::string_view path);

	/// Checks the keys of an .acgame through which the core writes a file: [data] dongle and card must be plain
	/// file names, and [data] sram and subdir (where a relative sram is joined) must stay inside the game's folder.
	/// elf and mediasrc are only read, so they may be anywhere. Returns why the file cannot be used, as a sentence
	/// fit to show, or an empty string when it can. [check_sram] is false when the host gives the SRAM path itself
	/// (VMBootParameters::arcade_sram), so the keys do not decide where it goes.
	std::string CheckWrittenNames(const SettingsInterface& ini, bool check_sram = true);

	/// Reads the .acgame at [path]. Nothing when the file cannot be read, is far bigger than an .acgame is, has
	/// no valid game ID, or names a file the core would write outside its folders (CheckWrittenNames()); [error],
	/// if given, then says which of those.
	std::optional<AcGameInfo> ReadAcGameInfo(const std::string& path, std::string* error = nullptr);

	/// What a loose image (see LooseImageGameId) says of its game, in the same shape: the game ID from its name,
	/// the image as the media. Nothing when [path] is not a loose image.
	std::optional<AcGameInfo> ReadLooseImageInfo(const std::string& path);
} // namespace Arcade
