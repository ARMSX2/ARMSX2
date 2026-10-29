// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Getting a Namco System 246/256 game ready to boot: the service behind the "Arcade Setup" page, shared by the
// Qt and Big Picture UIs. It works on plain file paths.
//
// What an arcade game needs, and who supplies it:
//
//   the arcade BIOS    a COH-H dump of a System 246 or 256 board, in the BIOS folder. The player brings it.
//   the boot program   boot.elf, the small ELF the board's BIOS starts. The PS2Homebrew-arcade "Proverb"
//                      project builds one per game; its nightly zip (about 1 MB) holds them all.
//                      DownloadBootFiles() stores that zip and a boot.elf is read out of it when needed.
//   the image          the game's disc or hard drive as a .chd or .iso. The player brings it.
//   the dongle         a memory card file the game checks at boot, in the memory cards folder. The player
//                      brings it, beside the image; it is copied there at the first launch.
//   (Soul Calibur II)  also a Conquest card in slot 2. A cleared one ships with the app (resources/arcade).
//
// A game reaches the board as an .acgame file (PCSX2x6's format, also what the game list shows) that names the
// files around it, or as a loose image (NM00010.chd, "Battle Gear 3 (NM00010).chd") with its dongle beside it.
// Prepare() gets either ready, writing an .acgame in the data folder for a loose image, and returns what to hand
// to VMManager::Initialize() (PrepareResult::ApplyTo()). Missing() says what a game still lacks without
// changing anything.
//
// Threads: every call blocks and may be made from any thread (a worker, not the GS thread). Two downloads of
// the boot files at once are refused. Progress callbacks run on the calling thread.

#pragma once

#include "Catalog/CancelToken.h"
#include "Catalog/Fetch.h"

#include "common/Pcsx2Types.h"

#include <ctime>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct VMBootParameters;

namespace ArcadeSetup
{
	/// Proverb's nightly build: one zip holding bin/NM#####/boot.elf for every game. The URL answers 302 with a
	/// signed, expiring one; the fetch layer follows it.
	constexpr const char* BOOT_FILES_URL = "https://github.com/PS2Homebrew-arcade/proverb/releases/download/nightly/proverb.zip";
	/// What the release is called and when it was built, and the SHA-256 of the zip it holds: for the version
	/// shown to the player and for "is there a newer one".
	constexpr const char* BOOT_RELEASE_URL = "https://api.github.com/repos/PS2Homebrew-arcade/proverb/releases/tags/nightly";
	/// The zip is about 1 MB; anything past this is not it.
	constexpr u64 MAX_BOOT_FILES_BYTES = 64ull << 20;

	/// Where the service keeps and finds things. FromEmuFolders() is what the app uses; tests build their own.
	struct Folders
	{
		/// The service's own folder: proverb.zip (and proverb.json, what is known about it), and for each game
		/// <arcade>/<ID>/ with its SRAM, the boot program, the .acgame made for a loose image and dongle.from.
		/// <DataRoot>/arcade.
		std::string arcade;

		/// Where the core mounts the dongle and the Conquest card from (EmuFolders::MemoryCards).
		std::string memory_cards;

		/// The cleared Soul Calibur II Conquest card, gzip packed (resources/arcade/NM00007.conquestcard.gzip).
		std::string conquest_card_gz;

		/// What the Conquest card looks like. The defaults are the real cards; tests set their own, so a swap by
		/// hash can be tested with small made-up files.
		struct ConquestCard
		{
			/// Size of a finished card, spare bytes included.
			u64 bytes = 8650752;
			/// The card the 2.8.2 Android app shipped, not yet cleared (SC2MAKER's cardmaterial.bin): the game never
			/// wrote to it, and it is swapped for the cleared one. Told by SHA-256, after a look at
			/// [probe_length] bytes at [probe_offset] (page 112, which the clear rewrites) so that almost every
			/// other card is rejected without reading all 8 MB.
			std::string uncleared_sha256 = "fe7eac4c5566fa4f16680e2ce9ea682215207f87688dabc4b9065a30050c71f8";
			u64 probe_offset = 112 * 528;
			std::string probe_hex = "4d4901007f6be5234c5ccbd5c7ddeda0";
		} conquest;

