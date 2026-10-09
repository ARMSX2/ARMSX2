// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The schema-1 catalog format: a plain zip of a pack, extracted with libzip. The live catalog has
// no zip entries any more, so this is the small compatibility path, with the same path policy as
// the tar+zstd extractor (TextureArchivePath.h): only .png .dds .astc .ktx are written, wrapper
// folders are stripped, a path with ".." or too many or too long components is refused, and two
// entries that land on one file are an error. At most 100,000 entries, no file over 512 MiB.
//
// Unlike tar+zstd there is no exact file count to check against, so the extractor reports how many
// textures it wrote and the caller decides what an empty result means.

#include "Catalog/CancelToken.h"
#include "Catalog/TextureArchivePath.h"

#include <string>

namespace Catalog
{
	class ZipTextureExtractor
	{
	public:
		static constexpr u64 MAX_ENTRIES = 100000;
		static constexpr u64 MAX_FILE_BYTES = 512ull * 1024 * 1024;

		/// Extracts the zip at `archive_path` into `dest_dir` (created if missing). The progress
		/// total is the number of texture entries in the zip. On any result but Ok the caller
		/// deletes `dest_dir`.
		static ExtractResult ExtractFile(const std::string& archive_path, const std::string& dest_dir,
			const ExtractProgress& progress = {}, const CancelToken& cancel = {});
	};
} // namespace Catalog
