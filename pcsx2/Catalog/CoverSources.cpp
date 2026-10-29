// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/CoverSources.h"

#include "fmt/format.h"

#include <algorithm>
#include <charconv>

namespace Catalog
{
	static constexpr std::string_view ARCADE_COVER_ROOT = "https://arcade.ps2ktxpak.net/arcade-covers";
	static constexpr std::string_view GITHUB_RAW_ROOT = "https://raw.githubusercontent.com/xlenore";

	static std::string ToUpperAscii(std::string_view text)
	{
		std::string out(text);
		for (char& c : out)
		{
			if (c >= 'a' && c <= 'z')
				c = static_cast<char>(c - 'a' + 'A');
		}
		return out;
	}

	static std::string_view TrimAscii(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
			text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
			text.remove_suffix(1);
		return text;
	}

	bool IsArcadeGameId(std::string_view serial)
	{
		if (serial.size() != 7 || serial[0] != 'N' || serial[1] != 'M')
			return false;
		return std::all_of(serial.begin() + 2, serial.end(), [](char c) { return c >= '0' && c <= '9'; });
	}

	bool IsUsableCoverSerial(std::string_view serial)
	{
		if (serial.empty() || serial.size() > 24)
			return false;
		return std::all_of(serial.begin(), serial.end(), [](char c) {
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
		});
	}

	std::string CoverUrl(CoverPlatform platform, std::string_view serial, CoverStyle style)
	{
		if (!IsUsableCoverSerial(serial))
			return {};

		if (platform == CoverPlatform::Arcade || IsArcadeGameId(serial))
		{
			if (!IsArcadeGameId(serial))
				return {};
			return (style == CoverStyle::Case3D) ? fmt::format("{}/3d/{}.png", ARCADE_COVER_ROOT, serial) :
			                                       fmt::format("{}/{}.jpg", ARCADE_COVER_ROOT, serial);
		}

		const std::string_view repo = (platform == CoverPlatform::PS1) ? "psx-covers" : "ps2-covers";
		return (style == CoverStyle::Case3D) ? fmt::format("{}/{}/main/covers/3d/{}.png", GITHUB_RAW_ROOT, repo, serial) :
		                                       fmt::format("{}/{}/main/covers/default/{}.jpg", GITHUB_RAW_ROOT, repo, serial);
	}

	namespace
	{
		const std::vector<std::string_view>& RegionPrefixes(CoverRegion region)
		{
			static const std::vector<std::string_view> none;
			static const std::vector<std::string_view> usa = {"SLUS", "SCUS"};
			static const std::vector<std::string_view> europe = {"SLES", "SCES", "SLED", "SCED"};
			static const std::vector<std::string_view> japan = {"SLPS", "SLPM", "SCPS", "SLKA", "SCKA", "SCAJ", "SLAJ"};
			switch (region)
			{
				case CoverRegion::USA:
					return usa;
				case CoverRegion::Europe:
					return europe;
				case CoverRegion::JapanAsia:
					return japan;
				case CoverRegion::Disc:
				default:
					return none;
			}
		}

		// The value of a `name:` / `name-en:` line: the text between the double quotes, or, if
		// unquoted, the text before a trailing "# comment". A '#' inside the quotes is part of
		// the title ("Guilty Gear XX #Reload"), so only a '#' after the closing quote starts a
		// comment.
		std::string UnquoteYamlScalar(std::string_view value)
		{
			value = TrimAscii(value);
			if (!value.empty() && value.front() == '"')
			{
				std::string out;
				for (size_t i = 1; i < value.size(); i++)
				{
					if (value[i] == '\\' && i + 1 < value.size())
					{
						out.push_back(value[i + 1]);
						i++;
					}
					else if (value[i] == '"')
					{
						return out;
					}
					else
					{
						out.push_back(value[i]);
					}
				}
			}

			const size_t hash = value.find('#');
			if (hash != std::string_view::npos)
				value = value.substr(0, hash);
			value = TrimAscii(value);
			while (!value.empty() && value.front() == '"')
				value.remove_prefix(1);
			while (!value.empty() && value.back() == '"')
				value.remove_suffix(1);
			return std::string(value);
		}
	} // namespace

