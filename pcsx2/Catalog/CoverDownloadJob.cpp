// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/CoverDownloadJob.h"

#include "Catalog/FileUtil.h"
#include "Catalog/Json.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <cstring>
#include <unordered_set>

namespace Catalog
{
	// A saved back-off further away than this is a clock that was set back, not a real one.
	static constexpr std::chrono::seconds MAX_PLAUSIBLE_BACKOFF = std::chrono::hours(24);

	// New misses between saves of the miss cache, so a long run that is killed loses little.
	static constexpr u32 MISSES_PER_SAVE = 50;

	// ---- CoverMissCache ----------------------------------------------------------------------

	CoverMissCache::CoverMissCache(std::string path, std::chrono::seconds miss_expiry)
		: m_path(std::move(path))
		, m_miss_expiry(miss_expiry)
	{
	}

	bool CoverMissCache::Load(std::time_t now)
	{
		// A memory-only cache has nothing to read and keeps what it holds.
		if (m_path.empty())
			return false;

		m_misses.clear();
		m_backoff_until = 0;
		m_dirty = false;

		const std::optional<std::string> text = FileSystem::ReadFileToString(m_path.c_str());
		if (!text.has_value())
			return false;

		rapidjson::Document doc;
		if (!ParseJson(*text, &doc) || !doc.IsObject())
			return false;

		if (const auto it = doc.FindMember("backoffUntil"); it != doc.MemberEnd() && it->value.IsInt64())
			m_backoff_until = static_cast<std::time_t>(it->value.GetInt64());

		if (const auto it = doc.FindMember("misses"); it != doc.MemberEnd() && it->value.IsObject())
		{
			for (const auto& member : it->value.GetObject())
			{
				if (!member.value.IsInt64())
					continue;
				const std::time_t stamp = static_cast<std::time_t>(member.value.GetInt64());
				if (now >= stamp && std::chrono::seconds(now - stamp) < m_miss_expiry)
					m_misses.emplace(std::string(member.name.GetString(), member.name.GetStringLength()), stamp);
				else
					m_dirty = true; // expired: rewrite the file without it next Save()
			}
		}
		return true;
	}

	bool CoverMissCache::Save()
	{
		if (m_path.empty() || !m_dirty)
			return true;

		rapidjson::Document doc;
		doc.SetObject();
		auto& alloc = doc.GetAllocator();
		doc.AddMember("version", 1, alloc);
		doc.AddMember("backoffUntil", static_cast<int64_t>(m_backoff_until), alloc);

		rapidjson::Value misses(rapidjson::kObjectType);
		for (const auto& [url, stamp] : m_misses)
		{
			misses.AddMember(rapidjson::Value(url.c_str(), static_cast<rapidjson::SizeType>(url.size()), alloc),
				rapidjson::Value(static_cast<int64_t>(stamp)), alloc);
		}
		doc.AddMember("misses", misses, alloc);

		Error error;
		if (!WriteJsonFileAtomic(m_path, doc, &error))
		{
			Console.WarningFmt("Covers: could not save {}: {}", m_path, error.GetDescription());
			return false;
		}
		m_dirty = false;
		return true;
	}

	bool CoverMissCache::IsMissed(const std::string& url, std::time_t now) const
	{
		const auto it = m_misses.find(url);
		if (it == m_misses.end())
			return false;
		return now >= it->second && std::chrono::seconds(now - it->second) < m_miss_expiry;
	}

	void CoverMissCache::RecordMiss(const std::string& url, std::time_t now)
	{
		m_misses[url] = now;
		m_dirty = true;
	}

	void CoverMissCache::ForgetMiss(const std::string& url)
	{
		if (m_misses.erase(url) != 0)
			m_dirty = true;
	}

	bool CoverMissCache::InBackoff(std::time_t now) const
	{
		return m_backoff_until > now && std::chrono::seconds(m_backoff_until - now) <= MAX_PLAUSIBLE_BACKOFF;
	}

	void CoverMissCache::StartBackoff(std::time_t now, std::chrono::seconds duration)
	{
		m_backoff_until = now + static_cast<std::time_t>(duration.count());
		m_dirty = true;
	}

	// ---- Image sniffing ----------------------------------------------------------------------

	std::string ImageExtensionForBytes(const void* data, size_t size)
	{
		const u8* bytes = static_cast<const u8*>(data);
		if (size >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
			return ".jpg";

		static constexpr u8 png_signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
		if (size >= sizeof(png_signature) && std::memcmp(bytes, png_signature, sizeof(png_signature)) == 0)
			return ".png";

		if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0)
			return ".webp";

		return {};
	}

	// ---- The job -----------------------------------------------------------------------------

	namespace
	{
		enum class Attempt
		{
			Saved,
			AlreadyHadCover, ///< Downloaded fine, but a cover appeared meanwhile; the download was dropped.
			Miss, ///< The server has no usable image for this URL.
			RateLimited,
			Failed, ///< No usable answer from the server (transport error, 5xx, disk error).
			Cancelled,
		};

