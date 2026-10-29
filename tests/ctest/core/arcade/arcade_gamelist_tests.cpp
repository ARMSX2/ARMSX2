// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Arcade games in the game list: an .acgame is a game of its own (serial = its game ID, CRC 0), it is found by
// a scan and survives the game list cache, and booting one must not set a disc source, which would take the
// boot away from the arcade branch of VMManager::Initialize.

#include <gtest/gtest.h>

#include "Arcade/AcGameFile.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "GameDatabase.h"
#include "GameList.h"
#include "Host.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	std::string Fixture(const char* name)
	{
		return Path::Combine(ARCADE_TEST_FIXTURES, name);
	}

	std::string ReadFile(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::stringstream text;
		text << in.rdbuf();
		return text.str();
	}

	void WriteFile(const std::string& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << content;
	}
} // namespace

TEST(ArcadeGameList, FileNames)
{
	EXPECT_TRUE(VMManager::IsArcadeGameFileName("/games/Tekken 4.acgame"));
	EXPECT_TRUE(VMManager::IsArcadeGameFileName("/games/TEKKEN.ACGAME"));
	EXPECT_FALSE(VMManager::IsArcadeGameFileName("/games/Tekken.iso"));

	// The open dialog and a dropped file take it like a disc image or an ELF.
	EXPECT_TRUE(VMManager::IsLoadableFileName("/games/Tekken 4.acgame"));
	EXPECT_TRUE(VMManager::IsLoadableFileName("/games/TEKKEN.ACGAME"));
	EXPECT_TRUE(VMManager::IsLoadableFileName("/games/game.iso"));
	EXPECT_TRUE(VMManager::IsLoadableFileName("/games/game.elf"));
	EXPECT_FALSE(VMManager::IsLoadableFileName("/games/game.txt"));
	// It is not a disc: the disc paths must not see it.
	EXPECT_FALSE(VMManager::IsDiscFileName("/games/Tekken 4.acgame"));
}

TEST(ArcadeGameList, AnEntryTypeHasAName)
{
	for (int type = 0; type < static_cast<int>(GameList::EntryType::Count); type++)
		EXPECT_NE(std::string(GameList::EntryTypeToString(static_cast<GameList::EntryType>(type), false)), "") << type;
	EXPECT_STREQ(GameList::EntryTypeToString(GameList::EntryType::Arcade, false), "Arcade");
	// The values already in game list caches keep their meaning.
	EXPECT_EQ(static_cast<int>(GameList::EntryType::PS2Disc), 0);
	EXPECT_EQ(static_cast<int>(GameList::EntryType::PS1Disc), 1);
	EXPECT_EQ(static_cast<int>(GameList::EntryType::ELF), 2);
	EXPECT_EQ(static_cast<int>(GameList::EntryType::Invalid), 3);
}

TEST(ArcadeGameList, AnAcgameIsAnArcadeEntryWithTheGameIDForSerialAndNoCRC)
{
	GameList::Entry entry;
	ASSERT_TRUE(GameList::PopulateEntryFromPath(Fixture("tekken4.acgame"), &entry));
	EXPECT_EQ(entry.type, GameList::EntryType::Arcade);
	EXPECT_TRUE(entry.IsArcade());
	EXPECT_FALSE(entry.IsDisc());
	EXPECT_EQ(entry.serial, "NM00004");
	EXPECT_EQ(entry.crc, 0u);
	EXPECT_EQ(entry.path, Fixture("tekken4.acgame"));
	// The .acgame's own name for the game.
	EXPECT_EQ(entry.title, "Tekken 4");
	EXPECT_EQ(entry.compatibility_rating, GameList::CompatibilityRating::Unknown);
}

