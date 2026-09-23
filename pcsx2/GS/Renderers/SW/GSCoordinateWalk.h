// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// How the GS walks an affine texture coordinate.

// The affine accumulator has fifteen fractional bits and floors after every add.
// Our coordinate is 16.16, so the console's grid is every even 16.16 value: floor
// the seed and the per-pixel step onto it, then add. floor(integer + step) is
// integer + floor(step), so no per-add floor is needed. The difference only shows
// when the step is not a power of two per pixel.
//
// The accumulator is not blocked, does not apply to the perspective route (a
// divided coordinate keeps its exact plane), and sprites and triangles share it.
//
// Putting the seed on the same grid is a model: clearing bit 0 moves neither the
// sampled texel (bits 16 up) nor the filter weight (bits 12-15). It must be a
// floor, not truncate-toward-zero, or a negative coordinate on a boundary rounds
// the wrong way.
static constexpr int GS_UV_FRACTIONAL_BITS = 15;
static constexpr int GS_UV_GRID_SHIFT = 16 - GS_UV_FRACTIONAL_BITS;
static constexpr s32 GS_UV_GRID_MASK = ~((1 << GS_UV_GRID_SHIFT) - 1);

// The formed coordinate saturates into a signed 12.4 field: at or above 2047.9375
// texels it samples texel 2047 (weight 15 under linear), at or below -2048 it
// samples texel -2048. It does not wrap. Each axis saturates independently.
//
// The clamp applies to the formed sixteenth: after truncation to sixteenths and
// the linear filter's half-texel step, before the tap pair is split and wrap/clamp
// addressing runs. Clamping earlier reads weight 7 at the top instead of 15.
// Bits below the sixteenth are dropped; nothing downstream reads them.
//
// The UV register (10.4) cannot reach the field, so the rule is not gated on the
// route. Mip levels apply it at the same point in their own chain; that part is
// a model, and the exact clamp point is only known to within one sixteenth.
static constexpr s32 GS_COORD_SIXTEENTH_MIN = -0x8000; // -2048.0 texels
static constexpr s32 GS_COORD_SIXTEENTH_MAX = 0x7FFF;  // +2047.9375 texels
static constexpr int GS_COORD_SIXTEENTH_SHIFT = 12;

// A triangle's coordinate trails the exact plane by one 16.16 unit on each axis
// it walks forward (GSCoordinateLag.h), which moves the sample one texel down
// where the exact coordinate lands on a texel boundary. Still and backward walks
// do not trail, sprites never do, and neither does a triangle whose twice-area
// (12.4 units squared) is a power of two, because then the setup's divide by it
// is exact.

