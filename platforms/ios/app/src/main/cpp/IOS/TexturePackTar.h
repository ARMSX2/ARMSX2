// TexturePackTar.h — the tar profile texture packs ship in
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

// Reads what Android's TarTextureExtractor accepts: regular files, directories and GNU long
// names, each header checksummed. Links, devices, pax records and base-256 sizes fail the
// archive, since the catalog's producer never writes them.
namespace TexturePackTar
{
	inline bool Octal(const char* field, size_t length, uint64_t& value)
	{
		if (static_cast<unsigned char>(field[0]) & 0x80)
			return false;
		size_t i = 0;
		while (i < length && field[i] == ' ')
			i++;
		for (value = 0; i < length && field[i] >= '0' && field[i] <= '7'; i++)
			value = value * 8 + static_cast<uint64_t>(field[i] - '0');
		for (; i < length; i++)
		{
			if (field[i] != '\0' && field[i] != ' ')
				return false;
		}
		return true;
	}

	inline bool ChecksumOk(const unsigned char* header)
	{
		uint64_t stored = 0;
		if (!Octal(reinterpret_cast<const char*>(header) + 148, 8, stored))
			return false;
		int64_t unsigned_sum = 0, signed_sum = 0;
		for (int i = 0; i < 512; i++)
		{
			const int b = (i >= 148 && i < 156) ? ' ' : header[i];
			unsigned_sum += b;
			signed_sum += static_cast<signed char>(b);
		}
		return static_cast<uint64_t>(unsigned_sum) == stored || static_cast<uint64_t>(signed_sum) == stored;
	}

	// read(buffer, n) fills exactly n bytes or returns false. file(name, size) consumes exactly
	// size bytes through read, and returns false to stop, setting error if it has a reason.
	inline bool Read(const std::function<bool(void*, size_t)>& read,
		const std::function<bool(const std::string&, uint64_t)>& file, std::string& error)
	{
		const auto fail = [&error](std::string message) {
			error = std::move(message);
			return false;
		};
		unsigned char header[512];
		char skip[512];
		std::string long_name;
		for (;;)
		{
			if (!read(header, sizeof(header)))
				return fail("the archive ends early");
			if (std::all_of(header, header + sizeof(header), [](unsigned char c) { return c == 0; }))
				return true;
			if (!ChecksumOk(header) || std::memcmp(header + 257, "ustar", 5) != 0)
				return fail("a tar header is damaged");

			uint64_t size = 0;
			if (!Octal(reinterpret_cast<const char*>(header) + 124, 12, size))
				return fail("a tar entry has an unreadable size");
			const size_t padding = static_cast<size_t>((512 - size % 512) % 512);
			const char type = static_cast<char>(header[156]);

			if (type == 'L')
			{
				if (size == 0 || size > 4097)
					return fail("a tar long name is out of range");
				long_name.resize(static_cast<size_t>(size));
				if (!read(long_name.data(), long_name.size()) || !read(skip, padding) || long_name.back() != '\0')
					return fail("a tar long name is damaged");
				long_name.pop_back();
				continue;
			}

			const char* text = reinterpret_cast<const char*>(header);
			std::string name = long_name.empty() ? std::string(text, strnlen(text, 100)) : long_name;
			long_name.clear();
			if (header[345] != '\0' && std::memcmp(header + 257, "ustar\0", 6) == 0)
				name = std::string(text + 345, strnlen(text + 345, 155)) + "/" + name;

			if (type == '5' && size == 0)
				continue;
			if (type != '0' && type != '\0')
				return fail("the archive holds an entry that is not a file or folder: " + name);
			if (!file(name, size))
				return error.empty() ? fail("the archive ends inside " + name) : false;
			if (!read(skip, padding))
				return fail("the archive ends inside " + name);
		}
	}
} // namespace TexturePackTar