TEST(ArcadeGameList, WithoutANameTheGameDatabaseNamesIt)
{
	GameList::Entry entry;
	ASSERT_TRUE(GameList::PopulateEntryFromPath(Fixture("minimal.acgame"), &entry));
	EXPECT_EQ(entry.type, GameList::EntryType::Arcade);
	EXPECT_EQ(entry.serial, "NM00023");
	EXPECT_EQ(entry.crc, 0u);
	if (const GameDatabaseSchema::GameEntry* db = GameDatabase::findGame("NM00023"))
		EXPECT_EQ(entry.title, db->name);
	else
		EXPECT_EQ(entry.title, "minimal"); // No database here: the file's name.
}

TEST(ArcadeGameList, AFileWithoutAGameIDIsInvalidAndNotAFailure)
{
	// Invalid entries are cached, so the file is not read again at every scan.
	for (const char* name : {"bad-gameid.acgame", "no-gameid.acgame"})
	{
		GameList::Entry entry;
		ASSERT_TRUE(GameList::PopulateEntryFromPath(Fixture(name), &entry)) << name;
		EXPECT_EQ(entry.type, GameList::EntryType::Invalid) << name;
	}

	GameList::Entry missing;
	EXPECT_FALSE(GameList::PopulateEntryFromPath(Fixture("does-not-exist.acgame"), &missing));
}

TEST(ArcadeGameList, AnAcgameSaysWhereItsMediaImageIs)
{
	const std::string dir(Path::GetDirectory(Fixture("tekken4.acgame")));

	// In the folder [data] subdir names...
	auto game = Arcade::ReadAcGameInfo(Fixture("tekken4.acgame"));
	ASSERT_TRUE(game.has_value());
	EXPECT_EQ(game->gameid, "NM00004");
	EXPECT_EQ(game->name, "Tekken 4");
	EXPECT_EQ(game->media_path, Path::Combine(Path::Combine(dir, "NM00004"), "NM00004.chd"));

	// ...in a folder named after the game when it names none, with the game ID's .chd...
	game = Arcade::ReadAcGameInfo(Fixture("minimal.acgame"));
	ASSERT_TRUE(game.has_value());
	EXPECT_EQ(game->name, "");
	EXPECT_EQ(game->media_path, Path::Combine(Path::Combine(dir, "NM00023"), "NM00023.chd"));

	// ...beside the .acgame when subdir is empty.
	game = Arcade::ReadAcGameInfo(Fixture("beside.acgame"));
	ASSERT_TRUE(game.has_value());
	EXPECT_EQ(game->media_path, Path::Combine(dir, "bg3.chd"));

	EXPECT_FALSE(Arcade::ReadAcGameInfo(Fixture("bad-gameid.acgame")).has_value());
	EXPECT_FALSE(Arcade::ReadAcGameInfo(Fixture("no-gameid.acgame")).has_value());
	EXPECT_FALSE(Arcade::ReadAcGameInfo(Fixture("does-not-exist.acgame")).has_value());
}

