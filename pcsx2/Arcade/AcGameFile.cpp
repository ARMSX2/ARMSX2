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

	// The files are found as VMManager::OpenArcadeGame() finds them: at the absolute path a key gives, else in the
	// folder [data] subdir names beside the .acgame (the game ID by default, none for the .acgame's own).
	info.dir = std::string(Path::GetDirectory(path));
	info.base_dir = info.dir;
	const std::string subdir = ini.GetStringValue("data", "subdir", info.gameid.c_str());
	if (!subdir.empty())
		info.base_dir = Path::Combine(info.base_dir, subdir);

	const auto locate = [&ini, &info](const char* key, const std::string& default_name) {
		std::string name = ini.GetStringValue("data", key, default_name.c_str());
		if (name.empty())
			name = default_name;
		return Path::IsAbsolute(name) ? name : Path::Combine(info.base_dir, name);
	};
	info.elf_path = locate("elf", "boot.elf");
	info.media_path = locate("mediasrc", info.gameid + ".chd");
	info.sram_path = locate("sram", "sram.bin");

	info.dongle = ini.GetStringValue("data", "dongle", "");
	if (info.dongle.empty())
		info.dongle = info.gameid + ".ps2";
	info.card = ini.GetStringValue("data", "card", "");
	return info;
}
