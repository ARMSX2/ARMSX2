// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The colour and fog step tables a scanline reads, across primitives.
//
// A primitive with a colour walk (a triangle's main pass) fills the tables per
// row. A primitive without one (sprite, line, point, AA1 edge) must see them all
// zero on every row it draws, whatever was drawn before it. The rasterizer is
// free to keep zero tables from one walkless primitive to the next instead of
// rewriting them, so these cases pin what the scanline sees in both orders:
// walked then walkless, and walkless then walkless.
//
// Nothing here compiles a scanline: the setup and scanline callbacks are stubs
// that inspect the tables, so what is under test is the rasterizer's handling of
// them. The rasterizer is per-architecture, so this rides ARCH_ARM64 like its
// siblings.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"

#include <gtest/gtest.h>

#include <cstring>

namespace
{
struct SpanLog
{
	int spans = 0;
	int zero_spans = 0;
	int live_spans = 0;
};

SpanLog g_log;

bool IsZero(const void* p, size_t size)
{
	const u8* b = static_cast<const u8*>(p);
	for (size_t i = 0; i < size; i++)
	{
		if (b[i] != 0)
			return false;
	}
	return true;
}

// Every table SetupColourWalkTables writes.
bool ColourTablesAreZero(const GSScanlineLocalData& local)
{
	for (int s = 0; s < 8; s++)
	{
		if (!IsZero(&local.d[s].rb, sizeof(local.d[s].rb)) || !IsZero(&local.d[s].ga, sizeof(local.d[s].ga)) ||
			!IsZero(&local.d[s].f, sizeof(local.d[s].f)))
			return false;
	}
#if _M_SSE >= 0x501
	return IsZero(&local.d8.c, sizeof(local.d8.c)) && IsZero(&local.d8.p.f, sizeof(local.d8.p.f));
#else
	if (!IsZero(&local.d4.c, sizeof(local.d4.c)) || !IsZero(&local.d4.f, sizeof(local.d4.f)))
		return false;
	return IsZero(local.dw, sizeof(local.dw));
#endif
}

void RecordSpan(int, int, int, const GSVertexSW&, GSScanlineLocalData& local)
{
	g_log.spans++;
	g_log.zero_spans += ColourTablesAreZero(local) ? 1 : 0;
	g_log.live_spans += local.cwalk.live ? 1 : 0;
}

void NoSetup(const GSVertexSW*, const u16*, const GSVertexSW&, GSScanlineLocalData&) {}

class ColourWalkTables : public ::testing::Test
{
protected:
	isa_native::GSRasterizer m_r{nullptr, 0, 1};

	SpanLog Run(GS_PRIM_CLASS primclass, GSVertexSW* vertex, int count, bool iip)
	{
		static const u16 index[6] = {0, 1, 2, 3, 4, 5};

		g_log = {};

		isa_native::GSRasterizerData data;
		data.primclass = primclass;
		data.vertex = vertex;
		data.vertex_count = count;
		data.index = const_cast<u16*>(index);
		data.index_count = count;
		data.scissor = GSVector4i(0, 0, 256, 256);
		data.bbox = GSVector4i(0, 0, 256, 256);
		data.global.sel.key = 0;
		data.global.sel.iip = iip ? 1 : 0;
		data.global.sel.fb = 1;
		data.global.sel.fge = 1;
		data.setup_prim = &NoSetup;
		data.draw_scanline = &RecordSpan;
		data.draw_edge = nullptr;

		m_r.Draw(data);

		// Draw owns nothing it was handed.
		data.vertex = nullptr;
		data.index = nullptr;
		return g_log;
	}

	static GSVertexSW Vertex(float x, float y, float r, float g, float b, float fog)
	{
		GSVertexSW v = GSVertexSW::zero();
		v.p = GSVector4(x, y, 0.0f, 0.0f);
		v.p.F64[1] = 0.0;
		v.c = GSVector4(r, g, b, 128.0f) * GSVector4(128.0f);
		v.t = GSVector4(0.0f, 0.0f, 1.0f, fog);
		return v;
	}

	// A gouraud triangle with a colour and fog gradient on both axes, so its rows
	// leave non-zero tables behind.
	SpanLog Gouraud()
	{
		GSVertexSW v[3] = {
			Vertex(10.0f, 10.0f, 0.0f, 40.0f, 255.0f, 0.0f),
			Vertex(90.0f, 20.0f, 255.0f, 0.0f, 10.0f, 200.0f),
			Vertex(30.0f, 80.0f, 60.0f, 255.0f, 0.0f, 90.0f),
		};
		return Run(GS_TRIANGLE_CLASS, v, 3, true);
	}

	SpanLog Sprites()
	{
		GSVertexSW v[4] = {
			Vertex(8.0f, 8.0f, 0.0f, 0.0f, 0.0f, 0.0f),
			Vertex(20.0f, 16.0f, 200.0f, 100.0f, 50.0f, 0.0f),
			Vertex(40.0f, 40.0f, 0.0f, 0.0f, 0.0f, 0.0f),
			Vertex(47.0f, 51.0f, 10.0f, 20.0f, 30.0f, 0.0f),
		};
		return Run(GS_SPRITE_CLASS, v, 4, false);
	}

	SpanLog Lines()
	{
		GSVertexSW v[2] = {
			Vertex(5.0f, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f),
			Vertex(60.0f, 30.0f, 255.0f, 255.0f, 255.0f, 100.0f),
		};
		return Run(GS_LINE_CLASS, v, 2, true);
	}
};
} // namespace

TEST_F(ColourWalkTables, AGouraudTriangleLeavesNonZeroTables)
{
	// The precondition every other case leans on: without it, "zero after a
	// triangle" would prove nothing.
	const SpanLog tri = Gouraud();
	ASSERT_GT(tri.spans, 0);
	EXPECT_EQ(tri.live_spans, tri.spans);
	EXPECT_LT(tri.zero_spans, tri.spans);
}

TEST_F(ColourWalkTables, SpritesAfterATriangleSeeZeroTables)
{
	Gouraud();
	const SpanLog spr = Sprites();
	ASSERT_GT(spr.spans, 0);
	EXPECT_EQ(spr.live_spans, 0);
	EXPECT_EQ(spr.zero_spans, spr.spans);
}

TEST_F(ColourWalkTables, SpritesAfterSpritesSeeZeroTables)
{
	Gouraud();
	Sprites();
	const SpanLog spr = Sprites();
	ASSERT_GT(spr.spans, 0);
	EXPECT_EQ(spr.zero_spans, spr.spans);
}

TEST_F(ColourWalkTables, LinesAfterATriangleSeeZeroTables)
{
	Gouraud();
	const SpanLog line = Lines();
	ASSERT_GT(line.spans, 0);
	EXPECT_EQ(line.zero_spans, line.spans);
}

TEST_F(ColourWalkTables, ATriangleAfterSpritesRebuildsItsTables)
{
	Sprites();
	Sprites();
	const SpanLog tri = Gouraud();
	ASSERT_GT(tri.spans, 0);
	EXPECT_LT(tri.zero_spans, tri.spans);
}

TEST_F(ColourWalkTables, SpritesBetweenTwoTrianglesStillSeeZeroTables)
{
	Gouraud();
	Sprites();
	Gouraud();
	const SpanLog spr = Sprites();
	ASSERT_GT(spr.spans, 0);
	EXPECT_EQ(spr.zero_spans, spr.spans);
}

#endif // ARCH_ARM64
