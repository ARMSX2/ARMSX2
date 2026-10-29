// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Which catalog packs are installed, and at which version. A pack extracts to loose replacement
// files under <Textures>/<SERIAL>/replacements with nothing recording where they came from, so
// the texture folder alone cannot say "Installed" or "Update available".
//
// The record is <DataRoot>/texture-packs.json, a JSON object keyed by pack id:
//   { "<packId>": { "serial": "SLUS-21410", "version": "...", "name": "...",
//                   "format": "zip|tar+zstd", "schema": 1|2, "archiveRevision": 0, "sha256": "" } }
// It is written through a .tmp file with the previous copy kept as .bak, and read from the .bak
// when the main file is unreadable.
//
// One pack is recorded per serial: the engine reads a single replacements folder per game and an
// install replaces the whole folder (see TexturePackInstaller).
//
// Every call is safe from any thread; file operations are serialised process-wide.

#include "Catalog/TexturePackCatalog.h"

#include <map>
#include <set>
#include <string>

namespace Catalog
{
	struct TexturePackRecord
	{
		std::string pack_id;
		std::string serial; ///< upper-case "SLUS-21410"
		std::string version;
		std::string name;

		// These describe the archive that was installed, not the catalog entry that described it,
		// so update eligibility survives a catalog outage. Their defaults are a legacy zip install.
		std::string format = "zip"; ///< "zip" or "tar+zstd"
		int schema = 1;
		u64 archive_revision = 0;
		std::string sha256; ///< lowercase hex, or empty

		bool IsTarZstd() const { return schema >= 2 && format == "tar+zstd"; }
	};

	enum class InstallAction
	{
		Install, ///< not installed: button "Download"
		Update, ///< a newer archive exists: button "Update"
		Installed, ///< up to date: "Installed", disabled
		Conflict, ///< same revision, different digest: the UI shows Update
	};

	class TexturePackState
	{
	public:
		/// The record at `file_path`. It need not exist yet.
		explicit TexturePackState(std::string file_path);

		static TexturePackState InDataRoot();

		const std::string& FilePath() const { return m_path; }

		std::map<std::string, TexturePackRecord> All() const;
		std::optional<TexturePackRecord> Find(std::string_view pack_id) const;
		std::optional<TexturePackRecord> FindForSerial(std::string_view serial) const;

		/// Records a finished install. Any other pack recorded for the same serial is dropped,
		/// since its files were replaced. Returns false if the file could not be written.
		bool Record(const TexturePackRecord& record);

		/// Drops the record of whatever is installed for `serial` (after a delete).
		bool ForgetSerial(std::string_view serial);

		/// Drops every record whose serial is not in `present_serials_upper`, the serials that
		/// really have a <Textures>/<SERIAL>/replacements folder. When record and disk disagree
		/// (pack deleted in a file manager, half-finished delete) the disk wins; otherwise the
		/// catalog shows a permanent "Installed" and the disabled button blocks reinstalling.
		///
		/// A pack on disk with no record is left alone: a hand-copied folder has no known version.
		/// An empty scan while records exist is almost always a failed scan (unreadable folder,
		/// not mounted yet), so it changes nothing.
		void Reconcile(const std::set<std::string>& present_serials_upper);

		/// Bumped whenever a write changed the record, so a UI can notice without re-reading.
		static u32 Revision();

		/// The update rule. A tar+zstd catalog pack: an installed record that is not tar+zstd
		/// updates; a higher catalog revision updates; a lower one counts as installed (a lagging
		/// mirror must not walk anyone backwards); an equal revision with the same digest is
		/// installed and with a different digest a Conflict. A zip catalog pack: installed if the
		/// record is tar+zstd or the version strings agree, else Update. No record means Install.
		static InstallAction ActionFor(const std::optional<TexturePackRecord>& installed, const TexturePack& pack);

		/// ActionFor() for a catalog row: looks the pack up by id. A different pack installed for
		/// the same game does not make this one "Installed"; installing this one replaces it.
		InstallAction ActionForPack(const TexturePack& pack) const;

		static TexturePackRecord RecordFor(const TexturePack& pack, std::string_view serial);

	private:
		std::string m_path;
	};
} // namespace Catalog
