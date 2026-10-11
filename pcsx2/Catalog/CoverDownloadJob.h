// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The automatic cover download: given a list of games with no cover, fetch one cover at a time
// into the covers folder as <serial>.<ext>, which is where GameList::GetCoverImagePathForEntry()
// looks. Everything that touches the game list, the INI or EmuFolders is in CoverAutoDownload.h;
// this file takes plain values and a Fetcher, so tests drive it with a FakeFetcher.
//
// Rules, in the order they bite:
//  * One download at a time, in the order given.
//  * Before each game `has_cover` is asked again, and once more after the bytes arrived, so a
//    cover that showed up meanwhile (the user ran the manual downloader, dropped a file in) is
//    never overwritten. Same discipline as GameList::DownloadCovers.
//  * A URL that answered 404 (or sent something that is not an image) is remembered on disk for
//    a few days (CoverMissCache), so 500 games without covers do not hit the network 500 times
//    on every launch.
//  * A 403 or 429 stops the run and starts a back-off that is also kept on disk, so the next
//    launch does not walk straight back into the rate limit.
//  * Several transport errors or 5xx replies in a row end the run (offline, DNS down) without
//    recording misses.
//  * Cancellation is checked between games and inside the transfer.

#include "Catalog/CancelToken.h"
#include "Catalog/CoverSources.h"
#include "Catalog/Fetch.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Catalog
{
	/// URLs that answered "not there" and the rate-limit back-off, kept in one small JSON file
	/// (<Cache>/covers/misses.json). Not thread safe: a job owns it for the duration of a run.
	class CoverMissCache
	{
	public:
		static constexpr std::chrono::seconds DEFAULT_MISS_EXPIRY = std::chrono::hours(24 * 5);
		static constexpr std::chrono::seconds DEFAULT_BACKOFF = std::chrono::minutes(60);

		/// `path` empty keeps everything in memory only.
		explicit CoverMissCache(std::string path = {}, std::chrono::seconds miss_expiry = DEFAULT_MISS_EXPIRY);

		/// Reads the file if there is one. Entries older than the expiry are dropped. A missing
		/// or damaged file leaves the cache empty and returns false.
		bool Load(std::time_t now = std::time(nullptr));

		/// Writes the file atomically if anything changed since Load/Save. True if there was
		/// nothing to write or the write worked.
		bool Save();

		/// Misses stamped in the future (clock set back) count as expired.
		bool IsMissed(const std::string& url, std::time_t now) const;
		void RecordMiss(const std::string& url, std::time_t now);
		void ForgetMiss(const std::string& url);
		size_t MissCount() const { return m_misses.size(); }

		bool InBackoff(std::time_t now) const;
		std::time_t BackoffUntil() const { return m_backoff_until; }
		void StartBackoff(std::time_t now, std::chrono::seconds duration = DEFAULT_BACKOFF);

		const std::string& Path() const { return m_path; }

	private:
		std::string m_path;
		std::chrono::seconds m_miss_expiry;
		std::map<std::string, std::time_t> m_misses;
		std::time_t m_backoff_until = 0;
		bool m_dirty = false;
	};

	/// One game that wants a cover.
	struct CoverWorkItem
	{
		std::string id; ///< The caller's handle for the game (the game-list path); echoed back in callbacks.
		std::string serial; ///< The disc's own serial: the cover is saved as <serial>.<ext>.
		std::string title; ///< For progress text only.
		CoverPlatform platform = CoverPlatform::PS2;

		/// Download even if `has_cover` says one exists and even if the URL is in the miss cache.
		/// Used when the user changed the style or region of a game that already has a cover.
		bool force = false;

		/// With `force`: the existing cover file, deleted once the new one is written (when it is
		/// a different file).
		std::string replace_path;
	};

	struct CoverProgress
	{
		u32 done = 0; ///< Games finished (downloaded, skipped or missed).
		u32 total = 0;
		std::string current_title; ///< The game being worked on; empty at the end.
	};

	struct CoverSaved
	{
		std::string id; ///< CoverWorkItem::id
		std::string serial;
		std::string path; ///< The file written, <covers_dir>/<serial>.<ext>.
		std::string url; ///< Where it came from.
	};

	enum class CoverJobEnd
	{
		Completed,
		Cancelled,
		RateLimited, ///< A 403 or 429 arrived; a back-off was recorded.
		BackedOff, ///< A back-off from an earlier run is still running; nothing was requested.
		NetworkDown, ///< Too many transport errors or server errors in a row.
	};

	struct CoverJobSummary
	{
		CoverJobEnd end = CoverJobEnd::Completed;
		u32 total = 0; ///< Games after removing duplicate serials.
		u32 downloaded = 0;
		u32 already_had_cover = 0; ///< has_cover said yes (before or after the download).
		u32 skipped_known_miss = 0; ///< Every URL was in the miss cache; no request made.
		u32 missed = 0; ///< Asked, and every URL answered 404 or sent no image.
		u32 failed = 0; ///< Transport or server errors; not remembered, retried next run.
		u32 requests = 0; ///< HTTP requests made.
	};

	struct CoverJobOptions
	{
		/// Where covers are written (EmuFolders::Covers). Created if missing.
		std::string covers_dir;

		CoverPreferences preferences;

		/// Called once, on the thread running the job, and only if the preferences need the
		/// region index. Returning null means no index: every game uses its disc's serial.
		std::function<std::shared_ptr<const CoverRegionIndex>()> load_region_index;

		/// May be null: then misses and back-offs are not remembered. The job calls Load() itself
		/// and Save() at the end of the run and every few dozen new misses. Shared so a runner's
		/// thread keeps it alive; a test keeps its own copy of the pointer to look inside.
		std::shared_ptr<CoverMissCache> miss_cache;

		CancelToken cancel;

		/// True if the game already has a cover. Called from the job's thread. May be empty.
		std::function<bool(const CoverWorkItem&)> has_cover;

		/// Called from the job's thread; marshal to your own thread before touching UI.
		std::function<void(const CoverProgress&)> on_progress;
		std::function<void(const CoverSaved&)> on_cover_saved;

		/// Unix time; tests substitute their own.
		std::function<std::time_t()> clock;

		/// A body larger than this is not a cover. Real ones are under 1 MB.
		size_t max_cover_bytes = 16 * 1024 * 1024;

		/// This many failed games in a row (no answer from the server) end the run.
		u32 max_consecutive_failures = 3;

		u32 connect_timeout_seconds = 10;
	};

	/// Runs the downloads on the calling thread and returns when done, cancelled or stopped.
	/// Games with the same serial are fetched once. Never throws; every outcome is in the
	/// summary.
	CoverJobSummary RunCoverDownloads(Fetcher& fetcher, std::vector<CoverWorkItem> items, CoverJobOptions options);

	/// The file extension (".jpg", ".png" or ".webp") for image bytes judged by their first
	/// bytes, or an empty string if they are not one of those. The covers folder lookup only
	/// knows these extensions, and a 200 reply carrying an HTML error page must not be saved.
	std::string ImageExtensionForBytes(const void* data, size_t size);

	/// A job on its own thread. One run at a time. The destructor cancels and waits, so a runner
	/// that outlives nothing it uses is safe; hosts should still call Shutdown() explicitly
	/// before tearing down the things the callbacks touch.
	class CoverJobRunner
	{
	public:
		CoverJobRunner() = default;
		~CoverJobRunner();
		CoverJobRunner(const CoverJobRunner&) = delete;
		CoverJobRunner& operator=(const CoverJobRunner&) = delete;

		/// Starts `items` on a worker thread. Returns false (and does nothing) if a run is still
		/// going. `options.cancel` is replaced with the runner's own token. `on_finished` is
		/// called on the worker thread after the last progress call; do not call Start() from
		/// inside it. `fetcher` must outlive the run.
		bool Start(Fetcher& fetcher, std::vector<CoverWorkItem> items, CoverJobOptions options, std::function<void(const CoverJobSummary&)> on_finished = {});

		bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

		/// Asks the running job to stop; returns at once.
		void Cancel();

		/// Cancel() and wait for the thread. Safe to call repeatedly and when idle.
		void Shutdown();

	private:
		std::mutex m_mutex; ///< Serialises Start and Shutdown; held while joining the thread.
		std::mutex m_token_mutex; ///< Guards m_cancel only, so Cancel() never waits on a join.
		std::thread m_thread;
		std::atomic_bool m_running{false};
		CancelToken m_cancel;
	};
} // namespace Catalog
