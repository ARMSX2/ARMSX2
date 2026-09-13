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
// Neither may move a far edge that the next sprite in the batch starts on, because both would then
// be drawing the neighbour's first device column twice. The fix answers that with its
// `hole_in_vertex` question; the snap answers it per sprite with DropAbuttingAxes, pinned at the
// bottom of this file on the batch shape that found it -- Need for Speed Underground's bloom
// downsample, sixteen abutting strips with a bright line at all fifteen seams from 1.76x up.
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

#include <algorithm>

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

	// The snap as the renderer's batch walk applies it: each sprite's own arithmetic, then the
	// near corners of the sprites either side of it (GSRendererHW::SnapSpriteEdgesToPixelGrid).
	// Where a sprite starts comes off the lower of its two corners, because a vertex pair is not
	// stored in a fixed order.
	NearCorner Start(const Sprite& s) { return {std::min(s.x0, s.x1), std::min(s.y0, s.y1), true}; }

	Delta SnapInBatch(const Sprite* v, u32 sprites, u32 i)
	{
		const NearCorner prev = (i >= 1) ? Start(v[i - 1]) : NearCorner{};
		const NearCorner next = (i + 1 < sprites) ? Start(v[i + 1]) : NearCorner{};
		return DropAbuttingAxes(Snap(v[i]), v[i].x1, v[i].y1, prev, next);
	}

	// One strip of Need for Speed Underground's bloom downsample: 16 pixels wide starting half a
	// pixel early, 32 texels of the screen behind it, tiled so each strip starts where the last
	// one ended.
	constexpr Sprite Strip(int k)
	{
		return Sprite{16 * 16 * k - 8, 0, 16 * 16 * (k + 1) - 8, 16 * 224, 16 * 32 * k, 0, 16 * 32 * (k + 1), 16 * 224};
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

TEST(GSSpriteEdgeSnap, AStripKeepsTheEdgeItsNeighbourStartsOn)
{
	// Sixteen strips, each ending exactly where the next begins. On its own every one of them
	// snaps -- the far edge is half a pixel short and 32 texels over 16 pixels makes the slide a
	// whole texel -- and doing it hands each neighbour's first device column a second draw.
	Sprite v[16];
	for (int k = 0; k < 16; k++)
		v[k] = Strip(k);

	ASSERT_EQ(v[0].x1, v[1].x0);
	ASSERT_EQ(Snap(v[0]).dx, 8);
	ASSERT_EQ(Snap(v[0]).du, 16);

	// Fifteen of them have a neighbour on their far edge and stay put.
	for (u32 i = 0; i < 15; i++)
		EXPECT_TRUE(SnapInBatch(v, 16, i).IsZero()) << "strip " << i;

	// The sixteenth ends the batch with nothing beyond it, so it still gets its edge back and the
	// strip as a whole ends where it should.
	const Delta last = SnapInBatch(v, 16, 15);
	EXPECT_EQ(last.dx, 8);
	EXPECT_EQ(last.du, 16);
}

TEST(GSSpriteEdgeSnap, AStripEmittedBackwardsIsStillRecognised)
{
	// Nothing says a batch runs left to right, so the sprite before is asked as well as the one
	// after. Same sixteen strips, reversed.
	Sprite v[16];
	for (int k = 0; k < 16; k++)
		v[k] = Strip(15 - k);

	for (u32 i = 1; i < 16; i++)
		EXPECT_TRUE(SnapInBatch(v, 16, i).IsZero()) << "strip " << i;

	// The rightmost strip is now first, and it is the one with nothing past its far edge.
	EXPECT_EQ(SnapInBatch(v, 16, 0).dx, 8);
}

TEST(GSSpriteEdgeSnap, ANeighbourThatDoesNotTouchChangesNothing)
{
	// Two strips with a pixel of daylight between them. Neither far edge is anybody's start, so
	// both keep the snap -- this is the case the guard must not swallow, and it is the shape of
	// the NASCAR seam the snap was written for.
	Sprite v[2] = {Strip(0), Strip(1)};
	v[1].x0 += 16; // the whole sprite, so its texel ratio stays exact and only the gap is new
	v[1].x1 += 16;

	EXPECT_EQ(SnapInBatch(v, 2, 0).dx, 8);
	EXPECT_EQ(SnapInBatch(v, 2, 1).dx, 8);
}

TEST(GSSpriteEdgeSnap, AnAbuttingAxisDoesNotSilenceTheOther)
{
	// Rows rather than columns: the sprites meet in Y and are staggered in X, so the Y snap goes
	// and the X snap stays.
	Sprite v[2] = {
		{0, 0, 16 * 32 + 8, 16 * 16 + 8, 0, 0, 16 * 65, 16 * 33},
		{16 * 40, 16 * 16 + 8, 16 * 72 + 8, 16 * 32 + 8, 0, 0, 16 * 65, 16 * 33},
	};

	const Delta lone = Snap(v[0]);
	ASSERT_EQ(lone.dx, 8);
	ASSERT_EQ(lone.dy, 8);

	const Delta d = SnapInBatch(v, 2, 0);
	EXPECT_EQ(d.dx, 8);
	EXPECT_EQ(d.du, 16);
	EXPECT_EQ(d.dy, 0);
	EXPECT_EQ(d.dv, 0);
}

TEST(GSSpriteEdgeSnap, ALoneSpriteHasNoNeighbourToDeferTo)
{
	// The single-sprite batch, which is what NASCAR Thunder's alpha-plane draw is: no neighbours,
	// so the guard is inert and the fix that motivated the snap still fires.
	const Sprite v[1] = {Strip(0)};
	EXPECT_EQ(SnapInBatch(v, 1, 0).dx, 8);
}

TEST(GSSpriteEdgeSnap, ANeighbourStoredBackToFrontStillCounts)
{
	// A sprite's two vertices come in whichever order the game sent them, so where a neighbour
	// starts is the lower of its corners and not simply the first one.
	Sprite v[2] = {Strip(0), Strip(1)};
	std::swap(v[1].x0, v[1].x1);
	std::swap(v[1].u0, v[1].u1);

	EXPECT_TRUE(SnapInBatch(v, 2, 0).IsZero());
}