	std::string CoverRegionIndex::NormalizeTitle(std::string_view title)
	{
		// Lower case ASCII; every other byte (including all of UTF-8) ends up a separator below.
		std::string lowered(title);
		for (char& c : lowered)
		{
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		}

		// Drop "[...]" and "(...)". The database appends qualifiers such as "[Asia Version]" or
		// "(Disc 1)" that differ between the regional entries of one game, and keeping them would
		// split the group. As in the Android regex, either kind of closer ends either opener.
		std::string without_brackets;
		without_brackets.reserve(lowered.size());
		for (size_t i = 0; i < lowered.size();)
		{
			if (lowered[i] == '[' || lowered[i] == '(')
			{
				const size_t close = lowered.find_first_of("])", i + 1);
				if (close != std::string::npos)
				{
					without_brackets.push_back(' ');
					i = close + 1;
					continue;
				}
			}
			without_brackets.push_back(lowered[i]);
			i++;
		}

		std::string key;
		bool pending_separator = false;
		for (const char c : without_brackets)
		{
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			{
				if (pending_separator && !key.empty())
					key.push_back(' ');
				pending_separator = false;
				key.push_back(c);
			}
			else
			{
				pending_separator = true;
			}
		}
		return key;
	}

	bool CoverRegionIndex::SerialIsInRegion(std::string_view serial, CoverRegion region)
	{
		const std::string_view prefix = serial.substr(0, serial.find('-'));
		for (const std::string_view candidate : RegionPrefixes(region))
		{
			if (prefix.size() == candidate.size() &&
				std::equal(prefix.begin(), prefix.end(), candidate.begin(), [](char a, char b) {
					return ((a >= 'a' && a <= 'z') ? a - 'a' + 'A' : a) == b;
				}))
			{
				return true;
			}
		}
		return false;
	}

	CoverRegionIndex CoverRegionIndex::FromGameIndexYaml(std::string_view yaml)
	{
		CoverRegionIndex index;
		std::unordered_map<std::string, u32> group_of_key;

		std::string serial;
		std::string name;
		std::string name_en;

		const auto flush = [&]() {
			if (!serial.empty())
			{
				// English title first, so a Japanese release files under the same key as the
				// Western one.
				const std::string& title = !name_en.empty() ? name_en : name;
				const std::string key = NormalizeTitle(title);
				if (!key.empty())
				{
					auto [it, inserted] = group_of_key.try_emplace(key, static_cast<u32>(index.m_groups.size()));
					if (inserted)
						index.m_groups.emplace_back();
					index.m_groups[it->second].push_back(serial);
					index.m_group_of[serial] = it->second;
				}
			}
			serial.clear();
			name.clear();
			name_en.clear();
		};

		size_t pos = 0;
		while (pos < yaml.size())
		{
			size_t end = yaml.find('\n', pos);
			if (end == std::string_view::npos)
				end = yaml.size();
			std::string_view line = yaml.substr(pos, end - pos);
			pos = end + 1;

			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			if (line.empty() || line.front() == '#')
				continue;

			if (line.front() != ' ' && line.front() != '\t')
			{
				// Column 0 is a new serial key ("SLUS-21007:") and ends the previous entry.
				flush();
				const std::string_view key = TrimAscii(line.substr(0, line.find(':')));
				if (key.size() >= 8 && key.size() <= 12 && key[4] == '-')
					serial = ToUpperAscii(key);
			}
			else if (!serial.empty())
			{
				const std::string_view field = TrimAscii(line);
				// "name-en:" and "name:" are distinct prefixes; "name-sort:" matches neither.
				if (field.substr(0, 8) == "name-en:")
					name_en = UnquoteYamlScalar(field.substr(8));
				else if (field.substr(0, 5) == "name:")
					name = UnquoteYamlScalar(field.substr(5));
			}
		}
		flush();
		return index;
	}

