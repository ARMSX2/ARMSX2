// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "IPU/yuv2rgb.h"
#include "IPU/IPU_MultiISA.h"

MULTI_ISA_UNSHARED_START

#if defined(_M_X86)
void ipu_dither_sse2(const macroblock_rgb32 &rgb32, macroblock_rgb16 &rgb16, int dte);
#endif

#if defined(ARCH_ARM64)
void ipu_dither_neon(const macroblock_rgb32 &rgb32, macroblock_rgb16 &rgb16, int dte);
#endif

__ri void ipu_dither(const macroblock_rgb32 &rgb32, macroblock_rgb16 &rgb16, int dte)
{
#if defined(_M_X86)
    ipu_dither_sse2(rgb32, rgb16, dte);
#elif defined(ARCH_ARM64)
    ipu_dither_neon(rgb32, rgb16, dte);
#else
    ipu_dither_reference(rgb32, rgb16, dte);
#endif
}

// Deliberately not inlineable: this is the semantic oracle the vector paths are
// written against, so the tests need a symbol to call. (__ri collapses to
// __forceinline in Release, which would leave nothing to link to.)
void ipu_dither_reference(const macroblock_rgb32 &rgb32, macroblock_rgb16 &rgb16, int dte)
{
    if (dte) {
        // I'm guessing values are rounded down when clamping.
        const int dither_coefficient[4][4] = {
            {-4, 0, -3, 1},
            {2, -2, 3, -1},
            {-3, 1, -4, 0},
            {3, -1, 2, -2},
        };
        for (int i = 0; i < 16; ++i) {
            for (int j = 0; j < 16; ++j) {
                const int dither = dither_coefficient[i & 3][j & 3];
                const int r = std::max(0, std::min(rgb32.c[i][j].r + dither, 255));
                const int g = std::max(0, std::min(rgb32.c[i][j].g + dither, 255));
                const int b = std::max(0, std::min(rgb32.c[i][j].b + dither, 255));

                rgb16.c[i][j].r = r >> 3;
                rgb16.c[i][j].g = g >> 3;
                rgb16.c[i][j].b = b >> 3;
                rgb16.c[i][j].a = rgb32.c[i][j].a == 0x40;
            }
        }
    } else {
        for (int i = 0; i < 16; ++i) {
            for (int j = 0; j < 16; ++j) {
                rgb16.c[i][j].r = rgb32.c[i][j].r >> 3;
                rgb16.c[i][j].g = rgb32.c[i][j].g >> 3;
                rgb16.c[i][j].b = rgb32.c[i][j].b >> 3;
                rgb16.c[i][j].a = rgb32.c[i][j].a == 0x40;
            }
        }
    }
}

#if defined(_M_X86)

__ri void ipu_dither_sse2(const macroblock_rgb32 &rgb32, macroblock_rgb16 &rgb16, int dte)
{
    const __m128i alpha_test = _mm_set1_epi16(0x40);
    const __m128i dither_add_matrix[] = {
        _mm_setr_epi32(0x00000000, 0x00000000, 0x00000000, 0x00010101),
        _mm_setr_epi32(0x00020202, 0x00000000, 0x00030303, 0x00000000),
        _mm_setr_epi32(0x00000000, 0x00010101, 0x00000000, 0x00000000),
        _mm_setr_epi32(0x00030303, 0x00000000, 0x00020202, 0x00000000),
    };
    const __m128i dither_sub_matrix[] = {
        _mm_setr_epi32(0x00040404, 0x00000000, 0x00030303, 0x00000000),
        _mm_setr_epi32(0x00000000, 0x00020202, 0x00000000, 0x00010101),
        _mm_setr_epi32(0x00030303, 0x00000000, 0x00040404, 0x00000000),
        _mm_setr_epi32(0x00000000, 0x00010101, 0x00000000, 0x00020202),
    };
    for (int i = 0; i < 16; ++i) {
        const __m128i dither_add = dither_add_matrix[i & 3];
        const __m128i dither_sub = dither_sub_matrix[i & 3];
        for (int n = 0; n < 2; ++n) {
            __m128i rgba_8_0123 = _mm_load_si128(reinterpret_cast<const __m128i *>(&rgb32.c[i][n * 8]));
            __m128i rgba_8_4567 = _mm_load_si128(reinterpret_cast<const __m128i *>(&rgb32.c[i][n * 8 + 4]));

            // Dither and clamp
            if (dte) {
                rgba_8_0123 = _mm_adds_epu8(rgba_8_0123, dither_add);
                rgba_8_0123 = _mm_subs_epu8(rgba_8_0123, dither_sub);
                rgba_8_4567 = _mm_adds_epu8(rgba_8_4567, dither_add);
                rgba_8_4567 = _mm_subs_epu8(rgba_8_4567, dither_sub);
            }

            // Split into channel components and extend to 16 bits
            const __m128i rgba_16_0415 = _mm_unpacklo_epi8(rgba_8_0123, rgba_8_4567);
            const __m128i rgba_16_2637 = _mm_unpackhi_epi8(rgba_8_0123, rgba_8_4567);
            const __m128i rgba_32_0246 = _mm_unpacklo_epi8(rgba_16_0415, rgba_16_2637);
            const __m128i rgba_32_1357 = _mm_unpackhi_epi8(rgba_16_0415, rgba_16_2637);
            const __m128i rg_64_01234567 = _mm_unpacklo_epi8(rgba_32_0246, rgba_32_1357);
            const __m128i ba_64_01234567 = _mm_unpackhi_epi8(rgba_32_0246, rgba_32_1357);

            const __m128i zero = _mm_setzero_si128();
            __m128i r = _mm_unpacklo_epi8(rg_64_01234567, zero);
            __m128i g = _mm_unpackhi_epi8(rg_64_01234567, zero);
            __m128i b = _mm_unpacklo_epi8(ba_64_01234567, zero);
            __m128i a = _mm_unpackhi_epi8(ba_64_01234567, zero);

            // Create RGBA
            r = _mm_srli_epi16(r, 3);
            g = _mm_slli_epi16(_mm_srli_epi16(g, 3), 5);
            b = _mm_slli_epi16(_mm_srli_epi16(b, 3), 10);
            a = _mm_slli_epi16(_mm_cmpeq_epi16(a, alpha_test), 15);

            const __m128i rgba16 = _mm_or_si128(_mm_or_si128(r, g), _mm_or_si128(b, a));

            _mm_store_si128(reinterpret_cast<__m128i *>(&rgb16.c[i][n * 8]), rgba16);
        }
    }
}

