// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "GS/GSRegs.h"

// What the software texture cache has to map, given that the scanline does not
// sample where the exact plane puts the coordinate.
//
// A triangle's coordinate trails the plane by one unit of our 16.16 on each axis
// whose walk goes forward (GSDrawScanline's `tclag`), and that moves the sample
// one texel DOWN wherever the exact coordinate lands on a texel boundary. The
// cache was being told the EXACT range, so a draw whose range began on a block
// boundary asked for a texel below everything mapped and read an unfilled buffer,
// which is zeros. Any non-sprite primitive with a walking coordinate and a range
// whose minimum lands on a block boundary hits it.
//
// So the rect handed to the cache covers what the scanline can actually ask for.
// The minimum drops by one texel on BOTH axes rather than only on the axes whose
// walk goes forward: a texel of over-mapping is free, since the cache aligns the
// rect to the block anyway, and keeping the gradient test in one place stops the
// two rules drifting apart later.
//
// Sprites take no lag (GSDrawScanline gates it on `sel.prim != GS_SPRITE_CLASS`),
// so they take no expansion.
__forceinline static GSVector4i GSCoverageWithCoordinateLag(const GSVector4i& r, u32 primclass)
{
	if (primclass == GS_SPRITE_CLASS)
		return r;

	return GSVector4i(r.x - 1, r.y - 1, r.z, r.w).max_i32(GSVector4i::zero());
}
