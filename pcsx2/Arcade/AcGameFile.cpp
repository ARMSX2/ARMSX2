// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Arcade/AcGameFile.h"

#include "INISettingsInterface.h"

#include "common/ARCADE.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

// An .acgame is a few lines of INI; anything much bigger is something else with that name.
static constexpr s64 MAX_ACGAME_SIZE = 64 * 1024;

std::optional<Arcade::AcGameInfo> Arcade::ReadAcGameInfo(const std::string& path)
{
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(path.c_str(), &sd) || sd.Size > MAX_ACGAME_SIZE)
		return std::nullopt;

	INISettingsInterface ini(path);
	if (!ini.Load())
		return std::nullopt;

	AcGameInfo info;
	info.gameid = ini.GetStringValue("game", "gameid", "");
	if (!IsGameId(info.gameid))
	{
		Console.Warning("(Arcade) '%s' has no arcade game ID (NM and five digits).", path.c_str());
		return std::nullopt;
	}
	info.name = ini.GetStringValue("game", "name", "");

	// The media image is found as VMManager::OpenArcadeGame() finds it: at the absolute path [data] mediasrc gives,
	// else in the folder [data] subdir names beside the .acgame (the game ID by default, none for the .acgame's own).
	std::string media = ini.GetStringValue("data", "mediasrc", "");
	if (media.empty())
		media = info.gameid + ".chd";
	if (!Path::IsAbsolute(media))
	{
		std::string basedir(Path::GetDirectory(path));
		const std::string subdir = ini.GetStringValue("data", "subdir", info.gameid.c_str());
		if (!subdir.empty())
			basedir = Path::Combine(basedir, subdir);
		media = Path::Combine(basedir, media);
	}
	info.media_path = std::move(media);
	return info;
}
