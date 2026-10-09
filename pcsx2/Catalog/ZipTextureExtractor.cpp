// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/ZipTextureExtractor.h"

#include "Catalog/FileUtil.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ZipHelpers.h"

#include "fmt/format.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace Catalog
{
	namespace
	{
		struct Candidate
		{
			zip_uint64_t index;
			u64 size;
			std::string rel;
		};
	} // namespace

	ExtractResult ZipTextureExtractor::ExtractFile(const std::string& archive_path, const std::string& dest_dir,
		const ExtractProgress& progress, const CancelToken& cancel, const FreeSpaceQuery& free_space)
	{
		ExtractResult result;
		const auto fail = [&result](std::string why) {
			result.status = ExtractStatus::Failed;
			result.error = std::move(why);
			return result;
		};

		zip_error_t ze = {};
		const auto zip = zip_open_managed(archive_path.c_str(), ZIP_RDONLY, &ze);
		if (!zip)
			return fail(fmt::format("Archive could not be read: {}", zip_error_strerror(&ze)));

		const zip_int64_t entry_count = zip_get_num_entries(zip.get(), 0);
		if (entry_count < 0 || static_cast<u64>(entry_count) > MAX_ENTRIES)
			return fail(fmt::format("zip: entry count exceeds {}", MAX_ENTRIES));

		// First pass: decide what is written, and refuse the whole archive on anything unsafe or
		// ambiguous before a byte of it reaches the disk.
		std::vector<Candidate> candidates;
		std::unordered_set<std::string> keys;
		u64 total_bytes = 0; // what the zip declares for the files it will write
		for (zip_int64_t i = 0; i < entry_count; i++)
		{
			zip_stat_t st;
			zip_stat_init(&st);
			if (zip_stat_index(zip.get(), static_cast<zip_uint64_t>(i), 0, &st) != 0 || !st.name)
				return fail("zip: could not read an entry's name");

			std::string name = st.name;
			std::replace(name.begin(), name.end(), '\\', '/');
			if (name.empty() || name.back() == '/')
				continue; // folder
			if (TextureArchivePath::IsJunkEntry(name) || !TextureArchivePath::IsTextureFile(name))
				continue;

			// Leading slashes are dropped, not refused: the result is relative to the destination
			// either way. ".." and the rest of the path rules still apply.
			const size_t first = name.find_first_not_of('/');
			const std::optional<std::string> checked = TextureArchivePath::ValidateTarPath(
				first == std::string::npos ? std::string_view() : std::string_view(name).substr(first));
			if (!checked.has_value() || checked->empty())
				return fail("Archive contained an unsafe path");

			const std::optional<std::string> rel = TextureArchivePath::ReplacementRelativePath(*checked);
			if (!rel.has_value() || !TextureArchivePath::IsSafeRelativePath(*rel))
				return fail("Archive contained an unsafe path");

			if (!(st.valid & ZIP_STAT_SIZE))
				return fail(fmt::format("zip: {} has no size", *rel));
			if (st.size > MAX_FILE_BYTES)
				return fail(fmt::format("zip: payload exceeds {} bytes: {}", MAX_FILE_BYTES, *rel));

			if (!keys.insert(TextureArchivePath::CollisionKey(*rel)).second)
				return fail(fmt::format("Archive contains duplicate textures ({})", *rel));

			// Each entry is at most MAX_FILE_BYTES and there are at most MAX_ENTRIES, so this cannot overflow.
			total_bytes += st.size;
			if (total_bytes > MAX_TOTAL_BYTES)
				return fail(fmt::format("zip: unpacked size exceeds {} GB", MAX_TOTAL_BYTES / (1024ull * 1024 * 1024)));

			candidates.push_back({static_cast<zip_uint64_t>(i), st.size, *rel});
		}

		Error error;
		if (!FileSystem::EnsureDirectoryExists(dest_dir.c_str(), true, &error))
			return fail(fmt::format("Could not create {}: {}", dest_dir, error.GetDescription()));

		// Each file is held to its declared size as it is written, so the sum is the most this can take. An
		// unknown or zero answer skips the check, as the installer's does: a filesystem that cannot say is
		// better tried than refused.
		const std::optional<u64> available = free_space ? free_space(dest_dir) : FreeSpaceOn(dest_dir);
		if (available.has_value() && *available > 0 && *available < total_bytes + FREE_SPACE_MARGIN)
		{
			return fail(fmt::format("Needs ~{} MB free to unpack, {} MB available", (total_bytes + FREE_SPACE_MARGIN) / 1024 / 1024,
				*available / 1024 / 1024));
		}

		std::unordered_set<std::string> made_directories;
		std::vector<u8> buffer(256 * 1024);
		const u64 total = candidates.size();
		for (const Candidate& c : candidates)
		{
			if (cancel.IsCancelled())
			{
				result.status = ExtractStatus::Cancelled;
				return result;
			}

			const std::string out_path = Path::Combine(dest_dir, c.rel);
			const std::string directory(Path::GetDirectory(out_path));
			if (made_directories.insert(directory).second && !FileSystem::EnsureDirectoryExists(directory.c_str(), true, &error))
				return fail(fmt::format("Could not create a folder for {}: {}", c.rel, error.GetDescription()));

			const auto source = zip_fopen_index_managed(zip.get(), c.index, 0);
			if (!source)
				return fail(fmt::format("zip: could not open {}", c.rel));

			FileSystem::ManagedCFilePtr out = FileSystem::OpenManagedCFile(out_path.c_str(), "wb", &error);
			if (!out)
				return fail(fmt::format("Could not write {}: {}", c.rel, error.GetDescription()));

			u64 written = 0;
			for (;;)
			{
				if (cancel.IsCancelled())
				{
					result.status = ExtractStatus::Cancelled;
					return result;
				}

				const zip_int64_t got = zip_fread(source.get(), buffer.data(), buffer.size());
				if (got < 0)
					return fail(fmt::format("zip: {} could not be read ({})", c.rel, zip_file_strerror(source.get())));
				if (got == 0)
					break;
				written += static_cast<u64>(got);
				if (written > c.size)
					return fail(fmt::format("zip: {} is larger than it declared", c.rel));
				if (std::fwrite(buffer.data(), 1, static_cast<size_t>(got), out.get()) != static_cast<size_t>(got))
					return fail(fmt::format("Could not write {} (disk full?)", c.rel));
			}
			if (written != c.size)
				return fail(fmt::format("zip: {} is shorter than it declared", c.rel));

			std::FILE* raw = out.release();
			if (std::fclose(raw) != 0)
				return fail(fmt::format("Could not finish writing {} (disk full?)", c.rel));

			result.textures++;
			if (progress && (result.textures % 32 == 0 || result.textures == total))
				progress(result.textures, total);
		}

		result.status = ExtractStatus::Ok;
		return result;
	}
} // namespace Catalog
