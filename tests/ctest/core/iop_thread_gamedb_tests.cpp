// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The game database entries that switch the IOP thread on for God of War, God of War II and Shadow of the Colossus,
// read from the real files: bin/resources/GameIndex.yaml with the ARMSX2 overlay (bin/resources-overlay/
// armsx2_overrides.yaml) on top, the way a build that ships the overlay (Android, iOS, ARM64 Linux) loads them.
//
// Every retail serial of the three titles is listed here by hand. The demos and trials are left out on purpose.
// Add a serial to the list when GameIndex.yaml gains a retail release of one of these games.

#include <gtest/gtest.h>

#include "Config.h"
#include "GameDatabase.h"
#include "IopThread/IopThreadMode.h"
#include "PerGameOverrides.h"

#include "common/MemorySettingsInterface.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	// Retail releases of God of War, in the order GameIndex.yaml lists them by region.
	const std::vector<const char*> kGodOfWar = {
		"SCUS-97399", "SCES-51533", "SCES-53133", "SCAJ-30010", "SCKA-30002", "SCKA-30003",
		"SLPM-66167", "SLPM-67010", "SLPM-67011", "SLPM-67012"};

	// God of War II keeps its upstream mvuFlag: 0 in the overlay, because an overlay speedHacks list replaces the upstream one.
	const std::vector<const char*> kGodOfWarII = {
		"SCUS-97481", "SCES-54206", "SCAJ-20190", "SCAJ-30011", "SCKA-30006", "SCKA-30007",
		"SLPM-67013", "SLPM-67017"};

	// Shadow of the Colossus, including the two Korean releases ("Wander and the Colossus" in the database).
	const std::vector<const char*> kShadowOfTheColossus = {
		"SCUS-97472", "SCES-53326", "SCPS-15097", "SCPS-19320", "SCPS-19335", "SCKA-20061", "SCKA-20106"};

	// What the real files give a serial. The database loads once per process, from EmuFolders::Resources, so the files are
	// copied into one folder where the loader looks for them, and the folder is set before the first lookup.
	class IopThreadGameDb : public ::testing::Test
	{
	public:
		static void SetUpTestSuite()
		{
			s_dir = fs::temp_directory_path() / ("armsx2-iop-gamedb-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			fs::create_directories(s_dir);
			fs::copy_file(ARMSX2_GAMEINDEX_YAML, s_dir / "GameIndex.yaml");
			fs::copy_file(ARMSX2_GAMEDB_OVERLAY_YAML, s_dir / "armsx2_overrides.yaml");

			s_previous_resources = EmuFolders::Resources;
			EmuFolders::Resources = s_dir.string();
			GameDatabase::ensureLoaded();
		}

		static void TearDownTestSuite()
		{
			EmuFolders::Resources = s_previous_resources;
			std::error_code ignored;
			fs::remove_all(s_dir, ignored);
		}

	protected:
		static const GameDatabaseSchema::GameEntry* Entry(const char* serial)
		{
			return GameDatabase::findGame(serial);
		}

		static std::vector<const char*> AllSerials()
		{
			std::vector<const char*> all;
			for (const auto* list : {&kGodOfWar, &kGodOfWarII, &kShadowOfTheColossus})
				all.insert(all.end(), list->begin(), list->end());
			return all;
		}

		static std::optional<int> SpeedHackValue(const GameDatabaseSchema::GameEntry& entry, SpeedHack hack)
		{
			for (const auto& [id, value] : entry.speedHacks)
			{
				if (id == hack)
					return value;
			}
			return std::nullopt;
		}

		static IopThread::HostCpuInfo Host(bool arm64, u32 total_cores, u32 performance_cores)
		{
			IopThread::HostCpuInfo host;
			host.is_arm64 = arm64;
			host.total_cores = total_cores;
			host.performance_cores = performance_cores;
			return host;
		}

		inline static fs::path s_dir;
		inline static std::string s_previous_resources;
	};
} // namespace

TEST_F(IopThreadGameDb, EveryRetailSerialOfTheThreeTitlesIsInTheDatabase)
{
	// If GameIndex.yaml renames or drops one, this says which, instead of a null entry further down.
	for (const char* serial : AllSerials())
		EXPECT_NE(Entry(serial), nullptr) << serial;
}

TEST_F(IopThreadGameDb, EveryRetailSerialOfTheThreeTitlesAsksForTheIopThread)
{
	for (const char* serial : AllSerials())
	{
		const GameDatabaseSchema::GameEntry* entry = Entry(serial);
		ASSERT_NE(entry, nullptr) << serial;
		EXPECT_EQ(SpeedHackValue(*entry, SpeedHack::IOPThread), std::optional<int>(1)) << serial;
	}
}

