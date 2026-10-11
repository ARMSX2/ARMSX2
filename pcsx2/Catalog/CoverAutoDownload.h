// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Automatic cover art for the game list: the entry point a UI calls. It reads the settings, picks
// the games that have no cover, and runs CoverDownloadJob on a worker thread, saving each cover
// as <EmuFolders::Covers>/<serial>.<jpg|png|webp> so GameList::GetCoverImagePathForEntry() finds
// it. The manual cover downloader (GameList::DownloadCovers, CoverDownloadDialog) is untouched.
//
// What a UI does:
//
//   // after a game-list refresh has finished (any thread):
//   Catalog::AutoCovers::Callbacks cb;
//   cb.on_cover_saved = [](const Catalog::CoverSaved& saved) { /* invalidate the cover cache of
//       the game whose path is saved.id; runs on the worker thread, so queue it to the UI thread */ };
//   Catalog::AutoCovers::StartAfterRefresh(cb);
//
//   // at shutdown, before the game list and the UI are torn down:
//   Catalog::AutoCovers::Shutdown();
//
// StartAfterRefresh does nothing when the setting is off, when a run is already going (the games
// added meanwhile are picked up after the next refresh), or when a rate-limit back-off is active.
//
// Settings, all in the base INI under [UI]:
//   AutoDownloadCovers  bool, default TRUE   the automatic download itself
//   CoverArt3D          bool, default false  3D case renders instead of flat scans
//   CoverRegion         int 0..3, default 0  0 disc's own, 1 USA, 2 Europe, 3 Japan/Asia
//   CoverRegionPerGame  string, default ""   "SLUS-21007=3,SLES-50021=1": per-game pins that win
//                                            over CoverRegion (a pin of 0 means "follow the disc")
//
// The style and region only decide which URL a NEW download uses. A game that already has a cover
// keeps it until StartRedownload() replaces it, which a UI calls after the user changes them.

#include "Catalog/CoverDownloadJob.h"
#include "Catalog/CoverSources.h"

#include "GameList.h"

#include "common/SettingsInterface.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Catalog
{
	namespace AutoCovers
	{
		inline constexpr const char* SETTINGS_SECTION = "UI";
		inline constexpr const char* KEY_AUTO_DOWNLOAD = "AutoDownloadCovers";
		inline constexpr const char* KEY_STYLE_3D = "CoverArt3D";
		inline constexpr const char* KEY_REGION = "CoverRegion";
		inline constexpr const char* KEY_REGION_PER_GAME = "CoverRegionPerGame";
		inline constexpr bool DEFAULT_AUTO_DOWNLOAD = true;

		/// Callbacks run on the worker thread.
		struct Callbacks
		{
			std::function<void(const CoverProgress&)> on_progress;
			std::function<void(const CoverSaved&)> on_cover_saved; ///< A file landed; `id` is the game's path.
			std::function<void(const CoverJobSummary&)> on_finished; ///< Last call of a run. Do not Start() from it.
		};

		// ---- Settings -----------------------------------------------------------------------
		//
		// Each setting has two forms: one that reads or writes the host's base INI (it takes the
		// settings lock, so do not call it while holding it), and one that works on any
		// SettingsInterface, which is what the tests and hosts with their own INI use.

		/// [UI] AutoDownloadCovers.
		bool IsEnabled();
		bool IsEnabled(const SettingsInterface& si);

		/// The style, library-wide region and pins from the INI.
		CoverPreferences LoadPreferences();
		CoverPreferences LoadPreferences(const SettingsInterface& si);

		/// Each of these writes the INI key (the host forms also commit). Changing them does not
		/// touch covers that are already downloaded: follow with StartRedownload() for the
		/// affected games. A pin of nullopt removes the game's pin; removing the last one deletes
		/// the key.
		void SetStyle(CoverStyle style);
		void SetStyle(SettingsInterface& si, CoverStyle style);
		void SetLibraryRegion(CoverRegion region);
		void SetLibraryRegion(SettingsInterface& si, CoverRegion region);
		void SetGameRegionPin(const std::string& serial, std::optional<CoverRegion> pin);
		void SetGameRegionPin(SettingsInterface& si, const std::string& serial, std::optional<CoverRegion> pin);

		// ---- Work selection -----------------------------------------------------------------

		/// The cover job for one game-list entry: PS2 and PS1 discs and arcade games with a usable
		/// serial. Null for ELF, invalid and serial-less entries. Does not look at existing covers.
		std::optional<CoverWorkItem> WorkItemForEntry(const GameList::Entry& entry);

		/// Every entry with a usable serial and no cover yet. Takes the game list lock.
		std::vector<CoverWorkItem> CollectMissingCovers();

		// ---- Running ------------------------------------------------------------------------

		/// Starts the automatic download for entries with no cover, if [UI] AutoDownloadCovers is
		/// on. Returns true if a run was started. `fetcher` null means GetDefaultFetcher(); it must
		/// outlive the run.
		bool StartAfterRefresh(Callbacks callbacks = {}, Fetcher* fetcher = nullptr);

		/// Downloads covers again for the games at `game_paths` (game-list entry paths), replacing
		/// whatever cover they have and ignoring the miss cache and AutoDownloadCovers: the user
		/// asked. Uses the current style and region settings. Returns true if a run was started.
		bool StartRedownload(const std::vector<std::string>& game_paths, Callbacks callbacks = {}, Fetcher* fetcher = nullptr);

		bool IsRunning();

		/// Asks the current run to stop and returns at once.
		void Cancel();

		/// Cancels and waits for the worker thread. Call it at shutdown; safe when idle and safe
		/// to repeat.
		void Shutdown();
	} // namespace AutoCovers
} // namespace Catalog
