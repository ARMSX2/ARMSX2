// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// A small, defensive .tar.gz extractor for downloaded packs we do not control (GPU driver
// packs today). It reads the gzip stream with zlib and the tar stream by hand, so there is no
// archive library to keep in step and every policy decision is visible in one place.
//
// What it does with an entry:
//   regular file    written under the destination directory (mode 0644, or 0755 if the archive
//                   gave it any execute bit)
//   directory       created
//   symlink, hard link, device, FIFO
//                   skipped and counted, never created. Not creating links is what makes the
//                   path checks below sufficient: nothing extracted can redirect a later write.
//
// What it refuses (the whole extraction fails, nothing is partially trusted): an absolute path,
// a ".." component, a NUL or backslash in a name, a path over 4096 bytes / 32 components /
// 255 bytes per component, a bad tar header checksum, an archive that ends mid-entry, a file
// over TarLimits::max_file_bytes, more entries or more total bytes than the limits, and a gzip
// stream that is corrupt. Reading stops at the end of the first gzip member.
//
// GNU long names (type 'L'), POSIX pax headers (type 'x', `path` and `size` records) and the
// ustar `prefix` field are understood, because GNU tar and Python's tarfile both produce them.

#include "common/Pcsx2Types.h"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace Catalog
{
	struct TarLimits
	{
		/// Entries of any kind, including headers that only carry a long name.
		u32 max_entries = 10000;
		/// Largest single regular file.
		u64 max_file_bytes = 256ull << 20;
		/// Everything the gzip stream expands to. Bounds a decompression bomb.
		u64 max_total_bytes = 1024ull << 20;
	};

	enum class TarResult
	{
		Ok,
		Cancelled,
		Failed, ///< `error` says why.
	};

	struct TarExtractInfo
	{
		/// Regular files written, as normalised relative paths with '/' separators.
		std::vector<std::string> files;
		/// Entries that were not regular files or directories and were therefore skipped.
		u32 skipped_entries = 0;
		/// Bytes of file data written.
		u64 bytes_written = 0;
	};

	/// Extracts `archive_path` into `dest_dir` (created if needed). `cancel`, if given, is polled
	/// between chunks. On failure or cancellation files already written stay on disk: extract
	/// into a staging directory and delete it.
	TarResult ExtractTarGz(const std::string& archive_path, const std::string& dest_dir, const TarLimits& limits,
		TarExtractInfo* info, std::string* error, const std::atomic_bool* cancel = nullptr);

	/// The path policy above, on its own. On success `out` is the path with "." components and
	/// repeated or leading "./" removed, '/' separated, no leading or trailing '/'. A name that
	/// normalises to nothing (".", "./") is not an error; `out` comes back empty.
	bool NormalizeArchivePath(std::string_view name, std::string* out, std::string* error = nullptr);
} // namespace Catalog
