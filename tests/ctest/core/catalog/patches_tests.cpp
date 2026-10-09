// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the online patch browser (PatchRepo.h): search, file list caching and back-off, and
// install. The .pnach text handling it relies on is tested in pnach_text_tests.cpp.
//
// The trees in fixtures/patches/ are the live GitHub file lists captured on 2026-10-08, cut down
// to the entries each rule needs; the real .pnach samples are in the same folder. Everything else
// a test downloads is a small file built in the test itself.

#include "catalog_test_support.h"

#include "FakeFetcher.h"

#include "Catalog/PatchRepo.h"
#include "Catalog/PnachText.h"

#include "Config.h"
#include "INISettingsInterface.h"
#include "Patch.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace Catalog;
using CatalogTest::FakeFetcher;
using CatalogTest::ReadFixture;
using CatalogTest::ScopedTempDir;

namespace
{
	constexpr u32 GOW_CRC = 0xD6385328;
	const char* const GOW_SERIAL = "SCUS-97399";

	const std::string TREE_OFFICIAL = "https://api.github.com/repos/PCSX2/pcsx2_patches/git/trees/main?recursive=1";
	const std::string TREE_GABOMINATED = "https://api.github.com/repos/Gabominated/PCSX2/git/trees/main?recursive=1";
	const std::string TREE_SHADOWNINJA = "https://api.github.com/repos/shadowninja826/pcsx2_pnach_cheats/git/trees/main?recursive=1";
	const std::string TREE_XS1L3N7X = "https://api.github.com/repos/xs1l3n7x/pcsx2_cheats_collection/git/trees/main?recursive=1";
	const std::string TREE_LARGE = "https://api.github.com/repos/XiGuanChi/PCSX2-CheatsDB/git/trees/main?recursive=1";

	std::string RawOfficial(const std::string& path) { return "https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/" + RawGithubPath(path); }
	std::string RawGabominated(const std::string& path) { return "https://raw.githubusercontent.com/Gabominated/PCSX2/main/" + RawGithubPath(path); }
	std::string RawShadowninja(const std::string& path) { return "https://raw.githubusercontent.com/shadowninja826/pcsx2_pnach_cheats/main/" + RawGithubPath(path); }
	std::string RawXs1l3n7x(const std::string& path) { return "https://raw.githubusercontent.com/xs1l3n7x/pcsx2_cheats_collection/main/" + RawGithubPath(path); }
	std::string RawLarge(const std::string& path) { return "https://raw.githubusercontent.com/XiGuanChi/PCSX2-CheatsDB/main/" + RawGithubPath(path); }

	/// A small .pnach file: a title line, then one block per {name, description} pair.
	std::string MakePnach(const std::string& title, const std::vector<std::pair<std::string, std::string>>& sections)
	{
		std::string out;
		if (!title.empty())
			out += "gametitle=" + title + "\n\n";
		for (const auto& [name, description] : sections)
		{
			out += "[" + name + "]\n";
			if (!description.empty())
				out += "comment=" + description + "\n";
			out += "patch=0,EE,00100000,word,00000001\n\n";
		}
		return out;
	}

	std::string BodyOf(const std::string& name, const std::string& description = {})
	{
		std::string out = "[" + name + "]\n";
		if (!description.empty())
			out += "comment=" + description + "\n";
		out += "patch=0,EE,00100000,word,00000001";
		return out;
	}

	std::vector<std::string> Names(const std::vector<PatchEntry>& entries)
	{
		std::vector<std::string> names;
		for (const PatchEntry& e : entries)
			names.push_back(e.name);
		return names;
	}

	std::vector<std::string> SectionNames(const std::string& pnach_text)
	{
		std::vector<std::string> names;
		for (const PnachSection& s : ParsePnach(pnach_text).sections)
			names.push_back(s.name);
		return names;
	}

	std::string ReadFile(const std::string& path)
	{
		return FileSystem::ReadFileToString(path.c_str()).value_or("<missing>");
	}

	PatchEntry MakeEntry(PatchKind kind, const std::string& name, const std::string& description = {})
	{
		PatchEntry e;
		e.source = (kind == PatchKind::Patch) ? PatchSourceId::Pcsx2Patches : PatchSourceId::Shadowninja826;
		e.kind = kind;
		e.file_path = "x.pnach";
		e.name = name;
		e.description = description;
		e.body = BodyOf(name, description);
		return e;
	}

	class PatchRepoTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_old_patches = EmuFolders::Patches;
			m_old_cheats = EmuFolders::Cheats;
			m_old_game_settings = EmuFolders::GameSettings;
			EmuFolders::Patches = m_dir.File("patches");
			EmuFolders::Cheats = m_dir.File("cheats");
			EmuFolders::GameSettings = m_dir.File("gamesettings");
			m_clock = std::time(nullptr);
		}

		void TearDown() override
		{
			EmuFolders::Patches = m_old_patches;
			EmuFolders::Cheats = m_old_cheats;
			EmuFolders::GameSettings = m_old_game_settings;
		}

		PatchRepoHooks Hooks()
		{
			PatchRepoHooks hooks;
			hooks.now = [this]() { return m_clock.load(); };
			hooks.is_game_running = [this](std::string_view serial, u32 crc) {
				m_running_query = std::string(serial) + "/" + std::to_string(crc);
				return m_game_running.load();
			};
			hooks.reload_patches = [this]() { m_reloads++; };
			hooks.is_hardcore_active = [this]() { return m_hardcore.load(); };
			return hooks;
		}

		std::unique_ptr<PatchRepo> MakeRepo()
		{
			return std::make_unique<PatchRepo>(m_http, ResponseCache(m_dir.File("trees")), Hooks());
		}

		/// The captured file lists of all five repositories.
		void ScriptTrees()
		{
			m_http.Ok(TREE_OFFICIAL, ReadFixture("patches/pcsx2_patches.tree.json"))
				.Ok(TREE_GABOMINATED, ReadFixture("patches/gabominated.tree.json"))
				.Ok(TREE_SHADOWNINJA, ReadFixture("patches/shadowninja826.tree.json"))
				.Ok(TREE_XS1L3N7X, ReadFixture("patches/xs1l3n7x.tree.json"))
				.Ok(TREE_LARGE, ReadFixture("patches/xiguanchi.tree.json"));
		}

		/// God of War as the four community sources know it. Official and Gabominated both have a
		/// "60 FPS"; shadowninja826 has the game under two names, spelling one cheat two ways, and
		/// xs1l3n7x repeats a cheat.
		void ScriptGodOfWar()
		{
			m_http.Ok(RawOfficial("patches/SCUS-97399_D6385328.pnach"),
				MakePnach("God of War [SCUS-97399]", {{"Widescreen 16:9", "Official widescreen"}, {"60 FPS", "Official 60 FPS"}}));
			m_http.Ok(RawGabominated("PCSX2 Patches/SCUS-97399_D6385328.pnach"),
				MakePnach("God of War", {{"60 FPS", "Gabominated 60 FPS"}, {"No Blur", "Removes the blur"}}));
			m_http.Ok(RawShadowninja("D6385328.pnach"), MakePnach("God of War", {{"Infinite Health", "Never die"}, {"Infinite Magic", ""}}));
			m_http.Ok(RawShadowninja("SCUS-97399_D6385328.pnach"), MakePnach("God of War", {{"infinite  health", "again"}, {"Max Orbs", ""}}));
			m_http.Ok(RawXs1l3n7x("cheats/D6385328.pnach"), MakePnach("God of War", {{"INFINITE HEALTH", ""}, {"Skip Cutscenes", ""}}));
			m_http.Status(RawXs1l3n7x("cheats/SCUS-97399_D6385328.pnach"), 404);
		}

		std::vector<std::string> RequestedUrls() const
		{
			std::vector<std::string> urls;
			for (const auto& request : m_http.Requests())
				urls.push_back(request.url);
			return urls;
		}

		PatchSearchRequest GodOfWarRequest() const
		{
			PatchSearchRequest request;
			request.serial = GOW_SERIAL;
			request.crc = GOW_CRC;
			return request;
		}

		PatchInstallRequest InstallRequest(std::vector<PatchEntry> entries, u32 crc = GOW_CRC) const
		{
			PatchInstallRequest request;
			request.serial = GOW_SERIAL;
			request.crc = crc;
			request.game_title = "God of War";
			request.entries = std::move(entries);
			return request;
		}

		std::string PatchesPnachPath() const { return Patch::GetPnachFilename(GOW_SERIAL, GOW_CRC, false); }
		std::string CheatsPnachPath() const { return Patch::GetPnachFilename(GOW_SERIAL, GOW_CRC, true); }
		std::string SettingsPath() const { return Path::Combine(EmuFolders::GameSettings, "SCUS-97399_D6385328.ini"); }

		std::vector<std::string> IniList(const std::string& path, const char* section, const char* key) const
		{
			INISettingsInterface ini(path);
			EXPECT_TRUE(ini.Load()) << path;
			return ini.GetStringList(section, key);
		}

		ScopedTempDir m_dir;
		FakeFetcher m_http;
		std::atomic<std::time_t> m_clock{0};
		std::atomic_bool m_game_running{false};
		std::atomic_bool m_hardcore{false};
		std::atomic_int m_reloads{0};
		std::string m_running_query;

	private:
		std::string m_old_patches, m_old_cheats, m_old_game_settings;
	};
} // namespace

