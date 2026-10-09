// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Which files lying in a games folder belong to an arcade game: game IDs in file names, the kind of file by
// name and size, and the claims a scan makes so that an arcade game's boot program, image and dongle are not
// listed as games of their own. The rules are the Android app's (ArcadeFilesTest.kt).

#include <gtest/gtest.h>

#include "Arcade/ArcadeFiles.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Arcade;

namespace
{
	void WriteFile(const std::string& path, const std::string& content)
	{
		ASSERT_TRUE(FileSystem::CreateDirectoryPath(std::string(Path::GetDirectory(path)).c_str(), true));
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << content;
	}

	// A scan's result for the files under [root]: every file there, with its size.
	FileSystem::FindResultsArray Scan(const std::string& root)
	{
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(root.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE, &files);
		return files;
	}

	class SetClaimsTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_root = Path::Combine(testing::TempDir(), "arcade_files_claims");
			std::error_code ec;
			std::filesystem::remove_all(m_root, ec);
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(m_root.c_str(), true));
		}

		void TearDown() override
		{
			std::error_code ec;
			std::filesystem::remove_all(m_root, ec);
		}

		void Put(const char* relative, const std::string& content = "x") { WriteFile(Path::Combine(m_root, relative), content); }
		std::string P(const char* relative) const { return Path::Combine(m_root, relative); }

		std::string m_root;
	};
} // namespace

TEST(ArcadeFiles, AGameIDIsFoundInAFileOrFolderName)
{
	EXPECT_EQ(FindGameIdInName("NM00010.chd"), "NM00010");
	EXPECT_EQ(FindGameIdInName("Battle Gear 3 (NM00010).chd"), "NM00010");
	EXPECT_EQ(FindGameIdInName("battle gear 3 [nm00010] v2.iso"), "NM00010"); // Any case, answered in upper case.
	EXPECT_EQ(FindGameIdInName("NM00010"), "NM00010");
	EXPECT_EQ(FindGameIdInName("NM00010_NM00011.chd"), "NM00010"); // The first one.
	EXPECT_EQ(FindGameIdInName("NM00010b.chd"), "NM00010"); // A letter after it is fine, a digit is not.

	EXPECT_EQ(FindGameIdInName(""), "");
	EXPECT_EQ(FindGameIdInName("NM0001.chd"), ""); // Four digits.
	EXPECT_EQ(FindGameIdInName("NM000100.chd"), ""); // Six.
	EXPECT_EQ(FindGameIdInName("SNM00010.chd"), ""); // Part of a longer word.
	EXPECT_EQ(FindGameIdInName("1NM00010.chd"), "");
	EXPECT_EQ(FindGameIdInName("SLUS_NM00010.iso"), "NM00010"); // After an underscore it stands alone, as in the Android rule.
	EXPECT_EQ(FindGameIdInName("Tekken 4.chd"), "");
	EXPECT_EQ(FindGameIdInName("NM 00010.chd"), "");
}

TEST(ArcadeFiles, TheKindOfAFileComesFromItsNameAndSize)
{
	const u64 small = 1024, card = 8ull << 20, big = 400ull << 20;

	EXPECT_EQ(ClassifyFile("NM00010.chd", small), FileKind::Image);
	EXPECT_EQ(ClassifyFile("NM00010.CHD", small), FileKind::Image);
	EXPECT_EQ(ClassifyFile("NM00010.iso", big), FileKind::Image);
	EXPECT_EQ(ClassifyFile("NM00010.iso.gz", big), FileKind::PackedImage);
	EXPECT_EQ(ClassifyFile("NM00010.chd.gz", big), FileKind::PackedImage);

	EXPECT_EQ(ClassifyFile("NM00010.ps2", small), FileKind::Card); // By its name alone.
	EXPECT_EQ(ClassifyFile("NM00007.conquestcard", 8650752), FileKind::Card);

	// A .bin or .img is an image or a dongle by its size: a card is 8 MB (no spare bytes) up to 72 MB.
	EXPECT_EQ(ClassifyFile("NM00010.bin", small), FileKind::Other); // A board ROM.
	EXPECT_EQ(ClassifyFile("NM00010.bin", card - 1), FileKind::Other);
	EXPECT_EQ(ClassifyFile("NM00010.bin", card), FileKind::Card);
	EXPECT_EQ(ClassifyFile("NM00010.bin", 8650752), FileKind::Card);
	EXPECT_EQ(ClassifyFile("NM00010.bin", MAX_CARD_BYTES), FileKind::Card);
	EXPECT_EQ(ClassifyFile("NM00010.bin", MAX_CARD_BYTES + 1), FileKind::Image);
	EXPECT_EQ(ClassifyFile("NM00010.img", big), FileKind::Image);
	EXPECT_EQ(ClassifyFile("NM00010.bin.gz", big), FileKind::PackedImage);
	EXPECT_EQ(ClassifyFile("NM00010.bin.gz", 3000), FileKind::Card); // A packed dongle is far smaller than its card.

	EXPECT_EQ(ClassifyFile("boot.elf", 25028), FileKind::Other);
	EXPECT_EQ(ClassifyFile("NM00010.txt", small), FileKind::Unknown);
	EXPECT_EQ(ClassifyFile("NM00010", small), FileKind::Unknown);
	EXPECT_EQ(ClassifyFile("NM00010.gz", small), FileKind::Unknown);
	EXPECT_EQ(ClassifyFile("NM00010.acgame", small), FileKind::Unknown);
}

