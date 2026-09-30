// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The console's S, T and Q planes for a perspective triangle.
//
// The GS does not interpolate S, T and Q as the exact plane through the vertices.
// Five console captures on an SCPH-30001 (gs-sm3b, gs-sm3c, gs-sm3d, gs-tri1a-f)
// read the plane it does build, per pixel, and one integer arithmetic reproduces
// every reading: the low eight mantissa bits of each input dropped, one block
// exponent over all nine values, the vertices floored onto 2^(E-14), a gradient
// from a table-and-Newton reciprocal of the area truncated onto g/1024, the plane
// hung from one vertex, and every pixel floored to g/4. GSPerspectivePlane.h has the
// rules and what is modelled rather than measured.
//
// This half is the arithmetic. gs_sw_perspective_plane_golden.inc holds 212
// triangles: the game's own draw (Spider-Man 3, draw 16033), gs-tri1's nudges of
// its triangle 54, and synthetic families (all four anchor shapes, y ties and
// vertical long edges, Q crossing 2.0, power-of-two areas, negative gradients, vertices with
// a negative Q). The models the expected values come from have no rule for a negative Q,
// so the generator negates such a vertex whole first, as GSPerspectivePlane.h does.
// Their expected values are gs-sm3d's Fraction model (analysis/model.py), NOT this
// header's integers: exact rationals against integers is the whole comparison.
// Regenerate with hardware-oracle/captures/gs-sm3b/analysis-impl/gen_golden.py.
//
// The scanline half, which draws a perspective triangle through the real
// rasterizer and both scanlines, is gs_sw_perspective_plane_scanline_tests.cpp.

#include "common/Pcsx2Defs.h"

#include "GS/Renderers/SW/GSColourWalk.h"
#include "GS/Renderers/SW/GSPerspectivePlane.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
struct GoldenSample
{
	s32 x, y;
	s32 h[3]; ///< counts of g/4 for S, T, Q
};

struct Golden
{
	const char* name;
	s32 x16[3], y16[3]; ///< 12.4, XYOFFSET taken off
	u32 s[3], t[3], q[3]; ///< the GIF's float32 words
	s32 exp, anchor;
	s32 n[3][3]; ///< [attribute][vertex]: the vertex as a count of g
	s64 gx[3], gy[3];
	int nsamp;
	GoldenSample samp[6];
};

const Golden kGolden[] = {
#include "gs_sw_perspective_plane_golden.inc"
};

float FloatOf(u32 word)
{
	float f;

	std::memcpy(&f, &word, sizeof(f));

	return f;
}

struct Built
{
	GSPerspectivePlane p;
	float s[3], t[3], q[3];
};

/// The golden triangle's plane, with its vertices taken in the order `order` (three
/// source indices).
Built Build(const Golden& g, const int order[3])
{
	Built b;
	s32 x[3], y[3];

	for (int i = 0; i < 3; i++)
	{
		x[i] = g.x16[order[i]];
		y[i] = g.y16[order[i]];
		b.s[i] = FloatOf(g.s[order[i]]);
		b.t[i] = FloatOf(g.t[order[i]]);
		b.q[i] = FloatOf(g.q[order[i]]);
	}

	GSPerspectivePlaneSetup(x, y, b.s, b.t, b.q, b.p);

	return b;
}

const int kIdentity[3] = {0, 1, 2};
} // namespace