// ---- Plain functions in PatchRepo.h ------------------------------------------------------------

TEST(PatchRepoMatching, CheatFilesMatchByCrcInTheFileNameOnly)
{
	const std::vector<std::string> tree = {
		"D6385328.pnach",
		"cheats/d6385328.pnach",
		"cheats-v1/D6385328 - God of War SCUS-97399.pnach",
		"D6385328/readme.pnach", // the CRC is in the folder, not the file name
		"cheats/SCUS-97399_D6385328.pnach",
		"cheats/SCUS-97399_AAAAAAAA.pnach", // another revision of the same game
		"cheats/12345678.pnach",
	};
	EXPECT_EQ(MatchCheatFiles(tree, "D6385328", "SCUS-97399"),
		(std::vector<std::string>{"D6385328.pnach", "cheats/d6385328.pnach", "cheats-v1/D6385328 - God of War SCUS-97399.pnach", "cheats/SCUS-97399_D6385328.pnach"}));
	EXPECT_EQ(MatchCheatFiles(tree, "d6385328", ""), MatchCheatFiles(tree, "D6385328", "SCUS-97399"));
}

TEST(PatchRepoMatching, CheatFilesFallBackToTheSerialOnlyWhenNoCrcMatches)
{
	const std::vector<std::string> tree = {
		"cheats/SCUS-97399_AAAAAAAA.pnach",
		"cheats-v1/BBBBBBBB - God of War SCUS-97399.pnach",
		"cheats/scus-97399_cccccccc.pnach",
		"cheats/SCUS-973990_DDDDDDDD.pnach", // a different serial that starts the same
		"cheats/XSCUS-97399.pnach", // no space before the serial
		"cheats/SCUS-97400_EEEEEEEE.pnach",
	};
	EXPECT_EQ(MatchCheatFiles(tree, "D6385328", "SCUS-97399"),
		(std::vector<std::string>{"cheats/SCUS-97399_AAAAAAAA.pnach", "cheats-v1/BBBBBBBB - God of War SCUS-97399.pnach", "cheats/scus-97399_cccccccc.pnach"}));
	EXPECT_TRUE(MatchCheatFiles(tree, "D6385328", "").empty());
	EXPECT_TRUE(MatchCheatFiles(tree, "", "").empty());
}

TEST(PatchRepoMatching, RawPathsArePercentEncodedPerCharacter)
{
	EXPECT_EQ(RawGithubPath("patches/SCUS-97399_D6385328.pnach"), "patches/SCUS-97399_D6385328.pnach");
	EXPECT_EQ(RawGithubPath("PCSX2 Patches/a b.pnach"), "PCSX2%20Patches/a%20b.pnach");
	EXPECT_EQ(RawGithubPath("cheats-v1/1E65A50E - Resident Evil Outbreak File #2 SLUS-20984.pnach"),
		"cheats-v1/1E65A50E%20-%20Resident%20Evil%20Outbreak%20File%20%232%20SLUS-20984.pnach");
	EXPECT_EQ(RawGithubPath("120% (x).pnach"), "120%25%20%28x%29.pnach");
	EXPECT_EQ(RawGithubPath("Le Soul\xC3\xA8vement.pnach"), "Le%20Soul%C3%A8vement.pnach");
}

TEST(PatchRepoMatching, ExtractsOnlyPnachBlobsFromATreeResponse)
{
	std::vector<std::string> paths;
	bool truncated = true;
	ASSERT_TRUE(ExtractPnachPaths(ReadFixture("patches/pcsx2_patches.tree.json"), &paths, &truncated));
	EXPECT_FALSE(truncated);
	EXPECT_GT(paths.size(), 20u);
	for (const std::string& path : paths)
	{
		EXPECT_EQ(path.rfind("patches/", 0), 0u) << path;
		EXPECT_EQ(path.substr(path.size() - 6), ".pnach") << path;
	}
	EXPECT_NE(std::find(paths.begin(), paths.end(), "patches/PAPX-90203_55CE5111.pnach"), paths.end());

	ASSERT_TRUE(ExtractPnachPaths(
		R"({"truncated":true,"tree":[{"path":"a.PNACH","type":"blob"},{"path":"dir.pnach","type":"tree"},{"path":"b.txt","type":"blob"},{"path":"c\nd.pnach","type":"blob"},{"type":"blob"},7]})",
		&paths, &truncated));
	EXPECT_TRUE(truncated);
	EXPECT_EQ(paths, (std::vector<std::string>{"a.PNACH"}));

	EXPECT_FALSE(ExtractPnachPaths("<html>rate limited</html>", &paths));
	EXPECT_FALSE(ExtractPnachPaths(R"({"message":"API rate limit exceeded"})", &paths));
	EXPECT_FALSE(ExtractPnachPaths("[]", &paths));
}

TEST(PatchRepoMatching, SourcesAreNamed)
{
	EXPECT_STREQ(PatchSourceRepository(PatchSourceId::Pcsx2Patches), "PCSX2/pcsx2_patches");
	EXPECT_STREQ(PatchSourceRepository(PatchSourceId::Gabominated), "Gabominated/PCSX2");
	EXPECT_STREQ(PatchSourceRepository(PatchSourceId::Shadowninja826), "shadowninja826/pcsx2_pnach_cheats");
	EXPECT_STREQ(PatchSourceRepository(PatchSourceId::Xs1l3n7x), "xs1l3n7x/pcsx2_cheats_collection");
	EXPECT_STREQ(PatchSourceRepository(PatchSourceId::XiGuanChi), "XiGuanChi/PCSX2-CheatsDB");
	EXPECT_STRNE(PatchSourceLabel(PatchSourceId::Pcsx2Patches), PatchSourceLabel(PatchSourceId::Gabominated));
}

// ---- Search ------------------------------------------------------------------------------------

