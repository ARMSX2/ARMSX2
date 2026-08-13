// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The Tile renderer's depth transcription evaluates the SW scanline's float64
// depth walk in-shader, on devices with no float64, through a soft-float kernel
// built from 32-bit integer ops. This file holds the C++ MIRROR of that kernel:
// op-for-op the same algorithm as the GLSL in
// bin/resources/shaders/vulkan/tfx.glsl (PS_TILE_ZWALK), written against
// GLSL-primitive shims (umulExtended / uaddCarry / findMSB) so the
// transliteration is mechanical and this suite's verdict transfers.
//
// ⚠️ If you change the kernel here, change the shader, and vice versa — the
// probe (umbrella devs/bmdhacks/probes/gs-zgrad) arbitrates the pair end to
// end, but this suite is what makes a kernel bug findable in seconds instead
// of a shader-debug session.
//
// Envelope contract (guarded by the Tile draw lowering, asserted here):
// finite normals and zeros only — no NaN, no Inf, no denormals — and result
// exponents stay far from both ends. Zero SIGNS are not faithful (a zero
// product returns c as-is); every observable is truncated to an integer, so
// zero sign cannot matter.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <random>

#include "common/Pcsx2Defs.h"

namespace glsl_mirror
{
	// ---- GLSL primitive shims (the only 64-bit escapes allowed) ----
	static inline void umulExtended(u32 a, u32 b, u32& hi, u32& lo)
	{
		const u64 p = static_cast<u64>(a) * b;
		hi = static_cast<u32>(p >> 32);
		lo = static_cast<u32>(p);
	}
	static inline u32 uaddCarry(u32 a, u32 b, u32& carry)
	{
		const u64 s = static_cast<u64>(a) + b;
		carry = static_cast<u32>(s >> 32);
		return static_cast<u32>(s);
	}
	static inline int findMSB(u32 v) // GLSL: -1 for 0, else bit index of the highest set bit
	{
		return v == 0 ? -1 : 31 - __builtin_clz(v);
	}

	struct uvec2
	{
		u32 x, y; // f64 bits: x = low word, y = high word (sign/exponent/mantissa top)
	};

	// ---- 160-bit spine helpers (5 x u32, little-endian) ----
	// GLSL shifts by >= 32 are undefined; every shift below is split into a
	// word part and a guarded bit part.

	static inline void spine_clear(u32 S[5])
	{
		S[0] = S[1] = S[2] = S[3] = S[4] = 0u;
	}

	// OR a value of up to 64 significant bits (lo, hi) into the spine at bit
	// offset `shift` (0..96). The value must fit: shift + 64 <= 160.
	static inline void spine_or64(u32 S[5], u32 lo, u32 hi, u32 shift)
	{
		const u32 w = shift >> 5;
		const u32 b = shift & 31u;
		if (b == 0u)
		{
			S[w + 0] |= lo;
			S[w + 1] |= hi;
		}
		else
		{
			S[w + 0] |= lo << b;
			S[w + 1] |= (lo >> (32u - b)) | (hi << b);
			S[w + 2] |= hi >> (32u - b);
		}
	}

	// OR a value of up to 96 significant bits (three words) into the spine at
	// bit offset `shift` (0..64).
	static inline void spine_or96(u32 S[5], u32 w0, u32 w1, u32 w2, u32 shift)
	{
		const u32 w = shift >> 5;
		const u32 b = shift & 31u;
		if (b == 0u)
		{
			S[w + 0] |= w0;
			S[w + 1] |= w1;
			S[w + 2] |= w2;
		}
		else
		{
			S[w + 0] |= w0 << b;
			S[w + 1] |= (w0 >> (32u - b)) | (w1 << b);
			S[w + 2] |= (w1 >> (32u - b)) | (w2 << b);
			S[w + 3] |= w2 >> (32u - b);
		}
	}

	static inline void spine_add(u32 R[5], const u32 A[5], const u32 B[5])
	{
		u32 c = 0u, c2 = 0u;
		for (int i = 0; i < 5; i++)
		{
			const u32 s = uaddCarry(A[i], B[i], c2);
			R[i] = uaddCarry(s, c, c);
			c += c2;
		}
	}

