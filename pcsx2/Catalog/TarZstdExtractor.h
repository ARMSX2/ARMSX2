// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Unpacks the texture packs of the online catalog (schema 2, format "tar+zstd") straight from the
// compressed file into a folder, with no uncompressed tar ever on disk. It accepts exactly the
// profile the pack converter publishes and refuses everything else, because the bytes come from
// the network:
//
//   zstd: one frame, window log at most 27, no dictionary, nothing after the frame; the output
//         may not exceed the size the catalog declared.
//   tar:  GNU tar. Directories, regular files and GNU 'L' long-name records. Links, devices,
//         FIFOs, pax headers, sparse files and base-256 numbers are refused. Header checksums,
//         512-byte padding, two end blocks and zero-only trailing padding are verified.
//   paths: TextureArchivePath::ValidateTarPath(); at most 100,000 entries; no file over 512 MiB;
//         two entries that land on the same file (see TextureArchivePath::CollisionKey) are an
//         error, as is a file that is also a folder.
//   files: only .png .dds .astc .ktx are written, under the wrapper-stripped path
//         (TextureArchivePath::ReplacementRelativePath); everything else is read and dropped.
//   totals: the decompressed stream must be exactly `decompressed_bytes` long and hold exactly
//         `texture_count` texture files, the two numbers the catalog entry carries.
//
// The destination is created if it does not exist. On any result but Ok the caller deletes it.

#include "Catalog/CancelToken.h"
#include "Catalog/TextureArchivePath.h"

#include <span>
#include <string>

namespace Catalog
{
	struct TarZstdExpectations
	{
		/// Exact length of the decompressed tar stream (the catalog's decompressedSizeBytes). Also
		/// the most the zstd layer will produce.
		u64 decompressed_bytes = 0;

		/// Exact number of texture files (the catalog's fileCount).
		u64 texture_count = 0;
	};

	class TarZstdExtractor
	{
	public:
		/// Limits from the pack format. Public so the tests and the UI can quote them.
		static constexpr u64 MAX_ENTRIES = 100000;
		static constexpr u64 MAX_FILE_BYTES = 512ull * 1024 * 1024;
		static constexpr int MAX_WINDOW_LOG = 27;
		/// Largest decompressed stream a catalog entry may declare.
		static constexpr u64 MAX_DECOMPRESSED_BYTES = 16ull * 1024 * 1024 * 1024;

		/// Extracts the archive file at `archive_path` into `dest_dir`. Checks the cancel token
		/// between 256 KiB chunks. Safe to call from any worker thread.
		static ExtractResult ExtractFile(const std::string& archive_path, const std::string& dest_dir,
			const TarZstdExpectations& expected, const ExtractProgress& progress = {}, const CancelToken& cancel = {});

		/// The same from memory, for tests and small archives.
		static ExtractResult ExtractMemory(std::span<const u8> archive, const std::string& dest_dir,
			const TarZstdExpectations& expected, const ExtractProgress& progress = {}, const CancelToken& cancel = {});
	};
} // namespace Catalog
