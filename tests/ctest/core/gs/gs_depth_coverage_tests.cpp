// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the constant-depth-buffer pass rule (GS/Renderers/HW/GSDepthCoverage.h).
//
// This rule widens what the renderer will credit as covering its target, and everything downstream
// of that -- the target's tracked alpha, DATE, alpha scaling -- assumes the claim is true. So the
// cases that matter are the refusals: a Z that straddles the cleared value, a Z the vertex trace
// cannot vouch for, and a draw that writes depth into its own test.
//
// Rides gs_vertex_tests -- the rule is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/HW/GSDepthCoverage.h"

#include <gtest/gtest.h>

namespace
{
	constexpr u32 kMaxZ24 = 0x00FFFFFFu;

	bool Pass(u32 ztst, u32 draw_z, u32 buffer_z, bool flat = true, bool writes_depth = false,
		bool may_overlap = false)
	{
		return GSDepthCoverage::AllPixelsPassConstantDepth(ztst, flat, draw_z, buffer_z, writes_depth,
			may_overlap);
	}
} // namespace

TEST(GSDepthCoverage, GreaterPassesWhenTheDrawIsBeyondTheClearedValue)
{
	// Xenosaga's opening sprite: maximum Z for the format, against a buffer cleared to zero.
	EXPECT_TRUE(Pass(ZTST_GREATER, kMaxZ24, 0));
	EXPECT_TRUE(Pass(ZTST_GREATER, 1, 0));
}

TEST(GSDepthCoverage, GreaterFailsAtTheClearedValueItself)
{
	// Equal is not greater, so the pixels do not all pass.
	EXPECT_FALSE(Pass(ZTST_GREATER, 0, 0));
	EXPECT_FALSE(Pass(ZTST_GREATER, 0x8000, 0x8000));
	// And a draw behind the buffer passes nothing at all.
	EXPECT_FALSE(Pass(ZTST_GREATER, 0x7FFF, 0x8000));
}

TEST(GSDepthCoverage, GEqualPassesAtTheClearedValue)
{
	EXPECT_TRUE(Pass(ZTST_GEQUAL, 0, 0));
	EXPECT_TRUE(Pass(ZTST_GEQUAL, kMaxZ24, 0));
	EXPECT_FALSE(Pass(ZTST_GEQUAL, 0x7FFF, 0x8000));
}

TEST(GSDepthCoverage, AZThatStraddlesTheClearedValueIsRefused)
{
	// Not flat means the draw carries more than one Z, so some vertices can be on the failing
	// side. The vertex trace's Z bounds are not accurate to the bit, so a range is no answer.
	EXPECT_FALSE(Pass(ZTST_GREATER, kMaxZ24, 0, /*flat=*/false));
	EXPECT_FALSE(Pass(ZTST_GEQUAL, kMaxZ24, 0, /*flat=*/false));
}

TEST(GSDepthCoverage, ADrawThatWritesDepthIntoItsOwnTestIsRefused)
{
	// One primitive's depth write is the next primitive's buffer value, so the buffer is no
	// longer constant partway through the draw.
	EXPECT_FALSE(Pass(ZTST_GREATER, kMaxZ24, 0, true, /*writes_depth=*/true, /*may_overlap=*/true));
	// Non-overlapping primitives cannot see each other's writes.
	EXPECT_TRUE(Pass(ZTST_GREATER, kMaxZ24, 0, true, /*writes_depth=*/true, /*may_overlap=*/false));
	// Nor can a draw that leaves depth alone.
	EXPECT_TRUE(Pass(ZTST_GREATER, kMaxZ24, 0, true, /*writes_depth=*/false, /*may_overlap=*/true));
}

TEST(GSDepthCoverage, NeverAndAlwaysAreLeftToTheCaller)
{
	// The renderer answers both without needing to know anything about the buffer, and this rule
	// must not quietly claim them.
	EXPECT_FALSE(Pass(ZTST_NEVER, kMaxZ24, 0));
	EXPECT_FALSE(Pass(ZTST_ALWAYS, kMaxZ24, 0));
}

TEST(GSDepthCoverage, AClearedButNonZeroBufferStillComparesNormally)
{
	// The renderer only ever hands this rule a zero clear today, but the arithmetic does not
	// depend on that and should not start to.
	EXPECT_TRUE(Pass(ZTST_GREATER, 0x00FFFFFF, 0x00FFFFFE));
	EXPECT_FALSE(Pass(ZTST_GREATER, 0x00FFFFFE, 0x00FFFFFF));
}
