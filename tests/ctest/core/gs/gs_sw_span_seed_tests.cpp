// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The colour interpolator: what the setup decides, and what a span carries.
//
// The GS forms a colour or fog gradient by multiplying a channel delta by a
// reciprocal truncated to eight significant bits, and then truncates the product
// again -- toward zero, to a tenth of a fractional bit of a colour level. It
// walks that gradient from ONE anchor per primitive, on a block grid pinned to
// absolute screen x, and it ramps inside a block on a gradient coarser still.
//
// gs-edge (SCPH-30001, 2026-09-06, two byte-identical launches, 120,080 scored
// readings over a slope sweep, a height and baseline sweep and a second-row
// sweep) measures all of it; gs-shape (2026-09-05) and gs-walk2 hold the same
// model. GSColourWalk.h carries the model itself and what each section of the
// capture decided.
//
// Every expectation below is computed in the test, from the geometry, with its
// own truncated reciprocal and its own truncations -- never by reading the
// renderer's own numbers back.
//
// The rasterizer is compiled per-ISA. This drives the real one, so it needs a
// build with an isa_native -- ARM64, or an x86 build with DISABLE_ADVANCE_SIMD
// off. A multi-ISA x86 build compiles the rasterizer into isa_sse4/isa_avx/
// isa_avx2 and cannot even include the header.

#include "common/Pcsx2Defs.h"
#include "GS/MultiISA.h"

#ifndef MULTI_ISA_SHARED_COMPILATION

