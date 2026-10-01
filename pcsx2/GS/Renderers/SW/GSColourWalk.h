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
// Five rules:
//
//   1. THE GRADIENT IS TEN BITS.  The setup's product (channel delta times the
//      cross product's eight-bit truncated reciprocal) is truncated toward zero
//      to 1/8 of a unit, ten fractional bits of a colour level.
//
//   2. THE WALK INSIDE A BLOCK IS COARSER.  It ramps by gc, the gradient
//      truncated toward zero to a multiple of eight units, and makes up the
//      difference in one jump of dw = W*(g - gc).  W ramps plus one jump is
//      exactly W*g, so a whole block steps the true gradient.
//
//   2b. THE BLOCK IS EIGHT PIXELS WIDE ONLY WHEN TEXTURING, FOG AND AA1 ARE ALL
//      OFF, else four.  The three do not compound, and blending, depth and
//      target format do not affect it.  The width belongs to the DDA, not the
//      texture unit (TCC=0 still narrows the alpha channel).  Fog takes the same
//      width as colour.  See GSBlockWalk.h.
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
//      pixel walking right, down to an odd one walking left -- the same rounding
//      at either width.  A = S + 2d is the pixel where the value is the plane
//      exactly, also at either width, and phase is S's position in the W-pixel
//      period.
//
//   5. THE ROWS COME IN ABSOLUTE PAIRS.  A row's plane is evaluated at the even
//      row of its pair when the anchor is the top of the spine and at the odd
//      row when it is the bottom; the other row of the pair is the first plus
//      one coarse vertical step gyc.
//
// So a pixel is
//
//     V(x, y) = P(yf) + (y - yf)*gyc + (x - A)*gc + d*dw*j
//     P(yf)   = aR + tz8(g*(A - xR)) + tz8(gy*(yf - yR))
//     j       = floor(d*(x - S) / W)
//
// with tz8 truncating toward zero to 1/8 of a unit and yf the pair's own row.
// The vertical tz8 is assumed by symmetry with the horizontal one.
//
// For the scanline: take an absolute base xv that is a multiple of eight (so a
// multiple of W at either width) and a lane i measured from it:
//
//     V(xv + i) = base(xv) + off[i],  off[i] = i*gc + dw*floor((i - phase) / W)
//     base(xv + 8) = base(xv) + 8g,  exactly, at either width
//
// So one eight-entry table serves both widths: eight pixels is one block or two
// and advances by 8g either way.  At W = 4 the offsets repeat with period four,
// so indexing by `left & 7` reads the same as `left & 3`.  The scanline does not
// need to know the width.
//
// Derivation of phase: with d = +1 and r = S mod W, the block index at x is
// j(x) = floor((x - S) / W), so between the base and lane i
//
//     j(xv + i) - j(xv) = floor((i - r)/W) - floor(-r/W)
//
// and the second term is a constant that cancels in every difference the tables
// hold.  With d = -1, j(x) = floor((S - x)/W) and the same subtraction gives
// -floor((r - i)/W) = floor((i - (r+1))/W) + 1, a constant again, so phase is
// r + 1 there.  Hence one expression for both directions, with
// phase = (d > 0 ? S : S + 1) & (W - 1).
//
// So the walk is "seed, add a lane table, add a per-vector step".  The step is
// constant when a vector is a whole number of blocks and alternates when a block
// is two vectors (four-lane host at W = 8; see GSBlockWalk.h).
// GSDrawScanline::SetupColourWalkTables builds the tables.

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
	GSVector4 dw;  ///< W*(g - gc), a whole number of units
	GSVector4 g8;  ///< 8*g, what EIGHT pixels advance by -- one block or two
	GSVector4 pa;  ///< aR + tz8(g*(A - xR)) -- the plane's constant part
	int w;         ///< this lane's block width in pixels
	int wshift;    ///< log2(w), so the block index is a shift
	int phase;     ///< S's position in the w-pixel period, 0..w-1
};

/// One primitive's colour interpolator, set up once and read by the row seed and
/// the table builder. Lives in GSScanlineLocalData so tests can inspect it.
struct GSColourWalk
{
	GSColourWalkGradient c;
	GSColourWalkGradient f;
	float xr;        ///< the anchor vertex
	float yr;
	int S;           ///< the block grid's origin pixel
	int A;           ///< the pixel where the value is the plane exactly
	int d;           ///< +1 walking right, -1 walking left
	int top_anchor;  ///< the anchor is the spine's TOP end
	int live;        ///< this primitive walks a gradient at all