// ★ EVERY GOLDEN TRIANGLE, AGAINST THE FRACTION MODEL.
//
// Exponent, anchor, the three vertex counts of each attribute, both gradients of
// each, and the plane's value at the sampled pixels, floored to g/4. Two hundred
// triangles is enough to have every branch of the reciprocal in it: the table's
// 256 bins are sampled by the game's own areas, the power-of-two exception by the
// synthetic ones.
TEST(GsSwPerspectivePlane, EveryGoldenTriangleMatchesTheFractionModel)
{
	ASSERT_GE(std::size(kGolden), 212u);

	for (const Golden& g : kGolden)
	{
		const Built b = Build(g, kIdentity);

		ASSERT_TRUE(b.p.valid) << g.name;
		EXPECT_EQ(b.p.exp, g.exp) << g.name;
		EXPECT_EQ(b.p.anchor, g.anchor) << g.name;

		for (int k = 0; k < 3; k++)
		{
			EXPECT_EQ(b.p.gx[k], g.gx[k]) << g.name << " attribute " << k;
			EXPECT_EQ(b.p.gy[k], g.gy[k]) << g.name << " attribute " << k;
			EXPECT_EQ(b.p.value[k], g.n[k][g.anchor]) << g.name << " attribute " << k;
		}

		for (int i = 0; i < g.nsamp; i++)
		{
			const GoldenSample& s = g.samp[i];

			for (int k = 0; k < 3; k++)
			{
				EXPECT_EQ(GSPerspectivePlanePixel(GSPerspectivePlaneValue(b.p, k, s.x, s.y)), s.h[k])
					<< g.name << " attribute " << k << " at (" << s.x << ", " << s.y << ")";
			}
		}
	}
}

// ★ THE VERTEX ORDER IS NOT IN THE ANSWER.
//
// gs-sm3d issued one triangle's vertices in all six orders and the console's plane
// did not move: the anchor is a property of the geometry, and the gradient is the
// plane's own. The header takes vertices in any order for that reason, so all six
// permutations of every golden triangle must give one plane.
TEST(GsSwPerspectivePlane, TheVertexOrderIsNotInTheAnswer)
{
	static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

	for (const Golden& g : kGolden)
	{
		const Built ref = Build(g, kIdentity);

		for (const auto& order : perms)
		{
			const Built b = Build(g, order);

			ASSERT_TRUE(b.p.valid) << g.name;
			EXPECT_EQ(b.p.exp, ref.p.exp) << g.name;
			EXPECT_EQ(order[b.p.anchor], g.anchor) << g.name << ": another vertex anchors";
			EXPECT_EQ(b.p.xa, ref.p.xa) << g.name;
			EXPECT_EQ(b.p.ya, ref.p.ya) << g.name;

			for (int k = 0; k < 3; k++)
			{
				EXPECT_EQ(b.p.value[k], ref.p.value[k]) << g.name;
				EXPECT_EQ(b.p.gx[k], ref.p.gx[k]) << g.name << " attribute " << k;
				EXPECT_EQ(b.p.gy[k], ref.p.gy[k]) << g.name << " attribute " << k;
			}
		}
	}
}

// ★ THE ANCHOR IS THE COLOUR WALK'S, VERTEX FOR VERTEX.
//
// Models A and B took the colour interpolator's spine rule for the plane; gs-sm3d
// fitted its own ("an end of the longest y edge") and the two agree everywhere but
// a vertical long edge, where the colour walk anchors on the top and gs-sm3d's
// rule on the bottom. This pins that the header decides exactly what GSSetupColourWalk
// decides, on every golden triangle, including the vertical ones. The colour walk
// forms the spine in float, which is exact on these lattice positions.
TEST(GsSwPerspectivePlane, TheAnchorIsTheColourWalksVertexForVertex)
{
	int vertical_long_edges = 0;

	for (const Golden& g : kGolden)
	{
		int order[3] = {0, 1, 2};

		// The setup hands the walk its vertices sorted by y; ties keep their order.
		std::stable_sort(order, order + 3, [&g](int a, int b) { return g.y16[a] < g.y16[b]; });

		GSVertexSW v[3];

		for (int i = 0; i < 3; i++)
		{
			v[i] = GSVertexSW::zero();
			v[i].p = GSVector4(static_cast<float>(g.x16[order[i]]) / 16.0f,
				static_cast<float>(g.y16[order[i]]) / 16.0f, 0.0f, 0.0f);
		}

		GSColourWalk walk = {};

		GSSetupColourWalk(v[0], v[1], v[2], GSVertexSW::zero(), GSVertexSW::zero(), 4, walk);

		EXPECT_EQ(walk.xr * 16.0f, static_cast<float>(g.x16[g.anchor])) << g.name;
		EXPECT_EQ(walk.yr * 16.0f, static_cast<float>(g.y16[g.anchor])) << g.name;

		int top = 0, bottom = 0;

		for (int i = 1; i < 3; i++)
		{
			if (g.y16[i] < g.y16[top] || (g.y16[i] == g.y16[top] && g.x16[i] < g.x16[top]))
				top = i;
			if (g.y16[i] > g.y16[bottom] || (g.y16[i] == g.y16[bottom] && g.x16[i] < g.x16[bottom]))
				bottom = i;
		}

		vertical_long_edges += (g.x16[top] == g.x16[bottom]) ? 1 : 0;
	}

	// The set must contain the case the two rules split on, or this is not a test
	// of it.
	EXPECT_GE(vertical_long_edges, 4);
}

