// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// One texel-coordinate grain per primitive, and the gradient on a grid a thousandth
// of it, as gs-mag2 measured it on hardware and gs-mag1 was re-read under it.
//
// The capture draws one affine STQ triangle per region and reads the sampled
// coordinate back through a naming texture, so a column is a number rather than a
// threshold. Twenty-two of its twenty-three arms close at 256 of 256 columns under
// the law below, with no fitted constant in it:
//
//   1. one grain G for the whole primitive, from the LARGEST of its three vertices
//   2. every vertex truncated toward zero onto G
//   3. the gradient formed from those vertices, floored onto a grid of G / 1024 --
//      strictly, unless twice the triangle's area is a power of two
//   4. the walk from the seed vertex at that gradient
//
// GSCoordinateWalk.h carries the reading and the limits. This suite is its
// executable half: the arms' own numbers, in the units the renderer carries them in.
//
// Every coordinate below is in 16.16 units of a texel, and every vertex value is
// what the software renderer actually holds -- the GIF's S scaled by 2^(16 + TW),
// with the front end's own mantissa clear already applied and the linear filter's
// half texel already subtracted. The arms are TW 6 and TH 5, so the grain's floor
// exponent is 8 on U and 7 on V.

#include "GS/Renderers/SW/GSCoordinateWalk.h"

#include <gtest/gtest.h>

namespace
{
// TEX0 log2 width + 2, log2 height + 2: gs-mag2's naming texture is 64 x 32.
constexpr s32 kFloorExp[2] = {8, 7};

// The half texel the linear filter takes off at the vertex, in the same units.
constexpr float kHalf = 32768.0f;

// V is held at 1 + 1/32 texels on every arm, which is 2,162,688 units.
constexpr float kHeldV = 2162688.0f - kHalf;

GSVector4 Vertex(double u)
{
	return GSVector4(static_cast<float>(u - 32768.0), kHeldV, 1.0f, 0.0f);
}

struct Arm
{
	const char* name;
	double u0, u1, u2;  // the three vertices' U, before the half texel comes off
	bool pow2;          // twice the triangle's area is a power of two
	double width;       // pixels from the seed vertex to the far one
};

// The arms this suite pins, with the vertex words already through the front end.
constexpr Arm kA7{"A7-m10-r128", 66758656.0, 76705792.0, 66758656.0, false, 2990.0};
constexpr Arm kA3{"A3-m4-r128", 698368.0, 10649088.0, 698368.0, false, 2990.0};
constexpr Arm kB7{"B7-m8-r2", 16841728.0, 16950272.0, 16841728.0, false, 520.0};
constexpr Arm kB11{"B11-m10-r2", 67170304.0, 67280896.0, 67170304.0, false, 520.0};
constexpr Arm kB12{"B12-m10-r128", 67145728.0, 67768320.0, 67145728.0, false, 2990.0};
constexpr Arm kB15{"B15-m8-r2-p2", 16841728.0, 16948224.0, 16841728.0, true, 512.0};
constexpr Arm kB16{"B16-m10-r2-p2", 67170304.0, 67276800.0, 67170304.0, true, 512.0};

GSCoordinateGrain GrainOf(const Arm& a)
{
	return GSCoordinateGrainOfPrimitive(Vertex(a.u0), Vertex(a.u1), Vertex(a.u2), kFloorExp, kHalf);
}

// One vertex truncated onto the primitive's grain, back in coordinate units.
double OnGrain(const Arm& a, double u)
{
	const GSCoordinateGrain g = GrainOf(a);

	return static_cast<double>(GSCoordinateOnGrain(Vertex(u), g, kHalf).x) + 32768.0;
}

// The gradient the truncated vertices give, before the grid takes it.
double VertexGradient(const Arm& a)
{
	return (OnGrain(a, a.u1) - OnGrain(a, a.u0)) / a.width;
}

// That gradient after stage 3.
double Walked(const Arm& a)
{
	const GSVector4 d(static_cast<float>(VertexGradient(a)), 0.0f, 0.0f, 0.0f);

	return GSCoordinateGradientOnGrain(d, GrainOf(a), a.pow2).x;
}

// Truncation toward zero onto a power-of-two grid, in exact integers, for the
// identity check below.
s64 TruncOnto(s64 v, s64 grid)
{
	const s64 m = v < 0 ? -v : v;

	return (v < 0 ? -1 : 1) * (m / grid) * grid;
}
} // namespace

