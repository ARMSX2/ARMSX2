// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins which draws have their coordinate divided by Q at the vertex, and which
// keep Q and take the console's reciprocal per pixel.
//
// The GS does not divide. It multiplies by a reciprocal truncated to fourteen
// fractional bits below its leading mantissa bit (gs-grad case 34, gs-persp5), so
// dividing at the vertex computes a different number -- the exact quotient --
// wherever the result lands near a texel boundary. On a constant-quotient band
// that is every pixel at once.
//
// gs-persp3 and gs-persp4's s-only sections are exactly that shape: S ramping
// across the span with Q constant and not one. Our arm read 0.00% of 30,512
// readings on each while the console reads the ordinary chain at 98.24%.
// Instrumented, the draw arrived at the scanline with fst = 1 -- the affine path
// -- while carrying undivided STQ in its seed (S = 262392.25, T = 1572864.0,
// Q = 1.5), because two different predicates decided whether to divide and
// whether to say it had been divided, and they disagreed.
//
// With both driven from this one rule, s-only reads 98.74% and 98.72%.
//
// Sprites keep the divide: a sprite's Q comes from the second vertex rather than
// its own, which the scanline has no way to express.

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/SW/GSVertexQDivide.h"
#include "GS/GSState.h"

#include <gtest/gtest.h>

#include <vector>

// The s-only shape, in one cell: a triangle with Q constant and not one. It must
// keep its Q. Before the rule landed this returned true and the coordinate was
// divided exactly, which is every one of those 30,512 readings.
TEST(SwVertexQDivideTest, AConstantNonUnitQTriangleKeepsItsQ)
{
	EXPECT_FALSE(GSUseVertexQDivide(GS_TRIANGLE_CLASS, false, true, 1.5f));
}

// Q at one is the same answer by a different route: there is nothing to divide.
TEST(SwVertexQDivideTest, AUnitQTriangleKeepsItsQ)
{
	EXPECT_FALSE(GSUseVertexQDivide(GS_TRIANGLE_CLASS, false, true, 1.0f));
}

// A triangle whose Q varies has always taken the per-pixel divide.
TEST(SwVertexQDivideTest, AVaryingQTriangleKeepsItsQ)
{
	EXPECT_FALSE(GSUseVertexQDivide(GS_TRIANGLE_CLASS, false, false, 0.75f));
}

// A sprite whose Q varies has to be resolved at the vertex: its Q is the next
// vertex's, and the scanline cannot say that.
TEST(SwVertexQDivideTest, AVaryingQSpriteIsDividedAtTheVertex)
{
	EXPECT_TRUE(GSUseVertexQDivide(GS_SPRITE_CLASS, false, false, 0.75f));
}

// Mipmapping takes the divide off everything, as it always did -- the level
// selection needs the live Q.
TEST(SwVertexQDivideTest, MipmappingKeepsQEverywhere)
{
	EXPECT_FALSE(GSUseVertexQDivide(GS_SPRITE_CLASS, true, false, 0.75f));
	EXPECT_FALSE(GSUseVertexQDivide(GS_TRIANGLE_CLASS, true, true, 1.5f));
}

// ---------------------------------------------------------------------------
// Which route the scanline is told to take. Two rules, and they are not the same
// question: the one above is whether the coordinate was divided at the vertex,
// this one is whether the scanline reads it as a 16.16 integer (affine) or
// divides per pixel (perspective). Round 15 collapsed them and lost sixteen of
// gs-grad tc-snap's 10,240 readings; they are separate again here.

// Q identically one keeps the affine route, which is what the console was
// measured on: gs-grad's tc-snap is FST=0 with Q == 1.0 and gs-shade's FST=0 arm
// pins Q at one at both ends. Routing it through the perspective path instead
// costs those sixteen readings -- the reciprocal of one is exactly one, but the
// float-to-integer conversion rounds a boundary pixel the other way from the
// bit-cast.
TEST(SwVertexQDivideTest, AUnitQTriangleTakesTheAffineRoute)
{
	EXPECT_TRUE(GSUseAffineRoute(GS_TRIANGLE_CLASS, true, 1.0f));
}

