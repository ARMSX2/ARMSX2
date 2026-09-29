// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"
#include "GS/Renderers/SW/GSCoordinateWalk.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

// How the GS builds the S, T and Q planes of a perspective triangle (STQ, FST = 0).
//
// Measured on an SCPH-30001 (gs-sm3b, gs-sm3c, gs-sm3d, gs-tri1a-f): the console's
// plane is not the exact plane through the three vertices. The arithmetic below
// reproduces every plane those captures read, 100% of the pixels of gs-sm3d's
// 62 single-triangle arms and of its 56 predicted ones.
//
//   0. A vertex with a negative Q is negated whole: the setup works on Q's magnitude
//      and carries Q's sign in S and T, so (S, T, Q) becomes (-S, -T, |Q|) and S/Q is
//      unchanged. Found by trying it, not read off a capture (see below).
//   1. Each of the nine inputs (S, T and Q at three vertices) loses its low eight
//      mantissa bits, toward zero. The register keeps fifteen.
//   2. One exponent E = floor(log2 max|v|) over all nine values, so S, T and Q
//      share it. Every value is floored onto the grid g = 2^(E-14) and is from
//      then on an integer count of g.
//   3. The anchor is the vertex the colour walk anchors on (GSColourWalk.h): the
//      spine end furthest against the walk. The plane passes through the anchor's
//      quantised value and no other vertex's.
//   4. The gradients are the exact gradients of the plane through the quantised
//      vertices, formed with a reciprocal of twice the area that is a hair short
//      of exact (GSPerspectiveGradient), then truncated toward zero onto g/1024.
//   5. A pixel's value is anchor + gx*dx + gy*dy at the pixel's integer position
//      and the vertices' exact sub-pixel positions, floored to g/4.
//   6. The texture coordinate is that S over that Q, truncated toward zero to a
//      sixteenth of a texel, which the scanline already does. There is no
//      coordinate lag on this route (GSCoordinateWalk.h): the plane carries the
//      shortfall it stands in for, and gs-sm3d's whole-draw arms read 100.0000%
//      of 771,234 pixels without it and 92.3% with it.
//
// Everything is integer arithmetic. The accumulator's unit is F = g / 2^14, which
// is a gradient unit (g/1024) per sixteenth of a pixel, so
//
//     value(x, y) = (anchor << 14) + gx*(16x - xa) + gy*(16y - ya)
//
// is exact for a pixel (x, y) and a 12.4 anchor position (xa, ya), and a per-pixel
// step is 16*gx. The floor to g/4 is a clear of the low twelve bits of the value.
// Only the low 32 bits are carried: a pixel inside the triangle has a value inside
// +-2^30, so wrapped arithmetic gives the right answer wherever it is read.
//
// Rule 0 was not measured directly. Without it the 53 triangles of the game's draw
// with a vertex Q at or below zero (5.1% of its pixels, the "sliver" class the model
// could not explain) agree with the console on 12% of their coordinates; with it, 91%
// on U and 99% on V (gs-sm3b's `bil` arm). OutRun 2006's frame has every Q negative on
// 3,499 of its 3,539 perspective draws: exact-word identity with the console is 80.1%
// on the exact plane, 75.5% on a plane built on the raw negative Q, and 98.0% with the
// rule. gs-tri1e (Q swept through zero) would test it directly.
//
// Modelled rather than measured:
//   * The power-of-two exemption. A twice-area that is a power of two takes an
//     exact reciprocal (GSSetupInvertsExactly). The table-and-Newton form below
//     gives 1 - 3.8e-6 at a mantissa of exactly one, so the two have not been
//     reconciled; the exemption is measured for the affine route only.
//   * The anchor at a vertical long edge, where the colour walk's rule and
//     gs-sm3d's empirical one differ (top against bottom). No capture separates
//     them: on the game's draw 62 of 250 covered triangles have one and score
//     identically either way.
//   * The mantissa cut for values the front end has already cut (GSState.cpp does
//     so under constant Z), which is a finer grid and so changes nothing for a
//     positive value.
//   * A vertex with Q exactly zero, which has no sign to carry and no quotient.
//
// The divide is not in this file. The scanline still multiplies by a truncated
// reciprocal of Q.

