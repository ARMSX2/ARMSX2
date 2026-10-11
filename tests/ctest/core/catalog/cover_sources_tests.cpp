// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for where a game's cover comes from: URL resolution per platform and style, the
// cover-region index built from GameIndex.yaml, and the library-wide and per-game region pins.
// The fixtures are real entries and URLs from the live captures of 2026-10-08.

#include "cover_test_support.h"

#include "Catalog/CoverSources.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

using namespace Catalog;
using CoverTest::ARCADE_ROOT;
using CoverTest::ExcerptIndex;
using CoverTest::Ps2Jpg;
using CoverTest::Ps2Png;
using CoverTest::PSX_REPO;

// ---- URL resolution ------------------------------------------------------------------------

TEST(CoverUrls, EveryTemplateMatchesTheLiveCaptures)
{
	EXPECT_EQ(CoverUrl(CoverPlatform::PS2, "SLUS-21376", CoverStyle::Flat2D), "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/SLUS-21376.jpg");
	EXPECT_EQ(CoverUrl(CoverPlatform::PS2, "SLUS-21376", CoverStyle::Case3D), "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/3d/SLUS-21376.png");
	EXPECT_EQ(CoverUrl(CoverPlatform::PS1, "SCUS-94900", CoverStyle::Flat2D), "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/SCUS-94900.jpg");
	EXPECT_EQ(CoverUrl(CoverPlatform::PS1, "SCUS-94900", CoverStyle::Case3D), "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/3d/SCUS-94900.png");
	EXPECT_EQ(CoverUrl(CoverPlatform::Arcade, "NM00007", CoverStyle::Flat2D), "https://arcade.ps2ktxpak.net/arcade-covers/NM00007.jpg");
	EXPECT_EQ(CoverUrl(CoverPlatform::Arcade, "NM00007", CoverStyle::Case3D), "https://arcade.ps2ktxpak.net/arcade-covers/3d/NM00007.png");
}

TEST(CoverUrls, AnArcadeGameIdAlwaysGoesToTheArcadeServer)
{
	EXPECT_EQ(CoverUrl(CoverPlatform::PS2, "NM00007", CoverStyle::Flat2D), "https://arcade.ps2ktxpak.net/arcade-covers/NM00007.jpg");
	EXPECT_EQ(CoverUrl(CoverPlatform::PS1, "NM00007", CoverStyle::Case3D), "https://arcade.ps2ktxpak.net/arcade-covers/3d/NM00007.png");
	// An arcade entry whose id is not NM##### has no cover rather than a made-up URL.
	EXPECT_EQ(CoverUrl(CoverPlatform::Arcade, "SLUS-21376", CoverStyle::Flat2D), "");
}

TEST(CoverUrls, OnlyExactArcadeIdsAreArcade)
{
	EXPECT_TRUE(IsArcadeGameId("NM00007"));
	EXPECT_FALSE(IsArcadeGameId("nm00007"));
	EXPECT_FALSE(IsArcadeGameId("NM0007"));
	EXPECT_FALSE(IsArcadeGameId("NM000071"));
	EXPECT_FALSE(IsArcadeGameId("NM0000A"));
	EXPECT_FALSE(IsArcadeGameId(""));
}

TEST(CoverUrls, SerialsThatCouldEscapeTheUrlOrTheFolderGetNoUrl)
{
	for (const char* bad : {"", "../x", "SLUS 21376", "SL/US-1", "SLUS-1.jpg", "SLUS-21376?x=1", "A234567890123456789012345"})
	{
		EXPECT_FALSE(IsUsableCoverSerial(bad)) << bad;
		EXPECT_EQ(CoverUrl(CoverPlatform::PS2, bad, CoverStyle::Flat2D), "") << bad;
	}
	EXPECT_TRUE(IsUsableCoverSerial("SLUS-21376"));
	EXPECT_TRUE(IsUsableCoverSerial("NM00007"));
}

// ---- Region index --------------------------------------------------------------------------

TEST(CoverRegionIndexTest, TitlesReduceToAComparisonKey)
{
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("Kuon"), "kuon");
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("BioHazard 4 [Asia Version]"), "biohazard 4");
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("Metal Gear Solid 3 - Subsistence [First Print Limited Edition] [Disc 3 of 3]"), "metal gear solid 3 subsistence");
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("Foo (Disc 1) Bar"), "foo bar");
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("Guilty Gear XX #Reload"), "guilty gear xx reload");
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("九怨 -kuon-"), "kuon");
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("[Demo]"), "");
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("九怨"), "");
	// An opener that never closes is just punctuation.
	EXPECT_EQ(CoverRegionIndex::NormalizeTitle("Foo (Bar"), "foo bar");
}

