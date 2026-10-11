// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/TarGz.h"

#include "common/Error.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace Catalog
{
	namespace
	{
		constexpr size_t BLOCK_SIZE = 512;
		constexpr size_t MAX_PATH_BYTES = 4096;
		constexpr size_t MAX_PATH_COMPONENTS = 32;
		constexpr size_t MAX_COMPONENT_BYTES = 255;
		/// Cap on the data of a long-name or pax header; real ones are a few hundred bytes.
		constexpr u64 MAX_METADATA_BYTES = 64 * 1024;
		constexpr size_t COPY_CHUNK = 64 * 1024;

		/// Sequential reader over one gzip member. Every failure latches: after the first, Read()
		/// returns 0 and Error() has the cause.
		class GzReader
		{
		public:
			GzReader(std::FILE* fp, u64 max_total_bytes)
				: m_fp(fp)
				, m_max_total(max_total_bytes)
				, m_in(COPY_CHUNK)
			{
				std::memset(&m_z, 0, sizeof(m_z));
				// 15 + 16: gzip wrapper only. A bare zlib stream or random bytes are refused.
				m_initialised = (inflateInit2(&m_z, 15 + 16) == Z_OK);
				if (!m_initialised)
					Fail("Could not start the gzip decoder.");
			}

			~GzReader()
			{
				if (m_initialised)
					inflateEnd(&m_z);
			}

			GzReader(const GzReader&) = delete;
			GzReader& operator=(const GzReader&) = delete;

			/// Reads up to `size` bytes; fewer only at the end of the stream or on failure.
			size_t Read(void* out, size_t size)
			{
				u8* dst = static_cast<u8*>(out);
				size_t done = 0;
				while (done < size && !m_ended && !m_failed)
				{
					if (m_z.avail_in == 0)
					{
						const size_t n = std::fread(m_in.data(), 1, m_in.size(), m_fp);
						if (n == 0)
						{
							Fail(std::ferror(m_fp) ? "Reading the archive failed." : "The archive is truncated (the gzip data ends early).");
							break;
						}
						m_z.next_in = m_in.data();
						m_z.avail_in = static_cast<uInt>(n);
					}

					m_z.next_out = dst + done;
					m_z.avail_out = static_cast<uInt>(std::min<size_t>(size - done, 1u << 30));
					const uInt before = m_z.avail_out;
					const int rc = inflate(&m_z, Z_NO_FLUSH);
					done += before - m_z.avail_out;

					if (rc == Z_STREAM_END)
						m_ended = true;
					else if (rc != Z_OK && !(rc == Z_BUF_ERROR && m_z.avail_in == 0))
					{
						Fail(fmt::format("The archive is corrupt ({}).", m_z.msg ? m_z.msg : "gzip error"));
						break;
					}
				}

				m_total += done;
				if (m_total > m_max_total && !m_failed)
					Fail(fmt::format("The archive expands to more than {} bytes; refusing it.", m_max_total));
				return m_failed ? 0 : done;
			}

			bool Skip(u64 count)
			{
				u8 scratch[4096];
				while (count > 0)
				{
					const size_t want = static_cast<size_t>(std::min<u64>(count, sizeof(scratch)));
					if (Read(scratch, want) != want)
						return false;
					count -= want;
				}
				return true;
			}

			/// Reads to the end of the gzip member so zlib verifies its CRC and length trailer.
			bool Drain()
			{
				u8 scratch[4096];
				while (!m_ended && !m_failed)
					Read(scratch, sizeof(scratch));
				return !m_failed;
			}

			bool Failed() const { return m_failed; }
			bool Ended() const { return m_ended; }
			const std::string& Error() const { return m_error; }

		private:
			void Fail(std::string message)
			{
				if (!m_failed)
				{
					m_failed = true;
					m_error = std::move(message);
				}
			}

			std::FILE* m_fp;
			u64 m_max_total;
			u64 m_total = 0;
			std::vector<u8> m_in;
			z_stream m_z;
			bool m_initialised = false;
			bool m_ended = false;
			bool m_failed = false;
			std::string m_error;
		};

		bool IsZeroBlock(const u8* block)
		{
			for (size_t i = 0; i < BLOCK_SIZE; i++)
			{
				if (block[i] != 0)
					return false;
			}
			return true;
		}

		/// Octal ASCII (space/NUL padded), or GNU base-256 when the top bit of the first byte is set.
		bool ParseNumber(const u8* field, size_t length, u64* out)
		{
			if (field[0] & 0x80)
			{
				if (field[0] & 0x40)
					return false; // negative
				u64 value = field[0] & 0x3F;
				for (size_t i = 1; i < length; i++)
				{
					if (value >> 56)
						return false;
					value = (value << 8) | field[i];
				}
				*out = value;
				return true;
			}

			size_t i = 0;
			while (i < length && field[i] == ' ')
				i++;
			u64 value = 0;
			bool any = false;
			for (; i < length && field[i] >= '0' && field[i] <= '7'; i++)
			{
				if (value >> 60)
					return false;
				value = (value << 3) | static_cast<u64>(field[i] - '0');
				any = true;
			}
			// What follows must be padding.
			for (; i < length; i++)
			{
				if (field[i] != ' ' && field[i] != '\0')
					return false;
			}
			if (!any)
				return false;
			*out = value;
			return true;
		}

		bool HeaderChecksumOk(const u8* header)
		{
			u64 stored = 0;
			if (!ParseNumber(header + 148, 8, &stored))
				return false;

			u32 unsigned_sum = 0;
			s32 signed_sum = 0;
			for (size_t i = 0; i < BLOCK_SIZE; i++)
			{
				const u8 byte = (i >= 148 && i < 156) ? ' ' : header[i];
				unsigned_sum += byte;
				signed_sum += static_cast<s8>(byte);
			}
			return stored == unsigned_sum || static_cast<s64>(stored) == signed_sum;
		}

		std::string FieldString(const u8* field, size_t length)
		{
			const void* nul = std::memchr(field, 0, length);
			const size_t size = nul ? static_cast<size_t>(static_cast<const u8*>(nul) - field) : length;
			return std::string(reinterpret_cast<const char*>(field), size);
		}

		struct PaxRecords
		{
			bool has_path = false;
			std::string path;
			bool has_size = false;
			u64 size = 0;
		};

		/// "<len> <key>=<value>\n" records, <len> counting the whole record including itself.
		bool ParsePax(std::string_view data, PaxRecords* out)
		{
			size_t pos = 0;
			while (pos < data.size())
			{
				size_t length = 0;
				size_t digits = 0;
				while (pos + digits < data.size() && data[pos + digits] >= '0' && data[pos + digits] <= '9' && digits < 10)
				{
					length = length * 10 + static_cast<size_t>(data[pos + digits] - '0');
					digits++;
				}
				if (digits == 0 || pos + digits >= data.size() || data[pos + digits] != ' ' || length < digits + 3 || pos + length > data.size())
					return false;

				std::string_view record = data.substr(pos + digits + 1, length - digits - 1);
				if (record.empty() || record.back() != '\n')
					return false;
				record.remove_suffix(1);

				const size_t eq = record.find('=');
				if (eq != std::string_view::npos)
				{
					const std::string_view key = record.substr(0, eq);
					const std::string_view value = record.substr(eq + 1);
					if (key == "path")
					{
						out->has_path = true;
						out->path.assign(value);
					}
					else if (key == "size")
					{
						u64 parsed = 0;
						if (value.empty() || value.size() > 19)
							return false;
						for (const char c : value)
						{
							if (c < '0' || c > '9')
								return false;
							parsed = parsed * 10 + static_cast<u64>(c - '0');
						}
						out->has_size = true;
						out->size = parsed;
					}
				}
				pos += length;
			}
			return true;
		}

		TarResult Fail(std::string* error, std::string message)
		{
			if (error)
				*error = std::move(message);
			return TarResult::Failed;
		}
	} // namespace

	bool NormalizeArchivePath(std::string_view name, std::string* out, std::string* error)
	{
		const auto reject = [&](const char* why) {
			if (error)
				*error = fmt::format("Unsafe path in the archive ({}): \"{}\"", why, name.substr(0, 120));
			return false;
		};

		out->clear();
		if (name.size() > MAX_PATH_BYTES)
			return reject("too long");
		if (name.find('\0') != std::string_view::npos || name.find('\\') != std::string_view::npos)
			return reject("invalid character");
		if (!name.empty() && name.front() == '/')
			return reject("absolute");

		size_t components = 0;
		size_t pos = 0;
		while (pos <= name.size())
		{
			size_t end = name.find('/', pos);
			if (end == std::string_view::npos)
				end = name.size();
			const std::string_view part = name.substr(pos, end - pos);
			pos = end + 1;

			if (part.empty() || part == ".")
				continue;
			if (part == "..")
				return reject("parent directory");
			if (part.size() > MAX_COMPONENT_BYTES)
				return reject("name too long");
			if (++components > MAX_PATH_COMPONENTS)
				return reject("too deep");

			if (!out->empty())
				out->push_back('/');
			out->append(part);
		}
		return true;
	}

	TarResult ExtractTarGz(const std::string& archive_path, const std::string& dest_dir, const TarLimits& limits,
		TarExtractInfo* info, std::string* error, const std::atomic_bool* cancel)
	{
		namespace fs = std::filesystem;

		TarExtractInfo local_info;
		TarExtractInfo& result = info ? *info : local_info;
		result = {};

		Error open_error;
		std::FILE* fp = FileSystem::OpenCFile(archive_path.c_str(), "rb", &open_error);
		if (!fp)
			return Fail(error, fmt::format("Could not open the downloaded archive: {}", open_error.GetDescription()));
		struct FileCloser
		{
			std::FILE* fp;
			~FileCloser() { std::fclose(fp); }
		} closer{fp};

		std::error_code ec;
		fs::create_directories(fs::path(dest_dir), ec);
		if (ec)
			return Fail(error, fmt::format("Could not create \"{}\": {}", dest_dir, ec.message()));

		GzReader reader(fp, limits.max_total_bytes);

		const auto stream_failed = [&]() { return Fail(error, reader.Error()); };
		const auto cancelled = [&]() { return cancel && cancel->load(std::memory_order_acquire); };

		// Pending metadata from a preceding 'L' or 'x' header; applies to the next real entry.
		std::string long_name;
		bool have_long_name = false;
		PaxRecords pax;
		u32 entries = 0;

		u8 header[BLOCK_SIZE];
		std::vector<u8> chunk(COPY_CHUNK);

		for (;;)
		{
			if (cancelled())
				return TarResult::Cancelled;

			const size_t got = reader.Read(header, BLOCK_SIZE);
			if (reader.Failed())
				return stream_failed();
			if (got == 0)
				break; // clean end at a header boundary
			if (got != BLOCK_SIZE)
				return Fail(error, "The archive ends in the middle of a tar header.");
			if (IsZeroBlock(header))
				break; // end-of-archive marker

			if (!HeaderChecksumOk(header))
				return Fail(error, "The archive is not a valid tar file (header checksum mismatch).");
			if (++entries > limits.max_entries)
				return Fail(error, fmt::format("The archive has more than {} entries; refusing it.", limits.max_entries));

			const char type = static_cast<char>(header[156]);
			u64 size = 0;
			if (!ParseNumber(header + 124, 12, &size))
				return Fail(error, "The archive has an unreadable entry size.");
			u64 mode = 0644;
			ParseNumber(header + 100, 8, &mode);

			const u64 padding = (BLOCK_SIZE - (size % BLOCK_SIZE)) % BLOCK_SIZE;

			// ---- metadata headers: their data is not a file --------------------------------
			if (type == 'L' || type == 'K' || type == 'x' || type == 'g')
			{
				if (size > MAX_METADATA_BYTES)
					return Fail(error, "The archive has an oversized metadata header.");
				std::string data(static_cast<size_t>(size), '\0');
				if (size > 0 && reader.Read(data.data(), data.size()) != data.size())
					return reader.Failed() ? stream_failed() : Fail(error, "The archive ends in the middle of a metadata header.");
				if (!reader.Skip(padding))
					return reader.Failed() ? stream_failed() : Fail(error, "The archive ends in the middle of a metadata header.");

				if (type == 'L')
				{
					const size_t nul = data.find('\0');
					long_name = (nul == std::string::npos) ? data : data.substr(0, nul);
					have_long_name = true;
				}
				else if (type == 'x')
				{
					if (!ParsePax(data, &pax))
						return Fail(error, "The archive has a malformed pax header.");
				}
				continue; // 'K' (long link target) and 'g' (global pax) carry nothing we use
			}

			// ---- the entry's name ----------------------------------------------------------
			std::string name;
			if (pax.has_path)
				name = pax.path;
			else if (have_long_name)
				name = long_name;
			else
			{
				name = FieldString(header, 100);
				// Only POSIX ustar has the prefix field; GNU's "ustar  " uses those bytes for times.
				if (std::memcmp(header + 257, "ustar\0", 6) == 0)
				{
					const std::string prefix = FieldString(header + 345, 155);
					if (!prefix.empty())
						name = prefix + "/" + name;
				}
			}
			if (pax.has_size)
				size = pax.size;
			const u64 entry_padding = (BLOCK_SIZE - (size % BLOCK_SIZE)) % BLOCK_SIZE;
			have_long_name = false;
			long_name.clear();
			pax = {};

			const bool is_regular = (type == '0' || type == '\0' || type == '7');
			const bool is_directory = (type == '5');

			if (!is_regular && !is_directory)
			{
				// Links, devices, FIFOs: never created. Their data (if any) is skipped.
				result.skipped_entries++;
				if (!reader.Skip(size + entry_padding))
					return reader.Failed() ? stream_failed() : Fail(error, "The archive ends in the middle of an entry.");
				continue;
			}

			std::string relative;
			std::string path_error;
			if (!NormalizeArchivePath(name, &relative, &path_error))
				return Fail(error, std::move(path_error));

			if (is_directory)
			{
				if (!relative.empty())
				{
					fs::create_directories(fs::path(dest_dir) / relative, ec);
					if (ec)
						return Fail(error, fmt::format("Could not create \"{}\": {}", relative, ec.message()));
				}
				if (!reader.Skip(size + entry_padding))
					return reader.Failed() ? stream_failed() : Fail(error, "The archive ends in the middle of an entry.");
				continue;
			}

			if (relative.empty())
				return Fail(error, "The archive has a file with no name.");
			if (size > limits.max_file_bytes)
				return Fail(error, fmt::format("\"{}\" is {} bytes, over the {} byte limit for one file.", relative, size, limits.max_file_bytes));

			const fs::path target = fs::path(dest_dir) / relative;
			fs::create_directories(target.parent_path(), ec);
			if (ec)
				return Fail(error, fmt::format("Could not create the folder for \"{}\": {}", relative, ec.message()));

			Error file_error;
			std::FILE* out = FileSystem::OpenCFile(target.string().c_str(), "wb", &file_error);
			if (!out)
				return Fail(error, fmt::format("Could not write \"{}\": {}", relative, file_error.GetDescription()));

			u64 remaining = size;
			bool write_ok = true;
			bool truncated = false;
			while (remaining > 0)
			{
				if (cancelled())
				{
					std::fclose(out);
					return TarResult::Cancelled;
				}
				const size_t want = static_cast<size_t>(std::min<u64>(remaining, chunk.size()));
				if (reader.Read(chunk.data(), want) != want)
				{
					truncated = true;
					break;
				}
				if (std::fwrite(chunk.data(), 1, want, out) != want)
				{
					write_ok = false;
					break;
				}
				remaining -= want;
			}
			write_ok = (std::fclose(out) == 0) && write_ok;

			if (truncated)
				return reader.Failed() ? stream_failed() : Fail(error, fmt::format("The archive ends in the middle of \"{}\".", relative));
			if (!write_ok)
				return Fail(error, fmt::format("Writing \"{}\" failed (disk full?).", relative));

			fs::permissions(target, (mode & 0111) ? (fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec) : (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read), fs::perm_options::replace, ec);

			if (!reader.Skip(entry_padding))
				return reader.Failed() ? stream_failed() : Fail(error, fmt::format("The archive ends in the middle of \"{}\".", relative));

			result.files.push_back(relative);
			result.bytes_written += size;
		}

		// Read the gzip trailer so a corrupted archive is caught here and not later.
		if (!reader.Drain())
			return stream_failed();
		return TarResult::Ok;
	}
} // namespace Catalog
