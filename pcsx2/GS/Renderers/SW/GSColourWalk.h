// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <cmath>

// How the GS interpolates a gouraud colour across a triangle.
//
// Everything here is in the pipeline's own colour unit -- one unit is 1/128 of a
// colour level, which is the grid vertex colours arrive on (GSRendererSW builds
// them as byte << 7).  Fog rides the same interpolator in the same unit, carried
// in t.w, and takes every rule below unchanged.
//
// Measured on real hardware.  Five properties, each fitted on the geometry that
// isolates it and then scored together:
//
//   1. THE GRADIENT IS TEN BITS.  The setup's own product -- a channel delta
//      times the cross product's eight-bit truncated reciprocal -- is then
//      truncated toward zero to 1/8 of a unit, which is ten fractional bits of a
//      colour level.
//
//   2. THE WALK INSIDE A BLOCK IS COARSER STILL.  It ramps by gc, the gradient
//      truncated toward zero to a multiple of eight units (a sixteenth of a
//      level), and makes the difference up in one jump of d8 = 8*(g - gc) -- a
//      whole number of units, always inside (-64, 64).  Eight of those ramps and
//      one jump is exactly 8g, so a whole block still steps the true gradient.
//
//   3. THE PRIMITIVE HAS ONE ANCHOR, not one per section.  Order the vertices
//      the hardware's way: top is the smallest y, ties to the smaller x; bottom
//      is the largest y, ties again to the smaller x; the third is the middle.
//      The spine runs top to bottom.  The walk goes right when the middle vertex
//      is right of the spine at its own y, left otherwise.  The anchor is the
//      spine end furthest AGAINST the walk -- the smaller-x end walking right,
//      the larger-x end walking left, ties to the top.
//
//   4. THE BLOCKS ARE PINNED TO ABSOLUTE X, per primitive, unchanged row to row.
//      S is the anchor's x rounded to the grid the walk starts on: up to an even
//      pixel walking right, down to an odd one walking left.  A = S + 2d is the
//      pixel where the value is the plane exactly, and phase is S's position in
//      the eight-pixel period.  Period eight; sixteen fits nothing.
//
//   5. THE ROWS COME IN ABSOLUTE PAIRS.  A row's plane is evaluated at the even
//      row of its pair when the anchor is the top of the spine and at the odd
//      row when it is the bottom; the other row of the pair is the first plus
//      one coarse vertical step gyc.
//
// So a pixel is
//
//     V(x, y) = P(yf) + (y - yf)*gyc + (x - A)*gc + d*d8*j
//     P(yf)   = aR + tz8(g*(A - xR)) + tz8(gy*(yf - yR))
//     j       = floor(d*(x - S) / 8)
//
// with tz8 truncating toward zero to 1/8 of a unit and yf the pair's own row.
// The horizontal tz8 is measured -- a half-pixel anchor separates toward-zero
// from exact, from floor and from round-half-up.  The vertical one is the same
// operation by symmetry: every vertex y we could measure is a whole number, so it
// is not separately pinned.
//
/// Truncate toward zero to 1/8 of a colour unit -- rule 1's grid.
__forceinline static GSVector4 GSColourWalkTruncUnit(const GSVector4& v)
{
	return GSVector4(GSVector4i(v * GSVector4::cxpr(8.0f))) * GSVector4::cxpr(0.125f);
}

/// Truncate toward zero to a multiple of eight colour units -- rule 2's grid.
__forceinline static GSVector4 GSColourWalkTruncBlock(const GSVector4& v)
{
	return GSVector4(GSVector4i(v * GSVector4::cxpr(0.125f))) * GSVector4::cxpr(8.0f);
}

/// One attribute's gradients, ready for the walk. Colour keeps r, g, b and a one
/// per lane; fog keeps its single channel broadcast into all four, so the two
/// take the same code everywhere.
struct GSColourWalkGradient
{
	GSVector4 g;   ///< the pixel gradient, on the 1/8-unit grid
	GSVector4 gy;  ///< the row gradient, on the same grid
	GSVector4 gc;  ///< g on the eight-unit grid: the ramp inside a block
	GSVector4 gyc; ///< gy on the eight-unit grid: the step to a pair's second row
	GSVector4 d8;  ///< 8*(g - gc), a whole number of units in (-64, 64)
	GSVector4 g8;  ///< 8*g, the whole-block step, a whole number of units
	GSVector4 pa;  ///< aR + tz8(g*(A - xR)) -- the plane's constant part
};