TEST_F(PatchRepoTest, CommunitySearchMergesTheFourSourcesAndDropsDuplicates)
{
	ScriptTrees();
	ScriptGodOfWar();
	const PatchSearchResult result = MakeRepo()->Search(GodOfWarRequest());

	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_FALSE(result.incomplete);
	EXPECT_EQ(result.serial, "SCUS-97399");
	EXPECT_EQ(result.crc, GOW_CRC);
	EXPECT_FALSE(result.crc_from_database);
	EXPECT_EQ(result.game_title, "God of War [SCUS-97399]"); // the first file's title

	// Patches first (official, then Gabominated), then cheats; "60 FPS" once, the official one;
	// the cheat spelled three ways once, the first.
	ASSERT_EQ(Names(result.entries), (std::vector<std::string>{"Widescreen 16:9", "60 FPS", "No Blur", "Infinite Health", "Infinite Magic", "Max Orbs", "Skip Cutscenes"}));

	const PatchEntry& widescreen = result.entries[0];
	EXPECT_EQ(widescreen.source, PatchSourceId::Pcsx2Patches);
	EXPECT_EQ(widescreen.kind, PatchKind::Patch);
	EXPECT_EQ(widescreen.file_path, "patches/SCUS-97399_D6385328.pnach");
	EXPECT_EQ(widescreen.description, "Official widescreen");
	EXPECT_EQ(widescreen.body, BodyOf("Widescreen 16:9", "Official widescreen"));
	EXPECT_FALSE(widescreen.installed);

	EXPECT_EQ(result.entries[1].description, "Official 60 FPS");
	EXPECT_EQ(result.entries[2].source, PatchSourceId::Gabominated);
	EXPECT_EQ(result.entries[2].file_path, "PCSX2 Patches/SCUS-97399_D6385328.pnach");
	EXPECT_EQ(result.entries[3].source, PatchSourceId::Shadowninja826);
	EXPECT_EQ(result.entries[3].kind, PatchKind::Cheat);
	EXPECT_EQ(result.entries[3].file_path, "D6385328.pnach");
	EXPECT_EQ(result.entries[5].file_path, "SCUS-97399_D6385328.pnach");
	EXPECT_EQ(result.entries[6].source, PatchSourceId::Xs1l3n7x);
	EXPECT_EQ(result.entries[6].file_path, "cheats/D6385328.pnach");
}

TEST_F(PatchRepoTest, PatchesAreFetchedByNameAndCheatListsAreNeeded)
{
	ScriptTrees();
	ScriptGodOfWar();
	MakeRepo()->Search(GodOfWarRequest());

	// The patch repositories are asked for the exact file, with no file list; the first name that
	// exists wins, so "<CRC>.pnach" is never requested here.
	EXPECT_EQ(RequestedUrls(),
		(std::vector<std::string>{
			RawOfficial("patches/SCUS-97399_D6385328.pnach"),
			RawGabominated("PCSX2 Patches/SCUS-97399_D6385328.pnach"),
			TREE_SHADOWNINJA,
			RawShadowninja("D6385328.pnach"),
			RawShadowninja("SCUS-97399_D6385328.pnach"),
			TREE_XS1L3N7X,
			RawXs1l3n7x("cheats/D6385328.pnach"),
			RawXs1l3n7x("cheats/SCUS-97399_D6385328.pnach"),
		}));
	EXPECT_EQ(RawGabominated("PCSX2 Patches/SCUS-97399_D6385328.pnach"),
		"https://raw.githubusercontent.com/Gabominated/PCSX2/main/PCSX2%20Patches/SCUS-97399_D6385328.pnach");
}

TEST_F(PatchRepoTest, PatchSearchFallsBackToTheCrcOnlyName)
{
	// A serial the database does not file this CRC under: the "<SERIAL>_<CRC>" name is missing, the
	// old "<CRC>" name is there.
	ScriptTrees();
	m_http.Status(RawOfficial("patches/SLPM-11111_D6385328.pnach"), 404);
	m_http.Ok(RawOfficial("patches/D6385328.pnach"), MakePnach("Old Style Name", {{"Widescreen", "wide"}}));
	m_http.Status(RawGabominated("PCSX2 Patches/SLPM-11111_D6385328.pnach"), 404);
	m_http.Status(RawGabominated("PCSX2 Patches/D6385328.pnach"), 404);
	m_http.Status(RawShadowninja("D6385328.pnach"), 404);
	m_http.Status(RawShadowninja("SCUS-97399_D6385328.pnach"), 404);
	m_http.Status(RawXs1l3n7x("cheats/D6385328.pnach"), 404);
	m_http.Status(RawXs1l3n7x("cheats/SCUS-97399_D6385328.pnach"), 404);

	PatchSearchRequest request;
	request.serial = "SLPM-11111";
	request.crc = GOW_CRC;
	const PatchSearchResult result = MakeRepo()->Search(request);

	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	ASSERT_EQ(Names(result.entries), (std::vector<std::string>{"Widescreen"}));
	EXPECT_EQ(result.entries[0].file_path, "patches/D6385328.pnach");
	EXPECT_EQ(result.game_title, "Old Style Name");
	EXPECT_EQ(m_http.RequestCount(RawGabominated("PCSX2 Patches/D6385328.pnach")), 1u);
}

TEST_F(PatchRepoTest, ASearchWithoutASerialUsesTheCrcNamesOnly)
{
	ScriptTrees();
	m_http.Ok(RawOfficial("patches/D6385328.pnach"), MakePnach("T", {{"Widescreen", ""}}));
	m_http.Status(RawGabominated("PCSX2 Patches/D6385328.pnach"), 404);
	m_http.Status(RawShadowninja("D6385328.pnach"), 404);
	m_http.Status(RawShadowninja("SCUS-97399_D6385328.pnach"), 404);
	m_http.Status(RawXs1l3n7x("cheats/D6385328.pnach"), 404);
	m_http.Status(RawXs1l3n7x("cheats/SCUS-97399_D6385328.pnach"), 404);

	PatchSearchRequest request;
	request.crc = GOW_CRC;
	const PatchSearchResult result = MakeRepo()->Search(request);
	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_EQ(Names(result.entries), (std::vector<std::string>{"Widescreen"}));

	// One patch file name per repository, and no "<SERIAL>_" form to try.
	EXPECT_EQ(m_http.RequestCount(RawOfficial("patches/D6385328.pnach")), 1u);
	for (const std::string& url : RequestedUrls())
	{
		if (url.rfind("https://raw.githubusercontent.com/PCSX2/", 0) == 0 || url.rfind("https://raw.githubusercontent.com/Gabominated/", 0) == 0)
			EXPECT_EQ(url.find("_D6385328"), std::string::npos) << url;
	}
}

TEST_F(PatchRepoTest, TheCrcPicksTheRevision)
{
	// Resident Evil Outbreak File #2 has two revisions in the patch database; only the disc's own
	// CRC is fetched.
	ScriptTrees();
	m_http.Ok(RawOfficial("patches/SLUS-20984_1E65A50E.pnach"), MakePnach("RE Outbreak 2", {{"Widescreen", ""}}));
	m_http.Status(RawGabominated("PCSX2 Patches/SLUS-20984_1E65A50E.pnach"), 404);
	m_http.Status(RawGabominated("PCSX2 Patches/1E65A50E.pnach"), 404);

	PatchSearchRequest request;
	request.serial = "SLUS-20984";
	request.crc = 0x1E65A50E;
	const PatchSearchResult result = MakeRepo()->Search(request);
	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_EQ(m_http.RequestCount(RawOfficial("patches/SLUS-20984_1E65A59B.pnach")), 0u);
	EXPECT_EQ(result.entries[0].file_path, "patches/SLUS-20984_1E65A50E.pnach");
}

TEST_F(PatchRepoTest, TheSameCheatInTwoCommunityRepositoriesIsListedOnce)
{
	// The captured shadowninja826 and xs1l3n7x files for 02E1970F are byte for byte the same.
	ScriptTrees();
	m_http.Status(RawOfficial("patches/SLPS-20123_02E1970F.pnach"), 404);
	m_http.Status(RawOfficial("patches/02E1970F.pnach"), 404);
	m_http.Status(RawGabominated("PCSX2 Patches/SLPS-20123_02E1970F.pnach"), 404);
	m_http.Status(RawGabominated("PCSX2 Patches/02E1970F.pnach"), 404);
	m_http.Ok(RawShadowninja("02E1970F.pnach"), ReadFixture("patches/shadowninja826_02E1970F.pnach"));
	m_http.Ok(RawXs1l3n7x("cheats/02E1970F.pnach"), ReadFixture("patches/xs1l3n7x_02E1970F.pnach"));

	PatchSearchRequest request;
	request.serial = "SLPS-20123";
	request.crc = 0x02E1970F;
	const PatchSearchResult result = MakeRepo()->Search(request);
	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	ASSERT_EQ(result.entries.size(), 1u);
	EXPECT_EQ(result.entries[0].name, "Infinite Credits");
	EXPECT_EQ(result.entries[0].source, PatchSourceId::Shadowninja826);
	EXPECT_EQ(result.entries[0].kind, PatchKind::Cheat);
	EXPECT_EQ(result.game_title, "Sega Ages 2500 Series Vol.26 - Dynamite Deka [UNKNOWN] [02E1970F]");
	EXPECT_EQ(m_http.RequestCount(RawXs1l3n7x("cheats/02E1970F.pnach")), 1u); // fetched, then found redundant
}

