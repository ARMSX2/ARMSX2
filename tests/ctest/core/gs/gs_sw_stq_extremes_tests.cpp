// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The software renderer's rule for extreme S, T and Q, against the console.
//
// gs-stq-extremes (SCPH-30001) drew single-cell primitives with extreme S, T and Q, through a
// texture whose texels name themselves. Each row in gs_sw_stq_extremes_console_rows.inc is a
// cell our renderer read wrong before this rule: what the console sampled, as (u, v) after
// the REPEAT mask. The test runs the same words through GSStqExtremes.h's arithmetic and
// expects the console's texel.

#include "common/Pcsx2Defs.h"

#include "GS/Renderers/SW/GSPerspectivePlane.h"
#include "GS/Renderers/SW/GSStqExtremes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

namespace
{
struct Row
{
	int sprite, tw, th;
	u32 s, t, q;
	int u, v;
};

const Row kRows[] = {
#include "gs_sw_stq_extremes_console_rows.inc"
};

float FloatOf(u32 w)
{
	float f;
	std::memcpy(&f, &w, sizeof(f));
	return f;
}

/// The texel a NEAREST REPEAT sample reads at 16.16 coordinate `c16`, past the 12.4 field.
int Texel(s32 c16, int size_log2)
{
	const s32 sixteenths = (c16 < 0 ? c16 + 0xFFF : c16) >> 12;
	const s32 c = std::clamp<s32>(sixteenths, GS_COORD_SIXTEENTH_MIN, GS_COORD_SIXTEENTH_MAX);

	return (c >> 4) & ((1 << size_log2) - 1);
}
} // namespace

TEST(GsSwStqExtremes, TheCaptureRowsReadTheConsolesTexel)
{
	ASSERT_GE(std::size(kRows), 100u);

	for (const Row& r : kRows)
	{
		// Two vertices with the same words, as the capture's uniform cells draw them.
		const GSStqWords w[2] = {{r.s, r.t, r.q}, {r.s, r.t, r.q}};
		GSStqCounts counts[2];
		int exp;

		GSStqPrimitiveCounts(w, 2, counts, exp);

		const s32 u = GSStqCoordinate(counts[0].s, counts[1].q, r.tw);
		const s32 v = GSStqCoordinate(counts[0].t, counts[1].q, r.th);

		EXPECT_EQ(Texel(u, r.tw), r.u) << std::hex << r.s << " " << r.t << " " << r.q;
		EXPECT_EQ(Texel(v, r.th), r.v) << std::hex << r.s << " " << r.t << " " << r.q;
	}
}

// Q's sign bit moves to S and T even at Q = -0; +0 and -0 give opposite results.
TEST(GsSwStqExtremes, ASignedZeroQSendsTheCoordinateToOppositeEnds)
{
	const GSStqWords plus[2] = {{0x3e800000u, 0, 0x00000000u}, {0x3e800000u, 0, 0x00000000u}};
	const GSStqWords minus[2] = {{0x3e800000u, 0, 0x80000000u}, {0x3e800000u, 0, 0x80000000u}};
	GSStqCounts c[2];
	int e;

	GSStqPrimitiveCounts(plus, 2, c, e);
	EXPECT_GE(GSStqCoordinate(c[0].s, c[1].q, 8) >> 12, 32768);

	GSStqPrimitiveCounts(minus, 2, c, e);
	EXPECT_LE(GSStqCoordinate(c[0].s, c[1].q, 8) >> 12, -32768);
}

// A reciprocal of a non-power-of-two Q is short of exact; a power of two is exact.
TEST(GsSwStqExtremes, TheReciprocalIsTruncatedExceptForAPowerOfTwo)
{
	// S = 2.0, Q = 1.5: Q on a grid of 2^(1 - 14) is 12288 grid steps, S 16384. The exact
	// quotient 4/3 at tw = 8 is 341.333 texels; the truncated reciprocal reads less.
	const s32 c = GSStqCoordinate(16384, 12288, 8);
	EXPECT_LT(c, static_cast<s32>(341.3333333 * 65536.0));
	EXPECT_GT(c, static_cast<s32>(341.3 * 65536.0));

	// Q = 1 (2^14 steps) is exact.
	EXPECT_EQ(GSStqCoordinate(16384, 16384, 8), 256 * 65536);
}

