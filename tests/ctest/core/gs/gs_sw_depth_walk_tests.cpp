// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for the SW rasterizer's depth walk.
//
// The gs-zgrad probe, run on an SCPH-30001 against the software arm, measured
// that an interpolated depth never reaches its plane: on 13.64% of gradient
// pixels our stored depth was one unit away from silicon's, and the differing
// pixels were exactly the ones whose exact depth lands on an integer. Applying
// the shortfall took that to 3.80%, and the same change took the gs-interp
// capture's depth sections from 2450 differing readings to 1898.
//
// What is pinned here is the SHAPE of the shortfall, because it is asymmetric
// between the axes in a way that looks like a bug and is not. Each half was
// established by rebuilding and re-running the probe against the console, and
// the alternative is recorded with the number it scored:
//
//   * a flat triangle takes NO bias -- silicon is exact on all 896 flat
//     readings, so this belongs to the walk and not to the seed;
//   * the X half does not follow the gradient's sign (signing it: gs-zgrad's
//     gridx section 192 wrong pixels -> 384);
//   * the Y half does, and only once the walk has stepped off the vertex
//     scanline (not gating and not signing it: ygrad 0 -> 448 wrong pixels).
//
// One unit of depth is the entire margin a depth test between consecutive Z
// levels decides on, so none of this is cosmetic: it is what a Z-laddered scene
// resolves on. The capture, not this file, is the gate -- these are pins that
// make a silent "simplification" of the rule fail loudly.

#include "GS/Renderers/SW/GSDepthWalk.h"

#include <gtest/gtest.h>

namespace
{
constexpr double kStep = 1.0 / 1024.0; // the grid the depth step is truncated onto
}

TEST(GSDepthWalk, FlatPrimitiveTakesNoBias)
{
	// Silicon stores a flat triangle's depth exactly: 896 of 896 readings.
	EXPECT_EQ(GSDepthWalkBias(0.0, 0.0, false), 0.0);
	EXPECT_EQ(GSDepthWalkBias(0.0, 0.0, true), 0.0);
}

TEST(GSDepthWalk, XGradientBiasesFromTheFirstScanline)
{
	// The X shortfall is there at the vertex itself, so it does not wait for dy.
	EXPECT_EQ(GSDepthWalkBias(0.25, 0.0, false), kGSDepthWalkBias);
	EXPECT_EQ(GSDepthWalkBias(0.25, 0.0, true), kGSDepthWalkBias);
}

TEST(GSDepthWalk, XGradientBiasDoesNotFollowItsSign)
{
	// A falling X gradient runs short in the same direction as a rising one.
	// Signing this half doubled gs-zgrad's gridx miss count.
	EXPECT_EQ(GSDepthWalkBias(-0.5, 0.0, false), kGSDepthWalkBias);
	EXPECT_EQ(GSDepthWalkBias(-8192.0, 0.0, true), kGSDepthWalkBias);
}

TEST(GSDepthWalk, YGradientIsExactOnTheVertexScanline)
{
	// The Y shortfall accumulates, so the seed row carries none of it. This half
	// has a capture behind it: dropping the exemption lights three of gs-zgrad's
	// ygrad cases, one top row each.
	//
	// The caller passes "stepped off the PRIMITIVE's first scanline", not the
	// section's: a triangle with no flat edge is walked as two sections rebased
	// on the middle vertex, and exempting the second one's first row would lay a
	// one-unit depth discontinuity along that row. Silicon has no sections. That
	// distinction is reasoning, not measurement -- every gs-zgrad subject is
	// flat-topped, so no probe we own separates the two. Do not "simplify" it
	// back to the section on the strength of a green capture.
	EXPECT_EQ(GSDepthWalkBias(0.0, 0.5, false), 0.0);
	EXPECT_EQ(GSDepthWalkBias(0.0, -0.5, false), 0.0);
}

TEST(GSDepthWalk, YGradientBiasPointsBackTowardTheSeed)
{
	// Rising runs low, falling runs HIGH -- the shortfall is in the walk's
	// own direction, which is what took the ygrad section to zero.
	EXPECT_EQ(GSDepthWalkBias(0.0, 0.5, true), kGSDepthWalkBias);
	EXPECT_EQ(GSDepthWalkBias(0.0, -0.5, true), -kGSDepthWalkBias);
}

TEST(GSDepthWalk, AxesCompose)
{
	EXPECT_EQ(GSDepthWalkBias(0.25, 0.5, true), 2.0 * kGSDepthWalkBias);
	// A falling Y under a rising X cancels rather than compounding.
	EXPECT_EQ(GSDepthWalkBias(0.25, -0.5, true), 0.0);
}

