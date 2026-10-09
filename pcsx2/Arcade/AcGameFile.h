// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Reading an .acgame, the INI file that describes a Namco System 246/256 game, for the places that list games
// (the game list, an arcade setup page). Booting one is VMManager::OpenArcadeGame(), which reads every key.

#pragma once

#include <optional>
#include <string>

namespace Arcade
{
	struct AcGameInfo
	{
		std::string gameid; // [game] gameid: NM and five digits.
		std::string name; // [game] name; empty when the file gives none.

		// Where the board's files are, found the way VMManager::OpenArcadeGame() finds them: the absolute path a
		// key gives, else in the game folder. The files may not be there (yet).
		std::string dir; // The folder the .acgame is in.
		std::string base_dir; // The game folder: [data] subdir beside the .acgame (the game ID by default, none for the .acgame's own).
		std::string elf_path; // [data] elf, boot.elf by default.
		std::string media_path; // [data] mediasrc, GAMEID.chd by default.
		std::string sram_path; // [data] sram, sram.bin by default.

		// Memory card files, as names in the memory cards folder (OpenArcadeGame() looks nowhere else).
		std::string dongle; // [data] dongle, GAMEID.ps2 by default.
		std::string card; // [data] card, the second card (Soul Calibur II's Conquest card); empty for none.
	};

	/// Reads the .acgame at [path]. Nothing when the file cannot be read, is far bigger than an .acgame is, or has
	/// no valid game ID.
	std::optional<AcGameInfo> ReadAcGameInfo(const std::string& path);

	/// What a loose image (see LooseImageGameId) says of its game, in the same shape: the game ID from its name,
	/// the image as the media. Nothing when [path] is not a loose image.
	std::optional<AcGameInfo> ReadLooseImageInfo(const std::string& path);
} // namespace Arcade
