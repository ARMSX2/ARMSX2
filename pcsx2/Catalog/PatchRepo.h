// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The online patch and cheat browser: finds a game's .pnach files in community GitHub
// repositories and installs the sections the player picks. Port of the Android app's PatchRepo.kt,
// shared by the Qt and Big Picture UIs.
//
// What a UI does
//
//   Catalog::PatchRepo repo(Catalog::GetDefaultFetcher());      // keep one for the process
//
//   Catalog::PatchSearchRequest request;
//   request.serial = entry->serial;
//   request.crc = entry->crc;                                   // from the game list entry
//   auto job = repo.StartSearch(request, on_progress, on_done); // callbacks run on the worker
//   ...                                                         // job->Cancel() to stop
//
//   Catalog::PatchInstallRequest install;                       // later, on the player's OK
//   install.serial = ...; install.crc = ...; install.game_title = result.game_title;
//   install.entries = the chosen PatchEntry values from result.entries;
//   Catalog::PatchInstallResult done = repo.Install(install);
//
// Where things are looked up (the same list as Android, checked against PatchRepo.kt):
//
//   patches   PCSX2/pcsx2_patches        patches/<SERIAL>_<CRC>.pnach, patches/<CRC>.pnach
//             Gabominated/PCSX2          PCSX2 Patches/<SERIAL>_<CRC>.pnach, <CRC>.pnach
//   cheats    shadowninja826/pcsx2_pnach_cheats   any <CRC>.pnach in the repository
//             xs1l3n7x/pcsx2_cheats_collection    any .pnach (they are all under cheats/)
//   "large cheat database" (searched only on request, PatchSearchScope::LargeCheatDatabase)
//             XiGuanChi/PCSX2-CheatsDB   only cheats-v1/, cheats-v2/ and cheats-v6/. The repository
//             also holds copies of the other four sources (patches-v2, patches-v3, "PCSX2 Patches",
//             cheats-v5, ...); they are left out so a widescreen patch is not offered a second time
//             as a cheat.
//
// Patch files are fetched by their exact name. Cheat repositories name files inconsistently, so
// their file lists come from the GitHub trees API, are kept on disk as plain path lists for 7
// days, and a game's files are those whose name contains its CRC (or, if none does, its serial).
// GitHub allows 60 unauthenticated API calls an hour per address, so a list that failed is not
// asked for again for 60 minutes after a 403 or 429 and 5 minutes after anything else; while that
// wait lasts an older cached copy of the list, if there is one, is used instead.
//
// Threading: Search and Install block. StartSearch runs Search on a worker thread of its own and
// calls its callbacks on that thread, so a UI marshals them to its own thread (Qt signals,
// MTGS::RunOnGSThread). Install does file I/O only (no network) and may be called from any thread
// except the GS thread. Neither takes Host::GetSettingsLock(): Install edits the per-game settings
// file directly, and the core's reload it asks for runs later on the CPU thread.

#include "Catalog/CancelToken.h"
#include "Catalog/Fetch.h"
#include "Catalog/ResponseCache.h"

#include "common/Pcsx2Types.h"

