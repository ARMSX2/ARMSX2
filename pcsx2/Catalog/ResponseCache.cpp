// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/ResponseCache.h"

#include "Catalog/FileUtil.h"

#include "Config.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

namespace Catalog
{
	static bool IsPlainFileName(std::string_view name)
	{
		return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string_view::npos &&
		       name.find('\0') == std::string_view::npos;
	}

	ResponseCache::ResponseCache(std::string directory)
		: m_directory(std::move(directory))
	{
	}

	ResponseCache ResponseCache::InCacheFolder(std::string_view subdir)
	{
		return ResponseCache(Path::Combine(EmuFolders::Cache, subdir));
	}

	std::string ResponseCache::PathFor(std::string_view name) const
	{
		return Path::Combine(m_directory, name);
	}

	std::optional<ResponseCache::Entry> ResponseCache::Load(std::string_view name) const
	{
		if (!IsPlainFileName(name))
			return std::nullopt;

		const std::string path = PathFor(name);
		const std::optional<std::time_t> fetched_at = FileSystem::GetFileTimestamp(path.c_str());
		std::optional<std::string> body = FileSystem::ReadFileToString(path.c_str());
		if (!fetched_at.has_value() || !body.has_value())
			return std::nullopt;

		Entry entry;
		entry.body = std::move(*body);
		entry.fetched_at = *fetched_at;
		return entry;
	}

	std::optional<std::time_t> ResponseCache::FetchedAt(std::string_view name) const
	{
		if (!IsPlainFileName(name))
			return std::nullopt;
		return FileSystem::GetFileTimestamp(PathFor(name).c_str());
	}

	bool ResponseCache::IsFresh(std::string_view name, std::chrono::seconds max_age, std::time_t now) const
	{
		const std::optional<std::time_t> fetched_at = FetchedAt(name);
		if (!fetched_at.has_value() || now < *fetched_at)
			return false;
		return std::chrono::seconds(now - *fetched_at) < max_age;
	}

	bool ResponseCache::Store(std::string_view name, std::string_view body, Error* error) const
	{
		if (!IsPlainFileName(name))
		{
			Error::SetStringView(error, "Invalid cache entry name.");
			return false;
		}
		return WriteFileAtomic(PathFor(name), body, error);
	}

	void ResponseCache::Remove(std::string_view name) const
	{
		if (IsPlainFileName(name))
			FileSystem::DeleteFilePath(PathFor(name).c_str());
	}
} // namespace Catalog
