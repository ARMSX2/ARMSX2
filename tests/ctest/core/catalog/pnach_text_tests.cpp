// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the .pnach text handling the online patch browser is built on (PnachText.h). The three
// real files in fixtures/patches/ were downloaded on 2026-10-08; the widescreen one is trimmed to
// the first lines of each section.

#include "catalog_test_support.h"

#include "Catalog/PnachText.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace Catalog;
using CatalogTest::ReadFixture;

namespace
{
	std::vector<std::string> SectionNames(const std::string& pnach_text)
	{
		std::vector<std::string> names;
		for (const PnachSection& section : ParsePnach(pnach_text).sections)
			names.push_back(section.name);
		return names;
	}
} // namespace

TEST(PnachText, ParsesTheRealWidescreenFile)
{
	const PnachFile file = ParsePnach(ReadFixture("patches/PAPX-90203_55CE5111.pnach"));
	EXPECT_EQ(file.game_title, "Gran Turismo 2000 [PAPX-90203]");
	ASSERT_EQ(file.sections.size(), 7u);
	EXPECT_EQ(file.sections[0].name, "Widescreen 16:9");
	EXPECT_EQ(file.sections[1].name, "No-Interlacing");
	EXPECT_EQ(file.sections[6].name, "No time limit");

	// The database writes description=, not comment=.
	EXPECT_EQ(file.sections[0].description, "16:9 widescreen patch");

	// The block is verbatim from its header on, and ends at its last non-blank line.
	EXPECT_EQ(file.sections[0].body.rfind("[Widescreen 16:9]\ngsaspectratio=16:9\ndescription=16:9 widescreen patch\nauthor=Silent\n", 0), 0u);
	EXPECT_NE(file.sections[0].body.find("// Change Vert- to Hor+\n"), std::string::npos);
	EXPECT_NE(file.sections[0].body.back(), '\n');
	EXPECT_EQ(file.sections[0].body.find("[No-Interlacing]"), std::string::npos);
}

TEST(PnachText, ParsesTheRealCheatFile)
{
	const PnachFile file = ParsePnach(ReadFixture("patches/shadowninja826_02E1970F.pnach"));
	EXPECT_EQ(file.game_title, "Sega Ages 2500 Series Vol.26 - Dynamite Deka [UNKNOWN] [02E1970F]");
	ASSERT_EQ(file.sections.size(), 1u);
	EXPECT_EQ(file.sections[0].name, "Infinite Credits");
	EXPECT_EQ(file.sections[0].description, "Infinite Credits");
	EXPECT_EQ(file.sections[0].body, "[Infinite Credits]\nauthor=xs1l3n7x(pnach)/UPC(codes)\ndescription=Infinite Credits\n// Infinite Credits\npatch=1,EE,01FA9BF0,extended,00000009");

	// Both repositories carry the same file.
	EXPECT_EQ(ReadFixture("patches/shadowninja826_02E1970F.pnach"), ReadFixture("patches/xs1l3n7x_02E1970F.pnach"));
}

TEST(PnachText, FirstCommentWinsOverDescription)
{
	const PnachFile file = ParsePnach(
		"[A]\ndescription=from description\nCOMMENT = from comment\ncomment=second comment\npatch=0\n"
		"[B]\ndescription=only description\n"
		"[C]\ncomment=first\ndescription=second\n"
		"[D]\n");
	ASSERT_EQ(file.sections.size(), 4u);
	EXPECT_EQ(file.sections[0].description, "from comment");
	EXPECT_EQ(file.sections[1].description, "only description");
	EXPECT_EQ(file.sections[2].description, "first");
	EXPECT_EQ(file.sections[3].description, "");
}

TEST(PnachText, OnlyExactKeysCountAsDescriptions)
{
	const PnachFile file = ParsePnach("[A]\ncomments=no\n//comment=no\npatch=comment=no\ncomment=\ndescription=yes\n");
	ASSERT_EQ(file.sections.size(), 1u);
	EXPECT_EQ(file.sections[0].description, "yes");
}