TEST(ArcadeGameList, AnAbsoluteMediaPathIsUsedAsItIsAndABigFileIsNotAnAcgame)
{
	const std::string dir = Path::Combine(testing::TempDir(), "arcade_acgame_files");
	ASSERT_TRUE(FileSystem::CreateDirectoryPath(dir.c_str(), true));

	const std::string absolute = Path::Combine(dir, "absolute.acgame");
	const std::string image = Path::Combine(dir, "images/NM00005.chd");
	WriteFile(absolute, "[game]\ngameid=NM00005\n[data]\nmediasrc=" + image + "\n");
	auto game = Arcade::ReadAcGameInfo(absolute);
	ASSERT_TRUE(game.has_value());
	EXPECT_EQ(game->media_path, image);

	// A valid game ID in a file that is not an .acgame's size.
	const std::string big = Path::Combine(dir, "big.acgame");
	WriteFile(big, "[game]\ngameid=NM00005\n" + std::string(70 * 1024, '#') + "\n");
	EXPECT_FALSE(Arcade::ReadAcGameInfo(big).has_value());

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

// An .acgame comes from wherever the player got it. Its dongle and card are joined to the memory cards folder and
// copied or mounted there, and its SRAM is written by the core, so a name that climbs out of its folder would
// write anywhere the player can.
namespace
{
	std::string AcgameWith(const std::string& key, const std::string& value)
	{
		return "[game]\ngameid=NM00005\n[data]\n" + key + "=" + value + "\n";
	}

	bool ReadableWith(const std::string& key, const std::string& value)
	{
		const std::string dir = Path::Combine(testing::TempDir(), "arcade_acgame_names");
		FileSystem::CreateDirectoryPath(dir.c_str(), true);
		const std::string path = Path::Combine(dir, "names.acgame");
		WriteFile(path, AcgameWith(key, value));
		return Arcade::ReadAcGameInfo(path).has_value();
	}
} // namespace

TEST(ArcadeGameList, ADongleOrCardThatIsNotAPlainFileNameIsNotAnAcgame)
{
	for (const char* key : {"dongle", "card"})
	{
		for (const char* name : {"../../../.config/autostart/x.desktop", "..", ".", "../NM00005.ps2", "sub/NM00005.ps2",
				 "sub\\NM00005.ps2", "..\\NM00005.ps2", "/etc/cron.d/x", "\\\\server\\share\\x", "C:\\x.ps2", "C:x.ps2"})
		{
			EXPECT_FALSE(ReadableWith(key, name)) << key << "=" << name;
		}
		for (const char* name : {"NM00005.ps2", "my card.bin", "NM00007.conquestcard", "..hidden.ps2", "a.b.c"})
			EXPECT_TRUE(ReadableWith(key, name)) << key << "=" << name;
	}
}

TEST(ArcadeGameList, AnSramOrSubdirThatLeavesTheGameFolderIsNotAnAcgame)
{
	for (const char* name : {"/home/user/.bashrc", "../sram.bin", "a/../../sram.bin", "a\\..\\..\\sram.bin", "..", "C:\\sram.bin", "\\\\server\\share\\sram.bin"})
	{
		EXPECT_FALSE(ReadableWith("sram", name)) << "sram=" << name;
		EXPECT_FALSE(ReadableWith("subdir", name)) << "subdir=" << name;
	}
	for (const char* name : {"sram.bin", "saves/sram.bin", "./sram.bin", "NM00005"})
		EXPECT_TRUE(ReadableWith("sram", name)) << "sram=" << name;
	for (const char* name : {"NM00005", "games/NM00005", ""})
		EXPECT_TRUE(ReadableWith("subdir", name)) << "subdir=" << name;

	// What the core only reads may still be anywhere.
	EXPECT_TRUE(ReadableWith("elf", "/mnt/games/NM00005/boot.elf"));
	EXPECT_TRUE(ReadableWith("mediasrc", "../images/NM00005.chd"));
}

TEST(ArcadeGameList, BootingAnArcadeEntryLeavesTheSourceTypeUnset)
{
	GameList::Entry entry;
	ASSERT_TRUE(GameList::PopulateEntryFromPath(Fixture("tekken4.acgame"), &entry));

	// Even when the caller had a disc source in hand.
	VMBootParameters params;
	params.source_type = CDVD_SourceType::Iso;
	params.elf_override = "/some/game.elf";
	GameList::FillBootParametersForEntry(&params, &entry);
	EXPECT_EQ(params.filename, Fixture("tekken4.acgame"));
	EXPECT_FALSE(params.source_type.has_value());
	EXPECT_TRUE(params.elf_override.empty());
	EXPECT_TRUE(VMManager::IsArcadeGameFileName(params.filename)); // So Initialize() takes the arcade branch.

	// A disc still sets its source.
	GameList::Entry disc;
	disc.type = GameList::EntryType::PS2Disc;
	disc.path = "/games/game.iso";
	VMBootParameters disc_params;
	GameList::FillBootParametersForEntry(&disc_params, &disc);
	ASSERT_TRUE(disc_params.source_type.has_value());
	EXPECT_EQ(disc_params.source_type.value(), CDVD_SourceType::Iso);
}

namespace
{
	// The process-wide base settings the game list reads its folders from; the base layer can be set only once.
	MemorySettingsInterface& HostBaseSettings()
	{
		static MemorySettingsInterface s_base;
		static const bool s_set = [] {
			Host::Internal::SetBaseSettingsLayer(&s_base);
			return true;
		}();
		(void)s_set;
		return s_base;
	}

	class ArcadeGameListScanTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_root = Path::Combine(testing::TempDir(), "arcade_gamelist_scan");
			std::error_code ec;
			std::filesystem::remove_all(m_root, ec);
			m_games = Path::Combine(m_root, "games");
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(m_games.c_str(), true));
			m_old_cache = EmuFolders::Cache;
			m_old_settings = EmuFolders::Settings;
			EmuFolders::Cache = Path::Combine(m_root, "cache");
			EmuFolders::Settings = Path::Combine(m_root, "settings");
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(EmuFolders::Cache.c_str(), true));
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(EmuFolders::Settings.c_str(), true));

			MemorySettingsInterface& base = HostBaseSettings();
			base.SetStringList("GameList", "Paths", {m_games});
			base.SetStringList("GameList", "RecursivePaths", {});
			base.SetStringList("GameList", "ExcludedPaths", {});

			for (const char* name : {"tekken4.acgame", "minimal.acgame", "bad-gameid.acgame", "no-gameid.acgame", "beside.acgame"})
				WriteFile(Path::Combine(m_games, name), ReadFile(Fixture(name)));
			// Arcade media images are discs by their file name; this one is not a readable one.
			WriteFile(Path::Combine(m_games, "bg3.chd"), "not a chd");
			// The media image the Tekken 4 .acgame names, in the folder it names.
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(Path::Combine(m_games, "NM00004").c_str(), true));
			WriteFile(Path::Combine(Path::Combine(m_games, "NM00004"), "NM00004.chd"), std::string(1234, 'x'));
		}

		void TearDown() override
		{
			EmuFolders::Cache = m_old_cache;
			EmuFolders::Settings = m_old_settings;
			std::error_code ec;
			std::filesystem::remove_all(m_root, ec);
		}

		static const GameList::Entry* Find(const std::string& serial)
		{
			return GameList::GetEntryBySerialAndCRC(serial, 0);
		}

		std::string m_root, m_games, m_old_cache, m_old_settings;
	};
} // namespace