	std::optional<std::string> CoverRegionIndex::CoverSerialFor(std::string_view serial, CoverRegion wanted) const
	{
		if (wanted == CoverRegion::Disc || serial.empty())
			return std::nullopt;

		const auto found = m_group_of.find(ToUpperAscii(serial));
		if (found == m_group_of.end())
			return std::nullopt;

		if (SerialIsInRegion(serial, wanted))
			return std::nullopt;

		for (const std::string& candidate : m_groups[found->second])
		{
			if (SerialIsInRegion(candidate, wanted))
				return candidate;
		}
		return std::nullopt;
	}

	std::optional<CoverRegion> CoverPreferences::PinFor(std::string_view serial) const
	{
		if (serial.empty())
			return std::nullopt;
		const auto it = pins.find(ToUpperAscii(serial));
		if (it == pins.end())
			return std::nullopt;
		return it->second;
	}

	CoverRegion CoverPreferences::EffectiveRegion(std::string_view serial) const
	{
		return PinFor(serial).value_or(region);
	}

	void CoverPreferences::SetPin(std::string_view serial, std::optional<CoverRegion> pin)
	{
		if (serial.empty())
			return;
		if (pin.has_value())
			pins[ToUpperAscii(serial)] = *pin;
		else
			pins.erase(ToUpperAscii(serial));
	}

	bool CoverPreferences::NeedsRegionIndex() const
	{
		if (region != CoverRegion::Disc)
			return true;
		return std::any_of(pins.begin(), pins.end(), [](const auto& pin) { return pin.second != CoverRegion::Disc; });
	}

	std::map<std::string, CoverRegion> CoverPreferences::ParsePins(std::string_view text)
	{
		std::map<std::string, CoverRegion> pins;
		size_t pos = 0;
		while (pos <= text.size())
		{
			size_t end = text.find(',', pos);
			if (end == std::string_view::npos)
				end = text.size();
			const std::string_view entry = text.substr(pos, end - pos);
			pos = end + 1;

			const size_t equals = entry.find('=');
			if (equals == std::string_view::npos || equals == 0)
				continue;

			const std::string_view key = TrimAscii(entry.substr(0, equals));
			const std::string_view value = TrimAscii(entry.substr(equals + 1));
			if (key.empty())
				continue;

			int number = 0;
			const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
			if (result.ec != std::errc() || result.ptr != value.data() + value.size())
				continue;

			pins[ToUpperAscii(key)] = static_cast<CoverRegion>(std::clamp(number, 0, COVER_REGION_COUNT - 1));
		}
		return pins;
	}

	std::string CoverPreferences::FormatPins() const
	{
		std::string out;
		for (const auto& [serial, pin] : pins)
		{
			if (!out.empty())
				out.push_back(',');
			out += fmt::format("{}={}", serial, static_cast<int>(pin));
		}
		return out;
	}

	CoverUrls ResolveCoverUrls(CoverPlatform platform, std::string_view serial, const CoverPreferences& prefs, const CoverRegionIndex* index)
	{
		CoverUrls urls;
		const std::string disc_url = CoverUrl(platform, serial, prefs.style);
		if (disc_url.empty())
			return urls;

		urls.primary = disc_url;
		urls.cover_serial = std::string(serial);

		// Arcade games are not in the game database, so nothing could swap; skip the lookup.
		if (index && platform != CoverPlatform::Arcade && !IsArcadeGameId(serial))
		{
			const std::optional<std::string> swapped = index->CoverSerialFor(serial, prefs.EffectiveRegion(serial));
			if (swapped.has_value())
			{
				const std::string regional_url = CoverUrl(platform, *swapped, prefs.style);
				if (!regional_url.empty() && regional_url != disc_url)
				{
					urls.primary = regional_url;
					urls.fallback = disc_url;
					urls.cover_serial = *swapped;
				}
			}
		}
		return urls;
	}
} // namespace Catalog