/// Twice the triangle's signed area, in 12.4 units squared, exactly.
///
/// Position lanes hold the 12.4 word divided by sixteen (GSRendererSW's
/// `s_pos_scale`), so multiplying by sixteen recovers it. Needs 64 bits: an edge
/// is 17 bits and the cross product 34.
///
/// The ARM64 setup generator computes the same integer (FCVTZS at four
/// fractional bits, cross in NEON); neither side uses floating point for it, so
/// they cannot disagree.
__forceinline static s64 GSTriangleTwiceArea(const GSVector4& p0, const GSVector4& p1, const GSVector4& p2)
{
	const s64 x0 = static_cast<s64>(p0.x * 16.0f);
	const s64 y0 = static_cast<s64>(p0.y * 16.0f);
	const s64 x1 = static_cast<s64>(p1.x * 16.0f);
	const s64 y1 = static_cast<s64>(p1.y * 16.0f);
	const s64 x2 = static_cast<s64>(p2.x * 16.0f);
	const s64 y2 = static_cast<s64>(p2.y * 16.0f);

	return (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
}

/// Whether the setup's divide by twice the area is exact -- that is, whether twice
/// the area is a power of two. A degenerate triangle is not: zero has no bit set.
__forceinline static bool GSSetupInvertsExactly(s64 twice_area)
{
	const u64 a = static_cast<u64>(twice_area < 0 ? -twice_area : twice_area);

	return a != 0 && (a & (a - 1)) == 0;
}

/// Whether an axis whose walk carries `step` (16.16 texels per pixel) trails the
/// exact plane. Forward walks trail; still and backward ones do not, and neither
/// does any axis of a triangle whose setup inverts exactly.
__forceinline static bool GSCoordinateStepTrails(s32 step, bool inverts_exactly)
{
	return step > 0 && !inverts_exactly;
}

/// One affine-route coordinate, floored onto the accumulator grid. Floor, not
/// truncate: a fixed-point register drops low bits, which is floor in two's
/// complement.
__forceinline static s32 GSAffineCoordinateOnGrid(float v)
{
	return static_cast<s32>(std::floor(v)) & GS_UV_GRID_MASK;
}

// Per-primitive coordinate grain.
//
// An affine STQ triangle's S and T are held to fourteen significant bits of the
// LARGEST of its three vertices, each vertex truncated toward zero onto that one
// grain. The gradient, formed from the truncated vertices, is pushed down onto a
// grid of grain/1024. The push is strict (a gradient exactly on a grid point takes
// the point below, since the setup's truncated reciprocal leaves the product a
// hair low) except when twice the area is a power of two and the reciprocal is
// exact.
//
// The front end already truncates each vertex onto a finer power-of-two grid, and
// truncating onto a coarser multiple of that grid gives the same result as
// truncating the raw value. So the grain can be applied here in the software
// renderer without changing GSState::Draw or the hardware renderers.
//
// Modelled rather than measured: only the per-pixel gradient is pushed; the
// strict step down applies to forward gradients only (still keeps zero, backward
// takes a plain floor); and 1024 is not distinguished from 512 or 2048.
//
// The grain reads S's exponent only: an affine STQ primitive has Q identically
// one, so Q's exponent never leads.

/// One primitive's texel-coordinate grain, per axis, as the power of two it is.
/// `exp` is log2 of the grain in 16.16 units.
struct GSCoordinateGrain
{
	s32 exp[2];
};

/// The biased exponent field of a float, which is all the grain rule reads.
__forceinline static int GSCoordinateExponentOf(float v)
{
	u32 bits;

	std::memcpy(&bits, &v, sizeof(bits));

	return static_cast<int>((bits >> 23) & 0xff);
}

/// The grain the whole primitive is held to, from the largest of its three vertices.
///
/// `floor_exp` is TEX0's log2 width (height for V) plus two: the front end's rule
/// never cuts finer than 2^-14 in S, which is 2^(TW - 14) texels. `half` is the half
/// texel the linear filter already took off at the vertex, added back here because
/// the exponent the rule keys on is the coordinate's, not the biased one's.
__forceinline static GSCoordinateGrain GSCoordinateGrainOfPrimitive(const GSVector4& t0,
	const GSVector4& t1, const GSVector4& t2, const s32 floor_exp[2], float half)
{
	GSCoordinateGrain grain;

	for (int a = 0; a < 2; a++)
	{
		const int e = std::max(std::max(GSCoordinateExponentOf(t0.F32[a] + half),
								   GSCoordinateExponentOf(t1.F32[a] + half)),
			GSCoordinateExponentOf(t2.F32[a] + half));

		// Nine mantissa bits below the largest vertex, floored at the texture's own
		// 2^(TW - 14) texels, and the front end's 23-bit clamp under that.
		grain.exp[a] = std::max(e - 141, std::min(floor_exp[a], e - 127));
	}

	return grain;
}

/// One vertex's S and T truncated toward zero onto the primitive's grain. Q and
/// fog pass through. Adding and removing the half texel is exact.
__forceinline static GSVector4 GSCoordinateOnGrain(const GSVector4& t,
	const GSCoordinateGrain& grain, float half)
{
	GSVector4 out = t;

	for (int a = 0; a < 2; a++)
	{
		const double g = std::ldexp(1.0, grain.exp[a]);
		const double u = static_cast<double>(t.F32[a]) + static_cast<double>(half);

		out.F32[a] = static_cast<float>(std::trunc(u / g) * g - static_cast<double>(half));
	}

	return out;
}

/// The ST gradient pushed down onto a grid of grain/1024, strictly where the
/// setup's divide by twice the area is inexact. Done in double so both the divide
/// and the multiply back are exact; the result fits the float it is stored in.
__forceinline static GSVector4 GSCoordinateGradientOnGrain(const GSVector4& dscan_t,
	const GSCoordinateGrain& grain, bool inverts_exactly)
{
	GSVector4 out = dscan_t;

	for (int a = 0; a < 2; a++)
	{
		const double grid = std::ldexp(1.0, grain.exp[a] - 10);
		const double q = static_cast<double>(dscan_t.F32[a]) / grid;
		double n = std::floor(q);

		if (!inverts_exactly && n == q && q > 0.0)
			n -= 1.0;

		out.F32[a] = static_cast<float>(n * grid);
	}

	return out;
}

// UV-route sprite ramp bias.
//
// A UV-route sprite whose coordinate ramps ascending along an axis whose own
// pixel extent is not a power of two samples one sixteenth of a texel low on that
// axis, from the second pixel along that axis onward. Only the ramping axis's own
// extent matters; descending ramps are exact; the first row/column is exact.
//
// So a V ramp's first scanline is unadjusted and later ones use V - 1/16; a U
// ramp's first column is unadjusted and later ones use U - 1/16. The rasterizer
// applies it (one subtract per row for V, a separate one-pixel span for U's first
// column), so both scanline backends inherit it.
//
// Under a top or left clip this exempts the sprite's own first row/column, not
// the first one the scissor leaves; which one hardware uses is unknown.
//
// In 16.16 texels a sixteenth of a texel is 4096.
static constexpr float GS_UV_RAMP_BIAS = 4096.0f;

/// How far below the plane a UV-route sprite seeds one axis, given that axis's
/// per-pixel step and its own extent in pixels.
///
/// Applies only when the step is a whole number of sixteenths of a texel (an
/// exact multiple of 4096 in 16.16); other steps are exact.
///
/// Likely mechanism: the per-pixel step is span/extent truncated slightly low, so
/// the walk runs a hair behind the exact line. That only shows where the exact
/// coordinate lands on a sixteenth boundary, i.e. when the step is a whole number
/// of sixteenths.
__forceinline static float GSSpriteRampBias(float step, int extent)
{
	const bool dyadic = extent > 0 && (extent & (extent - 1)) == 0;

	const s32 istep = static_cast<s32>(std::floor(step));
	const bool whole_ulp = istep > 0 && (istep & (static_cast<s32>(GS_UV_RAMP_BIAS) - 1)) == 0;

	return (whole_ulp && !dyadic) ? GS_UV_RAMP_BIAS : 0.0f;
}
