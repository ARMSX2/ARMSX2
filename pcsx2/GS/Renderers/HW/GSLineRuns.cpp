// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSLineRuns.h"
#include "GS/Renderers/HW/GSLineWalk.h"
#include "GS/GSVector.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>

namespace
{
	float LineLerp(float a, float b, double t)
	{
		return static_cast<float>(a + (static_cast<double>(b) - a) * t);
	}

	template <typename T>
	T LineLerpRound(T a, T b, double t, double max)
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
	u32 LineCoverageAlpha(u32 rgba, int cov, bool abe)
	{
		if (abe && (rgba >> 24) != 128)
			return rgba;

		return (rgba & 0x00FFFFFFu) | (static_cast<u32>(cov >> 9) << 24);
	}

	/// The most rectangles one line can produce: one per pixel, and a line lights at most one
	/// pixel per whole-pixel step along its major axis plus one (two per step with AA1).
	u32 LineBound(const GSVertex& v0, const GSVertex& v1, bool aa1)
	{
		const int dx = GSLineWalk::Abs(static_cast<int>(v1.XYZ.X) - static_cast<int>(v0.XYZ.X));
		const int dy = GSLineWalk::Abs(static_cast<int>(v1.XYZ.Y) - static_cast<int>(v0.XYZ.Y));
		return static_cast<u32>((std::max(dx, dy) >> 4) + 2) << (aa1 ? 1 : 0);
	}

	/// Colour and fog along a line, one byte channel per lane: lanes 0-3 are R, G, B, A and lanes
	/// 4-7 the four bytes of FOG.
	///
	/// A channel at major-axis offset k (1/16 pixel from the first vertex) is
	/// a + floor((b - a) * k / dm), clamped to 0..255. The walk moves k by 16 per pixel, always in
	/// the same direction, so the floor is carried as a quotient and a remainder in [0, |dm|) and
	/// stepped by a constant quotient and remainder: exact, with no division per pixel. The worst
	/// numerator is 255 * (65536 + 32), well inside 32 bits.
	struct LineGradient
	{
		GSVector4i q[2]; ///< a + the floor, per channel
		GSVector4i r[2]; ///< the remainder, 0 <= r < den
		GSVector4i step_q[2];
		GSVector4i step_r[2];
		GSVector4i den_minus_1;
		s32 den;

		/// ca is each channel's value at k = 0 and e its delta over the line, with the sign of dm
		/// folded in so that den = |dm| is positive. k is the first pixel's offset and dk the
		/// offset's change per pixel.
		void Init(const s32 (&ca)[8], const s32 (&e)[8], s32 den_, s32 k, s32 dk)
		{
			den = den_;
			alignas(16) s32 q0[8], r0[8], sq[8], sr[8];
			for (int i = 0; i < 8; i++)
			{
				q0[i] = ca[i];
				r0[i] = 0;
				sq[i] = 0;
				sr[i] = 0;
				if (e[i] == 0)
					continue;
				const s32 n = e[i] * k;
				const s32 fq = FloorDiv(n, den);
				q0[i] += fq;
				r0[i] = n - fq * den;
				const s32 dn = e[i] * dk;
				const s32 fdq = FloorDiv(dn, den);
				sq[i] = fdq;
				sr[i] = dn - fdq * den;
			}
			q[0] = GSVector4i::load<true>(&q0[0]);
			q[1] = GSVector4i::load<true>(&q0[4]);
			r[0] = GSVector4i::load<true>(&r0[0]);
			r[1] = GSVector4i::load<true>(&r0[4]);
			step_q[0] = GSVector4i::load<true>(&sq[0]);
			step_q[1] = GSVector4i::load<true>(&sq[4]);
			step_r[0] = GSVector4i::load<true>(&sr[0]);
			step_r[1] = GSVector4i::load<true>(&sr[4]);
			den_minus_1 = GSVector4i(den - 1);
		}

