// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The rect GSState::CheckOverlapVertsSlow compares an incoming primitive against.
//
// That heuristic asks a question about native PS2 geometry -- "is this primitive
// inside what the buffered draw already covers, so the order matters?" -- and it
// must therefore get the same answer at every upscale. It used to get a different
// one, because its incoming box is the raw window box in native pixels at every
// scale while the buffered rect it was tested against was the shipped draw rect,
// which above native keeps its raw sub-texel extent AND takes every primitive the
// finer cull grid lets through, including the ones that paint no native pixel.
//
// GSVertexKernels::PrimNativeDrawRect is the buffered rect asked on the native
// grid instead. What these cases pin:
//   * at the native grid it is bit-identical to the shipped rect, so native
//     resolution is a structural no-op rather than a gated one;
//   * for the same geometry it is the same rect at 1x, 2x and 4x;
//   * a primitive that paints no native pixel contributes nothing, which is what
//     the native cull does with it;
//   * the point and line classes, which the shipped rect does not round either,
//     keep the shipped rect at every grid;
//   * the shape of the real draw this came from -- Stuntman's s_n 28, which merged
//     at 1x and flushed at 2x -- now answers the same at both.

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "GS/GSState.h"
#include "GS/GSVertexKick.h"

namespace
{
	__fi GSVector4i WindowEntry(int x, int y)
	{
		return GSVector4i(x, y, x, y);
	}

	// A scissor no case here is trying to exercise; the cull form is 12.4.
	const GSVector4i kWideCull(-0x10000, -0x10000, 0x10000, 0x10000);
	// The pixel-space scissor the fold clamps with.
	const GSVector4i kWideScissor(0, 0, 4096, 4096);

	// One triangle, as its three window entries.
	struct Tri
	{
		int x0, y0, x1, y1, x2, y2;
	};

	// What the vertex kick does with one primitive at one grid: cull it, or take
	// its two rects. Mirrors GSState::VertexKick's accepted-prim tail exactly.
	struct PrimRects
	{
		bool accepted;
		GSVector4i shipped;
		GSVector4i native;
	};

	PrimRects KickTri(const Tri& t, const GSVertexKernels::CullGrid& grid, bool aa1 = false)
	{
		GSVector4i bbox;
		const u32 skip = GSVertexKernels::CullTest<3, GS_TRIANGLE_CLASS>(WindowEntry(t.x0, t.y0),
			WindowEntry(t.x1, t.y1), WindowEntry(t.x2, t.y2), kWideCull, grid, aa1, bbox);

		PrimRects out;
		out.accepted = (skip == 0);
		out.shipped = GSVertexKernels::PrimDrawRect(bbox);
		out.native = GSVertexKernels::PrimNativeDrawRectOrNone<GS_TRIANGLE_CLASS>(bbox);
		return out;
	}

	// The buffered draw rect a run of primitives leaves behind, both ways, exactly
	// as the kick accumulates it: union the accepted primitives' rects, clamp once
	// with the scissor.
	struct BufferRects
	{
		int accepted;
		GSVector4i shipped;
		GSVector4i native;
	};

	BufferRects BufferedRects(const std::vector<Tri>& tris, const GSVertexKernels::CullGrid& grid)
	{
		BufferRects out{0, GSVector4i::zero(), GSVector4i::zero()};
		for (const Tri& t : tris)
		{
			const PrimRects p = KickTri(t, grid);
			if (!p.accepted)
				continue;

			if (out.accepted == 0)
			{
				out.shipped = p.shipped;
				out.native = p.native;
			}
			else
			{
				out.shipped = out.shipped.runion(p.shipped);
				out.native = out.native.runion(p.native);
			}
			out.accepted++;
		}

		if (out.accepted != 0)
		{
			out.shipped = out.shipped.rintersect(kWideScissor);
			out.native = out.native.rintersect(kWideScissor);
		}
		return out;
	}