TEST_F(PatchRepoTest, ASerialAloneSearchLooksTheGameUpInTheFileLists)
{
	ScriptTrees();
	m_http.Ok(RawOfficial("patches/PBPX-95201_2AF8719D.pnach"), MakePnach("Gran Turismo 3", {{"Widescreen", ""}}));

	PatchSearchRequest request;
	request.serial = "pbpx-95201";
	const PatchSearchResult result = MakeRepo()->Search(request);

	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	// The first of the serial's revisions in the list, and its CRC reported as the database's.
	EXPECT_EQ(result.entries[0].file_path, "patches/PBPX-95201_2AF8719D.pnach");
	EXPECT_EQ(result.crc, 0x2AF8719Du);
	EXPECT_TRUE(result.crc_from_database);
	EXPECT_EQ(m_http.RequestCount(TREE_OFFICIAL), 1u);
	EXPECT_EQ(m_http.RequestCount(TREE_GABOMINATED), 1u);
	EXPECT_EQ(m_http.RequestCount(RawOfficial("patches/PBPX-95201_31603A01.pnach")), 0u);
	EXPECT_FALSE(result.incomplete);
}

TEST_F(PatchRepoTest, ADatabaseFileNameWithoutAWholeCrcGivesNoCrc)
{
	// The database has "SLES-50396_B6F7BA5.pnach": seven digits, so no CRC can be read from it.
	ScriptTrees();
	m_http.Ok(RawOfficial("patches/SLES-50396_B6F7BA5.pnach"), MakePnach("T", {{"Widescreen", ""}}));

	PatchSearchRequest request;
	request.serial = "SLES-50396";
	const PatchSearchResult result = MakeRepo()->Search(request);
	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_EQ(result.crc, 0u);
	EXPECT_FALSE(result.crc_from_database);
}

TEST_F(PatchRepoTest, RejectsRequestsWithoutAUsableSerialOrCrc)
{
	ScriptTrees();
	auto repo = MakeRepo();
	for (const char* serial : {"", "SLUS/../97399", "SLUS 97399", "SLUS-97399.pnach", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})
	{
		PatchSearchRequest request;
		request.serial = serial;
		EXPECT_EQ(repo->Search(request).status, PatchSearchStatus::InvalidRequest) << serial;
	}

	PatchSearchRequest bad_serial_with_crc;
	bad_serial_with_crc.serial = "SLUS/../97399";
	bad_serial_with_crc.crc = GOW_CRC;
	EXPECT_EQ(repo->Search(bad_serial_with_crc).status, PatchSearchStatus::InvalidRequest);
	EXPECT_TRUE(m_http.Requests().empty());
}

TEST_F(PatchRepoTest, ReportsNoMatchesOnlyWhenEverySourceAnswered)
{
	ScriptTrees();
	m_http.Status(RawOfficial("patches/SLXX-99999_12345678.pnach"), 404);
	m_http.Status(RawOfficial("patches/12345678.pnach"), 404);
	m_http.Status(RawGabominated("PCSX2 Patches/SLXX-99999_12345678.pnach"), 404);
	m_http.Status(RawGabominated("PCSX2 Patches/12345678.pnach"), 404);

	PatchSearchRequest request;
	request.serial = "SLXX-99999";
	request.crc = 0x12345678;
	const PatchSearchResult result = MakeRepo()->Search(request);
	EXPECT_EQ(result.status, PatchSearchStatus::NoMatches);
	EXPECT_TRUE(result.entries.empty());
	EXPECT_FALSE(result.incomplete);
}

TEST_F(PatchRepoTest, ReportsUnreachableWhenNothingCameBackAndSomethingFailed)
{
	// Nothing scripted: every request fails like a dead connection.
	const PatchSearchResult result = MakeRepo()->Search(GodOfWarRequest());
	EXPECT_EQ(result.status, PatchSearchStatus::Unreachable);
	EXPECT_TRUE(result.entries.empty());
}

TEST_F(PatchRepoTest, AFailedSourceMakesAFoundListIncomplete)
{
	// shadowninja826's list fails; the rest answers.
	m_http.Status(TREE_SHADOWNINJA, 500)
		.Ok(TREE_XS1L3N7X, ReadFixture("patches/xs1l3n7x.tree.json"))
		.Ok(RawOfficial("patches/SCUS-97399_D6385328.pnach"), MakePnach("God of War", {{"Widescreen 16:9", ""}}))
		.Status(RawGabominated("PCSX2 Patches/SCUS-97399_D6385328.pnach"), 404)
		.Status(RawGabominated("PCSX2 Patches/D6385328.pnach"), 404)
		.Status(RawXs1l3n7x("cheats/D6385328.pnach"), 404)
		.Status(RawXs1l3n7x("cheats/SCUS-97399_D6385328.pnach"), 404);

	const PatchSearchResult result = MakeRepo()->Search(GodOfWarRequest());
	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_TRUE(result.incomplete);
	EXPECT_EQ(Names(result.entries), (std::vector<std::string>{"Widescreen 16:9"}));
}

// ---- File list cache and back-off ---------------------------------------------------------------

TEST_F(PatchRepoTest, FileListsAreKeptInMemoryAndOnDisk)
{
	ScriptTrees();
	ScriptGodOfWar();
	{
		auto repo = MakeRepo();
		repo->Search(GodOfWarRequest());
		repo->Search(GodOfWarRequest());
		EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 1u);
		EXPECT_EQ(m_http.RequestCount(TREE_XS1L3N7X), 1u);
	}

	// A new process finds the lists on disk, as one path per line.
	auto again = MakeRepo();
	again->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 1u);

	ResponseCache cache(m_dir.File("trees"));
	const auto stored = cache.Load("tree-shadowninja826.txt");
	ASSERT_TRUE(stored.has_value());
	EXPECT_NE(stored->body.find("\nSCUS-97399_D6385328.pnach\n"), std::string::npos);
	EXPECT_EQ(stored->body.find('{'), std::string::npos);
}

TEST_F(PatchRepoTest, FileListsAreRefetchedAfterAWeek)
{
	ScriptTrees();
	ScriptGodOfWar();
	auto repo = MakeRepo();
	repo->Search(GodOfWarRequest());

	m_clock += 6 * 24 * 3600;
	repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 1u);

	m_clock += 2 * 24 * 3600;
	repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 2u);
	EXPECT_EQ(m_http.RequestCount(TREE_XS1L3N7X), 2u);
}

TEST_F(PatchRepoTest, AnExpiredListStillServesWhenTheRefetchFails)
{
	m_http.Ok(TREE_SHADOWNINJA, ReadFixture("patches/shadowninja826.tree.json")).Status(TREE_SHADOWNINJA, 429);
	m_http.Ok(TREE_XS1L3N7X, ReadFixture("patches/xs1l3n7x.tree.json")).Status(TREE_XS1L3N7X, 429);
	ScriptGodOfWar();
	auto repo = MakeRepo();
	ASSERT_EQ(repo->Search(GodOfWarRequest()).status, PatchSearchStatus::Ok);

	m_clock += 8 * 24 * 3600; // expired; GitHub now says no
	PatchSearchResult result = repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 2u);
	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_FALSE(result.incomplete);
	EXPECT_EQ(result.entries.size(), 7u);

	// Backing off now, still served from the old copy.
	m_clock += 600;
	result = repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 2u);
	EXPECT_EQ(result.entries.size(), 7u);
}

class PatchRepoBackoffTest : public PatchRepoTest, public ::testing::WithParamInterface<std::pair<s32, int>>
{
};

