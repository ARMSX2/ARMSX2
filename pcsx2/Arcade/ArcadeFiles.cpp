// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Arcade/ArcadeFiles.h"

#include "Arcade/AcGameFile.h"

#include "common/Path.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <map>

namespace
{
	bool IsAsciiAlnum(char c)
	{
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	bool IsAsciiDigit(char c)
	{
		return c >= '0' && c <= '9';
	}

	// "Memory Card for SoulCaliburII": how a Conquest card starts (its first page, written by the game).
	constexpr char CONQUEST_HEADER[] = "Memory Card for SoulCaliburII";

	// Paths compared without regard to case or separator style.
	std::string Key(std::string_view path)
	{
		std::string key = StringUtil::toLower(path);
		std::replace(key.begin(), key.end(), '\\', '/');
		return key;
	}
} // namespace

std::string Arcade::FindGameIdInName(std::string_view name)
{
	// (?i)(?<![a-z0-9])NM\d{5}(?!\d), as the Android app finds it.
	constexpr size_t LENGTH = 7;
	for (size_t i = 0; i + LENGTH <= name.size(); i++)
	{
		if ((name[i] != 'N' && name[i] != 'n') || (name[i + 1] != 'M' && name[i + 1] != 'm'))
			continue;
		if (i > 0 && IsAsciiAlnum(name[i - 1]))
			continue;
		bool digits = true;
		for (size_t d = 2; d < LENGTH && digits; d++)
			digits = IsAsciiDigit(name[i + d]);
		if (!digits || (i + LENGTH < name.size() && IsAsciiDigit(name[i + LENGTH])))
			continue;
		return StringUtil::toUpper(name.substr(i, LENGTH));
	}
	return {};
}

Arcade::FileKind Arcade::ClassifyFile(std::string_view name, u64 size)
{
	const std::string lower = StringUtil::toLower(name);
	const bool packed = lower.ends_with(".gz");
	const std::string_view base = std::string_view(lower).substr(0, lower.size() - (packed ? 3 : 0));
	const size_t dot = base.rfind('.');
	const std::string_view extension = (dot == std::string_view::npos) ? std::string_view() : base.substr(dot + 1);

	if (extension == "chd" || extension == "iso")
		return packed ? FileKind::PackedImage : FileKind::Image;
	if (extension == "ps2" || extension == "conquestcard")
		return FileKind::Card;
	if (extension == "bin" || extension == "img")
	{
		if (size > MAX_CARD_BYTES)
			return packed ? FileKind::PackedImage : FileKind::Image;
		// A packed dongle is far smaller than its card; its size is checked once unpacked.
		if (packed)
			return FileKind::Card;
		return size >= MIN_CARD_BYTES ? FileKind::Card : FileKind::Other;
	}
	if (extension == "elf")
		return FileKind::Other;
	return FileKind::Unknown;
}

std::string Arcade::LooseImageGameId(std::string_view path, u64 size)
{
	const std::string_view name = Path::GetFileName(path);
	std::string id = FindGameIdInName(name);
	if (id.empty())
		id = FindGameIdInName(Path::GetFileName(Path::GetDirectory(path)));
	if (id.empty() || ClassifyFile(name, size) != FileKind::Image)
		return {};
	return id;
}

std::string Arcade::LooseImageGameId(const std::string& path)
{
	// Most files of a scan name no game: tell those by name alone, before asking the disk for a size.
	if (FindGameIdInName(Path::GetFileName(path)).empty() && FindGameIdInName(Path::GetFileName(Path::GetDirectory(path))).empty())
		return {};
	const s64 size = FileSystem::GetPathFileSize(path.c_str());
	return (size < 0) ? std::string() : LooseImageGameId(path, static_cast<u64>(size));
}

std::string Arcade::DongleName(std::string_view id, u64 bytes)
{
	return std::string(id) + ((bytes % (1ull << 20)) == 0 ? ".bin" : ".ps2");
}

size_t Arcade::ConquestHeaderBytes()
{
	return sizeof(CONQUEST_HEADER) - 1;
}

bool Arcade::IsConquestCardHeader(std::span<const u8> head)
{
	constexpr size_t length = sizeof(CONQUEST_HEADER) - 1;
	return head.size() >= length && std::equal(head.begin(), head.begin() + length, CONQUEST_HEADER);
}

Arcade::SetClaims::SetClaims(const FileSystem::FindResultsArray& files)
{
	struct Item
	{
		std::string name;
		u64 size;
	};
	std::map<std::string, std::vector<Item>> by_folder;
	for (const FILESYSTEM_FIND_DATA& file : files)
	{
		by_folder[std::string(Path::GetDirectory(file.FileName))].push_back(
			{std::string(Path::GetFileName(file.FileName)), static_cast<u64>(std::max<s64>(file.Size, 0))});
	}

	// The .acgame files come first, from everywhere in the scan, so the games that have one are known
	// before the games kept as their own files are looked at.
	std::unordered_set<std::string> acgame_games;
	for (const auto& [folder, items] : by_folder)
	{
		for (const Item& item : items)
		{
			if (!StringUtil::EndsWithNoCase(item.name, ".acgame"))
				continue;
			const std::optional<AcGameInfo> game = ReadAcGameInfo(Path::Combine(folder, item.name));
			if (!game.has_value())
				continue;

			acgame_games.insert(game->gameid);
			if (game->base_dir != game->dir)
				m_folders.push_back(Key(game->base_dir) + "/");
			for (const std::string& path : {game->elf_path, game->media_path, game->sram_path})
				m_files.insert(Key(path));
			// What the board writes beside its SRAM when the game runs from a hard drive image.
			m_files.insert(Key(Path::Combine(Path::GetDirectory(game->sram_path), "hdd-writes.bin")));
			// The dongle and the card can be beside the .acgame whatever its game folder is (ArcadeSetup looks there).
			for (const std::string& name : {game->dongle, game->card})
			{
				if (!name.empty())
					m_files.insert(Key(Path::Combine(game->dir, Path::GetFileName(name))));
			}
		}
	}

	// Games kept as their own files: a file named after a game, or in a folder named after one. Its image is a
	// game of its own unless an .acgame (anywhere in the scan) has the game, or has already claimed the file.
	for (const auto& [folder, items] : by_folder)
	{
		const std::string folder_id = FindGameIdInName(Path::GetFileName(folder));
		for (const Item& item : items)
		{
			if (FindGameIdInName(item.name).empty() && folder_id.empty())
				continue;
			if (ClassifyFile(item.name, item.size) == FileKind::Unknown)
				continue;

			const std::string path = Path::Combine(folder, item.name);
			const std::string key = Key(path);
			const std::string loose_id = LooseImageGameId(path, item.size);
			// (Only what the .acgame files claim is in m_files yet, as each file is looked at once.)
			if (!loose_id.empty() && !acgame_games.contains(loose_id) && !Contains(key))
				m_loose.insert(key);
			m_files.insert(key);
		}
	}
}

bool Arcade::SetClaims::Contains(const std::string& key) const
{
	if (m_files.contains(key))
		return true;
	return std::any_of(m_folders.begin(), m_folders.end(), [&key](const std::string& folder) { return key.starts_with(folder); });
}

bool Arcade::SetClaims::IsLooseImage(const std::string& path) const
{
	return !m_loose.empty() && m_loose.contains(Key(path));
}

bool Arcade::SetClaims::Owns(const std::string& path) const
{
	if (Empty())
		return false;
	return Contains(Key(path));
}
