// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Which reciprocal the triangle setup truncates, and which attributes take it.
//
// The GS does not divide to form an attribute gradient: it multiplies by a
// reciprocal read out of a table eight significant bits wide. That much was
// measured on an SCPH-30001 over twenty-four baselines, but on one triangle
// SHAPE -- flat top, vertical left side, and a height of 1024, a power of two.
// Under that shape the reciprocal of the horizontal baseline and the reciprocal
// of the setup's cross product have the identical mantissa, so the capture could
// not say which quantity the hardware inverts, and neither could anybody reading
// it.
//
// A second capture swept the height alone. The exact gradient does not contain
// the height at all, so the reading needs no model: if the denominator were the
// baseline every height would draw the same row, and if it is the cross product
// the rows move. On the console they move -- up to 340 of 442 readings between
// two heights -- while two heights a power of two apart stay byte-identical, as
// every candidate requires. Across four shape classes the cross product's
// reciprocal explains 93.7% to 96.3% where the exact quotient explains 56% to
// 81%, and the class whose cross product is neither edge's length (a slanted
// left side, W*H - K*P) prefers it just as strongly.
//
// It does NOT apply to everything the setup forms. On the same sweep, at heights
// whose truncated reciprocals would move a sampled texture coordinate by three
// sixteenths of a texel and a depth value by more than a whole pixel step, the
// console's texture coordinates and depth values do not move at all -- and our
// arm, which divides exactly, is 100.00% against silicon on both. Colour and fog
// take the truncated reciprocal; s, t, q and depth take the exact quotient. The
// tests below hold both halves, because a fix that spreads is as wrong as one
// that does not land.
//
// Capture: hardware-oracle/captures/gs-shape, SCPH-30001, GS revision 0x15,
// two byte-identical console runs.
//
// The rule lives in GSRasterizer::DrawTriangle, which has an ARM64/SSE4 body and an
// AVX2 twin, so this runs on both hosts: on x86 it is the AVX2 twin that is on trial.
// The gate is multi-ISA, not architecture -- a multi-ISA x86 build compiles the
// rasterizer into isa_sse4/isa_avx/isa_avx2 and has no isa_native at all, so the
// header cannot even be included there. That is every x86 CI configuration today, so
// on x86 this suite only runs in a local build with DISABLE_ADVANCE_SIMD=OFF.

#include "common/Pcsx2Defs.h"
#include "GS/MultiISA.h"

#ifndef MULTI_ISA_SHARED_COMPILATION

#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"

#include <gtest/gtest.h>

namespace
{
GSVertexSW g_dscan;
bool g_setup_ran = false;

void RecordSetup(const GSVertexSW*, const u16*, const GSVertexSW& dscan, GSScanlineLocalData&)
{
	g_dscan = dscan;
	g_setup_ran = true;
}

void NoSpan(int, int, int, const GSVertexSW&, GSScanlineLocalData&) {}

// gs-shape's own geometry: a flat top from (8.5, 7) to (232.5, 7) -- a baseline of
// 224 pixels -- and a vertical left side running `height` rows down. The colour
// rises from black to (255, 191, 127, 63) along the top edge and is flat down the
// left one, which is what makes the walk's seed the first vertex's own value
// exactly, with no edge interpolation between the seed and the measurement.
//
// Colours are in the pipeline's own 1/128 grid (GSRendererSW builds them as
// byte << 7), so a gradient printed here is 128 times the per-pixel colour step.
GSVertexSW SetupOfTriangle(float height)
{
	GSVertexSW vertex[3];
	for (int i = 0; i < 3; i++)
	{
		vertex[i] = GSVertexSW::zero();
		vertex[i].p.F64[1] = 0.0;
	}

	vertex[0].p = GSVector4(8.5f, 7.0f, 0.0f, 0.0f);
	vertex[1].p = GSVector4(232.5f, 7.0f, 0.0f, 0.0f);
	vertex[2].p = GSVector4(8.5f, 7.0f + height, 0.0f, 0.0f);

	vertex[0].c = GSVector4(0.0f, 0.0f, 0.0f, 0.0f);
	vertex[1].c = GSVector4(255.0f * 128.0f, 191.0f * 128.0f, 127.0f * 128.0f, 63.0f * 128.0f);
	vertex[2].c = GSVector4(0.0f, 0.0f, 0.0f, 0.0f);

	// t is (s, t, q, f) on a triangle. The coordinate runs 48 texels -- 768
	// sixteenths -- across the baseline; the fog coefficient runs 0 to 255.
	vertex[0].t = GSVector4(0.0f, 136.0f, 1.0f, 0.0f);
	vertex[1].t = GSVector4(768.0f, 136.0f, 1.0f, 255.0f);
	vertex[2].t = GSVector4(0.0f, 136.0f, 1.0f, 0.0f);

	// Depth rides the double lane, formed from the plane rather than from the
	// float32 barycentric coefficients the other attributes use.
	vertex[0].p.F64[1] = 1048576.0;
	vertex[1].p.F64[1] = 1048576.0 + 12582912.0;
	vertex[2].p.F64[1] = 1048576.0;

	static const u16 index[3] = {0, 1, 2};

	g_dscan = GSVertexSW::zero();
	g_setup_ran = false;

	isa_native::GSRasterizerData data;
	data.primclass = GS_TRIANGLE_CLASS;
	data.vertex = vertex;
	data.vertex_count = 3;
	data.index = const_cast<u16*>(index);
	data.index_count = 3;
	data.scissor = GSVector4i(0, 0, 640, 640);
	data.bbox = GSVector4i(0, 0, 640, 640);
	data.global.sel.key = 0;
	data.global.sel.iip = 1;
	data.setup_prim = &RecordSetup;
	data.draw_scanline = &NoSpan;
	// ⚠️ nullptr, deliberately. HasEdge() is "is there an edge callback", not "is
	// AA1 on", and a triangle with one runs a SECOND Flush carrying a zeroed
	// dscan -- which lands on the callback after the real one and reads as a
	// renderer that computes no gradients at all.
	data.draw_edge = nullptr;

	isa_native::GSRasterizer r(nullptr, 0, 1);
	r.Draw(data);

	EXPECT_TRUE(g_setup_ran) << "the setup callback never ran, so nothing was measured";
	return g_dscan;
}

// The exact quotient for this plane: 255 * 128 / 224, whatever the height.
constexpr float kExactRedGradient = 255.0f * 128.0f / 224.0f; // 145.714286

// What the console's OWN pixels admit. Sweeping the gradient and walking the
// block DDA against the row silicon stored at height 1024, the readings agree
// best (216 of 221) over exactly this interval -- and the exact quotient above
// sits outside it, which is the whole finding in one number.
constexpr float kConsoleBracketLo = 145.375f;
constexpr float kConsoleBracketHi = 145.500f;
} // namespace

