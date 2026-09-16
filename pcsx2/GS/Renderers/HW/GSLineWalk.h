// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <algorithm>
#include <cmath>

/// The pixels a GS line primitive lights.
///
/// This is the software renderer's line walk (GSRasterizer::DrawEdgeLine), which the gs-prim
/// console capture matched on all 188 of its line cases, restated in the 1/16-pixel integers the
/// vertex buffer holds. Every quantity the walk uses is a whole number of those units, so the
/// integer form here lands on the same pixels as the float form there.
///
/// The rule:
///
/// - The major axis is X when |dx| >= |dy|, otherwise Y. The walk lights one pixel per whole-pixel
///   step along it.
/// - Each endpoint belongs to the pixel whose centre it rounds to, floor(v + 0.5). Whether that
///   pixel is drawn is a diamond test on the endpoint's offset from the centre, |dx| + |dy| against
///   half a pixel: the first pixel is drawn when the endpoint does not leave its diamond in the
///   direction of travel, the last one only when it does. So a line between whole-pixel points
///   draws its first pixel and not its last, and a line and its reverse differ by two pixels.
/// - On the minor axis the walk lights floor(v(m) + 0.5), where v(m) is the exact line at major
///   coordinate m.
///
/// Coordinates are relative to XYOFFSET, in 1/16 pixel.
namespace GSLineWalk
{
	inline constexpr int Abs(int v) { return v < 0 ? -v : v; }

	/// floor(v / 16), negative v included: >> rounds towards -inf.
	inline constexpr int FloorPixel(int v) { return v >> 4; }

	/// The diamond test for one endpoint. dx/dy is the endpoint minus the centre of the pixel it
	/// rounds to, in 1/16 pixel. True means the line leaves that pixel's diamond heading along the
	/// major axis in the direction of travel.
	inline constexpr bool LeavesDiamond(int dx, int dy, bool step_x, bool pos_x, bool pos_y)
	{
		const int dist = Abs(dx) + Abs(dy);
		if (dist < 8)
			return false;
		if (step_x)
		{
			const bool x_good = pos_x ? (dx > 0) : (dx < 0);
			return x_good && (dist > 8 || dy >= 0);
		}
		const bool y_good = pos_y ? (dy > 0) : (dy < 0);
		return y_good && (dist > 8 || dx >= 0);
	}

	/// The walk itself. Calls step(x, y, D, scale) once per whole-pixel step along the major axis,
	/// in the order the GS takes them, and returns how many steps there were. D is the decision
	/// value -- the minor coordinate's signed distance from the centre of the pixel the step landed
	/// on, in units of 1/scale of a pixel -- which is what the AA1 coverage is read off. Everything
	/// else here is Walk(); the split exists so the coverage can be taken without a second
	/// transcription of the walk to keep in step with this one.
	template <typename Step>
	inline int WalkSteps(int x0, int y0, int x1, int y1, Step&& step_fn)
	{
		const int dx = x1 - x0;
		const int dy = y1 - y0;
		const bool step_x = Abs(dx) >= Abs(dy);
		const bool pos_x = dx >= 0;
		const bool pos_y = dy >= 0;
		const int dxi = pos_x ? 1 : -1;
		const int dyi = pos_y ? 1 : -1;

		int rx0 = FloorPixel(x0 + 8);
		int ry0 = FloorPixel(y0 + 8);
		int rx1 = FloorPixel(x1 + 8);
		int ry1 = FloorPixel(y1 + 8);

		const bool draw_first = !LeavesDiamond(x0 - (rx0 * 16), y0 - (ry0 * 16), step_x, pos_x, pos_y);
		const bool draw_last = LeavesDiamond(x1 - (rx1 * 16), y1 - (ry1 * 16), step_x, pos_x, pos_y);

		if (!draw_first)
		{
			rx0 += step_x ? dxi : 0;
			ry0 += step_x ? 0 : dyi;
		}
		if (!draw_last)
		{
			rx1 -= step_x ? dxi : 0;
			ry1 -= step_x ? 0 : dyi;
		}

		// Also rejects every zero-length line, so the major delta below is never zero.
		if ((step_x ? dxi * (rx1 - rx0) : dyi * (ry1 - ry0)) < 0)
			return 0;

		// D is the minor coordinate's distance from the centre of the current minor pixel, in units
		// of 1/(32 * major) pixel. The software renderer scales by 2 * 16 * 16 * |major| with the
		// major delta in pixels; with the delta in 1/16 pixel that factor is 32 * |major16|.
		const s64 major = Abs(step_x ? dx : dy);
		const s64 minor = step_x ? dy : dx;
		const s64 scale = 32 * major;
		const s64 half = scale / 2;
		const s64 step = 32 * minor;
		const int prestep = step_x ? dxi * ((rx0 * 16) - x0) : dyi * ((ry0 * 16) - y0);
		s64 D = 2 * major * (step_x ? (y0 - (ry0 * 16)) : (x0 - (rx0 * 16))) + 2 * minor * prestep;

		int xi = rx0;
		int yi = ry0;
		while (D >= half)
		{
			D -= scale;
			xi += step_x ? 0 : 1;
			yi += step_x ? 1 : 0;
		}
		while (D < -half)
		{
			D += scale;
			xi -= step_x ? 0 : 1;
			yi -= step_x ? 1 : 0;
		}

		const bool pos_minor = step_x ? pos_y : pos_x;
		const int end = step_x ? rx1 : ry1;
		int count = 0;
		for (;;)
		{
			step_fn(xi, yi, D, scale);
			count++;
			if ((step_x ? xi : yi) == end)
				break;

			D += step;
			xi += step_x ? dxi : 0;
			yi += step_x ? 0 : dyi;
			if (pos_minor ? (D >= half) : (D < -half))
			{
				const int sign = pos_minor ? 1 : -1;
				D -= scale * sign;
				xi += step_x ? 0 : sign;
				yi += step_x ? sign : 0;
			}
		}
		return count;
	}

