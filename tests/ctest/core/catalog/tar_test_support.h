// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Builds .tar.gz archives in memory for tests, including the awkward ones real tools produce:
// ustar prefix splits, GNU long-name headers, pax path headers, symlinks, and archives a hostile
// server could send (".." paths, absolute paths, bad checksums).

#include "common/FileSystem.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace CatalogTest
{
	// Not called TarEntry: texture_pack_test_support.h has a different CatalogTest::TarEntry, and
	// both headers end up in catalog_tests. Two structs with one name break the one-definition
	// rule (the linker keeps one copy of the implicit members, so one struct gets the other's layout).
	struct TarGzEntry
	{
		enum class Naming
		{
			Plain, ///< ustar: name, or prefix + name for a long path
			GnuLongName, ///< a type 'L' header carries the name
			Pax, ///< a type 'x' header carries a `path=` record
		};

		std::string name;
		std::string data;
		char type = '0'; ///< '0' file, '5' directory, '2' symlink, '1' hard link
		unsigned mode = 0644;
		std::string link_target;
		Naming naming = Naming::Plain;
		bool corrupt_checksum = false;
	};

	inline void PutOctal(char* field, size_t width, unsigned long long value)
	{
		std::snprintf(field, width, "%0*llo", static_cast<int>(width - 1), value);
	}

	inline std::string TarHeader(const std::string& name_field, const std::string& prefix_field, const TarGzEntry& entry,
		char type, unsigned long long size, bool corrupt)
	{
		std::string block(512, '\0');
		char* h = block.data();
		std::memcpy(h, name_field.data(), std::min<size_t>(name_field.size(), 100));
		PutOctal(h + 100, 8, entry.mode);
		PutOctal(h + 108, 8, 0);
		PutOctal(h + 116, 8, 0);
		PutOctal(h + 124, 12, size);
		PutOctal(h + 136, 12, 0);
		std::memset(h + 148, ' ', 8);
		h[156] = type;
		std::memcpy(h + 157, entry.link_target.data(), std::min<size_t>(entry.link_target.size(), 100));
		std::memcpy(h + 257, "ustar\0"
							 "00",
			8);
		std::memcpy(h + 345, prefix_field.data(), std::min<size_t>(prefix_field.size(), 155));

		unsigned sum = 0;
		for (size_t i = 0; i < 512; i++)
			sum += static_cast<unsigned char>(h[i]);
		std::snprintf(h + 148, 8, "%06o", corrupt ? sum + 1 : sum);
		h[154] = '\0';
		h[155] = ' ';
		return block;
	}

	inline void PadTo512(std::string& out)
	{
		out.resize((out.size() + 511) / 512 * 512, '\0');
	}

	inline std::string BuildTar(const std::vector<TarGzEntry>& entries)
	{
		std::string tar;
		for (const TarGzEntry& entry : entries)
		{
			std::string name_field = entry.name;
			std::string prefix_field;

			if (entry.naming == TarGzEntry::Naming::GnuLongName)
			{
				TarGzEntry meta;
				const std::string payload = entry.name + '\0';
				tar += TarHeader("././@LongLink", "", meta, 'L', payload.size(), false);
				tar += payload;
				PadTo512(tar);
				name_field = entry.name.substr(0, 90);
			}
			else if (entry.naming == TarGzEntry::Naming::Pax)
			{
				const std::string record_body = " path=" + entry.name + "\n";
				// The length counts its own digits.
				size_t length = record_body.size() + 1;
				while (std::to_string(length).size() + record_body.size() != length)
					length = std::to_string(length).size() + record_body.size();
				const std::string payload = std::to_string(length) + record_body;
				TarGzEntry meta;
				tar += TarHeader("PaxHeader", "", meta, 'x', payload.size(), false);
				tar += payload;
				PadTo512(tar);
				name_field = "pax-placeholder";
			}
			else if (entry.name.size() > 100)
			{
				// ustar: split at a '/' so the tail fits in 100 bytes and the head in 155.
				const size_t slash = entry.name.find('/', entry.name.size() - 100);
				if (slash != std::string::npos && slash <= 155)
				{
					prefix_field = entry.name.substr(0, slash);
					name_field = entry.name.substr(slash + 1);
				}
			}

			const bool has_data = (entry.type == '0');
			tar += TarHeader(name_field, prefix_field, entry, entry.type, has_data ? entry.data.size() : 0, entry.corrupt_checksum);
			if (has_data)
			{
				tar += entry.data;
				PadTo512(tar);
			}
		}
		tar.append(1024, '\0');
		PadTo512(tar);
		return tar;
	}

	inline std::string Gzip(const std::string& data)
	{
		z_stream z;
		std::memset(&z, 0, sizeof(z));
		deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
		std::string out(deflateBound(&z, static_cast<uLong>(data.size())) + 64, '\0');
		z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
		z.avail_in = static_cast<uInt>(data.size());
		z.next_out = reinterpret_cast<Bytef*>(out.data());
		z.avail_out = static_cast<uInt>(out.size());
		deflate(&z, Z_FINISH);
		out.resize(z.total_out);
		deflateEnd(&z);
		return out;
	}

	inline std::string BuildTarGz(const std::vector<TarGzEntry>& entries)
	{
		return Gzip(BuildTar(entries));
	}

	inline bool WriteBytes(const std::string& path, const std::string& bytes)
	{
		std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "wb");
		if (!fp)
			return false;
		const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
		return (std::fclose(fp) == 0) && ok;
	}
} // namespace CatalogTest