TEST(CoverRegionIndexTest, SwapsToTheFirstSerialOfTheWantedRegionInFileOrder)
{
	const CoverRegionIndex index = *ExcerptIndex();
	EXPECT_EQ(index.SerialCount(), 16u);

	// Kuon: SCAJ-20078, SLES-53411, SLPS-25329, SLUS-21007 in the file. SCAJ is in the Japan/Asia
	// set and comes first.
	EXPECT_EQ(index.CoverSerialFor("SLUS-21007", CoverRegion::JapanAsia), "SCAJ-20078");
	EXPECT_EQ(index.CoverSerialFor("SLUS-21007", CoverRegion::Europe), "SLES-53411");
	EXPECT_EQ(index.CoverSerialFor("SLPS-25329", CoverRegion::USA), "SLUS-21007");
	EXPECT_EQ(index.CoverSerialFor("SLES-53411", CoverRegion::USA), "SLUS-21007");
}

TEST(CoverRegionIndexTest, ReturnsNothingWhenThereIsNothingToSwap)
{
	const CoverRegionIndex index = *ExcerptIndex();

	// Disc region wanted.
	EXPECT_EQ(index.CoverSerialFor("SLUS-21007", CoverRegion::Disc), std::nullopt);
	// Already in the wanted region: no like-for-like swap.
	EXPECT_EQ(index.CoverSerialFor("SLUS-21007", CoverRegion::USA), std::nullopt);
	EXPECT_EQ(index.CoverSerialFor("SLPS-25329", CoverRegion::JapanAsia), std::nullopt);
	// The game has no release in that region (Guilty Gear XX Midnight Carnival has no US serial here).
	EXPECT_EQ(index.CoverSerialFor("SLPS-25261", CoverRegion::USA), std::nullopt);
	// Not in the database, and a PS1 serial.
	EXPECT_EQ(index.CoverSerialFor("SLUS-99999", CoverRegion::Europe), std::nullopt);
	EXPECT_EQ(index.CoverSerialFor("SCUS-94900", CoverRegion::Europe), std::nullopt);
	EXPECT_EQ(index.CoverSerialFor("", CoverRegion::Europe), std::nullopt);
}

TEST(CoverRegionIndexTest, TheEnglishTitleGroupsAJapaneseReleaseWithTheWesternOnes)
{
	const CoverRegionIndex index = *ExcerptIndex();
	// SLPS-20065 is "MADDEN NFL スーパーボウル 2001" with name-en "Madden NFL 2001".
	EXPECT_EQ(index.CoverSerialFor("SLUS-20093", CoverRegion::JapanAsia), "SLPS-20065");
	EXPECT_EQ(index.CoverSerialFor("SLPS-20065", CoverRegion::Europe), "SLES-50021");
}

TEST(CoverRegionIndexTest, SerialLookupIgnoresCase)
{
	const CoverRegionIndex index = *ExcerptIndex();
	EXPECT_EQ(index.CoverSerialFor("slus-21007", CoverRegion::Europe), "SLES-53411");
}

TEST(CoverRegionIndexTest, RegionPrefixSets)
{
	for (const char* serial : {"SLUS-1", "SCUS-1"})
		EXPECT_TRUE(CoverRegionIndex::SerialIsInRegion(serial, CoverRegion::USA)) << serial;
	for (const char* serial : {"SLES-1", "SCES-1", "SLED-1", "SCED-1"})
		EXPECT_TRUE(CoverRegionIndex::SerialIsInRegion(serial, CoverRegion::Europe)) << serial;
	for (const char* serial : {"SLPS-1", "SLPM-1", "SCPS-1", "SLKA-1", "SCKA-1", "SCAJ-1", "SLAJ-1"})
		EXPECT_TRUE(CoverRegionIndex::SerialIsInRegion(serial, CoverRegion::JapanAsia)) << serial;
	EXPECT_FALSE(CoverRegionIndex::SerialIsInRegion("SLUS-1", CoverRegion::Europe));
	EXPECT_FALSE(CoverRegionIndex::SerialIsInRegion("PBPX-1", CoverRegion::USA));
	EXPECT_FALSE(CoverRegionIndex::SerialIsInRegion("SLUS-1", CoverRegion::Disc));
}