#include "GS/Renderers/SW/GSColourWalk.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
	// One run of the rasterizer: what the setup decided, the tables the scanline
	// was handed, and the seed of every row the walk emitted.
	struct SpanRecord
	{
		bool setup_ran = false;
		bool tables_seen = false;
		int tables_top = 0;
		GSVertexSW dscan;
		GSColourWalk cwalk{};
		GSScanlineLocalData local{};
		std::vector<int> top;
		std::vector<int> left;
		std::vector<GSVector4> c;
		std::vector<GSVector4> t;
	};

	SpanRecord g_rec;

	void RecordSetup(const GSVertexSW*, const u16*, const GSVertexSW& dscan, GSScanlineLocalData& local)
	{
		g_rec.setup_ran = true;
		g_rec.dscan = dscan;
		g_rec.cwalk = local.cwalk;
	}

	void RecordSpan(int, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local)
	{
		// The tables are built between the setup callback and the first span, so
		// this is the first place they can be read.
		if (!g_rec.tables_seen)
		{
			g_rec.local = local;
			g_rec.tables_top = top;
			g_rec.tables_seen = true;
		}

		g_rec.top.push_back(top);
		g_rec.left.push_back(left);
		g_rec.c.push_back(scan.c);
		g_rec.t.push_back(scan.t);
	}

	// ⚠️ TFX_NONE is 4, not 0, so a zeroed selector is a MODULATE-textured draw --
	// which walks a FOUR-pixel block. Every case that scores the eight-wide walk
	// has to ask for TFX_NONE by name, and leave fog and AA1 off with it.
	const SpanRecord& Walk(GSVertexSW* v, GS_PRIM_CLASS pc = GS_TRIANGLE_CLASS, int count = 3,
		int tfx = TFX_NONE, int fge = 0, int aa1 = 0)
	{
		static const u16 index[3] = {0, 1, 2};

		g_rec = SpanRecord();

		isa_native::GSRasterizerData data;
		data.primclass = pc;
		data.vertex = v;
		data.vertex_count = count;
		data.index = const_cast<u16*>(index);
		data.index_count = count;
		data.scissor = GSVector4i(0, 0, 640, 640);
		data.bbox = GSVector4i(0, 0, 640, 640);
		data.global.sel.key = 0;
		data.global.sel.iip = 1;
		data.global.sel.tfx = tfx;
		data.global.sel.fge = fge;
		data.global.sel.aa1 = aa1;
		data.setup_prim = &RecordSetup;
		data.draw_scanline = &RecordSpan;
		// nullptr, deliberately. HasEdge() is "is there an edge callback", not
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
	// byte << 7), so these are 128 times a colour level. One such unit is the
	// unit every number below is in.
	constexpr float kLevel = 128.0f;

	// The eight-bit truncated reciprocal, as SetupTriangle forms it. Written out
	// here rather than shared, so that a change to the renderer's copy has to be
	// made deliberately in both places.
	float TruncatedReciprocal8(double v)
	{
		double r = 1.0 / v;
		u64 bits;

		std::memcpy(&bits, &r, sizeof(bits));
		bits &= ~((static_cast<u64>(1) << 45) - 1);
		std::memcpy(&r, &bits, sizeof(r));
		return static_cast<float>(r);
	}

	/// Toward zero, to an eighth of a colour unit -- the gradient's own grid.
	float Tz8(float v) { return std::trunc(v * 8.0f) * 0.125f; }

	/// Toward zero, to eight colour units -- the grid the ramp inside a block runs on.
	float TzBlock(float v) { return std::trunc(v * 0.125f) * 8.0f; }

	// The setup's plane arithmetic for one triangle, per channel, written out here.
	struct Plane
	{
		float raw[4];  // dscan.c before the ten-bit truncation
		float g[4];
		float gy[4];
		float gc[4];
		float gyc[4];
		float d8[4];
		float g8[4];
		float fraw, fg, fgy, fgc, fgyc, fd8, fg8;
	};

	Plane PlaneOf(const GSVertexSW* v)
	{
		const float d0x = v[1].p.x - v[0].p.x, d0y = v[1].p.y - v[0].p.y;
		const float d1x = v[2].p.x - v[0].p.x, d1y = v[2].p.y - v[0].p.y;
		const float cross = d0y * d1x - d0x * d1y;
		const float r = TruncatedReciprocal8(static_cast<double>(cross));

		Plane p = {};

		for (int ch = 0; ch < 4; ch++)
		{
			const float da0 = v[1].c.F32[ch] - v[0].c.F32[ch];
			const float da1 = v[2].c.F32[ch] - v[0].c.F32[ch];

			p.raw[ch] = da1 * (d0y * r) - da0 * (d1y * r);
			p.g[ch] = Tz8(p.raw[ch]);
			p.gy[ch] = Tz8(da0 * (d1x * r) - da1 * (d0x * r));
			p.gc[ch] = TzBlock(p.g[ch]);
			p.gyc[ch] = TzBlock(p.gy[ch]);
			p.d8[ch] = (p.g[ch] - p.gc[ch]) * 8.0f;
			p.g8[ch] = p.g[ch] * 8.0f;
		}

		const float df0 = v[1].t.w - v[0].t.w;
		const float df1 = v[2].t.w - v[0].t.w;

		p.fraw = df1 * (d0y * r) - df0 * (d1y * r);
		p.fg = Tz8(p.fraw);
		p.fgy = Tz8(df0 * (d1x * r) - df1 * (d0x * r));
		p.fgc = TzBlock(p.fg);
		p.fgyc = TzBlock(p.fgy);
		p.fd8 = (p.fg - p.fgc) * 8.0f;
		p.fg8 = p.fg * 8.0f;

		return p;
	}

	// One geometry's hand-computed walk, from GSColourWalk.h's rules 3 and 4.
	struct Anchor
	{
		int d;
		int S;
		int A;
		int phase;
		bool top_anchor;
		float xr;
		float yr;
		float ar;   // the anchor's red
		float fr;   // the anchor's fog
	};

	/// Rule 7, evaluated in the test. `bw` is the block width: eight for an
	/// untextured draw, four for a textured one.
	float Seed(const Plane& p, int ch, const Anchor& a, int x, int y, int bw = 8)
	{
		const int yf = a.top_anchor ? (y & ~1) : (y | 1);
		const int j = (a.d * (x - a.S)) >> ((bw == 8) ? 3 : 2);
		const float dw = (p.g[ch] - p.gc[ch]) * static_cast<float>(bw);
		const float P = a.ar + Tz8(p.g[ch] * (static_cast<float>(a.A) - a.xr))
		                + Tz8(p.gy[ch] * (static_cast<float>(yf) - a.yr));

		return std::floor(P + p.gyc[ch] * static_cast<float>(y - yf)
		                  + p.gc[ch] * static_cast<float>(x - a.A)
		                  + dw * static_cast<float>(a.d * j));
	}

	float SeedFog(const Plane& p, const Anchor& a, int x, int y, int bw = 8)
	{
		const int yf = a.top_anchor ? (y & ~1) : (y | 1);
		const int j = (a.d * (x - a.S)) >> ((bw == 8) ? 3 : 2);
		const float dw = (p.fg - p.fgc) * static_cast<float>(bw);
		const float P = a.fr + Tz8(p.fg * (static_cast<float>(a.A) - a.xr))
		                + Tz8(p.fgy * (static_cast<float>(yf) - a.yr));

		return std::floor(P + p.fgyc * static_cast<float>(y - yf)
		                  + p.fgc * static_cast<float>(x - a.A)
		                  + dw * static_cast<float>(a.d * j));
	}

	// The row of `rec` at scanline y, or -1.
	int RowAt(const SpanRecord& rec, int y)
	{
		for (size_t i = 0; i < rec.top.size(); i++)
		{
			if (rec.top[i] == y)
				return static_cast<int>(i);
		}
		return -1;
	}

	// How many pixels one packed table entry covers -- the scanline's vector width.
	constexpr int kLanes = static_cast<int>(sizeof(GSScanlineLocalData::skip::rb) / sizeof(s16)) / 2;

	template <typename T>
	int Lane16(const T& v, int i)
	{
		return static_cast<int>(v.I16[i]);
	}
} // namespace

// ---------------------------------------------------------------------------
// The four subjects. Each one's walk is worked out by hand in the comment and
// asserted in TheAnchorAndGridComeFromTheSpine below.
// ---------------------------------------------------------------------------

