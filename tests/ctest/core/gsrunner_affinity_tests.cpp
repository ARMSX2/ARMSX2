// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins pcsx2-gsrunner's -affinity argument parsing (pcsx2-gsrunner/GSRunnerAffinity.h).
//
// The flag names which cores the whole VM runs on, so the property that matters is that
// nothing malformed or out of range is quietly turned into a mode that runs. A round that
// asked for a placement it did not get, and said nothing, is the failure this flag was
// added to stop; a clamp here would reintroduce it one layer down.

#include "GSRunnerAffinity.h"

#include "gtest/gtest.h"

TEST(GSRunnerAffinity, AcceptsEveryDefinedMode)
{
	for (int mode = GSRunnerAffinity::MODE_MIN; mode <= GSRunnerAffinity::MODE_MAX; mode++)
	{
		const std::string arg = std::to_string(mode);
		const std::optional<int> parsed = GSRunnerAffinity::ParseMode(arg);
		ASSERT_TRUE(parsed.has_value()) << "mode " << mode << " was rejected";
		EXPECT_EQ(parsed.value(), mode);
	}
}

TEST(GSRunnerAffinity, RejectsOutOfRange)
{
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("8").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("-1").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("70").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("2147483648").has_value());
}

TEST(GSRunnerAffinity, RejectsMalformed)
{
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("perf").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("0x7").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("+7").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("7.0").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("7,0").has_value());
}

TEST(GSRunnerAffinity, RejectsSurroundingWhitespace)
{
	// Two spellings of one round would otherwise reach two artifacts that no longer
	// compare equal on the string.
	EXPECT_FALSE(GSRunnerAffinity::ParseMode(" 7").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("7 ").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("\t0").has_value());
}

TEST(GSRunnerAffinity, DoesNotStopAtTheFirstDigitItLikes)
{
	// std::from_chars stops at the first character it cannot use and reports success, so
	// "7abc" parses as 7 unless the remainder is checked. That is the shape that would let
	// a typo run under Performance Cores while the log says the operator asked for it.
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("7abc").has_value());
	EXPECT_FALSE(GSRunnerAffinity::ParseMode("0-3").has_value());
}