	// R = A - B, assuming A >= B.
	static inline void spine_sub(u32 R[5], const u32 A[5], const u32 B[5])
	{
		u32 borrow = 0u;
		for (int i = 0; i < 5; i++)
		{
			const u32 bi = B[i];
			const u32 t = A[i] - bi;
			const u32 b1 = (A[i] < bi) ? 1u : 0u;
			const u32 t2 = t - borrow;
			const u32 b2 = (t < borrow) ? 1u : 0u;
			R[i] = t2;
			borrow = b1 + b2;
		}
	}

	// -1 if A < B, 0 if equal, +1 if A > B.
	static inline int spine_cmp(const u32 A[5], const u32 B[5])
	{
		for (int i = 4; i >= 0; i--)
		{
			if (A[i] != B[i])
				return (A[i] < B[i]) ? -1 : 1;
		}
		return 0;
	}

	static inline int spine_msb(const u32 S[5])
	{
		for (int i = 4; i >= 0; i--)
		{
			const int m = findMSB(S[i]);
			if (m >= 0)
				return i * 32 + m;
		}
		return -1;
	}

	// Read bit `pos` of the spine (pos may be anything; out-of-range reads 0).
	static inline u32 spine_bit(const u32 S[5], int pos)
	{
		if (pos < 0 || pos >= 160)
			return 0u;
		return (S[pos >> 5] >> (static_cast<u32>(pos) & 31u)) & 1u;
	}

	// OR of all bits strictly below `pos`.
	static inline u32 spine_sticky_below(const u32 S[5], int pos)
	{
		if (pos <= 0)
			return 0u;
		if (pos >= 160)
			pos = 160;
		const u32 w = static_cast<u32>(pos) >> 5;
		const u32 b = static_cast<u32>(pos) & 31u;
		u32 acc = 0u;
		for (u32 i = 0; i < w; i++)
			acc |= S[i];
		if (b != 0u)
			acc |= S[w] & ((1u << b) - 1u);
		return (acc != 0u) ? 1u : 0u;
	}

	// Extract the 53-bit window [msb-52, msb] of the spine into (lo, hi21).
	// Only called with msb >= 53, so the window start is always positive.
	static inline void spine_extract53(const u32 S[5], int msb, u32& lo, u32& hi)
	{
		const u32 start = static_cast<u32>(msb - 52);
		const u32 w = start >> 5;
		const u32 b = start & 31u;
		u32 out_lo, out_hi;
		if (b == 0u)
		{
			out_lo = S[w + 0];
			out_hi = (w + 1 < 5) ? S[w + 1] : 0u;
		}
		else
		{
			const u32 hi1 = (w + 1 < 5) ? S[w + 1] : 0u;
			const u32 hi2 = (w + 2 < 5) ? S[w + 2] : 0u;
			out_lo = (S[w] >> b) | (hi1 << (32u - b));
			out_hi = (hi1 >> b) | (hi2 << (32u - b));
		}
		lo = out_lo;
		hi = out_hi & 0x1FFFFFu; // keep 53 bits total (21 in the high word)
	}

	// ---- the kernel ----