namespace
{
	// Middle vertex on the right, spine leaning right as it descends, so the
	// anchor is the spine's TOP end.
	//   spine v0->v2 at y = 50 is x = 113.83; the middle vertex is at 200.5.
	//   d = +1, R = v0, S = ceil(100.5) & ~1 = 100, A = 102, phase = 4.
	void MidRightTriangle(GSVertexSW* v)
	{
		Vertex(v[0], 100.5f, 10.0f, 0.0f, 0.0f);
		Vertex(v[1], 200.5f, 50.0f, 255.0f * kLevel, 255.0f * kLevel);
		Vertex(v[2], 140.5f, 130.0f, 120.0f * kLevel, 60.0f * kLevel);
	}

	constexpr Anchor kMidRight = {1, 100, 102, 4, true, 100.5f, 10.0f, 0.0f, 0.0f};

	// Middle vertex on the right, spine leaning LEFT as it descends, so the
	// anchor is the spine's BOTTOM end and the row pairs are bottom-anchored.
	//   spine v0->v2 at y = 50 is x = 126.42; the middle vertex is at 200.5.
	//   d = +1, R = v2, S = ceil(98.25) & ~1 = 98, A = 100, phase = 2.
	void MidRightFallingTriangle(GSVertexSW* v)
	{
		Vertex(v[0], 140.5f, 10.0f, 0.0f, 0.0f);
		Vertex(v[1], 200.5f, 50.0f, 255.0f * kLevel, 255.0f * kLevel);
		Vertex(v[2], 98.25f, 130.0f, 120.0f * kLevel, 60.0f * kLevel);
	}

	constexpr Anchor kMidRightFalling = {1, 98, 100, 2, false, 98.25f, 130.0f, 120.0f * kLevel, 60.0f * kLevel};

	// Middle vertex on the LEFT, so the walk runs leftward and the anchor is the
	// spine's larger-x end, which here is the top.
	//   spine v0->v2 at y = 50 is x = 193.83; the middle vertex is at 100.5.
	//   d = -1, R = v0, S = floor(200.5) | 1 = 201, A = 199, phase = (201+1)&7 = 2.
	void MidLeftTriangle(GSVertexSW* v)
	{
		Vertex(v[0], 200.5f, 10.0f, 0.0f, 0.0f);
		Vertex(v[1], 100.5f, 50.0f, 255.0f * kLevel, 255.0f * kLevel);
		Vertex(v[2], 180.5f, 130.0f, 120.0f * kLevel, 60.0f * kLevel);
	}

	constexpr Anchor kMidLeft = {-1, 201, 199, 2, true, 200.5f, 10.0f, 0.0f, 0.0f};

	// A flat bottom whose left edge is vertical. Both bottom vertices share a y,
	// so the spine ends at the LEFT one of the two -- which makes the spine the
	// left edge and the middle vertex the bottom-right corner.
	//   d = +1, R = v0, S = ceil(50.5) & ~1 = 50, A = 52, phase = 2.
	void FlatBottomTriangle(GSVertexSW* v)
	{
		Vertex(v[0], 50.5f, 10.0f, 0.0f, 0.0f);
		Vertex(v[1], 50.5f, 90.0f, 120.0f * kLevel, 60.0f * kLevel);
		Vertex(v[2], 150.5f, 90.0f, 255.0f * kLevel, 255.0f * kLevel);
	}

	constexpr Anchor kFlatBottom = {1, 50, 52, 2, true, 50.5f, 10.0f, 0.0f, 0.0f};

	// The same mid-right shape with a VERTICAL left edge, so `left` is the same
	// pixel on every row and a row pair's two seeds can only differ by the
	// vertical step.
	//   d = +1, R = v0, S = 100, A = 102, phase = 4.
	void VerticalEdgeTriangle(GSVertexSW* v)
	{
		Vertex(v[0], 100.5f, 10.0f, 0.0f, 0.0f);
		Vertex(v[1], 200.5f, 50.0f, 255.0f * kLevel, 255.0f * kLevel);
		Vertex(v[2], 100.5f, 130.0f, 120.0f * kLevel, 60.0f * kLevel);
	}

	constexpr Anchor kVerticalEdge = {1, 100, 102, 4, true, 100.5f, 10.0f, 0.0f, 0.0f};
} // namespace

// ---------------------------------------------------------------------------
// 1. The gradient is ten bits.
// ---------------------------------------------------------------------------

