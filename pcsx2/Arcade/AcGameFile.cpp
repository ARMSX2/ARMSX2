// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Arcade/AcGameFile.h"

#include "Host.h"
#include "INISettingsInterface.h"

#include "Arcade/ArcadeFiles.h"

#include "common/ARCADE.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

// An .acgame is a few lines of INI; anything much bigger is something else with that name.
static constexpr s64 MAX_ACGAME_SIZE = 64 * 1024;

// A drive ("C:"), or anything else a colon makes of a name: an NTFS stream, a drive-relative path.
static bool HasColon(std::string_view name)
{
	return name.find(':') != std::string_view::npos;
}

bool Arcade::IsPlainFileName(std::string_view name)
{
	return !name.empty() && name != "." && name != ".." && !HasColon(name) &&
	       name.find_first_of("/\\") == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

bool Arcade::IsPathInsideFolder(std::string_view path)
{
	if (path.empty() || path.front() == '/' || path.front() == '\\' || HasColon(path) || path.find('\0') != std::string_view::npos)
		return false;

	// Every component, split at either kind of separator.
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find_first_of("/\\", start);
		if (end == std::string_view::npos)
			end = path.size();
		if (path.substr(start, end - start) == "..")
			return false;
		start = end + 1;
	}
	return true;
}

std::string Arcade::CheckWrittenNames(const SettingsInterface& ini, bool check_sram)
{
	for (const char* key : {"dongle", "card"})
	{
		const std::string name = ini.GetStringValue("data", key, "");
		if (!name.empty() && !IsPlainFileName(name))
		{
			return fmt::format(TRANSLATE_FS("Arcade",
				"The .acgame's {0} '{1}' is not a plain file name. It is a file in the memory cards folder, so it cannot name a folder or a path."),
				key, name);
		}
	}

	if (check_sram)
	{
		for (const char* key : {"subdir", "sram"})
		{
			const std::string name = ini.GetStringValue("data", key, "");
			if (!name.empty() && !IsPathInsideFolder(name))
			{
				return fmt::format(TRANSLATE_FS("Arcade",
					"The .acgame's {0} '{1}' leaves the game's folder. It must be a relative name with no '..', as in sram.bin or NM00004."),
					key, name);
			}
		}
	}
	return {};
}

std::optional<Arcade::AcGameInfo> Arcade::ReadAcGameInfo(const std::string& path, std::string* error)
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

	const std::string unsafe = CheckWrittenNames(ini);
	if (!unsafe.empty())
	{
		Console.Warning("(Arcade) '%s' is not used: %s", path.c_str(), unsafe.c_str());
		if (error)
			*error = unsafe;
		return std::nullopt;
	}

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

std::optional<Arcade::AcGameInfo> Arcade::ReadLooseImageInfo(const std::string& path)
{
	AcGameInfo info;
	info.gameid = LooseImageGameId(path);
	if (info.gameid.empty())
		return std::nullopt;

	info.dir = std::string(Path::GetDirectory(path));
	info.base_dir = info.dir;
	info.media_path = path;
	info.dongle = info.gameid + ".ps2";
	return info;
}
