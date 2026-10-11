// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/TarZstdExtractor.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

namespace Catalog
{
	namespace
	{
		constexpr size_t RECORD = 512;
		constexpr size_t CHUNK_BYTES = 256 * 1024;
		/// A GNU 'L' payload is the name plus its NUL: names are at most 4096 bytes.
		constexpr u64 MAX_LONGNAME_PAYLOAD = 4097;

		constexpr u8 TYPE_FILE_NUL = 0;
		constexpr u8 TYPE_FILE = '0';
		constexpr u8 TYPE_DIR = '5';
		constexpr u8 TYPE_LONGNAME = 'L';

		// ---------------------------------------------------------------------------------
		// Compressed input
		// ---------------------------------------------------------------------------------

		class CompressedSource
		{
		public:
			virtual ~CompressedSource() = default;
			/// Bytes read, 0 at the end of the input, -1 on a read error.
			virtual s64 Read(void* dst, size_t max) = 0;
		};

		class FileSource final : public CompressedSource
		{
		public:
			explicit FileSource(std::FILE* fp)
				: m_fp(fp)
			{
			}

			s64 Read(void* dst, size_t max) override
			{
				const size_t got = std::fread(dst, 1, max, m_fp);
				if (got == 0 && std::ferror(m_fp))
					return -1;
				return static_cast<s64>(got);
			}

		private:
			std::FILE* m_fp;
		};

		class MemorySource final : public CompressedSource
		{
		public:
			explicit MemorySource(std::span<const u8> data)
				: m_data(data)
			{
			}

			s64 Read(void* dst, size_t max) override
			{
				const size_t n = std::min(max, m_data.size() - m_pos);
				if (n != 0)
					std::memcpy(dst, m_data.data() + m_pos, n);
				m_pos += n;
				return static_cast<s64>(n);
			}

		private:
			std::span<const u8> m_data;
			size_t m_pos = 0;
		};

		// ---------------------------------------------------------------------------------
		// zstd: exactly one frame, then the end of the input
		// ---------------------------------------------------------------------------------

		class ZstdFrameReader
		{
		public:
			ZstdFrameReader(CompressedSource& source, u64 max_output)
				: m_source(source)
				, m_max_output(max_output)
				, m_ctx(ZSTD_createDCtx())
				, m_in_data(CHUNK_BYTES)
			{
				if (!m_ctx || ZSTD_isError(ZSTD_DCtx_setParameter(m_ctx, ZSTD_d_windowLogMax, TarZstdExtractor::MAX_WINDOW_LOG)))
					Fail("could not create the decoder");
			}

			~ZstdFrameReader()
			{
				if (m_ctx)
					ZSTD_freeDCtx(m_ctx);
			}

			ZstdFrameReader(const ZstdFrameReader&) = delete;
			ZstdFrameReader& operator=(const ZstdFrameReader&) = delete;

			bool Failed() const { return m_failed; }
			const std::string& Error() const { return m_error; }
			u64 Produced() const { return m_produced; }

			/// Decompresses up to `n` bytes into `dst`. A result below `n` means the frame ended
			/// (after checking that nothing follows it) or the stream failed: look at Failed().
			size_t Read(void* dst, size_t n)
			{
				if (m_failed)
					return 0;

				ZSTD_outBuffer out{dst, n, 0};
				while (out.pos < out.size && !m_frame_done)
				{
					const size_t out_before = out.pos;
					const size_t in_before = m_in.pos;
					const size_t ret = ZSTD_decompressStream(m_ctx, &out, &m_in);
					if (ZSTD_isError(ret))
					{
						Fail(ZSTD_getErrorName(ret));
						return out.pos;
					}

					m_produced += out.pos - out_before;
					if (m_produced > m_max_output)
					{
						Fail("decompressed output exceeds the size the catalog declared");
						return out.pos;
					}

					if (ret == 0)
					{
						m_frame_done = true;
						break;
					}

					if (out.pos == out_before && m_in.pos == in_before)
					{
						// No progress: the decoder needs more input.
						if (m_in.pos < m_in.size)
						{
							Fail("decoder made no progress");
							return out.pos;
						}

						const s64 got = m_source.Read(m_in_data.data(), m_in_data.size());
						if (got < 0)
						{
							Fail("could not read the archive");
							return out.pos;
						}
						if (got == 0)
						{
							Fail("truncated frame: unexpected end of compressed input");
							return out.pos;
						}
						m_in = ZSTD_inBuffer{m_in_data.data(), static_cast<size_t>(got), 0};
					}
				}

				if (m_frame_done && !m_trailer_checked)
				{
					m_trailer_checked = true;
					if (m_in.pos < m_in.size)
					{
						Fail("trailing compressed bytes after the frame");
					}
					else
					{
						u8 probe;
						const s64 got = m_source.Read(&probe, 1);
						if (got < 0)
							Fail("could not read the archive");
						else if (got > 0)
							Fail("trailing compressed bytes after the frame");
					}
				}
				return out.pos;
			}

