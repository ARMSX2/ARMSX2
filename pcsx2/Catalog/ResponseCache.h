// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

class Error;

namespace Catalog
{
	/// A folder of cached HTTP responses, one file per name, e.g. the texture catalog at
	/// <Cache>/texture-catalog/catalog.json. The body is stored as received (so the file is
	/// the JSON itself) and the time it was fetched is the file's modification time: there is
	/// no sidecar to fall out of step, and every Store() is atomic.
	///
	/// Freshness is the caller's policy: IsFresh() tests the age and Load() returns the entry
	/// however old it is, so a stale copy can serve as the offline fallback.
	class ResponseCache
	{
	public:
		struct Entry
		{
			std::string body;
			std::time_t fetched_at = 0;
		};

		explicit ResponseCache(std::string directory);

		/// The cache folder EmuFolders::Cache/<subdir>.
		static ResponseCache InCacheFolder(std::string_view subdir);

		const std::string& Directory() const { return m_directory; }
		std::string PathFor(std::string_view name) const;

		/// `name` must be a plain file name (no directory separators, not "." or ".."); other
		/// names are rejected by every call below.
		std::optional<Entry> Load(std::string_view name) const;
		std::optional<std::time_t> FetchedAt(std::string_view name) const;

		/// True if the entry exists and was fetched less than `max_age` before `now`. An entry
		/// stamped in the future (clock set back, e.g. a handheld without a battery-backed
		/// clock) counts as not fresh, so it is refetched rather than trusted for years.
		bool IsFresh(std::string_view name, std::chrono::seconds max_age, std::time_t now = std::time(nullptr)) const;

		/// Atomically replaces the entry; its fetch time becomes now.
		bool Store(std::string_view name, std::string_view body, Error* error = nullptr) const;

		void Remove(std::string_view name) const;

	private:
		std::string m_directory;
	};
} // namespace Catalog