TEST(CoverRegionIndexTest, ReadsTheYamlTheWayTheDatabaseWritesIt)
{
	// '#' inside the quotes is part of the title; a '#' after them starts a comment; name-sort
	// is not a name; CRLF line endings; an escaped quote; entries without a title add nothing;
	// keys that are not serials end the previous entry.
	const std::string yaml =
		"# comment\r\n"
		"SLUS-20001:\r\n"
		"  name: \"Guilty Gear XX #Reload\"\r\n"
		"  region: \"NTSC-U\"\r\n"
		"SLPM-60001:\r\n"
		"  name: \"ギルティギア\"\r\n"
		"  name-sort: \"zzz\"\r\n"
		"  name-en: \"Guilty Gear XX #Reload\" # a trailing comment\r\n"
		"SLES-50001:\r\n"
		"  name: \"The \\\"Quoted\\\" One\"\r\n"
		"SLPS-70001:\r\n"
		"  name: The \\\"Quoted\\\" One # unquoted\r\n"
		"SLUS-20002:\r\n"
		"  region: \"NTSC-U\"\r\n"
		"NOTASERIAL:\r\n"
		"  name: \"Orphan\"\r\n"
		"SLUS-20003:\r\n"
		"  name: \"Orphan\"\r\n"
		"SLES-50003:\r\n"
		"  name: \"Orphan\"\r\n";
	const CoverRegionIndex index = CoverRegionIndex::FromGameIndexYaml(yaml);

	EXPECT_EQ(index.CoverSerialFor("SLUS-20001", CoverRegion::JapanAsia), "SLPM-60001");
	EXPECT_EQ(index.CoverSerialFor("SLPM-60001", CoverRegion::USA), "SLUS-20001");
	// An escaped quote reads the same quoted or plain, so these two release titles group.
	EXPECT_EQ(index.CoverSerialFor("SLES-50001", CoverRegion::JapanAsia), "SLPS-70001");
	// SLUS-20002 has no title, so it is in no group; "NOTASERIAL" is not an entry.
	EXPECT_EQ(index.CoverSerialFor("SLUS-20002", CoverRegion::Europe), std::nullopt);
	EXPECT_EQ(index.CoverSerialFor("SLUS-20003", CoverRegion::Europe), "SLES-50003");
	EXPECT_FALSE(index.Empty());
}

TEST(CoverRegionIndexTest, TheShippedGameDatabaseBuildsAFullIndex)
{
	const std::string path = Path::Combine(CATALOG_FIXTURE_DIR, "../../../../../bin/resources/GameIndex.yaml");
	const std::optional<std::string> yaml = FileSystem::ReadFileToString(path.c_str());
	if (!yaml.has_value())
		GTEST_SKIP() << "bin/resources/GameIndex.yaml not found at " << path;

	const auto start = std::chrono::steady_clock::now();
	const CoverRegionIndex index = CoverRegionIndex::FromGameIndexYaml(*yaml);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

	EXPECT_GT(index.SerialCount(), 10000u);
	EXPECT_LT(elapsed.count(), 2000) << "building the index took " << elapsed.count() << " ms";
	EXPECT_EQ(index.CoverSerialFor("SLUS-21007", CoverRegion::Europe), "SLES-53411");
	EXPECT_EQ(index.CoverSerialFor("SLUS-21007", CoverRegion::JapanAsia), "SCAJ-20078");
}

// ---- Preferences and pins ------------------------------------------------------------------

TEST(CoverPreferencesTest, PinsParseTheWayAndroidParsesThem)
{
	const auto pins = CoverPreferences::ParsePins("SLUS-20946=3, sles-51234 =1,bogus,=2,SLPS-1=x,SLPM-2=9,SLPM-3=-4,SLUS-5=");
	ASSERT_EQ(pins.size(), 4u);
	EXPECT_EQ(pins.at("SLUS-20946"), CoverRegion::JapanAsia);
	EXPECT_EQ(pins.at("SLES-51234"), CoverRegion::USA);
	EXPECT_EQ(pins.at("SLPM-2"), CoverRegion::JapanAsia); // 9 clamps to 3
	EXPECT_EQ(pins.at("SLPM-3"), CoverRegion::Disc); // -4 clamps to 0

	EXPECT_TRUE(CoverPreferences::ParsePins("").empty());
}

TEST(CoverPreferencesTest, PinsFormatSortedAndRoundTrip)
{
	CoverPreferences prefs;
	prefs.SetPin("slus-20946", CoverRegion::JapanAsia);
	prefs.SetPin("SLES-51234", CoverRegion::USA);
	EXPECT_EQ(prefs.FormatPins(), "SLES-51234=1,SLUS-20946=3");
	EXPECT_EQ(CoverPreferences::ParsePins(prefs.FormatPins()), prefs.pins);

	prefs.SetPin("SLES-51234", std::nullopt);
	EXPECT_EQ(prefs.FormatPins(), "SLUS-20946=3");
	prefs.SetPin("", CoverRegion::USA); // ignored
	EXPECT_EQ(prefs.pins.size(), 1u);
}