	// round(a * b + c) with a single IEEE round-to-nearest-even rounding, for
	// a, c float64 (as bit pairs) and b float32. Envelope: normals and zeros
	// only; no denormal, NaN or Inf in or out; zero signs not faithful.
	static uvec2 zw_fma(uvec2 a, float b, uvec2 c)
	{
		const u32 ea = (a.y >> 20) & 0x7FFu;
		u32 bb;
		std::memcpy(&bb, &b, 4);
		const u32 eb = (bb >> 23) & 0xFFu;

		// A zero product contributes nothing; the sum rounds to c exactly.
		if (ea == 0u || eb == 0u)
			return c;

		const u32 sa = a.y >> 31;
		const u32 sb = bb >> 31;
		const u32 sp = sa ^ sb;

		const u32 ma_lo = a.x;
		const u32 ma_hi = (a.y & 0xFFFFFu) | 0x100000u; // 53-bit mantissa, high 21 bits
		const u32 mb = (bb & 0x7FFFFFu) | 0x800000u;    // 24-bit mantissa

		const u32 sc = c.y >> 31;
		const u32 ec = (c.y >> 20) & 0x7FFu;
		const u32 mc_lo = c.x;
		const u32 mc_hi = (c.y & 0xFFFFFu) | ((ec != 0u) ? 0x100000u : 0u);

		// P = ma * mb: 53 x 24 -> up to 77 bits, in three words.
		u32 p0h, p0l, p1h, p1l;
		umulExtended(ma_lo, mb, p0h, p0l);
		umulExtended(ma_hi, mb, p1h, p1l);
		u32 carry;
		const u32 P0 = p0l;
		const u32 P1 = uaddCarry(p0h, p1l, carry);
		const u32 P2 = p1h + carry; // ma_hi*mb < 2^45, so no carry out of P2

		// Scales: a = ma * 2^(ea-1075), b = mb * 2^(eb-150), c = mc * 2^(ec-1075).
		// The product's scale ("anchor") and c's shift relative to it:
		const int anchor = static_cast<int>(ea) + static_cast<int>(eb) - 1225;
		const int shift = (static_cast<int>(ec) - 1075) - anchor; // c's bit offset on the product's grid

		// c so dominant the product is under half an ulp of c: the sum rounds
		// to c exactly (strict inequality — no tie is reachable).
		if (ec != 0u && shift >= 82)
			return c;

		// Product so dominant that c is far below the guard bit: c is pure
		// sticky, and at an exact tie its SIGN decides the direction.
		const bool c_is_sticky = (shift <= -80);

		// Build both terms on a 160-bit spine anchored at grid = anchor - up.
		const u32 up = c_is_sticky ? 0u : static_cast<u32>((shift < 0) ? -shift : 0);
		const int grid = anchor - static_cast<int>(up);

		u32 SP[5], SC[5];
		spine_clear(SP);
		spine_clear(SC);
		spine_or96(SP, P0, P1, P2, up);
		if (ec != 0u && !c_is_sticky)
			spine_or64(SC, mc_lo, mc_hi, static_cast<u32>((shift > 0) ? shift : 0));

		// Signed sum.
		u32 SUM[5];
		u32 ssign;
		u32 sticky_extra = 0u;
		u32 sticky_sign = 0u; // meaningful only with sticky_extra
		if (c_is_sticky)
		{
			for (int i = 0; i < 5; i++)
				SUM[i] = SP[i];
			ssign = sp;
			sticky_extra = (ec != 0u && (mc_lo | mc_hi) != 0u) ? 1u : 0u;
			sticky_sign = (sc == sp) ? 0u : 1u; // 0: pushes away from zero, 1: toward zero
		}
		else if (sp == sc || ec == 0u)
		{
			spine_add(SUM, SP, SC);
			ssign = sp;
		}
		else
		{
			const int cmp = spine_cmp(SP, SC);
			if (cmp == 0)
				return uvec2{0u, 0u}; // exact cancellation: +0 (RNE)
			if (cmp > 0)
			{
				spine_sub(SUM, SP, SC);
				ssign = sp;
			}
			else
			{
				spine_sub(SUM, SC, SP);
				ssign = sc;
			}
		}

		// Normalize and round to 53 bits, RNE.
		const int msb = spine_msb(SUM);
		if (msb < 0)
			return uvec2{0u, 0u};

		u32 mant_lo, mant_hi;
		int e2; // result = mant * 2^e2, mant in [2^52, 2^53) after normalization
		if (msb <= 52)
		{
			// Exact: 53 or fewer bits, no rounding. Shift up so the top bit
			// lands at bit 52 for packing; the exponent absorbs the shift.
			const u32 sh = static_cast<u32>(52 - msb);
			const u32 w = sh >> 5;
			const u32 bsh = sh & 31u;
			u32 lo = SUM[0], hi = SUM[1];
			if (w == 1u)
			{
				hi = lo;
				lo = 0u;
			}
			if (bsh != 0u)
			{
				hi = (hi << bsh) | (lo >> (32u - bsh));
				lo = lo << bsh;
			}
			mant_lo = lo;
			mant_hi = hi & 0x1FFFFFu;
			e2 = grid - static_cast<int>(sh);
		}
		else
		{
			spine_extract53(SUM, msb, mant_lo, mant_hi);
			e2 = grid + (msb - 52);
			const u32 guard = spine_bit(SUM, msb - 53);
			u32 sticky = spine_sticky_below(SUM, msb - 53) | sticky_extra;
			u32 round_up = 0u;
			if (guard != 0u)
			{
				if (sticky != 0u)
				{
					// Above the tie — unless the sticky is an opposite-signed
					// tail (c far below), which pulls the true value BELOW the
					// tie: round down.
					round_up = (sticky_extra != 0u && sticky_sign != 0u &&
					            spine_sticky_below(SUM, msb - 53) == 0u) ? 0u : 1u;
				}
				else
				{
					round_up = mant_lo & 1u; // exact tie: round to even
				}
			}
			if (round_up != 0u)
			{
				u32 cr;
				mant_lo = uaddCarry(mant_lo, 1u, cr);
				mant_hi += cr;
				if (mant_hi == 0x200000u) // 2^53: renormalize
				{
					mant_hi = 0x100000u;
					mant_lo = 0u;
					e2 += 1;
				}
			}
		}

		// Pack. value = mant * 2^e2 with the implicit bit at 52 => biased
		// exponent = e2 + 52 + 1023.
		const int E = e2 + 1075;
		if (E <= 0)
			return uvec2{0u, ssign << 31}; // envelope: unreachable; flush to zero
		// (E >= 2047 is likewise unreachable within the envelope.)
		uvec2 r;
		r.x = mant_lo;
		r.y = (ssign << 31) | (static_cast<u32>(E) << 20) | (mant_hi & 0xFFFFFu);
		return r;
	}