TEST(SwColourWalk, TheGradientIsTruncatedToTenBitsOfALevel)
{
	GSVertexSW v[3];
	MidRightTriangle(v);
	const SpanRecord& rec = Walk(v);

	const Plane p = PlaneOf(v);

	// The subject has to have a gradient the truncation can move, or the case
	// decides nothing.
	ASSERT_NE(p.raw[0], p.g[0]) << "red's gradient is already on the 1/8 grid here";
	ASSERT_NE(p.fraw, p.fg) << "fog's gradient is already on the 1/8 grid here";

	const float got_c = rec.dscan.c.x;
	const float got_f = rec.dscan.t.w;

	EXPECT_FLOAT_EQ(got_c * 8.0f, std::trunc(got_c * 8.0f)) << "not a multiple of 1/8 of a unit";
	EXPECT_FLOAT_EQ(got_f * 8.0f, std::trunc(got_f * 8.0f)) << "not a multiple of 1/8 of a unit";

	// Toward zero: never further from zero than the untruncated product, and
	// never more than one step of the grid closer to it.
	EXPECT_LE(std::abs(got_c), std::abs(p.raw[0]));
	EXPECT_LT(std::abs(p.raw[0]) - std::abs(got_c), 0.125f);
	EXPECT_LE(std::abs(got_f), std::abs(p.fraw));
	EXPECT_LT(std::abs(p.fraw) - std::abs(got_f), 0.125f);

	EXPECT_FLOAT_EQ(got_c, p.g[0]);
	EXPECT_FLOAT_EQ(got_f, p.fg);
}

// ---------------------------------------------------------------------------
// 2. The anchor, the direction and the block grid.
// ---------------------------------------------------------------------------

namespace
{
	void ExpectAnchor(const GSColourWalk& w, const Anchor& a, const char* what)
	{
		EXPECT_EQ(w.live, 1) << what;
		EXPECT_EQ(w.c.w, 8) << what
						  << ": untextured with fog and AA1 off walks an eight-pixel block";
		EXPECT_EQ(w.d, a.d) << what << ": walk direction";
		EXPECT_EQ(w.top_anchor, a.top_anchor ? 1 : 0) << what << ": which end of the spine anchors";
		EXPECT_FLOAT_EQ(w.xr, a.xr) << what << ": anchor x";
		EXPECT_FLOAT_EQ(w.yr, a.yr) << what << ": anchor y";
		EXPECT_EQ(w.S, a.S) << what << ": block grid origin";
		EXPECT_EQ(w.A, a.A) << what << ": the pixel that is the plane exactly";
		EXPECT_EQ(w.c.phase, a.phase) << what << ": the eight-pixel phase";
	}
} // namespace

TEST(SwColourWalk, TheAnchorAndGridComeFromTheSpine)
{
	GSVertexSW v[3];

	MidRightTriangle(v);
	ExpectAnchor(Walk(v).cwalk, kMidRight, "middle vertex right, spine leaning right");

	MidRightFallingTriangle(v);
	ExpectAnchor(Walk(v).cwalk, kMidRightFalling, "middle vertex right, spine leaning left");

	MidLeftTriangle(v);
	ExpectAnchor(Walk(v).cwalk, kMidLeft, "middle vertex left");

	FlatBottomTriangle(v);
	ExpectAnchor(Walk(v).cwalk, kFlatBottom, "flat bottom, vertical left edge");
}

// ---------------------------------------------------------------------------
// 3. The row seed.
// ---------------------------------------------------------------------------

TEST(SwColourWalk, ARowPairsSecondRowIsTheFirstPlusTheCoarseVerticalStep)
{
	// A vertical left edge, so `left` is 101 on every row and the only thing that
	// can move between the two rows of a pair is the vertical step.
	GSVertexSW v[3];
	VerticalEdgeTriangle(v);
	const SpanRecord& rec = Walk(v);

	const Plane p = PlaneOf(v);

	ASSERT_NE(p.gyc[0], 0.0f) << "no vertical step here, so the case decides nothing";

	int checked = 0;
	for (int y = 12; y <= 126; y += 6)
	{
		// y and y + 1 share a pair when y is even and the anchor is the top.
		const int i0 = RowAt(rec, y);
		const int i1 = RowAt(rec, y + 1);
		ASSERT_GE(i0, 0) << "row " << y << " was not drawn";
		ASSERT_GE(i1, 0) << "row " << (y + 1) << " was not drawn";
		ASSERT_EQ(rec.left[i0], 101);
		ASSERT_EQ(rec.left[i1], 101);

		EXPECT_FLOAT_EQ(rec.c[i1].x - rec.c[i0].x, p.gyc[0]) << "rows " << y << " and " << (y + 1);
		EXPECT_FLOAT_EQ(rec.t[i1].w - rec.t[i0].w, p.fgyc) << "rows " << y << " and " << (y + 1);
		checked++;
	}
	EXPECT_GT(checked, 8);
}

TEST(SwColourWalk, TheSeedIsThePlaneAtTheAnchorWalkedToTheFirstPixel)
{
	GSVertexSW v[3];
	MidRightTriangle(v);
	const SpanRecord& rec = Walk(v);

	const Plane p = PlaneOf(v);

	int checked = 0;
	for (int y = 11; y <= 128; y += 3)
	{
		const int i = RowAt(rec, y);
		ASSERT_GE(i, 0) << "row " << y << " was not drawn";

		EXPECT_FLOAT_EQ(rec.c[i].x, Seed(p, 0, kMidRight, rec.left[i], y)) << "row " << y;
		EXPECT_FLOAT_EQ(rec.t[i].w, SeedFog(p, kMidRight, rec.left[i], y)) << "row " << y;
		checked++;
	}
	EXPECT_GT(checked, 20);
}

