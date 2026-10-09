// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/PnachText.h"

#include <cstddef>

namespace Catalog
{
	// ASCII only on purpose: section names are UTF-8, and the C locale functions mishandle bytes
	// above 0x7F.
	static bool IsSpace(char c)
	{
		return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
	}

	static char ToLower(char c)
	{
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}

	static std::string_view TrimStart(std::string_view s)
	{
		size_t i = 0;
		while (i < s.size() && IsSpace(s[i]))
			i++;
		return s.substr(i);
	}

	static std::string_view TrimEnd(std::string_view s)
	{
		size_t n = s.size();
		while (n > 0 && IsSpace(s[n - 1]))
			n--;
		return s.substr(0, n);
	}

	static std::string_view Trim(std::string_view s)
	{
		return TrimEnd(TrimStart(s));
	}

	/// `key = value` with the key matched case-insensitively and exactly (so "comments=" is not
	/// "comment="). Returns the trimmed value, or an empty view if the line is not that key or
	/// the value is blank.
	static std::string_view KeyValue(std::string_view trimmed_line, std::string_view key)
	{
		if (trimmed_line.size() < key.size())
			return {};
		for (size_t i = 0; i < key.size(); i++)
		{
			if (ToLower(trimmed_line[i]) != key[i])
				return {};
		}

		std::string_view rest = TrimStart(trimmed_line.substr(key.size()));
		if (rest.empty() || rest.front() != '=')
			return {};
		return Trim(rest.substr(1));
	}

	PnachFile ParsePnach(std::string_view text)
	{
		PnachFile file;

		if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
			text.remove_prefix(3);

		bool in_section = false;
		PnachSection current;
		std::string comment;
		std::string description;

		const auto flush = [&]() {
			if (in_section && !current.name.empty())
			{
				current.description = !comment.empty() ? comment : description;
				const std::string_view body = TrimEnd(current.body);
				current.body.resize(body.size());
				file.sections.push_back(std::move(current));
			}
			current = PnachSection();
			comment.clear();
			description.clear();
			in_section = false;
		};

		size_t pos = 0;
		while (pos <= text.size())
		{
			size_t end = pos;
			while (end < text.size() && text[end] != '\n' && text[end] != '\r')
				end++;
			const std::string_view line = text.substr(pos, end - pos);

			// "\r\n" is one terminator; a lone "\r" or "\n" is one too.
			if (end < text.size() && text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n')
				end++;
			pos = end + 1;

			const std::string_view trimmed = Trim(line);

			if (file.game_title.empty())
				file.game_title = std::string(KeyValue(trimmed, "gametitle"));

			if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']')
			{
				flush();
				in_section = true;
				current.name = std::string(Trim(trimmed.substr(1, trimmed.size() - 2)));
				current.body.assign(line);
				current.body.push_back('\n');
			}
			else if (in_section)
			{
				current.body.append(line);
				current.body.push_back('\n');

				if (comment.empty())
					comment = std::string(KeyValue(trimmed, "comment"));
				if (description.empty())
					description = std::string(KeyValue(trimmed, "description"));
			}
		}
		flush();

		return file;
	}

	std::string ComposePnach(std::string_view existing, std::string_view game_title, const std::vector<std::string>& bodies)
	{
		std::string out;

		if (existing.empty())
		{
			// A title with a line break in it would end the gametitle= line early.
			std::string title(game_title);
			for (char& c : title)
			{
				if (c == '\r' || c == '\n')
					c = ' ';
			}
			const std::string_view trimmed_title = Trim(title);
			if (!trimmed_title.empty())
			{
				out.append("gametitle=");
				out.append(trimmed_title);
				out.append("\n\n");
			}
		}
		else
		{
			out.assign(existing);

			// Count the newlines the file already ends with (ignoring the CR of CRLF) and top up to
			// a blank line.
			size_t newlines = 0;
			for (size_t i = out.size(); i > 0; i--)
			{
				const char c = out[i - 1];
				if (c == '\n')
					newlines++;
				else if (c != '\r')
					break;
			}
			for (; newlines < 2; newlines++)
				out.push_back('\n');
		}

		for (const std::string& body : bodies)
		{
			out.append(TrimEnd(body));
			out.append("\n\n");
		}
		return out;
	}

	std::string CheatNameKey(std::string_view name)
	{
		std::string key;
		key.reserve(name.size());

		bool pending_space = false;
		for (const char c : Trim(name))
		{
			if (IsSpace(c))
			{
				pending_space = true;
				continue;
			}
			if (pending_space)
			{
				key.push_back(' ');
				pending_space = false;
			}
			key.push_back(ToLower(c));
		}
		return key;
	}
} // namespace Catalog
