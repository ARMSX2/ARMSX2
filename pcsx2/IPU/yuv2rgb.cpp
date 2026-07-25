// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "IPU/IPU.h"
#include "IPU/IPU_MultiISA.h"
#include "IPU/yuv2rgb.h"

// The IPU's colour space conversion conforms to ITU-R Recommendation BT.601 if anyone wants to make a
// faster or "more accurate" implementation, but this is the precise documented integer method used by
// the hardware and is fast enough with SSE2.

#define IPU_Y_BIAS    16
#define IPU_C_BIAS    128
#define IPU_Y_COEFF   0x95	//  1.1640625
#define IPU_GCR_COEFF (-0x68)	// -0.8125
#define IPU_GCB_COEFF (-0x32)	// -0.390625
#define IPU_RCR_COEFF 0xcc	//  1.59375
#define IPU_BCB_COEFF 0x102	//  2.015625

MULTI_ISA_UNSHARED_START

// conforming implementation for reference, do not optimise
void yuv2rgb_reference(void)
{
	const macroblock_8& mb8 = decoder.mb8;
	macroblock_rgb32& rgb32 = decoder.rgb32;

	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
		{
			s32 lum = (IPU_Y_COEFF * (std::max(0, (s32)mb8.Y[y][x] - IPU_Y_BIAS))) >> 6;
			s32 rcr = (IPU_RCR_COEFF * ((s32)mb8.Cr[y>>1][x>>1] - 128)) >> 6;
			s32 gcr = (IPU_GCR_COEFF * ((s32)mb8.Cr[y>>1][x>>1] - 128)) >> 6;
			s32 gcb = (IPU_GCB_COEFF * ((s32)mb8.Cb[y>>1][x>>1] - 128)) >> 6;
			s32 bcb = (IPU_BCB_COEFF * ((s32)mb8.Cb[y>>1][x>>1] - 128)) >> 6;

			rgb32.c[y][x].r = std::max(0, std::min(255, (lum + rcr + 1) >> 1));
			rgb32.c[y][x].g = std::max(0, std::min(255, (lum + gcr + gcb + 1) >> 1));
			rgb32.c[y][x].b = std::max(0, std::min(255, (lum + bcb + 1) >> 1));
			rgb32.c[y][x].a = 0x80; // the norm to save doing this on the alpha pass
		}
}

#if defined(ARCH_X86)