// The same, swept: a lattice of small triangles, every ordering, so the colour walk's
// float spine and the header's integer cross product are compared where a rounding
// could tell them apart.
TEST(GsSwPerspectivePlane, TheAnchorAgreesWithTheColourWalkOnALattice)
{
	int checked = 0;

	for (int a = 0; a < 6; a++)
	{
		for (int b = 0; b < 6; b++)
		{
			for (int c = 0; c < 6; c++)
			{
				for (int d = 0; d < 6; d++)
				{
					const s32 x[3] = {0, a * 3 + 1, b * 5 - 4};
					const s32 y[3] = {0, c * 2, d * 3 + 1};
					const s64 cross = static_cast<s64>(x[1]) * y[2] - static_cast<s64>(x[2]) * y[1];

					if (cross == 0)
						continue;

					int order[3] = {0, 1, 2};

					std::stable_sort(order, order + 3, [&y](int p, int q) { return y[p] < y[q]; });

					GSVertexSW v[3];

					for (int i = 0; i < 3; i++)
					{
						v[i] = GSVertexSW::zero();
						v[i].p = GSVector4(static_cast<float>(x[order[i]]), static_cast<float>(y[order[i]]), 0.0f, 0.0f);
					}

					GSColourWalk walk = {};

					GSSetupColourWalk(v[0], v[1], v[2], GSVertexSW::zero(), GSVertexSW::zero(), 4, walk);

					const int anchor = GSPerspectiveAnchor(x, y);

					EXPECT_EQ(walk.xr, static_cast<float>(x[anchor])) << a << b << c << d;
					EXPECT_EQ(walk.yr, static_cast<float>(y[anchor])) << a << b << c << d;
					checked++;
				}
			}
		}
	}

	EXPECT_GT(checked, 800);
}

// ★ THE RECIPROCAL IS A HAIR SHORT, AND THE POWER-OF-TWO EXCEPTION IS EXACT.
//
// gs-tri1f measured the shortfall between 2.259e-6 and 2.376e-6 on a triangle of
// twice-area 574,798,354. A table seed and one Newton step lands at 2.28e-6 there,
// where a reciprocal cut to eighteen bits gives 2.9e-6. 16,383,962 is what that
// step leaves of an exact 16,384,000.
TEST(GsSwPerspectiveGradient, TheShortfallOnGsTri1fsTriangleIsInsideItsBracket)
{
	constexpr s64 kArea = 574798354;
	constexpr s64 kExact = 1000 * (static_cast<s64>(1) << GS_PLANE_ACCUM_BITS);

	const s64 got = GSPerspectiveGradient(kArea * 1000, kArea);
	const double shortfall = 1.0 - static_cast<double>(got) / static_cast<double>(kExact);

	EXPECT_EQ(got, 16383962);
	// The gradient is truncated to an integer, so the shortfall read back carries up
	// to one part in 1.6e7 of that as well.
	EXPECT_GT(shortfall, 2.259e-6);
	EXPECT_LT(shortfall, 2.376e-6);
}

