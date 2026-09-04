// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Where an AA1 triangle's widened band is allowed to land.
//
// AA1 widens each of a triangle's three sides by one pixel -- along Y for sides
// closer to the X axis than 45 degrees, along X for the steeper ones -- and ramps
// the coverage from full on the original side to zero on the new outer one. So a
// vertical side puts its zero-coverage column one pixel OUTSIDE the primitive's own
// x extent, exactly as a horizontal side puts its zero-coverage row one pixel above
// or below the y extent.
//
// The rasterizer bounded the two axes differently: the y bound was widened by a
// pixel and the x bound was not, under a comment saying the x rule had been picked
// arbitrarily because the hardware's behaviour was unknown. It is known now. A
// console capture of a right triangle with a vertical left side draws that column,
// at coverage zero, on every row of the side; our arm drew nothing there at all.
//
// The cases below are that capture's own geometry, and the expectations are the
// pixels it read back.
//
// The rasterizer is per-architecture, so this rides ARCH_ARM64 like its siblings.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace
{
struct EdgePixel
{
	int x;
	int y;
	u32 cov;   // the 16-bit coverage the edge walk stores in scan.p.U32[0]
	u32 alpha; // what the scanline makes of it: cov >> 9, the GS's 0..128 range
};

std::vector<EdgePixel>* g_edges = nullptr;
int g_spans = 0;

void RecordEdge(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData&)
{
	const u32 cov = scan.p.U32[0];
	for (int i = 0; i < pixels; i++)
		g_edges->push_back({left + i, top, cov, cov >> 9});
}

void RecordSpan(int, int, int, const GSVertexSW&, GSScanlineLocalData&)
{
	g_spans++;
}

void NoSetup(const GSVertexSW*, const u16*, const GSVertexSW&, GSScanlineLocalData&) {}

// Run one AA1 triangle through the real rasterizer and collect what its edge walk
// emitted. Nothing here compiles a scanline: the two callbacks stand in for the
// generated code, so what is under test is the walk's own geometry.
std::vector<EdgePixel> EdgePixelsOfTriangle(const float (&xy)[3][2])
{
	std::vector<EdgePixel> out;
	g_edges = &out;
	g_spans = 0;

	GSVertexSW vertex[3];
	for (int i = 0; i < 3; i++)
	{
		vertex[i] = GSVertexSW::zero();
		vertex[i].p = GSVector4(xy[i][0], xy[i][1], 0.0f, 0.0f);
		vertex[i].p.F64[1] = 0.0;
		vertex[i].c = GSVector4(0.0f, 0.0f, 0.0f, 128.0f * 128.0f);
	}
	static const u16 index[3] = {0, 1, 2};

	isa_native::GSRasterizerData data;
	data.primclass = GS_TRIANGLE_CLASS;
	data.vertex = vertex;
	data.vertex_count = 3;
	data.index = const_cast<u16*>(index);
	data.index_count = 3;
	data.scissor = GSVector4i(0, 0, 640, 640);
	data.bbox = GSVector4i(0, 0, 640, 640);
	data.global.sel.key = 0;
	data.global.sel.aa1 = 1;
	data.global.sel.iip = 1;
	data.setup_prim = &NoSetup;
	data.draw_scanline = &RecordSpan;
	data.draw_edge = &RecordEdge;

	isa_native::GSRasterizer r(nullptr, 0, 1);
	r.Draw(data);

	g_edges = nullptr;
	std::sort(out.begin(), out.end(), [](const EdgePixel& a, const EdgePixel& b) {
		return a.y != b.y ? a.y < b.y : a.x < b.x;
	});
	return out;
}

bool HasPixel(const std::vector<EdgePixel>& px, int x, int y, u32 alpha)
{
	for (const EdgePixel& p : px)
	{
		if (p.x == x && p.y == y)
			return p.alpha == alpha;
	}
	return false;
}

int CountAtX(const std::vector<EdgePixel>& px, int x)
{
	int n = 0;
	for (const EdgePixel& p : px)
		n += (p.x == x);
	return n;
}

// gs-prim case 389 on an SCPH-30001: a right triangle whose left side is vertical
// at x = 81 and whose top side is horizontal at y = 385.
const float kRightTriangle[3][2] = {{81.0f, 385.0f}, {95.0f, 385.0f}, {81.0f, 399.0f}};

TEST(Aa1EdgeBand, AVerticalSideWidensOneColumnOutsideThePrimitive)
{
	const std::vector<EdgePixel> px = EdgePixelsOfTriangle(kRightTriangle);

	// The console drew x = 80 on every row the vertical side covers, at coverage 0.
	for (int y = 385; y <= 399; y++)
		EXPECT_TRUE(HasPixel(px, 80, y, 0)) << "no zero-coverage edge pixel at (80," << y << ")";

	EXPECT_EQ(CountAtX(px, 80), 15);
}

TEST(Aa1EdgeBand, TheHorizontalSideStillWidensOneRowAbove)
{
	const std::vector<EdgePixel> px = EdgePixelsOfTriangle(kRightTriangle);

	// The control: the Y half of the same rule was always right, and must stay right.
	bool any = false;
	for (const EdgePixel& p : px)
	{
		if (p.y == 384)
		{
			any = true;
			EXPECT_EQ(p.alpha, 0u) << "row above the top side at (" << p.x << ",384)";
		}
	}
	EXPECT_TRUE(any) << "the row above the horizontal side was not drawn at all";
}

TEST(Aa1EdgeBand, TheBandDoesNotReachTwoColumnsOut)
{
	const std::vector<EdgePixel> px = EdgePixelsOfTriangle(kRightTriangle);

	// The widening is one pixel, not two: nothing at x = 79, and nothing past the
	// right-hand vertex either.
	EXPECT_EQ(CountAtX(px, 79), 0);
	for (const EdgePixel& p : px)
		EXPECT_LE(p.x, 96) << "edge pixel past the primitive at (" << p.x << "," << p.y << ")";
}

} // namespace

#endif // ARCH_ARM64