TEST_F(ArcadeGameListScanTest, AScanListsTheValidAcgamesAndNothingElseInTheFolder)
{
	GameList::Refresh(true);
	auto lock = GameList::GetLock();

	// tekken4, minimal and beside; the files that name no game and the media image that is no disc are not listed.
	EXPECT_EQ(GameList::GetEntryCount(), 3u);
	const GameList::Entry* tekken = Find("NM00004");
	ASSERT_NE(tekken, nullptr);
	EXPECT_EQ(tekken->type, GameList::EntryType::Arcade);
	EXPECT_EQ(tekken->title, "Tekken 4");
	EXPECT_EQ(tekken->path, Path::Combine(m_games, "tekken4.acgame"));
	EXPECT_EQ(tekken->total_size, 1234u); // Its media image's size.

	const GameList::Entry* bg3 = Find("NM00010");
	ASSERT_NE(bg3, nullptr);
	EXPECT_EQ(bg3->title, "Battle Gear 3");
	EXPECT_EQ(bg3->total_size, 9u); // [data] subdir= is empty: beside the .acgame.

	const GameList::Entry* minimal = Find("NM00023");
	ASSERT_NE(minimal, nullptr);
	EXPECT_EQ(minimal->total_size, 0u); // No media image there: listed all the same.
}