static constexpr int GS_PLANE_GRID_BITS = 14;     ///< g = 2^(E - 14)
static constexpr int GS_PLANE_GRADIENT_BITS = 10; ///< a gradient is a multiple of g / 2^10
static constexpr int GS_PLANE_POSITION_BITS = 4;  ///< vertex positions are 12.4
/// The accumulator's unit F = g / 2^14: one gradient unit per sixteenth of a pixel.
static constexpr int GS_PLANE_ACCUM_BITS = GS_PLANE_GRADIENT_BITS + GS_PLANE_POSITION_BITS;
/// g/4 in accumulator units is 2^12: the per-pixel floor clears this many low bits.
static constexpr int GS_PLANE_PIXEL_SHIFT = GS_PLANE_ACCUM_BITS - 2;

/// One primitive's S, T and Q planes, indexed [0] = S, [1] = T, [2] = Q.
struct GSPerspectivePlane
{
	/// False for a primitive the rules cannot express (no non-zero finite value,
	/// or no area). Every field below is then zero.
	bool valid;
	s32 exp;       ///< E, the shared block exponent, unbiased
	s32 anchor;    ///< the anchor vertex, as an index into the vertices given
	s32 xa, ya;    ///< the anchor's position, 12.4
	s32 value[3];  ///< the anchor's value in units of g
	s64 gx[3];     ///< per pixel, in units of g / 2^10
	s64 gy[3];     ///< per row, in units of g / 2^10
};

/// A float with its low eight mantissa bits cleared: fifteen kept, magnitude cut
/// toward zero.
__forceinline static float GSPlaneCutMantissa(float v)
{
	u32 bits;

	std::memcpy(&bits, &v, sizeof(bits));
	bits &= 0xffffff00u;
	std::memcpy(&v, &bits, sizeof(v));

	return v;
}

/// One value as an integer count of the grid 2^(exp - 14), floored, as a
/// fixed-point register drops low bits. `exp` is at or above the value's own
/// exponent, so the count is the 24-bit significand shifted right by at least
/// 23 - 14 bits: exact, with no floating point. A negative value floors away from zero.
__forceinline static s32 GSPlaneOnGrid(float v, int exp)
{
	u32 bits;

	std::memcpy(&bits, &v, sizeof(bits));

	const u32 field = (bits >> 23) & 0xff;

	// Zero and denormals are below every grid the exponent can name.
	if (field == 0)
		return 0;

	const u32 significand = (bits & 0x7fffffu) | 0x800000u;
	const int shift = 23 - GS_PLANE_GRID_BITS + exp - (static_cast<int>(field) - 127);
	const bool negative = (bits >> 31) != 0;

	if (shift >= 32)
		return negative ? -1 : 0;

	if (!negative)
		return static_cast<s32>(significand >> shift);

	// floor(-x) = -ceil(x)
	return -static_cast<s32>((significand + ((1u << shift) - 1)) >> shift);
}

