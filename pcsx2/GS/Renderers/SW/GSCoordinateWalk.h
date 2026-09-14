// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// How the GS walks an affine texture coordinate, and the shapes the walk is held
// in. Everything here was measured on real hardware; each block says what the
// rule is, what relies on it, and where it is a model rather than a reading.

// THE ACCUMULATOR IS 12.15, AND IT FLOORS AFTER EVERY ADD.
//
// The UV register is 12.4, but the walk is not at that width and it is not exact:
// the console accumulates fifteen fractional bits of a texel and drops what falls
// below them at every per-pixel add. Our coordinate is 16.16, so a value on the
// console's grid is a multiple of two of our units, and the model is one line --
// floor the seed and the per-pixel step onto an even 16.16 value, then add.
// Flooring after every add needs nothing further, because floor(integer + step)
// is integer + floor(step).
//
// The difference is invisible wherever the step is a power of two per pixel,
// which is why it took a capture drawing an odd gradient to see at all.
//
// ⚠️ The accumulator is NOT blocked, it is not the perspective route (a divided
// coordinate keeps its exact plane), and the sprite path is not separate from the
// triangle one. Each of those was measured and refused.
//
// ⚠️ The seed's own grid is a model rather than a reading. The fit says only that
// the seed sits below the plane; it is put on the accumulator's grid because a
// seed finer than the thing it seeds has nowhere to keep the extra bits, and at
// fifteen bits clearing bit 0 can move neither the sampled texel (bits 16 and up)
// nor the filter weight (bits 12 to 15). What IS observable on the seed is the
// FLOOR: a negative coordinate sitting exactly on a boundary rounds the other way
// under truncate-toward-zero.
static constexpr int GS_UV_FRACTIONAL_BITS = 15;
static constexpr int GS_UV_GRID_SHIFT = 16 - GS_UV_FRACTIONAL_BITS;
static constexpr s32 GS_UV_GRID_MASK = ~((1 << GS_UV_GRID_SHIFT) - 1);

// THE FORMED COORDINATE SATURATES INTO A SIGNED 12.4 FIELD.
//
// A coordinate at or above 2,047.9375 texels samples texel 2,047 -- at weight 15
// under the linear filter -- and one at or below -2,048 samples texel -2,048. It
// saturates, it does not wrap.
//
// The clamp is on the FORMED SIXTEENTH: after the truncation to sixteenths and
// after the linear filter's half-texel step, and before the tap pair is split out
// and the wrap or clamp addressing runs. Clamping earlier would read weight 7 at
// the top of the field where the console reads 15. The two axes saturate
// independently.
//
// The UV register cannot reach the field (it is 10.4, so it tops out at 1,023.9375
// texels), so the rule is unobservable on that route by construction rather than
// excluded from it, and nothing here is gated on the route.
//
// ⚠️ The clamp point is bracketed, not pinned: nothing we measured ramps across
// 2,047.9375, so where exactly it bites is known only to within that gap. Nothing
// drives it under mipmapping either -- the mip levels take the field at the same
// point in their own copy of the chain because that is the same place, not because
// a reading says so.
//
// Below the sixteenth there is nothing either half of the split reads, so the
// saturation may drop those bits and does.
static constexpr s32 GS_COORD_SIXTEENTH_MIN = -0x8000; // -2048.0 texels
static constexpr s32 GS_COORD_SIXTEENTH_MAX = 0x7FFF;  // +2047.9375 texels
static constexpr int GS_COORD_SIXTEENTH_SHIFT = 12;

// A TRIANGLE WHOSE SETUP INVERTS EXACTLY DOES NOT TRAIL THE PLANE.
//
// The scanline's coordinate trails the exact plane by one 16.16 unit on each axis
// a triangle walks forward (GSCoordinateLag.h), which moves the sample one texel
// down wherever the exact coordinate lands on a texel boundary. A descending or
// still walk never trails, and sprites take nothing.
//
// The exemption is a property of the triangle, not of its step: an axis is exact
// when twice the triangle's area, in 12.4 units squared, is a power of two --
// which is exactly when the setup's divide by that area is exact. What reaches the
// pixel is the grid the setup's products are truncated onto, not the reciprocal's
// own error.
//
// ⚠️ Nothing separates "the setup inverts exactly" from "the setup inverts exactly
// AND the step is simple": no measurement we hold draws a non-unit step at a
// power-of-two area.

