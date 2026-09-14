// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstring>

// The console's logarithm is a 128-entry table, not a curve.
//
// Measured on real hardware. The GS indexes a table with the TOP SEVEN fractional
// bits of Q's mantissa and gets the logarithm back to (4 + L) fractional bits. The
// level of detail, in SIXTEENTHS of a level, is
//
//     LOD16 = K + (-e) * 2^(4+L) - T[L][idx]
//
// with `e` Q's IEEE exponent, `idx` those seven mantissa bits, and K TEX1's own
// field, which is already in sixteenths. Everything after that is integer
// arithmetic on that one number.
//
// Three things the measurement settles that a curve cannot:
//
//   * The table is indexed from Q's OWN mantissa, not the reciprocal's. Every
//     step of the curve lands on an exact multiple of 1/128 of Q's mantissa; a
//     table on 1/Q would step at values that are not dyadic in Q.
//
//   * It repeats per octave exactly. The same 128 entries reproduce every
//     reading at exponent -7 as at -1; the exponent enters only through the
//     shift.
//
//   * ⚠️ One entry is BROKEN, and it is reproduced here rather than smoothed.
//     At index 115 -- mantissa 1.8984375 -- T0 and T1 both step BACKWARDS by
//     one, so the level of detail there is a sixteenth higher than either
//     neighbour's and a level can fall and rise again inside a single Q ramp.
//     T2 and T3 are monotone. It is in two independent runs and in sixteen
//     independent K values. Do not "fix" it.
//
// T3 is round(log2(1 + idx/128) * 128) on 128 of 128 entries and T2 is
// ceil(T3/2) on 128 of 128, both exactly. T1 and T0 are within one unit of
// (T3+1)>>2 and (T3+2)>>3 but are NOT those -- 114 and 116 of 128 -- so all four
// are carried as measured.
//
// ⚠️ Our own log2 is not the problem and a better one is a no-op: it was measured
// at -0.0023..+0.0010 levels over the boundary mantissas that matter. The gap was
// always the console's curve, and the console's curve is this table.
//
// ⚠️ On a RAMPED Q -- and only there -- the level settles once per four-pixel
// group, from the group's second column. The group's phase follows the primitive
// rather than the screen, so what supplies the group's Q is the perspective walk's
// business. It is deliberately NOT in this file, and this file is per pixel.

/// The seven bits of Q's mantissa the table is indexed by.
static constexpr int GS_LOD_TABLE_BITS = 7;
static constexpr int GS_LOD_TABLE_SIZE = 1 << GS_LOD_TABLE_BITS;

/// The four measured tables, one per TEX1.L, in units of 2^-(4+L) of a level.
///
/// Held as 32-bit words rather than the 8 bits every entry fits in: the scanline
/// generator reads them one lane at a time with the same extract-add-load triple
/// it uses for texels, and a word-sized entry keeps that to three instructions
/// per lane. Two kilobytes for all four rows.
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