		private:
			void Fail(std::string why)
			{
				if (!m_failed)
				{
					m_failed = true;
					m_error = "zstd: " + std::move(why);
				}
			}

			CompressedSource& m_source;
			const u64 m_max_output;
			ZSTD_DCtx* m_ctx;
			std::vector<u8> m_in_data;
			ZSTD_inBuffer m_in{nullptr, 0, 0};
			u64 m_produced = 0;
			bool m_frame_done = false;
			bool m_trailer_checked = false;
			bool m_failed = false;
			std::string m_error;
		};

		// ---------------------------------------------------------------------------------
		// tar
		// ---------------------------------------------------------------------------------

		bool IsAllZero(const u8* data, size_t size)
		{
			return std::all_of(data, data + size, [](u8 b) { return b == 0; });
		}

		// A tar numeric field: octal digits, optional leading spaces, ended by NUL or space.
		// GNU base-256 (high bit set) is outside the profile.
		std::optional<u64> ParseOctal(const u8* field, size_t len)
		{
			if (field[0] & 0x80)
				return std::nullopt;

			size_t i = 0;
			while (i < len && field[i] == ' ')
				i++;

			u64 value = 0;
			for (; i < len && field[i] != 0 && field[i] != ' '; i++)
			{
				if (field[i] < '0' || field[i] > '7')
					return std::nullopt;
				if (value > (~u64(0) >> 3))
					return std::nullopt;
				value = (value << 3) | static_cast<u64>(field[i] - '0');
			}
			return value;
		}

		bool ChecksumOk(const u8* header)
		{
			const std::optional<u64> stored = ParseOctal(header + 148, 8);
			if (!stored.has_value())
				return false;

			u64 unsigned_sum = 0;
			s64 signed_sum = 0;
			for (size_t i = 0; i < RECORD; i++)
			{
				const u8 b = (i >= 148 && i < 156) ? u8(' ') : header[i];
				unsigned_sum += b;
				signed_sum += static_cast<s8>(b);
			}
			return unsigned_sum == *stored || (signed_sum >= 0 && static_cast<u64>(signed_sum) == *stored);
		}

		class TarExtraction
		{
		public:
			TarExtraction(ZstdFrameReader& zstd, const std::string& dest_dir, const TarZstdExpectations& expected,
				const ExtractProgress& progress, const CancelToken& cancel)
				: m_zstd(zstd)
				, m_dest(dest_dir)
				, m_expected(expected)
				, m_progress(progress)
				, m_cancel(cancel)
				, m_buffer(CHUNK_BYTES)
			{
			}

			ExtractResult Run()
			{
				m_result = ExtractResult{};
				m_result.status = ExtractStatus::Ok;
				if (!Extract() && m_result.status == ExtractStatus::Ok)
					m_result.status = ExtractStatus::Failed;
				m_result.textures = m_textures;
				return m_result;
			}

		private:
			bool Fail(std::string why)
			{
				m_result.status = ExtractStatus::Failed;
				m_result.error = std::move(why);
				return false;
			}