#endif

#if defined(ARCH_ARM64)

// dither_coefficient[] above with the sign folded into the choice of operation:
// a positive cell goes in the add table, a negative one goes in the sub table at
// its magnitude, and the other table holds zero for that lane. Saturating byte
// arithmetic then gives the reference's clamp to [0, 255] for free.
//
// One row of the source matrix covers four pixel columns and the pattern repeats
// every four, so each entry is that row's four cells laid out four times — lane
// k is the cell for pixel k, which is what a deinterleaved row wants.
alignas(16) static const u8 dither_add_matrix[4][16] = {
    {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
    {2, 0, 3, 0, 2, 0, 3, 0, 2, 0, 3, 0, 2, 0, 3, 0},
    {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
    {3, 0, 2, 0, 3, 0, 2, 0, 3, 0, 2, 0, 3, 0, 2, 0},
};
alignas(16) static const u8 dither_sub_matrix[4][16] = {
    {4, 0, 3, 0, 4, 0, 3, 0, 4, 0, 3, 0, 4, 0, 3, 0},
    {0, 2, 0, 1, 0, 2, 0, 1, 0, 2, 0, 1, 0, 2, 0, 1},
    {3, 0, 4, 0, 3, 0, 4, 0, 3, 0, 4, 0, 3, 0, 4, 0},
    {0, 1, 0, 2, 0, 1, 0, 2, 0, 1, 0, 2, 0, 1, 0, 2},
};

__ri void ipu_dither_neon(const macroblock_rgb32 &rgb32, macroblock_rgb16 &rgb16, int dte)
{
    const uint8x16_t alpha_test = vdupq_n_u8(0x40);

    for (int i = 0; i < 16; ++i) {
        // NEON deinterleaves on the load, so a whole 16-pixel row arrives already
        // split one register per channel. The SSE path needs six unpacks to reach
        // the same place because x86 has no equivalent load.
        uint8x16x4_t px = vld4q_u8(&rgb32.c[i][0].r);

        if (dte) {
            const uint8x16_t add = vld1q_u8(dither_add_matrix[i & 3]);
            const uint8x16_t sub = vld1q_u8(dither_sub_matrix[i & 3]);

            px.val[0] = vqsubq_u8(vqaddq_u8(px.val[0], add), sub);
            px.val[1] = vqsubq_u8(vqaddq_u8(px.val[1], add), sub);
            px.val[2] = vqsubq_u8(vqaddq_u8(px.val[2], add), sub);
        }

        const uint8x16_t r = vshrq_n_u8(px.val[0], 3);
        const uint8x16_t g = vshrq_n_u8(px.val[1], 3);
        const uint8x16_t b = vshrq_n_u8(px.val[2], 3);
        const uint8x16_t a = vceqq_u8(px.val[3], alpha_test);

        // r:5 g:5 b:5 a:1, least significant field first. The alpha compare widens
        // to 0x00FF, and 0x00FF << 15 truncates to exactly the 0x8000 top bit.
        const uint16x8_t lo = vorrq_u16(
            vorrq_u16(vmovl_u8(vget_low_u8(r)), vshlq_n_u16(vmovl_u8(vget_low_u8(g)), 5)),
            vorrq_u16(vshlq_n_u16(vmovl_u8(vget_low_u8(b)), 10), vshlq_n_u16(vmovl_u8(vget_low_u8(a)), 15)));
        const uint16x8_t hi = vorrq_u16(
            vorrq_u16(vmovl_high_u8(r), vshlq_n_u16(vmovl_high_u8(g), 5)),
            vorrq_u16(vshlq_n_u16(vmovl_high_u8(b), 10), vshlq_n_u16(vmovl_high_u8(a), 15)));

        vst1q_u16(reinterpret_cast<u16 *>(&rgb16.c[i][0]), lo);
        vst1q_u16(reinterpret_cast<u16 *>(&rgb16.c[i][8]), hi);
    }
}

#endif

MULTI_ISA_UNSHARED_END
