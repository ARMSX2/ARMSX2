// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

/// Where a point lands, and how much vertex offset a figure needs to cover that same pixel once
/// the device grid is bigger than the native one.
///
/// **A point rounds to nearest.** It does not obey the corner-sample rule sprites and triangles
/// follow. The gs-prim console capture (SCPH-30001) swept the phase a sixteenth at a time on both
/// axes and read the answer off the table: fractions 0..7 stay on the pixel, 8..15 move to the
/// next one. Round-to-nearest explains all 60 of its point cells; truncation explains 24. The
/// software renderer has always done this -- GSRasterizer::DrawPoint adds 0.5 and truncates -- and
/// it is the same rounding GSLineWalk uses for a line's endpoints and for its minor axis, which
/// the same capture matched on 188 of 188 line cases.
///
/// **Upscaled, a point covers the whole device block of the native pixel it lights**, the way a
/// one-pixel sprite does. The rounding cannot be folded into the draw's vertex offset instead: the
/// offset is one constant for the whole draw and rounding is a step function of the coordinate, so
/// above 1x there is no constant that lands every phase on the right block. Apply the rule where
/// it was measured -- snap the vertex onto the boundary of the pixel it lights, at native
/// resolution, in the 1/16 units the vertex buffer holds -- and the offset is then only asked to
/// turn a whole native pixel into its block of device pixels.
///
/// **Which offset that is depends on the figure the backend draws**, and above 1x the two answers
/// differ:
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
/// At native resolution the two are the same number, which is why nothing at 1x has ever had to
/// choose -- and why Align to Native's half a native pixel has been right for hardware points all
/// along and wrong for the pixel-run rectangles.
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