	// Truncate toward zero to u32, matching the scanline's store conversion for
	// in-envelope values (negative -> 0 is a guard, not scanline semantics; the
	// draw lowering floors any draw whose walk could go negative).
	static u32 zw_trunc_u32(uvec2 z)
	{
		const u32 s = z.y >> 31;
		const u32 e = (z.y >> 20) & 0x7FFu;
		if (s != 0u || e < 1023u)
			return 0u;
		if (e > 1054u)
			return 0x7FFFFFFFu; // fcvtzs saturation; unreachable within the envelope
		const u32 mant_hi = (z.y & 0xFFFFFu) | 0x100000u;
		const u32 mant_lo = z.x;
		// value = mant * 2^(e-1075); integer part = mant >> (1075 - e).
		const u32 sh = 1075u - e;
		if (sh >= 53u)
			return 0u; // e < 1023 already handled; unreachable
		if (sh >= 32u)
			return mant_hi >> (sh - 32u);
		if (sh == 0u)
			return mant_lo; // e == 1075: value >= 2^52 — outside envelope
		return (mant_lo >> sh) | (mant_hi << (32u - sh));
	}

	// Exact conversion of a signed 64-bit count of 2^scale units to float64.
	// |v| must have at most 53 significant bits (envelope-guaranteed).
	static uvec2 zw_f64_from_i64(u32 neg, u32 mag_lo, u32 mag_hi, int scale)
	{
		if ((mag_lo | mag_hi) == 0u)
			return uvec2{0u, 0u};
		int msb = findMSB(mag_hi);
		msb = (msb >= 0) ? msb + 32 : findMSB(mag_lo);
		// Shift so the top bit lands at 52.
		u32 lo = mag_lo, hi = mag_hi;
		if (msb < 52)
		{
			const u32 sh = static_cast<u32>(52 - msb);
			if (sh >= 32u)
			{
				hi = lo << (sh - 32u);
				lo = 0u;
			}
			else
			{
				hi = (hi << sh) | (lo >> (32u - sh));
				lo = lo << sh;
			}
		}
		// msb > 52 cannot happen: the value fits 53 bits by contract.
		const int E = (scale + msb) + 1023;
		uvec2 r;
		r.x = lo;
		r.y = (neg << 31) | (static_cast<u32>(E) << 20) | (hi & 0xFFFFFu);
		return r;
	}

	// (neg, mag) signed 64-bit addition: same sign adds magnitudes, opposite
	// signs subtract the smaller from the larger. Zero comes out positive.
	static void zw_i64_signed_add(u32 na, u32 alo, u32 ahi, u32 nb, u32 blo, u32 bhi,
		u32& nr, u32& rlo, u32& rhi)
	{
		if (na == nb)
		{
			u32 carry;
			rlo = uaddCarry(alo, blo, carry);
			rhi = ahi + bhi + carry;
			nr = na;
			return;
		}
		const bool a_ge_b = (ahi > bhi) || (ahi == bhi && alo >= blo);
		const u32 xlo = a_ge_b ? alo : blo;
		const u32 xhi = a_ge_b ? ahi : bhi;
		const u32 ylo = a_ge_b ? blo : alo;
		const u32 yhi = a_ge_b ? bhi : ahi;
		const u32 borrow = (xlo < ylo) ? 1u : 0u;
		rlo = xlo - ylo;
		rhi = xhi - yhi - borrow;
		nr = ((rlo | rhi) == 0u) ? 0u : (a_ge_b ? na : nb);
	}