			bool CheckCancel()
			{
				if (m_cancel.IsCancelled())
				{
					m_result.status = ExtractStatus::Cancelled;
					return false;
				}
				return true;
			}

			bool ReadExact(void* dst, size_t n)
			{
				const size_t got = m_zstd.Read(dst, n);
				if (got == n)
					return true;
				if (m_zstd.Failed())
					return Fail(m_zstd.Error());
				return Fail("tar: unexpected end of the archive");
			}

			bool Consume(u64 size)
			{
				while (size > 0)
				{
					if (!CheckCancel())
						return false;
					const size_t n = static_cast<size_t>(std::min<u64>(size, m_buffer.size()));
					if (!ReadExact(m_buffer.data(), n))
						return false;
					size -= n;
				}
				return true;
			}

			bool SkipPadding(u64 size)
			{
				const u64 pad = (RECORD - size % RECORD) % RECORD;
				return Consume(pad);
			}

			bool EnsureParentDirectory(const std::string& file_path)
			{
				const std::string directory(Path::GetDirectory(file_path));
				if (directory.empty() || m_made_directories.count(directory))
					return true;
				Error error;
				if (!FileSystem::EnsureDirectoryExists(directory.c_str(), true, &error))
					return Fail(fmt::format("tar: could not create a folder: {}", error.GetDescription()));
				m_made_directories.insert(directory);
				return true;
			}

			bool WriteTexture(const std::string& rel, u64 size)
			{
				const std::string out_path = Path::Combine(m_dest, rel);
				if (FileSystem::FileExists(out_path.c_str()) || FileSystem::DirectoryExists(out_path.c_str()))
					return Fail(fmt::format("tar: duplicate destination {}", rel));
				if (!EnsureParentDirectory(out_path))
					return false;

				Error error;
				FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(out_path.c_str(), "wb", &error);
				if (!fp)
					return Fail(fmt::format("tar: could not write {}: {}", rel, error.GetDescription()));

				u64 remaining = size;
				while (remaining > 0)
				{
					if (!CheckCancel())
						return false;
					const size_t n = static_cast<size_t>(std::min<u64>(remaining, m_buffer.size()));
					if (!ReadExact(m_buffer.data(), n))
						return false;
					if (std::fwrite(m_buffer.data(), 1, n, fp.get()) != n)
						return Fail(fmt::format("tar: could not write {} (disk full?)", rel));
					remaining -= n;
				}

				std::FILE* raw = fp.release();
				if (std::fclose(raw) != 0)
					return Fail(fmt::format("tar: could not finish writing {} (disk full?)", rel));
				return true;
			}

