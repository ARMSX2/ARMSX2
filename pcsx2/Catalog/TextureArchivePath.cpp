// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/TextureArchivePath.h"

#include "common/Pcsx2Types.h"

#include <algorithm>
#include <array>
#include <vector>

namespace Catalog::TextureArchivePath
{
	namespace
	{
		bool StartsWith(std::string_view s, std::string_view prefix)
		{
			return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
		}

		bool EndsWith(std::string_view s, std::string_view suffix)
		{
			return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		char AsciiLower(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}

		std::string AsciiLowerCopy(std::string_view s)
		{
			std::string out(s);
			for (char& c : out)
				c = AsciiLower(c);
			return out;
		}

		bool EqualsIgnoreCase(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && AsciiLowerCopy(a) == AsciiLowerCopy(b);
		}

		std::vector<std::string_view> Split(std::string_view s, char sep)
		{
			std::vector<std::string_view> parts;
			size_t start = 0;
			while (true)
			{
				const size_t pos = s.find(sep, start);
				if (pos == std::string_view::npos)
				{
					parts.push_back(s.substr(start));
					return parts;
				}
				parts.push_back(s.substr(start, pos - start));
				start = pos + 1;
			}
		}

		std::string Join(const std::vector<std::string_view>& parts, size_t first)
		{
			std::string out;
			for (size_t i = first; i < parts.size(); i++)
			{
				if (i != first)
					out += '/';
				out += parts[i];
			}
			return out;
		}

		// Lowercase + canonical decomposition of U+0080..U+017F, generated with Python's
		// unicodedata: NFD(chr(cp).lower()) for every code point it changes. A second value of 0
		// means the mapping is a single code point (e.g. U+00C6 -> U+00E6).
		struct FoldEntry
		{
			char16_t cp;
			char16_t first;
			char16_t second;
		};
		constexpr FoldEntry kFold[] = {
			{0x00C0, 0x0061, 0x0300}, {0x00C1, 0x0061, 0x0301}, {0x00C2, 0x0061, 0x0302}, {0x00C3, 0x0061, 0x0303},
			{0x00C4, 0x0061, 0x0308}, {0x00C5, 0x0061, 0x030A}, {0x00C6, 0x00E6, 0x0000}, {0x00C7, 0x0063, 0x0327},
			{0x00C8, 0x0065, 0x0300}, {0x00C9, 0x0065, 0x0301}, {0x00CA, 0x0065, 0x0302}, {0x00CB, 0x0065, 0x0308},
			{0x00CC, 0x0069, 0x0300}, {0x00CD, 0x0069, 0x0301}, {0x00CE, 0x0069, 0x0302}, {0x00CF, 0x0069, 0x0308},
			{0x00D0, 0x00F0, 0x0000}, {0x00D1, 0x006E, 0x0303}, {0x00D2, 0x006F, 0x0300}, {0x00D3, 0x006F, 0x0301},
			{0x00D4, 0x006F, 0x0302}, {0x00D5, 0x006F, 0x0303}, {0x00D6, 0x006F, 0x0308}, {0x00D8, 0x00F8, 0x0000},
			{0x00D9, 0x0075, 0x0300}, {0x00DA, 0x0075, 0x0301}, {0x00DB, 0x0075, 0x0302}, {0x00DC, 0x0075, 0x0308},
			{0x00DD, 0x0079, 0x0301}, {0x00DE, 0x00FE, 0x0000}, {0x00E0, 0x0061, 0x0300}, {0x00E1, 0x0061, 0x0301},
			{0x00E2, 0x0061, 0x0302}, {0x00E3, 0x0061, 0x0303}, {0x00E4, 0x0061, 0x0308}, {0x00E5, 0x0061, 0x030A},
			{0x00E7, 0x0063, 0x0327}, {0x00E8, 0x0065, 0x0300}, {0x00E9, 0x0065, 0x0301}, {0x00EA, 0x0065, 0x0302},
			{0x00EB, 0x0065, 0x0308}, {0x00EC, 0x0069, 0x0300}, {0x00ED, 0x0069, 0x0301}, {0x00EE, 0x0069, 0x0302},
			{0x00EF, 0x0069, 0x0308}, {0x00F1, 0x006E, 0x0303}, {0x00F2, 0x006F, 0x0300}, {0x00F3, 0x006F, 0x0301},
			{0x00F4, 0x006F, 0x0302}, {0x00F5, 0x006F, 0x0303}, {0x00F6, 0x006F, 0x0308}, {0x00F9, 0x0075, 0x0300},
			{0x00FA, 0x0075, 0x0301}, {0x00FB, 0x0075, 0x0302}, {0x00FC, 0x0075, 0x0308}, {0x00FD, 0x0079, 0x0301},
			{0x00FF, 0x0079, 0x0308}, {0x0100, 0x0061, 0x0304}, {0x0101, 0x0061, 0x0304}, {0x0102, 0x0061, 0x0306},
			{0x0103, 0x0061, 0x0306}, {0x0104, 0x0061, 0x0328}, {0x0105, 0x0061, 0x0328}, {0x0106, 0x0063, 0x0301},
			{0x0107, 0x0063, 0x0301}, {0x0108, 0x0063, 0x0302}, {0x0109, 0x0063, 0x0302}, {0x010A, 0x0063, 0x0307},
			{0x010B, 0x0063, 0x0307}, {0x010C, 0x0063, 0x030C}, {0x010D, 0x0063, 0x030C}, {0x010E, 0x0064, 0x030C},
			{0x010F, 0x0064, 0x030C}, {0x0110, 0x0111, 0x0000}, {0x0112, 0x0065, 0x0304}, {0x0113, 0x0065, 0x0304},
			{0x0114, 0x0065, 0x0306}, {0x0115, 0x0065, 0x0306}, {0x0116, 0x0065, 0x0307}, {0x0117, 0x0065, 0x0307},
			{0x0118, 0x0065, 0x0328}, {0x0119, 0x0065, 0x0328}, {0x011A, 0x0065, 0x030C}, {0x011B, 0x0065, 0x030C},
			{0x011C, 0x0067, 0x0302}, {0x011D, 0x0067, 0x0302}, {0x011E, 0x0067, 0x0306}, {0x011F, 0x0067, 0x0306},
			{0x0120, 0x0067, 0x0307}, {0x0121, 0x0067, 0x0307}, {0x0122, 0x0067, 0x0327}, {0x0123, 0x0067, 0x0327},
			{0x0124, 0x0068, 0x0302}, {0x0125, 0x0068, 0x0302}, {0x0126, 0x0127, 0x0000}, {0x0128, 0x0069, 0x0303},
			{0x0129, 0x0069, 0x0303}, {0x012A, 0x0069, 0x0304}, {0x012B, 0x0069, 0x0304}, {0x012C, 0x0069, 0x0306},
			{0x012D, 0x0069, 0x0306}, {0x012E, 0x0069, 0x0328}, {0x012F, 0x0069, 0x0328}, {0x0130, 0x0069, 0x0307},
			{0x0132, 0x0133, 0x0000}, {0x0134, 0x006A, 0x0302}, {0x0135, 0x006A, 0x0302}, {0x0136, 0x006B, 0x0327},
			{0x0137, 0x006B, 0x0327}, {0x0139, 0x006C, 0x0301}, {0x013A, 0x006C, 0x0301}, {0x013B, 0x006C, 0x0327},
			{0x013C, 0x006C, 0x0327}, {0x013D, 0x006C, 0x030C}, {0x013E, 0x006C, 0x030C}, {0x013F, 0x0140, 0x0000},
			{0x0141, 0x0142, 0x0000}, {0x0143, 0x006E, 0x0301}, {0x0144, 0x006E, 0x0301}, {0x0145, 0x006E, 0x0327},
			{0x0146, 0x006E, 0x0327}, {0x0147, 0x006E, 0x030C}, {0x0148, 0x006E, 0x030C}, {0x014A, 0x014B, 0x0000},
			{0x014C, 0x006F, 0x0304}, {0x014D, 0x006F, 0x0304}, {0x014E, 0x006F, 0x0306}, {0x014F, 0x006F, 0x0306},
			{0x0150, 0x006F, 0x030B}, {0x0151, 0x006F, 0x030B}, {0x0152, 0x0153, 0x0000}, {0x0154, 0x0072, 0x0301},
			{0x0155, 0x0072, 0x0301}, {0x0156, 0x0072, 0x0327}, {0x0157, 0x0072, 0x0327}, {0x0158, 0x0072, 0x030C},
			{0x0159, 0x0072, 0x030C}, {0x015A, 0x0073, 0x0301}, {0x015B, 0x0073, 0x0301}, {0x015C, 0x0073, 0x0302},
			{0x015D, 0x0073, 0x0302}, {0x015E, 0x0073, 0x0327}, {0x015F, 0x0073, 0x0327}, {0x0160, 0x0073, 0x030C},
			{0x0161, 0x0073, 0x030C}, {0x0162, 0x0074, 0x0327}, {0x0163, 0x0074, 0x0327}, {0x0164, 0x0074, 0x030C},
			{0x0165, 0x0074, 0x030C}, {0x0166, 0x0167, 0x0000}, {0x0168, 0x0075, 0x0303}, {0x0169, 0x0075, 0x0303},
			{0x016A, 0x0075, 0x0304}, {0x016B, 0x0075, 0x0304}, {0x016C, 0x0075, 0x0306}, {0x016D, 0x0075, 0x0306},
			{0x016E, 0x0075, 0x030A}, {0x016F, 0x0075, 0x030A}, {0x0170, 0x0075, 0x030B}, {0x0171, 0x0075, 0x030B},
			{0x0172, 0x0075, 0x0328}, {0x0173, 0x0075, 0x0328}, {0x0174, 0x0077, 0x0302}, {0x0175, 0x0077, 0x0302},
			{0x0176, 0x0079, 0x0302}, {0x0177, 0x0079, 0x0302}, {0x0178, 0x0079, 0x0308}, {0x0179, 0x007A, 0x0301},
			{0x017A, 0x007A, 0x0301}, {0x017B, 0x007A, 0x0307}, {0x017C, 0x007A, 0x0307}, {0x017D, 0x007A, 0x030C},
			{0x017E, 0x007A, 0x030C},
};

		void AppendUtf8(std::string& out, char32_t cp)
		{
			if (cp < 0x80)
			{
				out += static_cast<char>(cp);
			}
			else if (cp < 0x800)
			{
				out += static_cast<char>(0xC0 | (cp >> 6));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xE0 | (cp >> 12));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
		}

		bool IsSerialLike(std::string_view s)
		{
			size_t digits_at = 4;
			if (s.size() == 10 && s[4] == '-')
				digits_at = 5;
			else if (s.size() != 9)
				return false;

			for (size_t i = 0; i < 4; i++)
			{
				const char c = s[i];
				if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
					return false;
			}
			for (size_t i = digits_at; i < s.size(); i++)
			{
				if (s[i] < '0' || s[i] > '9')
					return false;
			}
			return true;
		}

		// "<anything>-<40 lowercase hex>", the root folder of a GitHub source archive.
		bool IsGithubRoot(std::string_view s)
		{
			if (s.size() < 42 || s[s.size() - 41] != '-')
				return false;
			for (size_t i = s.size() - 40; i < s.size(); i++)
			{
				const char c = s[i];
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
					return false;
			}
			return true;
		}
	} // namespace

