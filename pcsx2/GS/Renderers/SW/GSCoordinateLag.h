// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/SW/GSCoordinateWalk.h"

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

// The same concern, for the other end of the coordinate.
//
// The scanline's coordinate saturates into a signed 12.4 field
// (GSCoordinateWalk.h), so a draw whose coordinate leaves that field samples the
// field's EDGE -- texel 2,047, or -2,048 -- and not a texel anywhere near the
// range its own vertices name. The cache is told the range, so the edge is
// unmapped and the fetch reads an unfilled buffer. Whether that is visible then
// depends on what else happened to fill the page, which is worse than a defect
// that is always wrong.
//
// So an axis whose coordinate leaves the field is mapped whole. It fires only for
// draws that do leave it -- a few dozen in a whole dump corpus -- and it cannot
// under-map under any wrap mode, because the edge's image is inside the texture
// whichever one is in force.
__forceinline static GSVector4i GSCoverageWithCoordinateField(const GSVector4i& r,
	const GSVector4& tmin, const GSVector4& tmax, const GIFRegTEX0& TEX0)
{
	constexpr float lo = static_cast<float>(GS_COORD_SIXTEENTH_MIN) / 16.0f;
	constexpr float hi = static_cast<float>(GS_COORD_SIXTEENTH_MAX) / 16.0f;

	GSVector4i out = r;

	if (tmin.x < lo || tmax.x > hi)
	{
		out.x = 0;
		out.z = 1 << TEX0.TW;
	}

	if (tmin.y < lo || tmax.y > hi)
	{
		out.y = 0;
		out.w = 1 << TEX0.TH;
	}

	return out;
}
