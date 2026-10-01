// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins how far a full stream ring grows instead of waiting (GS/Renderers/Common/GSStreamRingGrowth.h).
//
// Rides gs_vertex_tests -- the policy is header-only.

#include "GS/Renderers/Common/GSStreamRingGrowth.h"

#include <gtest/gtest.h>

namespace
{
	constexpr u32 KiB = 1024;
	constexpr u32 MiB = 1024 * 1024;
} // namespace

TEST(GSStreamRingGrowth, DoublesWhenItWouldWait)
{
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(16 * MiB, 64 * MiB, 4 * KiB), 32 * MiB);
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(32 * MiB, 64 * MiB, 4 * KiB), 64 * MiB);
}

TEST(GSStreamRingGrowth, StopsAtTheCap)
{
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(64 * MiB, 64 * MiB, 4 * KiB), 0u);
	// A cap that is not a power-of-two multiple of the start is still reached exactly.
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(16 * MiB, 24 * MiB, 4 * KiB), 24 * MiB);
}

TEST(GSStreamRingGrowth, NeverGrowsARingWithoutHeadroom)
{
	// max_size == size is how every ring that must not grow is created.
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(8 * MiB, 8 * MiB, 4 * KiB), 0u);
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(0, 8 * MiB, 4 * KiB), 0u);
}

TEST(GSStreamRingGrowth, GrowsFarEnoughForTheRequest)
{
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(16 * MiB, 64 * MiB, 40 * MiB), 64 * MiB);
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(64 * KiB, 64 * MiB, 3 * MiB), 4 * MiB);
	// Not even the cap holds it: wait, and let the caller's overflow handling speak.
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(16 * MiB, 64 * MiB, 80 * MiB), 0u);
}

TEST(GSStreamRingGrowth, NoOverflowNearTheTopOfTheRange)
{
	// Doubling happens in 64 bits, so a ring near 4 GiB still clamps to its cap.
	EXPECT_EQ(GSStreamRingGrowth::SizeInsteadOfWait(0x80000000u, 0xFFFFFFFFu, 4 * KiB), 0xFFFFFFFFu);
}
