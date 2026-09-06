// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

/// The two upscaling adjustments a sprite's far edge can get, and the arithmetic behind each.
///
/// Both push the far edge outwards by up to half a pixel, for the same reason: the GS rasterises a
/// sprite in whole pixels, so an edge part way into a pixel covers what an edge on the boundary
/// covers, and upscaling multiplies the coordinate before rasterising and loses the difference.
/// They disagree about which sprites deserve it -- the pixel-grid snap takes any sprite whose UV
/// slide comes out whole, the AlignSpriteX game fix takes every sprite in a batch whose first
/// sprite is half a pixel short -- so a sprite must never get both.
///
/// The rules live in a header of their own so the order they run in can be tested without a GS
/// device: the fix's one decision is read off the first sprite's coordinates, so a snap that ran
/// first would answer it from coordinates it had already moved.
namespace GSSpriteEdgeSnap
{
	/// How far one sprite's far corner has to move, in the sprite's own 1/16 units.
	struct Delta
	{
		int dx = 0;
		int dy = 0;
		int du = 0;
		int dv = 0;

		constexpr bool IsZero() const { return (dx | dy | du | dv) == 0; }
	};

	/// ceil(a / 16) * 16, negative a included: >> rounds towards -inf.
	inline constexpr int SnapUp(int a) { return ((a + 15) >> 4) << 4; }

	/// The pixel-grid snap for one sprite. X and Y are relative to XYOFFSET; adjust_uv says the
	/// sprite samples a texture with FST coordinates, so the UV has to slide with the position.
	///
	/// A zero delta means the sprite is left alone, which happens for three reasons: it already
	/// ends on the grid, its far edge is not to the right of / below its near edge, or the UV
	/// slide the position slide implies is not a whole step of the coordinate's own 1/16-texel
	/// grid. The last one is a refusal, not an oversight -- writing a fractional slide down means
	/// rounding, and rounding resamples the whole sprite to buy one edge pixel.
	inline constexpr Delta FarEdge(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, bool adjust_uv)
	{
		const int dx = (x1 > x0) ? (SnapUp(x1) - x1) : 0;
		const int dy = (y1 > y0) ? (SnapUp(y1) - y1) : 0;
		if ((dx | dy) == 0)
			return {};

		if (!adjust_uv)
			return {dx, dy, 0, 0};

		const int lx = x1 - x0;
		const int ly = y1 - y0;
		const int lu = u1 - u0;
		const int lv = v1 - v0;
		if (dx != 0 && (lx == 0 || (lu * dx) % lx != 0))
			return {};
		if (dy != 0 && (ly == 0 || (lv * dy) % ly != 0))
			return {};

		return {dx, dy, (lx != 0) ? ((lu * dx) / lx) : 0, (ly != 0) ? ((lv * dy) / ly) : 0};
	}

	/// Whether the AlignSpriteX game fix (UserHacks_AlignSpriteX, ace combat / tekken) fires on
	/// this batch. It is one decision for the whole batch, taken on the first sprite, and every
	/// sprite in the batch then gets half a pixel added to its far X.
	///
	/// `win_position` is the first sprite's far X relative to XYOFFSET, `far_u` its far U, `count`
	/// the vertex count, and `x1`/`x2` the far X of the first sprite and the near X of the second
	/// (equal when the batch tiles without a hole).
	inline constexpr bool AlignSpriteXApplies(int win_position, int far_u, bool fst, u32 count, int x1, int x2)
	{
		const bool unaligned_position = ((win_position & 0xF) == 8);
		const bool unaligned_texture = ((far_u & 0xF) == 0) && fst;
		const bool hole_in_vertex = (count < 4) || (x1 != x2);
		return hole_in_vertex && unaligned_position && (unaligned_texture || !fst);
	}
} // namespace GSSpriteEdgeSnap
