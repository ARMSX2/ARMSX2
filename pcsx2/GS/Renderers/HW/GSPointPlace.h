// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

/// Where a point lands, and how much vertex offset a figure needs to cover that same pixel once
/// the device grid is bigger than the native one.
///
/// A point rounds to nearest: fractions 0..7 stay on the pixel, 8..15 move to the next. It does
/// not follow the corner-sample rule of sprites and triangles. This matches console behaviour,
/// GSRasterizer::DrawPoint (add 0.5, truncate), and GSLineWalk's endpoint and minor-axis rounding.
///
/// Upscaled, a point covers the whole device block of the native pixel it lights, like a
/// one-pixel sprite. The rounding cannot be folded into the draw's vertex offset, which is one
/// constant per draw, while rounding is a step function of the coordinate. So the vertex is
/// snapped onto its pixel boundary at native resolution in 1/16 units, and the offset then only
/// maps a native pixel to its device block.
///
/// The offset depends on the figure the backend draws; above 1x the two differ:
///
/// - A figure whose corners are pixel boundaries takes **half a device pixel**, the amount that
///   puts a boundary between two device pixels. VSExpand::Point grows a quad one native pixel
///   right and down from the position, so it is one of these, and so are the rectangles
///   LinesToPixelRuns emits.
/// - A figure centred on the coordinate and one native pixel across takes **half a native pixel**,
///   which is what puts its centre on the middle of the block. A hardware point sprite of
///   target_scale device pixels is one of these, and so are a wide line and the quad
///   VSExpand::Line builds.
///
/// At native resolution the two are equal.
namespace GSPointPlace
{
	/// The native pixel a point at v lights. v is relative to XYOFFSET, in 1/16 pixel. >> rounds
	/// towards -inf, so a negative coordinate rounds the same way a positive one does.
	inline constexpr int Pixel(int v) { return (v + 8) >> 4; }

	/// v moved onto the near boundary of the pixel it lights, still relative to XYOFFSET, still in
	/// 1/16 pixel. Moves a coordinate by at most half a pixel and never off its own pixel.
	inline constexpr int SnapToPixel(int v) { return Pixel(v) * 16; }

	/// Half a device pixel, in the clip-space units DetermineVSConfig's vertex offset is in.
	/// `scale` is that function's sx or sy: one GS unit -- a sixteenth of a native pixel -- in
	/// clip space, so sixteen of them span a native pixel and target_scale device pixels.
	inline constexpr float BoundaryFigureOffset(float scale, float target_scale)
	{
		return 8.0f * scale / target_scale;
	}

	/// Half a native pixel, in the same units.
	inline constexpr float CentredFigureOffset(float scale) { return 8.0f * scale; }
} // namespace GSPointPlace