// A Q that is constant but not one takes the perspective route and the console's
// truncated reciprocal. This is the s-only shape: 0.00% to 98.7% on gs-persp3 and
// gs-persp4 when it stopped being routed as affine.
TEST(SwVertexQDivideTest, AConstantNonUnitQTriangleTakesThePerspectiveRoute)
{
	EXPECT_FALSE(GSUseAffineRoute(GS_TRIANGLE_CLASS, true, 1.5f));
	EXPECT_FALSE(GSUseAffineRoute(GS_TRIANGLE_CLASS, true, 0.0013f));
}

// A varying Q has always taken the perspective route.
TEST(SwVertexQDivideTest, AVaryingQTriangleTakesThePerspectiveRoute)
{
	EXPECT_FALSE(GSUseAffineRoute(GS_TRIANGLE_CLASS, false, 0.75f));
}

// Sprites take the affine route whatever their Q, as they always have.
TEST(SwVertexQDivideTest, ASpriteAlwaysTakesTheAffineRoute)
{
	EXPECT_TRUE(GSUseAffineRoute(GS_SPRITE_CLASS, true, 1.0f));
	EXPECT_TRUE(GSUseAffineRoute(GS_SPRITE_CLASS, true, 1.5f));
	EXPECT_TRUE(GSUseAffineRoute(GS_SPRITE_CLASS, false, 0.75f));
}

// The two rules are independent, and the pair that matters is a constant non-unit
// Q triangle: it is neither divided at the vertex nor routed as affine.
TEST(SwVertexQDivideTest, TheTwoRulesAreDecidedSeparately)
{
	EXPECT_FALSE(GSUseVertexQDivide(GS_TRIANGLE_CLASS, false, true, 1.5f));
	EXPECT_FALSE(GSUseAffineRoute(GS_TRIANGLE_CLASS, true, 1.5f));

	// And a unit-Q triangle is not divided either, but is routed affine.
	EXPECT_FALSE(GSUseVertexQDivide(GS_TRIANGLE_CLASS, false, true, 1.0f));
	EXPECT_TRUE(GSUseAffineRoute(GS_TRIANGLE_CLASS, true, 1.0f));
}

// A full-screen composite sprite with FST=0 and Q identically one is the shape
// Spider-Man 3's draw 16,245 has, and the shape the console verdict on round 15
// turned on. It must take the pre-round-15 path on BOTH rules -- undivided at the
// vertex and routed affine -- so that it bit-casts its 16.16 coordinate exactly as
// it always did. Measured: 16,245 routes fst=1 at fd51170872 and at 790c69138c
// alike, so this pins an invariant rather than a change.
TEST(SwVertexQDivideTest, AUnitQStqSpriteIsUndividedAndAffine)
{
	EXPECT_FALSE(GSUseVertexQDivide(GS_SPRITE_CLASS, false, true, 1.0f));
	EXPECT_TRUE(GSUseAffineRoute(GS_SPRITE_CLASS, true, 1.0f));

	// And with mipmapping on, which forces the divide off by itself.
	EXPECT_FALSE(GSUseVertexQDivide(GS_SPRITE_CLASS, true, true, 1.0f));
	EXPECT_TRUE(GSUseAffineRoute(GS_SPRITE_CLASS, true, 1.0f));
}

// ---------------------------------------------------------------------------
// The truth table, and the invariant that binds the two rules together.
//
// They were written as separate predicates and drifted apart twice. The second
// time cost Spider-Man 3's building facades: a sprite with Q constant at 64 kept
// the affine route and lost its vertex divide, so the scanline bit-cast an
// UNDIVIDED ST as though it were a 16.16 texel coordinate. Draw 20 of that frame
// is the first draw whose output moves, and 116 draws in its first 540 inherit
// it -- 92.6% of the frame's moved pixels.
//
// The invariant is one line and it is what the enumeration below asserts on
// every row the code can distinguish:
//
//     affine route  =>  (divided at the vertex) or (Q is identically one) or FST
//
// An affine route means the scanline reads a 16.16 integer and never touches Q.
// So if Q is not one, it must already have been divided out, or the coordinate
// is simply wrong. UV draws (FST) carry no Q at all and are exempt.