TEST_P(PatchRepoBackoffTest, AFailedListIsNotAskedForAgainUntilTheWaitIsOver)
{
	const s32 status = GetParam().first;
	const int wait_seconds = GetParam().second;

	// Fails first, then works.
	if (status == 0)
		m_http.Fail(TREE_SHADOWNINJA, "connection reset");
	else
		m_http.Status(TREE_SHADOWNINJA, status);
	m_http.Ok(TREE_SHADOWNINJA, ReadFixture("patches/shadowninja826.tree.json"));
	m_http.Ok(TREE_XS1L3N7X, ReadFixture("patches/xs1l3n7x.tree.json"));
	ScriptGodOfWar();

	auto repo = MakeRepo();
	PatchSearchResult result = repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 1u);
	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_TRUE(result.incomplete);
	for (const PatchEntry& e : result.entries)
		EXPECT_NE(e.file_path, "D6385328.pnach");

	m_clock += wait_seconds - 1;
	result = repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 1u);
	EXPECT_TRUE(result.incomplete);

	m_clock += 2;
	result = repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 2u);
	EXPECT_FALSE(result.incomplete);
	EXPECT_EQ(result.entries.size(), 7u);

	// And it is remembered as good: no further requests.
	repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 2u);
}

INSTANTIATE_TEST_SUITE_P(PatchRepo, PatchRepoBackoffTest,
	::testing::Values(
		std::make_pair(429, 60 * 60), // rate limited
		std::make_pair(403, 60 * 60), // GitHub also uses 403 for that
		std::make_pair(500, 5 * 60),
		std::make_pair(404, 5 * 60),
		std::make_pair(0, 5 * 60))); // dropped connection

TEST_F(PatchRepoTest, ABodyThatIsNotAFileListIsAFailureToo)
{
	m_http.Ok(TREE_SHADOWNINJA, "<html>captive portal</html>");
	m_http.Ok(TREE_XS1L3N7X, ReadFixture("patches/xs1l3n7x.tree.json"));
	ScriptGodOfWar();
	auto repo = MakeRepo();
	EXPECT_TRUE(repo->Search(GodOfWarRequest()).incomplete);
	repo->Search(GodOfWarRequest());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 1u);

	ResponseCache cache(m_dir.File("trees"));
	EXPECT_FALSE(cache.Load("tree-shadowninja826.txt").has_value());
}

// ---- Large cheat database -----------------------------------------------------------------------

TEST_F(PatchRepoTest, LargeDatabaseOnlyCountsItsThreeCheatFolders)
{
	ScriptTrees();
	const std::string v1 = "cheats-v1/D6385328 - God of War SCUS-97399.pnach";
	const std::string v2 = "cheats-v2/D6385328 - God of War SCUS-97399.pnach";
	const std::string v6 = "cheats-v6/SCUS-97399_D6385328.pnach";
	m_http.Ok(RawLarge(v1), MakePnach("God of War", {{"Infinite Health", ""}, {"Unlock Everything", "Opens all"}}));
	m_http.Ok(RawLarge(v2), MakePnach("God of War", {{"Infinite Health", "second copy"}, {"Unlock Everything", ""}}));
	m_http.Ok(RawLarge(v6), MakePnach("God of War", {{"infinite  health", ""}, {"Red Orbs", ""}}));

	PatchSearchRequest request = GodOfWarRequest();
	request.scope = PatchSearchScope::LargeCheatDatabase;
	const PatchSearchResult result = MakeRepo()->Search(request);

	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	ASSERT_EQ(Names(result.entries), (std::vector<std::string>{"Infinite Health", "Unlock Everything", "Red Orbs"}));
	for (const PatchEntry& e : result.entries)
	{
		EXPECT_EQ(e.source, PatchSourceId::XiGuanChi);
		EXPECT_EQ(e.kind, PatchKind::Cheat);
	}
	EXPECT_EQ(result.entries[0].file_path, v1);
	EXPECT_EQ(result.entries[2].file_path, v6);

	// The tree also lists this game under "PCSX2 Patches/", cheats-v4/, cheats-v5/, patches-v2/ and
	// patches-v3/: copies of other sources, never requested. Nor are the other four sources.
	EXPECT_EQ(RequestedUrls(),
		(std::vector<std::string>{
			TREE_LARGE,
			RawLarge(v1),
			RawLarge(v2),
			RawLarge(v6),
		}));
	EXPECT_EQ(RawLarge(v1),
		"https://raw.githubusercontent.com/XiGuanChi/PCSX2-CheatsDB/main/cheats-v1/D6385328%20-%20God%20of%20War%20SCUS-97399.pnach");
}

TEST_F(PatchRepoTest, LargeDatabaseLeavesOutCheatsTheListAlreadyHas)
{
	ScriptTrees();
	const std::string v1 = "cheats-v1/D6385328 - God of War SCUS-97399.pnach";
	m_http.Ok(RawLarge(v1), MakePnach("God of War", {{"Infinite Health", ""}, {"Unlock Everything", ""}}));
	m_http.Ok(RawLarge("cheats-v2/D6385328 - God of War SCUS-97399.pnach"), MakePnach("God of War", {}));
	m_http.Ok(RawLarge("cheats-v6/SCUS-97399_D6385328.pnach"), MakePnach("God of War", {}));

	PatchSearchRequest request = GodOfWarRequest();
	request.scope = PatchSearchScope::LargeCheatDatabase;
	request.already_listed = {"  INFINITE health "};
	const PatchSearchResult result = MakeRepo()->Search(request);
	EXPECT_EQ(Names(result.entries), (std::vector<std::string>{"Unlock Everything"}));
}

TEST_F(PatchRepoTest, LargeDatabaseFindsAGameBySerialWhenItsCrcIsNotThere)
{
	ScriptTrees();
	// "<CRC> - <title> <SERIAL>.pnach", a non-ASCII title, and a CRC that is not the disc's.
	const std::string path = "cheats-v1/0F81D0B0 - Terminator 3 - Le Soul\xC3\xA8vement des Machines SLES-51957.pnach";
	m_http.Ok(RawLarge(path), MakePnach("Terminator 3", {{"Infinite Ammo", ""}}));

	PatchSearchRequest request;
	request.serial = "SLES-51957";
	request.crc = 0x11111111;
	request.scope = PatchSearchScope::LargeCheatDatabase;
	const PatchSearchResult result = MakeRepo()->Search(request);

	ASSERT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_EQ(result.entries[0].file_path, path);
	EXPECT_EQ(RawLarge(path),
		"https://raw.githubusercontent.com/XiGuanChi/PCSX2-CheatsDB/main/cheats-v1/0F81D0B0%20-%20Terminator%203%20-%20Le%20Soul%C3%A8vement%20des%20Machines%20SLES-51957.pnach");
}

TEST_F(PatchRepoTest, LargeDatabaseNamesWithHashAndPercentSurviveTheUrl)
{
	ScriptTrees();
	const std::string hash = "cheats-v1/1E65A50E - Resident Evil Outbreak File #2 SLUS-20984.pnach";
	const std::string percent = "cheats-v1/C80DF46D - The Battle of YuYu Hakushou - Shitou! Ankoku bBujutsukai 120% SLPS-25734.pnach";
	m_http.Ok(RawLarge(hash), MakePnach("RE", {{"A", ""}}));
	m_http.Ok(RawLarge("cheats-v2/1E65A50E - Resident Evil Outbreak File #2 SLUS-20984.pnach"), MakePnach("RE", {}));
	m_http.Ok(RawLarge(percent), MakePnach("YuYu", {{"B", ""}}));
	m_http.Ok(RawLarge("cheats-v2/C80DF46D - The Battle of YuYu Hakushou - Shitou! Ankoku bBujutsukai 120% SLPS-25734.pnach"), MakePnach("YuYu", {}));

	for (const auto& [serial, crc, expected] : std::vector<std::tuple<std::string, u32, std::string>>{
			 {"SLUS-20984", 0x1E65A50E, "A"}, {"SLPS-25734", 0xC80DF46D, "B"}})
	{
		PatchSearchRequest request;
		request.serial = serial;
		request.crc = crc;
		request.scope = PatchSearchScope::LargeCheatDatabase;
		const PatchSearchResult result = MakeRepo()->Search(request);
		ASSERT_EQ(result.status, PatchSearchStatus::Ok) << serial;
		EXPECT_EQ(Names(result.entries), (std::vector<std::string>{expected}));
	}
	EXPECT_NE(RawLarge(hash).find("%232"), std::string::npos);
	EXPECT_NE(RawLarge(percent).find("120%25"), std::string::npos);
}

