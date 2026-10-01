// TexturePackPaths.h — texture pack archive paths
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

// Where an archive entry lands under textures/<serial>/replacements. Follows Android's
// TextureArchivePath, so a pack installs to the same files on both platforms.
namespace TexturePackPaths
{
	enum class Kind
	{
		Texture,
		Skip,
		Unsafe,
	};

	struct Entry
	{
		Kind kind;
		std::string path;
	};

	inline std::string Lower(std::string_view text)
	{
		std::string out(text);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	inline std::vector<std::string> Split(std::string_view name)
	{
		std::vector<std::string> parts;
		std::string part;
		for (const char c : name)
		{
			if (c != '/' && c != '\\')
			{
				part += c;
				continue;
			}
			if (!part.empty() && part != ".")
				parts.push_back(part);
			part.clear();
		}
		if (!part.empty() && part != ".")
			parts.push_back(part);
		return parts;
	}

	inline bool IsSerialFolder(const std::string& part)
	{
		static const std::regex serial("^[A-Za-z]{4}-?[0-9]{5}$");
		return std::regex_match(part, serial);
	}

	// "slus_212.87", "SLUS 21287" and "slus-21287" all name SLUS-21287.
	inline std::optional<std::string> FindSerial(const std::string& text)
	{
		static const std::regex serial("([A-Za-z]{4})[-_ ]?([0-9]{3})[._ ]?([0-9]{2})");
		std::smatch match;
		if (!std::regex_search(text, match, serial))
			return std::nullopt;
		std::string letters = match[1].str();
		std::transform(letters.begin(), letters.end(), letters.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		return letters + "-" + match[2].str() + match[3].str();
	}

	inline Entry Classify(std::string_view name)
	{
		if (name.empty() || name.front() == '/' || name.front() == '\\' || (name.size() > 1 && name[1] == ':'))
			return {name.empty() ? Kind::Skip : Kind::Unsafe, {}};

		std::vector<std::string> parts = Split(name);
		if (std::find(parts.begin(), parts.end(), "..") != parts.end())
			return {Kind::Unsafe, {}};
		if (parts.empty() || name.back() == '/' || name.back() == '\\')
			return {Kind::Skip, {}};

		const std::string file = Lower(parts.back());
		const bool junk = file.front() == '.' || file == "thumbs.db" ||
			std::any_of(parts.begin(), parts.end(), [](const std::string& p) { return Lower(p) == "__macosx"; });
		const size_t dot = file.rfind('.');
		const std::string ext = dot == std::string::npos ? std::string() : file.substr(dot);
		if (junk || (ext != ".png" && ext != ".dds" && ext != ".astc" && ext != ".ktx"))
			return {Kind::Skip, {}};

		// Packs ship bare, inside <SERIAL>/, or inside <SERIAL>/replacements/, and GitHub zips
		// add a name-<40 hex> root on top.
		size_t start = 0;
		for (size_t i = 0; i + 1 < parts.size(); i++)
		{
			if (Lower(parts[i]) == "replacements")
				start = i + 1;
		}
		if (start == 0)
		{
			for (size_t i = 0; i + 1 < parts.size(); i++)
			{
				if (IsSerialFolder(parts[i]))
					start = i + 1;
			}
		}
		static const std::regex github_root("^.+-[0-9a-f]{40}$");
		if (start == 0 && parts.size() > 1 && std::regex_match(parts[0], github_root))
			start = 1;

		std::string path;
		for (size_t i = start; i < parts.size(); i++)
			path += (path.empty() ? "" : "/") + parts[i];
		return {Kind::Texture, path};
	}

	// A serial folder anywhere above the file names the game the pack is for.
	inline std::optional<std::string> SerialFolderIn(std::string_view name)
	{
		std::vector<std::string> parts = Split(name);
		for (size_t i = 0; i + 1 < parts.size(); i++)
		{
			if (IsSerialFolder(parts[i]))
				return FindSerial(parts[i]);
		}
		return std::nullopt;
	}
} // namespace TexturePackPaths