TEST(SwColourWalk, TheSeedIsThePlaneWalkingLeftToo)
{
	GSVertexSW v[3];
	MidLeftTriangle(v);
	const SpanRecord& rec = Walk(v);

	const Plane p = PlaneOf(v);

	int checked = 0;
	for (int y = 11; y <= 128; y += 3)
	{
		const int i = RowAt(rec, y);
		ASSERT_GE(i, 0) << "row " << y << " was not drawn";

		EXPECT_FLOAT_EQ(rec.c[i].x, Seed(p, 0, kMidLeft, rec.left[i], y)) << "row " << y;
		checked++;
	}
	EXPECT_GT(checked, 20);
}

// ---------------------------------------------------------------------------
// 4. The lane tables.
// ---------------------------------------------------------------------------

namespace
{
	// The walk's offset at absolute pixel i, in colour units: the ramp out to it
	// plus the block jump accumulated to its block, floored -- which is what a
	// whole-unit lane can carry, and at a four-pixel block the jump is half a unit
	// so the flooring is what makes successive blocks alternate.
	int OffW(float gc, float g, int S, int d, int bw, int i, float pf)
	{
		const float dw = (g - gc) * static_cast<float>(bw);
		const int b = d * ((d * (i - S)) >> ((bw == 8) ? 3 : 2));

		// ⚠️ The jump is floored WITH the row's own fractional part in it, which is
		// what makes the walk one floor at every pixel rather than a floored seed
		// plus a separately floored jump (gs-cwalk's landing). It is inert wherever
		// dw is whole -- every eight-wide block -- and decisive at four.
		return static_cast<int>(gc) * i
		       + static_cast<int>(std::floor(dw * static_cast<float>(b) + pf));
	}

	/// The fractional part of the row's plane constant, per channel: what OffW's
	/// jump is floored with.
	float RowFrac(const Plane& p, int ch, const Anchor& a, int y)
	{
		const int yf = a.top_anchor ? (y & ~1) : (y | 1);
		const float P = a.ar + Tz8(p.g[ch] * (static_cast<float>(a.A) - a.xr))
		                + Tz8(p.gy[ch] * (static_cast<float>(yf) - a.yr));

		return P - std::floor(P);
	}

	float RowFracFog(const Plane& p, const Anchor& a, int y)
	{
		const int yf = a.top_anchor ? (y & ~1) : (y | 1);
		const float P = a.fr + Tz8(p.fg * (static_cast<float>(a.A) - a.xr))
		                + Tz8(p.fgy * (static_cast<float>(yf) - a.yr));

		return P - std::floor(P);
	}

	/// `bw` is the colour lane's block width and `fbw` the fog lane's. They differ
	/// on exactly one kind of draw -- untextured with fog on.
	void ExpectLaneTables(const SpanRecord& rec, const Plane& p, const Anchor& a, int bw,
		int fbw, const char* what)
	{
		const GSScanlineLocalData& local = rec.local;
		ASSERT_TRUE(rec.tables_seen) << what << ": no span was drawn, so no table was read";

		// The tables follow the row, so the oracle needs the row they were read at.
		const float pf0 = RowFrac(p, 0, a, rec.tables_top);
		const float pf1 = RowFrac(p, 1, a, rec.tables_top);
		const float pf2 = RowFrac(p, 2, a, rec.tables_top);
		const float pf3 = RowFrac(p, 3, a, rec.tables_top);
		const float pff = RowFracFog(p, a, rec.tables_top);

		for (int s = 0; s < 8; s++)
		{
			const int base = s & ~(kLanes - 1);

			for (int l = 0; l < kLanes; l++)
			{
				const int i = base + l;

				const int want_r = static_cast<s16>(
					OffW(p.gc[0], p.g[0], a.S, a.d, bw, i, pf0) - OffW(p.gc[0], p.g[0], a.S, a.d, bw, s, pf0));
				const int want_b = static_cast<s16>(
					OffW(p.gc[2], p.g[2], a.S, a.d, bw, i, pf2) - OffW(p.gc[2], p.g[2], a.S, a.d, bw, s, pf2));
				const int want_g = static_cast<s16>(
					OffW(p.gc[1], p.g[1], a.S, a.d, bw, i, pf1) - OffW(p.gc[1], p.g[1], a.S, a.d, bw, s, pf1));
				const int want_a = static_cast<s16>(
					OffW(p.gc[3], p.g[3], a.S, a.d, bw, i, pf3) - OffW(p.gc[3], p.g[3], a.S, a.d, bw, s, pf3));
				const int want_f = static_cast<s16>(
					OffW(p.fgc, p.fg, a.S, a.d, fbw, i, pff) - OffW(p.fgc, p.fg, a.S, a.d, fbw, s, pff));

				EXPECT_EQ(Lane16(local.d[s].rb, 2 * l), want_r) << what << " d[" << s << "] lane " << l << " red";
				EXPECT_EQ(Lane16(local.d[s].rb, 2 * l + 1), want_b) << what << " d[" << s << "] lane " << l << " blue";
				EXPECT_EQ(Lane16(local.d[s].ga, 2 * l), want_g) << what << " d[" << s << "] lane " << l << " green";
				EXPECT_EQ(Lane16(local.d[s].ga, 2 * l + 1), want_a) << what << " d[" << s << "] lane " << l << " alpha";
				EXPECT_EQ(Lane16(local.d[s].f, 2 * l), want_f) << what << " d[" << s << "] lane " << l << " fog";
			}
		}

#if _M_SSE < 0x501
		// A whole block is one 8g however the two vectors split it, so the two
		// phases of every pair have to sum to it.
		for (int s = 0; s < 8; s++)
		{
			for (int l = 0; l < kLanes; l++)
			{
				EXPECT_EQ(Lane16(local.dw[s][0].rb, 2 * l) + Lane16(local.dw[s][1].rb, 2 * l),
					static_cast<int>(p.g8[0]))
					<< what << " dw[" << s << "] lane " << l << " red";
				EXPECT_EQ(Lane16(local.dw[s][0].ga, 2 * l) + Lane16(local.dw[s][1].ga, 2 * l),
					static_cast<int>(p.g8[1]))
					<< what << " dw[" << s << "] lane " << l << " green";
				EXPECT_EQ(Lane16(local.dw[s][0].f, 2 * l) + Lane16(local.dw[s][1].f, 2 * l),
					static_cast<int>(p.fg8))
					<< what << " dw[" << s << "] lane " << l << " fog";
			}
		}
#endif
	}
} // namespace