/// Which vertex anchors the plane, by the colour walk's rule (GSColourWalk.h) in
/// exact integers. Order the vertices top (smallest y, ties to the smaller x) and
/// bottom (largest y, ties to the smaller x); the third is the middle. The walk
/// goes right when the middle vertex is right of the top-to-bottom spine, left
/// otherwise; the anchor is the spine end furthest against the walk, and a tie goes
/// to the top. `x` and `y` are 12.4.
///
/// The colour walk forms the spine's x in float; this decides the same question
/// with the cross product, so a middle vertex that is only just off the spine is
/// not at the mercy of a rounding.
__forceinline static int GSPerspectiveAnchor(const s32 x[3], const s32 y[3])
{
	int top = 0;
	int bottom = 0;

	for (int i = 1; i < 3; i++)
	{
		if (y[i] < y[top] || (y[i] == y[top] && x[i] < x[top]))
			top = i;
		if (y[i] > y[bottom] || (y[i] == y[bottom] && x[i] < x[bottom]))
			bottom = i;
	}

	// One row of vertices has no spine and no area either.
	if (top == bottom)
		return 0;

	const int mid = 3 - top - bottom;

	// Positive when the middle vertex is right of the spine at its own row. The
	// spine runs downward, so the sign of the cross product is the side.
	const s64 side = static_cast<s64>(x[mid] - x[top]) * (y[bottom] - y[top])
	                 - static_cast<s64>(y[mid] - y[top]) * (x[bottom] - x[top]);
	const bool right = side > 0;

	if (right)
		return (x[top] <= x[bottom]) ? top : bottom;

	return (x[top] >= x[bottom]) ? top : bottom;
}

// A 128-bit unsigned value, just enough for the gradient's product. The tree
// builds with compilers that have no native one.
struct GSPlaneU128
{
	u64 lo;
	u64 hi;
};

__forceinline static GSPlaneU128 GSPlaneMul(u64 a, u64 b)
{
	const u64 a0 = a & 0xffffffffull;
	const u64 a1 = a >> 32;
	const u64 b0 = b & 0xffffffffull;
	const u64 b1 = b >> 32;
	const u64 p00 = a0 * b0;
	const u64 p01 = a0 * b1;
	const u64 p10 = a1 * b0;
	const u64 p11 = a1 * b1;
	const u64 mid = (p00 >> 32) + (p01 & 0xffffffffull) + (p10 & 0xffffffffull);

	GSPlaneU128 r;

	r.lo = (p00 & 0xffffffffull) | (mid << 32);
	r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);

	return r;
}

/// x << n for 0 < n < 64.
__forceinline static GSPlaneU128 GSPlaneShiftLeft(GSPlaneU128 x, int n)
{
	x.hi = (x.hi << n) | (x.lo >> (64 - n));
	x.lo <<= n;

	return x;
}

/// floor(x / d) for d below 2^32, in place.
__forceinline static void GSPlaneDivide(GSPlaneU128& x, u32 d)
{
	const u64 limb[4] = {x.hi >> 32, x.hi & 0xffffffffull, x.lo >> 32, x.lo & 0xffffffffull};
	u64 quotient[4];
	u64 rem = 0;

	for (int i = 0; i < 4; i++)
	{
		const u64 cur = (rem << 32) | limb[i];

		quotient[i] = cur / d;
		rem = cur % d;
	}

	x.hi = (quotient[0] << 32) | quotient[1];
	x.lo = (quotient[2] << 32) | quotient[3];
}

/// The low 64 bits of x >> n, for 0 <= n < 128.
__forceinline static u64 GSPlaneShiftRight(const GSPlaneU128& x, int n)
{
	if (n == 0)
		return x.lo;
	if (n < 64)
		return (x.lo >> n) | (x.hi << (64 - n));

	return x.hi >> (n - 64);
}