TEST(ArcadeFiles, ADongleIsNamedByWhatItHolds)
{
	EXPECT_EQ(DongleName("NM00010", 8650752), "NM00010.ps2"); // 16384 pages with their spare bytes.
	EXPECT_EQ(DongleName("NM00010", 8ull << 20), "NM00010.bin"); // A whole number of megabytes: no spare bytes.
	EXPECT_EQ(DongleName("NM00010", 16ull << 20), "NM00010.bin");
	EXPECT_EQ(DongleName("NM00023", 8650753), "NM00023.ps2");
}

TEST(ArcadeFiles, AConquestCardIsToldByItsHeader)
{
	const std::string header = "Memory Card for SoulCaliburII (C)1995";
	const std::vector<u8> good(header.begin(), header.end());
	EXPECT_EQ(ConquestHeaderBytes(), 29u);
	EXPECT_TRUE(IsConquestCardHeader(good));
	EXPECT_TRUE(IsConquestCardHeader(std::span<const u8>(good).first(29)));
	EXPECT_FALSE(IsConquestCardHeader(std::span<const u8>(good).first(28))); // Cut short.

	std::vector<u8> other = good;
	other[0] = 'm';
	EXPECT_FALSE(IsConquestCardHeader(other));
	EXPECT_FALSE(IsConquestCardHeader({}));
	EXPECT_FALSE(IsConquestCardHeader(std::vector<u8>(64, 0)));
}

TEST_F(SetClaimsTest, AnAcgamesGameFolderAndEverythingInItIsItsOwn)
{
	// PCSX2x6's layout.
	Put("NM00004.acgame", "[game]\ngameid=NM00004\n[data]\nsubdir=NM00004\n");
	Put("NM00004/boot.elf");
	Put("NM00004/NM00004.chd");
	Put("NM00004/anything-else.bin");
	Put("NM00004/deeper/file.iso");
	Put("Other/hello.elf");
	Put("Other/Some Game.iso");

	const SetClaims claims(Scan(m_root));
	EXPECT_TRUE(claims.Owns(P("NM00004/boot.elf")));
	EXPECT_TRUE(claims.Owns(P("NM00004/NM00004.chd")));
	EXPECT_TRUE(claims.Owns(P("NM00004/anything-else.bin")));
	EXPECT_TRUE(claims.Owns(P("NM00004/deeper/file.iso")));

	EXPECT_FALSE(claims.Owns(P("NM00004.acgame"))); // The .acgame is the game.
	EXPECT_FALSE(claims.Owns(P("Other/hello.elf")));
	EXPECT_FALSE(claims.Owns(P("Other/Some Game.iso")));
}

