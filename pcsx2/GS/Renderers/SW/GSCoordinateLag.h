// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/SW/GSCoordinateWalk.h"

// What the software texture cache has to map, given that the scanline does not
// sample where the exact plane puts the coordinate.
//
// A triangle's coordinate trails the plane by one 16.16 unit on each forward axis
// (GSDrawScanline's `tclag`), moving the sample one texel down where the exact
// coordinate lands on a texel boundary. If the cache maps only the exact range, a
// range starting on a block boundary reads one texel below the mapped area.
//
// So the minimum drops by one texel on both axes. Over-mapping by a texel is
// free since the cache aligns to blocks, and it keeps the gradient test in one
// place.
//
// Sprites take no lag, so no expansion.
__forceinline static GSVector4i GSCoverageWithCoordinateLag(const GSVector4i& r, u32 primclass)
{
	if (primclass == GS_SPRITE_CLASS)
		return r;

	return GSVector4i(r.x - 1, r.y - 1, r.z, r.w).max_i32(GSVector4i::zero());
}

// The coordinate saturates into a signed 12.4 field (GSCoordinateWalk.h), so a
// draw whose coordinate leaves it samples the field's edge (texel 2047 or -2048),
// outside the range its vertices name. An axis that leaves the field is therefore
// mapped whole. That cannot under-map under any wrap mode, since the edge's image
// is inside the texture.
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