// Suikoden Tactics FMV speed results: Reference - ~72fps, SSE2 - ~120fps
// An AVX2 version is only slightly faster than an SSE2 version (+2-3fps)
// (or I'm a poor optimiser), though it might be worth attempting again
// once we've ported to 64 bits (the extra registers should help).
__ri void yuv2rgb_sse2()
{
	const __m128i c_bias = _mm_set1_epi8(s8(IPU_C_BIAS));
	const __m128i y_bias = _mm_set1_epi8(IPU_Y_BIAS);
	const __m128i y_mask = _mm_set1_epi16(s16(0xFF00));
	// Specifying round off instead of round down as everywhere else
	// implies that this is right
	const __m128i round_1bit = _mm_set1_epi16(0x0001);;

	const __m128i y_coefficient = _mm_set1_epi16(s16(IPU_Y_COEFF << 2));
	const __m128i gcr_coefficient = _mm_set1_epi16(s16(u16(IPU_GCR_COEFF) << 2));
	const __m128i gcb_coefficient = _mm_set1_epi16(s16(u16(IPU_GCB_COEFF) << 2));
	const __m128i rcr_coefficient = _mm_set1_epi16(s16(IPU_RCR_COEFF << 2));
	const __m128i bcb_coefficient = _mm_set1_epi16(s16(IPU_BCB_COEFF << 2));

	// Alpha set to 0x80 here. The threshold stuff is done later.
	const __m128i& alpha = c_bias;

	for (int n = 0; n < 8; ++n) {
		// could skip the loadl_epi64 but most SSE instructions require 128-bit
		// alignment so two versions would be needed.
		__m128i cb = _mm_loadl_epi64(reinterpret_cast<__m128i*>(&decoder.mb8.Cb[n][0]));
		__m128i cr = _mm_loadl_epi64(reinterpret_cast<__m128i*>(&decoder.mb8.Cr[n][0]));

		// (Cb - 128) << 8, (Cr - 128) << 8
		cb = _mm_xor_si128(cb, c_bias);
		cr = _mm_xor_si128(cr, c_bias);
		cb = _mm_unpacklo_epi8(_mm_setzero_si128(), cb);
		cr = _mm_unpacklo_epi8(_mm_setzero_si128(), cr);

		__m128i rc = _mm_mulhi_epi16(cr, rcr_coefficient);
		__m128i gc = _mm_adds_epi16(_mm_mulhi_epi16(cr, gcr_coefficient), _mm_mulhi_epi16(cb, gcb_coefficient));
		__m128i bc = _mm_mulhi_epi16(cb, bcb_coefficient);

		for (int m = 0; m < 2; ++m) {
			__m128i y = _mm_load_si128(reinterpret_cast<__m128i*>(&decoder.mb8.Y[n * 2 + m][0]));
			y = _mm_subs_epu8(y, y_bias);
			// Y << 8 for pixels 0, 2, 4, 6, 8, 10, 12, 14
			__m128i y_even = _mm_slli_epi16(y, 8);
			// Y << 8 for pixels 1, 3, 5, 7 ,9, 11, 13, 15
			__m128i y_odd = _mm_and_si128(y, y_mask);

			y_even = _mm_mulhi_epu16(y_even, y_coefficient);
			y_odd  = _mm_mulhi_epu16(y_odd,  y_coefficient);

			__m128i r_even = _mm_adds_epi16(rc, y_even);
			__m128i r_odd  = _mm_adds_epi16(rc, y_odd);
			__m128i g_even = _mm_adds_epi16(gc, y_even);
			__m128i g_odd  = _mm_adds_epi16(gc, y_odd);
			__m128i b_even = _mm_adds_epi16(bc, y_even);
			__m128i b_odd  = _mm_adds_epi16(bc, y_odd);

			// round
			r_even = _mm_srai_epi16(_mm_add_epi16(r_even, round_1bit), 1);
			r_odd  = _mm_srai_epi16(_mm_add_epi16(r_odd,  round_1bit), 1);
			g_even = _mm_srai_epi16(_mm_add_epi16(g_even, round_1bit), 1);
			g_odd  = _mm_srai_epi16(_mm_add_epi16(g_odd,  round_1bit), 1);
			b_even = _mm_srai_epi16(_mm_add_epi16(b_even, round_1bit), 1);
			b_odd  = _mm_srai_epi16(_mm_add_epi16(b_odd,  round_1bit), 1);

			// combine even and odd bytes in original order
			__m128i r = _mm_packus_epi16(r_even, r_odd);
			__m128i g = _mm_packus_epi16(g_even, g_odd);
			__m128i b = _mm_packus_epi16(b_even, b_odd);

			r = _mm_unpacklo_epi8(r, _mm_shuffle_epi32(r, _MM_SHUFFLE(3, 2, 3, 2)));
			g = _mm_unpacklo_epi8(g, _mm_shuffle_epi32(g, _MM_SHUFFLE(3, 2, 3, 2)));
			b = _mm_unpacklo_epi8(b, _mm_shuffle_epi32(b, _MM_SHUFFLE(3, 2, 3, 2)));

			// Create RGBA (we could generate A here, but we don't) quads
			__m128i rg_l = _mm_unpacklo_epi8(r, g);
			__m128i ba_l = _mm_unpacklo_epi8(b, alpha);
			__m128i rgba_ll = _mm_unpacklo_epi16(rg_l, ba_l);
			__m128i rgba_lh = _mm_unpackhi_epi16(rg_l, ba_l);

			__m128i rg_h = _mm_unpackhi_epi8(r, g);
			__m128i ba_h = _mm_unpackhi_epi8(b, alpha);
			__m128i rgba_hl = _mm_unpacklo_epi16(rg_h, ba_h);
			__m128i rgba_hh = _mm_unpackhi_epi16(rg_h, ba_h);

			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][0]), rgba_ll);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][4]), rgba_lh);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][8]), rgba_hl);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][12]), rgba_hh);
		}
	}
}

#elif defined(ARCH_ARM64)

#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