#include <atomic>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Catalog
{
	enum class PatchSourceId : u8
	{
		Pcsx2Patches, ///< PCSX2/pcsx2_patches: widescreen and fixes, no cheats.
		Gabominated, ///< Gabominated/PCSX2: extra improvement patches (no interlace, 60 FPS, no blur).
		Shadowninja826, ///< shadowninja826/pcsx2_pnach_cheats
		Xs1l3n7x, ///< xs1l3n7x/pcsx2_cheats_collection
		XiGuanChi, ///< XiGuanChi/PCSX2-CheatsDB, the large cheat database.
	};

	/// Decides where an entry installs: patches go to EmuFolders::Patches and `[Patches] Enable`,
	/// cheats to EmuFolders::Cheats and `[Cheats] Enable`. The core applies patches always and
	/// cheats only with EnableCheats on and never in hardcore mode.
	enum class PatchKind : u8
	{
		Patch,
		Cheat,
	};

	/// "PCSX2 patches", "Gabominated", ... for the chip next to a result.
	const char* PatchSourceLabel(PatchSourceId source);

	/// "PCSX2/pcsx2_patches", ... as owner/repository.
	const char* PatchSourceRepository(PatchSourceId source);

	/// One `[Section]` found online.
	struct PatchEntry
	{
		PatchSourceId source = PatchSourceId::Pcsx2Patches;
		PatchKind kind = PatchKind::Patch;

		/// Where it came from in the repository, e.g. "patches/SLUS-20946_2C6BE434.pnach".
		std::string file_path;

		/// The section name. It is what the player sees and what Install puts in the Enable list.
		std::string name;

		/// First `comment=` of the section, else its first `description=`. May be empty.
		std::string description;

		/// The section text Install writes. Pass entries back to Install unchanged.
		std::string body;

		/// True when the game's own .pnach file (the one Install writes) already holds a section
		/// of this name: patches are compared by exact name, cheats ignoring case and spacing.
		/// Installing it again only makes sure it is in the Enable list.
		bool installed = false;
	};

	enum class PatchSearchScope : u8
	{
		/// The two patch repositories and the two cheat repositories, in that order.
		Community,

		/// Only the large cheat database. Its file list is several MB, which is why it is a
		/// separate request: the UI offers it as a button after a Community search.
		LargeCheatDatabase,
	};

	struct PatchSearchRequest
	{
		/// "SLUS-20946". Case and surrounding spaces are fixed up; letters, digits and '-' only.
		/// May be empty if `crc` is set.
		std::string serial;

		/// The disc CRC, taken from the game list entry. 0 means unknown, and then the search goes
		/// by serial alone (looking the game up in the repositories' file lists, which costs more
		/// API calls) and Install will refuse the results; see PatchSearchResult::crc.
		u32 crc = 0;

		PatchSearchScope scope = PatchSearchScope::Community;

		/// LargeCheatDatabase only: the names the UI already lists. Cheats whose name matches one
		/// of these (ignoring case and spacing) are not returned again.
		std::vector<std::string> already_listed;
	};

	enum class PatchSearchStatus : u8
	{
		Ok, ///< `entries` is not empty. See `incomplete`.
		NoMatches, ///< Every source answered and none has this game.
		Unreachable, ///< Nothing found and at least one source could not be read (offline, rate limited).
		Cancelled,
		InvalidRequest, ///< Neither a usable serial nor a CRC.
	};

	struct PatchSearchResult
	{
		PatchSearchStatus status = PatchSearchStatus::NoMatches;

		/// The first `gametitle=` seen. Pass it to Install so a new file gets a title line.
		std::string game_title;

		/// The request's serial, upper-cased.
		std::string serial;

		/// The request's CRC. When the request had none, the CRC in the file name of the matching
		/// patch file, or 0 if it has none. That value is the database's revision of the game and
		/// is only informational: Install needs the CRC of the player's own disc.
		u32 crc = 0;
		bool crc_from_database = false;

		/// Patches first (PCSX2, then Gabominated), then cheats in source order. Names are
		/// de-duplicated: patches by exact name, first source wins; cheats by PnachText.h's
		/// CheatNameKey across all sources, first wins.
		std::vector<PatchEntry> entries;

		/// Some source failed even though others answered, so the list may be missing entries.
		bool incomplete = false;

		/// Hardcore (achievements) mode is on right now: the core ignores every cheat and any
		/// patch that is not a presentation tweak while it is. The UI should say so next to the
		/// cheats instead of letting the player install them unaware. See also
		/// PatchRepo::IsHardcoreActive().
		bool hardcore_active = false;
	};

	struct PatchSearchProgress
	{
		std::string message; ///< "Searching Gabominated", for a label or the progress bar text.
		u32 done = 0; ///< Sources finished. Equal to `total` in the last call.
		u32 total = 0; ///< 4 for Community, 1 for LargeCheatDatabase.
	};

	using PatchSearchProgressFn = std::function<void(const PatchSearchProgress&)>;
	using PatchSearchDoneFn = std::function<void(PatchSearchResult result)>;

	/// A search running on its own thread. Destroying it cancels the search and waits for the
	/// thread (when destroyed from inside one of its own callbacks it cancels and lets the thread
	/// run out instead). The PatchRepo that started it must outlive the thread.
	class PatchSearchJob
	{
	public:
		~PatchSearchJob();
		PatchSearchJob(const PatchSearchJob&) = delete;
		PatchSearchJob& operator=(const PatchSearchJob&) = delete;

		/// Asks the search to stop. It does so at its next network call boundary; `on_done` is
		/// still called, with PatchSearchStatus::Cancelled.
		void Cancel();

		/// True once `on_done` has returned.
		bool IsFinished() const { return m_state->finished.load(std::memory_order_acquire); }

		/// Blocks until the thread has ended.
		void Wait();

	private:
		friend class PatchRepo;
		PatchSearchJob() = default;

		// Shared with the thread, so the job can be destroyed from inside its own callback (the
		// thread is then detached) without the thread touching freed memory.
		struct State
		{
			CancelToken cancel;
			std::atomic_bool finished{false};
		};

		std::shared_ptr<State> m_state = std::make_shared<State>();
		std::thread m_thread;
	};

	struct PatchInstallRequest
	{
		/// The game's serial and the CRC of the player's disc, from the game list entry. The
		/// files are named <serial>_<CRC>.pnach, the only name the core loads patches under.
		std::string serial;
		u32 crc = 0;

		/// Written as the `gametitle=` line when the .pnach file is created.
		std::string game_title;

		/// The sections to install, as returned by a search. Patch entries and cheat entries may
		/// be mixed. Entries without a name or body are ignored.
		std::vector<PatchEntry> entries;
	};

	enum class PatchInstallStatus : u8
	{
		Ok,
		NothingSelected, ///< No usable entries.
		CrcUnknown, ///< `crc` is 0. Nothing was written: <serial>_00000000.pnach would never load.
		InvalidSerial, ///< Characters other than letters, digits and '-'.
		WriteFailed, ///< A .pnach file or the settings file could not be written; see `error`.
	};

	struct PatchInstallResult
	{
		PatchInstallStatus status = PatchInstallStatus::NothingSelected;
		std::string error; ///< For a UI to show when `status` is WriteFailed.

		u32 patches_added = 0; ///< Sections newly written to the patches .pnach file.
		u32 cheats_added = 0; ///< Sections newly written to the cheats .pnach file.
		u32 already_present = 0; ///< Chosen sections the file already had (left alone, still enabled).

		std::vector<std::string> files_written; ///< The .pnach files created or extended.
		std::string settings_file; ///< The per-game settings file the Enable lists went into.

		/// `[EmuCore] EnableCheats` was set in the game's settings because cheats were chosen.
		bool cheats_switched_on = false;

		/// The game is running and the core was told to reload its patches, so they are live
		/// already. Otherwise they apply the next time it boots.
		bool reload_requested = false;

		/// Hardcore mode is on: the cheats just installed will not apply while it is. See
		/// PatchSearchResult::hardcore_active.
		bool hardcore_active = false;
	};

	/// Everything that touches the clock or the running emulator, as replaceable functions so
	/// tests need no VM. A default-constructed PatchRepoHooks does the real thing.
	struct PatchRepoHooks
	{
		/// Seconds since the epoch. Cache ages and back-off waits are judged against it.
		std::function<std::time_t()> now;

		/// Is this game the one running right now?
		std::function<bool(std::string_view serial, u32 crc)> is_game_running;

		/// Makes the running game re-read its settings file and pnach files. The default posts to
		/// the CPU thread: VMManager::ReloadGameSettings(), then VMManager::ReloadPatches().
		std::function<void()> reload_patches;

		/// Achievements::IsHardcoreModeActive().
		std::function<bool()> is_hardcore_active;
	};

	class PatchRepo
	{
	public:
		/// `fetcher` must outlive the PatchRepo. File lists are cached in <Cache>/patch-trees/.
		explicit PatchRepo(Fetcher& fetcher, PatchRepoHooks hooks = {});

		/// As above with the list cache in a folder of the caller's choosing.
		PatchRepo(Fetcher& fetcher, ResponseCache tree_cache, PatchRepoHooks hooks = {});

		~PatchRepo();
		PatchRepo(const PatchRepo&) = delete;
		PatchRepo& operator=(const PatchRepo&) = delete;

		/// Searches on the calling thread. `progress` may be empty. Cancelling `cancel` ends the
		/// search at its next network call with PatchSearchStatus::Cancelled.
		PatchSearchResult Search(const PatchSearchRequest& request, const CancelToken& cancel = {}, const PatchSearchProgressFn& progress = {});

		/// Search() on a new thread. `on_done` is called exactly once, on that thread, also when
		/// the search was cancelled.
		std::unique_ptr<PatchSearchJob> StartSearch(PatchSearchRequest request, PatchSearchProgressFn progress, PatchSearchDoneFn on_done);

		/// Writes the chosen sections into the game's .pnach files (Patch::GetPnachFilename),
		/// adds their names to the game's settings file, switches EnableCheats on for cheats and,
		/// if the game is running, makes the core reload. Blocking, no network.
		///
		/// Sections the .pnach file already has are not written again, and the rest are appended
		/// without touching what the file holds. Refuses (CrcUnknown) when the CRC is 0.
		///
		/// The settings file is edited on disk. A settings dialog that has the same file open
		/// holds its own copy and must reload it afterwards, or its next Save() would drop the
		/// names added here.
		///
		/// Installing while hardcore mode is on is allowed; the result says it is on.
		PatchInstallResult Install(const PatchInstallRequest& request);

		/// Sets PatchEntry::installed for every entry from the game's current .pnach files, so a
		/// UI can refresh its list after an install without searching again. Does nothing if
		/// `crc` is 0.
		void UpdateInstalledFlags(std::vector<PatchEntry>& entries, std::string_view serial, u32 crc) const;

		/// Achievements hardcore mode is on: cheats do not apply (see PatchSearchResult).
		bool IsHardcoreActive() const;

	private:
		struct SearchContext;
		struct MemoryTree
		{
			std::vector<std::string> paths; ///< Already limited to the source's folders.
			std::time_t fetched_at = 0;
		};

		enum class TreeStatus
		{
			Ok,
			Failed,
			Cancelled,
		};

		enum class FileStatus
		{
			Found,
			NotFound,
			Failed,
			Cancelled,
		};

		std::time_t Now() const;
		TreeStatus LoadTree(PatchSourceId source, const CancelToken& cancel, std::vector<std::string>* paths);
		void RecordTreeFailure(const std::string& url, s32 status, std::time_t now);
		FileStatus FetchPnach(PatchSourceId source, const std::string& repo_path, SearchContext& context, std::string* text);
		void AddSections(PatchSourceId source, const std::string& repo_path, const std::string& text, SearchContext& context);
		void SearchPatchSource(PatchSourceId source, SearchContext& context);
		void SearchCheatSource(PatchSourceId source, SearchContext& context);

		Fetcher& m_fetcher;
		ResponseCache m_tree_cache;
		PatchRepoHooks m_hooks;

		// m_load_mutex is held for the whole of LoadTree() so two searches do not both spend API
		// calls on the same list; m_mutex only guards the maps below, briefly.
		std::mutex m_load_mutex;
		mutable std::mutex m_mutex;
		std::map<PatchSourceId, MemoryTree> m_trees;
		std::map<std::string, std::time_t> m_retry_after; ///< file list URL -> not before.
	};

	// ---- Pieces of the above that are plain functions, exposed so they can be tested. ----

	/// Cheat files in `tree` (repository paths) that belong to a game: those whose file name
	/// contains `crc` (8 hex digits, any case), or if none does, those named "<SERIAL>_..." or
	/// ending " <SERIAL>.pnach" (the large database's "<CRC> - <title> <SERIAL>.pnach"). `crc` or
	/// `serial` may be empty. Matching ignores case and looks at the file name, not the folders.
	std::vector<std::string> MatchCheatFiles(const std::vector<std::string>& tree, std::string_view crc, std::string_view serial);

	/// A repository path as the path part of a raw.githubusercontent.com URL: each segment
	/// percent-encoded as UTF-8, '/' kept. File names carry game titles, so '#', '%', spaces and
	/// non-ASCII letters all occur.
	std::string RawGithubPath(std::string_view path);

	/// The .pnach file paths in a GitHub trees API response body (`{"tree":[{"path":...,
	/// "type":"blob"},...]}`). Only blobs ending in ".pnach". Returns false if `json` is not such
	/// a response.
	bool ExtractPnachPaths(std::string_view json, std::vector<std::string>* paths, bool* truncated = nullptr);
} // namespace Catalog