// ★ THE GRAIN IS ONE NUMBER FOR THE PRIMITIVE, NOT ONE PER VERTEX.
//
// A7 is the arm that says so. Its seed vertex sits at 1,018.66 texels, one binade
// below its far vertex at 1,170.49, so its own nine-bit clear is a grain of 2,048
// units where the primitive's is 4,096. Read per vertex the seed loses nothing and
// the arm reads 2,565 units at column zero, which no walk term reaches; read over
// the primitive its residue is 2,048 and the arm closes at 256 of 256.
TEST(GsSwCoordinateGrain, TheGrainComesFromTheLargestVertexOfThePrimitive)
{
	EXPECT_EQ(GrainOf(kA7).exp[0], 12); // 4,096 units, not the seed's own 2,048

	EXPECT_EQ(GrainOf(kB7).exp[0], 10);  //  1,024
	EXPECT_EQ(GrainOf(kB12).exp[0], 12); //  4,096
	EXPECT_EQ(GrainOf(kB15).exp[0], 10); //  1,024
	EXPECT_EQ(GrainOf(kA3).exp[0], 9);   //    512, from the far vertex again
}

// ★ THE FRONT END'S OWN CLEAR IS SUBSUMED BY IT, WHICH IS WHY THIS LIVES IN THE
// SOFTWARE RENDERER AND GSState::Draw IS UNTOUCHED.
//
// The front end truncates each vertex onto a finer power-of-two grid. The primitive
// grain is a multiple of that grid, so truncating the already-cleared value onto it
// gives the same number as truncating the raw GIF value would -- which is what makes
// widening the rule here identical to widening it up there, with nothing changed for
// the hardware renderers.
TEST(GsSwCoordinateGrain, TruncatingTheClearedVertexLandsWhereTheRawVertexWould)
{
	// A7's far vertex is the one the front end actually moved: 76,709,376 raw,
	// 76,705,792 after its own 4,096-unit clear. Both land on 76,705,792 at the
	// primitive's grain. Its seed vertex was already clear at 2,048 and the
	// primitive's 4,096 takes a further 2,048 off it.
	EXPECT_EQ(OnGrain(kA7, 76705792.0), 76705792.0);
	EXPECT_EQ(static_cast<double>(TruncOnto(76709376, 4096)), OnGrain(kA7, 76705792.0));

	EXPECT_EQ(OnGrain(kA7, 66758656.0), 66756608.0);
	EXPECT_EQ(static_cast<double>(TruncOnto(66758656, 4096)), OnGrain(kA7, 66758656.0));

	// And on every other arm, where the cleared value is already on the grain.
	EXPECT_EQ(OnGrain(kB7, 16841728.0), 16841728.0);
	EXPECT_EQ(static_cast<double>(TruncOnto(16842440, 1024)), OnGrain(kB7, 16841728.0));
	EXPECT_EQ(OnGrain(kB12, 67145728.0), 67145728.0);
	EXPECT_EQ(static_cast<double>(TruncOnto(67147880, 4096)), OnGrain(kB12, 67145728.0));
	EXPECT_EQ(OnGrain(kB15, 16841728.0), 16841728.0);
	EXPECT_EQ(static_cast<double>(TruncOnto(16842440, 1024)), OnGrain(kB15, 16841728.0));

	// The property itself, swept: truncating twice onto nested power-of-two grids is
	// truncating once onto the coarser of them, on either side of zero.
	for (s64 fine = 1; fine <= 4096; fine <<= 1)
	{
		for (s64 coarse = fine; coarse <= 8192; coarse <<= 1)
		{
			for (s64 v = -20000; v <= 20000; v += 331)
				EXPECT_EQ(TruncOnto(TruncOnto(v, fine), coarse), TruncOnto(v, coarse))
					<< fine << " then " << coarse << " at " << v;
		}
	}
}

// ★ A GRADIENT THAT LANDS EXACTLY ON A GRID POINT TAKES THE POINT BELOW IT.
//
// A3 and A5 are what make the floor strict: their quotient is exactly 3,328 units a
// pixel, an exact multiple of their grid of a half unit, and the console reads one
// step below it. The setup multiplies by a truncated reciprocal of twice the area,
// so the product comes out a hair low and the floor then drops a whole step.
TEST(GsSwCoordinateGrain, AGradientOnAGridPointFallsOneStepOnANonPowerOfTwoArea)
{
	EXPECT_EQ(VertexGradient(kA3), 3328.0);
	EXPECT_EQ(Walked(kA3), 3327.5);

	// B16's geometry with its power-of-two area taken away: 208 exactly on a grid of
	// four units becomes 204.
	const GSVector4 d(208.0f, 0.0f, 0.0f, 0.0f);
	EXPECT_EQ(GSCoordinateGradientOnGrain(d, GrainOf(kB16), false).x, 204.0f);
}

// ★ AND IT KEEPS ITS OWN VALUE WHERE TWICE THE AREA IS A POWER OF TWO.
//
// B15 and B16 are the same two coordinates as B7 and B11 on a triangle whose twice
// area is 2^25. Their quotients are exactly 208, an exact grid multiple on both, and
// the console keeps them: there the setup's divide is exact and there is no hair to
// lose. The magnitude term is still fully present on those arms -- 712 and 3,784
// units of seed residue -- which is what separates stage 2 from stage 3.
TEST(GsSwCoordinateGrain, AGradientOnAGridPointHoldsOnAPowerOfTwoArea)
{
	EXPECT_EQ(VertexGradient(kB15), 208.0);
	EXPECT_EQ(Walked(kB15), 208.0f);

	EXPECT_EQ(VertexGradient(kB16), 208.0);
	EXPECT_EQ(Walked(kB16), 208.0f);
}

