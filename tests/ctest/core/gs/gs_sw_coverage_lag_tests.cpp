// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the rule that keeps the software texture cache mapping every texel the
// scanline can ask for.
//
// The scanline samples one 16.16 unit BELOW the exact plane on each axis whose
// walk goes forward (GSDrawScanline's tclag, measured on gs-shade), so wherever
// the exact coordinate lands on a texel boundary the sample lands one texel
// lower. The cache was being handed the exact range, so such a draw read a texel
// the cache had never mapped -- unmapped buffer, which is zeros.
//
// gs-persp4 (SCPH-30001, 2026-09-07) is the capture that surfaced it, and the
// numbers are worth keeping next to the rule: 115,536 of its 216,908 readings
// came back as zeros, and its dq, q-only and c-ladder sections read nothing at
// all. With the expansion, none of the 216,908 is a zero.
//
// This is not probe-only. Three conditions do it: a non-sprite primitive, a
// coordinate that walks, and an exact range whose minimum sits on a block
// boundary.

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/SW/GSCoordinateLag.h"
#include "GS/GSState.h"

#include <gtest/gtest.h>

namespace
{
GSVector4i Rect(int x, int y, int z, int w) { return GSVector4i(x, y, z, w); }
} // namespace

// The gs-persp4 shape, in one cell. Its constant-quotient bands need exactly one
// texel, at (4, 16). The cache aligns that outward to the 8x8 block at (0, 16),
// so row 15 -- where the lagged sample actually lands -- was outside everything
// mapped. The rect the cache is given must reach it.
TEST(SwCoverageLagTest, TheRangeReachesTheTexelTheLagSamples)
{
	const GSVector4i exact = Rect(4, 16, 5, 17);
	const GSVector4i mapped = GSCoverageWithCoordinateLag(exact, GS_TRIANGLE_CLASS);

	EXPECT_EQ(mapped.x, 3);
	EXPECT_EQ(mapped.y, 15);

	// The maximum does not move: the lag only ever samples lower.
	EXPECT_EQ(mapped.z, 5);
	EXPECT_EQ(mapped.w, 17);
}

// Sprites take no lag, so they take no expansion -- gs-interp reads a sprite's
// coordinate exact on 3,072 of 3,072, negative gradients included.
TEST(SwCoverageLagTest, ASpriteIsNotExpanded)
{
	const GSVector4i exact = Rect(4, 16, 5, 17);
	const GSVector4i mapped = GSCoverageWithCoordinateLag(exact, GS_SPRITE_CLASS);

	EXPECT_TRUE(mapped.eq(exact));
}

// A range already at the texture's origin cannot go below it.
TEST(SwCoverageLagTest, TheOriginIsNotCrossed)
{
	const GSVector4i mapped = GSCoverageWithCoordinateLag(Rect(0, 0, 8, 8), GS_TRIANGLE_CLASS);

	EXPECT_EQ(mapped.x, 0);
	EXPECT_EQ(mapped.y, 0);
	EXPECT_EQ(mapped.z, 8);
	EXPECT_EQ(mapped.w, 8);
}

// One axis at the origin and one not: each is decided on its own.
TEST(SwCoverageLagTest, EachAxisIsClampedSeparately)
{
	const GSVector4i mapped = GSCoverageWithCoordinateLag(Rect(0, 16, 4, 24), GS_TRIANGLE_CLASS);

	EXPECT_EQ(mapped.x, 0);
	EXPECT_EQ(mapped.y, 15);
}

// A range that already spans whole blocks still grows, because the cache aligns
// outward afterwards and a range starting exactly on a block boundary is the
// case that fails.
TEST(SwCoverageLagTest, ABlockAlignedRangeStillGrows)
{
	const GSVector4i mapped = GSCoverageWithCoordinateLag(Rect(8, 8, 16, 16), GS_TRIANGLE_CLASS);

	EXPECT_EQ(mapped.x, 7);
	EXPECT_EQ(mapped.y, 7);
}
