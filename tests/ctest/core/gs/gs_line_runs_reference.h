// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The line pixel-run conversion in its original form: two walks per line (count, then emit), a
// 64-bit divide per colour channel per pixel, and every corner interpolated on its own. Kept
// verbatim as the oracle for gs_line_runs_tests.cpp, which requires GSLineRuns::Convert to write
// the same bytes as this for every input. Do not optimise this copy.

#pragma once

#include "GS/Renderers/HW/GSLineRuns.h"
#include "GS/Renderers/HW/GSLineWalk.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GSLineRunsReference
{
	namespace detail
	{
		/// floor(num / den), either sign of either.
		inline s64 LineFloorDiv(s64 num, s64 den)
		{
			if (den < 0)
			{
				num = -num;
				den = -den;
			}
			return (num >= 0) ? (num / den) : -((-num + den - 1) / den);
		}

		/// Four 8-bit channels packed in a u32, each taken k/den of the way from a to b, floored and
		/// clamped. Used for RGBA and for FOG.
		inline u32 LineGradient8(u32 a, u32 b, s64 k, s64 den)
		{
			u32 out = 0;
			for (int shift = 0; shift < 32; shift += 8)
			{
				const s64 ca = (a >> shift) & 0xFF;
				const s64 cb = (b >> shift) & 0xFF;
				const s64 c = std::clamp<s64>(ca + LineFloorDiv((cb - ca) * k, den), 0, 255);
				out |= static_cast<u32>(c) << shift;
			}
			return out;
		}

		inline float LineLerp(float a, float b, double t)
		{
			return static_cast<float>(a + (static_cast<double>(b) - a) * t);
		}

		template <typename T>
		inline T LineLerpRound(T a, T b, double t, double max)
		{
			const double v = a + (static_cast<double>(b) - a) * t;
			return static_cast<T>(std::clamp(std::round(v), 0.0, max));
		}

		/// The alpha an AA1 pixel carries, given the pixel's own colour and the walk's 16-bit coverage.
		///
		/// The GS substitutes the coverage FOR the alpha rather than multiplying anything by it: with
		/// blending off it always does, and with blending on only where the alpha is exactly 128
		/// (gs-prim Results 4 and 7 -- one above the boundary switches it off, so the rule is equality
		/// and not a threshold). The scanline reads the top 7 bits of the 16-bit value, which is why a
		/// line sitting on a row of pixel centres comes back at 127 and not 128.
		inline u32 LineCoverageAlpha(u32 rgba, int cov, bool abe)
		{
			if (abe && (rgba >> 24) != 128)
				return rgba;

			return (rgba & 0x00FFFFFFu) | (static_cast<u32>(cov >> 9) << 24);
		}
	} // namespace detail

	inline GSLineRuns::Result Convert(const GSLineRuns::Input& in, GSVertex* dst, std::vector<size_t>* groups)
	{
		using namespace detail;
		const u32 line_count = in.line_count;
		const int ofx = in.ofx;
		const int ofy = in.ofy;

		// Corners go back into the vertex buffer as absolute 16-bit positions. Pixels whose corners
		// would not fit are dropped; they are thousands of pixels outside any target.
		const int min_x = -(ofx >> 4);
		const int max_x = ((0xFFFF - ofx) >> 4) - 1;
		const int min_y = -(ofy >> 4);
		const int max_y = ((0xFFFF - ofy) >> 4) - 1;

		const bool flat = in.flat;
		const bool fog = in.fog;
		const bool abe = in.abe;
		const bool aa1 = in.aa1;

		// Calls run(v0, v1, step_x, m0, dm, lo, hi, minor, rgba, fog) for each run of one line, after
		// clipping to the representable range. lo/hi are the run's end pixels on the major axis.
		const auto walk_runs = [&](const GSVertex& v0, const GSVertex& v1, auto&& run) {
			const int x0 = v0.XYZ.X - ofx;
			const int y0 = v0.XYZ.Y - ofy;
			const int x1 = v1.XYZ.X - ofx;
			const int y1 = v1.XYZ.Y - ofy;
			const bool step_x = GSLineWalk::Abs(x1 - x0) >= GSLineWalk::Abs(y1 - y0);
			const int m0 = step_x ? x0 : y0;
			const int dm = step_x ? (x1 - x0) : (y1 - y0);
			const bool colour_varies = !flat && v0.RGBAQ.U32[0] != v1.RGBAQ.U32[0];
			const bool fog_varies = fog && v0.FOG != v1.FOG;
			const int major_min = step_x ? min_x : min_y;
			const int major_max = step_x ? max_x : max_y;
			const int minor_min = step_x ? min_y : min_x;
			const int minor_max = step_x ? max_y : max_x;

			bool open = false;
			int lo = 0, hi = 0, minor = 0;
			u32 rgba = 0, f = 0;

			const auto close = [&]() {
				if (!open || minor < minor_min || minor > minor_max)
					return;
				const int clo = std::max(lo, major_min);
				const int chi = std::min(hi, major_max);
				if (clo <= chi)
					run(v0, v1, step_x, m0, dm, clo, chi, minor, rgba, f);
			};

			// cov < 0 means no antialiasing: the pixel keeps the colour the gradient gives it.
			const auto pixel = [&](int x, int y, int cov) {
				const int m = step_x ? x : y;
				const int n = step_x ? y : x;
				// dm is never zero here: a zero-length line lights nothing.
				const s64 k = static_cast<s64>(m) * 16 - m0;
				u32 c = colour_varies ? LineGradient8(v0.RGBAQ.U32[0], v1.RGBAQ.U32[0], k, dm) : v1.RGBAQ.U32[0];
				if (cov >= 0)
					c = LineCoverageAlpha(c, cov, abe);
				const u32 fg = fog_varies ? LineGradient8(v0.FOG, v1.FOG, k, dm) : v1.FOG;
				if (open && n == minor && c == rgba && fg == f)
				{
					lo = std::min(lo, m);
					hi = std::max(hi, m);
					return;
				}
				close();
				open = true;
				lo = hi = m;
				minor = n;
				rgba = c;
				f = fg;
			};

			if (aa1)
			{
				// An AA1 line lights two pixels per step, and they interleave: the walk's own pixel,
				// then its neighbour on the other side of the exact line. Taking one side at a time
				// keeps a single open run, so a line that holds one coverage for its whole length --
				// every axis-aligned one -- comes out as two rectangles instead of two per pixel.
				// Nothing depends on the order: within a line no two of these pixels coincide, since
				// the two of a step differ by one on the minor axis and consecutive steps differ on
				// the major.
				for (int side = 0; side < 2; side++)
				{
					GSLineWalk::WalkAA1(x0, y0, x1, y1, [&](int x, int y, int cov, int s) {
						if (s == side)
							pixel(x, y, cov);
					});
					close();
					open = false;
				}
			}
			else
			{
				GSLineWalk::Walk(x0, y0, x1, y1, [&](int x, int y) { pixel(x, y, -1); });
				close();
			}
		};

		// The full-barrier draw list counts primitives per group. Each group's line count becomes its
		// rectangle count, so the backend's count * indices_per_prim still lands on group boundaries.
		const bool remap_drawlist = groups != nullptr && !groups->empty();

		// Pass 1: count, and refuse the cases the draw cannot take.
		u32 total = 0;
		{
			size_t group = 0;
			size_t group_left = remap_drawlist ? (*groups)[0] : 0;
			u32 group_quads = 0;
			for (u32 i = 0; i < line_count; i++)
			{
				const GSVertex& v0 = in.vertices[in.indices[i * 2]];
				const GSVertex& v1 = in.vertices[in.indices[i * 2 + 1]];
				u32 quads = 0;
				walk_runs(v0, v1, [&](auto&&...) { quads++; });
				total += quads;

				if (remap_drawlist && group < groups->size())
				{
					group_quads += quads;
					if (--group_left == 0)
					{
						if (group_quads == 0)
							return {GSLineRuns::Outcome::GroupEmpty, 0};
						group_quads = 0;
						if (++group < groups->size())
							group_left = (*groups)[group];
					}
				}
			}
		}

		if (total == 0)
			return {GSLineRuns::Outcome::NothingLit, 0};

		if (total > GSLineRuns::MAX_RECTS)
			return {GSLineRuns::Outcome::TooMany, total};

		// Pass 2: write the rectangles.
		u32 written = 0;
		{
			size_t group = 0;
			size_t group_left = remap_drawlist ? (*groups)[0] : 0;
			u32 group_quads = 0;

			const auto emit = [&](const GSVertex& v0, const GSVertex& v1, bool step_x, int m0, int dm, int lo, int hi,
								  int minor, u32 rgba, u32 f) {
				const int major_of = step_x ? ofx : ofy;
				const int minor_of = step_x ? ofy : ofx;
				const int major_edge[2] = {lo * 16 + major_of, (hi + 1) * 16 + major_of};
				const int minor_edge[2] = {minor * 16 + minor_of, (minor + 1) * 16 + minor_of};
				const double t[2] = {static_cast<double>(lo * 16 - m0) / dm, static_cast<double>((hi + 1) * 16 - m0) / dm};

				GSVertex* const q = &dst[written * 4];
				for (int corner = 0; corner < 4; corner++)
				{
					const int a = corner & 1; // major edge
					const int b = corner >> 1; // minor edge
					GSVertex& v = q[corner];
					v = v1;
					v.XYZ.X = static_cast<u16>(step_x ? major_edge[a] : minor_edge[b]);
					v.XYZ.Y = static_cast<u16>(step_x ? minor_edge[b] : major_edge[a]);
					v.XYZ.Z = LineLerpRound<u32>(v0.XYZ.Z, v1.XYZ.Z, t[a], 4294967295.0);
					v.RGBAQ.U32[0] = rgba;
					v.RGBAQ.Q = LineLerp(v0.RGBAQ.Q, v1.RGBAQ.Q, t[a]);
					v.ST.S = LineLerp(v0.ST.S, v1.ST.S, t[a]);
					v.ST.T = LineLerp(v0.ST.T, v1.ST.T, t[a]);
					v.U = LineLerpRound<u16>(v0.U, v1.U, t[a], 65535.0);
					v.V = LineLerpRound<u16>(v0.V, v1.V, t[a], 65535.0);
					v.FOG = f;
				}
				written++;
			};

			for (u32 i = 0; i < line_count; i++)
			{
				const GSVertex& v0 = in.vertices[in.indices[i * 2]];
				const GSVertex& v1 = in.vertices[in.indices[i * 2 + 1]];
				const u32 before = written;
				walk_runs(v0, v1, emit);

				if (remap_drawlist && group < groups->size())
				{
					group_quads += written - before;
					if (--group_left == 0)
					{
						(*groups)[group] = group_quads;
						group_quads = 0;
						if (++group < groups->size())
							group_left = (*groups)[group];
					}
				}
			}
		}

		return {GSLineRuns::Outcome::Converted, written};
	}
} // namespace GSLineRunsReference