// ---- Cancellation, progress, worker thread ------------------------------------------------------

TEST_F(PatchRepoTest, ACancelledTokenStopsBeforeAnyRequest)
{
	ScriptTrees();
	ScriptGodOfWar();
	CancelToken cancel;
	cancel.Cancel();
	const PatchSearchResult result = MakeRepo()->Search(GodOfWarRequest(), cancel);
	EXPECT_EQ(result.status, PatchSearchStatus::Cancelled);
	EXPECT_TRUE(result.entries.empty());
	EXPECT_TRUE(m_http.Requests().empty());
}

TEST_F(PatchRepoTest, CancellingBetweenSourcesStopsTheSearch)
{
	ScriptTrees();
	ScriptGodOfWar();
	CancelToken cancel;
	const PatchSearchResult result = MakeRepo()->Search(GodOfWarRequest(), cancel, [&](const PatchSearchProgress& p) {
		if (p.message == "Searching shadowninja826 cheats")
			cancel.Cancel();
	});
	EXPECT_EQ(result.status, PatchSearchStatus::Cancelled);
	EXPECT_TRUE(result.entries.empty());
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 0u);
	EXPECT_EQ(m_http.RequestCount(TREE_XS1L3N7X), 0u);
}

TEST_F(PatchRepoTest, ACancelledListRequestIsNotRememberedAsAFailure)
{
	ScriptTrees();
	ScriptGodOfWar();
	auto repo = MakeRepo();

	CancelToken cancel;
	const PatchSearchResult cancelled = repo->Search(GodOfWarRequest(), cancel, [&](const PatchSearchProgress& p) {
		if (p.message == "Searching shadowninja826 cheats")
			cancel.Cancel();
	});
	EXPECT_EQ(cancelled.status, PatchSearchStatus::Cancelled);

	// The next search may ask for the list at once: no back-off, no half-written cache.
	const PatchSearchResult again = repo->Search(GodOfWarRequest());
	EXPECT_EQ(again.status, PatchSearchStatus::Ok);
	EXPECT_FALSE(again.incomplete);
	EXPECT_EQ(m_http.RequestCount(TREE_SHADOWNINJA), 1u);
}

TEST_F(PatchRepoTest, ProgressNamesEachSourceAndEndsComplete)
{
	ScriptTrees();
	ScriptGodOfWar();
	std::vector<PatchSearchProgress> seen;
	MakeRepo()->Search(GodOfWarRequest(), {}, [&](const PatchSearchProgress& p) { seen.push_back(p); });

	ASSERT_EQ(seen.size(), 5u);
	const char* const messages[] = {"Searching PCSX2 patches", "Searching Gabominated", "Searching shadowninja826 cheats", "Searching xs1l3n7x cheats", "Done"};
	for (u32 i = 0; i < 5; i++)
	{
		EXPECT_EQ(seen[i].message, messages[i]);
		EXPECT_EQ(seen[i].done, i);
		EXPECT_EQ(seen[i].total, 4u);
	}

	seen.clear();
	PatchSearchRequest large = GodOfWarRequest();
	large.scope = PatchSearchScope::LargeCheatDatabase;
	MakeRepo()->Search(large, {}, [&](const PatchSearchProgress& p) { seen.push_back(p); });
	ASSERT_EQ(seen.size(), 2u);
	EXPECT_EQ(seen[0].total, 1u);
	EXPECT_EQ(seen[1].done, 1u);
	EXPECT_EQ(seen[1].message, "Done");
}

TEST_F(PatchRepoTest, StartSearchRunsOnAWorkerThreadAndReportsOnce)
{
	ScriptTrees();
	ScriptGodOfWar();
	auto repo = MakeRepo();

	std::promise<PatchSearchResult> promise;
	std::atomic_int calls{0};
	std::thread::id done_thread;
	std::atomic_int progress_calls{0};
	auto job = repo->StartSearch(
		GodOfWarRequest(), [&](const PatchSearchProgress&) { progress_calls++; },
		[&](PatchSearchResult result) {
			done_thread = std::this_thread::get_id();
			calls++;
			promise.set_value(std::move(result));
		});

	const PatchSearchResult result = promise.get_future().get();
	job->Wait();
	EXPECT_TRUE(job->IsFinished());
	EXPECT_EQ(result.status, PatchSearchStatus::Ok);
	EXPECT_EQ(result.entries.size(), 7u);
	EXPECT_NE(done_thread, std::this_thread::get_id());
	EXPECT_EQ(calls.load(), 1);
	EXPECT_EQ(progress_calls.load(), 5);
}

TEST_F(PatchRepoTest, ASearchJobCanBeCancelled)
{
	ScriptTrees();
	ScriptGodOfWar();
	auto repo = MakeRepo();

	std::promise<void> started, release;
	std::promise<PatchSearchResult> promise;
	auto job = repo->StartSearch(
		GodOfWarRequest(),
		[&](const PatchSearchProgress& p) {
			if (p.done == 0)
			{
				started.set_value();
				release.get_future().wait();
			}
		},
		[&](PatchSearchResult result) { promise.set_value(std::move(result)); });

	started.get_future().wait();
	EXPECT_FALSE(job->IsFinished());
	job->Cancel();
	release.set_value();

	EXPECT_EQ(promise.get_future().get().status, PatchSearchStatus::Cancelled);
	job->Wait();
	EXPECT_TRUE(m_http.Requests().empty());
}

TEST_F(PatchRepoTest, DestroyingAJobCancelsAndJoinsIt)
{
	ScriptTrees();
	ScriptGodOfWar();
	auto repo = MakeRepo();

	// The search may be held in its first progress callback when the job goes away; a helper
	// lets it go 50 ms later, long after the destructor has asked it to stop.
	std::promise<void> release;
	std::atomic_bool cancelled{false};
	std::thread releaser([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		release.set_value();
	});
	{
		auto job = repo->StartSearch(
			GodOfWarRequest(),
			[&](const PatchSearchProgress& p) {
				if (p.done == 0)
					release.get_future().wait();
			},
			[&](PatchSearchResult result) { cancelled = (result.status == PatchSearchStatus::Cancelled); });
	} // the destructor returns only once the thread has ended
	EXPECT_TRUE(cancelled.load());
	releaser.join();
}

// ---- Install -----------------------------------------------------------------------------------