// A gradient whose exact value is a grid point takes the point below it when the
// reciprocal is short, on either side of zero (truncation is toward zero), and holds
// it when the area is a power of two and the division is exact.
TEST(GsSwPerspectiveGradient, AnExactGridPointFallsOneStepUnlessTheAreaIsAPowerOfTwo)
{
	// c = 3 * 2^10: the exact gradient of numerator 3t is 16t.
	EXPECT_EQ(GSPerspectiveGradient(3, 3 * 1024), 15);
	EXPECT_EQ(GSPerspectiveGradient(300, 3 * 1024), 1599);
	EXPECT_EQ(GSPerspectiveGradient(-300, 3 * 1024), -1599);
	EXPECT_EQ(GSPerspectiveGradient(-300, -3 * 1024), 1599);
	EXPECT_EQ(GSPerspectiveGradient(300, -3 * 1024), -1599);

	// A power of two: exact, and truncated toward zero where it is not a whole number.
	EXPECT_EQ(GSPerspectiveGradient(17, 4096), 68);
	EXPECT_EQ(GSPerspectiveGradient(-17, 4096), -68);
	EXPECT_EQ(GSPerspectiveGradient(17, 65536), 4);
	EXPECT_EQ(GSPerspectiveGradient(-17, 65536), -4);
	EXPECT_EQ(GSPerspectiveGradient(17, static_cast<s64>(1) << 30), 0);
}

// An area whose mantissa is exactly the middle of a seed bin has an exact seed, and
// the Newton step keeps it exact: nothing is short there, so a grid point holds.
TEST(GsSwPerspectiveGradient, ASeedBinMiddleIsExact)
{
	for (s64 idx : {0, 1, 100, 255})
	{
		const s64 area = (513 + 2 * idx) << 11; // m = (513 + 2 idx) / 512, top bit 20
		const s64 numerator = area * 7;

		EXPECT_EQ(GSPerspectiveGradient(numerator, area), 7 * (static_cast<s64>(1) << GS_PLANE_ACCUM_BITS))
			<< "bin " << idx;
	}
}

// The step is a reciprocal, not a divide: across a sweep of areas and numerators it
// is never above the exact truncated quotient, and never more than the seed's own
// error (2^-18 of it, plus the one unit truncation loses) below it.
TEST(GsSwPerspectiveGradient, NeverAboveExactAndNeverFarBelow)
{
	int checked = 0;

	for (s64 area = 3; area < 3000000000ll; area = area * 7 / 5 + 13)
	{
		for (s64 numerator : {1ll, 12345ll, -987654ll, 40000000000ll, -6000000000ll})
		{
			const s64 got = GSPerspectiveGradient(numerator, area);
			const long double exact = static_cast<long double>(numerator) * (1 << GS_PLANE_ACCUM_BITS) / static_cast<long double>(area);
			const long double lo = std::fabs(exact) * (1.0L - 4.0e-6L) - 1.0L;

			EXPECT_LE(std::fabs(static_cast<long double>(got)), std::fabs(exact)) << area << " " << numerator;
			EXPECT_GE(std::fabs(static_cast<long double>(got)), lo) << area << " " << numerator;
			const bool negative = got != 0 && ((numerator < 0) != (area < 0));

			EXPECT_EQ(got < 0, negative) << area << " " << numerator;
			checked++;
		}
	}

	EXPECT_GT(checked, 100);
}

// ★ THE INPUTS, ONE STEP AT A TIME.
TEST(GsSwPerspectivePlane, TheLowEightMantissaBitsGo)
{
	// 1 + 2^-23 * 255 has every one of the low eight bits set.
	const float v = FloatOf(0x3f8000ffu);

	EXPECT_EQ(GSPlaneCutMantissa(v), 1.0f);
	// Toward zero, on either side.
	EXPECT_EQ(GSPlaneCutMantissa(-v), -1.0f);
	// The ninth bit is kept.
	EXPECT_EQ(GSPlaneCutMantissa(FloatOf(0x3f800100u)), FloatOf(0x3f800100u));
}