/// ★ THE RECIPROCAL, ISOLATED. One function forms every plane gradient, so this is
/// the only place the setup's divide lives.
///
/// `numerator` is the plane's exact numerator, in units of g times a sixteenth of a
/// pixel, and `cross` is twice the triangle's area, in 12.4 squared (the integer
/// GSTriangleTwiceArea forms). The result is numerator / cross, in units of g/2^10
/// per pixel, truncated toward zero.
///
/// The console does not divide by the area. It multiplies by a reciprocal built
/// from a 256-entry table and one Newton-Raphson step (gs-sm3d). Write the area as
/// D = m * 2^k with the mantissa m in [1, 2); the seed R0 is the reciprocal of the
/// middle of the 2^-8 wide bin m falls in, R0 = 1 / (1 + (idx + 1/2) / 256) with
/// idx = floor((m - 1) * 256); the step is R1 = R0 * (2 - m * R0); and R = R1 / 2^k.
/// R1 is always a hair below 1 / D: R1 * m = 1 - (1 - m * R0)^2. The shortfall is
/// 2.28e-6 on gs-tri1f's triangle (D = 574,798,354), inside the (2.259e-6,
/// 2.376e-6] its phase captures bound, where a reciprocal truncated to eighteen
/// bits gives 2.9e-6.
///
/// In integers, with c = |cross| = m * 2^L (L = its top bit) and a = 513 + 2*idx:
///
///     R1 / 2^k = 2^17 * W / (a^2 * 2^(2L)),   W = 2*a*2^L - 512*c
///
/// so the gradient is |numerator| * W * 2^23 / (a^2 * 2^(2L)), floored. Nothing is
/// rounded on the way: a product landing exactly on a grid point takes the point
/// below it only if the reciprocal really is short of exact.
///
/// A twice-area that is a power of two takes the exact quotient instead
/// (GSSetupInvertsExactly): the affine route's console measurements say so, and
/// the seed above would give a mantissa of one a shortfall of 3.8e-6. That
/// exemption and this form have not been reconciled; a console test with
/// power-of-two areas is pending.
__forceinline static s64 GSPerspectiveGradient(s64 numerator, s64 cross)
{
	if (numerator == 0 || cross == 0)
		return 0;

	const bool negative = (numerator < 0) != (cross < 0);
	const u64 n = static_cast<u64>(numerator < 0 ? -numerator : numerator);
	const u64 c = static_cast<u64>(cross < 0 ? -cross : cross);
	u64 magnitude;

	if (GSSetupInvertsExactly(cross))
	{
		magnitude = (n << GS_PLANE_ACCUM_BITS) / c;
	}
	else
	{
		const int top = std::bit_width(c) - 1;
		const u64 idx = ((c - (static_cast<u64>(1) << top)) << 8) >> top;
		const u64 a = 513 + 2 * idx;
		const u64 w = 2 * a * (static_cast<u64>(1) << top) - 512 * c;

		GSPlaneU128 k = GSPlaneShiftLeft(GSPlaneMul(n, w), 23);

		// floor(floor(k / a) / a) = floor(k / a^2), and a^2 is below 2^20.
		GSPlaneDivide(k, static_cast<u32>(a * a));

		magnitude = GSPlaneShiftRight(k, 2 * top);
	}

	return negative ? -static_cast<s64>(magnitude) : static_cast<s64>(magnitude);
}

/// Builds one primitive's planes. `x` and `y` are the vertices' 12.4 positions with
/// XYOFFSET already taken off; `s`, `t` and `q` are the GIF's own floats, not
/// scaled by the texture size. The vertices may come in any order.
__forceinline static void GSPerspectivePlaneSetup(const s32 x[3], const s32 y[3],
	const float s[3], const float t[3], const float q[3], GSPerspectivePlane& out)
{
	std::memset(&out, 0, sizeof(out));

	const float* in[3] = {s, t, q};
	float cut[3][3];
	u32 largest = 0;

	for (int k = 0; k < 3; k++)
	{
		for (int i = 0; i < 3; i++)
		{
			// Rule 0. Negating is exact and the cut below is symmetric in sign, so the
			// order of the two does not matter.
			const float value = (q[i] < 0.0f) ? -in[k][i] : in[k][i];

			cut[k][i] = GSPlaneCutMantissa(value);

			u32 bits;

			std::memcpy(&bits, &cut[k][i], sizeof(bits));
			largest = std::max(largest, bits & 0x7fffffffu);
		}
	}

	// All zero (or denormal), or an infinity or NaN, has no exponent to share.
	const u32 field = largest >> 23;

	if (field == 0 || field == 0xff)
		return;

	const int anchor = GSPerspectiveAnchor(x, y);
	const int i1 = (anchor == 0) ? 1 : 0;
	const int i2 = (anchor == 2) ? 1 : 2;

	const s64 ax = x[i1] - x[anchor];
	const s64 ay = y[i1] - y[anchor];
	const s64 bx = x[i2] - x[anchor];
	const s64 by = y[i2] - y[anchor];
	const s64 cross = ax * by - ay * bx;

	if (cross == 0)
		return;

	out.valid = true;
	out.exp = static_cast<s32>(field) - 127;
	out.anchor = anchor;
	out.xa = x[anchor];
	out.ya = y[anchor];

	for (int k = 0; k < 3; k++)
	{
		s32 n[3];

		for (int i = 0; i < 3; i++)
			n[i] = GSPlaneOnGrid(cut[k][i], out.exp);

		const s64 d1 = n[i1] - n[anchor];
		const s64 d2 = n[i2] - n[anchor];

		out.value[k] = n[anchor];
		out.gx[k] = GSPerspectiveGradient(d1 * by - d2 * ay, cross);
		out.gy[k] = GSPerspectiveGradient(ax * d2 - bx * d1, cross);
	}
}

