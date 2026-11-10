// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

// What the GS does with extreme S, T and Q, for the primitives the software renderer
// resolves at the vertex: sprites and the affine (Q identically one) triangles.
// The perspective triangle has its own plane setup (GSPerspectivePlane.h) and follows
// the same rule there, including the grid and the sign of Q.
//
// Rule, per primitive, measured on an SCPH-30001 (capture gs-stq-extremes):
//
//   1. S, T and Q are ordinary numbers. A word with exponent field 255 is 2^128 times
//      its mantissa, and its own bit pattern matters. It is not an infinity or a NaN.
//   2. If Q's sign bit is set (including Q = -0), S and T are negated and |Q| is used.
//   3. Each word loses its low eight mantissa bits toward zero.
//   4. E is the exponent of the largest of |S|, |T| and |Q| over the primitive's
//      vertices. The grid is g = 2^(E - 14).
//   5. S and T floor onto g and stop at +/-32767 grid steps. |Q| floors onto g. Words
//      with exponent field zero are zero on every grid.
//   6. A Q that floored to zero multiplies by 2/g. Otherwise the multiplier is 1/|Q|
//      truncated to fifteen bits below its leading bit (see below for what is measured).
//   7. The coordinate in sixteenths of a texel is S x reciprocal x 2^TW x 16, truncated
//      toward zero. At 32768 or more it reads texel 2047, weight 15; at -32768 or less
//      it reads -2048, weight 0. Anything between takes the linear filter's half-texel
//      step unclamped, which can reach texel -2049 (GSDrawScanline.cpp applies this).
//
// Rule 6's width is the one the capture ties down at Q = 1.5 on sprites (an exact
// reciprocal and floor(x + 0.7) both read 16 of 1,056 low-bit cells there, against
// 1,055 for a fifteen-bit truncation); GS_STQ_RECIP_BITS keeps fifteen. Rule 3 is the
// cut the plane setup already uses, and the capture favours it over no cut (round 2,
// 38 cells to 6). The fifteen-bit width on the perspective route is a different
// reciprocal (GSPerspectiveRecip) and is unchanged.

static constexpr int GS_STQ_GRID_BITS = 14;      ///< g = 2^(E - 14)
static constexpr int GS_STQ_RECIP_BITS = 15;     ///< kept below the leading bit of 1/Q
static constexpr s32 GS_STQ_COUNT_MAX = 32767;   ///< grid steps S and T stop at
static constexpr u32 GS_STQ_WORD_CUT = 0xFFFFFF00u;
static constexpr s32 GS_STQ_COORD_LIMIT = (1 << 30); ///< 16.16 far past the field, kept finite for the float

/// A float word with exponent field `field` as the exponent the rule reads.
__forceinline static int GSStqWordExponent(u32 word)
{
	return static_cast<int>((word >> 23) & 0xff) - 127;
}

/// One value as a count of the grid 2^(exp - 14), floored toward minus infinity.
/// The word is cut (rule 3) and Q's sign is already moved. Field zero is zero.
__forceinline static s32 GSStqWordOnGrid(u32 word, int exp)
{
	const u32 field = (word >> 23) & 0xff;

	if (field == 0)
		return 0;

	const u32 significand = (word & 0x7fffffu) | 0x800000u;
	const int shift = 23 - GS_STQ_GRID_BITS + exp - (static_cast<int>(field) - 127);
	const bool negative = (word >> 31) != 0;
	s32 n;

	if (shift >= 32)
		n = negative ? -1 : 0;
	else if (!negative)
		n = static_cast<s32>(significand >> shift);
	else
		n = -static_cast<s32>((significand + ((1u << shift) - 1)) >> shift);

	return std::clamp(n, -GS_STQ_COUNT_MAX, GS_STQ_COUNT_MAX);
}

/// One primitive's three-word vertex (S, T, Q), as the GIF words of a vertex.
struct GSStqWords
{
	u32 s, t, q;
};

/// The counts a primitive's vertices land on: S and T signed, Q non-negative.
struct GSStqCounts
{
	s32 s, t;
	u32 q;
};

/// Applies rules 1-5 to a primitive's `n` vertices. Returns false when every word is
/// zero on every grid (the coordinate is then zero).
__forceinline static bool GSStqPrimitiveCounts(const GSStqWords* w, int n, GSStqCounts* out, int& exp)
{
	GSStqWords cut[4];
	u32 largest = 0;

	for (int i = 0; i < n; i++)
	{
		// Rule 2: the sign of Q moves to S and T, and to Q itself as |Q|.
		const u32 sign = w[i].q & 0x80000000u;

		cut[i].s = (w[i].s ^ sign) & GS_STQ_WORD_CUT;
		cut[i].t = (w[i].t ^ sign) & GS_STQ_WORD_CUT;
		cut[i].q = (w[i].q & 0x7fffffffu) & GS_STQ_WORD_CUT;

		for (u32 word : {cut[i].s & 0x7fffffffu, cut[i].t & 0x7fffffffu, cut[i].q})
			largest = std::max(largest, word);
	}

	if ((largest >> 23) == 0)
	{
		std::memset(out, 0, sizeof(GSStqCounts) * n);
		exp = 0;
		return false;
	}

	exp = static_cast<int>(largest >> 23) - 127;

	for (int i = 0; i < n; i++)
	{
		out[i].s = GSStqWordOnGrid(cut[i].s, exp);
		out[i].t = GSStqWordOnGrid(cut[i].t, exp);
		out[i].q = static_cast<u32>(GSStqWordOnGrid(cut[i].q, exp));
	}

	return true;
}

