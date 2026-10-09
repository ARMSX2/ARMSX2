// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The files an arcade game is made of, told apart by name and size: its image, its security dongle, its
// boot program. A Namco System 246/256 game is not one file. Its image (.chd or .iso) is the board's disc or
// hard drive, its dongle is a memory card file the game checks at boot, and its boot program is a small ELF.
// None of them is a PS2 game, so the game list must not list them as discs or ELFs (ArcadeSetClaims), and
// ArcadeSetup needs to know which of the files lying around a game is which.
//
// This is a port of the Android app's ArcadeFiles.kt (game IDs in file names, kindOf, dongleName, the
// Conquest card header), with the same rules.

#pragma once

#include "common/FileSystem.h"
#include "common/Pcsx2Types.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Arcade
{
	/// The game ID a file or folder name carries, upper-case: "Battle Gear 3 (NM00010).chd" is NM00010. It is NM
	/// and five digits that are not part of a longer run of letters or digits. Empty when the name has none.
	std::string FindGameIdInName(std::string_view name);

	enum class FileKind
	{
		Unknown, ///< Not a file of an arcade set by its name.
		Image, ///< The game's disc or hard drive image (.chd, .iso, or a big .bin/.img), as the board's drive reads it.
		PackedImage, ///< An image still packed (.iso.gz): the drive cannot read it as it is.
		Card, ///< A memory card file: the dongle (.ps2, a .bin of a card's size, or one packed as .gz) or Soul Calibur II's Conquest card.
		Other, ///< Something else of the set (a small board ROM, a boot program): never a game.
	};

	/// The smallest PS2 memory card (8 MB without its ECC). Arcade sets carry small board ROMs that are easily
	/// taken for the dongle; anything smaller than this is not one.
	constexpr u64 MIN_CARD_BYTES = 8ull << 20;
	/// Above any memory card file (the biggest the core makes, 64 MB with its ECC, is 66 MB): anything bigger is
	/// an image.
	constexpr u64 MAX_CARD_BYTES = 72ull << 20;

	/// What a file of an arcade set is, by its name, and by its [size] when the name cannot tell (a .bin is an
	/// image or a dongle). [size] of a packed file is its packed size.
	FileKind ClassifyFile(std::string_view name, u64 size);

	/// The game ID of a loose image: an image (FileKind::Image) of an arcade game kept as a file of its own, with no
	/// .acgame. Its name carries the ID ("Battle Gear 3 (NM00010).chd"), else the name of the folder it is in does
	/// (NM00010/game.chd). Empty for anything else: a dongle, a boot program, a packed image, a PS2 game. The second
	/// form reads the file's size when its name cannot tell (a .bin is an image or a dongle).
	std::string LooseImageGameId(std::string_view path, u64 size);
	std::string LooseImageGameId(const std::string& path);

	/// The name the dongle of the game [id] has in the memory cards folder, by what it holds: a card with its ECC
	/// is a .ps2, one without (a whole number of megabytes) a .bin.
	std::string DongleName(std::string_view id, u64 bytes);

	/// Soul Calibur II (NM00007) is the one arcade game that reads a card of its own besides its dongle: its
	/// Conquest card, in slot 2.
	constexpr const char* CONQUEST_GAME = "NM00007";
	/// Its name in the memory cards folder, where the core mounts it from when the game's .acgame names no card
	/// (VMManager::OpenArcadeGame). Never a .bin: the core converts a .bin as a card without ECC, and Soul Calibur
	/// II keeps its own checksums where the ECC would be.
	constexpr const char* CONQUEST_CARD_NAME = "NM00007.conquestcard";
	/// A Conquest card with its spare bytes, the only kind the game reads: 16384 pages of 512 + 16 bytes.
	constexpr u64 CONQUEST_CARD_BYTES = 8650752;

	/// How many of a card's first bytes IsConquestCardHeader() reads.
	size_t ConquestHeaderBytes();
	/// Whether the first bytes of a card file (unpacked) are a Conquest card's, which a dongle never has.
	bool IsConquestCardHeader(std::span<const u8> head);

	/// The files in the folders a scan covers that belong to an arcade set, which the game list must leave out
	/// of its disc and ELF scan: an arcade game's boot.elf would be listed as an ELF "boot", its dongle, named
	/// .bin, as a disc, and its image as a bogus PS2 disc.
	///
	/// A file belongs to a set when
	///  - an .acgame in the scan names it (its boot program, image, SRAM, dongle or card), or, when that .acgame
	///    has a game folder ([data] subdir, the game ID by default), when it lies anywhere inside that folder, or
	///  - its name carries a game ID (or the folder it is in is named after one) and is a file of an arcade set
	///    by ClassifyFile().
	/// Names and folders are compared without regard to case (a hand-typed .acgame often gets it wrong).
	class SetClaims
	{
	public:
		/// [files] are the files a scan found (full paths, with sizes).
		explicit SetClaims(const FileSystem::FindResultsArray& files);

		bool Owns(const std::string& path) const;
		bool Empty() const { return m_files.empty() && m_folders.empty(); }

		/// [path] is a loose image (LooseImageGameId) of a game that no .acgame in the scan describes. It is owned like
		/// the rest of a set, so a scan that does not know better leaves it out, but it is a game: the one file of the
		/// set that a game list shows. An image of a game that has an .acgame is that .acgame's, wherever it lies.
		bool IsLooseImage(const std::string& path) const;

	private:
		bool Contains(const std::string& key) const;

		std::unordered_set<std::string> m_files; // lower-case full paths
		std::unordered_set<std::string> m_loose; // the loose images among them
		std::vector<std::string> m_folders; // lower-case full paths with a trailing separator
	};
} // namespace Arcade