TEST_F(ArcadeGameListScanTest, TheCacheKeepsArcadeEntriesWithoutRescanningTheFiles)
{
	GameList::Refresh(true);
	{
		auto lock = GameList::GetLock();
		ASSERT_EQ(GameList::GetEntryCount(), 3u);
	}

	// The cache alone, no scan: the files could be gone.
	GameList::Refresh(false, true);
	auto lock = GameList::GetLock();
	EXPECT_EQ(GameList::GetEntryCount(), 3u);
	const GameList::Entry* tekken = Find("NM00004");
	ASSERT_NE(tekken, nullptr);
	EXPECT_EQ(tekken->type, GameList::EntryType::Arcade);
	EXPECT_EQ(tekken->title, "Tekken 4");
	EXPECT_EQ(tekken->crc, 0u);
	EXPECT_EQ(tekken->total_size, 1234u);
	EXPECT_EQ(Find("NM00010")->type, GameList::EntryType::Arcade);
}

namespace
{
	// A scan of a games folder as an arcade collection keeps it. The images are real (tiny) CHD files: the scan
	// opens every .chd as a disc, and a made-up one that fails to open would hide what a real one does.
	class ArcadeSetScanTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_root = Path::Combine(testing::TempDir(), "arcade_gamelist_sets");
			std::error_code ec;
			std::filesystem::remove_all(m_root, ec);
			m_games = Path::Combine(m_root, "games");
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(m_games.c_str(), true));
			m_old_cache = EmuFolders::Cache;
			m_old_settings = EmuFolders::Settings;
			EmuFolders::Cache = Path::Combine(m_root, "cache");
			EmuFolders::Settings = Path::Combine(m_root, "settings");
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(EmuFolders::Cache.c_str(), true));
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(EmuFolders::Settings.c_str(), true));

			MemorySettingsInterface& base = HostBaseSettings();
			base.SetStringList("GameList", "Paths", {});
			base.SetStringList("GameList", "RecursivePaths", {m_games});
			base.SetStringList("GameList", "ExcludedPaths", {});
		}

		void TearDown() override
		{
			EmuFolders::Cache = m_old_cache;
			EmuFolders::Settings = m_old_settings;
			std::error_code ec;
			std::filesystem::remove_all(m_root, ec);
		}

		std::string Path_(const char* relative) const { return Path::Combine(m_games, relative); }

		void Put(const char* relative, const std::string& content)
		{
			const std::string path = Path_(relative);
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(std::string(Path::GetDirectory(path)).c_str(), true));
			WriteFile(path, content);
		}

		void PutFixture(const char* relative, const char* fixture) { Put(relative, ReadFile(Fixture(fixture))); }

		// A memory card file's size: a dongle in the .bin form, which looks like a disc by its name.
		void PutDongle(const char* relative) { Put(relative, std::string(8u << 20, '\0')); }

		std::vector<std::string> ListedPaths()
		{
			auto lock = GameList::GetLock();
			std::vector<std::string> paths;
			for (u32 i = 0; i < GameList::GetEntryCount(); i++)
				paths.push_back(GameList::GetEntryByIndex(i)->path);
			std::sort(paths.begin(), paths.end());
			return paths;
		}

		std::string m_root, m_games, m_old_cache, m_old_settings;
	};
} // namespace

TEST_F(ArcadeSetScanTest, TheFilesAnAcgameKeepForItselfAreNotListedAsDiscsOrELFs)
{
	// PCSX2x6's layout: the .acgame beside a folder named after the game, holding the boot program, the image
	// and the board's settings memory.
	PutFixture("NM00004.acgame", "tekken4.acgame");
	PutFixture("NM00004/boot.elf", "boot.elf");
	PutFixture("NM00004/NM00004.chd", "NM00004.chd");
	Put("NM00004/sram.bin", std::string(1024, 's'));
	Put("NM00004/hdd-writes.bin", std::string(4096, 'w'));

	// An .acgame with no game folder: the files it names are beside it, the dongle a .bin (a disc by its name).
	Put("beside/NM00010.acgame", "[game]\ngameid=NM00010\n[data]\nsubdir=\nelf=boot.elf\nmediasrc=bg3.chd\ndongle=NM00010.bin\n");
	PutFixture("beside/boot.elf", "boot.elf");
	PutFixture("beside/bg3.chd", "NM00010.chd");
	PutDongle("beside/NM00010.bin");

	// A homebrew ELF is a game all the same.
	PutFixture("homebrew/hello.elf", "boot.elf");

	std::vector<std::string> expected{Path_("beside/NM00010.acgame"), Path_("homebrew/hello.elf"), Path_("NM00004.acgame")};
	std::sort(expected.begin(), expected.end());

	GameList::Refresh(true);
	EXPECT_EQ(ListedPaths(), expected);

	// And the same from the cache.
	GameList::Refresh(false, true);
	EXPECT_EQ(ListedPaths(), expected);
}

