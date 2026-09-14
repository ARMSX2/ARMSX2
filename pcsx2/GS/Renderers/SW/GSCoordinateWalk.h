// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"

#include <cmath>

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
// which is exactly when the setup's divide by that area is exact. Measured over
// twenty-two one-variable arms, against which the primitive class, the vertices'
// sub-pixel placement, the texture size, the extent, the wrap mode, the filter,
// the route, XYOFFSET, the blend, the texture function and the scissor are each
// refuted.
//
// ⚠ The mechanism behind it is not measured. A reciprocal of a power of two is
// exact where every other one is truncated, so the likely story is that the trail
// IS that truncation and vanishes when there is nothing to truncate -- but that is
// a story, and what is implemented is the fitted rule.
//
// ⚠ Nothing separates "the setup inverts exactly" from "the setup inverts exactly
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