TEST(SwColourWalk, TheLaneTablesCarryTheBlocksOwnOffsets)
{
	GSVertexSW v[3];

	MidRightTriangle(v);
	{
		const SpanRecord& rec = Walk(v);
		ASSERT_EQ(rec.cwalk.c.phase, 4);
		ExpectLaneTables(rec, PlaneOf(v), kMidRight, 8, 8, "phase 4");
	}

	FlatBottomTriangle(v);
	{
		const SpanRecord& rec = Walk(v);
		ASSERT_EQ(rec.cwalk.c.phase, 2);
		ExpectLaneTables(rec, PlaneOf(v), kFlatBottom, 8, 8, "phase 2");
	}
}

// ---------------------------------------------------------------------------
// 5. The separation control. A test that cannot tell two anchors apart proves
//    nothing by passing, so the distance between them is asserted directly.
// ---------------------------------------------------------------------------

TEST(SwColourWalk, TheOtherAnchorIsFarEnoughAwayToSee)
{
	GSVertexSW v[3];
	MidLeftTriangle(v);
	const SpanRecord& rec = Walk(v);

	const Plane p = PlaneOf(v);

	// The middle vertex is the anchor the section walk used to take. Evaluating
	// the same plane from there instead of from v0 has to move the seed by more
	// than a colour unit, or nothing above can fail.
	const int y = 60;
	const int i = RowAt(rec, y);
	ASSERT_GE(i, 0);

	const int x = rec.left[i];
	const float from_anchor = Seed(p, 0, kMidLeft, x, y);
	const float from_middle = v[1].c.x + p.g[0] * (static_cast<float>(x) - v[1].p.x)
	                          + p.gy[0] * (static_cast<float>(y) - v[1].p.y);

	EXPECT_GT(std::abs(from_anchor - from_middle), 1.0f)
		<< "the two anchors are within a colour unit of each other here, so this "
		   "triangle cannot separate them";
}

// ---------------------------------------------------------------------------
// 5b. The block is eight pixels wide untextured and FOUR textured, and nothing
//     else about the walk moves with it.
//
//     gs-walk2's width section draws the same geometry with TME off and on: at
//     eight its untextured arm reads 100.000% and its textured arm 91.181%, and
//     at four the two swap exactly. gs-shade's CDDA reference cell -- a textured
//     draw at the texture function's identity, so the stored pixel IS the
//     interpolated colour -- reads 94.531% at eight and 100.000% at four.
//
//     Both cases below draw the SAME triangle. Only the texture function differs.
// ---------------------------------------------------------------------------