TEST(GsSwPerspectivePlane, OneExponentIsSharedByAllNineValues)
{
	const s32 x[3] = {0, 160, 0};
	const s32 y[3] = {0, 0, 160};

	// Q alone would give E = 0; S's 5.0 gives E = 2 to S, T and Q alike.
	{
		const float s[3] = {5.0f, 0.5f, 0.25f}, t[3] = {0.5f, 0.25f, 0.125f}, q[3] = {1.0f, 1.5f, 1.25f};
		GSPerspectivePlane p;

		GSPerspectivePlaneSetup(x, y, s, t, q, p);
		EXPECT_TRUE(p.valid);
		EXPECT_EQ(p.exp, 2);
	}

	// Q reaching 2.0 moves it to one from every value below.
	{
		const float s[3] = {0.5f, 0.25f, 0.75f}, t[3] = {0.5f, 0.25f, 0.125f}, q[3] = {1.9999f, 2.0f, 1.5f};
		GSPerspectivePlane p;

		GSPerspectivePlaneSetup(x, y, s, t, q, p);
		EXPECT_EQ(p.exp, 1);
	}

	// T alone can lead, and a negative value leads by its magnitude.
	{
		const float s[3] = {0.5f, 0.25f, 0.75f}, t[3] = {-9.0f, 0.25f, 0.125f}, q[3] = {1.0f, 1.5f, 1.25f};
		GSPerspectivePlane p;

		GSPerspectivePlaneSetup(x, y, s, t, q, p);
		EXPECT_EQ(p.exp, 3);
	}
}

// ★ A VERTEX WITH A NEGATIVE Q IS NEGATED WHOLE.
//
// The setup works on Q's magnitude and carries its sign in S and T, so (S, T, Q) at
// such a vertex becomes (-S, -T, |Q|) and S/Q is unchanged. Without it the game
// draw's 53 triangles with a vertex Q at or below zero read 12% of their pixels right
// and OutRun 2006's frame, on which nearly every draw has every Q negative, falls
// from 80% to 75% exact words; with it they read 91% and 98%.
TEST(GsSwPerspectivePlane, ANegativeQVertexIsNegatedWhole)
{
	const s32 x[3] = {0, 3200, 1600};
	const s32 y[3] = {0, 800, 4800};
	const float s[3] = {0.5f, -0.75f, 1.25f};
	const float t[3] = {-0.25f, 0.5f, 0.125f};

	const auto same = [](const GSPerspectivePlane& a, const GSPerspectivePlane& b) {
		EXPECT_EQ(a.valid, b.valid);
		EXPECT_EQ(a.exp, b.exp);
		EXPECT_EQ(a.anchor, b.anchor);
		EXPECT_EQ(a.xa, b.xa);
		EXPECT_EQ(a.ya, b.ya);

		for (int k = 0; k < 3; k++)
		{
			EXPECT_EQ(a.value[k], b.value[k]) << k;
			EXPECT_EQ(a.gx[k], b.gx[k]) << k;
			EXPECT_EQ(a.gy[k], b.gy[k]) << k;
		}
	};

	// Every Q negative: the plane is the negated triangle's.
	{
		const float q[3] = {-0.5f, -1.75f, -1.0f};
		const float ns[3] = {-s[0], -s[1], -s[2]}, nt[3] = {-t[0], -t[1], -t[2]}, nq[3] = {0.5f, 1.75f, 1.0f};
		GSPerspectivePlane a, b;

		GSPerspectivePlaneSetup(x, y, s, t, q, a);
		GSPerspectivePlaneSetup(x, y, ns, nt, nq, b);
		ASSERT_TRUE(a.valid);
		same(a, b);
	}

	// One negative: that vertex is negated and the others are not.
	{
		const float q[3] = {-0.5f, 1.75f, 1.0f};
		const float ms[3] = {-s[0], s[1], s[2]}, mt[3] = {-t[0], t[1], t[2]}, mq[3] = {0.5f, 1.75f, 1.0f};
		GSPerspectivePlane a, b;

		GSPerspectivePlaneSetup(x, y, s, t, q, a);
		GSPerspectivePlaneSetup(x, y, ms, mt, mq, b);
		ASSERT_TRUE(a.valid);
		same(a, b);
	}

	// A positive Q is untouched: the plane is not the plane of anything negated.
	{
		const float q[3] = {0.5f, 1.75f, 1.0f};
		const float ns[3] = {-s[0], -s[1], -s[2]}, nt[3] = {-t[0], -t[1], -t[2]};
		GSPerspectivePlane a, b;

		GSPerspectivePlaneSetup(x, y, s, t, q, a);
		GSPerspectivePlaneSetup(x, y, ns, nt, q, b);
		EXPECT_NE(a.gx[0], b.gx[0]);
	}
}