			bool Extract()
			{
				std::array<u8, RECORD> header;
				std::string long_name;
				bool have_long_name = false;
				u64 entries = 0;

				// Collision keys. A file may not repeat a file or a folder; a folder may not repeat a
				// folder or a file, but may be the parent of files listed before it.
				std::unordered_set<std::string> file_keys;
				std::unordered_set<std::string> explicit_dir_keys;
				std::unordered_set<std::string> implicit_dir_keys;

				for (;;)
				{
					if (!CheckCancel())
						return false;
					if (!ReadExact(header.data(), RECORD))
						return false;
					if (IsAllZero(header.data(), RECORD))
						break; // first end block

					if (++entries > TarZstdExtractor::MAX_ENTRIES)
						return Fail(fmt::format("tar: entry count exceeds {}", TarZstdExtractor::MAX_ENTRIES));

					if (!ChecksumOk(header.data()))
						return Fail("tar: header checksum mismatch");

					const bool gnu_magic = std::memcmp(header.data() + 257, "ustar ", 6) == 0;
					const bool posix_magic = std::memcmp(header.data() + 257, "ustar\0", 6) == 0;
					if (!gnu_magic && !posix_magic)
						return Fail("tar: unknown magic (not a GNU tar archive)");
					// The ustar prefix field would move the file into another folder, which this
					// reader does not do. A GNU archive uses that area for other things.
					if (posix_magic && header[345] != 0)
						return Fail("tar: ustar prefix names are outside the pack format");

					const u8 typeflag = header[156];
					const std::optional<u64> size = ParseOctal(header.data() + 124, 12);
					if (!size.has_value())
						return Fail("tar: malformed size field (or base-256)");

					if (typeflag == TYPE_LONGNAME)
					{
						if (have_long_name)
							return Fail("tar: GNU L record without a following entry");
						if (*size == 0 || *size > MAX_LONGNAME_PAYLOAD)
							return Fail(fmt::format("tar: GNU L payload must be 1..{} bytes", MAX_LONGNAME_PAYLOAD));
						std::vector<u8> payload(static_cast<size_t>(*size));
						if (!ReadExact(payload.data(), payload.size()) || !SkipPadding(*size))
							return false;
						if (payload.back() != 0)
							return Fail("tar: GNU L payload not NUL-terminated");
						long_name.assign(reinterpret_cast<const char*>(payload.data()), payload.size() - 1);
						have_long_name = true;
						continue;
					}

					const bool is_dir = typeflag == TYPE_DIR;
					const bool is_file = typeflag == TYPE_FILE || typeflag == TYPE_FILE_NUL;
					if (!is_dir && !is_file)
					{
						return Fail(fmt::format("tar: unsupported entry type '{}' (links, devices and pax headers are outside the pack format)",
							(typeflag >= 0x20 && typeflag < 0x7F) ? static_cast<char>(typeflag) : '?'));
					}

					std::string name;
					if (have_long_name)
					{
						name = std::move(long_name);
						long_name.clear();
						have_long_name = false;
					}
					else
					{
						const char* field = reinterpret_cast<const char*>(header.data());
						name.assign(field, strnlen(field, 100));
					}

					const std::optional<std::string> checked = TextureArchivePath::ValidateTarPath(name);
					if (!checked.has_value())
						return Fail(fmt::format("tar: rejected path {}", name.substr(0, 80)));
					const std::string& path = *checked;

					if (path.empty())
					{
						// The producer's root "./" directory.
						if (!is_dir || *size != 0)
							return Fail("tar: empty name on a non-root entry");
						continue;
					}

					const std::string key = TextureArchivePath::CollisionKey(path);

					// Every parent folder must not be a file, and becomes an implicit folder.
					{
						size_t pos = 0;
						while ((pos = path.find('/', pos)) != std::string::npos)
						{
							const std::string ancestor = TextureArchivePath::CollisionKey(path.substr(0, pos));
							if (file_keys.count(ancestor))
								return Fail(fmt::format("tar: file/directory conflict at {}", path.substr(0, pos)));
							implicit_dir_keys.insert(ancestor);
							pos++;
						}
					}

					if (is_dir)
					{
						if (*size != 0)
							return Fail(fmt::format("tar: directory entry with a payload: {}", path));
						if (file_keys.count(key) || !explicit_dir_keys.insert(key).second)
							return Fail(fmt::format("tar: duplicate or colliding destination {}", path));
						continue;
					}

					if (file_keys.count(key) || explicit_dir_keys.count(key) || implicit_dir_keys.count(key))
						return Fail(fmt::format("tar: duplicate or colliding destination {}", path));
					file_keys.insert(key);

					if (*size > TarZstdExtractor::MAX_FILE_BYTES)
						return Fail(fmt::format("tar: payload exceeds {} bytes: {}", TarZstdExtractor::MAX_FILE_BYTES, path));

					if (TextureArchivePath::IsJunkEntry(path) || !TextureArchivePath::IsTextureFile(path))
					{
						if (!Consume(*size) || !SkipPadding(*size))
							return false;
						continue;
					}

					const std::optional<std::string> rel = TextureArchivePath::ReplacementRelativePath(path);
					if (!rel.has_value() || !TextureArchivePath::IsSafeRelativePath(*rel))
						return Fail(fmt::format("tar: cannot place {} in the replacements folder", path));

					// Stripping a wrapper can make two entries land on one name.
					const std::string rel_key = TextureArchivePath::CollisionKey(*rel);
					if (rel_key != key && (file_keys.count(rel_key) || explicit_dir_keys.count(rel_key) || implicit_dir_keys.count(rel_key)))
						return Fail(fmt::format("tar: duplicate normalized destination {}", *rel));
					file_keys.insert(rel_key);

					if (!WriteTexture(*rel, *size) || !SkipPadding(*size))
						return false;

					m_textures++;
					if (m_progress && m_textures % 32 == 0)
						m_progress(m_textures, m_expected.texture_count);
				}

				// The second end block, then nothing but zero padding up to the end of the frame.
				if (!ReadExact(header.data(), RECORD))
					return false;
				if (!IsAllZero(header.data(), RECORD))
					return Fail("tar: missing second end block");

				for (;;)
				{
					if (!CheckCancel())
						return false;
					const size_t got = m_zstd.Read(m_buffer.data(), m_buffer.size());
					if (!IsAllZero(m_buffer.data(), got))
						return Fail("tar: nonzero trailing padding");
					if (m_zstd.Failed())
						return Fail(m_zstd.Error());
					if (got < m_buffer.size())
						break;
				}

				if (m_zstd.Produced() != m_expected.decompressed_bytes)
				{
					return Fail(fmt::format("tar: decompressed size mismatch (stream {}, catalog {})",
						m_zstd.Produced(), m_expected.decompressed_bytes));
				}
				if (m_textures != m_expected.texture_count)
				{
					return Fail(fmt::format("tar: texture count mismatch (stream {}, catalog {})",
						m_textures, m_expected.texture_count));
				}

				if (m_progress)
					m_progress(m_textures, m_expected.texture_count);
				return true;
			}