// The whole conversion runs on sqdmulh at rescaled coefficients; since
// sqdmulh(a, b) floors (2*a*b) >> 16 and floors compose, these are exactly
// the reference c*coeff >> 6 / y*coeff >> 6 with no post-multiply shifts:
//   c*coeff >> 6 == sqdmulh(c << 8, coeff << 1)   (|coeff << 1| <= 0x204, no saturation)
//   y*coeff >> 6 == sqdmulh(y << 5, coeff << 4)   (y << 5 <= 239 << 5 = 7648)
// The << 8 and << 5 widenings come for free via shll/ushll.
__ri void yuv2rgb_neon()
{
	const uint8x16_t y_bias = vdupq_n_u8(IPU_Y_BIAS);
	const uint8x8_t c_bias = vdup_n_u8(IPU_C_BIAS);
	const uint8x16_t alpha = vdupq_n_u8(0x80);

	const int16x8_t y_coefficient = vdupq_n_s16(s16(IPU_Y_COEFF << 4));
	const int16x8_t gcr_coefficient = vdupq_n_s16(s16(u16(IPU_GCR_COEFF) << 1));
	const int16x8_t gcb_coefficient = vdupq_n_s16(s16(u16(IPU_GCB_COEFF) << 1));
	const int16x8_t rcr_coefficient = vdupq_n_s16(s16(IPU_RCR_COEFF << 1));
	const int16x8_t bcb_coefficient = vdupq_n_s16(s16(IPU_BCB_COEFF << 1));

	for (int n = 0; n < 8; ++n)
	{
		uint8x8_t cb_u8 = vld1_u8(reinterpret_cast<u8*>(&decoder.mb8.Cb[n][0]));
		uint8x8_t cr_u8 = vld1_u8(reinterpret_cast<u8*>(&decoder.mb8.Cr[n][0]));

		// (Cb - 128) << 8, (Cr - 128) << 8
		int16x8_t cb = vreinterpretq_s16_u16(vshll_n_u8(veor_u8(cb_u8, c_bias), 8));
		int16x8_t cr = vreinterpretq_s16_u16(vshll_n_u8(veor_u8(cr_u8, c_bias), 8));

		int16x8_t rc = vqdmulhq_s16(cr, rcr_coefficient);
		int16x8_t gc = vqaddq_s16(vqdmulhq_s16(cr, gcr_coefficient), vqdmulhq_s16(cb, gcb_coefficient));
		int16x8_t bc = vqdmulhq_s16(cb, bcb_coefficient);

		// duplicate chroma for linear pixel order: each value covers 2 adjacent pixels
		int16x8_t rc_lo = vzip1q_s16(rc, rc);
		int16x8_t rc_hi = vzip2q_s16(rc, rc);
		int16x8_t gc_lo = vzip1q_s16(gc, gc);
		int16x8_t gc_hi = vzip2q_s16(gc, gc);
		int16x8_t bc_lo = vzip1q_s16(bc, bc);
		int16x8_t bc_hi = vzip2q_s16(bc, bc);

		for (int m = 0; m < 2; ++m)
		{
			uint8x16_t y = vld1q_u8(&decoder.mb8.Y[n * 2 + m][0]);
			y = vqsubq_u8(y, y_bias);

			// Y * coefficient >> 6 for pixels 0-7 and 8-15
			int16x8_t y_res_lo = vqdmulhq_s16(
				vreinterpretq_s16_u16(vshll_n_u8(vget_low_u8(y), 5)), y_coefficient);
			int16x8_t y_res_hi = vqdmulhq_s16(
				vreinterpretq_s16_u16(vshll_high_n_u8(y, 5)), y_coefficient);

			// add chroma + rounding shift right + saturating narrow
			uint8x16_t r = vcombine_u8(
				vqrshrun_n_s16(vqaddq_s16(rc_lo, y_res_lo), 1),
				vqrshrun_n_s16(vqaddq_s16(rc_hi, y_res_hi), 1));
			uint8x16_t g = vcombine_u8(
				vqrshrun_n_s16(vqaddq_s16(gc_lo, y_res_lo), 1),
				vqrshrun_n_s16(vqaddq_s16(gc_hi, y_res_hi), 1));
			uint8x16_t b = vcombine_u8(
				vqrshrun_n_s16(vqaddq_s16(bc_lo, y_res_lo), 1),
				vqrshrun_n_s16(vqaddq_s16(bc_hi, y_res_hi), 1));

			uint8x16x4_t rgba = {r, g, b, alpha};
			vst4q_u8(reinterpret_cast<u8*>(&decoder.rgb32.c[n * 2 + m][0]), rgba);
		}
	}
}

#endif

MULTI_ISA_UNSHARED_END