	// GSState::CheckOverlapVertsSlow's containment test, given the incoming
	// primitive's raw box already shifted to native pixels.
	bool Contained(const GSVector4i& new_area, const GSVector4i& rect)
	{
		return new_area.rintersect(rect).eq(new_area);
	}

	const GSVertexKernels::CullGrid kGrid1x = GSState::CullGridFor(1.0f, GSHalfPixelOffset::Native);
	const GSVertexKernels::CullGrid kGrid2x = GSState::CullGridFor(2.0f, GSHalfPixelOffset::Native);
	const GSVertexKernels::CullGrid kGrid4x = GSState::CullGridFor(4.0f, GSHalfPixelOffset::Native);
	// A scale whose sample points are not a sub-texel grid: nothing is culled and
	// the rect takes only the bottom/right sub-texel trim.
	const GSVertexKernels::CullGrid kGridNone = GSState::CullGridFor(1.5f, GSHalfPixelOffset::Native);
} // namespace

// ---------------------------------------------------------------------------
// At the native grid the native rect IS the shipped rect. This is the whole of
// the 1x byte-identity argument, and it is arithmetic rather than a branch: the
// rounded bbox's top/left is already a multiple of 16, so the +15 the ceil needs
// cannot carry it anywhere.
// ---------------------------------------------------------------------------
TEST(GsDrawBufferOverlap, NativeRectEqualsShippedRectAtTheNativeGrid)
{
	std::mt19937 rng(0xC13u);
	auto coord = [&rng]() { return static_cast<int>(rng() % 0x8000) - 0x1000; };

	int accepted = 0;
	for (int iter = 0; iter < 200000; iter++)
	{
		Tri t{coord(), coord(), coord(), coord(), coord(), coord()};
		// Land on the interior-rounding edge often.
		if ((iter & 3) == 0)
		{
			t.x0 &= ~0xF;
			t.y2 &= ~0xF;
		}

		const bool aa1 = (rng() & 7) == 0;
		const PrimRects p = KickTri(t, kGrid1x, aa1);
		if (!p.accepted)
			continue;
		accepted++;

		ASSERT_TRUE(p.native.eq(p.shipped))
			<< "iter " << iter << " v0=(" << t.x0 << "," << t.y0 << ") v1=(" << t.x1 << "," << t.y1
			<< ") v2=(" << t.x2 << "," << t.y2 << ") aa1=" << aa1;
		// An accepted primitive's native rect is never the union identity: at the
		// native grid CullTest already rejected everything that rounds to empty.
		ASSERT_FALSE(p.native.rempty()) << "iter " << iter;
	}
	EXPECT_GT(accepted, 1000);
}

// ---------------------------------------------------------------------------
// The same geometry, four grids, one rect. Where the native cull would have
// dropped the primitive the native rect is the union identity, which is what
// makes it drop out of the buffered union instead of dragging it outwards.
// ---------------------------------------------------------------------------
TEST(GsDrawBufferOverlap, NativeRectIsTheSameAtEveryScale)
{
	std::mt19937 rng(0x13Cu);
	auto coord = [&rng]() { return static_cast<int>(rng() % 0x4000) - 0x800; };

	int matched = 0, dropped = 0;
	for (int iter = 0; iter < 200000; iter++)
	{
		Tri t{coord(), coord(), coord(), coord(), coord(), coord()};
		// Sub-native primitives are the interesting population, so make a third of
		// the sweep small enough to fall between two sample points.
		if ((iter % 3) == 0)
		{
			t.x1 = t.x0 + static_cast<int>(rng() % 24);
			t.y1 = t.y0 + static_cast<int>(rng() % 24);
			t.x2 = t.x0 + static_cast<int>(rng() % 24);
			t.y2 = t.y0 + static_cast<int>(rng() % 24);
		}

		const PrimRects at1x = KickTri(t, kGrid1x);

		for (const GSVertexKernels::CullGrid& grid : {kGrid2x, kGrid4x, kGridNone})
		{
			const PrimRects up = KickTri(t, grid);
			if (!up.accepted)
				continue;

			if (at1x.accepted)
			{
				// It paints at native, so both scales must agree on where.
				ASSERT_TRUE(up.native.eq(at1x.native))
					<< "iter " << iter << " v0=(" << t.x0 << "," << t.y0 << ") v1=(" << t.x1 << ","
					<< t.y1 << ") v2=(" << t.x2 << "," << t.y2 << ")";
				matched++;
			}
			else
			{
				// It paints no native pixel, so it contributes nothing.
				ASSERT_TRUE(up.native.eq(GSVertexKernels::NativeDrawRectNone()))
					<< "iter " << iter << " v0=(" << t.x0 << "," << t.y0 << ") v1=(" << t.x1 << ","
					<< t.y1 << ") v2=(" << t.x2 << "," << t.y2 << ")";
				ASSERT_TRUE(GSVector4i::zero().runion(up.native).eq(GSVector4i::zero()))
					<< "the identity element must not move a union";
				dropped++;
			}
		}
	}
	EXPECT_GT(matched, 1000);
	EXPECT_GT(dropped, 1000);
}