// ★ THE THREE COLUMNS THE LAW IS READ OFF, END TO END.
//
// B7 and B15 carry the same coordinate at the same grain and differ only in the
// area; B12 carries a run-up of 128 pixels at four times the grain. Each arm's
// numbers are the seed vertex's residue against the exact plane and the gradient the
// console walked with, both of which the capture reads as numbers.
TEST(GsSwCoordinateGrain, B7WalksTwoHundredAndEightFromASeedSevenHundredAndTwelveLow)
{
	EXPECT_EQ(GrainOf(kB7).exp[0], 10);
	EXPECT_EQ(OnGrain(kB7, kB7.u0), 16841728.0);
	EXPECT_EQ(16842440.0 - OnGrain(kB7, kB7.u0), 712.0); // the console's (704, 720]
	EXPECT_NEAR(VertexGradient(kB7), 208.738461538, 1e-6);
	EXPECT_EQ(Walked(kB7), 208.0f);
}

TEST(GsSwCoordinateGrain, B12WalksTwoHundredAndEightFromASeedTwoThousandLow)
{
	EXPECT_EQ(GrainOf(kB12).exp[0], 12);
	EXPECT_EQ(OnGrain(kB12, kB12.u0), 67145728.0);
	EXPECT_EQ(67147880.0 - OnGrain(kB12, kB12.u0), 2152.0);
	EXPECT_NEAR(VertexGradient(kB12), 208.224749164, 1e-6);
	EXPECT_EQ(Walked(kB12), 208.0f);
}

TEST(GsSwCoordinateGrain, B15IsB7WordForWordOnAPowerOfTwoArea)
{
	EXPECT_EQ(GrainOf(kB15).exp[0], GrainOf(kB7).exp[0]);
	EXPECT_EQ(OnGrain(kB15, kB15.u0), OnGrain(kB7, kB7.u0));
	EXPECT_EQ(16842440.0 - OnGrain(kB15, kB15.u0), 712.0);
	EXPECT_EQ(Walked(kB15), Walked(kB7));
}

// B11 against B12: the same magnitude and the same grain, a run-up of two against a
// run-up of 128, and the grid moves one of them and not the other. That pair is what
// refuses a shortfall that is a function of the coordinate's size.
TEST(GsSwCoordinateGrain, TheRunUpMovesTheGradientAndNotTheSeed)
{
	EXPECT_EQ(GrainOf(kB11).exp[0], GrainOf(kB12).exp[0]);
	EXPECT_EQ(67174088.0 - OnGrain(kB11, kB11.u0), 3784.0);
	EXPECT_EQ(Walked(kB11), 212.0f);
	EXPECT_EQ(Walked(kB12), 208.0f);
}

// A still axis does not move, and neither does one walking backward onto a grid
// point. The strict step down is the limit of a relative deficit on a product, and a
// product of zero has nothing to lose; taking the step regardless would start a still
// coordinate walking backwards a grid step a pixel.
TEST(GsSwCoordinateGrain, AStillAxisIsNotPushedOffZero)
{
	const GSCoordinateGrain g = GrainOf(kB7);

	EXPECT_EQ(GSCoordinateGradientOnGrain(GSVector4(0.0f, 0.0f, 0.0f, 0.0f), g, false).x, 0.0f);
	EXPECT_EQ(GSCoordinateGradientOnGrain(GSVector4(0.0f, 0.0f, 0.0f, 0.0f), g, true).x, 0.0f);
	EXPECT_EQ(GSCoordinateGradientOnGrain(GSVector4(-208.0f, 0.0f, 0.0f, 0.0f), g, false).x, -208.0f);
}

// Q and fog ride through both stages untouched: the rule is about S and T.
TEST(GsSwCoordinateGrain, QAndFogRideThroughUntouched)
{
	const GSCoordinateGrain g = GrainOf(kB7);
	const GSVector4 t(16808960.0f, kHeldV, 1.0f, 0.25f);
	const GSVector4 out = GSCoordinateOnGrain(t, g, kHalf);

	EXPECT_EQ(out.z, 1.0f);
	EXPECT_EQ(out.w, 0.25f);

	const GSVector4 d(208.7f, 0.0f, 3.5f, 0.75f);
	const GSVector4 dout = GSCoordinateGradientOnGrain(d, g, false);

	EXPECT_EQ(dout.z, 3.5f);
	EXPECT_EQ(dout.w, 0.75f);
}
