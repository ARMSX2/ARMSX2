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