TEST(GSDepthWalk, BiasMovesIntegerLandingsAndNothingElse)
{
	// The property the magnitude exists for: a value that lands exactly on an
	// integer drops below it, and a value even one step of the 2^-10 grid above
	// that integer does not. This is what makes the bias safe to apply to the
	// seed instead of testing every pixel at the store.
	const double bias = GSDepthWalkBias(0.25, 0.0, false);
	for (const double z : {1.0, 2.0, 1024.0, 8388608.0})
	{
		EXPECT_LT(z - bias, z) << "an exact landing must fall below the integer";
		EXPECT_GE(z - bias, z - 1.0) << "and must never fall a whole unit";
		EXPECT_GT((z + kStep) - bias, z) << "one step above must stay put";
	}
}

// ---- the gradient the walk steps by must come from the PLANE, not the triangle ------
//
// gs-block (SCPH-30001, 2026-08-15) swept one fixed depth plane across eight left
// edges and read the same absolute pixels back. Silicon: every integer landing reads
// one below the plane on all eight -- span-invariant. Our software arm: k=0 and k=15
// read the integer ON the plane, the other six read one below -- 46 of 136 readings
// separated by where the span began.
//
// The mechanism is not the walk. SetupTriangle formed the depth gradient as
// (double delta-z) x (float32 barycentric coefficient), so the same plane carried by
// triangles of different width came out with a float32 relative error of either sign,
// and the truncation onto the 2^-10 grid then landed a step below the true gradient
// (deficit: integer landings fall to N-1, matching silicon) or a step ABOVE it
// (surplus: the walk overtakes the plane and integer landings read N). Which one a
// triangle got depended on how 1/dx rounded in float32 -- 90 and 75 rounded up, 87,
// 84, 81, 78, 72 and 69 rounded down.
//
// The pin: the truncated gradient of one plane is one number, whatever triangle
// carries it, and it is the truncation of the EXACT gradient. That is what makes the
// walk a function of the plane and the pixel, which is what a fragment shader (and
// silicon) can compute.

// The rasterizer is compiled per-ISA. This section drives the real one, so it needs a
// build that has an isa_native -- ARM64, or an x86 build with DISABLE_ADVANCE_SIMD off.
// A multi-ISA x86 build compiles the rasterizer into isa_sse4/isa_avx/isa_avx2 and
// cannot even include the header.
#ifndef MULTI_ISA_SHARED_COMPILATION

#include "GS/MultiISA.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <cmath>
#include <utility>
#include <vector>

namespace
{
	// What one run of the rasterizer says about the plane it walked: the per-pixel
	// step the setup formed, and the z seed of every row the walk emitted.
	struct WalkRecord
	{
		bool setup_ran = false;
		double dscan_z = 0.0;
		std::vector<std::pair<int, double>> rows; // (top, z at the span's first pixel)
	};

	WalkRecord g_rec;

	void RecordSetup(const GSVertexSW*, const u16*, const GSVertexSW& dscan, GSScanlineLocalData&)
	{
		g_rec.setup_ran = true;
		g_rec.dscan_z = dscan.p.F64[1];
	}

	void RecordSpan(int, int, int top, const GSVertexSW& scan, GSScanlineLocalData&)
	{
		g_rec.rows.emplace_back(top, scan.p.F64[1]);
	}

	// A flat-topped right triangle: A=(x0,y) B=(x1,y) C=(x0,y+h) in pixels, with z as
	// the caller says. The gs-block anchor shape.
	void MakeAnchorTriangle(GSVertexSW* v, float x0, float x1, float y, float h, double za, double zb)
	{
		for (int i = 0; i < 3; i++)
		{
			v[i] = GSVertexSW::zero();
			v[i].p.F64[1] = 0.0;
		}
		v[0].p.x = x0;
		v[0].p.y = y;
		v[0].p.F64[1] = za;
		v[1].p.x = x1;
		v[1].p.y = y;
		v[1].p.F64[1] = zb;
		v[2].p.x = x0;
		v[2].p.y = y + h;
		v[2].p.F64[1] = za;
	}