	// (lo, hi) * k for values whose product fits 64 bits.
	static void zw_u64_mul_u32(u32 alo, u32 ahi, u32 k, u32& rlo, u32& rhi)
	{
		u32 hi0, lo0;
		umulExtended(alo, k, hi0, lo0);
		rlo = lo0;
		rhi = hi0 + ahi * k;
	}

	// f32 -> signed count of 2^-10 units, exact by construction (the input is
	// always a lane offset: an fp32 product of the narrowed gradient and a
	// small integer, which lives on the 2^-10 grid — see the setup analysis).
	static void zw_i64_units10_from_f32(float v, u32& neg, u32& mag_lo, u32& mag_hi)
	{
		u32 bb;
		std::memcpy(&bb, &v, 4);
		neg = bb >> 31;
		const u32 e = (bb >> 23) & 0xFFu;
		if (e == 0u)
		{
			neg = 0u;
			mag_lo = mag_hi = 0u;
			return;
		}
		const u32 m = (bb & 0x7FFFFFu) | 0x800000u;
		// value = m * 2^(e-150); in 2^-10 units: m * 2^(e-140).
		const int sh = static_cast<int>(e) - 140;
		if (sh >= 0)
		{
			const u32 s = static_cast<u32>(sh);
			if (s >= 32u)
			{
				mag_hi = m << (s - 32u);
				mag_lo = 0u;
			}
			else if (s == 0u)
			{
				mag_lo = m;
				mag_hi = 0u;
			}
			else
			{
				mag_lo = m << s;
				mag_hi = m >> (32u - s);
			}
		}
		else
		{
			// On-grid inputs only: the shifted-out bits are zero.
			const u32 s = static_cast<u32>(-sh);
			mag_lo = (s < 32u) ? (m >> s) : 0u;
			mag_hi = 0u;
		}
	}
} // namespace glsl_mirror

namespace
{
	using glsl_mirror::uvec2;

	uvec2 bits_of(double d)
	{
		u64 b;
		std::memcpy(&b, &d, 8);
		return uvec2{static_cast<u32>(b), static_cast<u32>(b >> 32)};
	}
	double val_of(uvec2 v)
	{
		const u64 b = (static_cast<u64>(v.y) << 32) | v.x;
		double d;
		std::memcpy(&d, &b, 8);
		return d;
	}
	bool same_bits_mod_zero(uvec2 a, double d)
	{
		const uvec2 b = bits_of(d);
		if (a.x == b.x && a.y == b.y)
			return true;
		// Zero signs are documented as unfaithful.
		const bool az = ((a.y & 0x7FFFFFFFu) | a.x) == 0u;
		const bool bz = ((b.y & 0x7FFFFFFFu) | b.x) == 0u;
		return az && bz;
	}

	// Random double with exponent in [emin, emax] (unbiased), random mantissa/sign.
	double rand_double(std::mt19937_64& rng, int emin, int emax)
	{
		const u64 mant = rng() & 0xFFFFFFFFFFFFFull;
		const int e = emin + static_cast<int>(rng() % static_cast<u64>(emax - emin + 1));
		const u64 sign = (rng() & 1ull) << 63;
		const u64 bits = sign | (static_cast<u64>(e + 1023) << 52) | mant;
		double d;
		std::memcpy(&d, &bits, 8);
		return d;
	}
	float rand_float(std::mt19937_64& rng, int emin, int emax)
	{
		const u32 mant = static_cast<u32>(rng()) & 0x7FFFFFu;
		const int e = emin + static_cast<int>(rng() % static_cast<u64>(emax - emin + 1));
		const u32 sign = static_cast<u32>(rng() & 1ull) << 31;
		const u32 bits = sign | (static_cast<u32>(e + 127) << 23) | mant;
		float f;
		std::memcpy(&f, &bits, 4);
		return f;
	}
} // namespace