// ---------------------------------------------------------------------------
// RoundCullRect does not round the point and line classes at any grid, so their
// contribution is already scale-invariant and the ceil must not be applied to
// them -- doing so would move native output.
// ---------------------------------------------------------------------------
TEST(GsDrawBufferOverlap, PointAndLineClassesKeepTheShippedRect)
{
	std::mt19937 rng(0x0C13u);
	auto coord = [&rng]() { return static_cast<int>(rng() % 0x8000) - 0x1000; };

	for (int iter = 0; iter < 50000; iter++)
	{
		const int x0 = coord(), y0 = coord(), x1 = coord(), y1 = coord();

		for (const GSVertexKernels::CullGrid& grid : {kGrid1x, kGrid2x, kGrid4x, kGridNone})
		{
			GSVector4i bbox;
			GSVertexKernels::CullTest<1, GS_POINT_CLASS>(WindowEntry(x0, y0), WindowEntry(x1, y1),
				WindowEntry(x1, y1), kWideCull, grid, false, bbox);
			ASSERT_TRUE(GSVertexKernels::PrimNativeDrawRectOrNone<GS_POINT_CLASS>(bbox).eq(
				GSVertexKernels::PrimDrawRect(bbox)))
				<< "point, iter " << iter;

			GSVertexKernels::CullTest<2, GS_LINE_CLASS>(WindowEntry(x0, y0), WindowEntry(x1, y1),
				WindowEntry(x1, y1), kWideCull, grid, false, bbox);
			ASSERT_TRUE(GSVertexKernels::PrimNativeDrawRectOrNone<GS_LINE_CLASS>(bbox).eq(
				GSVertexKernels::PrimDrawRect(bbox)))
				<< "line, iter " << iter;
		}
	}
}