		class Job
		{
		public:
			Job(Fetcher& fetcher, CoverJobOptions& options)
				: m_fetcher(fetcher)
				, m_options(options)
			{
			}

			CoverJobSummary Run(std::vector<CoverWorkItem> items);

		private:
			std::time_t Now() const { return m_options.clock ? m_options.clock() : std::time(nullptr); }
			Attempt TryUrl(const CoverWorkItem& item, const std::string& url, CoverSaved* saved);
			void Report(u32 done, const std::string& title);
			void SaveMissCache();

			Fetcher& m_fetcher;
			CoverJobOptions& m_options;
			CoverJobSummary m_summary;
			u32 m_total = 0;
			u32 m_unsaved_misses = 0;
		};

		void Job::Report(u32 done, const std::string& title)
		{
			if (m_options.on_progress)
			{
				CoverProgress progress;
				progress.done = done;
				progress.total = m_total;
				progress.current_title = title;
				m_options.on_progress(progress);
			}
		}

		void Job::SaveMissCache()
		{
			if (m_options.miss_cache)
				m_options.miss_cache->Save();
			m_unsaved_misses = 0;
		}

		Attempt Job::TryUrl(const CoverWorkItem& item, const std::string& url, CoverSaved* saved)
		{
			FetchOptions fetch_options;
			fetch_options.cancel = m_options.cancel;
			fetch_options.headers.push_back({"Accept", "image/*"});
			fetch_options.connect_timeout_seconds = m_options.connect_timeout_seconds;

			m_summary.requests++;
			const FetchResponse response = m_fetcher.Get(url, m_options.max_cover_bytes, fetch_options);

			switch (response.result)
			{
				case FetchResult::Ok:
					break;
				case FetchResult::Cancelled:
					return Attempt::Cancelled;
				case FetchResult::TooLarge:
				case FetchResult::BadUrl:
				case FetchResult::BadRedirect:
					// Retrying cannot change these: remember them like a 404.
					return Attempt::Miss;
				case FetchResult::FileError:
				case FetchResult::Transport:
				default:
					return Attempt::Failed;
			}

			if (response.status == 403 || response.status == 429)
				return Attempt::RateLimited;
			if (response.status == 408 || response.status >= 500)
				return Attempt::Failed;
			if (response.status < 200 || response.status >= 300)
				return Attempt::Miss; // 404 above all

			// A 200 can still be an error page; only real image bytes are saved.
			const std::string extension = ImageExtensionForBytes(response.body.data(), response.body.size());
			if (extension.empty())
				return Attempt::Miss;

			// It may have turned up while we were downloading.
			if (!item.force && m_options.has_cover && m_options.has_cover(item))
				return Attempt::AlreadyHadCover;

			const std::string path = Path::Combine(m_options.covers_dir, item.serial + extension);
			Error error;
			if (!WriteFileAtomic(path, response.body, &error))
			{
				Console.WarningFmt("Covers: could not write {}: {}", path, error.GetDescription());
				return Attempt::Failed;
			}

			if (item.force && !item.replace_path.empty() && item.replace_path != path)
				FileSystem::DeleteFilePath(item.replace_path.c_str());

			saved->id = item.id;
			saved->serial = item.serial;
			saved->path = path;
			saved->url = url;
			return Attempt::Saved;
		}

