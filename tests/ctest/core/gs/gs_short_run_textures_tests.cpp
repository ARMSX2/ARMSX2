// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Which texture bindings each group of a short run pushes when it is recorded with the ordinary calls
// (GSShortRunTextures.h).

#include <gtest/gtest.h>

#include "GS/Renderers/Common/GSShortRunTextures.h"

namespace
{
	// Binding numbers as in GSDeviceVK: texture, palette, render target.
	constexpr u32 TEX = 1u << 0;
	constexpr u32 PAL = 1u << 1;
	constexpr u32 RT = 1u << 2;
	constexpr u32 ALL = (1u << 7) - 1;
} // namespace

TEST(GSShortRunTextures, AGroupPushesOnlyWhatItReads)
{
	GSShortRunTextures t;
	EXPECT_EQ(t.Push(ALL, TEX | PAL), TEX | PAL);
	EXPECT_EQ(t.owed, ALL & ~(TEX | PAL));
}

// After a run through the entry every binding is owed. An untextured group cannot push the texture and palette it
// leaves set; the textured group after it, which was taken with nothing owed because the texture did not change,
// pushes them.
TEST(GSShortRunTextures, AnUntexturedGroupLeavesTheTextureToTheNextGroupThatReadsIt)
{
	GSShortRunTextures t;
	EXPECT_EQ(t.Push(ALL, 0), 0u);
	EXPECT_EQ(t.Push(0, TEX | PAL), TEX | PAL);
	EXPECT_EQ(t.owed, ALL & ~(TEX | PAL));
}

TEST(GSShortRunTextures, WhatNoGroupReadsIsStillOwedAfterTheRun)
{
	GSShortRunTextures t;
	EXPECT_EQ(t.Push(TEX | RT, 0), 0u);
	EXPECT_EQ(t.Push(PAL, PAL), PAL);
	EXPECT_EQ(t.owed, TEX | RT);
}

TEST(GSShortRunTextures, NothingOwedPushesNothing)
{
	GSShortRunTextures t;
	EXPECT_EQ(t.Push(0, TEX | PAL), 0u);
	EXPECT_EQ(t.Push(0, RT), 0u);
	EXPECT_EQ(t.owed, 0u);
}