		static Folders FromEmuFolders();
	};

	struct BootFilesStatus
	{
		bool installed = false; ///< A stored zip that holds at least one boot program.
		std::string path; ///< Where the zip is, or would be.
		std::vector<std::string> game_ids; ///< The games it has a boot program for, sorted ("NM00001"...).
		u64 size_bytes = 0;
		std::time_t installed_at = 0; ///< When the stored zip was written (its modification time).
		std::string sha256;
		/// What the release was called when the zip was downloaded ("Nightly build of v1.0.4"), and when that build
		/// was made. Empty and 0 when GitHub could not be asked then, or the zip came from InstallBootFilesFromFile().
		std::string release_name;
		std::time_t release_time = 0;
	};

	struct InstallResult
	{
		bool ok = false;
		bool cancelled = false;
		std::string error; ///< Why not, in a sentence for the player.
		BootFilesStatus status; ///< The boot files as they are now (unchanged on failure).
	};

	struct DownloadOptions
	{
		Catalog::CancelToken cancel;
		/// Bytes of the zip so far and in all (0 when the server does not say).
		std::function<void(u64 done, u64 total)> on_progress;
	};

	struct UpdateCheck
	{
		enum class State
		{
			Failed, ///< GitHub could not be asked or answered something else; [error] says what.
			NotInstalled, ///< Nothing is stored. [release_name] etc. describe what a download would get.
			UpToDate,
			UpdateAvailable,
		};

		State state = State::Failed;
		std::string error;
		std::string release_name;
		std::time_t release_time = 0;
		u64 size_bytes = 0; ///< Of the zip on GitHub.
	};

	struct BiosDump
	{
		std::string file_name; ///< In the BIOS folder.
		std::string board; ///< "System 256", "System 246 Rack C", ... as the core names it.
	};

	struct BiosStatus
	{
		std::vector<BiosDump> dumps; ///< Every COH-H dump in the BIOS folder, which the core all uses.
		bool IsPresent() const { return !dumps.empty(); }
		/// The boards of the dumps, "System 246 Rack C, System 256", else their file names; empty for none.
		std::string Summary() const;
	};

	/// The arcade BIOS the game [game_id] boots on: a file name in the BIOS folder (FindArcadeBiosFor), empty when
	/// none will do. Then [needs] names the board whose BIOS it needs ("System 246" for Battle Gear 3, which
	/// refuses the System 256 one), and stays empty when the BIOS folder has no arcade BIOS at all.
	std::string FindBiosFor(const std::string& game_id, std::string* needs = nullptr);

	enum class PartKind
	{
		ArcadeBios, ///< The BIOS folder has no arcade BIOS at all.
		BoardBios, ///< It has some, but the game runs on none of them. [detail] is the board it needs.
		BootProgram, ///< No boot.elf beside the game, and none in the boot files (not downloaded, or not for this game).
		Image, ///< The game's disc or hard drive image. [detail] is its file name.
		PackedImage, ///< The image is packed (.iso.gz); the drive cannot read it as it is.
		Dongle, ///< [detail] is its file name (the game's ID for a loose image).
		Card, ///< The second memory card the .acgame names. [detail] is its file name.
		Unreadable, ///< The file is no .acgame the core can use (no game ID, a name that leaves its folder), or no arcade image. [detail] is why, when known.
	};

	struct MissingPart
	{
		PartKind kind;
		std::string detail;

		/// A few words naming the part, for a list: "dongle", "boot file", "arcade BIOS", "System 246 BIOS". The
		/// words are translated (context "Arcade").
		std::string Short() const;
		/// A sentence saying what is missing and where it goes, for an error message (context "Arcade").
		std::string Explain() const;
	};