		__fi void Step()
		{
			for (int h = 0; h < 2; h++)
			{
				const GSVector4i rn = r[h].add32(step_r[h]);
				const GSVector4i carry = rn.gt32(den_minus_1); // all ones where rn >= den
				r[h] = rn.sub32(den_minus_1.add32(GSVector4i(1)) & carry);
				q[h] = q[h].add32(step_q[h]).sub32(carry);
			}
		}

		/// RGBA in the low 32 bits, FOG in the high 32, each channel clamped to 0..255.
		__fi u64 Packed() const
		{
			return static_cast<u64>(q[0].ps32(q[1]).pu16().extract64<0>());
		}

		static s32 FloorDiv(s32 num, s32 den)
		{
			const s32 quot = num / den;
			return (num % den != 0 && num < 0) ? quot - 1 : quot;
		}
	};

	/// The line's depth, texture coordinates and Q at one major-axis position, laid out as the two
	/// halves of a GSVertex with the per-run fields (RGBA, XY, FOG) zero.
	struct LineEdge
	{
		GSVector4i st_q; ///< S, T, 0, Q
		GSVector4i z_uv; ///< 0, Z, UV, 0
		s32 k; ///< the position, 1/16 pixel from the first vertex
	};

	struct DrawConstants
	{
		int ofx;
		int ofy;
		int min_x;
		int max_x;
		int min_y;
		int max_y;
		bool flat;
		bool fog;
		bool abe;
	};

	/// What a rectangle of one line needs besides its run.
	struct LineRects
	{
		const GSVertex* v0;
		const GSVertex* v1;
		int m0; ///< the first vertex on the major axis, 1/16 pixel
		int dm; ///< the major-axis delta, 1/16 pixel, never zero
		int major_min, major_max, minor_min, minor_max; ///< the pixels whose corners fit 16 bits
		int major_of, minor_of;
		u32 shift_major, shift_minor; ///< where each axis goes in the packed XY
		/// Adjacent runs of a line share a major-axis edge, so the values at the last two edges
		/// are kept and each rectangle computes at most one new one.
		LineEdge edges[2];

		/// Each value is a + (b - a) * t in double, t = k / dm, then narrowed to its field.
		__fi void ComputeEdge(LineEdge& edge, s32 k) const
		{
			const double t = static_cast<double>(k) / dm;
			const float s = LineLerp(v0->ST.S, v1->ST.S, t);
			const float tt = LineLerp(v0->ST.T, v1->ST.T, t);
			const float q = LineLerp(v0->RGBAQ.Q, v1->RGBAQ.Q, t);
			const u32 z = LineLerpRound<u32>(v0->XYZ.Z, v1->XYZ.Z, t, 4294967295.0);
			const u16 u = LineLerpRound<u16>(v0->U, v1->U, t, 65535.0);
			const u16 v = LineLerpRound<u16>(v0->V, v1->V, t, 65535.0);
			edge.st_q = GSVector4i(std::bit_cast<s32>(s), std::bit_cast<s32>(tt), 0, std::bit_cast<s32>(q));
			edge.z_uv = GSVector4i(0, static_cast<s32>(z), static_cast<s32>(static_cast<u32>(u) | (static_cast<u32>(v) << 16)), 0);
			edge.k = k;
		}

		/// The run over major pixels lo..hi on row `minor`, clipped to the pixels whose corners fit:
		/// returns 1 if any pixel is left, and with EMIT writes its rectangle to q. rgba_fog is RGBA
		/// in the low 32 bits and FOG in the high.
		template <bool EMIT>
		__fi u32 Close(GSVertex* q, int lo, int hi, int minor, u64 rgba_fog)
		{
			if (minor < minor_min || minor > minor_max)
				return 0;
			const int clo = std::max(lo, major_min);
			const int chi = std::min(hi, major_max);
			if (clo > chi)
				return 0;
			if constexpr (EMIT)
				Write(q, clo, chi, minor, rgba_fog);
			return 1;
		}