TEST_F(SetClaimsTest, AnAcgameWithoutAGameFolderOwnsTheFilesItNamesBesideIt)
{
	Put("bg3/NM00010.acgame",
		"[game]\ngameid=NM00010\n[data]\nsubdir=\nelf=Boot.ELF\nmediasrc=bg3.chd\nsram=settings.bin\ndongle=NM00010.bin\ncard=extra.ps2\n");
	Put("bg3/boot.elf");
	Put("bg3/bg3.chd");
	Put("bg3/settings.bin");
	Put("bg3/hdd-writes.bin");
	Put("bg3/NM00010.bin", "x");
	Put("bg3/extra.ps2");
	Put("bg3/unrelated.iso");
	Put("bg3/unrelated.elf");

	const SetClaims claims(Scan(m_root));
	EXPECT_TRUE(claims.Owns(P("bg3/boot.elf"))); // Without regard to case.
	EXPECT_TRUE(claims.Owns(P("bg3/bg3.chd")));
	EXPECT_TRUE(claims.Owns(P("bg3/settings.bin")));
	EXPECT_TRUE(claims.Owns(P("bg3/hdd-writes.bin"))); // What the board writes beside the SRAM.
	EXPECT_TRUE(claims.Owns(P("bg3/NM00010.bin"))); // The dongle.
	EXPECT_TRUE(claims.Owns(P("bg3/extra.ps2")));

	// Only what it names: a disc or an ELF beside it is a game of its own.
	EXPECT_FALSE(claims.Owns(P("bg3/unrelated.iso")));
	EXPECT_FALSE(claims.Owns(P("bg3/unrelated.elf")));
}

TEST_F(SetClaimsTest, AnAcgamesDongleBesideItIsItsOwnWhateverItsGameFolderIs)
{
	Put("NM00004.acgame", "[game]\ngameid=NM00004\n[data]\ndongle=NM00004.bin\n");
	Put("NM00004.bin");

	EXPECT_TRUE(SetClaims(Scan(m_root)).Owns(P("NM00004.bin")));
}

TEST_F(SetClaimsTest, AnAbsolutePathAnAcgameGivesIsClaimedWhereverItIs)
{
	const std::string elsewhere = Path::Combine(m_root, "images");
	Put("games/NM00005.acgame", "[game]\ngameid=NM00005\n[data]\nsubdir=\nmediasrc=" + Path::Combine(elsewhere, "NM00005.chd") + "\n");
	Put("images/NM00005.chd");
	Put("images/another.chd");

	const SetClaims claims(Scan(m_root));
	EXPECT_TRUE(claims.Owns(P("images/NM00005.chd")));
	EXPECT_FALSE(claims.Owns(P("images/another.chd")));
}

TEST_F(SetClaimsTest, AnImageNamedAfterItsGameAndTheSetBesideItAreNotGames)
{
	// No .acgame: games kept as their own files.
	Put("loose/Battle Gear 3 (NM00010).chd");
	Put("loose/Battle Gear 3 (NM00010).ps2");
	Put("loose/Battle Gear 3 (NM00010).bin.gz"); // A packed dongle.
	Put("loose/NM00010 boot.elf");
	Put("loose/Some Other Game.chd"); // Names no game: a PS2 game as far as this goes.
	Put("loose/notes.txt");

	const SetClaims claims(Scan(m_root));
	EXPECT_TRUE(claims.Owns(P("loose/Battle Gear 3 (NM00010).chd")));
	EXPECT_TRUE(claims.Owns(P("loose/Battle Gear 3 (NM00010).ps2")));
	EXPECT_TRUE(claims.Owns(P("loose/Battle Gear 3 (NM00010).bin.gz")));
	EXPECT_TRUE(claims.Owns(P("loose/NM00010 boot.elf")));
	EXPECT_FALSE(claims.Owns(P("loose/Some Other Game.chd")));
	EXPECT_FALSE(claims.Owns(P("loose/notes.txt")));
}

TEST(ArcadeFiles, ALooseImageIsAnImageNamedAfterItsGameOrInAFolderThatIs)
{
	EXPECT_EQ(LooseImageGameId("/games/Battle Gear 3 (NM00010).chd", 100), "NM00010");
	EXPECT_EQ(LooseImageGameId("/games/nm00010.iso", 100), "NM00010");
	EXPECT_EQ(LooseImageGameId("/games/NM00004/game.chd", 100), "NM00004");
	// The game ID in the file's own name wins over the folder's.
	EXPECT_EQ(LooseImageGameId("/games/NM00004/NM00010.chd", 100), "NM00010");
	// A big .bin is an image, a dongle-sized one is not.
	EXPECT_EQ(LooseImageGameId("/games/NM00010.bin", MAX_CARD_BYTES + 1), "NM00010");
	EXPECT_EQ(LooseImageGameId("/games/NM00010.bin", MIN_CARD_BYTES), "");

	// Not an image of an arcade game: a PS2 game, a dongle, a boot program, a packed image, a file with no ID.
	EXPECT_EQ(LooseImageGameId("/games/Some Other Game.chd", 100), "");
	EXPECT_EQ(LooseImageGameId("/games/NM00010.ps2", 100), "");
	EXPECT_EQ(LooseImageGameId("/games/NM00010.elf", 100), "");
	EXPECT_EQ(LooseImageGameId("/games/NM00010.iso.gz", 100), "");
	EXPECT_EQ(LooseImageGameId("/games/NM00010.txt", 100), "");
}