			ZstdFrameReader& m_zstd;
			const std::string& m_dest;
			const TarZstdExpectations m_expected;
			const ExtractProgress& m_progress;
			const CancelToken& m_cancel;
			std::vector<u8> m_buffer;
			std::unordered_set<std::string> m_made_directories;
			u64 m_textures = 0;
			ExtractResult m_result;
		};

		ExtractResult Run(CompressedSource& source, const std::string& dest_dir, const TarZstdExpectations& expected,
			const ExtractProgress& progress, const CancelToken& cancel)
		{
			ExtractResult failed;
			if (expected.decompressed_bytes == 0 || expected.decompressed_bytes > TarZstdExtractor::MAX_DECOMPRESSED_BYTES)
			{
				failed.error = "The pack declares an unusable decompressed size.";
				return failed;
			}

			Error error;
			if (!FileSystem::EnsureDirectoryExists(dest_dir.c_str(), true, &error))
			{
				failed.error = fmt::format("Could not create {}: {}", dest_dir, error.GetDescription());
				return failed;
			}

			ZstdFrameReader zstd(source, expected.decompressed_bytes);
			if (zstd.Failed())
			{
				failed.error = zstd.Error();
				return failed;
			}

			TarExtraction extraction(zstd, dest_dir, expected, progress, cancel);
			return extraction.Run();
		}
	} // namespace

	ExtractResult TarZstdExtractor::ExtractFile(const std::string& archive_path, const std::string& dest_dir,
		const TarZstdExpectations& expected, const ExtractProgress& progress, const CancelToken& cancel)
	{
		Error error;
		FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(archive_path.c_str(), "rb", &error);
		if (!fp)
		{
			ExtractResult failed;
			failed.error = fmt::format("Could not open the downloaded archive: {}", error.GetDescription());
			return failed;
		}

		FileSource source(fp.get());
		return Run(source, dest_dir, expected, progress, cancel);
	}

	ExtractResult TarZstdExtractor::ExtractMemory(std::span<const u8> archive, const std::string& dest_dir,
		const TarZstdExpectations& expected, const ExtractProgress& progress, const CancelToken& cancel)
	{
		MemorySource source(archive);
		return Run(source, dest_dir, expected, progress, cancel);
	}
} // namespace Catalog