/// Twice the triangle's signed area, in 12.4 units squared, exactly.
///
/// The position lanes carry the 12.4 word divided by sixteen (GSRendererSW's
/// `s_pos_scale`), so multiplying by sixteen recovers the word the GIF sent. The
/// product needs sixty-four bits: a screen coordinate is sixteen bits of 12.4, so
/// an edge is seventeen and the cross of two of them is thirty-four.
///
/// The ARM64 setup generator computes the identical integer from the identical
/// words -- one FCVTZS at four fractional bits, then the cross in NEON -- so the
/// two roads cannot disagree about it, and neither forms it in floating point.
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

/// One affine-route coordinate, floored onto the console's accumulator grid.
/// Floor, not truncate-toward-zero: a fixed-point register drops the bits below
/// it, which is floor in two's complement. No measurement we hold walks a
/// coordinate backwards far enough to separate the two, so this is the model's
/// shape rather than a measured one.
__forceinline static s32 GSAffineCoordinateOnGrid(float v)
{
	return static_cast<s32>(std::floor(v)) & GS_UV_GRID_MASK;
}

// THE COORDINATE IS HELD TO ONE GRAIN PER PRIMITIVE, AND THE GRADIENT SITS ON A
// GRID A THOUSANDTH OF IT.
//
// An affine STQ triangle's S and T are kept to fourteen significant bits of the
// LARGEST of its three vertices rather than of each vertex's own, every vertex
// truncated toward zero onto that one grain. The gradient is then formed from the
// truncated vertices and pushed DOWN onto a grid of the grain over 1,024. The push
// is strict -- a gradient landing exactly on a grid point takes the point below
// it, because the setup multiplies by a truncated reciprocal of twice the area and
// the product comes out a hair low -- except where twice the area is a power of
// two, where that reciprocal is exact and the gradient keeps its own value.
//
// The front end already truncates each vertex onto a FINER power-of-two grid: its
// own nine low mantissa bits, more while Q's exponent leads S's. Truncating an
// already-truncated value onto a coarser multiple of the same grid lands exactly
// where truncating the raw value onto the coarser grid would, so the primitive's
// grain can be applied here, in the software renderer, and reach the coordinate
// that widening the front end's own rule would have reached. That identity is why
// GSState::Draw is untouched and the hardware renderers still see what they saw.
//
// The vertex truncation and the gradient's grid are one mechanism read in two
// places: the first is the magnitude shortfall a coordinate sweep measures, the
// second is the trailing lag the pixel sees, which is why that lag grows with the
// walk and vanishes on a power-of-two area.
//
// Three things here are the model written down rather than measured. Only the
// per-pixel gradient is pushed, because nothing measured varies down a column. The
// strict step down is the limit of a relative deficit on a product, so it is taken
// on a FORWARD gradient only -- a still axis keeps zero and a backward one takes
// the plain floor. And the denominator is 1,024 on every reading, with the grain
// and the gradient moving together on all of them, so nothing separates it from
// 512 or 2,048.
//
// The grain reads S's exponent alone, which is the route's own condition rather
// than a simplification: an affine STQ primitive carries Q identically one at
// every vertex, so Q's exponent never leads.

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

/// One vertex's S and T truncated toward zero onto the primitive's grain.
///
/// Q and fog ride through untouched. The half texel comes off again afterwards; both
/// steps are exact, because the half is a whole number of the coordinate's own units
/// and every value here is a multiple of one.
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