TEST_F(SetClaimsTest, AnImageNamedAfterItsGameIsLooseUnlessAnAcgameHasThatGame)
{
	Put("loose/Battle Gear 3 (NM00010).chd");
	Put("loose/Battle Gear 3 (NM00010).bin", std::string(MIN_CARD_BYTES, 'd')); // Its dongle: claimed, not a game.
	Put("loose/NM00010 boot.elf");
	Put("loose/Some Other Game.chd");
	Put("folder/NM00005/game.iso");

	// Tekken 4 has an .acgame, so its image is the .acgame's, wherever it lies.
	Put("tekken/NM00004.acgame", "[game]\ngameid=NM00004\n");
	Put("tekken/NM00004 copy.chd");
	Put("tekken/NM00004/NM00004.chd");

	const SetClaims claims(Scan(m_root));
	EXPECT_TRUE(claims.IsLooseImage(P("loose/Battle Gear 3 (NM00010).chd")));
	EXPECT_TRUE(claims.IsLooseImage(P("folder/NM00005/game.iso")));
	EXPECT_FALSE(claims.IsLooseImage(P("loose/Battle Gear 3 (NM00010).bin")));
	EXPECT_FALSE(claims.IsLooseImage(P("loose/NM00010 boot.elf")));
	EXPECT_FALSE(claims.IsLooseImage(P("loose/Some Other Game.chd")));
	EXPECT_FALSE(claims.IsLooseImage(P("tekken/NM00004 copy.chd")));
	EXPECT_FALSE(claims.IsLooseImage(P("tekken/NM00004/NM00004.chd")));

	// A loose image is still claimed: nothing but the arcade code lists it.
	EXPECT_TRUE(claims.Owns(P("loose/Battle Gear 3 (NM00010).chd")));
}

TEST_F(SetClaimsTest, AFolderNamedAfterAGameOwnsTheSetInIt)
{
	Put("NM00010/boot.elf");
	Put("NM00010/game.chd");
	Put("NM00010/dongle.ps2");
	Put("NM00010/sram.bin"); // A board ROM by size: not a game either.
	Put("NM00010/readme.txt");
	Put("Homebrew/boot.elf");
	Put("Homebrew/game.chd");

	const SetClaims claims(Scan(m_root));
	EXPECT_TRUE(claims.Owns(P("NM00010/boot.elf")));
	EXPECT_TRUE(claims.Owns(P("NM00010/game.chd")));
	EXPECT_TRUE(claims.Owns(P("NM00010/dongle.ps2")));
	EXPECT_TRUE(claims.Owns(P("NM00010/sram.bin")));
	EXPECT_FALSE(claims.Owns(P("NM00010/readme.txt"))); // Not a file of a set.
	EXPECT_FALSE(claims.Owns(P("Homebrew/boot.elf")));
	EXPECT_FALSE(claims.Owns(P("Homebrew/game.chd")));
}

TEST_F(SetClaimsTest, NothingIsClaimedWhereThereIsNoArcade)
{
	Put("a.iso");
	Put("b.chd");
	Put("homebrew/hello.elf");
	Put("a game.bin");

	const SetClaims claims(Scan(m_root));
	EXPECT_TRUE(claims.Empty());
	EXPECT_FALSE(claims.Owns(P("a.iso")));
	EXPECT_FALSE(claims.Owns(P("homebrew/hello.elf")));
	EXPECT_FALSE(claims.Owns(P("a game.bin")));
}

TEST_F(SetClaimsTest, AnUnreadableAcgameClaimsNothing)
{
	Put("NM00004.acgame", "[game]\nname=no id here\n");
	Put("NM00004/boot.elf"); // Still named after a game, so the folder rule claims it.
	Put("elsewhere/boot.elf");
	Put("elsewhere/x.chd");

	const SetClaims claims(Scan(m_root));
	EXPECT_FALSE(claims.Owns(P("elsewhere/boot.elf")));
	EXPECT_FALSE(claims.Owns(P("elsewhere/x.chd")));
}