/// The accumulator for attribute `k` at pixel (x, y), not yet floored, in units of
/// g / 2^14. Wraps at 32 bits by design (see the top of the file).
__forceinline static s32 GSPerspectivePlaneValue(const GSPerspectivePlane& p, int k, int x, int y)
{
	const u64 v = (static_cast<u64>(static_cast<s64>(p.value[k])) << GS_PLANE_ACCUM_BITS)
	              + static_cast<u64>(p.gx[k]) * static_cast<u64>(static_cast<s64>(16 * x - p.xa))
	              + static_cast<u64>(p.gy[k]) * static_cast<u64>(static_cast<s64>(16 * y - p.ya));

	return static_cast<s32>(static_cast<u32>(v));
}

/// What one pixel step adds to the accumulator: sixteen positions of a gradient
/// unit. Wraps like the value does.
__forceinline static s32 GSPerspectivePlaneStep(const GSPerspectivePlane& p, int k)
{
	return static_cast<s32>(static_cast<u32>(static_cast<u64>(p.gx[k]) << GS_PLANE_POSITION_BITS));
}

/// The accumulator floored to g/4, as a count of g/4. The scanline forms this per
/// pixel and per attribute before the divide.
__forceinline static s32 GSPerspectivePlanePixel(s32 accumulator)
{
	return accumulator >> GS_PLANE_PIXEL_SHIFT;
}

// The per-pixel reciprocal of Q.
//
// With the plane rule fixing S, T and Q, the console's per-pixel 1/Q is a function of
// Q alone (gs-sm3c `score`: 603,826 readings, 2,497 distinct Q, no two disagreeing),
// on a grid of fifteen bits below its leading bit. A fourteen-bit grid is excluded.
// The rounding is not truncation: r = floor(x + 0.7) on that grid, x = 1/Q in grid
// units, with 0.7 fitted on half of the game's triangles and held on the other half
// (97.85% of gs-sm3b's `bil` arm, against 94.1% for truncating fourteen bits). It is
// a description, not a derived mechanism.
//
// On a float32 quotient the grid is 2^8 units of the mantissa's last bit, and 0.7 of
// it is 179: add that to the bits and clear the low eight, so a carry out of the
// mantissa rounds up to the next power of two, as it should. ARM64 only, with the
// generators that read it.
static constexpr int GS_RECIP_GRID_SHIFT = 8;
static constexpr u32 GS_RECIP_ROUND_UP = 179;

/// One primitive's plane and the two scales the scanline reads. Lives in
/// GSScanlineLocalData so the setup, the rasterizer's row seeds and the scanline
/// see one copy, and so tests can inspect it.
struct GSPerspectivePlaneWalk
{
	GSPerspectivePlane plane;

	/// 2^(16 + TW), 2^(16 + TH): the counts of g/4 divide, so S/Q is exact in them,
	/// and the coordinate is in 16.16 texels once these are multiplied in.
	GSVector4 stscale;

	/// 2^(E - 16): a count of g/4 as the float Q the level of detail and the
	/// filter crossover read.
	GSVector4 qscale;
};
