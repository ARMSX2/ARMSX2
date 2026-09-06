// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the sprite far-edge adjustments and the order they run in
// (GS/Renderers/HW/GSSpriteEdgeSnap.h).
//
// Two of them exist. The pixel-grid snap moves a sprite's far edge to the next whole pixel when
// the UV slide that implies is exact, per sprite. The AlignSpriteX game fix (ace combat, tekken)
// takes one decision off the FIRST sprite of a batch -- "is its far X half a pixel short" -- and
// then adds half a pixel to every sprite in the batch.
//
// Running the snap first breaks the fix, and the two-sprite batch below is the case that shows it:
// the first sprite's texel ratio is inexact so the snap leaves it alone, the second's is exact so
// the snap moves it, and the fix then adds half a pixel to a far edge that is already whole. The
// mirror case, where the snap moves the first sprite, silently answers the fix's question no and
// turns it off for the whole batch. Both are pinned here.
//
// Rides gs_vertex_tests -- the rules are header-only constexpr, so they need no extra linkage.

#include "GS/Renderers/HW/GSSpriteEdgeSnap.h"

#include <gtest/gtest.h>

using namespace GSSpriteEdgeSnap;

namespace
{
	// One sprite as the vertex buffer holds it: 1/16 pixel positions, 1/16 texel UVs.
	struct Sprite
	{
		int x0, y0, x1, y1;
		int u0, v0, u1, v1;
	};

	Delta Snap(const Sprite& s) { return FarEdge(s.x0, s.y0, s.x1, s.y1, s.u0, s.v0, s.u1, s.v1, true); }

	// The batch the fix looks at: its answer comes off the first sprite only.
	bool FixApplies(const Sprite* v, u32 sprites)
	{
		return AlignSpriteXApplies(v[0].x1, v[0].u1, true, sprites * 2, v[0].x1, (sprites >= 2) ? v[1].x0 : 0);
	}
} // namespace

TEST(GSSpriteEdgeSnap, AWholeEdgeIsLeftAlone)
{
	// 16 units to the pixel. A far edge already on the grid has nothing to snap to.
	const Sprite s{0, 0, 16 * 32, 16 * 32, 0, 0, 16 * 32, 16 * 32};
	EXPECT_TRUE(Snap(s).IsZero());
}

TEST(GSSpriteEdgeSnap, AHalfPixelEdgeWithAnExactTexelRatioMoves)
{
	// 32.5 pixels wide over 65 texels: one texel per half pixel, so the half-pixel slide is a
	// whole 8/16 of a texel and the sprite can be written back without resampling.
	const Sprite s{0, 0, 16 * 32 + 8, 16 * 32, 0, 0, 16 * 65, 16 * 32};
	const Delta d = Snap(s);
	EXPECT_EQ(d.dx, 8);
	EXPECT_EQ(d.dy, 0);
	EXPECT_EQ(d.du, 16);
	EXPECT_EQ(d.dv, 0);
}

TEST(GSSpriteEdgeSnap, AHalfPixelEdgeWithAnInexactTexelRatioIsRefused)
{
	// 32.5 pixels wide over 3 texels. The half-pixel slide is 3/65 of a texel, which is not a
	// whole 1/16 step, so writing it down would resample the sprite for one edge pixel.
	const Sprite s{0, 0, 16 * 32 + 8, 16 * 32, 0, 0, 16 * 3, 16 * 32};
	EXPECT_TRUE(Snap(s).IsZero());
}

TEST(GSSpriteEdgeSnap, TheFixDecidesOnUnsnappedCoordinates)
{
	// The batch the reordering is about. Sprite 0 ends at 32.5 pixels with an inexact texel ratio
	// (the snap refuses it), sprite 1 ends at 64.5 with an exact one (the snap takes it), and the
	// two do not meet, so the fix sees its hole.
	Sprite v[2] = {
		{0, 0, 16 * 32 + 8, 16 * 32, 0, 0, 16 * 3, 16 * 32},
		{16 * 40, 0, 16 * 64 + 8, 16 * 32, 0, 0, 16 * 49, 16 * 32},
	};

	// On the original coordinates the fix fires: sprite 0's far X is half a pixel short and its
	// far U is on a whole texel.
	ASSERT_TRUE(FixApplies(v, 2));

	// Had the snap run first it would have moved sprite 1 only, leaving the fix's answer intact
	// but its far edge already whole -- and the fix then pushes it half a pixel past the edge.
	const Delta pre = Snap(v[1]);
	ASSERT_FALSE(pre.IsZero());

	// The order the renderer uses: the fix first, on every sprite in the batch.
	for (Sprite& s : v)
		s.x1 += 8;

	// And now every far edge in the batch is whole, so the snap has nothing left to do on any of
	// them -- which is why the renderer can skip it outright when the fix fired.
	for (const Sprite& s : v)
		EXPECT_TRUE(Snap(s).IsZero());
}

TEST(GSSpriteEdgeSnap, ASnappedFirstSpriteWouldTurnTheFixOff)
{
	// The mirror case, and the quieter one: the snap moves sprite 0, whose far X is then whole,
	// and the fix's "half a pixel short" question answers no for the whole batch. The black line
	// the fix removes comes back on every sprite, including the ones the snap refused.
	Sprite v[2] = {
		{0, 0, 16 * 32 + 8, 16 * 32, 0, 0, 16 * 65, 16 * 32},
		{16 * 40, 0, 16 * 64 + 8, 16 * 32, 0, 0, 16 * 3, 16 * 32},
	};

	ASSERT_TRUE(FixApplies(v, 2));

	const Delta d = Snap(v[0]);
	ASSERT_FALSE(d.IsZero());
	v[0].x1 += d.dx;
	v[0].u1 += d.du;

	EXPECT_FALSE(FixApplies(v, 2));
}

TEST(GSSpriteEdgeSnap, ASingleSpriteBatchCountsAsAHole)
{
	// count < 4 short-circuits the second-sprite comparison, so a lone sprite is always a hole.
	EXPECT_TRUE(AlignSpriteXApplies(16 * 32 + 8, 16 * 65, true, 2, 16 * 32 + 8, 0));
}