		void Write(GSVertex* q, int lo, int hi, int minor, u64 rgba_fog)
		{
			const s32 ka = lo * 16 - m0;
			const s32 kb = (hi + 1) * 16 - m0;
			int ia = (edges[0].k == ka) ? 0 : ((edges[1].k == ka) ? 1 : -1);
			int ib = (edges[0].k == kb) ? 0 : ((edges[1].k == kb) ? 1 : -1);
			if (ia < 0)
			{
				ia = (ib == 0) ? 1 : 0;
				ComputeEdge(edges[ia], ka);
			}
			if (ib < 0)
			{
				ib = ia ^ 1;
				ComputeEdge(edges[ib], kb);
			}

			const u32 major_a = static_cast<u16>(lo * 16 + major_of) << shift_major;
			const u32 major_b = static_cast<u16>((hi + 1) * 16 + major_of) << shift_major;
			const u32 minor_a = static_cast<u16>(minor * 16 + minor_of) << shift_minor;
			const u32 minor_b = static_cast<u16>((minor + 1) * 16 + minor_of) << shift_minor;
			const int rgba = static_cast<int>(static_cast<u32>(rgba_fog));
			const int fog = static_cast<int>(static_cast<u32>(rgba_fog >> 32));

			// Corners in the order (major lo, minor lo), (major hi, minor lo), (major lo, minor hi),
			// (major hi, minor hi).
			const GSVector4i st_a = edges[ia].st_q.insert32<2>(rgba);
			const GSVector4i st_b = edges[ib].st_q.insert32<2>(rgba);
			const GSVector4i zf_a = edges[ia].z_uv.insert32<3>(fog);
			const GSVector4i zf_b = edges[ib].z_uv.insert32<3>(fog);
			GSVector4i::store<true>(&q[0].m[0], st_a);
			GSVector4i::store<true>(&q[0].m[1], zf_a.insert32<0>(static_cast<int>(major_a | minor_a)));
			GSVector4i::store<true>(&q[1].m[0], st_b);
			GSVector4i::store<true>(&q[1].m[1], zf_b.insert32<0>(static_cast<int>(major_b | minor_a)));
			GSVector4i::store<true>(&q[2].m[0], st_a);
			GSVector4i::store<true>(&q[2].m[1], zf_a.insert32<0>(static_cast<int>(major_a | minor_b)));
			GSVector4i::store<true>(&q[3].m[0], st_b);
			GSVector4i::store<true>(&q[3].m[1], zf_b.insert32<0>(static_cast<int>(major_b | minor_b)));
		}
	};

