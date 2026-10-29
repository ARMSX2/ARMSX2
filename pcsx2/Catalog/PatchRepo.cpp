// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/PatchRepo.h"

#include "Catalog/FileUtil.h"
#include "Catalog/Json.h"
#include "Catalog/PnachText.h"

#include "Achievements.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "Patch.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>

namespace Catalog
{
	namespace
	{
		struct SourceDef
		{
			PatchSourceId id;
			const char* label;
			const char* repository;
			PatchKind kind;
			const char* cache_name; ///< Name of the file list in the tree cache folder.

			/// When non-empty, only paths under one of these count. The cache keeps the whole
			/// list, so changing this never needs a new download.
			std::array<const char*, 3> folders;
		};

		constexpr std::array<SourceDef, 5> SOURCES = {{
			{PatchSourceId::Pcsx2Patches, "PCSX2 patches", "PCSX2/pcsx2_patches", PatchKind::Patch, "tree-pcsx2-patches.txt", {"patches/", nullptr, nullptr}},
			{PatchSourceId::Gabominated, "Gabominated", "Gabominated/PCSX2", PatchKind::Patch, "tree-gabominated.txt", {"PCSX2 Patches/", nullptr, nullptr}},
			{PatchSourceId::Shadowninja826, "shadowninja826 cheats", "shadowninja826/pcsx2_pnach_cheats", PatchKind::Cheat, "tree-shadowninja826.txt", {nullptr, nullptr, nullptr}},
			{PatchSourceId::Xs1l3n7x, "xs1l3n7x cheats", "xs1l3n7x/pcsx2_cheats_collection", PatchKind::Cheat, "tree-xs1l3n7x.txt", {nullptr, nullptr, nullptr}},
			{PatchSourceId::XiGuanChi, "Large cheat database", "XiGuanChi/PCSX2-CheatsDB", PatchKind::Cheat, "tree-xiguanchi.txt", {"cheats-v1/", "cheats-v2/", "cheats-v6/"}},
		}};

		const SourceDef& GetSource(PatchSourceId id)
		{
			return SOURCES[static_cast<size_t>(id)];
		}

		constexpr std::time_t TREE_CACHE_TTL_SECONDS = 7 * 24 * 60 * 60;
		constexpr std::time_t RETRY_RATE_LIMITED_SECONDS = 60 * 60; ///< GitHub's allowance resets hourly.
		constexpr std::time_t RETRY_FAILED_SECONDS = 5 * 60;
		constexpr size_t MAX_TREE_BYTES = 48u * 1024 * 1024;
		constexpr size_t MAX_PNACH_BYTES = 4u * 1024 * 1024;
		constexpr size_t MAX_SERIAL_LENGTH = 24;

		std::string TreeUrl(const SourceDef& source)
		{
			return fmt::format("https://api.github.com/repos/{}/git/trees/main?recursive=1", source.repository);
		}

		std::string RawUrl(const SourceDef& source, const std::string& repo_path)
		{
			return fmt::format("https://raw.githubusercontent.com/{}/main/{}", source.repository, RawGithubPath(repo_path));
		}

		char UpperAscii(char c)
		{
			return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
		}

		std::string UpperAscii(std::string_view s)
		{
			std::string out(s);
			for (char& c : out)
				c = UpperAscii(c);
			return out;
		}

		std::string_view TrimAscii(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
				s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
				s.remove_suffix(1);
			return s;
		}

