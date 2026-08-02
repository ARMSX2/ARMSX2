// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SaveStateLegacy.h"

#include <gtest/gtest.h>

// The legacy save state formats count cycles in 32 bits, which wrap roughly
// every 14.6 seconds of emulated time; today's counters are 64-bit. Widening
// has to preserve the signed distance to the domain's own cycle counter, since
// that distance — not the absolute value — is what schedules events.

TEST(SaveStateLegacyWidenCycle, ValueAtTheBaseLandsOnTheNewBase)
{
	EXPECT_EQ(SaveStateLegacy::WidenCycle(0x1000, 0x1000, 0x1000), 0x1000u);
	EXPECT_EQ(SaveStateLegacy::WidenCycle(0x1000, 0x1000, 0x1'0000'0000ull), 0x1'0000'0000ull);
}

TEST(SaveStateLegacyWidenCycle, PreservesDistanceToTheBase)
{
	// Ahead of the base (an event scheduled in the future).
	EXPECT_EQ(SaveStateLegacy::WidenCycle(0x1064, 0x1000, 0x5000), 0x5064u);
	// Behind the base (an event whose deadline has passed).
	EXPECT_EQ(SaveStateLegacy::WidenCycle(0x0F9C, 0x1000, 0x5000), 0x4F9Cu);
}

TEST(SaveStateLegacyWidenCycle, SurvivesAWrapAheadOfTheBase)
{
	// The base is just short of the 32-bit wrap and the event is just past it,
	// so the raw values are 4 billion apart while the real distance is 200.
	// Zero-extension would schedule the event an eternity away.
	constexpr u32 base = 0xFFFF'FF9Cu;
	constexpr u32 value = 0x0000'0064u; // base + 200, wrapped
	EXPECT_EQ(SaveStateLegacy::WidenCycle(value, base, 0x1'0000'0000ull), 0x1'0000'00C8ull);
}

TEST(SaveStateLegacyWidenCycle, SurvivesAWrapBehindTheBase)
{
	// Mirror image: the base has wrapped past zero and the event has not, so
	// the event is 200 cycles in the past.
	constexpr u32 base = 0x0000'0064u;
	constexpr u32 value = 0xFFFF'FF9Cu; // base - 200, wrapped
	EXPECT_EQ(SaveStateLegacy::WidenCycle(value, base, 0x1'0000'0064ull), 0xFFFF'FF9Cull);
}

TEST(SaveStateLegacyWidenCycle, IsUsableAtCompileTime)
{
	// The mappers widen in bulk, so this has to fold rather than call.
	static_assert(SaveStateLegacy::WidenCycle(0x1064, 0x1000, 0x5000) == 0x5064u);
	static_assert(SaveStateLegacy::WidenCycle(0x64, 0xFFFF'FF9Cu, 0x1'0000'0000ull) == 0x1'0000'00C8ull);
	SUCCEED();
}

TEST(SaveStateLegacyVersion, AcceptsOnlyTheTwoFormatsSeenInTheWild)
{
	// 0x9A2C is the AetherSX2 v1.5-era format, 0x9A34 NetherSX2 v2.1's.
	EXPECT_TRUE(SaveStateLegacy::IsSupportedVersion(0x9A2C0000));
	EXPECT_TRUE(SaveStateLegacy::IsSupportedVersion(0x9A340000));

	// Minor revisions within an accepted major share its blob layout.
	EXPECT_TRUE(SaveStateLegacy::IsSupportedVersion(0x9A2C0001));

	// Neighbouring majors are different layouts we have never seen a file of,
	// and the current major goes through the normal reader.
	EXPECT_FALSE(SaveStateLegacy::IsSupportedVersion(0x9A2B0000));
	EXPECT_FALSE(SaveStateLegacy::IsSupportedVersion(0x9A350000));
	EXPECT_FALSE(SaveStateLegacy::IsSupportedVersion(0x9A590000));
	EXPECT_FALSE(SaveStateLegacy::IsSupportedVersion(0));
}