TEST(GSTileZWalkSoftFloat, FmaDirected)
{
	// Zeros and identity multipliers.
	EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(0.0), 5.0f, bits_of(7.0)), 7.0));
	EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(3.0), 0.0f, bits_of(7.0)), 7.0));
	EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(3.0), 1.0f, bits_of(4.0)), 7.0));
	EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(3.0), 1.0f, bits_of(-3.0)), 0.0));
	EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(1e9), 1.0f, bits_of(0.0)), 1e9));

	// The row-seed shapes: full-mantissa gradient x small dy + integer seed.
	{
		const double dedge_z = -0.10948437500000001; // arbitrary 53-bit value
		const float dy = 37.0f;
		const double z0 = 524288.0;
		const double want = std::fma(dedge_z, static_cast<double>(dy), z0);
		EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(dedge_z), dy, bits_of(z0)), want));
	}

	// Exact ties must round to even, both directions.
	{
		// mant ...0 + exactly half an ulp -> stays; mant ...1 + half -> up.
		const double base_even = 4503599627370496.0; // 2^52, ulp = 1, LSB 0
		const double base_odd = 4503599627370497.0;  // LSB 1
		EXPECT_TRUE(same_bits_mod_zero(
			glsl_mirror::zw_fma(bits_of(0.25), 2.0f, bits_of(base_even)),
			std::fma(0.25, 2.0, base_even)));
		EXPECT_TRUE(same_bits_mod_zero(
			glsl_mirror::zw_fma(bits_of(0.25), 2.0f, bits_of(base_odd)),
			std::fma(0.25, 2.0, base_odd)));
	}

	// Product at an exact tie with a tiny far-below c of each sign: the sticky
	// tail's sign must decide the direction.
	{
		const double a = 1.5;         // x 3.0f -> 4.5: guard set, no sticky at some scale
		const float b = 3.0f;
		const double big = 4503599627370496.0; // 2^52 so 4.5 lands at guard/below
		// c = big makes the product the small term; instead test the dominant-P
		// path: P = a*b + tiny c.
		const double tiny_pos = 0x1p-60;
		const double tiny_neg = -0x1p-60;
		// Choose a, b so a*b has a one-bit tail exactly at the guard position of
		// a larger sum — easier to just trust std::fma as oracle over a sweep:
		for (int k = 0; k < 64; k++)
		{
			const double aa = 1.0 + std::ldexp(1.0, -k);
			const double want_p = std::fma(aa, static_cast<double>(b), tiny_pos);
			const double want_n = std::fma(aa, static_cast<double>(b), tiny_neg);
			EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(aa), b, bits_of(tiny_pos)), want_p)) << "k=" << k;
			EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(aa), b, bits_of(tiny_neg)), want_n)) << "k=" << k;
		}
		(void)a;
		(void)big;
	}

	// Deep cancellation: a*b and c nearly equal with opposite signs.
	for (int k = 1; k < 52; k++)
	{
		const double a = 1.0 + std::ldexp(1.0, -k);
		const float b = 1.0f;
		const double c = -1.0;
		const double want = std::fma(a, static_cast<double>(b), c);
		EXPECT_TRUE(same_bits_mod_zero(glsl_mirror::zw_fma(bits_of(a), b, bits_of(c)), want)) << "k=" << k;
	}

	// The far-below-sticky path at an EXACT product tie: the product
	// (1 + 2^-30)(1 + 2^-23) = 1 + 2^-23 + 2^-30 + 2^-53 has its guard bit set
	// with nothing below — a true tie, mantissa even, so RNE alone rounds down.
	// A c term ~2^-90 sits far below the spine (pure sticky) and must break the
	// tie by its sign: up when it agrees with the product, down when it opposes.
	{
		const double a = 1.0 + 0x1p-30;
		const float b = 1.0f + 0x1p-23f;
		for (const double c : {0.0, 0x1p-90, -0x1p-90})
		{
			const double want = std::fma(a, static_cast<double>(b), c);
			const uvec2 got = glsl_mirror::zw_fma(bits_of(a), b, bits_of(c));
			EXPECT_TRUE(same_bits_mod_zero(got, want)) << "c=" << c << " want=" << want << " got=" << val_of(got);
		}
	}
}

