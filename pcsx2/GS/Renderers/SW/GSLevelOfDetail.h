// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstring>

// The GS computes the LOD logarithm from a 128-entry table, not a curve.
//
// The table is indexed by the top seven fractional bits of Q's own mantissa (not
// 1/Q's) and returns the logarithm to (4 + L) fractional bits. In sixteenths of a
// level:
//
//     LOD16 = K + (-e) * 2^(4+L) - T[L][idx]
//
// with `e` Q's IEEE exponent, `idx` those seven bits, and K TEX1's field (already
// in sixteenths). The same entries serve every octave; the exponent enters only
// through the shift.
//
// Index 115 (mantissa 1.8984375) steps BACKWARDS by one in T0 and T1, so the LOD
// there is a sixteenth higher than both neighbours. This matches the console; do
// not smooth it. T2 and T3 are monotone.
//
// T3 is round(log2(1 + idx/128) * 128) and T2 is ceil(T3/2). T1 and T0 are close
// to (T3+1)>>2 and (T3+2)>>3 but not equal, so all four are stored as tables.
//
// On a ramped Q the level settles once per four-pixel group, from the group's
// second column, with phase following the primitive. That belongs to the
// perspective walk; this file is per pixel.

/// The seven bits of Q's mantissa the table is indexed by.
static constexpr int GS_LOD_TABLE_BITS = 7;
static constexpr int GS_LOD_TABLE_SIZE = 1 << GS_LOD_TABLE_BITS;

/// The four tables, one per TEX1.L, in units of 2^-(4+L) of a level.
///
/// 32-bit entries rather than 8-bit so the scanline generator can fetch them with
/// the same three-instruction per-lane sequence it uses for texels.
inline constexpr s32 GSLevelOfDetailTable[4][GS_LOD_TABLE_SIZE] = {
	{
		  0,   0,   0,   0,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
		  3,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,
		  5,   5,   5,   5,   6,   6,   6,   6,   6,   6,   6,   6,   7,   7,   7,   7,
		  7,   7,   7,   7,   8,   8,   8,   8,   8,   8,   8,   8,   9,   9,   9,   9,
		  9,   9,   9,   9,  10,  10,  10,  10,  10,  10,  10,  10,  11,  11,  11,  11,
		 11,  11,  11,  11,  12,  12,  12,  12,  12,  12,  12,  12,  13,  13,  13,  13,
		 13,  13,  13,  13,  13,  13,  13,  13,  14,  14,  14,  14,  14,  14,  14,  14,
		 15,  15,  15,  14,  15,  15,  15,  15,  15,  15,  15,  15,  16,  16,  16,  16,
	},
	{
		  0,   0,   1,   1,   2,   2,   2,   2,   3,   3,   4,   4,   4,   4,   5,   5,
		  6,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,   9,   9,   9,  10,  10,
		 10,  10,  11,  11,  12,  12,  12,  12,  13,  13,  13,  13,  14,  14,  14,  14,
		 15,  15,  15,  15,  16,  16,  16,  16,  17,  17,  17,  17,  18,  18,  18,  18,
		 19,  19,  19,  19,  20,  20,  20,  20,  21,  21,  21,  21,  22,  22,  22,  22,
		 23,  23,  23,  23,  23,  23,  24,  24,  24,  24,  25,  25,  25,  25,  26,  26,
		 26,  26,  26,  26,  27,  27,  27,  27,  28,  28,  28,  28,  28,  28,  29,  29,
		 29,  29,  30,  29,  30,  30,  30,  30,  31,  31,  31,  31,  31,  31,  32,  32,
	},
	{
		  0,   1,   2,   2,   3,   4,   4,   5,   6,   7,   7,   8,   9,   9,  10,  10,
		 11,  12,  12,  13,  14,  14,  15,  16,  16,  17,  17,  18,  19,  19,  20,  20,
		 21,  21,  22,  23,  23,  24,  24,  25,  25,  26,  26,  27,  28,  28,  29,  29,
		 30,  30,  31,  31,  32,  32,  33,  33,  34,  34,  35,  35,  36,  36,  37,  37,
		 38,  38,  39,  39,  40,  40,  41,  41,  41,  42,  42,  43,  43,  44,  44,  45,
		 45,  46,  46,  46,  47,  47,  48,  48,  49,  49,  49,  50,  50,  51,  51,  52,
		 52,  52,  53,  53,  54,  54,  54,  55,  55,  56,  56,  56,  57,  57,  58,  58,
		 58,  59,  59,  59,  60,  60,  61,  61,  61,  62,  62,  62,  63,  63,  64,  64,
	},
	{
		  0,   1,   3,   4,   6,   7,   8,  10,  11,  13,  14,  15,  17,  18,  19,  20,
		 22,  23,  24,  26,  27,  28,  29,  31,  32,  33,  34,  35,  37,  38,  39,  40,
		 41,  42,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  55,  56,  57,  58,
		 59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,
		 75,  76,  77,  78,  79,  80,  81,  81,  82,  83,  84,  85,  86,  87,  88,  89,
		 90,  91,  91,  92,  93,  94,  95,  96,  97,  97,  98,  99, 100, 101, 102, 103,
		103, 104, 105, 106, 107, 107, 108, 109, 110, 111, 111, 112, 113, 114, 115, 115,
		116, 117, 118, 118, 119, 120, 121, 121, 122, 123, 124, 124, 125, 126, 127, 127,
	},
};

/// The index the table is read at: the top seven fractional bits of Q's mantissa.
__forceinline static u32 GSLevelOfDetailIndex(u32 qbits)
{
	return (qbits >> (23 - GS_LOD_TABLE_BITS)) & (GS_LOD_TABLE_SIZE - 1);
}

/// Q's IEEE exponent, unbiased.
__forceinline static s32 GSLevelOfDetailExponent(u32 qbits)
{
	return static_cast<s32>((qbits >> 23) & 0xFF) - 127;
}

/// The level of detail in sixteenths of a level, from Q alone.
///
/// `tab` is the row for this draw's TEX1.L, `k16` is TEX1.K (already sixteenths)
/// and `lshift` is 4 + TEX1.L. A Q of zero gives the largest exponent term and so
/// the deepest level, which is what a vanishing Q means.
__forceinline static s32 GSLevelOfDetail16(float q, const s32* tab, s32 k16, s32 lshift)
{
	u32 bits;
	std::memcpy(&bits, &q, sizeof(bits));

	return k16 + ((-GSLevelOfDetailExponent(bits)) << lshift) - tab[GSLevelOfDetailIndex(bits)];
}
