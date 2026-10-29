// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Downloads a catalog texture pack and installs it into <Textures>/<SERIAL>/replacements, the
// folder GSTextureReplacements scans for the running disc's serial. Also lists, measures and
// removes installed packs.
//
// Packs are large and land on a handheld's SD card, so Install() checks free space before
// starting, hashes the bytes as they arrive, and replaces the old folder in a way that can roll
// back.
//
// Staging is in <Textures>/.texturepacks-tmp, on the same filesystem as the destination so the
// commit is a rename, not a copy; it is deleted and recreated at the start of an install and
// deleted at the end. Each part of a multi-part archive is checked as it finishes, so a bad part
// is named and the remaining parts are not downloaded for nothing.
//
// The commit replaces the whole replacements folder rather than merging: a game has one pack. A
// folder with no install record for the serial (textures the player made or copied by hand) is
// never deleted. It is kept as replacements.user-<date and time> beside the new one
// (InstallResult::kept_previous), and a UI should ask first (HasUnrecordedReplacements()). A
// symbolic link is never followed when deleting; the link itself goes. The install is then recorded
// in TexturePackState, dropping any record of another pack for the serial.
//
// One install runs at a time in the whole process; a second Install() returns Status::Busy.
// Cancellation is honoured until the Installing phase is reported; after that the commit runs to
// completion, and the UI should hide its Cancel button.
//
// Install() blocks for as long as the download takes: call it from a worker thread. The progress
// callback runs on that thread; marshal to the UI thread yourself.
//
// After a successful install the UI calls TexturePackEngine::ActivateAfterInstall() (turns
// texture replacement on and reloads it in a running game), and PostInstallHit() if the player
// allows it.

#include "Catalog/CancelToken.h"
#include "Catalog/Fetch.h"
#include "Catalog/TexturePackCatalog.h"
#include "Catalog/TexturePackState.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Catalog
{
	class TexturePackInstaller
	{
	public:
		/// Peak disk use beyond the archive and its extracted files: the outgoing pack is kept
		/// until the new one is in place.
		static constexpr u64 FREE_SPACE_SLACK = 512ull * 1024 * 1024;

		struct Config
		{
			/// EmuFolders::Textures. Created if missing.
			std::string textures_dir;

			/// Where measured pack sizes are remembered between runs (keyed on the folder's
			/// modification time). Empty keeps them in memory only.
			std::string size_cache_path;

			/// Free space, in bytes, on the filesystem holding `path`; nullopt if unknown (the
			/// check is then skipped). Empty uses std::filesystem::space(). Tests set this.
			std::function<std::optional<u64>(const std::string& path)> query_free_space;
		};

		/// textures_dir = EmuFolders::Textures, size cache in <Cache>/texture-catalog.
		static Config DefaultConfig();

		/// `fetcher` and `state` must outlive the installer.
		TexturePackInstaller(Fetcher& fetcher, TexturePackState& state, Config config);

		enum class Phase
		{
			Starting,
			Downloading, ///< done/total are bytes of the whole archive (all parts)
			Verifying,
			Extracting, ///< done/total are texture files
			Installing, ///< The commit has begun; cancelling no longer works.
		};

		struct Progress
		{
			Phase phase = Phase::Starting;
			u64 done = 0;
			u64 total = 0;
		};
		using ProgressCallback = std::function<void(const Progress&)>;

		enum class Status
		{
			Ok,
			Cancelled,
			Failed, ///< `error` says why, in a sentence fit to show.
			Busy,
		};

		struct InstallResult
		{
			Status status = Status::Failed;
			std::string error;
			TexturePackRecord record; ///< Valid when status is Ok.
			u64 textures = 0;
			/// The name (inside the game's folder) the player's own replacements folder was kept
			/// under, "replacements.user-<date and time>"; empty when there was none to keep.
			std::string kept_previous;

			bool IsOk() const { return status == Status::Ok; }
		};

		/// Installs `pack` for `serial`, which must be one of the pack's own serials (any
		/// spelling; see TexturePack::InstallSerialFor() for choosing). A pack can never land in
		/// another game's folder.
		InstallResult Install(const TexturePack& pack, std::string_view serial,
			const ProgressCallback& progress = {}, const CancelToken& cancel = {});

		/// True while any installer in the process is inside Install().
		static bool IsInstalling();

		static constexpr const char* KEPT_FOLDER_PREFIX = "replacements.user-";

		/// True when <Textures>/<serial>/replacements exists and no install record names `serial`: a
		/// folder of textures the player made or copied. Install() sets it aside as
		/// KEPT_FOLDER_PREFIX + date and time instead of replacing it, and a UI should say so and ask
		/// before it starts. `serial` is spelled as for Install(); false for one that is not valid.
		/// Cheap enough for the UI thread.
		bool HasUnrecordedReplacements(std::string_view serial) const;

		/// Free space an install needs on the textures filesystem. tar+zstd is streamed, so it is
		/// the archive plus its exact decompressed size; zip roughly doubles the archive. Both
		/// plus FREE_SPACE_SLACK.
		static u64 RequiredFreeSpace(const TexturePack& pack);

		/// Swaps `staged` into `target`, keeping the previous contents until the new ones are in
		/// place (target + ".old"), and restoring them if the swap fails. Public so it can be
		/// tested; Install() uses it.
		static bool CommitDirectory(const std::string& staged, const std::string& target);

		struct InstalledPack
		{
			std::string serial; ///< The folder's name under <Textures>, normally "SLUS-21410".
			std::string directory;
			u64 file_count = 0;
			u64 bytes = 0;
			std::optional<TexturePackRecord> record; ///< Absent for a hand-copied folder.
		};

		/// Every <Textures>/<X>/replacements folder, sorted by name. Finishes deleting packs an
		/// earlier Remove() could not delete completely, and reconciles the install record with
		/// what is on disk. Counts and sizes come from a cache keyed on the folder's modification
		/// time, so a listing does not walk every file. Blocking; use a worker thread. `cancel` is
		/// looked at before each pack's folder is walked; a cancelled listing returns nothing and
		/// leaves the install record as it was.
		std::vector<InstalledPack> ListInstalled(const CancelToken& cancel = {});

		/// Removes the pack in <Textures>/<folder_name>: takes it out of use in one rename (so a
		/// delete that fails half way can never leave a half pack where the engine reads it),
		/// then deletes the files (a symbolic link is removed, never followed), and finally the
		/// folder itself if nothing else (texture dumps) is in it. The install record is dropped
		/// whatever happened on disk. Returns true when no pack is left there. `folder_name` is a
		/// single folder name as ListInstalled() reports it, not a path.
		bool Remove(std::string_view folder_name);

		/// Finishes deleting packs Remove() could not delete completely.
		void SweepRemoved();

		const Config& GetConfig() const { return m_config; }

	private:
		struct SizeEntry
		{
			s64 mtime = 0;
			u64 file_count = 0;
			u64 bytes = 0;
		};

		void LoadSizeCacheLocked();
		void SaveSizeCacheLocked();
		void ForgetSize(std::string_view folder_name);

		Fetcher& m_fetcher;
		TexturePackState& m_state;
		Config m_config;

		std::mutex m_size_mutex;
		bool m_size_loaded = false;
		std::unordered_map<std::string, SizeEntry> m_sizes;
	};

	/// A pack's size as the catalog shows it, e.g. "241 MB", "1.2 GB".
	std::string FormatPackSize(u64 bytes);

	/// An English fallback sentence for a progress report. UIs translate their own.
	std::string DescribeProgress(const TexturePackInstaller::Progress& progress);
} // namespace Catalog
