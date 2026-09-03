// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the sprite-union cover rule (GS/GSSpriteCover.h).
//
// GSState::SpriteDrawWithoutGaps() asks whether a draw's sprites tile, in one of three patterns.
// Xenosaga's per-frame alpha-knowledge eater is two sprites drawn one on top of the other, each
// exactly the whole target: not a tiling, so refused, although either one covers everything on its
// own. This rule asks the question the alpha tracker is really asking -- is every pixel of the
// rectangle written by at least one primitive -- and the cases below are the ones the corpus
// actually contains, plus the near miss that has to fail.
//
// The near miss is the case that matters. One corpus draw covers 99.22% of the rectangle it bounds
// -- a single 128-pixel row short -- and a rule that said yes to it would put a wrong known-bits
// pair on that target for the rest of the frame.
//
// Rides gs_vertex_tests -- the rule is a header, so it needs no extra linkage.

#include "GS/GSSpriteCover.h"

#include <gtest/gtest.h>

using namespace GSSpriteCover;

namespace
{
	constexpr GSVector4i Rect(int x, int y, int z, int w) { return GSVector4i::cxpr(x, y, z, w); }

	bool Covers(std::initializer_list<GSVector4i> rects, const GSVector4i& r)
	{
		GSVector4i buf[16];
		u32 n = 0;
		for (const GSVector4i& rect : rects)
			buf[n++] = rect;

		return UnionCoversRect(buf, n, r);
	}
} // namespace

TEST(GSSpriteCover, TwoCoincidentSpritesCoverWhatEitherOneCovers)
{
	// Xenosaga's draw, to the pixel: two sprites at (0,0)-(512,448) on a 512x448 target.
	const GSVector4i target = Rect(0, 0, 512, 448);
	EXPECT_TRUE(Covers({target, target}, target));
}

TEST(GSSpriteCover, TwoOverlappingSpritesCoverBetweenThem)
{
	const GSVector4i target = Rect(0, 0, 512, 448);
	EXPECT_TRUE(Covers({Rect(0, 0, 300, 448), Rect(200, 0, 512, 448)}, target));
	EXPECT_TRUE(Covers({Rect(0, 0, 512, 300), Rect(0, 200, 512, 448)}, target));
}

TEST(GSSpriteCover, TwoAbuttingSpritesCoverWithNoOverlapAtAll)
{
	const GSVector4i target = Rect(0, 0, 512, 448);
	EXPECT_TRUE(Covers({Rect(0, 0, 256, 448), Rect(256, 0, 512, 448)}, target));
}

TEST(GSSpriteCover, APairOneRowShortDoesNotCover)
{
	// The corpus's near miss: 16,256 of 16,384 pixels, one row of 128 left unwritten.
	const GSVector4i target = Rect(0, 0, 128, 128);
	EXPECT_FALSE(Covers({Rect(0, 0, 64, 127), Rect(64, 0, 128, 127)}, target));

	// And one column short, which the band sweep has to catch differently.
	EXPECT_FALSE(Covers({Rect(0, 0, 64, 128), Rect(64, 0, 127, 128)}, target));
}

TEST(GSSpriteCover, AHoleInTheMiddleIsFound)
{
	// Four sprites round the edges of the rectangle, leaving the centre unwritten. Every band is
	// covered at its left and right ends, so only a real sweep says no.
	const GSVector4i target = Rect(0, 0, 100, 100);
	EXPECT_FALSE(Covers(
		{Rect(0, 0, 100, 40), Rect(0, 60, 100, 100), Rect(0, 0, 40, 100), Rect(60, 0, 100, 100)}, target));
}

TEST(GSSpriteCover, MoreSpritesThanTheCapAnswerNo)
{
	// Above the cap the answer is no whatever the geometry, so a set that plainly covers still
	// comes back false. The class this rule pays on is two sprites.
	const GSVector4i target = Rect(0, 0, 512, 448);
	GSVector4i rects[MaxSprites + 1];
	for (u32 i = 0; i < MaxSprites + 1; i++)
		rects[i] = target;

	EXPECT_TRUE(UnionCoversRect(rects, MaxSprites, target));
	EXPECT_FALSE(UnionCoversRect(rects, MaxSprites + 1, target));
	EXPECT_FALSE(UnionCoversRect(rects, 0, target));
}

TEST(GSSpriteCover, ARectangleNothingReachesIsNotCovered)
{
	const GSVector4i target = Rect(0, 0, 512, 448);
	EXPECT_FALSE(Covers({Rect(0, 0, 512, 224)}, target));
	EXPECT_FALSE(Covers({Rect(0, 0, 256, 448), Rect(0, 0, 256, 448)}, target));
}

TEST(GSSpriteCover, AnEmptyRectangleIsNotAskedAbout)
{
	EXPECT_FALSE(Covers({Rect(0, 0, 512, 448)}, Rect(0, 0, 0, 0)));
	EXPECT_FALSE(Covers({Rect(0, 0, 512, 448)}, Rect(10, 10, 5, 20)));
}

TEST(GSSpriteCover, SpritesOutsideTheRectangleDoNotHelp)
{
	// The renderer clips each sprite to the draw rect before asking, so a rectangle that only
	// touches the outside contributes nothing here either.
	const GSVector4i target = Rect(100, 100, 200, 200);
	EXPECT_TRUE(Covers({Rect(0, 0, 300, 300)}, target));
	EXPECT_FALSE(Covers({Rect(0, 0, 100, 300), Rect(200, 0, 300, 300)}, target));
}
