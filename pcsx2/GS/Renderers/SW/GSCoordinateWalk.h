// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

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