/// The ST gradient pushed down onto a grid a thousandth of the grain, strictly where
/// the setup's divide by twice the area is inexact.
///
/// Taken in double so the quotient by a power of two and the product back are both
/// exact; the result is a multiple of the grid at the gradient's own magnitude, so
/// the float it is stored back into holds it.
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

// A UV-ROUTE SPRITE'S ASCENDING RAMP RUNS ONE SIXTEENTH OF A TEXEL LOW.
//
// A sprite on the UV route whose coordinate ramps ASCENDING along an axis whose OWN
// EXTENT IN PIXELS is not a power of two samples one sixteenth of a texel low on
// that axis, from the sprite's second pixel along that axis, for its whole length,
// with no recovery. Everything else is exact. Each clause is measured:
//
//   * It is the RAMPING axis's own extent. A non-dyadic height moves a V ramp and
//     leaves a U ramp alone, and the other way round; both cross terms are exact.
//   * ASCENDING only. The descending twin is exact, which is what rules out a lag:
//     a lag would read high descending.
//   * The onset is after that axis's FIRST pixel, not on the seed. Putting the term
//     in the seed is one pixel cheaper and takes the first column or row of every
//     qualifying draw a sixteenth low, where the console has it exact.
//
// So a V ramp's first scanline is unadjusted and every later scanline is V - 1/16,
// and a U ramp's first column is unadjusted and every later column is U - 1/16. It
// stays off the per-pixel path: the V case is one subtraction per row, and the U
// case draws the sprite's own first column as its own one-pixel span. Both are in
// the rasterizer, so the two scanline roads inherit it unchanged.
//
// ⚠️ Under a top or left clip the exempt pixel could be the sprite's own first row
// or column, or the first one the scissor left; nothing measured separates them.
// This implements the SPRITE'S OWN, so a sprite whose first row is clipped away has
// the term on every row it does draw.
//
// In the units the FST route walks -- 16.16 texels -- a sixteenth of a texel is 4096.
static constexpr float GS_UV_RAMP_BIAS = 4096.0f;

/// How far below the plane a UV-route sprite seeds one axis, given that axis's
/// per-pixel step and its own extent in pixels.
///
/// ⚠️ The step has to be a whole number of sixteenths of a texel -- in 16.16, an
/// exact multiple of 4096. A half, one, one and a half, two, three and four texels
/// per pixel all depart by one sixteenth at a non-dyadic extent, and every step that
/// is not a whole number of sixteenths is exact. The separation is one variable:
/// extent 192 at 8 sixteenths a pixel departs, and extent 192 at 4.7396 sixteenths a
/// pixel is exact over the same pixels.
///
/// The shape points at a mechanism, written down because it explains the whole
/// family rather than because the landing needs it: a per-pixel step whose magnitude
/// is truncated slightly low. A non-dyadic extent makes span/extent inexact, the
/// quotient rounds down, and the walk runs a hair behind the exact line. Where the
/// exact coordinate lands ON a sixteenth boundary -- which is exactly when the step
/// is a whole number of sixteenths -- that hair drops the floor by one sixteenth at
/// every pixel after the first; where it lands inside a sixteenth the same hair is
/// invisible. It predicts the dyadic case exact, the departure constant and never
/// recovering, the first pixel exact, and descending exact.
///
/// ⚠️ Untested, and the one case nothing measured reaches: a non-dyadic extent at a
/// non-unit step on an axis whose extent is not a power of two.
__forceinline static float GSSpriteRampBias(float step, int extent)
{
	const bool dyadic = extent > 0 && (extent & (extent - 1)) == 0;

	// The gradient as the walk carries it, in 16.16: a whole number of sixteenths
	// of a texel is an exact multiple of 4096.
	const s32 istep = static_cast<s32>(std::floor(step));
	const bool whole_ulp = istep > 0 && (istep & (static_cast<s32>(GS_UV_RAMP_BIAS) - 1)) == 0;

	return (whole_ulp && !dyadic) ? GS_UV_RAMP_BIAS : 0.0f;
}