	/// Writes the rectangles for one line (only counts them when EMIT is false) and returns how
	/// many there are. step_x is the line's major axis, X when |dx| >= |dy|.
	template <bool EMIT, bool AA1, bool step_x>
	u32 ConvertLine(const GSVertex& v0, const GSVertex& v1, const DrawConstants& dc, GSVertex* dst)
	{
		const int x0 = v0.XYZ.X - dc.ofx;
		const int y0 = v0.XYZ.Y - dc.ofy;
		const int x1 = v1.XYZ.X - dc.ofx;
		const int y1 = v1.XYZ.Y - dc.ofy;
		const int m0 = step_x ? x0 : y0;
		const int dm = step_x ? (x1 - x0) : (y1 - y0);

		// dm is zero only for a zero-length line, which lights nothing.
		if (dm == 0)
			return 0;

		LineRects lr;
		lr.v0 = &v0;
		lr.v1 = &v1;
		lr.m0 = m0;
		lr.dm = dm;
		lr.major_min = step_x ? dc.min_x : dc.min_y;
		lr.major_max = step_x ? dc.max_x : dc.max_y;
		lr.minor_min = step_x ? dc.min_y : dc.min_x;
		lr.minor_max = step_x ? dc.max_y : dc.max_x;
		lr.major_of = step_x ? dc.ofx : dc.ofy;
		lr.minor_of = step_x ? dc.ofy : dc.ofx;
		lr.shift_major = step_x ? 0 : 16;
		lr.shift_minor = step_x ? 16 : 0;
		lr.edges[0].k = INT_MIN;
		lr.edges[1].k = INT_MIN;

		// A channel that does not vary along the line is the second vertex's value throughout.
		const u32 rgba0 = v0.RGBAQ.U32[0];
		const u32 rgba1 = v1.RGBAQ.U32[0];
		const bool colour_varies = !dc.flat && rgba0 != rgba1;
		const bool fog_varies = dc.fog && v0.FOG != v1.FOG;
		const s32 sign = dm < 0 ? -1 : 1;
		s32 ca[8], e[8];
		for (int i = 0; i < 4; i++)
		{
			const s32 a = (rgba0 >> (i * 8)) & 0xFF;
			const s32 b = (rgba1 >> (i * 8)) & 0xFF;
			ca[i] = colour_varies ? a : b;
			e[i] = colour_varies ? (b - a) * sign : 0;
			const s32 fa = (v0.FOG >> (i * 8)) & 0xFF;
			const s32 fb = (v1.FOG >> (i * 8)) & 0xFF;
			ca[4 + i] = fog_varies ? fa : fb;
			e[4 + i] = fog_varies ? (fb - fa) * sign : 0;
		}
		const s32 den = dm * sign;
		// The walk steps one pixel at a time towards the far vertex along the major axis.
		const s32 dk = 16 * sign;

		u32 rects = 0;
		LineGradient grad;

		if constexpr (AA1)
		{
			// An AA1 line lights two pixels per step, and they interleave: the walk's own pixel,
			// then its neighbour on the other side of the exact line. Taking one side at a time
			// keeps a single open run, so a line that holds one coverage for its whole length --
			// every axis-aligned one -- comes out as two rectangles instead of two per pixel.
			// Nothing depends on the order: within a line no two of these pixels coincide, since
			// the two of a step differ by one on the minor axis and consecutive steps differ on
			// the major. The coverage is GSLineWalk::WalkAA1's, value for value.
			for (int side = 0; side < 2; side++)
			{
				bool started = false;
				int lo = 0, hi = 0, minor = 0;
				u64 run = 0;
				GSLineWalk::WalkStepsOnAxis<step_x>(x0, y0, x1, y1, [&](int x, int y, s64 D, s64 scale) {
					const int m = step_x ? x : y;
					int n = step_x ? y : x;
					if (started)
						grad.Step();
					else
						grad.Init(ca, e, den, m * 16 - m0, dk);
					const u64 cf = grad.Packed();
					const float cov = 0xffff * std::abs(static_cast<float>(D) / static_cast<float>(scale));
					const int covi = std::clamp(static_cast<int>(cov), 0, 0xffff);
					int pcov = 0xffff - covi;
					if (side)
					{
						n += (D >= 0) ? 1 : -1;
						pcov = covi;
					}
					const u64 c = (cf & ~0xFFFFFFFFull) | LineCoverageAlpha(static_cast<u32>(cf), pcov, dc.abe);
					if (started && n == minor && c == run)
					{
						lo = std::min(lo, m);
						hi = std::max(hi, m);
						return;
					}
					if (started)
						rects += lr.template Close<EMIT>(dst + rects * 4, lo, hi, minor, run);
					started = true;
					lo = hi = m;
					minor = n;
					run = c;
				});
				if (started)
					rects += lr.template Close<EMIT>(dst + rects * 4, lo, hi, minor, run);
			}
		}
		else
		{
			bool started = false;
			int lo = 0, hi = 0, minor = 0;
			u64 run = 0;
			GSLineWalk::WalkStepsOnAxis<step_x>(x0, y0, x1, y1, [&](int x, int y, s64, s64) {
				const int m = step_x ? x : y;
				const int n = step_x ? y : x;
				if (started)
					grad.Step();
				else
					grad.Init(ca, e, den, m * 16 - m0, dk);
				const u64 c = grad.Packed();
				if (started && n == minor && c == run)
				{
					lo = std::min(lo, m);
					hi = std::max(hi, m);
					return;
				}
				if (started)
					rects += lr.template Close<EMIT>(dst + rects * 4, lo, hi, minor, run);
				started = true;
				lo = hi = m;
				minor = n;
				run = c;
			});
			if (started)
				rects += lr.template Close<EMIT>(dst + rects * 4, lo, hi, minor, run);
		}
		return rects;
	}

