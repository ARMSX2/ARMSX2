// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the glue between the cover job and the game list and settings: which entries get a
// cover job, and the INI keys (defaults, round trips, pins).

#include "Catalog/CoverAutoDownload.h"

#include "common/MemorySettingsInterface.h"

#include <gtest/gtest.h>

using namespace Catalog;

TEST(CoverAutoDownloadTest, GameListEntriesBecomeWorkItems)
{
	GameList::Entry entry;
	entry.path = "/games/kuon.iso";
	entry.serial = "SLUS-21007";
	entry.title = "Kuon";

	entry.type = GameList::EntryType::PS2Disc;
	auto item = AutoCovers::WorkItemForEntry(entry);
	ASSERT_TRUE(item.has_value());
	EXPECT_EQ(item->id, "/games/kuon.iso");
	EXPECT_EQ(item->serial, "SLUS-21007");
	EXPECT_EQ(item->title, "Kuon");
	EXPECT_EQ(item->platform, CoverPlatform::PS2);
	EXPECT_FALSE(item->force);

	entry.type = GameList::EntryType::PS1Disc;
	entry.serial = "SCUS-94900";
	item = AutoCovers::WorkItemForEntry(entry);
	ASSERT_TRUE(item.has_value());
	EXPECT_EQ(item->platform, CoverPlatform::PS1);

	entry.type = GameList::EntryType::Arcade;
	entry.serial = "NM00007";
	item = AutoCovers::WorkItemForEntry(entry);
	ASSERT_TRUE(item.has_value());
	EXPECT_EQ(item->platform, CoverPlatform::Arcade);

	// No cover source for these.
	entry.serial = "SLUS-21007"; // an arcade entry must carry an NM id
	EXPECT_FALSE(AutoCovers::WorkItemForEntry(entry).has_value());
	entry.type = GameList::EntryType::ELF;
	EXPECT_FALSE(AutoCovers::WorkItemForEntry(entry).has_value());
	entry.type = GameList::EntryType::Invalid;
	EXPECT_FALSE(AutoCovers::WorkItemForEntry(entry).has_value());
	entry.type = GameList::EntryType::PS2Disc;
	entry.serial.clear();
	EXPECT_FALSE(AutoCovers::WorkItemForEntry(entry).has_value());
	entry.serial = "../etc";
	EXPECT_FALSE(AutoCovers::WorkItemForEntry(entry).has_value());
}

TEST(CoverAutoDownloadTest, AutomaticDownloadIsOnByDefault)
{
	MemorySettingsInterface si;
	EXPECT_TRUE(AutoCovers::DEFAULT_AUTO_DOWNLOAD);
	EXPECT_TRUE(AutoCovers::IsEnabled(si));

	si.SetBoolValue("UI", "AutoDownloadCovers", false);
	EXPECT_FALSE(AutoCovers::IsEnabled(si));
	si.SetBoolValue("UI", "AutoDownloadCovers", true);
	EXPECT_TRUE(AutoCovers::IsEnabled(si));
}

TEST(CoverAutoDownloadTest, PreferencesDefaultToFlatCoversOfTheDiscsRegion)
{
	MemorySettingsInterface si;
	const CoverPreferences prefs = AutoCovers::LoadPreferences(si);
	EXPECT_EQ(prefs.style, CoverStyle::Flat2D);
	EXPECT_EQ(prefs.region, CoverRegion::Disc);
	EXPECT_TRUE(prefs.pins.empty());
	EXPECT_FALSE(prefs.NeedsRegionIndex());
}

TEST(CoverAutoDownloadTest, PreferencesRoundTripThroughTheIni)
{
	MemorySettingsInterface si;
	AutoCovers::SetStyle(si, CoverStyle::Case3D);
	AutoCovers::SetLibraryRegion(si, CoverRegion::Europe);
	AutoCovers::SetGameRegionPin(si, "slus-21007", CoverRegion::JapanAsia);
	AutoCovers::SetGameRegionPin(si, "SLES-50021", CoverRegion::Disc);

	EXPECT_TRUE(si.GetBoolValue("UI", "CoverArt3D", false));
	EXPECT_EQ(si.GetIntValue("UI", "CoverRegion", -1), 1 + 1);
	EXPECT_EQ(si.GetStringValue("UI", "CoverRegionPerGame", ""), "SLES-50021=0,SLUS-21007=3");

	const CoverPreferences prefs = AutoCovers::LoadPreferences(si);
	EXPECT_EQ(prefs.style, CoverStyle::Case3D);
	EXPECT_EQ(prefs.region, CoverRegion::Europe);
	EXPECT_EQ(prefs.EffectiveRegion("SLUS-21007"), CoverRegion::JapanAsia);
	EXPECT_EQ(prefs.EffectiveRegion("SLES-50021"), CoverRegion::Disc);

	// Removing the last pin deletes the key instead of leaving an empty string behind.
	AutoCovers::SetGameRegionPin(si, "SLUS-21007", std::nullopt);
	AutoCovers::SetGameRegionPin(si, "SLES-50021", std::nullopt);
	EXPECT_FALSE(si.ContainsValue("UI", "CoverRegionPerGame"));

	// A region outside 0..3 in a hand-edited INI is clamped.
	si.SetIntValue("UI", "CoverRegion", 7);
	EXPECT_EQ(AutoCovers::LoadPreferences(si).region, CoverRegion::JapanAsia);
}

TEST(CoverAutoDownloadTest, ShutdownWhenNothingRanIsHarmless)
{
	EXPECT_FALSE(AutoCovers::IsRunning());
	AutoCovers::Cancel();
	AutoCovers::Shutdown();
	AutoCovers::Shutdown();
}