TEST(CoverPreferencesTest, APinBeatsTheLibraryRegionIncludingAPinToTheDisc)
{
	CoverPreferences prefs;
	prefs.region = CoverRegion::USA;
	prefs.SetPin("SLES-53411", CoverRegion::JapanAsia);
	prefs.SetPin("SLPS-25329", CoverRegion::Disc);

	EXPECT_EQ(prefs.EffectiveRegion("SLES-53411"), CoverRegion::JapanAsia);
	EXPECT_EQ(prefs.EffectiveRegion("slps-25329"), CoverRegion::Disc);
	EXPECT_EQ(prefs.EffectiveRegion("SLUS-21007"), CoverRegion::USA);
	EXPECT_EQ(prefs.PinFor("SLUS-21007"), std::nullopt);
}

TEST(CoverPreferencesTest, TheIndexIsOnlyNeededWhenSomethingAsksForAnotherRegion)
{
	CoverPreferences prefs;
	EXPECT_FALSE(prefs.NeedsRegionIndex());
	prefs.SetPin("SLUS-1", CoverRegion::Disc);
	EXPECT_FALSE(prefs.NeedsRegionIndex()); // a pin to the disc asks for nothing
	prefs.SetPin("SLUS-2", CoverRegion::Europe);
	EXPECT_TRUE(prefs.NeedsRegionIndex());
	prefs.SetPin("SLUS-2", std::nullopt);
	prefs.region = CoverRegion::JapanAsia;
	EXPECT_TRUE(prefs.NeedsRegionIndex());
}

TEST(CoverResolveTest, ARegionSwapKeepsTheDiscCoverAsTheFallback)
{
	const auto index = ExcerptIndex();
	CoverPreferences prefs;
	prefs.region = CoverRegion::JapanAsia;

	const CoverUrls urls = ResolveCoverUrls(CoverPlatform::PS2, "SLUS-21007", prefs, index.get());
	EXPECT_EQ(urls.primary, Ps2Jpg("SCAJ-20078"));
	EXPECT_EQ(urls.fallback, Ps2Jpg("SLUS-21007"));
	EXPECT_EQ(urls.cover_serial, "SCAJ-20078");

	prefs.style = CoverStyle::Case3D;
	EXPECT_EQ(ResolveCoverUrls(CoverPlatform::PS2, "SLUS-21007", prefs, index.get()).primary, Ps2Png("SCAJ-20078"));
}

TEST(CoverResolveTest, NoSwapMeansNoFallback)
{
	const auto index = ExcerptIndex();
	CoverPreferences prefs; // disc region
	const CoverUrls disc = ResolveCoverUrls(CoverPlatform::PS2, "SLUS-21007", prefs, index.get());
	EXPECT_EQ(disc.primary, Ps2Jpg("SLUS-21007"));
	EXPECT_EQ(disc.fallback, "");

	prefs.region = CoverRegion::USA; // already USA
	EXPECT_EQ(ResolveCoverUrls(CoverPlatform::PS2, "SLUS-21007", prefs, index.get()).fallback, "");

	// No index built: the disc's cover, silently.
	prefs.region = CoverRegion::JapanAsia;
	const CoverUrls no_index = ResolveCoverUrls(CoverPlatform::PS2, "SLUS-21007", prefs, nullptr);
	EXPECT_EQ(no_index.primary, Ps2Jpg("SLUS-21007"));
	EXPECT_EQ(no_index.fallback, "");

	EXPECT_FALSE(ResolveCoverUrls(CoverPlatform::PS2, "../x", prefs, index.get()).Valid());
}

TEST(CoverResolveTest, ArcadeAndPs1NeverSwap)
{
	const auto index = ExcerptIndex();
	CoverPreferences prefs;
	prefs.region = CoverRegion::Europe;
	EXPECT_EQ(ResolveCoverUrls(CoverPlatform::Arcade, "NM00007", prefs, index.get()).primary, std::string(ARCADE_ROOT) + "/NM00007.jpg");
	EXPECT_EQ(ResolveCoverUrls(CoverPlatform::PS1, "SCUS-94900", prefs, index.get()).primary, std::string(PSX_REPO) + "/default/SCUS-94900.jpg");
}
