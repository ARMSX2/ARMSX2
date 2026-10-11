// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// ParseJson reads text that comes from a remote host (catalogs, GitHub's API), so what it is given can be
// anything: a parse that recursed once per nesting level would overflow the stack on a few megabytes of '['.

#include "Catalog/Json.h"

#include <gtest/gtest.h>

#include <string>

using namespace Catalog;

namespace
{
	// Far more levels than the 8 MiB main-thread stack of a recursive parser survives (rapidjson's recursive
	// parser uses on the order of a hundred bytes of stack per level).
	constexpr size_t DEPTH = 1000000;
} // namespace

TEST(Json, ParsesAnOrdinaryDocument)
{
	rapidjson::Document document;
	std::string error;
	ASSERT_TRUE(ParseJson(R"({"a":[1,2,{"b":"c"}],"d":null})", &document, &error)) << error;
	ASSERT_TRUE(document.IsObject());
	EXPECT_EQ(document["a"].Size(), 3u);
	EXPECT_STREQ(document["a"][2]["b"].GetString(), "c");
}

TEST(Json, AnErrorSaysWhereItIs)
{
	rapidjson::Document document;
	std::string error;
	EXPECT_FALSE(ParseJson(R"({"a":[1,2,})", &document, &error));
	EXPECT_NE(error.find("offset"), std::string::npos) << error;
}

TEST(Json, DeeplyNestedInputDoesNotOverflowTheStack)
{
	// Valid, and a million levels deep: it parses (on the heap) rather than crashing.
	std::string nested(DEPTH, '[');
	nested.append(DEPTH, ']');
	rapidjson::Document document;
	std::string error;
	ASSERT_TRUE(ParseJson(nested, &document, &error)) << error;
	EXPECT_TRUE(document.IsArray());
	EXPECT_EQ(document.Size(), 1u);
}

TEST(Json, DeeplyNestedInputThatNeverEndsIsAnError)
{
	// What a hostile host sends: nothing but openings.
	for (const std::string& opening : {std::string("["), std::string(R"({"a":)")})
	{
		std::string text;
		text.reserve(DEPTH * opening.size());
		for (size_t i = 0; i < DEPTH; i++)
			text += opening;

		rapidjson::Document document;
		std::string error;
		EXPECT_FALSE(ParseJson(text, &document, &error)) << opening;
		EXPECT_FALSE(error.empty()) << opening;
	}
}