TEST(SwColourWalk, ATexturedTriangleWalksFourPixelBlocks)
{
	GSVertexSW v[3];
	FlatBottomTriangle(v);
	const SpanRecord& rec = Walk(v, GS_TRIANGLE_CLASS, 3, TFX_MODULATE);

	const Plane p = PlaneOf(v);

	EXPECT_EQ(rec.cwalk.c.w, 4) << "a textured draw walks a four-pixel block";
	EXPECT_EQ(rec.cwalk.c.wshift, 2);
	// Everything else about the walk is the geometry's, unchanged by the width.
	EXPECT_EQ(rec.cwalk.d, kFlatBottom.d);
	EXPECT_EQ(rec.cwalk.S, kFlatBottom.S);
	EXPECT_EQ(rec.cwalk.A, kFlatBottom.A);
	EXPECT_EQ(rec.cwalk.c.phase, kFlatBottom.S & 3);
	EXPECT_FLOAT_EQ(rec.dscan.c.x, p.g[0]) << "the ten-bit gradient is the same at either width";

	int checked = 0;
	for (int y = 11; y <= 89; y += 3)
	{
		const int i = RowAt(rec, y);
		ASSERT_GE(i, 0) << "row " << y << " was not drawn";

		EXPECT_FLOAT_EQ(rec.c[i].x, Seed(p, 0, kFlatBottom, rec.left[i], y, 4)) << "row " << y;
		EXPECT_FLOAT_EQ(rec.t[i].w, SeedFog(p, kFlatBottom, rec.left[i], y, 4)) << "row " << y;
		checked++;
	}
	EXPECT_GT(checked, 20);

	// The width lives in the lane tables, which is where it is scored. The seed
	// alone cannot separate the two: this subject's left edge is vertical and its
	// first pixel is in the anchor's own block at either width.
	ExpectLaneTables(rec, p, kFlatBottom, 4, 4, "textured, four-pixel block");

	// And a test that cannot tell four from eight proves nothing by passing.
	int separated = 0;
	for (int i = 0; i < 8; i++)
	{
		const float pf = RowFrac(p, 0, kFlatBottom, rec.tables_top);

		if (OffW(p.gc[0], p.g[0], kFlatBottom.S, kFlatBottom.d, 4, i, pf)
			!= OffW(p.gc[0], p.g[0], kFlatBottom.S, kFlatBottom.d, 8, i, pf))
		{
			separated++;
		}
	}
	EXPECT_GT(separated, 0) << "the two widths give the same offsets on this subject, "
	                           "so it cannot tell them apart";
}

namespace
{
	// One of the three narrowing factors, on the same triangle, with everything
	// else off. Each narrows the block on its own, both lanes together; `fbw` is
	// carried separately only so that a future factor that split them would have
	// somewhere to say so.
	void ExpectNarrowedByOneFactor(const SpanRecord& rec, const Plane& p, int fbw, const char* what)
	{
		EXPECT_EQ(rec.cwalk.c.w, 4) << what << " narrows the colour block on its own";
		EXPECT_EQ(rec.cwalk.c.wshift, 2) << what;
		EXPECT_EQ(rec.cwalk.f.w, fbw) << what << ": the fog lane's own width";
		// The factor moves the width and nothing else about the walk.
		EXPECT_EQ(rec.cwalk.d, kFlatBottom.d) << what;
		EXPECT_EQ(rec.cwalk.S, kFlatBottom.S) << what;
		EXPECT_EQ(rec.cwalk.A, kFlatBottom.A) << what;
		EXPECT_EQ(rec.cwalk.c.phase, kFlatBottom.S & 3) << what;

		int checked = 0;
		for (int y = 11; y <= 89; y += 7)
		{
			const int i = RowAt(rec, y);
			ASSERT_GE(i, 0) << what << ": row " << y << " was not drawn";

			EXPECT_FLOAT_EQ(rec.c[i].x, Seed(p, 0, kFlatBottom, rec.left[i], y, 4))
				<< what << ": row " << y;
			EXPECT_FLOAT_EQ(rec.t[i].w, SeedFog(p, kFlatBottom, rec.left[i], y, fbw))
				<< what << ": row " << y;
			checked++;
		}
		EXPECT_GT(checked, 8);

		ExpectLaneTables(rec, p, kFlatBottom, 4, fbw, what);
	}
} // namespace

// gs-pipe (SCPH-30001, 2026-09-07) swept the pipeline one factor at a time against
// an untextured base. Fog and AA1 each narrow the COLOUR block to four on their
// own, and blending, a depth test with depth writes and the target format do not.
// These are the two that do.

TEST(SwColourWalk, FogNarrowsBothLanesOnAnUntexturedDraw)
{
	// gs-pipe's fog cell holds F at 0xff on every vertex, so it has no fog gradient
	// and could only say what FGE does to the COLOUR lane. gs-fog (2026-09-07)
	// walks F itself: Model7 on F at width four, 100.000% over 115,040 readings on
	// all three sections, with texture or AA1 on top moving nothing. Fog is not a
	// second interpolator, so both lanes narrow together.
	GSVertexSW v[3];
	FlatBottomTriangle(v);
	const SpanRecord& rec = Walk(v, GS_TRIANGLE_CLASS, 3, TFX_NONE, 1, 0);

	ExpectNarrowedByOneFactor(rec, PlaneOf(v), 4, "fog");
}

