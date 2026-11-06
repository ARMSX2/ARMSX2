// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The game database entries that move Burnout Revenge from halfPixelOffset 4 to 5, read from the real files:
// bin/resources/GameIndex.yaml with the ARMSX2 overlay (bin/resources-overlay/armsx2_overrides.yaml) on top, the way a
// build that ships the overlay (Android, iOS, ARM64 Linux) loads them. The overlay comment above the entries says why.

#include <gtest/gtest.h>

#include "Config.h"
#include "GameDatabase.h"
#include "GS/GS.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	// Every Burnout Revenge serial GameIndex.yaml lists, demos included: they all carry the same upstream block.
	const std::vector<const char*> kBurnoutRevenge = {
		"SLUS-21242", "SLUS-29153", "SLES-53506", "SLES-53507", "SLED-53512", "SLED-53637",
		"SLAJ-25066", "SLKA-25304", "SLPM-55004", "SLPM-66108", "SLPM-66652"};

	// The database loads once per process, from EmuFolders::Resources, so the files are copied into one folder where the
	// loader looks for them, and the folder is set before the first lookup.
	class BurnoutGameDb : public ::testing::Test
	{
	public:
		static void SetUpTestSuite()
		{
			s_dir = fs::temp_directory_path() / ("armsx2-burnout-gamedb-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
		static std::optional<s32> Fix(const GameDatabaseSchema::GameEntry& entry, GameDatabaseSchema::GSHWFixId id)
		{
			for (const auto& [fix, value] : entry.gsHWFixes)
			{
				if (fix == id)
					return value;
			}
			return std::nullopt;
		}

		inline static fs::path s_dir;
		inline static std::string s_previous_resources;
	};
} // namespace

TEST_F(BurnoutGameDb, EveryBurnoutRevengeSerialAlignsToNativeWithTheTextureOffset)
{
	using GameDatabaseSchema::GSHWFixId;
	for (const char* serial : kBurnoutRevenge)
	{
		const GameDatabaseSchema::GameEntry* entry = GameDatabase::findGame(serial);
		ASSERT_NE(entry, nullptr) << serial;
		EXPECT_EQ(Fix(*entry, GSHWFixId::HalfPixelOffset), std::optional<s32>(static_cast<s32>(GSHalfPixelOffset::NativeWTexOffset))) << serial;
	}
}

TEST_F(BurnoutGameDb, TheRestOfTheUpstreamBlockIsRestated)
{
	// An overlay gsHWFixes block replaces upstream's whole block, so the sun occlusion palette (gpuTargetCLUT) and the
	// bloom state machine (getSkipCount / beforeDraw) must still be there.
	using GameDatabaseSchema::GSHWFixId;
	for (const char* serial : kBurnoutRevenge)
	{
		const GameDatabaseSchema::GameEntry* entry = GameDatabase::findGame(serial);
		ASSERT_NE(entry, nullptr) << serial;
		EXPECT_EQ(Fix(*entry, GSHWFixId::GPUTargetCLUT), std::optional<s32>(1)) << serial;
		EXPECT_EQ(Fix(*entry, GSHWFixId::AutoFlush), std::optional<s32>(2)) << serial;
		EXPECT_EQ(Fix(*entry, GSHWFixId::NativeScaling), std::optional<s32>(2)) << serial;
		EXPECT_EQ(Fix(*entry, GSHWFixId::BilinearUpscale), std::optional<s32>(2)) << serial;
		EXPECT_EQ(Fix(*entry, GSHWFixId::GetSkipCount), std::optional<s32>(GSLookupGetSkipCountFunctionId("GSC_BurnoutGames"))) << serial;
		EXPECT_EQ(Fix(*entry, GSHWFixId::BeforeDraw), std::optional<s32>(GSLookupBeforeDrawFunctionId("OI_BurnoutGames"))) << serial;
	}
}
