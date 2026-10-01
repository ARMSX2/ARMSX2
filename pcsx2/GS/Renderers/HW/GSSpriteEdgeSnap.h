// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

/// The two upscaling adjustments a sprite's far edge can get, and the arithmetic behind each.
///
/// Both push the far edge outwards by up to half a pixel. The GS rasterises a sprite in whole
/// pixels, so an edge part way into a pixel covers the same pixels as an edge on the boundary;
/// upscaling multiplies the coordinate first and loses that. The pixel-grid snap applies to any
/// sprite whose UV slide comes out whole; the AlignSpriteX game fix applies to every sprite in a
/// batch whose first sprite is half a pixel short. A sprite must never get both.
///
/// Both must leave alone a far edge the next sprite starts on: the fix via `hole_in_vertex`, the
/// snap via DropAbuttingAxes.
///
/// Order matters: the fix decides from the first sprite's coordinates, so the snap must not run
/// before it.
///
/// Also holds the test the Align to Native with Texture Offset half-pixel mode uses to move a
/// sprite batch onto the grid, since it reads the same fractions.
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
	/// Returns a zero delta when the sprite already ends on the grid, its far edge is not right of
	/// / below its near edge, or the implied UV slide is not a whole 1/16-texel step. Rounding a
	/// fractional slide would resample the whole sprite to gain one edge pixel.
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

	/// One sprite's near corner, as the batch walk hands it to the abutment test below. `present`
	/// is false at the two ends of the batch, where there is no neighbour to compare against.
	struct NearCorner
	{
		int x = 0;
		int y = 0;
		bool present = false;
	};

	/// Cancels the snap on an axis where a neighbouring sprite starts exactly on this sprite's far
	/// edge.
	///
	/// At native resolution the neighbour draws the pixel at a shared edge, so this sprite loses
	/// nothing there. Snapping it would draw the neighbour's first device column twice, which a
	/// blended batch shows as a bright seam (Need for Speed Underground's bloom strips).
	///
	/// Unlike AlignSpriteX, which refuses a whole tiling batch, this is per sprite, so the last
	/// sprite of a strip still gets its edge.
	///
	/// The other axis is not checked, so a neighbour on the same coordinate that shares no row
	/// also cancels the snap. That errs towards not snapping, which is safe.
	inline constexpr Delta DropAbuttingAxes(Delta d, int x1, int y1, NearCorner prev, NearCorner next)
	{
		if ((prev.present && prev.x == x1) || (next.present && next.x == x1))
		{
			d.dx = 0;
			d.du = 0;
		}
		if ((prev.present && prev.y == y1) || (next.present && next.y == y1))
		{
			d.dy = 0;
			d.dv = 0;
		}
		return d;
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

	/// Whether halfPixelOffset mode 5 (Align to Native with Texture Offset) moves a textured FST
	/// sprite batch towards the pixel grid on one axis. Like the rest of that mode it is decided on
	/// the first sprite. The move is (16 - fraction) sixteenths of a pixel, so it lands an edge
	/// sitting half a pixel or more off the grid on the next whole pixel.
	///
	/// A sprite whose near edge sits on a whole pixel is exempt: it is already on the grid (at
	/// native 31.0 .. 41.9375 covers the same pixels as 31.0 .. 42.0), and moving it shifts every
	/// texel by a device pixel when upscaled (Dirge of Cerberus item text). Every other sprite
	/// gets the move, including ones whose two edges carry different fractions.
	///
	/// `frac0` and `frac1` are the 1/16 fractions of the sprite's first and second vertex on this
	/// axis, relative to XYOFFSET; `first_is_near` says the first vertex is the lower coordinate.
	inline constexpr bool NativeSpritePushApplies(int frac0, int frac1, bool first_is_near)
	{
		if (first_is_near && frac0 == 0)
			return false;
		return (frac1 & 8) != 0;
	}
} // namespace GSSpriteEdgeSnap