TEST(PnachText, DropsLinesBeforeTheFirstSectionAndHeaderlessFiles)
{
	const PnachFile with_leading = ParsePnach("patch=0,EE,00100000,word,00000001\n\n[A]\npatch=1\n");
	ASSERT_EQ(with_leading.sections.size(), 1u);
	EXPECT_EQ(with_leading.sections[0].body, "[A]\npatch=1");

	const PnachFile headerless = ParsePnach("gametitle=Old Style\npatch=0,EE,00100000,word,00000001\n");
	EXPECT_EQ(headerless.game_title, "Old Style");
	EXPECT_TRUE(headerless.sections.empty());

	EXPECT_TRUE(ParsePnach("").sections.empty());
}

TEST(PnachText, HandlesLineEndingsBomAndIndentedHeaders)
{
	const std::string text = "\xEF\xBB\xBFgametitle=T\r\n\r\n[A]\r\ncomment=c\r\npatch=1\r\n[B]\rpatch=2\r  [ Spaced Name ]  \npatch=3";
	const PnachFile file = ParsePnach(text);
	EXPECT_EQ(file.game_title, "T");
	ASSERT_EQ(file.sections.size(), 3u);
	EXPECT_EQ(file.sections[0].body, "[A]\ncomment=c\npatch=1");
	EXPECT_EQ(file.sections[1].body, "[B]\npatch=2");
	EXPECT_EQ(file.sections[2].name, "Spaced Name");
}

TEST(PnachText, SkipsSectionsWithNoNameAndKeepsDuplicates)
{
	const PnachFile file = ParsePnach("[]\npatch=1\n[ ]\npatch=2\n[Same]\npatch=3\n[Same]\npatch=4\n");
	EXPECT_EQ(SectionNames("[]\npatch=1\n[ ]\npatch=2\n[Same]\npatch=3\n[Same]\npatch=4\n"), (std::vector<std::string>{"Same", "Same"}));
	EXPECT_EQ(file.sections[0].body, "[Same]\npatch=3");
	EXPECT_EQ(file.sections[1].body, "[Same]\npatch=4");
}

TEST(PnachText, ComposesANewFile)
{
	EXPECT_EQ(ComposePnach("", "God of War", {"[A]\npatch=1\n\n", "[B]\npatch=2"}), "gametitle=God of War\n\n[A]\npatch=1\n\n[B]\npatch=2\n\n");
	EXPECT_EQ(ComposePnach("", "", {"[A]\npatch=1"}), "[A]\npatch=1\n\n");
	EXPECT_EQ(ComposePnach("", "Line one\nline two\r", {"[A]"}), "gametitle=Line one line two\n\n[A]\n\n");
}

TEST(PnachText, AppendingKeepsTheFileAndSeparatesWithOneBlankLine)
{
	const std::vector<std::string> bodies = {"[New]\npatch=9"};
	EXPECT_EQ(ComposePnach("[Old]\npatch=1", "T", bodies), "[Old]\npatch=1\n\n[New]\npatch=9\n\n");
	EXPECT_EQ(ComposePnach("[Old]\npatch=1\n", "T", bodies), "[Old]\npatch=1\n\n[New]\npatch=9\n\n");
	EXPECT_EQ(ComposePnach("[Old]\npatch=1\n\n", "T", bodies), "[Old]\npatch=1\n\n[New]\npatch=9\n\n");
	EXPECT_EQ(ComposePnach("[Old]\r\npatch=1\r\n\r\n", "T", bodies), "[Old]\r\npatch=1\r\n\r\n[New]\npatch=9\n\n");
	EXPECT_EQ(ComposePnach("gametitle=Kept\n// hand written\n[Old]\n", "Ignored", bodies), "gametitle=Kept\n// hand written\n[Old]\n\n[New]\npatch=9\n\n");
}

TEST(PnachText, CheatNameKeyIgnoresCaseAndSpacing)
{
	EXPECT_EQ(CheatNameKey("  Infinite \t HEALTH  "), "infinite health");
	EXPECT_EQ(CheatNameKey("Infinite Health"), CheatNameKey("infinite  health"));
	EXPECT_NE(CheatNameKey("Infinite Health"), CheatNameKey("Infinite Healths"));
	EXPECT_EQ(CheatNameKey(""), "");
}