		CoverJobSummary Job::Run(std::vector<CoverWorkItem> items)
		{
			// One request per serial: two ISOs of one game share a cover file.
			std::vector<CoverWorkItem> work;
			{
				std::unordered_set<std::string> seen;
				for (CoverWorkItem& item : items)
				{
					if (!IsUsableCoverSerial(item.serial) || !seen.insert(item.serial).second)
						continue;
					work.push_back(std::move(item));
				}
			}
			m_total = static_cast<u32>(work.size());
			m_summary.total = m_total;
			if (work.empty())
				return m_summary;

			CoverMissCache* misses = m_options.miss_cache.get();
			if (misses)
			{
				misses->Load(Now());
				if (misses->InBackoff(Now()))
				{
					Console.WriteLnFmt("Covers: backing off after a rate limit, {} more minutes",
						static_cast<long long>((misses->BackoffUntil() - Now()) / 60 + 1));
					m_summary.end = CoverJobEnd::BackedOff;
					return m_summary;
				}
			}

			std::shared_ptr<const CoverRegionIndex> region_index;
			if (m_options.preferences.NeedsRegionIndex() && m_options.load_region_index)
				region_index = m_options.load_region_index();

			u32 done = 0;
			u32 consecutive_failures = 0;
			for (CoverWorkItem& item : work)
			{
				if (m_options.cancel.IsCancelled())
				{
					m_summary.end = CoverJobEnd::Cancelled;
					break;
				}

				Report(done, item.title);

				if (!item.force && m_options.has_cover && m_options.has_cover(item))
				{
					m_summary.already_had_cover++;
					done++;
					continue;
				}

				const CoverUrls urls = ResolveCoverUrls(item.platform, item.serial, m_options.preferences, region_index.get());
				std::vector<std::string> candidates;
				if (urls.Valid())
					candidates.push_back(urls.primary);
				if (!urls.fallback.empty())
					candidates.push_back(urls.fallback);

				bool asked = false;
				bool missed = false;
				bool downloaded = false;
				bool had_cover = false;
				bool failed = false;
				bool stop = false;

				for (const std::string& url : candidates)
				{
					if (!item.force && misses && misses->IsMissed(url, Now()))
						continue;

					asked = true;
					CoverSaved saved;
					const Attempt attempt = TryUrl(item, url, &saved);
					if (attempt == Attempt::Saved)
					{
						downloaded = true;
						if (misses)
							misses->ForgetMiss(url);
						if (m_options.on_cover_saved)
							m_options.on_cover_saved(saved);
						break;
					}
					else if (attempt == Attempt::AlreadyHadCover)
					{
						had_cover = true;
						break;
					}
					else if (attempt == Attempt::Miss)
					{
						missed = true;
						if (misses)
						{
							misses->RecordMiss(url, Now());
							m_unsaved_misses++;
						}
						continue; // the disc's own serial is next
					}
					else if (attempt == Attempt::Failed)
					{
						failed = true;
						break;
					}
					else if (attempt == Attempt::RateLimited)
					{
						m_summary.end = CoverJobEnd::RateLimited;
						if (misses)
							misses->StartBackoff(Now());
						Console.WarningFmt("Covers: the server refused with a rate limit; pausing downloads for {} minutes",
							static_cast<long long>(CoverMissCache::DEFAULT_BACKOFF.count() / 60));
						stop = true;
						break;
					}
					else // Cancelled
					{
						m_summary.end = CoverJobEnd::Cancelled;
						stop = true;
						break;
					}
				}

				if (stop)
					break;

				if (downloaded)
					m_summary.downloaded++;
				else if (had_cover)
					m_summary.already_had_cover++;
				else if (failed)
					m_summary.failed++;
				else if (missed)
					m_summary.missed++;
				else if (!asked)
					m_summary.skipped_known_miss++;

				// A 404 is an answer from the server, so the network is up.
				if (failed)
				{
					consecutive_failures++;
				}
				else if (asked)
				{
					consecutive_failures = 0;
				}

				done++;

				if (m_unsaved_misses >= MISSES_PER_SAVE)
					SaveMissCache();

				if (consecutive_failures >= m_options.max_consecutive_failures)
				{
					Console.WarningFmt("Covers: {} downloads in a row got no answer; giving up for now", consecutive_failures);
					m_summary.end = CoverJobEnd::NetworkDown;
					break;
				}
			}

			SaveMissCache();
			Report(done, std::string());

			Console.WriteLnFmt("Covers: {} saved, {} already had one, {} known missing, {} not found, {} failed ({} requests)",
				m_summary.downloaded, m_summary.already_had_cover, m_summary.skipped_known_miss, m_summary.missed, m_summary.failed,
				m_summary.requests);
			return m_summary;
		}
	} // namespace

	CoverJobSummary RunCoverDownloads(Fetcher& fetcher, std::vector<CoverWorkItem> items, CoverJobOptions options)
	{
		Job job(fetcher, options);
		return job.Run(std::move(items));
	}

	// ---- Runner ------------------------------------------------------------------------------

	CoverJobRunner::~CoverJobRunner()
	{
		Shutdown();
	}

	bool CoverJobRunner::Start(Fetcher& fetcher, std::vector<CoverWorkItem> items, CoverJobOptions options, std::function<void(const CoverJobSummary&)> on_finished)
	{
		std::lock_guard lock(m_mutex);
		if (m_running.load(std::memory_order_acquire))
			return false;
		if (m_thread.joinable())
			m_thread.join(); // the previous run finished; collect it

		{
			std::lock_guard token_lock(m_token_mutex);
			m_cancel = CancelToken();
			options.cancel = m_cancel;
		}

		m_running.store(true, std::memory_order_release);
		m_thread = std::thread([this, &fetcher, items = std::move(items), options = std::move(options), on_finished = std::move(on_finished)]() mutable {
			const CoverJobSummary summary = RunCoverDownloads(fetcher, std::move(items), std::move(options));
			if (on_finished)
				on_finished(summary);
			m_running.store(false, std::memory_order_release);
		});
		return true;
	}

	void CoverJobRunner::Cancel()
	{
		std::lock_guard token_lock(m_token_mutex);
		m_cancel.Cancel();
	}

	void CoverJobRunner::Shutdown()
	{
		Cancel();
		std::lock_guard lock(m_mutex);
		Cancel(); // a Start() that slipped in before we got the lock
		if (m_thread.joinable())
			m_thread.join();
	}
} // namespace Catalog