	std::optional<std::string> ValidateTarPath(std::string_view raw)
	{
		if (raw.size() > MAX_PATH_BYTES)
			return std::nullopt;
		if (raw.find('\0') != std::string_view::npos)
			return std::nullopt;

		std::string_view path = raw;
		if (StartsWith(path, "./"))
			path.remove_prefix(2);
		if (path.empty())
			return std::string(); // the producer's root "./" entry

		if (path.front() == '/')
			return std::nullopt;

		// A trailing slash is how tar spells a directory. "a//b" still fails on the empty
		// component below.
		while (!path.empty() && path.back() == '/')
			path.remove_suffix(1);
		if (path.empty())
			return std::nullopt;

		const std::vector<std::string_view> parts = Split(path, '/');
		if (parts.size() > MAX_COMPONENTS)
			return std::nullopt;
		for (const std::string_view part : parts)
		{
			if (part.empty() || part == "." || part == "..")
				return std::nullopt;
			if (part.size() > MAX_COMPONENT_BYTES)
				return std::nullopt;
		}
		return std::string(path);
	}

	bool IsSafeRelativePath(std::string_view path)
	{
		if (path.empty() || path.front() == '/' || path.find('\0') != std::string_view::npos)
			return false;
		for (const std::string_view part : Split(path, '/'))
		{
			if (part.empty() || part == "." || part == "..")
				return false;
#ifdef _WIN32
			if (part.find_first_of(":\\") != std::string_view::npos)
				return false;
#endif
		}
		return true;
	}