TEST(SwSetupReciprocal, TheColourGradientMovesWithTheTriangleHeight)
{
	// The console's twin: one plane, one baseline, one left edge, two heights.
	// The exact gradient does not contain the height, so a renderer that divides
	// produces the identical number twice. Silicon does not.
	const GSVertexSW a = SetupOfTriangle(1024.0f);
	const GSVertexSW b = SetupOfTriangle(2287.0f);

	EXPECT_NE(a.c.x, b.c.x) << "the colour gradient is the same at both heights, so "
	                           "the setup is still dividing by the cross product "
	                           "rather than multiplying by its truncated reciprocal";
}

TEST(SwSetupReciprocal, TheConsoleExcludesTheExactQuotient)
{
	const GSVertexSW a = SetupOfTriangle(1024.0f);

	EXPECT_GE(a.c.x, kConsoleBracketLo);
	EXPECT_LE(a.c.x, kConsoleBracketHi);
	// Stated separately so a failure says which half is wrong: the value we ship
	// today is outside the bracket the console's own row admits.
	EXPECT_GT(kExactRedGradient, kConsoleBracketHi);
}

TEST(SwSetupReciprocal, TwoHeightsAPowerOfTwoApartGiveOneGradient)
{
	// The control, and it is the one that must be green both before and after: two
	// cross products differing by a factor of two share a mantissa, so no
	// candidate -- including the one being landed -- may separate them. The
	// console reads them byte-identical, 0 of 442.
	const GSVertexSW a = SetupOfTriangle(1024.0f);
	const GSVertexSW b = SetupOfTriangle(2048.0f);

	EXPECT_EQ(a.c.x, b.c.x);
	EXPECT_EQ(a.c.y, b.c.y);
	EXPECT_EQ(a.c.z, b.c.z);
	EXPECT_EQ(a.c.w, b.c.w);
}

TEST(SwSetupReciprocal, EveryColourChannelTakesIt)
{
	// The shortfall scales with the channel delta, so a rule that is additive
	// rather than proportional would show here. Deltas 255, 191, 127, 63.
	const GSVertexSW a = SetupOfTriangle(1024.0f);
	const GSVertexSW b = SetupOfTriangle(2287.0f);

	EXPECT_NE(a.c.y, b.c.y);
	EXPECT_NE(a.c.z, b.c.z);
	EXPECT_NE(a.c.w, b.c.w);
}

TEST(SwSetupReciprocal, FogTakesItToo)
{
	// Console: the fog rows move with the height at exactly the heights where the
	// colour rows move, and are identical at exactly the heights where the colour
	// rows are identical.
	const GSVertexSW a = SetupOfTriangle(1024.0f);
	const GSVertexSW b = SetupOfTriangle(2287.0f);

	EXPECT_NE(a.t.w, b.t.w);
}

TEST(SwSetupReciprocal, TheTextureCoordinateDoesNotTakeIt)
{
	// The guard on the other side. Under the truncated reciprocal these two
	// heights would put the sampled coordinate in a different sixteenth of a texel
	// on 206 of 221 pixels, so the readout could plainly have seen it; the console
	// moved 0 of 442, and our exact quotient scores 100.00% against silicon on
	// that section. If this test ever fails the fix has spread past its evidence.
	const GSVertexSW a = SetupOfTriangle(1024.0f);
	const GSVertexSW b = SetupOfTriangle(2287.0f);

	EXPECT_EQ(a.t.x, b.t.x);
	EXPECT_EQ(a.t.y, b.t.y);
	EXPECT_EQ(a.t.z, b.t.z);
	EXPECT_FLOAT_EQ(a.t.x, 768.0f / 224.0f);
}

TEST(SwSetupReciprocal, DepthDoesNotTakeIt)
{
	// Same guard, and the stronger one: at these heights the two models differ by
	// 94,607 depth units at the far end of the span against a per-pixel step of
	// 56,173 -- more than a whole pixel. The console moved 0 of 442.
	const GSVertexSW a = SetupOfTriangle(1024.0f);
	const GSVertexSW b = SetupOfTriangle(2287.0f);

	EXPECT_EQ(a.p.F64[1], b.p.F64[1]);
}

#endif // MULTI_ISA_SHARED_COMPILATION