TEST_F(IopThreadGameDb, GodOfWarIIKeepsItsUpstreamMvuFlag)
{
	for (const char* serial : kGodOfWarII)
	{
		const GameDatabaseSchema::GameEntry* entry = Entry(serial);
		ASSERT_NE(entry, nullptr) << serial;
		EXPECT_EQ(SpeedHackValue(*entry, SpeedHack::MVUFlag), std::optional<int>(0)) << serial;
	}
}

TEST_F(IopThreadGameDb, TheKoreanBigHitReleaseKeepsItsUpstreamGraphicsFixes)
{
	// SCKA-20106 has an overlay entry of its own now. It names speedHacks only, so the graphics fixes the upstream file
	// gives it must still be there.
	const GameDatabaseSchema::GameEntry* entry = Entry("SCKA-20106");
	ASSERT_NE(entry, nullptr);
	EXPECT_FALSE(entry->gsHWFixes.empty());
}

TEST_F(IopThreadGameDb, GodOfWarBindsTheIopToTheTightLagForItsStreamedAudio)
{
	// At the one-frame default, streamed FMV speech repeats in 150 ms blocks (see the overlay comment).
	std::vector<const char*> serials = kGodOfWar;
	serials.insert(serials.end(), kGodOfWarII.begin(), kGodOfWarII.end());
	for (const char* serial : serials)
	{
		const GameDatabaseSchema::GameEntry* entry = Entry(serial);
		ASSERT_NE(entry, nullptr) << serial;
		EXPECT_EQ(SpeedHackValue(*entry, SpeedHack::IOPThreadLag), std::optional<int>(IopThread::kDefaultLag)) << serial;

		Pcsx2Config config;
		entry->applyGameFixes(config, true, {}, GameDatabaseSchema::ApplyMode::Hypothetical);
		EXPECT_EQ(config.Speedhacks.iopThreadLag, IopThread::kDefaultLag) << serial;
	}
}

TEST_F(IopThreadGameDb, ShadowOfTheColossusKeepsTheSettingsLag)
{
	for (const char* serial : kShadowOfTheColossus)
	{
		const GameDatabaseSchema::GameEntry* entry = Entry(serial);
		ASSERT_NE(entry, nullptr) << serial;
		EXPECT_EQ(SpeedHackValue(*entry, SpeedHack::IOPThreadLag), std::nullopt) << serial;
	}
}

TEST_F(IopThreadGameDb, TheEntryTurnsTheThreadOnOverTheDefaultOfOff)
{
	for (const char* serial : AllSerials())
	{
		const GameDatabaseSchema::GameEntry* entry = Entry(serial);
		ASSERT_NE(entry, nullptr) << serial;

		Pcsx2Config config;
		config.Speedhacks.iopThread = false;
		entry->applyGameFixes(config, true, {}, GameDatabaseSchema::ApplyMode::Hypothetical);
		EXPECT_TRUE(config.Speedhacks.iopThread) << serial;
	}
}

TEST_F(IopThreadGameDb, APlayersPerGameOffStillWins)
{
	MemorySettingsInterface game_file;
	game_file.SetBoolValue("EmuCore/Speedhacks", "iopThread", false);
	const PerGameOverrides overrides = ComputePerGameOverrides(game_file);

	for (const char* serial : AllSerials())
	{
		const GameDatabaseSchema::GameEntry* entry = Entry(serial);
		ASSERT_NE(entry, nullptr) << serial;

		Pcsx2Config config;
		config.Speedhacks.iopThread = false;
		entry->applyGameFixes(config, true, overrides, GameDatabaseSchema::ApplyMode::Hypothetical);
		EXPECT_FALSE(config.Speedhacks.iopThread) << serial;
	}
}

TEST_F(IopThreadGameDb, TheHostRulesStillApplyToATitleTheEntryTurnsOn)
{
	const GameDatabaseSchema::GameEntry* entry = Entry("SCUS-97481");
	ASSERT_NE(entry, nullptr);

	Pcsx2Config config;
	config.Speedhacks.iopThread = false;
	entry->applyGameFixes(config, true, {}, GameDatabaseSchema::ApplyMode::Hypothetical);
	ASSERT_TRUE(config.Speedhacks.iopThread);

	// A host that can run it: the entry's thread is Threaded.
	EXPECT_EQ(IopThread::ResolveMode(config, Host(true, 8, 2), {}), IopThreadMode::Threaded); // RG 55G1
	EXPECT_EQ(IopThread::ResolveMode(config, Host(true, 8, 4), {}), IopThreadMode::Threaded); // SD865
	// Hosts that cannot: the entry does not get around the rules.
	EXPECT_EQ(IopThread::ResolveMode(config, Host(true, 4, 4), {}), IopThreadMode::Off); // four alike cores
	EXPECT_EQ(IopThread::ResolveMode(config, Host(true, 8, 1), {}), IopThreadMode::Off); // one fast core
	EXPECT_EQ(IopThread::ResolveMode(config, Host(false, 16, 8), {}), IopThreadMode::Off); // not ARM64
}