/// One primitive's colour interpolator, decided once by the setup and read by
/// the row seed and by the table builder. It lives in GSScanlineLocalData so
/// that a test's setup_prim hook can read the decision.
struct GSColourWalk
{
	GSColourWalkGradient c;
	GSColourWalkGradient f;
	float xr;        ///< the anchor vertex
	float yr;
	int S;           ///< the block grid's origin pixel
	int A;           ///< the pixel where the value is the plane exactly
	int phase;       ///< S's position in the eight-pixel period, 0..7
	int d;           ///< +1 walking right, -1 walking left
	int top_anchor;  ///< the anchor is the spine's TOP end
	int live;        ///< this primitive walks a gradient at all
};

__forceinline static void GSColourWalkGradientInit(GSColourWalkGradient& out,
	const GSVector4& g, const GSVector4& gy, const GSVector4& a, const GSVector4& ax)
{
	out.g = g;
	out.gy = gy;
	out.gc = GSColourWalkTruncBlock(g);
	out.gyc = GSColourWalkTruncBlock(gy);
	out.d8 = (g - out.gc) * GSVector4::cxpr(8.0f);
	out.g8 = g * GSVector4::cxpr(8.0f);
	out.pa = a + GSColourWalkTruncUnit(g * ax);
}

/// Derive one triangle's walk. v0, v1, v2 are the setup's y-sorted vertices;
/// dscan and dedge carry the gradients already truncated to the 1/8-unit grid.
__forceinline static void GSSetupColourWalk(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& v2,
	const GSVertexSW& dscan, const GSVertexSW& dedge, GSColourWalk& out)
{
	// Rule 3's vertex order. The setup has already sorted by y, so only a flat
	// edge can disagree with it, and there the hardware takes the LEFT vertex as
	// the spine's end.
	const GSVertexSW* t = &v0;
	const GSVertexSW* m = &v1;
	const GSVertexSW* b = &v2;

	if (v0.p.y == v1.p.y)
	{
		if (v1.p.x < v0.p.x)
		{
			t = &v1;
			m = &v0;
		}
	}
	else if (v1.p.y == v2.p.y)
	{
		if (v1.p.x < v2.p.x)
		{
			b = &v1;
			m = &v2;
		}
	}

	// Which side of the spine the middle vertex falls on, at its own row.
	const float xspine = t->p.x + (m->p.y - t->p.y) * (b->p.x - t->p.x) / (b->p.y - t->p.y);
	out.d = (m->p.x > xspine) ? 1 : -1;

	// The anchor is the spine end furthest against the walk; a tie goes to the top.
	const GSVertexSW* r = (out.d > 0) ? ((t->p.x <= b->p.x) ? t : b) : ((t->p.x >= b->p.x) ? t : b);

	out.top_anchor = (r == t) ? 1 : 0;
	out.xr = r->p.x;
	out.yr = r->p.y;

	out.S = (out.d > 0) ? (static_cast<int>(std::ceil(out.xr)) & ~1)
	                    : (static_cast<int>(std::floor(out.xr)) | 1);
	out.A = out.S + 2 * out.d;
	out.phase = ((out.d > 0) ? out.S : (out.S + 1)) & 7;

	const GSVector4 ax = GSVector4(static_cast<float>(out.A) - out.xr);

	GSColourWalkGradientInit(out.c, dscan.c, dedge.c, r->c, ax);
	GSColourWalkGradientInit(out.f, dscan.t.wwww(), dedge.t.wwww(), r->t.wwww(), ax);

	out.live = 1;
}

/// The value at (x, y), floored to the colour unit so that the scanline's own
/// float-to-int conversion cannot disagree with it on a negative fraction.
__forceinline static GSVector4 GSColourWalkRowSeed(const GSColourWalk& w, const GSColourWalkGradient& a, int x, int y)
{
	const int yf = w.top_anchor ? (y & ~1) : (y | 1);
	// Floor division by the block width. The anchor is the spine end the walk
	// runs away from, so d*(x - S) is never negative inside the primitive; the
	// shift is written rather than a divide so that it stays a floor if it ever is.
	const int j = (w.d * (x - w.S)) >> 3;

	const GSVector4 p = a.pa + GSColourWalkTruncUnit(a.gy * GSVector4(static_cast<float>(yf) - w.yr));

	return (p + a.gyc * GSVector4(static_cast<float>(y - yf))
	          + a.gc * GSVector4(static_cast<float>(x - w.A))
	          + a.d8 * GSVector4(static_cast<float>(w.d * j)))
	    .floor();
}