TEST_F(PatchRepoTest, InstallWritesTheFilesTheEnableListsAndTheCheatSwitch)
{
	auto repo = MakeRepo();
	const PatchInstallResult result = repo->Install(InstallRequest({
		MakeEntry(PatchKind::Patch, "Widescreen 16:9", "wide"),
		MakeEntry(PatchKind::Cheat, "Infinite Health"),
		MakeEntry(PatchKind::Patch, "60 FPS"),
		MakeEntry(PatchKind::Cheat, "Max Orbs"),
	}));

	ASSERT_EQ(result.status, PatchInstallStatus::Ok) << result.error;
	EXPECT_EQ(result.patches_added, 2u);
	EXPECT_EQ(result.cheats_added, 2u);
	EXPECT_EQ(result.already_present, 0u);
	EXPECT_TRUE(result.cheats_switched_on);
	EXPECT_FALSE(result.reload_requested);

	// The names come from the core's own helper.
	EXPECT_EQ(PatchesPnachPath(), Path::Combine(EmuFolders::Patches, "SCUS-97399_D6385328.pnach"));
	EXPECT_EQ(CheatsPnachPath(), Path::Combine(EmuFolders::Cheats, "SCUS-97399_D6385328.pnach"));
	EXPECT_EQ(result.files_written, (std::vector<std::string>{PatchesPnachPath(), CheatsPnachPath()}));
	EXPECT_EQ(ReadFile(PatchesPnachPath()),
		"gametitle=God of War\n\n" + BodyOf("Widescreen 16:9", "wide") + "\n\n" + BodyOf("60 FPS") + "\n\n");
	EXPECT_EQ(ReadFile(CheatsPnachPath()),
		"gametitle=God of War\n\n" + BodyOf("Infinite Health") + "\n\n" + BodyOf("Max Orbs") + "\n\n");

	EXPECT_EQ(result.settings_file, SettingsPath());
	EXPECT_EQ(IniList(SettingsPath(), "Patches", "Enable"), (std::vector<std::string>{"Widescreen 16:9", "60 FPS"}));
	EXPECT_EQ(IniList(SettingsPath(), "Cheats", "Enable"), (std::vector<std::string>{"Infinite Health", "Max Orbs"}));
	INISettingsInterface ini(SettingsPath());
	ASSERT_TRUE(ini.Load());
	EXPECT_TRUE(ini.GetBoolValue("EmuCore", "EnableCheats", false));

	// The core finds what was written.
	EXPECT_EQ(SectionNames(ReadFile(PatchesPnachPath())), (std::vector<std::string>{"Widescreen 16:9", "60 FPS"}));
}

TEST_F(PatchRepoTest, InstallingOnlyPatchesLeavesCheatsAlone)
{
	const PatchInstallResult result = MakeRepo()->Install(InstallRequest({MakeEntry(PatchKind::Patch, "Widescreen 16:9")}));
	ASSERT_EQ(result.status, PatchInstallStatus::Ok) << result.error;
	EXPECT_FALSE(result.cheats_switched_on);
	EXPECT_FALSE(FileSystem::FileExists(CheatsPnachPath().c_str()));

	INISettingsInterface ini(SettingsPath());
	ASSERT_TRUE(ini.Load());
	EXPECT_FALSE(ini.ContainsValue("EmuCore", "EnableCheats"));
	EXPECT_FALSE(ini.ContainsValue("Cheats", "Enable"));
}

TEST_F(PatchRepoTest, InstallAppendsWithoutDuplicatingOrTouchingWhatIsThere)
{
	FileSystem::CreateDirectoryPath(EmuFolders::Patches.c_str(), true);
	FileSystem::CreateDirectoryPath(EmuFolders::Cheats.c_str(), true);
	const std::string mine = "gametitle=Mine\n// my own notes\n[Widescreen 16:9]\npatch=0,EE,00200000,word,00000002 // edited by hand";
	FileSystem::WriteStringToFile(PatchesPnachPath().c_str(), mine);
	FileSystem::WriteStringToFile(CheatsPnachPath().c_str(), "[infinite health]\npatch=1,EE,00300000,word,00000003\n");

	PatchEntry same_cheat_twice = MakeEntry(PatchKind::Cheat, "Max Orbs");
	const PatchInstallResult result = MakeRepo()->Install(InstallRequest({
		MakeEntry(PatchKind::Patch, "Widescreen 16:9"), // exact name is there: not rewritten
		MakeEntry(PatchKind::Patch, "widescreen 16:9"), // patches match exactly, so this one is new
		MakeEntry(PatchKind::Cheat, "Infinite  HEALTH"), // cheats ignore case and spacing
		same_cheat_twice,
		same_cheat_twice, // chosen twice, written once
	}));

	ASSERT_EQ(result.status, PatchInstallStatus::Ok) << result.error;
	EXPECT_EQ(result.patches_added, 1u);
	EXPECT_EQ(result.cheats_added, 1u);
	EXPECT_EQ(result.already_present, 3u);

	EXPECT_EQ(ReadFile(PatchesPnachPath()), mine + "\n\n" + BodyOf("widescreen 16:9") + "\n\n");
	EXPECT_EQ(ReadFile(CheatsPnachPath()), "[infinite health]\npatch=1,EE,00300000,word,00000003\n\n" + BodyOf("Max Orbs") + "\n\n");

	// Names already in the file are still switched on.
	EXPECT_EQ(IniList(SettingsPath(), "Patches", "Enable"), (std::vector<std::string>{"Widescreen 16:9", "widescreen 16:9"}));
	EXPECT_EQ(IniList(SettingsPath(), "Cheats", "Enable"), (std::vector<std::string>{"Infinite  HEALTH", "Max Orbs"}));
}

TEST_F(PatchRepoTest, InstallingTwiceChangesNothing)
{
	auto repo = MakeRepo();
	const PatchInstallRequest request = InstallRequest({MakeEntry(PatchKind::Patch, "A"), MakeEntry(PatchKind::Cheat, "B")});
	ASSERT_EQ(repo->Install(request).status, PatchInstallStatus::Ok);
	const std::string patches = ReadFile(PatchesPnachPath());
	const std::string cheats = ReadFile(CheatsPnachPath());
	const std::string ini = ReadFile(SettingsPath());

	const PatchInstallResult again = repo->Install(request);
	ASSERT_EQ(again.status, PatchInstallStatus::Ok);
	EXPECT_EQ(again.patches_added, 0u);
	EXPECT_EQ(again.cheats_added, 0u);
	EXPECT_EQ(again.already_present, 2u);
	EXPECT_TRUE(again.files_written.empty());
	EXPECT_EQ(ReadFile(PatchesPnachPath()), patches);
	EXPECT_EQ(ReadFile(CheatsPnachPath()), cheats);
	EXPECT_EQ(ReadFile(SettingsPath()), ini);
}

TEST_F(PatchRepoTest, InstallRefusesAnUnknownCrcAndWritesNothing)
{
	auto repo = MakeRepo();
	const PatchInstallResult result = repo->Install(InstallRequest({MakeEntry(PatchKind::Patch, "Widescreen 16:9"), MakeEntry(PatchKind::Cheat, "Cheat")}, 0));
	EXPECT_EQ(result.status, PatchInstallStatus::CrcUnknown);
	EXPECT_FALSE(result.error.empty());
	EXPECT_TRUE(result.files_written.empty());

	// Not even the folders: no "<serial>_00000000.pnach" and no settings file.
	EXPECT_FALSE(std::filesystem::exists(EmuFolders::Patches));
	EXPECT_FALSE(std::filesystem::exists(EmuFolders::Cheats));
	EXPECT_FALSE(std::filesystem::exists(EmuFolders::GameSettings));
}

TEST_F(PatchRepoTest, InstallChecksTheSerialAndThatSomethingWasChosen)
{
	auto repo = MakeRepo();

	PatchInstallRequest bad_serial = InstallRequest({MakeEntry(PatchKind::Patch, "A")});
	bad_serial.serial = "SCUS/../97399";
	EXPECT_EQ(repo->Install(bad_serial).status, PatchInstallStatus::InvalidSerial);

	EXPECT_EQ(repo->Install(InstallRequest({})).status, PatchInstallStatus::NothingSelected);

	PatchEntry nameless = MakeEntry(PatchKind::Patch, "A");
	nameless.name = "  ";
	PatchEntry bodiless = MakeEntry(PatchKind::Cheat, "B");
	bodiless.body.clear();
	EXPECT_EQ(repo->Install(InstallRequest({nameless, bodiless})).status, PatchInstallStatus::NothingSelected);

	EXPECT_FALSE(std::filesystem::exists(EmuFolders::Patches));
	EXPECT_FALSE(std::filesystem::exists(EmuFolders::GameSettings));
}