TEST(GSTileZWalkSoftFloat, FmaFuzzEnvelope)
{
	// The shapes the depth walk actually feeds the kernel, wide margins.
	std::mt19937_64 rng(0x5eed0001);
	for (int i = 0; i < 2000000; i++)
	{
		const double a = rand_double(rng, -40, 40);   // gradients
		float b;
		if ((i & 3) == 0)
			b = 1.0f; // the add case
		else
			b = rand_float(rng, -4, 11); // dy / prestep magnitudes
		const double c = (i & 1) ? static_cast<double>(rng() & 0xFFFFFFFFull) // integer seeds
		                         : rand_double(rng, -10, 33);
		const double want = std::fma(a, static_cast<double>(b), c);
		const uvec2 got = glsl_mirror::zw_fma(bits_of(a), b, bits_of(c));
		ASSERT_TRUE(same_bits_mod_zero(got, want))
			<< "i=" << i << " a=" << a << " b=" << b << " c=" << c
			<< " want=" << want << " got=" << val_of(got);
	}
}

TEST(GSTileZWalkSoftFloat, FmaFuzzWide)
{
	// Beyond the envelope but inside normals: dominance both ways, random signs.
	std::mt19937_64 rng(0x5eed0002);
	for (int i = 0; i < 2000000; i++)
	{
		const double a = rand_double(rng, -120, 120);
		const float b = rand_float(rng, -60, 60);
		const double c = rand_double(rng, -120, 120);
		const double want = std::fma(a, static_cast<double>(b), c);
		// Skip results outside normal range (the kernel documents them out).
		if (want != 0.0 && (std::abs(want) < 0x1p-1000 || std::abs(want) > 0x1p1000))
			continue;
		const uvec2 got = glsl_mirror::zw_fma(bits_of(a), b, bits_of(c));
		ASSERT_TRUE(same_bits_mod_zero(got, want))
			<< "i=" << i << " a=" << a << " b=" << b << " c=" << c
			<< " want=" << want << " got=" << val_of(got);
	}
}

TEST(GSTileZWalkSoftFloat, TruncU32)
{
	EXPECT_EQ(glsl_mirror::zw_trunc_u32(bits_of(0.0)), 0u);
	EXPECT_EQ(glsl_mirror::zw_trunc_u32(bits_of(0.999999999)), 0u);
	EXPECT_EQ(glsl_mirror::zw_trunc_u32(bits_of(1.0)), 1u);
	EXPECT_EQ(glsl_mirror::zw_trunc_u32(bits_of(-3.5)), 0u);
	EXPECT_EQ(glsl_mirror::zw_trunc_u32(bits_of(16777215.9999)), 16777215u);
	EXPECT_EQ(glsl_mirror::zw_trunc_u32(bits_of(1048576.0)), 1048576u);

	std::mt19937_64 rng(0x5eed0003);
	for (int i = 0; i < 1000000; i++)
	{
		const double z = std::ldexp(static_cast<double>(rng() & 0xFFFFFFFFFFFFFull), -20);
		if (z >= 0x1p31)
			continue;
		ASSERT_EQ(glsl_mirror::zw_trunc_u32(bits_of(z)), static_cast<u32>(z)) << "z=" << z;
	}
}

TEST(GSTileZWalkSoftFloat, I64Conversions)
{
	std::mt19937_64 rng(0x5eed0004);

	// f32 lane offsets on the 2^-10 grid, exact by construction.
	for (int i = 0; i < 1000000; i++)
	{
		// Build an on-grid float: an integer count of 2^-10 units that fits 24 bits
		// of significance at its magnitude.
		const s64 units = static_cast<s64>(rng() % (1ll << 36)) - (1ll << 35);
		const float f = static_cast<float>(static_cast<double>(units) * 0x1p-10);
		const double fv = static_cast<double>(f);
		const s64 want = static_cast<s64>(fv * 1024.0); // exact: f is on the grid
		u32 neg, lo, hi;
		glsl_mirror::zw_i64_units10_from_f32(f, neg, lo, hi);
		const s64 mag = static_cast<s64>((static_cast<u64>(hi) << 32) | lo);
		const s64 got = neg ? -mag : mag;
		ASSERT_EQ(got, want) << "f=" << f;
	}

	// i64 -> f64 exact within 53 bits, at the two scales the walk uses.
	for (int i = 0; i < 1000000; i++)
	{
		const s64 v = static_cast<s64>(rng() % (1ll << 50)) - (1ll << 49);
		const int scale = (i & 1) ? -10 : -14;
		const u32 neg = v < 0 ? 1u : 0u;
		const u64 mag = neg ? static_cast<u64>(-v) : static_cast<u64>(v);
		const uvec2 got = glsl_mirror::zw_f64_from_i64(neg, static_cast<u32>(mag), static_cast<u32>(mag >> 32), scale);
		const double want = std::ldexp(static_cast<double>(v), scale);
		ASSERT_TRUE(same_bits_mod_zero(got, want)) << "v=" << v << " scale=" << scale;
	}
}

