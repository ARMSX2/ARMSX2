// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Extracts a schema-1 catalog pack: a plain zip, read with libzip. It applies the same path policy
// as the tar+zstd extractor (TextureArchivePath.h): only .png .dds .astc .ktx are written, wrapper
// folders are stripped, a path with ".." or too many or too long components is refused, and two
// entries that land on one file are an error. At most 100,000 entries, no file over 512 MiB.
//
// Unlike tar+zstd there is no exact file count to check against, so the extractor reports how many
// textures it wrote and the caller decides what an empty result means.

#include "Catalog/CancelToken.h"
#include "Catalog/TextureArchivePath.h"

#include <functional>
#include <optional>
#include <string>

namespace Catalog
{
	class ZipTextureExtractor
	{
	public:
		static constexpr u64 MAX_ENTRIES = 100000;
		static constexpr u64 MAX_FILE_BYTES = 512ull * 1024 * 1024;
		/// The sizes the zip declares for its textures, added up, may not pass this: a zip of a hundred
		/// thousand small entries can still unpack to terabytes.
		static constexpr u64 MAX_TOTAL_BYTES = 16ull * 1024 * 1024 * 1024;
		/// Kept free on the destination's filesystem beyond what the textures take.
		static constexpr u64 FREE_SPACE_MARGIN = 64ull * 1024 * 1024;

		/// Free space, in bytes, on the filesystem holding `path`; nullopt or 0 if unknown (the check
		/// is then skipped). Empty asks the operating system.
		using FreeSpaceQuery = std::function<std::optional<u64>(const std::string& path)>;

		/// Extracts the zip at `archive_path` into `dest_dir` (created if missing). The progress
		/// total is the number of texture entries in the zip. On any result but Ok the caller
		/// deletes `dest_dir`.
		///
		/// Before a byte is written the sizes the zip declares for its textures are added up: over
		/// MAX_TOTAL_BYTES is refused, and the sum plus FREE_SPACE_MARGIN must fit in the free space
		/// of `dest_dir`'s filesystem.
		static ExtractResult ExtractFile(const std::string& archive_path, const std::string& dest_dir,
			const ExtractProgress& progress = {}, const CancelToken& cancel = {}, const FreeSpaceQuery& free_space = {});
	};
} // namespace Catalog