/// Rule 6-7 for one coordinate: the count `s` over the count `q` into 16.16 texels, for
/// a texture 2^tw wide, truncated toward zero. The grid cancels between S and 1/Q:
/// (s*g) * floor_K(1/(q*g)) = s * floor_K(1/q). q == 0 is the fixed reciprocal 2/g.
/// Large results come back clamped at GS_STQ_COORD_LIMIT; the scanline's field applies
/// to what a pixel reads.
__forceinline static s32 GSStqCoordinate(s32 s, u32 q, int tw)
{
	s64 value;

	if (q == 0)
	{
		value = static_cast<s64>(s) * (static_cast<s64>(1) << (17 + tw));
	}
	else
	{
		// floor_K(1/q): n in [2^K, 2^(K+1)), and 1/q = n * 2^(e-K).
		const int b = 31 - std::countl_zero(q);
		const bool pow2 = (q & (q - 1)) == 0;
		const int e = pow2 ? -b : -(b + 1);
		const u64 n = pow2 ? (static_cast<u64>(1) << GS_STQ_RECIP_BITS) :
		                     (static_cast<u64>(1) << (GS_STQ_RECIP_BITS + b + 1)) / q;

		// s * n * 2^(e-K) * 2^(16+tw) = (s * n) * 2^(e + 1 + tw).
		const int shift = e + 1 + tw;
		const s64 product = static_cast<s64>(s) * static_cast<s64>(n);

		if (shift >= 0)
			value = product * (static_cast<s64>(1) << shift);
		else
			value = (product < 0 ? -1 : 1) * (std::abs(product) >> -shift);
	}

	return static_cast<s32>(std::clamp<s64>(value, -GS_STQ_COORD_LIMIT, GS_STQ_COORD_LIMIT));
}

/// A 16.16 value as a float, cut toward zero rather than rounded, so the float the
/// scanline converts back never crosses the sixteenth above the true value.
__forceinline static float GSStqCoordinateFloat(s32 c)
{
	float f = static_cast<float>(c);

	if (std::abs(static_cast<s64>(f)) > std::abs(static_cast<s64>(c)))
		f = std::nextafter(f, 0.0f);

	return f;
}

/// Why a primitive needs the rule rather than the ordinary route.
enum GSStqDrawNeed : u32
{
	GS_STQ_NEED_RAW_WORDS = 1,       ///< a word cannot survive the 2^(16+TW) scale
	GS_STQ_NEED_FIELD_EDGE = 2,      ///< the scanline may see the field's limits, or Q floored to zero
	GS_STQ_NEED_PRIMITIVE_RULE = 4,  ///< a sprite or affine triangle needs its vertices rewritten
};

/// Classifies one primitive's words for a draw on texture 2^tw x 2^th: `sprite` when
/// the vertices are a sprite's pair, `plane` when the route is the perspective plane.
/// Conservative (false positives only cost a few integer operations and a JIT key).
__forceinline static u32 GSStqClassifyPrimitive(const GSStqWords* w, int n, bool sprite, bool plane, int tw, int th)
{
	u32 need = 0;
	int efield = 0;
	int efields[4] = {};
	int qfields[4] = {};
	bool q_nonpositive = false;
	bool q_pow2 = true;

	for (int i = 0; i < n; i++)
	{
		const int fs = (w[i].s >> 23) & 0xff;
		const int ft = (w[i].t >> 23) & 0xff;
		const int fq = (w[i].q >> 23) & 0xff;

		efields[i] = std::max(fs, ft);
		efield = std::max({efield, fs, ft, fq});
		qfields[i] = fq;

		if (fq == 0 || (w[i].q >> 31) != 0)
			q_nonpositive = true;

		if ((w[i].q & 0x7fffffu) != 0)
			q_pow2 = false;

		if (fs >= 230 || ft >= 230 || fq >= 230)
			need |= GS_STQ_NEED_RAW_WORDS;
	}

	const int grid_gate = 11 - std::max(tw, th);
	const bool visible_grid = (efield - 127) >= grid_gate;
	bool sub_grid_q = false;
	bool field_edge = false;

	for (int i = 0; i < n; i++)
	{
		if (qfields[i] < efield - GS_STQ_GRID_BITS)
			sub_grid_q = true;

		// Where |S/Q| or |T/Q| can reach 2047 texels, the half-texel order at the field's
		// edge is visible, and a zero Q count is the fixed reciprocal.
		if ((efields[i] - qfields[i]) + std::max(tw, th) >= 10)
			field_edge = true;
	}

	if (q_nonpositive || sub_grid_q || field_edge || (need & GS_STQ_NEED_RAW_WORDS))
		need |= GS_STQ_NEED_FIELD_EDGE;

	// A sprite with a Q that is not a power of two divides by a truncated reciprocal the
	// vertex divide cannot reproduce. A plane triangle resolves all of this at setup.
	if (!plane && (q_nonpositive || sub_grid_q || visible_grid || field_edge || (need & GS_STQ_NEED_RAW_WORDS) || (sprite && !q_pow2)))
		need |= GS_STQ_NEED_PRIMITIVE_RULE;

	return need;
}