// The composed per-fragment evaluation against a closed-form double oracle:
// zrow = fma(dedge_z, dy, zseed); zrow = fma(dscan_z, prestep, zrow);
// z = zrow + (off + steps), truncated. The kernel path must match bit-for-bit
// because every kernel op is exactly the corresponding IEEE op.
TEST(GSTileZWalkSoftFloat, ComposedWalk)
{
	std::mt19937_64 rng(0x5eed0005);
	for (int i = 0; i < 500000; i++)
	{
		// Envelope-shaped inputs.
		const u32 zseed = static_cast<u32>(rng()) & 0xFFFFFF;
		const double dedge_z = (i % 5 == 0) ? 0.0 : rand_double(rng, -20, 10);
		const s64 m1024 = static_cast<s64>(rng() % (1ll << 30)) - (1ll << 29); // M, |M| < 2^29 here
		const double dscan_z = static_cast<double>(m1024) * 0x1p-10;
		const float dz32 = static_cast<float>(dscan_z);
		const float dy = static_cast<float>(static_cast<int>(rng() % 2048)) -
		                 static_cast<float>(static_cast<int>(rng() % 32)) / 16.0f;
		const float prestep = static_cast<float>(static_cast<int>(rng() % 1024)) +
		                      static_cast<float>(static_cast<int>(rng() % 16)) / 16.0f;
		const int lane = static_cast<int>(rng() % 7) - 3;
		const int q = static_cast<int>(rng() % 512);

		// Oracle in hardware doubles (closed form).
		const double zrow1 = std::fma(dedge_z, static_cast<double>(dy), static_cast<double>(zseed));
		const double zrow2 = std::fma(dscan_z, static_cast<double>(prestep), zrow1);
		const float offf = dz32 * static_cast<float>(lane);
		const s64 off10 = static_cast<s64>(static_cast<double>(offf) * 1024.0);
		const s64 walk10 = off10 + static_cast<s64>(q) * (4 * m1024);
		const double zfin = zrow2 + std::ldexp(static_cast<double>(walk10), -10);
		if (zfin < 0.0 || zfin >= 0x1p31)
			continue;
		const u32 want = static_cast<u32>(zfin);

		// Kernel path, walk assembly through the mirrored i64 helpers exactly as
		// the shader runs them.
		const uvec2 k1 = glsl_mirror::zw_fma(bits_of(dedge_z), dy, bits_of(static_cast<double>(zseed)));
		const uvec2 k2 = glsl_mirror::zw_fma(bits_of(dscan_z), prestep, k1);
		u32 oneg, olo, ohi;
		glsl_mirror::zw_i64_units10_from_f32(offf, oneg, olo, ohi);
		const u64 m4 = static_cast<u64>(m1024 < 0 ? -m1024 : m1024) * 4; // the payload's |4M|
		u32 slo, shi;
		glsl_mirror::zw_u64_mul_u32(static_cast<u32>(m4), static_cast<u32>(m4 >> 32), static_cast<u32>(q), slo, shi);
		const u32 sneg = (m1024 < 0) ? 1u : 0u; // q >= 0 in this test; the shader XORs in q's sign
		u32 wneg, wlo, whi;
		glsl_mirror::zw_i64_signed_add(oneg, olo, ohi, sneg, slo, shi, wneg, wlo, whi);
		const uvec2 wf = glsl_mirror::zw_f64_from_i64(wneg, wlo, whi, -10);
		const uvec2 k3 = glsl_mirror::zw_fma(wf, 1.0f, k2);
		const u32 got = glsl_mirror::zw_trunc_u32(k3);

		ASSERT_EQ(got, want) << "i=" << i << " zseed=" << zseed << " dedge_z=" << dedge_z
		                     << " dscan_z=" << dscan_z << " dy=" << dy << " prestep=" << prestep
		                     << " lane=" << lane << " q=" << q << " zfin=" << zfin;
	}
}