namespace
{
struct QDivideCase
{
	u32 primclass;
	bool mipmap;
	bool eq_q;
	float min_q;
	bool fst;
};

// Every combination the two predicates distinguish: the three primitive classes
// they branch on, both mipmap states, both eq_q states, Q at one and away from
// one, and both FST states.
std::vector<QDivideCase> AllQDivideCases()
{
	std::vector<QDivideCase> v;
	for (const u32 pc : {static_cast<u32>(GS_POINT_CLASS), static_cast<u32>(GS_LINE_CLASS),
			 static_cast<u32>(GS_TRIANGLE_CLASS), static_cast<u32>(GS_SPRITE_CLASS)})
		for (const bool mip : {false, true})
			for (const bool eq : {false, true})
				for (const float q : {1.0f, 1.5f, 64.0f, 0.0013f})
					for (const bool fst : {false, true})
						v.push_back({pc, mip, eq, q, fst});
	return v;
}
} // namespace

TEST(SwVertexQDivideTest, TheAffineRouteAlwaysHasSomethingItCanRead)
{
	for (const QDivideCase& c : AllQDivideCases())
	{
		const bool divided = GSUseVertexQDivide(c.primclass, c.mipmap, c.eq_q, c.min_q);
		// GSRendererSW promotes to the affine route only in its non-mipmap branch:
		// with mipmapping active gd.sel.fst stays PRIM->FST, because the level
		// selection needs the live Q. The model has to carry that or it asks the
		// invariant of rows the renderer never produces.
		const bool affine = c.fst || (!c.mipmap && GSUseAffineRoute(c.primclass, c.eq_q, c.min_q));
		const bool unit_q = c.eq_q && c.min_q == 1.0f;

		if (affine)
		{
			EXPECT_TRUE(divided || unit_q || c.fst)
				<< "affine route with an undivided, non-unit Q: primclass " << c.primclass
				<< " mipmap " << c.mipmap << " eq_q " << c.eq_q << " min_q " << c.min_q
				<< " fst " << c.fst;
		}

		// And the converse half: nothing is divided that the scanline will then
		// divide again, which would apply Q twice.
		if (divided)
		{
			EXPECT_TRUE(affine) << "divided at the vertex but walked as perspective: primclass "
								<< c.primclass << " eq_q " << c.eq_q << " min_q " << c.min_q;
		}
	}
}

// Mipmapping takes the divide off everything, so on those rows the invariant can
// only hold because nothing is routed affine that needs it. Pinned separately so
// a future mipmap change cannot quietly break the pairing.
TEST(SwVertexQDivideTest, MipmappedDrawsNeverDivideAndNeverNeedTo)
{
	for (const QDivideCase& c : AllQDivideCases())
	{
		if (!c.mipmap)
			continue;

		EXPECT_FALSE(GSUseVertexQDivide(c.primclass, true, c.eq_q, c.min_q));
	}
}

// The two concrete regressions this round closed, as cells.

// Spider-Man 3 draw 20: a sprite, FST=0, Q constant at 64. It takes the affine
// route, so it must be divided at the vertex.
TEST(SwVertexQDivideTest, AConstantNonUnitQSpriteIsDividedAtTheVertex)
{
	EXPECT_TRUE(GSUseAffineRoute(GS_SPRITE_CLASS, true, 64.0f));
	EXPECT_TRUE(GSUseVertexQDivide(GS_SPRITE_CLASS, false, true, 64.0f));
}

// gs-persp4's s-only: a triangle, FST=0, Q constant at 1.5. It takes the
// perspective route, so it must NOT be divided -- that is round 15's gain, and
// it is untouched by the sprite fix.
TEST(SwVertexQDivideTest, AConstantNonUnitQTriangleIsNotDivided)
{
	EXPECT_FALSE(GSUseAffineRoute(GS_TRIANGLE_CLASS, true, 1.5f));
	EXPECT_FALSE(GSUseVertexQDivide(GS_TRIANGLE_CLASS, false, true, 1.5f));
}
