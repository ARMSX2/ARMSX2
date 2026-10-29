// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Builders for the texture-pack tests: a GNU tar writer (hand-written headers, so the tests can
// also write the headers a real tar never would), zstd and zip helpers, and a catalog entry that
// describes an archive it was just handed.

#include "catalog_test_support.h"

#include "Catalog/TexturePackCatalog.h"

#include "common/SHA256.h"

#include <zip.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace CatalogTest
{
	/// One tar entry; the header fields can be set wrong on purpose.
	struct TarEntry
	{
		std::string name;
		char typeflag = '0';
		std::string data;

		/// Size to write in the header; ~0 means data.size().
		u64 declared_size = ~u64(0);

		/// 8 bytes: magic + version. The default is GNU's "ustar  \0".
		std::string magic_version = std::string("ustar  \0", 8);
		std::string prefix;
		std::string linkname;
		bool bad_checksum = false;
		bool base256_size = false;
		bool omit_payload = false; ///< write the header only, whatever declared_size says
	};

	inline std::string TarHeader(const TarEntry& e)
	{
		std::string h(512, '\0');
		const auto put = [&h](size_t offset, std::string_view s, size_t max) {
			std::memcpy(&h[offset], s.data(), std::min(s.size(), max));
		};
		const auto octal = [](u64 value, int digits) {
			std::string s(static_cast<size_t>(digits), '0');
			for (int i = digits - 1; i >= 0; i--)
			{
				s[static_cast<size_t>(i)] = static_cast<char>('0' + (value & 7));
				value >>= 3;
			}
			return s;
		};

		put(0, e.name, 100);
		put(100, octal(0644, 7), 7);
		put(108, octal(0, 7), 7);
		put(116, octal(0, 7), 7);
		const u64 size = (e.declared_size == ~u64(0)) ? e.data.size() : e.declared_size;
		if (e.base256_size)
		{
			h[124] = static_cast<char>(0x80);
			h[135] = static_cast<char>(size & 0xFF);
		}
		else
		{
			put(124, octal(size, 11), 11);
		}
		put(136, octal(0, 11), 11);
		h[156] = e.typeflag;
		put(157, e.linkname, 100);
		put(257, e.magic_version, 8);
		put(345, e.prefix, 155);

		std::memset(&h[148], ' ', 8);
		u64 sum = 0;
		for (const char c : h)
			sum += static_cast<u8>(c);
		if (e.bad_checksum)
			sum += 1;
		put(148, octal(sum, 6), 6);
		h[154] = '\0';
		h[155] = ' ';
		return h;
	}

	class TarBuilder
	{
	public:
		/// A fully described entry. Pads the payload to 512 bytes.
		TarBuilder& Entry(const TarEntry& e)
		{
			m_data += TarHeader(e);
			if (!e.omit_payload)
			{
				m_data += e.data;
				m_data.append((512 - e.data.size() % 512) % 512, '\0');
			}
			return *this;
		}

		TarBuilder& Dir(std::string_view name)
		{
			TarEntry e;
			e.name = std::string(name);
			e.typeflag = '5';
			return Entry(e);
		}

		/// A regular file. A name over 100 bytes goes in a GNU 'L' record like GNU tar writes.
		TarBuilder& File(std::string_view name, std::string_view data)
		{
			if (name.size() > 100)
				LongName(name);
			TarEntry e;
			e.name = std::string(name.substr(0, 100));
			e.data = std::string(data);
			return Entry(e);
		}

		/// The 'L' record for `name` (NUL terminator added), which applies to the next entry.
		TarBuilder& LongName(std::string_view name)
		{
			return LongNamePayload(std::string(name) + '\0');
		}

		/// An 'L' record with exactly this payload, for malformed ones.
		TarBuilder& LongNamePayload(std::string payload)
		{
			TarEntry e;
			e.name = "././@LongLink";
			e.typeflag = 'L';
			e.data = std::move(payload);
			return Entry(e);
		}

		/// The tar bytes. `end_blocks` zero blocks close the archive; the result is then padded
		/// with zeros to a multiple of `pad_to` like tar's blocking factor.
		std::string Finish(size_t end_blocks = 2, size_t pad_to = 10240) const
		{
			std::string out = m_data;
			out.append(end_blocks * 512, '\0');
			if (pad_to != 0)
				out.append((pad_to - out.size() % pad_to) % pad_to, '\0');
			return out;
		}

	private:
		std::string m_data;
	};

	inline std::string ZstdCompress(std::string_view data, int level = 3)
	{
		std::string out(ZSTD_compressBound(data.size()), '\0');
		const size_t n = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), level);
		EXPECT_FALSE(ZSTD_isError(n));
		out.resize(n);
		return out;
	}

	/// One frame whose header announces a window of 2^window_log bytes: compressed as a stream
	/// with no promised size, so the encoder cannot shrink the window to fit the input.
	inline std::string ZstdCompressWithWindow(std::string_view data, int window_log)
	{
		ZSTD_CCtx* cctx = ZSTD_createCCtx();
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, window_log);
		std::string out(ZSTD_compressBound(data.size()) + 1024, '\0');
		ZSTD_inBuffer in{data.data(), data.size(), 0};
		ZSTD_outBuffer o{out.data(), out.size(), 0};
		// Feeding the data with ZSTD_e_continue and ending separately keeps the size unpromised;
		// a single ZSTD_e_end call would let the encoder shrink the window to the input.
		EXPECT_FALSE(ZSTD_isError(ZSTD_compressStream2(cctx, &o, &in, ZSTD_e_continue)));
		ZSTD_inBuffer none{nullptr, 0, 0};
		const size_t rest = ZSTD_compressStream2(cctx, &o, &none, ZSTD_e_end);
		EXPECT_EQ(rest, 0u);
		ZSTD_freeCCtx(cctx);
		out.resize(o.pos);
		return out;
	}

	inline const u8* Bytes(const std::string& s)
	{
		return reinterpret_cast<const u8*>(s.data());
	}

	inline bool WriteZip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files)
	{
		int error = 0;
		zip_t* za = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
		if (!za)
			return false;
		for (const auto& [name, data] : files)
		{
			zip_source_t* source = zip_source_buffer(za, data.data(), data.size(), 0);
			if (!source || zip_file_add(za, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0)
			{
				zip_discard(za);
				return false;
			}
		}
		// The sources read from `files` while closing, which is still alive here.
		return zip_close(za) == 0;
	}

	/// Every regular file under `dir`, as '/'-separated paths relative to it, sorted.
	inline std::set<std::string> ListFiles(const std::string& dir)
	{
		std::set<std::string> out;
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec))
			return out;
		for (const auto& item : std::filesystem::recursive_directory_iterator(dir, ec))
		{
			if (item.is_regular_file())
				out.insert(std::filesystem::relative(item.path(), dir).generic_string());
		}
		return out;
	}

	inline std::string ReadFileOrEmpty(const std::string& path)
	{
		return FileSystem::ReadFileToString(path.c_str()).value_or(std::string());
	}

	/// A catalog entry that describes `archive` exactly.
	inline Catalog::TexturePack DescribeArchive(std::string id, std::string serial, const std::string& archive,
		Catalog::TexturePackFormat format, u64 decompressed_bytes = 0, u32 file_count = 1)
	{
		Catalog::TexturePack pack;
		pack.id = std::move(id);
		pack.name = "Test pack " + pack.id;
		pack.serials = {std::move(serial)};
		pack.version = "v1";
		pack.authors = {"tester"};
		pack.source_url = "https://example.com/" + pack.id;
		pack.download_url = "https://dl.example.com/" + pack.id + ".bin";
		pack.format = format;
		pack.size_bytes = archive.size();
		pack.sha256 = SHA256Digest::HashHex(archive);
		pack.file_count = file_count;
		if (format == Catalog::TexturePackFormat::TarZstd)
		{
			pack.archive_revision = 1;
			pack.decompressed_size_bytes = decompressed_bytes;
		}
		pack.creators_from_authors = true;
		pack.creators = {Catalog::TexturePackCreator{"tester", {}, {}, {}, {}}};
		return pack;
	}
} // namespace CatalogTest