	struct MissingReport
	{
		std::string game_id; ///< Empty when the file is Unreadable.
		std::string title;
		std::vector<MissingPart> parts; ///< Empty: ready to play.
		/// The game starts but only reaches its attract demo so far (see IsAttractOnly()).
		bool attract_only = false;

		bool IsReady() const { return parts.empty(); }
		/// The parts' Short() names joined with ", ".
		std::string Summary() const;
	};

	/// The games that start but only reach their attract demo, in PCSX2x6 as here
	/// (https://github.com/PS2Homebrew-arcade/pcsx2x6/issues/9): Dragon Chronicle, Dragon Chronicle Online and
	/// The IDOLM@STER.
	bool IsAttractOnly(std::string_view game_id);

	struct PrepareResult
	{
		bool ok = false;
		std::string error; ///< Why not: every missing part explained, one sentence each. Empty when ok.
		MissingReport missing; ///< The same, in parts. Empty when ok.

		std::string game_id;
		std::string title;

		// All set when ok.
		std::string acgame; ///< The .acgame to boot: the one given, or the one made for a loose image.
		std::string elf; ///< The boot program the board runs.
		std::string media; ///< The image.
		std::string sram; ///< The board's settings memory (its test menu's saves), kept in the data folder.

		/// Fills the boot parameters VMManager::Initialize() takes: filename, arcade_elf, arcade_media and
		/// arcade_sram, and leaves the disc source unset, which sends it down its arcade branch.
		void ApplyTo(VMBootParameters* params) const;
	};

	class Service
	{
	public:
		explicit Service(Folders folders = Folders::FromEmuFolders());

		const Folders& GetFolders() const { return m_folders; }

		BootFilesStatus GetBootFilesStatus() const;

		/// Whether the stored zip has a boot program for the game [game_id].
		bool HasBootProgram(std::string_view game_id) const;

		/// Writes the game's boot.elf out of the stored zip to [dest_path] (replacing what is there, via a temporary
		/// file). False, with [error] saying why, when the zip is not there or has none for the game.
		bool ExtractBootProgram(std::string_view game_id, const std::string& dest_path, std::string* error = nullptr) const;

		/// Downloads the boot files (BOOT_FILES_URL, about 1 MB) and replaces the stored zip with them. The zip is
		/// kept only if it holds boot programs; a failed or cancelled download leaves the old one. Also asks
		/// GitHub (BOOT_RELEASE_URL) what the release is called, which a failure there does not stop.
		InstallResult DownloadBootFiles(Catalog::Fetcher& fetcher, const DownloadOptions& options = {});

		/// The same for a zip the player already has (a machine with no network). It is copied.
		InstallResult InstallBootFilesFromFile(const std::string& zip_path);

		/// Asks GitHub whether the release has changed since the stored zip was downloaded.
		UpdateCheck CheckForBootFilesUpdate(Catalog::Fetcher& fetcher, const Catalog::CancelToken& cancel = {}) const;

		BiosStatus GetBiosStatus() const;

		/// What the game at [path] (an .acgame, or a loose image) still lacks. Changes nothing.
		MissingReport Missing(const std::string& path) const;

		/// Gets the game at [path] ready to boot, as Missing() judged it: the boot program (from the boot files
		/// when the .acgame has none beside it), the dongle and a second card copied into the memory cards folder,
		/// the Conquest card put in place for Soul Calibur II, the SRAM seeded, and for a loose image its .acgame
		/// written. When something is missing nothing is changed and the result says what.
		///
		/// The dongle is copied once and kept, since the game writes to it. A loose game remembers which file it came
		/// from (<arcade>/<ID>/dongle.from: name, size, SHA-256); when that file is gone from beside the image and a
		/// different dongle is there, the new one replaces the copy. The same dongle under a new name keeps the copy.
		PrepareResult Prepare(const std::string& path);

	private:
		Folders m_folders;
	};
} // namespace ArcadeSetup