		/// Upper-cased, letters digits and '-' only. Serials end up in URLs and file names.
		bool NormalizeSerial(std::string_view in, std::string* out)
		{
			const std::string_view trimmed = TrimAscii(in);
			if (trimmed.size() > MAX_SERIAL_LENGTH)
				return false;
			out->clear();
			for (const char c : trimmed)
			{
				const char u = UpperAscii(c);
				if (!((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-'))
					return false;
				out->push_back(u);
			}
			return true;
		}

		std::string_view BaseName(std::string_view path)
		{
			const size_t slash = path.rfind('/');
			return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
		}

		bool StartsWithFolders(const std::string& path, const std::array<const char*, 3>& folders)
		{
			bool any = false;
			for (const char* folder : folders)
			{
				if (!folder)
					continue;
				any = true;
				if (path.compare(0, std::char_traits<char>::length(folder), folder) == 0)
					return true;
			}
			return !any;
		}

		std::vector<std::string> LimitToFolders(const std::vector<std::string>& paths, const SourceDef& source)
		{
			std::vector<std::string> out;
			out.reserve(paths.size());
			for (const std::string& path : paths)
			{
				if (StartsWithFolders(path, source.folders))
					out.push_back(path);
			}
			return out;
		}

		std::vector<std::string> SplitLines(std::string_view text)
		{
			std::vector<std::string> lines;
			size_t pos = 0;
			while (pos < text.size())
			{
				size_t end = text.find('\n', pos);
				if (end == std::string_view::npos)
					end = text.size();
				const std::string_view line = TrimAscii(text.substr(pos, end - pos));
				if (!line.empty())
					lines.emplace_back(line);
				pos = end + 1;
			}
			return lines;
		}

		/// The 8 hex digits after "<serial>_" in a database file name, or 0.
		u32 CrcFromFileName(std::string_view path, std::string_view serial)
		{
			std::string_view name = BaseName(path);
			if (name.size() > 6 && StringUtil::EndsWithNoCase(name, ".pnach"))
				name.remove_suffix(6);

			const std::string prefix = fmt::format("{}_", serial);
			if (!StringUtil::StartsWithNoCase(name, prefix))
				return 0;
			name.remove_prefix(prefix.size());

			const size_t underscore = name.find('_');
			if (underscore != std::string_view::npos)
				name = name.substr(0, underscore);
			if (name.size() != 8)
				return 0;
			return StringUtil::FromChars<u32>(name, 16).value_or(0);
		}

		const char* PnachSectionForKind(PatchKind kind)
		{
			return (kind == PatchKind::Cheat) ? Patch::CHEATS_CONFIG_SECTION : Patch::PATCHES_CONFIG_SECTION;
		}

		/// The per-game settings file the running game would read: the serial-named one, or the
		/// older <CRC>.ini when only that exists (VMManager::UpdateGameSettingsLayer does the same).
		std::string ResolveGameSettingsPath(const std::string& serial, u32 crc)
		{
			std::string path = VMManager::GetGameSettingsPath(serial, crc);
			if (!serial.empty() && !FileSystem::FileExists(path.c_str()))
			{
				std::string legacy = VMManager::GetGameSettingsPath(std::string_view(), crc);
				if (FileSystem::FileExists(legacy.c_str()))
					return legacy;
			}
			return path;
		}

		bool SectionIsPresent(PatchKind kind, const std::string& name, const std::vector<PnachSection>& sections)
		{
			if (kind == PatchKind::Cheat)
			{
				const std::string key = CheatNameKey(name);
				return std::any_of(sections.begin(), sections.end(),
					[&key](const PnachSection& s) { return CheatNameKey(s.name) == key; });
			}
			return std::any_of(sections.begin(), sections.end(), [&name](const PnachSection& s) { return s.name == name; });
		}

		void DefaultReloadPatches()
		{
			Host::RunOnCPUThread([]() {
				// The game can end between the check that queued this and now.
				if (!VMManager::HasValidVM())
					return;
				// Re-read the settings file we just edited (installs the game layer if the game
				// booted without one), then pnach files and Enable lists.
				VMManager::ReloadGameSettings();
				VMManager::ReloadPatches(true, true, true, true);
			});
		}

		bool DefaultIsGameRunning(std::string_view serial, u32 crc)
		{
			if (!VMManager::HasValidVM())
				return false;
			if (!serial.empty() && UpperAscii(VMManager::GetDiscSerial()) != serial)
				return false;
			return crc == VMManager::GetDiscCRC() || crc == VMManager::GetCurrentCRC();
		}
	} // namespace

	const char* PatchSourceLabel(PatchSourceId source)
	{
		return GetSource(source).label;
	}

	const char* PatchSourceRepository(PatchSourceId source)
	{
		return GetSource(source).repository;
	}

	std::vector<std::string> MatchCheatFiles(const std::vector<std::string>& tree, std::string_view crc, std::string_view serial)
	{
		const std::string c = UpperAscii(crc);
		const std::string s = UpperAscii(serial);

		std::vector<std::string> by_crc;
		if (!c.empty())
		{
			for (const std::string& path : tree)
			{
				if (UpperAscii(BaseName(path)).find(c) != std::string::npos)
					by_crc.push_back(path);
			}
		}
		if (!by_crc.empty() || s.empty())
			return by_crc;

		const std::string prefix = s + "_";
		const std::string suffix = " " + s + ".PNACH";
		std::vector<std::string> by_serial;
		for (const std::string& path : tree)
		{
			const std::string base = UpperAscii(BaseName(path));
			const bool starts = base.compare(0, prefix.size(), prefix) == 0;
			const bool ends = base.size() >= suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0;
			if (starts || ends)
				by_serial.push_back(path);
		}
		return by_serial;
	}

	std::string RawGithubPath(std::string_view path)
	{
		static constexpr char HEX[] = "0123456789ABCDEF";

		std::string out;
		out.reserve(path.size() + 16);
		for (const char ch : path)
		{
			const unsigned char c = static_cast<unsigned char>(ch);
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
				c == '.' || c == '~' || c == '/')
			{
				out.push_back(ch);
			}
			else
			{
				out.push_back('%');
				out.push_back(HEX[c >> 4]);
				out.push_back(HEX[c & 15]);
			}
		}
		return out;
	}

	bool ExtractPnachPaths(std::string_view json, std::vector<std::string>* paths, bool* truncated)
	{
		paths->clear();
		if (truncated)
			*truncated = false;

		rapidjson::Document doc;
		if (!ParseJson(json, &doc) || !doc.IsObject())
			return false;

		const auto tree = doc.FindMember("tree");
		if (tree == doc.MemberEnd() || !tree->value.IsArray())
			return false;

		if (truncated)
		{
			const auto flag = doc.FindMember("truncated");
			*truncated = (flag != doc.MemberEnd() && flag->value.IsBool() && flag->value.GetBool());
		}

		paths->reserve(tree->value.Size());
		for (const rapidjson::Value& item : tree->value.GetArray())
		{
			if (!item.IsObject())
				continue;
			const auto type = item.FindMember("type");
			const auto path = item.FindMember("path");
			if (type == item.MemberEnd() || !type->value.IsString() || std::string_view(type->value.GetString()) != "blob")
				continue;
			if (path == item.MemberEnd() || !path->value.IsString())
				continue;

			const std::string_view p(path->value.GetString(), path->value.GetStringLength());
			// The cache stores one path per line.
			if (!StringUtil::EndsWithNoCase(p, ".pnach") || p.find_first_of("\r\n") != std::string_view::npos)
				continue;
			paths->emplace_back(p);
		}
		return true;
	}

	PatchSearchJob::~PatchSearchJob()
	{
		Cancel();
		if (!m_thread.joinable())
			return;
		if (m_thread.get_id() == std::this_thread::get_id())
			m_thread.detach();
		else
			m_thread.join();
	}

	void PatchSearchJob::Cancel()
	{
		m_state->cancel.Cancel();
	}

	void PatchSearchJob::Wait()
	{
		if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
			m_thread.join();
	}

	struct PatchRepo::SearchContext
	{
		const CancelToken& cancel;
		std::string serial;
		u32 crc = 0;
		std::string crc_text; ///< "%08X", empty when the CRC is unknown.

		PatchSearchResult result;
		std::unordered_set<std::string> patch_names;
		std::unordered_set<std::string> cheat_keys;

		u32 failures = 0; ///< Lists or files that could not be read (a 404 is not one).
		bool cancelled = false;

		explicit SearchContext(const CancelToken& token)
			: cancel(token)
		{
		}

		bool ShouldStop()
		{
			if (cancel.IsCancelled())
				cancelled = true;
			return cancelled;
		}
	};

	PatchRepo::PatchRepo(Fetcher& fetcher, PatchRepoHooks hooks)
		: PatchRepo(fetcher, ResponseCache::InCacheFolder("patch-trees"), std::move(hooks))
	{
	}

	PatchRepo::PatchRepo(Fetcher& fetcher, ResponseCache tree_cache, PatchRepoHooks hooks)
		: m_fetcher(fetcher)
		, m_tree_cache(std::move(tree_cache))
		, m_hooks(std::move(hooks))
	{
		if (!m_hooks.now)
			m_hooks.now = []() { return std::time(nullptr); };
		if (!m_hooks.is_game_running)
			m_hooks.is_game_running = &DefaultIsGameRunning;
		if (!m_hooks.reload_patches)
			m_hooks.reload_patches = &DefaultReloadPatches;
		if (!m_hooks.is_hardcore_active)
			m_hooks.is_hardcore_active = []() { return Achievements::IsHardcoreModeActive(); };
	}

	PatchRepo::~PatchRepo() = default;

	std::time_t PatchRepo::Now() const
	{
		return m_hooks.now();
	}

	bool PatchRepo::IsHardcoreActive() const
	{
		return m_hooks.is_hardcore_active();
	}

	void PatchRepo::RecordTreeFailure(const std::string& url, s32 status, std::time_t now)
	{
		const std::time_t wait = (status == 403 || status == 429) ? RETRY_RATE_LIMITED_SECONDS : RETRY_FAILED_SECONDS;
		Console.WarningFmt("PatchRepo: file list {} failed (status {}), not asked for again for {} min", url, status, wait / 60);

		std::lock_guard lock(m_mutex);
		m_retry_after[url] = now + wait;
	}

	PatchRepo::TreeStatus PatchRepo::LoadTree(PatchSourceId id, const CancelToken& cancel, std::vector<std::string>* paths)
	{
		const SourceDef& source = GetSource(id);
		const std::string url = TreeUrl(source);

		std::lock_guard load_lock(m_load_mutex);
		const std::time_t now = Now();

		{
			std::lock_guard lock(m_mutex);
			const auto it = m_trees.find(id);
			if (it != m_trees.end() && now >= it->second.fetched_at && now - it->second.fetched_at < TREE_CACHE_TTL_SECONDS)
			{
				*paths = it->second.paths;
				return TreeStatus::Ok;
			}
		}

		const auto remember = [&](const std::vector<std::string>& all, std::time_t fetched_at) {
			*paths = LimitToFolders(all, source);
			std::lock_guard lock(m_mutex);
			m_trees[id] = MemoryTree{*paths, fetched_at};
		};

		if (m_tree_cache.IsFresh(source.cache_name, std::chrono::seconds(TREE_CACHE_TTL_SECONDS), now))
		{
			if (const std::optional<ResponseCache::Entry> entry = m_tree_cache.Load(source.cache_name))
			{
				const std::vector<std::string> all = SplitLines(entry->body);
				if (!all.empty())
				{
					remember(all, entry->fetched_at);
					return TreeStatus::Ok;
				}
			}
		}

		bool backing_off = false;
		{
			std::lock_guard lock(m_mutex);
			const auto it = m_retry_after.find(url);
			backing_off = (it != m_retry_after.end() && now < it->second);
		}
		if (!backing_off)
		{
			FetchOptions options;
			options.cancel = cancel;
			options.headers = {{"Accept", "application/vnd.github+json"}};
			const FetchResponse response = m_fetcher.Get(url, MAX_TREE_BYTES, options);

			if (response.result == FetchResult::Cancelled)
				return TreeStatus::Cancelled;

			std::vector<std::string> all;
			bool truncated = false;
			if (response.IsSuccess() && ExtractPnachPaths(response.body, &all, &truncated) && !all.empty())
			{
				if (truncated)
					Console.WarningFmt("PatchRepo: GitHub cut the file list of {} short; some files will not be found", source.repository);

				{
					std::lock_guard lock(m_mutex);
					m_retry_after.erase(url);
				}

				std::string joined;
				for (const std::string& path : all)
				{
					joined.append(path);
					joined.push_back('\n');
				}
				m_tree_cache.Store(source.cache_name, joined);

				remember(all, now);
				return TreeStatus::Ok;
			}

			RecordTreeFailure(url, response.result == FetchResult::Ok ? response.status : 0, now);
		}

		// Failed or backing off: a stale list beats none. The cost is that a new file shows up late.
		if (const std::optional<ResponseCache::Entry> entry = m_tree_cache.Load(source.cache_name))
		{
			const std::vector<std::string> all = SplitLines(entry->body);
			if (!all.empty())
			{
				*paths = LimitToFolders(all, source);
				return TreeStatus::Ok;
			}
		}
		return TreeStatus::Failed;
	}

	PatchRepo::FileStatus PatchRepo::FetchPnach(PatchSourceId id, const std::string& repo_path, SearchContext& context, std::string* text)
	{
		const SourceDef& source = GetSource(id);
		const std::string url = RawUrl(source, repo_path);

		FetchOptions options;
		options.cancel = context.cancel;
		const FetchResponse response = m_fetcher.Get(url, MAX_PNACH_BYTES, options);

		if (response.result == FetchResult::Cancelled)
		{
			context.cancelled = true;
			return FileStatus::Cancelled;
		}
		if (response.result == FetchResult::Ok && response.status == 404)
			return FileStatus::NotFound;
		if (response.IsSuccess() && !response.body.empty())
		{
			*text = response.body;
			return FileStatus::Found;
		}

		Console.WarningFmt("PatchRepo: {} failed (result {}, status {})", url, static_cast<int>(response.result), response.status);
		context.failures++;
		return FileStatus::Failed;
	}

	void PatchRepo::AddSections(PatchSourceId id, const std::string& repo_path, const std::string& text, SearchContext& context)
	{
		const SourceDef& source = GetSource(id);
		PnachFile file = ParsePnach(text);

		if (context.result.game_title.empty())
			context.result.game_title = file.game_title;

		for (PnachSection& section : file.sections)
		{
			const bool is_new = (source.kind == PatchKind::Cheat) ? context.cheat_keys.insert(CheatNameKey(section.name)).second :
			                                                        context.patch_names.insert(section.name).second;
			if (!is_new)
				continue;

			PatchEntry entry;
			entry.source = id;
			entry.kind = source.kind;
			entry.file_path = repo_path;
			entry.name = std::move(section.name);
			entry.description = std::move(section.description);
			entry.body = std::move(section.body);
			context.result.entries.push_back(std::move(entry));
		}
	}

	void PatchRepo::SearchPatchSource(PatchSourceId id, SearchContext& context)
	{
		const SourceDef& source = GetSource(id);
		const std::string folder = source.folders[0];

		if (context.crc != 0)
		{
			// The exact file, so no file list is needed: <SERIAL>_<CRC> first, then <CRC>. The first
			// one that exists wins.
			std::vector<std::string> candidates;
			if (!context.serial.empty())
				candidates.push_back(fmt::format("{}{}_{}.pnach", folder, context.serial, context.crc_text));
			candidates.push_back(fmt::format("{}{}.pnach", folder, context.crc_text));

			for (const std::string& candidate : candidates)
			{
				if (context.ShouldStop())
					return;
				std::string text;
				const FileStatus status = FetchPnach(id, candidate, context, &text);
				if (status == FileStatus::Found)
				{
					AddSections(id, candidate, text, context);
					return;
				}
			}
			return;
		}

		// No CRC: find the serial's file in the repository's file list. If it has several
		// revisions, the first in the list is the one used.
		std::vector<std::string> tree;
		const TreeStatus tree_status = LoadTree(id, context.cancel, &tree);
		if (tree_status == TreeStatus::Cancelled)
		{
			context.cancelled = true;
			return;
		}
		if (tree_status == TreeStatus::Failed)
		{
			context.failures++;
			return;
		}

		const std::string prefix = context.serial + "_";
		for (const std::string& path : tree)
		{
			if (!StringUtil::StartsWithNoCase(BaseName(path), prefix))
				continue;

			if (context.ShouldStop())
				return;
			std::string text;
			if (FetchPnach(id, path, context, &text) == FileStatus::Found)
			{
				AddSections(id, path, text, context);
				if (context.result.crc == 0)
				{
					context.result.crc = CrcFromFileName(path, context.serial);
					context.result.crc_from_database = (context.result.crc != 0);
				}
			}
			return;
		}
	}

	void PatchRepo::SearchCheatSource(PatchSourceId id, SearchContext& context)
	{
		std::vector<std::string> tree;
		const TreeStatus tree_status = LoadTree(id, context.cancel, &tree);
		if (tree_status == TreeStatus::Cancelled)
		{
			context.cancelled = true;
			return;
		}
		if (tree_status == TreeStatus::Failed)
		{
			context.failures++;
			return;
		}

		for (const std::string& path : MatchCheatFiles(tree, context.crc_text, context.serial))
		{
			if (context.ShouldStop())
				return;
			std::string text;
			if (FetchPnach(id, path, context, &text) == FileStatus::Found)
				AddSections(id, path, text, context);
		}
	}

	PatchSearchResult PatchRepo::Search(const PatchSearchRequest& request, const CancelToken& cancel, const PatchSearchProgressFn& progress)
	{
		SearchContext context(cancel);
		PatchSearchResult& result = context.result;
		result.hardcore_active = IsHardcoreActive();

		if (!NormalizeSerial(request.serial, &context.serial) || (context.serial.empty() && request.crc == 0))
		{
			result.status = PatchSearchStatus::InvalidRequest;
			return result;
		}
		context.crc = request.crc;
		if (request.crc != 0)
			context.crc_text = fmt::format("{:08X}", request.crc);
		result.serial = context.serial;
		result.crc = request.crc;

		std::vector<PatchSourceId> sources;
		if (request.scope == PatchSearchScope::LargeCheatDatabase)
		{
			sources = {PatchSourceId::XiGuanChi};
			for (const std::string& name : request.already_listed)
				context.cheat_keys.insert(CheatNameKey(name));
		}
		else
		{
			sources = {PatchSourceId::Pcsx2Patches, PatchSourceId::Gabominated, PatchSourceId::Shadowninja826, PatchSourceId::Xs1l3n7x};
		}

		const u32 total = static_cast<u32>(sources.size());
		const auto report = [&](u32 done, std::string message) {
			if (progress)
				progress(PatchSearchProgress{std::move(message), done, total});
		};

		for (u32 i = 0; i < total && !context.ShouldStop(); i++)
		{
			const PatchSourceId id = sources[i];
			report(i, fmt::format("Searching {}", GetSource(id).label));

			if (GetSource(id).kind == PatchKind::Patch)
				SearchPatchSource(id, context);
			else
				SearchCheatSource(id, context);
		}

		if (context.ShouldStop())
		{
			result.entries.clear();
			result.status = PatchSearchStatus::Cancelled;
			return result;
		}
		report(total, "Done");

		UpdateInstalledFlags(result.entries, context.serial, context.crc);

		if (!result.entries.empty())
		{
			result.status = PatchSearchStatus::Ok;
			result.incomplete = (context.failures > 0);
		}
		else
		{
			result.status = (context.failures > 0) ? PatchSearchStatus::Unreachable : PatchSearchStatus::NoMatches;
		}
		return result;
	}

	std::unique_ptr<PatchSearchJob> PatchRepo::StartSearch(PatchSearchRequest request, PatchSearchProgressFn progress, PatchSearchDoneFn on_done)
	{
		std::unique_ptr<PatchSearchJob> job(new PatchSearchJob());
		job->m_thread = std::thread([this, state = job->m_state, request = std::move(request), progress = std::move(progress),
										on_done = std::move(on_done)]() mutable {
			PatchSearchResult result = Search(request, state->cancel, progress);
			if (on_done)
				on_done(std::move(result));
			state->finished.store(true, std::memory_order_release);
		});
		return job;
	}

	void PatchRepo::UpdateInstalledFlags(std::vector<PatchEntry>& entries, std::string_view serial, u32 crc) const
	{
		if (crc == 0)
			return;

		std::string normalized;
		if (!NormalizeSerial(serial, &normalized))
			return;

		for (const PatchKind kind : {PatchKind::Patch, PatchKind::Cheat})
		{
			const std::optional<std::string> text =
				FileSystem::ReadFileToString(Patch::GetPnachFilename(normalized, crc, kind == PatchKind::Cheat).c_str());
			const std::vector<PnachSection> sections = text.has_value() ? ParsePnach(*text).sections : std::vector<PnachSection>();
			for (PatchEntry& entry : entries)
			{
				if (entry.kind == kind)
					entry.installed = SectionIsPresent(kind, entry.name, sections);
			}
		}
	}

	PatchInstallResult PatchRepo::Install(const PatchInstallRequest& request)
	{
		PatchInstallResult result;
		result.hardcore_active = IsHardcoreActive();

		std::string serial;
		if (!NormalizeSerial(request.serial, &serial))
		{
			result.status = PatchInstallStatus::InvalidSerial;
			return result;
		}
		if (request.crc == 0)
		{
			result.status = PatchInstallStatus::CrcUnknown;
			result.error = "The disc CRC is not known, so the file could not be named in a way the emulator loads.";
			return result;
		}

		std::vector<const PatchEntry*> chosen[2]; // indexed by PatchKind
		for (const PatchEntry& entry : request.entries)
		{
			if (TrimAscii(entry.name).empty() || TrimAscii(entry.body).empty())
				continue;
			chosen[static_cast<size_t>(entry.kind)].push_back(&entry);
		}
		if (chosen[0].empty() && chosen[1].empty())
		{
			result.status = PatchInstallStatus::NothingSelected;
			return result;
		}

		const auto fail = [&result](std::string message) {
			result.status = PatchInstallStatus::WriteFailed;
			result.error = std::move(message);
			return result;
		};

		// Read the settings file before writing anything, so a file we cannot make sense of stops
		// the install instead of being half applied.
		result.settings_file = ResolveGameSettingsPath(serial, request.crc);
		INISettingsInterface settings(result.settings_file);
		if (FileSystem::FileExists(result.settings_file.c_str()) && !settings.Load())
			return fail(fmt::format("Could not read the game settings file {}.", result.settings_file));

		for (const PatchKind kind : {PatchKind::Patch, PatchKind::Cheat})
		{
			const std::vector<const PatchEntry*>& entries = chosen[static_cast<size_t>(kind)];
			if (entries.empty())
				continue;

			const std::string path = Patch::GetPnachFilename(serial, request.crc, kind == PatchKind::Cheat);

			std::string existing;
			if (FileSystem::FileExists(path.c_str()))
			{
				const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
				if (!text.has_value())
					return fail(fmt::format("Could not read {}.", path));
				existing = *text;
			}

			// Sections already in the file, plus the ones this request adds as it goes, so two
			// chosen entries of one name are written once.
			std::vector<PnachSection> known = ParsePnach(existing).sections;
			std::vector<std::string> new_bodies;
			std::vector<std::string> enable_names;
			for (const PatchEntry* entry : entries)
			{
				const std::string name(TrimAscii(entry->name));
				if (std::find(enable_names.begin(), enable_names.end(), name) == enable_names.end())
					enable_names.push_back(name);

				if (SectionIsPresent(kind, name, known))
				{
					result.already_present++;
					continue;
				}
				PnachSection added;
				added.name = name;
				known.push_back(std::move(added));
				new_bodies.push_back(entry->body);
			}

			if (!new_bodies.empty())
			{
				const std::string text = ComposePnach(existing, request.game_title, new_bodies);
				Error error;
				if (!WriteFileAtomic(path, text, &error))
					return fail(fmt::format("Could not write {}: {}", path, error.GetDescription()));

				result.files_written.push_back(path);
				(kind == PatchKind::Cheat ? result.cheats_added : result.patches_added) += static_cast<u32>(new_bodies.size());
			}

			const char* section = PnachSectionForKind(kind);
			for (const std::string& name : enable_names)
			{
				settings.AddToStringList(section, Patch::PATCH_ENABLE_CONFIG_KEY, name.c_str());
				// Picking a patch in the browser is a decision to use it, so a Disable entry for the
				// same name (the widescreen toggle writes one) must not cancel it.
				if (kind == PatchKind::Patch)
					settings.RemoveFromStringList(section, Patch::PATCH_DISABLE_CONFIG_KEY, name.c_str());
			}
		}

		if (!chosen[static_cast<size_t>(PatchKind::Cheat)].empty())
		{
			// Cheats are off unless EnableCheats is set; patches have no such switch.
			settings.SetBoolValue("EmuCore", "EnableCheats", true);
			result.cheats_switched_on = true;
		}

		if (settings.IsDirty())
		{
			FileSystem::CreateDirectoryPath(std::string(Path::GetDirectory(result.settings_file)).c_str(), true);
			Error error;
			if (!settings.Save(&error))
				return fail(fmt::format("Could not save {}: {}", result.settings_file, error.GetDescription()));
		}

		if (m_hooks.is_game_running(serial, request.crc))
		{
			m_hooks.reload_patches();
			result.reload_requested = true;
		}

		result.status = PatchInstallStatus::Ok;
		return result;
	}
} // namespace Catalog