// Exponent 255 is ordinary: S = 2^128, Q = 1.5 * 2^128 reads 2/3 of the texture.
TEST(GsSwStqExtremes, ExponentField255IsAnOrdinaryNumber)
{
	const GSStqWords w[2] = {{0x7f800000u, 0, 0x7fc00000u}, {0x7f800000u, 0, 0x7fc00000u}};
	GSStqCounts c[2];
	int e;

	EXPECT_TRUE(GSStqPrimitiveCounts(w, 2, c, e));
	EXPECT_EQ(e, 128);
	EXPECT_EQ(Texel(GSStqCoordinate(c[0].s, c[1].q, 8), 8), 170);
}

// The plane setup takes the same words and keeps the same reading.
TEST(GsSwStqExtremes, ThePlaneSetupTreatsExponent255AsOrdinary)
{
	const s32 x[3] = {0, 160, 0};
	const s32 y[3] = {0, 0, 160};
	const float s[3] = {FloatOf(0x7f800000u), FloatOf(0x7f800000u), FloatOf(0x7f800000u)};
	const float t[3] = {0.0f, 0.0f, 0.0f};
	const float q[3] = {FloatOf(0x7fc00000u), FloatOf(0x7fc00000u), FloatOf(0x7fc00000u)};
	GSPerspectivePlane p;

	GSPerspectivePlaneSetup(x, y, s, t, q, p);

	ASSERT_TRUE(p.valid);
	EXPECT_EQ(p.exp, 128);
	EXPECT_EQ(p.value[0], 16384); // S = 2^128 on a grid of 2^114
	EXPECT_EQ(p.value[2], 24576); // Q = 1.5 * 2^128
	EXPECT_EQ(p.value[1], 0);
}

// A large coordinate erases a small one on the shared grid.
TEST(GsSwStqExtremes, TheSharedGridErasesASmallValue)
{
	// S = 1e6 sets E = 19; T = 0.25 falls below the grid 2^5 and floors to 0.
	const GSStqWords w[2] = {{0x49742400u, 0x3e800000u, 0x3f800000u}, {0x49742400u, 0x3e800000u, 0x3f800000u}};
	GSStqCounts c[2];
	int e;

	GSStqPrimitiveCounts(w, 2, c, e);
	EXPECT_EQ(e, 19);
	EXPECT_EQ(c[0].t, 0);
}

// A draw with ordinary coordinates must not take the rule: it costs nothing and moves nothing.
TEST(GsSwStqExtremes, AnOrdinaryPrimitiveDoesNotNeedTheRule)
{
	const GSStqWords w[3] = {{0x3f000000u, 0x3f000000u, 0x3f800000u}, {0x3f400000u, 0x3e800000u, 0x3f800000u}, {0x3e800000u, 0x3f400000u, 0x3f800000u}};

	EXPECT_EQ(GSStqClassifyPrimitive(w, 3, false, false, 8, 8), 0u);
	EXPECT_EQ(GSStqClassifyPrimitive(w, 2, true, false, 8, 8), 0u);

	// A sprite whose Q is not a power of two needs its reciprocal.
	const GSStqWords q[2] = {{0x3f000000u, 0x3f000000u, 0x3fc00000u}, {0x3f400000u, 0x3e800000u, 0x3fc00000u}};
	EXPECT_NE(GSStqClassifyPrimitive(q, 2, true, false, 8, 8) & GS_STQ_NEED_PRIMITIVE_RULE, 0u);

	// A zero Q and an exponent-255 word do.
	const GSStqWords z[2] = {{0x3f000000u, 0, 0}, {0x3f400000u, 0, 0}};
	EXPECT_NE(GSStqClassifyPrimitive(z, 2, true, true, 8, 8) & GS_STQ_NEED_FIELD_EDGE, 0u);
	const GSStqWords e[2] = {{0x7f800000u, 0, 0x3f800000u}, {0x3f400000u, 0, 0x3f800000u}};
	EXPECT_NE(GSStqClassifyPrimitive(e, 2, false, true, 8, 8) & GS_STQ_NEED_RAW_WORDS, 0u);
}