TEST_F(ArcadeSetScanTest, ALooseImageIsAnArcadeGameAndItsDongleAndBootProgramAreNot)
{
	// No .acgame: the image named after its game, the dongle beside it, and the boot program of a folder named
	// after the game. The image is the game, listed as an arcade game and not as a PS2 disc.
	PutFixture("loose/Battle Gear 3 (NM00010).chd", "NM00010.chd");
	PutDongle("loose/Battle Gear 3 (NM00010).bin");
	PutFixture("NM00004/boot.elf", "boot.elf");
	PutFixture("NM00004/NM00004.chd", "NM00004.chd");
	PutFixture("homebrew/hello.elf", "boot.elf");

	std::vector<std::string> expected{
		Path_("homebrew/hello.elf"), Path_("loose/Battle Gear 3 (NM00010).chd"), Path_("NM00004/NM00004.chd")};
	std::sort(expected.begin(), expected.end());

	GameList::Refresh(true);
	EXPECT_EQ(ListedPaths(), expected);

	{
		auto lock = GameList::GetLock();
		const GameList::Entry* bg3 = GameList::GetEntryForPath(Path_("loose/Battle Gear 3 (NM00010).chd").c_str());
		ASSERT_NE(bg3, nullptr);
		EXPECT_EQ(bg3->type, GameList::EntryType::Arcade);
		EXPECT_EQ(bg3->serial, "NM00010");
		EXPECT_EQ(bg3->crc, 0u);
		EXPECT_GT(bg3->total_size, 0u);
		EXPECT_FALSE(bg3->title.empty());

		// The folder's name is the game's ID when the file's is not.
		const GameList::Entry* tekken = GameList::GetEntryForPath(Path_("NM00004/NM00004.chd").c_str());
		ASSERT_NE(tekken, nullptr);
		EXPECT_EQ(tekken->type, GameList::EntryType::Arcade);
		EXPECT_EQ(tekken->serial, "NM00004");

		// Booting it leaves the disc source unset: the host prepares the game, and the board's files are not a disc.
		VMBootParameters params;
		params.source_type = CDVD_SourceType::Iso;
		GameList::FillBootParametersForEntry(&params, bg3);
		EXPECT_EQ(params.filename, bg3->path);
		EXPECT_FALSE(params.source_type.has_value());
	}

	// And the same from the cache.
	GameList::Refresh(false, true);
	EXPECT_EQ(ListedPaths(), expected);
}

TEST_F(ArcadeSetScanTest, AnImageOfAGameThatHasAnAcgameIsTheAcgamesNotAGameOfItsOwn)
{
	// The .acgame looks for its image in NM00004/; this one lies beside it. It is still Tekken 4's.
	PutFixture("NM00004.acgame", "tekken4.acgame");
	PutFixture("NM00004.chd", "NM00004.chd");
	PutFixture("NM00004/boot.elf", "boot.elf");

	GameList::Refresh(true);
	EXPECT_EQ(ListedPaths(), (std::vector<std::string>{Path_("NM00004.acgame")}));
}

TEST_F(ArcadeSetScanTest, APackedImageAndABigDongleAreNotGames)
{
	PutFixture("packed/NM00010.chd", "NM00010.chd");
	std::filesystem::rename(Path_("packed/NM00010.chd"), Path_("packed/NM00010.chd.gz"));
	PutDongle("packed/NM00010.bin");
	PutDongle("other/NM00007.ps2");

	GameList::Refresh(true);
	EXPECT_EQ(ListedPaths(), std::vector<std::string>());
}