	/// Which row's tables GSDrawScanline::SetupColourWalkTables last built, so a
	/// row wanting the same tables skips the rebuild. The tables depend only on
	/// this walk and the row's fractional part, so rows with equal fractions share
	/// them.
	///
	/// Kept inside the walk so clearing the walk invalidates it.
	/// GSRasterizer::SetupPrim marks it stale for every primitive with a walk.
	/// A walkless primitive keeps GSColourWalkTablesZero from the one before.
	struct
	{
		GSVector4 cfrac; ///< the colour fraction those tables were built from
		GSVector4 ffrac; ///< the fog fraction, likewise
		int state;       ///< GSColourWalkTablesState
	} tables;
};

/// What the tables named by GSColourWalk::tables currently hold.
enum GSColourWalkTablesState : int
{
	GSColourWalkTablesStale = 0, ///< a new primitive; nothing there can be reused
	GSColourWalkTablesZero,      ///< the tables hold the zeros a walkless primitive wants
	GSColourWalkTablesBuilt,     ///< the tables hold the row whose fractions are recorded
};

__forceinline static void GSColourWalkGradientInit(GSColourWalkGradient& out,
	const GSVector4& g, const GSVector4& gy, const GSVector4& a, const GSVector4& ax, int w,
	int S, int d)
{
	out.g = g;
	out.gy = gy;
	out.gc = GSColourWalkTruncBlock(g);
	out.gyc = GSColourWalkTruncBlock(gy);
	out.dw = (g - out.gc) * GSVector4(static_cast<float>(w));
	out.g8 = g * GSVector4::cxpr(8.0f);
	out.pa = a + GSColourWalkTruncUnit(g * ax);
	out.w = w;
	out.wshift = (w == 8) ? 3 : 2;
	out.phase = ((d > 0) ? S : (S + 1)) & (w - 1);
}

/// Derive one triangle's walk. v0, v1, v2 are the setup's y-sorted vertices;
/// dscan and dedge carry the gradients already truncated to the 1/8-unit grid.
/// `w` is the block width, shared by colour and fog (fog uses the same
/// interpolator).
__forceinline static void GSSetupColourWalk(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& v2,
	const GSVertexSW& dscan, const GSVertexSW& dedge, int w, GSColourWalk& out)
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

	// The left-walking origin is (ceil(xR) - 1) | 1, not floor(xR) | 1. They
	// differ by two pixels when xR is a whole number; this one matches the console.
	out.S = (out.d > 0) ? (static_cast<int>(std::ceil(out.xr)) & ~1)
	                    : ((static_cast<int>(std::ceil(out.xr)) - 1) | 1);
	out.A = out.S + 2 * out.d;

	const GSVector4 ax = GSVector4(static_cast<float>(out.A) - out.xr);

	GSColourWalkGradientInit(out.c, dscan.c, dedge.c, r->c, ax, w, out.S, out.d);
	GSColourWalkGradientInit(out.f, dscan.t.wwww(), dedge.t.wwww(), r->t.wwww(), ax, w, out.S, out.d);

	out.live = 1;
}

/// The value at (x, y), floored to the colour unit so that the scanline's own
/// float-to-int conversion cannot disagree with it on a negative fraction.
///
/// The ramp and the block jump go into ONE floor. SetupColourWalkTables builds
/// per-row tables that reproduce that single floor at every pixel. Flooring the
/// seed and adding the jump afterwards gives different results wherever dw is
/// not whole (every four-wide block).
__forceinline static GSVector4 GSColourWalkRowSeed(const GSColourWalk& w, const GSColourWalkGradient& a, int x, int y)
{
	const int yf = w.top_anchor ? (y & ~1) : (y | 1);
	// Floor division by the block width. d*(x - S) is non-negative inside the
	// primitive; the shift keeps it a floor if that ever changes.
	const int j = (w.d * (x - w.S)) >> a.wshift;

	const GSVector4 p = a.pa + GSColourWalkTruncUnit(a.gy * GSVector4(static_cast<float>(yf) - w.yr));

	return (p + a.gyc * GSVector4(static_cast<float>(y - yf))
	          + a.gc * GSVector4(static_cast<float>(x - w.A))
	          + a.dw * GSVector4(static_cast<float>(w.d * j)))
	    .floor();
}