	std::string CollisionKey(std::string_view path)
	{
		std::string out;
		out.reserve(path.size());
		for (size_t i = 0; i < path.size();)
		{
			const u8 lead = static_cast<u8>(path[i]);
			if (lead < 0x80)
			{
				out += AsciiLower(static_cast<char>(lead));
				i++;
				continue;
			}

			// Only 2-byte sequences (U+0080..U+07FF) can hit the table; everything else is copied.
			if ((lead & 0xE0) == 0xC0 && i + 1 < path.size() && (static_cast<u8>(path[i + 1]) & 0xC0) == 0x80)
			{
				const char32_t cp = (static_cast<char32_t>(lead & 0x1F) << 6) | (static_cast<u8>(path[i + 1]) & 0x3F);
				const auto it = std::lower_bound(std::begin(kFold), std::end(kFold), cp,
					[](const FoldEntry& e, char32_t value) { return e.cp < value; });
				if (it != std::end(kFold) && it->cp == cp)
				{
					AppendUtf8(out, it->first);
					if (it->second != 0)
						AppendUtf8(out, it->second);
				}
				else
				{
					out += path[i];
					out += path[i + 1];
				}
				i += 2;
				continue;
			}

			out += path[i];
			i++;
		}
		return out;
	}

	bool IsTextureFile(std::string_view name)
	{
		const std::string lower = AsciiLowerCopy(name);
		return EndsWith(lower, ".png") || EndsWith(lower, ".dds") || EndsWith(lower, ".astc") || EndsWith(lower, ".ktx");
	}