	template <bool EMIT>
	u32 ConvertLine(const GSVertex& v0, const GSVertex& v1, const DrawConstants& dc, bool aa1, GSVertex* dst)
	{
		const bool step_x = GSLineWalk::Abs(v1.XYZ.X - v0.XYZ.X) >= GSLineWalk::Abs(v1.XYZ.Y - v0.XYZ.Y);
		if (aa1)
			return step_x ? ConvertLine<EMIT, true, true>(v0, v1, dc, dst) : ConvertLine<EMIT, true, false>(v0, v1, dc, dst);
		return step_x ? ConvertLine<EMIT, false, true>(v0, v1, dc, dst) : ConvertLine<EMIT, false, false>(v0, v1, dc, dst);
	}
} // namespace

u32 GSLineRuns::Capacity(const Input& in)
{
	// Convert() stops writing once it is past MAX_RECTS, which it can only notice at the end of a
	// line, so it never writes more than MAX_RECTS plus one line.
	u64 sum = 0;
	u32 largest = 0;
	for (u32 i = 0; i < in.line_count; i++)
	{
		const u32 bound = LineBound(in.vertices[in.indices[i * 2]], in.vertices[in.indices[i * 2 + 1]], in.aa1);
		sum += bound;
		largest = std::max(largest, bound);
	}
	return static_cast<u32>(std::min<u64>(sum, static_cast<u64>(MAX_RECTS) + largest));
}

GSLineRuns::Result GSLineRuns::Convert(const Input& in, GSVertex* dst, std::vector<size_t>* groups)
{
	// Corners go back into the vertex buffer as absolute 16-bit positions. Pixels whose corners
	// would not fit are dropped; they are thousands of pixels outside any target.
	const DrawConstants dc = {
		.ofx = in.ofx,
		.ofy = in.ofy,
		.min_x = -(in.ofx >> 4),
		.max_x = ((0xFFFF - in.ofx) >> 4) - 1,
		.min_y = -(in.ofy >> 4),
		.max_y = ((0xFFFF - in.ofy) >> 4) - 1,
		.flat = in.flat,
		.fog = in.fog,
		.abe = in.abe,
	};

	// The full-barrier draw list counts primitives per group. Each group's line count becomes its
	// rectangle count, so the backend's count * indices_per_prim still lands on group boundaries.
	// The new counts are held aside until the draw is known to convert, since any other outcome
	// leaves the list alone.
	const bool remap_drawlist = groups != nullptr && !groups->empty();
	thread_local std::vector<size_t> group_rects;
	group_rects.clear();
	size_t group = 0;
	size_t group_left = remap_drawlist ? (*groups)[0] : 0;
	u32 in_group = 0;

	// Past MAX_RECTS the draw is refused, but the rest of its lines are still counted: an empty
	// group later in the list takes precedence, and the refusal reports the whole count.
	u32 total = 0;
	bool emitting = true;
	for (u32 i = 0; i < in.line_count; i++)
	{
		const GSVertex& v0 = in.vertices[in.indices[i * 2]];
		const GSVertex& v1 = in.vertices[in.indices[i * 2 + 1]];
		const u32 rects = emitting ? ConvertLine<true>(v0, v1, dc, in.aa1, dst + total * 4) :
		                             ConvertLine<false>(v0, v1, dc, in.aa1, nullptr);
		total += rects;
		emitting = emitting && total <= MAX_RECTS;

		if (remap_drawlist && group < groups->size())
		{
			in_group += rects;
			if (--group_left == 0)
			{
				if (in_group == 0)
					return {Outcome::GroupEmpty, 0};
				group_rects.push_back(in_group);
				in_group = 0;
				if (++group < groups->size())
					group_left = (*groups)[group];
			}
		}
	}

	if (total == 0)
		return {Outcome::NothingLit, 0};

	if (total > MAX_RECTS)
		return {Outcome::TooMany, total};

	if (remap_drawlist)
		std::copy(group_rects.begin(), group_rects.end(), groups->begin());
	return {Outcome::Converted, total};
}
