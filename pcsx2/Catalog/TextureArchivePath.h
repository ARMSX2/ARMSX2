// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The path policy shared by the tar+zstd and zip extractors (TarZstdExtractor.h,
// ZipTextureExtractor.h): which archive entries are accepted, which files of a pack are textures,
// and how the wrapper folders that packs are published in are stripped so that the files land
// directly in <Textures>/<SERIAL>/replacements/, where GSTextureReplacements scans.

#include "common/Pcsx2Types.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Catalog
{
	enum class ExtractStatus
	{
		Ok,
		Cancelled, ///< The CancelToken fired; whatever was written so far is the caller's to delete.
		Failed, ///< The archive is unusable; ExtractResult::error says why.
	};

	/// What an extractor (TarZstdExtractor, ZipTextureExtractor) reports. Both write into a
	/// destination folder the caller owns and deletes on anything but Ok.
	struct ExtractResult
	{
		ExtractStatus status = ExtractStatus::Failed;
		std::string error; ///< Human-readable cause when status is Failed.
		u64 textures = 0; ///< Texture files written.

		bool IsOk() const { return status == ExtractStatus::Ok; }
	};

	/// (textures written so far, textures expected). Called from the extracting thread about every
	/// 32 files and once at the end.
	using ExtractProgress = std::function<void(u64 done, u64 total)>;
} // namespace Catalog

namespace Catalog::TextureArchivePath
{
	/// A tar header path longer than this many bytes is refused.
	static constexpr size_t MAX_PATH_BYTES = 4096;
	/// A path with more components than this is refused.
	static constexpr size_t MAX_COMPONENTS = 32;
	/// A component longer than this many bytes (UTF-8) is refused.
	static constexpr size_t MAX_COMPONENT_BYTES = 255;

	/// Checks one path exactly as a tar header spelled it (UTF-8). A leading "./" and one trailing
	/// "/" (how tar spells a directory) are removed. Returns the cleaned path, "" for the
	/// archive's own root entry ("./"), or nullopt when the entry breaks the contract and the
	/// whole archive has to be refused: absolute paths, ".." or "." or empty components ("a//b"),
	/// a NUL byte, more than MAX_COMPONENTS components, a component over MAX_COMPONENT_BYTES, a
	/// path over MAX_PATH_BYTES.
	std::optional<std::string> ValidateTarPath(std::string_view raw);

	/// True for a path that can be joined onto a destination folder without leaving it: no empty,
	/// "." or ".." component and nothing that is absolute (also "C:" style components on Windows).
	/// Applied to the result of ReplacementRelativePath(), since stripping a wrapper can expose
	/// components that were harmless where they stood (a backslash turns into a separator).
	bool IsSafeRelativePath(std::string_view path);

	/// The key two entries are compared by to decide that they would land on the same file: the
	/// path lowercased, with accented Latin letters (Latin-1 Supplement and Latin Extended-A)
	/// split into base letter plus combining mark so that the precomposed and decomposed
	/// spellings of one name agree. Other scripts fold ASCII case only. Case and Unicode
	/// insensitive filesystems (macOS, Windows) would merge such entries, and failing the same way
	/// on every platform beats overwriting a file silently on one of them.
	std::string CollisionKey(std::string_view path);

	/// The engine loads only PNG, DDS, raw ASTC and ASTC KTX (GSTextureReplacementLoaders.cpp);
	/// everything else in a pack is metadata and is skipped.
	bool IsTextureFile(std::string_view name);

	/// __MACOSX/ folders, .DS_Store, Thumbs.db.
	bool IsJunkEntry(std::string_view name);

	/// Strips the wrapper a pack is published in, so the result is relative to the "replacements"
	/// folder. In order: everything after the last "replacements" component (any case); else
	/// everything after the last component that looks like a serial ("SLUS-21410", "SLUS21410");
	/// else the first component when it looks like a GitHub archive root ("name-<40 hex>"); else
	/// the path unchanged. Backslashes count as separators. Returns nullopt for an empty path.
	std::optional<std::string> ReplacementRelativePath(std::string_view name);
} // namespace Catalog::TextureArchivePath