	bool IsJunkEntry(std::string_view name)
	{
		const std::string lower = AsciiLowerCopy(name);
		return StartsWith(lower, "__macosx/") || lower.find("/__macosx/") != std::string::npos ||
		       EndsWith(lower, "/.ds_store") || lower == ".ds_store" || EndsWith(lower, "/thumbs.db");
	}

	std::optional<std::string> ReplacementRelativePath(std::string_view name)
	{
		std::string norm(name);
		std::replace(norm.begin(), norm.end(), '\\', '/');

		std::vector<std::string_view> parts;
		for (const std::string_view part : Split(norm, '/'))
		{
			if (!part.empty() && part != ".")
				parts.push_back(part);
		}
		if (parts.empty())
			return std::nullopt;

		for (size_t i = parts.size(); i-- > 0;)
		{
			if (i + 1 < parts.size() && EqualsIgnoreCase(parts[i], "replacements"))
				return Join(parts, i + 1);
		}
		for (size_t i = parts.size(); i-- > 0;)
		{
			if (i + 1 < parts.size() && IsSerialLike(parts[i]))
				return Join(parts, i + 1);
		}
		if (parts.size() > 1 && IsGithubRoot(parts[0]))
			return Join(parts, 1);
		return Join(parts, 0);
	}
} // namespace Catalog::TextureArchivePath