// ---------------------------------------------------------------------------
// Stuntman's s_n 28, in shape.
//
// Q-stuntman measured the real draw: buffer 0 holding 9 indices at 1x and 15 at
// 2x -- the same geometry, plus two triangles the native cull drops and the 2x
// cull grid keeps -- with the incoming primitive's box [304, 226, 307, 226] at
// both scales. At 1x the buffered rect was [302, 225, 304, 227] and 307 > 304, so
// the primitive merged into the buffer. At 2x the rect had grown to
// [301, 224, 314, 227], the primitive was contained, and the kick ran
// Flush(CONTEXTCHANGE) instead -- turning both buffers into draws.
//
// Reconstructed here from geometry with those properties rather than from the
// capture: two painting triangles whose native union is [302, 225, 304, 227], and
// two sub-native ones sitting out at pixel 313 that the 2x grid keeps.
// ---------------------------------------------------------------------------
TEST(GsDrawBufferOverlap, StuntmanDraw28MergesAtBothScales)
{
	// Painting geometry. Deliberately not 16-aligned on the left, so the floor /
	// ceil half of the difference shows up as well as the culled-primitive half.
	const std::vector<Tri> buffered = {
		// native rect [303, 225, 304, 226); shipped rect above native [302, 225, 304, 226)
		{302 * 16 + 5, 225 * 16, 303 * 16 + 5, 225 * 16 + 9, 302 * 16 + 5, 225 * 16 + 9},
		// native rect [302, 226, 304, 227)
		{302 * 16, 226 * 16, 303 * 16 + 12, 226 * 16 + 11, 302 * 16, 226 * 16 + 11},
		// Two sub-native triangles out at pixel 313: they span no native sample
		// point, so the native cull drops them, and the 2x grid (step 4 sub-texels)
		// keeps them.
		{313 * 16 + 1, 225 * 16 + 1, 313 * 16 + 15, 225 * 16 + 14, 313 * 16 + 1, 225 * 16 + 14},
		{313 * 16 + 2, 226 * 16 + 1, 313 * 16 + 14, 226 * 16 + 13, 313 * 16 + 2, 226 * 16 + 13},
	};

	const BufferRects at1x = BufferedRects(buffered, kGrid1x);
	const BufferRects at2x = BufferedRects(buffered, kGrid2x);

	// The premise: the native cull drops the two sub-native triangles and the 2x
	// grid keeps them, which is why the shipped rect grows.
	EXPECT_EQ(2, at1x.accepted);
	EXPECT_EQ(4, at2x.accepted);
	EXPECT_TRUE(at1x.shipped.eq(GSVector4i(302, 225, 304, 227))) << "1x buffered rect";
	EXPECT_TRUE(at2x.shipped.eq(GSVector4i(302, 225, 314, 227))) << "2x buffered rect, shipped";

	// The fix: the rect the heuristic reads is the same at both scales.
	EXPECT_TRUE(at1x.native.eq(at2x.native)) << "native rect differs between scales";
	EXPECT_TRUE(at1x.native.eq(at1x.shipped)) << "1x native rect is the shipped rect";

	// The incoming primitive, whose box is the same at every scale by construction.
	const GSVector4i new_area(304, 226, 307, 226);

	// What shipped did: merge at 1x, flush at 2x.
	EXPECT_FALSE(Contained(new_area, at1x.shipped));
	EXPECT_TRUE(Contained(new_area, at2x.shipped));

	// What the native rect does: merge at both.
	EXPECT_FALSE(Contained(new_area, at1x.native));
	EXPECT_FALSE(Contained(new_area, at2x.native));
}

// ---------------------------------------------------------------------------
// A buffer whose every primitive is sub-native ends up with an empty native rect.
// Nothing is contained in it and nothing intersects it, which is the right answer:
// at native that draw would not exist at all, every one of its primitives having
// been culled before FlushPrim.
// ---------------------------------------------------------------------------
TEST(GsDrawBufferOverlap, AnAllSubNativeBufferContainsNothing)
{
	const std::vector<Tri> buffered = {
		{100 * 16 + 1, 100 * 16 + 1, 100 * 16 + 15, 100 * 16 + 14, 100 * 16 + 1, 100 * 16 + 14},
		{101 * 16 + 2, 100 * 16 + 3, 101 * 16 + 13, 100 * 16 + 15, 101 * 16 + 2, 100 * 16 + 15},
	};

	const BufferRects at1x = BufferedRects(buffered, kGrid1x);
	const BufferRects at2x = BufferedRects(buffered, kGrid2x);

	EXPECT_EQ(0, at1x.accepted); // no draw at all at native
	EXPECT_EQ(2, at2x.accepted);
	EXPECT_TRUE(at2x.shipped.eq(GSVector4i(100, 100, 102, 101))) << "shipped rect still covers them";
	EXPECT_TRUE(at2x.native.rempty()) << "native rect is empty";

	for (const GSVector4i& probe : {GSVector4i(100, 100, 100, 100), GSVector4i(100, 100, 101, 101),
			 GSVector4i(500, 500, 501, 501)})
	{
		EXPECT_FALSE(Contained(probe, at2x.native));
		EXPECT_TRUE(probe.rintersect(at2x.native).rempty());
	}
}