	// Draw the triangle through the real rasterizer, with the two callbacks standing in
	// for the generated scanline. Nothing here compiles a scanline: what is under test
	// is the setup's own arithmetic and the walk's own geometry.
	const WalkRecord& WalkTriangle(GSVertexSW* v)
	{
		static const u16 index[3] = {0, 1, 2};

		g_rec = WalkRecord();

		isa_native::GSRasterizerData data;
		data.primclass = GS_TRIANGLE_CLASS;
		data.vertex = v;
		data.vertex_count = 3;
		data.index = const_cast<u16*>(index);
		data.index_count = 3;
		data.scissor = GSVector4i(0, 0, 640, 448);
		data.bbox = GSVector4i(0, 0, 640, 448);
		data.global.sel.key = 0;
		data.global.sel.zb = 1;
		data.setup_prim = &RecordSetup;
		data.draw_scanline = &RecordSpan;
		// ⚠️ nullptr, deliberately. HasEdge() is "is there an edge callback", not "is
		// AA1 on", and a triangle with one runs a SECOND Flush carrying a zeroed dscan.
		data.draw_edge = nullptr;

		isa_native::GSRasterizer r(nullptr, 0, 1);
		r.Draw(data);

		EXPECT_TRUE(g_rec.setup_ran) << "the setup callback never ran, so nothing was measured";
		return g_rec;
	}

	double TruncStep(double g) { return std::trunc(g * 1024.0) / 1024.0; }
} // namespace

TEST(GSDepthWalk, GradientIsThePlanesNotTheTriangles)
{
	// The gs-block anchor sweep, set 0: plane z = (x - 8.5) * 100000/3, left edge at
	// 8.5 + k for k in {0,3,...,21}, right vertex fixed at 98.5. Same plane, eight
	// triangles, one truncated gradient.
	const double exact = 100000.0 / 3.0;
	const double want = TruncStep(exact);
	for (int k = 0; k <= 21; k += 3)
	{
		GSVertexSW v[3];
		MakeAnchorTriangle(v, 8.5f + k, 98.5f, 16.0f, 30.0f, k * 100000.0 / 3.0, 90 * 100000.0 / 3.0);
		EXPECT_EQ(WalkTriangle(v).dscan_z, want)
			<< "k=" << k << ": the walk's step must be the truncation of the exact "
			   "gradient; a step above it lets the walk overtake the plane and integer "
			   "landings read N instead of N-1";
	}
}

TEST(GSDepthWalk, GradientTruncatesTheExactSlopeAtEveryWidth)
{
	// Sweep the triangle width so 1/dx rounds both ways in float32; the truncated
	// gradient must not follow the rounding.
	int wrong = 0;
	for (int dx = 8; dx <= 200; dx++)
	{
		const double slope = 100000.0 / 3.0;
		GSVertexSW v[3];
		MakeAnchorTriangle(v, 8.5f, 8.5f + dx, 16.0f, 30.0f, 0.0, dx * slope);
		if (WalkTriangle(v).dscan_z != TruncStep(slope))
			wrong++;
	}
	EXPECT_EQ(wrong, 0) << "widths whose float32 reciprocal rounded the gradient onto the wrong 2^-10 step";
}

TEST(GSDepthWalk, EdgeGradientTruncatesTheExactSlopeAtEveryHeight)
{
	// The same for the row step along the edge, read off the walk itself: a plane with
	// a pure Y gradient, carried by triangles of different height. The left edge is
	// vertical and the pixel step is zero, so the difference between two consecutive
	// rows' seeds IS the edge step -- and the seed bias is a constant off the first
	// row, so it cancels in the difference.
	int wrong = 0;
	for (int h = 4; h <= 120; h++)
	{
		const double slope = 50000.0 / 3.0; // per row
		GSVertexSW v[3];
		// A=(8.5,16) z=0, B=(98.5,16) z=0, C=(8.5,16+h) z=h*slope -> dz/dy = slope, dz/dx = 0
		MakeAnchorTriangle(v, 8.5f, 98.5f, 16.0f, static_cast<float>(h), 0.0, 0.0);
		v[2].p.F64[1] = h * slope;

		const WalkRecord& rec = WalkTriangle(v);
		ASSERT_GE(rec.rows.size(), 3u) << "h=" << h;

		// Rows 1 and 2, so both carry the same bias and it subtracts out.
		const double step = rec.rows[2].second - rec.rows[1].second;
		if (std::abs(step - slope) > slope * 1e-12)
			wrong++;
		if (rec.dscan_z != 0.0)
			wrong++;
	}
	EXPECT_EQ(wrong, 0) << "heights whose float32 reciprocal moved the row step or leaked into the pixel step";
}

#endif // MULTI_ISA_SHARED_COMPILATION