// A vertex value is FLOORED onto the grid, not truncated toward zero: a negative
// value one grid step below its neighbour reads there. (gs-sm3b's triangles 46, 49
// and 50.)
TEST(GsSwPerspectivePlane, AVertexIsFlooredOntoTheGrid)
{
	// E = 0, g = 2^-14. -1.5 g and +1.5 g.
	EXPECT_EQ(GSPlaneOnGrid(-1.5f * 6.103515625e-05f, 0), -2);
	EXPECT_EQ(GSPlaneOnGrid(1.5f * 6.103515625e-05f, 0), 1);
	EXPECT_EQ(GSPlaneOnGrid(-6.103515625e-05f, 0), -1);
	EXPECT_EQ(GSPlaneOnGrid(0.0f, 0), 0);
}

// What the rules cannot express comes back invalid and zero, so a caller carries a
// plane of zeros rather than garbage.
TEST(GsSwPerspectivePlane, APrimitiveWithNoExponentOrNoAreaIsInvalid)
{
	const s32 x[3] = {0, 160, 0};
	const s32 y[3] = {0, 0, 160};
	const float zero[3] = {0.0f, 0.0f, 0.0f};
	const float one[3] = {1.0f, 1.0f, 1.0f};
	GSPerspectivePlane p;

	GSPerspectivePlaneSetup(x, y, zero, zero, zero, p);
	EXPECT_FALSE(p.valid);
	EXPECT_EQ(p.gx[0], 0);
	EXPECT_EQ(p.value[2], 0);

	const float inf[3] = {std::numeric_limits<float>::infinity(), 1.0f, 1.0f};

	GSPerspectivePlaneSetup(x, y, inf, one, one, p);
	EXPECT_FALSE(p.valid);

	// Three collinear vertices have no plane.
	const s32 cx[3] = {0, 160, 320};
	const s32 cy[3] = {0, 160, 320};

	GSPerspectivePlaneSetup(cx, cy, one, one, one, p);
	EXPECT_FALSE(p.valid);
}

// The floor to g/4 is an arithmetic shift, so a negative value rounds toward minus
// infinity, and the step is sixteen gradient units.
TEST(GsSwPerspectivePlane, ThePixelFloorAndTheStep)
{
	EXPECT_EQ(GSPerspectivePlanePixel(0x0fff), 0);
	EXPECT_EQ(GSPerspectivePlanePixel(0x1000), 1);
	EXPECT_EQ(GSPerspectivePlanePixel(-1), -1);
	EXPECT_EQ(GSPerspectivePlanePixel(-0x1000), -1);
	EXPECT_EQ(GSPerspectivePlanePixel(-0x1001), -2);

	GSPerspectivePlane p = {};

	p.gx[0] = 1234;
	p.gx[1] = -77;
	EXPECT_EQ(GSPerspectivePlaneStep(p, 0), 1234 * 16);
	EXPECT_EQ(GSPerspectivePlaneStep(p, 1), -77 * 16);
}