	/// Calls pixel(x, y) for every pixel the line from (x0, y0) to (x1, y1) lights, in the order the
	/// GS walks them, and returns how many there were. Pixel coordinates are whole pixels relative
	/// to XYOFFSET. A zero-length line lights nothing.
	template <typename Pixel>
	inline int Walk(int x0, int y0, int x1, int y1, Pixel&& pixel)
	{
		return WalkSteps(x0, y0, x1, y1, [&](int x, int y, s64, s64) { pixel(x, y); });
	}

	/// The pixels an AA1 line lights, and the coverage each one carries.
	///
	/// An AA1 line lights TWO pixels per step, not one: the pixel the walk owns and its neighbour
	/// on the minor axis, on whichever side the exact line leans. The two share a coverage between
	/// them -- the near one gets what is left of full coverage, the far one the remainder -- so a
	/// line that sits exactly on a row of pixel centres writes that row at full coverage and the
	/// row beside it at zero. The zero-coverage row still writes: it blends nothing, but its alpha
	/// lands in memory, which the gs-prim console capture measured (Result 8).
	///
	/// Calls pixel(x, y, cov, side) twice per step, side 0 for the walk's own pixel and 1 for its
	/// neighbour, and returns the number of pixels. `cov` is the software renderer's 16-bit edge
	/// value; the scanline reads the top 7 bits of it, so the alpha it becomes runs 0 to 127.
	///
	/// This is GSRasterizer::DrawEdgeLine's antialiased arm, value for value, including the float
	/// division it truncates the coverage out of. Both renderers therefore put the same coverage on
	/// the same pixel, which is what gs_line_walk_tests.cpp pins.
	template <typename Pixel>
	inline int WalkAA1(int x0, int y0, int x1, int y1, Pixel&& pixel)
	{
		const bool step_x = Abs(x1 - x0) >= Abs(y1 - y0);

		return 2 * WalkSteps(x0, y0, x1, y1, [&](int x, int y, s64 D, s64 scale) {
			const float cov = 0xffff * std::abs(static_cast<float>(D) / static_cast<float>(scale));
			const int covi = std::clamp(static_cast<int>(cov), 0, 0xffff);
			const int offset = (D >= 0) ? 1 : -1;

			pixel(x, y, 0xffff - covi, 0);
			pixel(x + (step_x ? 0 : offset), y + (step_x ? offset : 0), covi, 1);
		});
	}
} // namespace GSLineWalk
