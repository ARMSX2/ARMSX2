// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Where a span's colour seed comes from.
//
// The GS forms a colour or fog gradient by multiplying a channel delta by a
// reciprocal truncated to eight significant bits, so the gradient it walks is
// deliberately a hair below the plane's true slope. That makes the POINT a span
// is seeded from observable: with an exact gradient every anchor gives the same
// answer, with a low one the answer depends on how far the span is from wherever
// the walk started.
//
// gs-edge (SCPH-30001, 2026-09-06, 120,080 scored readings) has now measured the
// whole interpolator, and the point is not the span's left edge: it is ONE
// anchor for the primitive, on a block grid pinned to absolute screen x, with a
// coarser ramp inside each block. GSColourWalk.h carries the model. The cases
// this file used to hold pinned the left-edge rule the console has overruled and
// are gone; the separation control below survives them, because the distance it
// measures is what makes any of it visible at all.
//
// The rasterizer is compiled per-ISA. This drives the real one, so it needs a
// build with an isa_native -- ARM64, or an x86 build with DISABLE_ADVANCE_SIMD
// off. A multi-ISA x86 build compiles the rasterizer into isa_sse4/isa_avx/
// isa_avx2 and cannot even include the header.

#include "common/Pcsx2Defs.h"
#include "GS/MultiISA.h"

#ifndef MULTI_ISA_SHARED_COMPILATION

#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
	// One run of the rasterizer: the gradients the setup formed, and the seed of
	// every row the walk emitted.
	struct SpanRecord
	{
		bool setup_ran = false;
		GSVertexSW dscan;
		std::vector<int> top;
		std::vector<int> left;
		std::vector<GSVector4> c;
		std::vector<GSVector4> t;
	};

	SpanRecord g_rec;

	void RecordSetup(const GSVertexSW*, const u16*, const GSVertexSW& dscan, GSScanlineLocalData&)
	{
		g_rec.setup_ran = true;
		g_rec.dscan = dscan;
	}

	void RecordSpan(int, int left, int top, const GSVertexSW& scan, GSScanlineLocalData&)
	{
		g_rec.top.push_back(top);
		g_rec.left.push_back(left);
		g_rec.c.push_back(scan.c);
		g_rec.t.push_back(scan.t);
	}

	const SpanRecord& Walk(GSVertexSW* v)
	{
		static const u16 index[3] = {0, 1, 2};

		g_rec = SpanRecord();

		isa_native::GSRasterizerData data;
		data.primclass = GS_TRIANGLE_CLASS;
		data.vertex = v;
		data.vertex_count = 3;
		data.index = const_cast<u16*>(index);
		data.index_count = 3;
		data.scissor = GSVector4i(0, 0, 640, 640);
		data.bbox = GSVector4i(0, 0, 640, 640);
		data.global.sel.key = 0;
		data.global.sel.iip = 1;
		data.setup_prim = &RecordSetup;
		data.draw_scanline = &RecordSpan;
		// ⚠️ nullptr, deliberately. HasEdge() is "is there an edge callback", not
		// "is AA1 on", and a triangle with one runs a SECOND Flush whose dscan is
		// zeroed, which lands on the callback after the real one.
		data.draw_edge = nullptr;

		isa_native::GSRasterizer r(nullptr, 0, 1);
		r.Draw(data);

		EXPECT_TRUE(g_rec.setup_ran) << "the setup callback never ran, so nothing was measured";
		return g_rec;
	}

	void Vertex(GSVertexSW& v, float x, float y, float red, float fog)
	{
		v = GSVertexSW::zero();
		v.p = GSVector4(x, y, 0.0f, 0.0f);
		v.p.F64[1] = 0.0;
		v.c = GSVector4(red, 0.0f, 0.0f, 0.0f);
		v.t = GSVector4(0.0f, 0.0f, 1.0f, fog);
	}

	// Colours ride the pipeline's own 1/128 grid (GSRendererSW builds them as
	// byte << 7), so these are 128 times a colour level.
	constexpr float kLevel = 128.0f;

} // namespace

// ---------------------------------------------------------------------------
// The subject: a triangle with no horizontal edge whose middle vertex is on the
// RIGHT, so its lower section's left bound is the long v0->v2 edge while the
// middle vertex sits 100 pixels away across the span.
//
//   v0 (100.5, 10)   red 0        the long left edge is vertical at x = 100.5
//   v1 (200.5, 50)   red 255      the middle vertex, on the right
//   v2 (100.5, 130)  red 0        and red is CONSTANT down the left edge
//
// Red is zero at both ends of the left edge, so the left edge's value at every
// row of the lower section is 0 exactly and the answer needs no edge model: the
// first drawn pixel of each row must read dscan.c.x times the half-pixel from
// the edge (100.5) to that pixel (101), and nothing else.
// ---------------------------------------------------------------------------

namespace
{
	void NotchedTopTriangle(GSVertexSW* v)
	{
		Vertex(v[0], 100.5f, 10.0f, 0.0f, 0.0f);
		Vertex(v[1], 200.5f, 50.0f, 255.0f * kLevel, 255.0f);
		Vertex(v[2], 100.5f, 130.0f, 0.0f, 0.0f);
	}
} // namespace

TEST(SwSpanSeed, TheAnchoredSeedIsFarEnoughAwayToSee)
{
	// The separation control. A test that cannot tell the two seeds apart proves
	// nothing by passing, so the distance between them is asserted before
	// anything is scored against it: the old seed was the middle vertex's colour
	// (255 levels) stepped back 100 pixels, and the two answers differ by the
	// gradient's shortfall times that distance.
	GSVertexSW v[3];
	NotchedTopTriangle(v);
	const SpanRecord& rec = Walk(v);

	const float exact = 255.0f * kLevel / 100.0f;
	const float anchored = 255.0f * kLevel + rec.dscan.c.x * (101.0f - 200.5f);
	const float seeded = rec.dscan.c.x * (101.0f - 100.5f);

	EXPECT_LT(rec.dscan.c.x, exact) << "the gradient must be below the exact quotient, "
	                                   "or there is nothing here to measure";
	EXPECT_GT(std::abs(anchored - seeded), 100.0f)
		<< "the two seeds are within a colour level of each other, so this "
		   "triangle cannot separate them";
}

#endif // MULTI_ISA_SHARED_COMPILATION