TEST_F(PatchRepoTest, InstallKeepsTheRestOfTheGameSettingsAndUndoesADisable)
{
	FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), true);
	{
		INISettingsInterface ini(SettingsPath());
		ini.SetIntValue("EmuCore", "Foo", 7);
		ini.SetBoolValue("EmuCore", "EnableCheats", false);
		ini.AddToStringList("Patches", "Enable", "Other Patch");
		ini.AddToStringList("Patches", "Disable", "Widescreen 16:9");
		ini.AddToStringList("Patches", "Disable", "No-Interlacing");
		ini.AddToStringList("Cheats", "Enable", "Other Cheat");
		ASSERT_TRUE(ini.Save());
	}

	const PatchInstallResult result = MakeRepo()->Install(InstallRequest({MakeEntry(PatchKind::Patch, "Widescreen 16:9"), MakeEntry(PatchKind::Cheat, "New Cheat")}));
	ASSERT_EQ(result.status, PatchInstallStatus::Ok) << result.error;

	INISettingsInterface ini(SettingsPath());
	ASSERT_TRUE(ini.Load());
	int foo = 0;
	EXPECT_TRUE(ini.GetIntValue("EmuCore", "Foo", &foo));
	EXPECT_EQ(foo, 7);
	EXPECT_TRUE(ini.GetBoolValue("EmuCore", "EnableCheats", false));
	EXPECT_EQ(ini.GetStringList("Patches", "Enable"), (std::vector<std::string>{"Other Patch", "Widescreen 16:9"}));
	EXPECT_EQ(ini.GetStringList("Patches", "Disable"), (std::vector<std::string>{"No-Interlacing"}));
	EXPECT_EQ(ini.GetStringList("Cheats", "Enable"), (std::vector<std::string>{"Other Cheat", "New Cheat"}));
}

TEST_F(PatchRepoTest, InstallUsesTheOlderCrcNamedSettingsFileWhenThatIsTheOnlyOne)
{
	FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), true);
	const std::string legacy = Path::Combine(EmuFolders::GameSettings, "D6385328.ini");
	{
		INISettingsInterface ini(legacy);
		ini.SetIntValue("EmuCore", "Foo", 7);
		ASSERT_TRUE(ini.Save());
	}

	const PatchInstallResult result = MakeRepo()->Install(InstallRequest({MakeEntry(PatchKind::Patch, "A")}));
	ASSERT_EQ(result.status, PatchInstallStatus::Ok) << result.error;
	EXPECT_EQ(result.settings_file, legacy);
	EXPECT_FALSE(FileSystem::FileExists(SettingsPath().c_str()));
	EXPECT_EQ(IniList(legacy, "Patches", "Enable"), (std::vector<std::string>{"A"}));

	// Once the serial-named file exists it is the one the core reads, so it wins.
	{
		INISettingsInterface ini(SettingsPath());
		ini.SetIntValue("EmuCore", "Bar", 1);
		ASSERT_TRUE(ini.Save());
	}
	EXPECT_EQ(MakeRepo()->Install(InstallRequest({MakeEntry(PatchKind::Patch, "B")})).settings_file, SettingsPath());
}

TEST_F(PatchRepoTest, InstallReloadsOnlyWhenTheGameIsRunning)
{
	auto repo = MakeRepo();
	PatchInstallResult result = repo->Install(InstallRequest({MakeEntry(PatchKind::Patch, "A")}));
	EXPECT_FALSE(result.reload_requested);
	EXPECT_EQ(m_reloads.load(), 0);
	EXPECT_EQ(m_running_query, "SCUS-97399/" + std::to_string(GOW_CRC)); // asked about this game, normalised

	m_game_running = true;
	result = repo->Install(InstallRequest({MakeEntry(PatchKind::Patch, "B")}));
	EXPECT_TRUE(result.reload_requested);
	EXPECT_EQ(m_reloads.load(), 1);

	// Nothing to reload when the install was refused.
	repo->Install(InstallRequest({MakeEntry(PatchKind::Patch, "C")}, 0));
	EXPECT_EQ(m_reloads.load(), 1);
}

TEST_F(PatchRepoTest, HardcoreIsReportedButDoesNotStopAnInstall)
{
	auto repo = MakeRepo();
	EXPECT_FALSE(repo->IsHardcoreActive());
	EXPECT_FALSE(repo->Install(InstallRequest({MakeEntry(PatchKind::Cheat, "A")})).hardcore_active);

	m_hardcore = true;
	EXPECT_TRUE(repo->IsHardcoreActive());
	const PatchInstallResult result = repo->Install(InstallRequest({MakeEntry(PatchKind::Cheat, "B")}));
	EXPECT_EQ(result.status, PatchInstallStatus::Ok);
	EXPECT_TRUE(result.hardcore_active);
	EXPECT_EQ(result.cheats_added, 1u);

	ScriptTrees();
	ScriptGodOfWar();
	EXPECT_TRUE(repo->Search(GodOfWarRequest()).hardcore_active);
}

TEST_F(PatchRepoTest, InstallReportsAFolderItCannotWriteAndSavesNoSettings)
{
	// A file where the patches folder should be.
	FileSystem::WriteStringToFile(m_dir.File("blocker").c_str(), "x");
	EmuFolders::Patches = m_dir.File("blocker/patches");

	const PatchInstallResult result = MakeRepo()->Install(InstallRequest({MakeEntry(PatchKind::Patch, "A")}));
	EXPECT_EQ(result.status, PatchInstallStatus::WriteFailed);
	EXPECT_FALSE(result.error.empty());
	EXPECT_FALSE(FileSystem::FileExists(SettingsPath().c_str()));
}

// ---- Search and install together ---------------------------------------------------------------

TEST_F(PatchRepoTest, InstalledEntriesAreFlaggedInTheNextSearch)
{
	ScriptTrees();
	ScriptGodOfWar();
	auto repo = MakeRepo();

	PatchSearchResult first = repo->Search(GodOfWarRequest());
	ASSERT_EQ(first.status, PatchSearchStatus::Ok);
	for (const PatchEntry& e : first.entries)
		EXPECT_FALSE(e.installed) << e.name;

	// Install the widescreen patch and two cheats.
	PatchInstallRequest install = InstallRequest({});
	install.game_title = first.game_title;
	for (const PatchEntry& e : first.entries)
	{
		if (e.name == "Widescreen 16:9" || e.name == "Infinite Health" || e.name == "Max Orbs")
			install.entries.push_back(e);
	}
	const PatchInstallResult result = repo->Install(install);
	ASSERT_EQ(result.status, PatchInstallStatus::Ok) << result.error;
	EXPECT_EQ(result.patches_added, 1u);
	EXPECT_EQ(result.cheats_added, 2u);

	// What was written is the section text as downloaded.
	EXPECT_EQ(ParsePnach(ReadFile(PatchesPnachPath())).sections[0].body, first.entries[0].body);
	EXPECT_EQ(ParsePnach(ReadFile(PatchesPnachPath())).game_title, "God of War [SCUS-97399]");

	// A refreshed list without searching again...
	std::vector<PatchEntry> refreshed = first.entries;
	repo->UpdateInstalledFlags(refreshed, GOW_SERIAL, GOW_CRC);
	// ...and a new search agree.
	const PatchSearchResult second = repo->Search(GodOfWarRequest());
	ASSERT_EQ(second.entries.size(), refreshed.size());
	for (size_t i = 0; i < refreshed.size(); i++)
	{
		const bool chosen = (refreshed[i].name == "Widescreen 16:9" || refreshed[i].name == "Infinite Health" || refreshed[i].name == "Max Orbs");
		EXPECT_EQ(refreshed[i].installed, chosen) << refreshed[i].name;
		EXPECT_EQ(second.entries[i].installed, chosen) << second.entries[i].name;
	}
}

TEST_F(PatchRepoTest, NothingIsFlaggedInstalledWithoutACrc)
{
	FileSystem::CreateDirectoryPath(EmuFolders::Patches.c_str(), true);
	FileSystem::WriteStringToFile(Path::Combine(EmuFolders::Patches, "SCUS-97399_00000000.pnach").c_str(), "[A]\npatch=1\n");

	auto repo = MakeRepo();
	std::vector<PatchEntry> entries = {MakeEntry(PatchKind::Patch, "A")};
	repo->UpdateInstalledFlags(entries, GOW_SERIAL, 0);
	EXPECT_FALSE(entries[0].installed);
}