TEST(SwColourWalk, AA1NarrowsBothLanesOnAnUntexturedDraw)
{
	// Nothing separates the two lanes under AA1, so it narrows both.
	GSVertexSW v[3];
	FlatBottomTriangle(v);
	const SpanRecord& rec = Walk(v, GS_TRIANGLE_CLASS, 3, TFX_NONE, 0, 1);

	ExpectNarrowedByOneFactor(rec, PlaneOf(v), 4, "AA1");
}

TEST(SwColourWalk, TheSameTriangleUntexturedStillWalksEight)
{
	GSVertexSW v[3];
	FlatBottomTriangle(v);
	const SpanRecord& rec = Walk(v);

	const Plane p = PlaneOf(v);

	EXPECT_EQ(rec.cwalk.c.w, 8)
		<< "an untextured draw with fog and AA1 off walks an eight-pixel block";
	EXPECT_EQ(rec.cwalk.c.wshift, 3);
	EXPECT_EQ(rec.cwalk.c.phase, kFlatBottom.phase);

	int checked = 0;
	for (int y = 11; y <= 89; y += 3)
	{
		const int i = RowAt(rec, y);
		ASSERT_GE(i, 0) << "row " << y << " was not drawn";

		EXPECT_FLOAT_EQ(rec.c[i].x, Seed(p, 0, kFlatBottom, rec.left[i], y)) << "row " << y;
		checked++;
	}
	EXPECT_GT(checked, 20);

	ExpectLaneTables(rec, p, kFlatBottom, 8, 8, "untextured, eight-pixel block");
}

// ---------------------------------------------------------------------------
// 6. A primitive with no walk leaves every colour table entry zero.
// ---------------------------------------------------------------------------

namespace
{
	void ExpectZeroTables(const SpanRecord& rec, const char* what)
	{
		ASSERT_TRUE(rec.tables_seen) << what << ": no span was drawn, so no table was read";
		const GSScanlineLocalData& local = rec.local;

		for (int s = 0; s < 8; s++)
		{
			for (int k = 0; k < 2 * kLanes; k++)
			{
				EXPECT_EQ(Lane16(local.d[s].rb, k), 0) << what << " d[" << s << "].rb[" << k << "]";
				EXPECT_EQ(Lane16(local.d[s].ga, k), 0) << what << " d[" << s << "].ga[" << k << "]";
				EXPECT_EQ(Lane16(local.d[s].f, k), 0) << what << " d[" << s << "].f[" << k << "]";
			}
		}

#if _M_SSE < 0x501
		for (int s = 0; s < 8; s++)
		{
			for (int ph = 0; ph < 2; ph++)
			{
				for (int k = 0; k < 2 * kLanes; k++)
				{
					EXPECT_EQ(Lane16(local.dw[s][ph].rb, k), 0) << what << " dw";
					EXPECT_EQ(Lane16(local.dw[s][ph].ga, k), 0) << what << " dw";
					EXPECT_EQ(Lane16(local.dw[s][ph].f, k), 0) << what << " dw";
				}
			}
		}
		for (int k = 0; k < 8; k++)
		{
			EXPECT_EQ(Lane16(local.d4.c, k), 0) << what << " d4.c";
			EXPECT_EQ(Lane16(local.d4.f, k), 0) << what << " d4.f";
		}
#else
		EXPECT_EQ(local.d8.c.rb, 0u) << what;
		EXPECT_EQ(local.d8.c.ga, 0u) << what;
		EXPECT_EQ(local.d8.p.f, 0u) << what;
#endif
	}
} // namespace

TEST(SwColourWalk, AFlatTriangleWalksNothing)
{
	GSVertexSW v[3];
	Vertex(v[0], 100.5f, 10.0f, 40.0f * kLevel, 20.0f * kLevel);
	Vertex(v[1], 200.5f, 50.0f, 40.0f * kLevel, 20.0f * kLevel);
	Vertex(v[2], 140.5f, 130.0f, 40.0f * kLevel, 20.0f * kLevel);

	ExpectZeroTables(Walk(v), "flat triangle");
}

TEST(SwColourWalk, ALineWalksNothing)
{
	GSVertexSW v[2];
	Vertex(v[0], 40.0f, 40.0f, 0.0f, 0.0f);
	Vertex(v[1], 200.0f, 90.0f, 255.0f * kLevel, 255.0f * kLevel);

	ExpectZeroTables(Walk(v, GS_LINE_CLASS, 2), "line");
}

TEST(SwColourWalk, ASpriteWalksNothing)
{
	GSVertexSW v[2];
	Vertex(v[0], 40.0f, 40.0f, 0.0f, 0.0f);
	Vertex(v[1], 200.0f, 90.0f, 255.0f * kLevel, 255.0f * kLevel);
	v[0].t = GSVector4(0.0f, 0.0f, 1.0f, 0.0f);
	v[1].t = GSVector4(64.0f, 32.0f, 1.0f, 0.0f);

	ExpectZeroTables(Walk(v, GS_SPRITE_CLASS, 2), "sprite");
}

#endif // MULTI_ISA_SHARED_COMPILATION
