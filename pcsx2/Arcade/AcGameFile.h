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
		std::string media_path; // Where [data] mediasrc is. The file may not be there (yet).
	};

	/// Reads the .acgame at [path]. Nothing when the file cannot be read, is far bigger than an .acgame is, or has
	/// no valid game ID.
	std::optional<AcGameInfo> ReadAcGameInfo(const std::string& path);
} // namespace Arcade
