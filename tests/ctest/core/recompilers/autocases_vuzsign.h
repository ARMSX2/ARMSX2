// GENERATED from a capture taken on an SCPH-90000 (two byte-identical runs).
// Do not edit by hand.
//
// The sign of a zero as it crosses a VU0 macro-mode FMAC's two stages. Each row
// clears the status flag, seeds ACC from vf3 with
//
//     vmulax.xyzw $ACC, $vf3, $vf5     ; vf5 is 1.0, so ACC = acc, sign and all
//     <op>.xyzw   $vf4, $vf1, $vf2
//
// and records vf4 (or ACC), the MAC flag and the status flag. The seed is a
// multiply rather than the `vadda ACC, vf3, vf7` an earlier capture used
// because (-0) + (+0) is +0: an adder seed cannot express a -0 accumulator at
// all, and half these rows need one.
//
// bad_interp / bad_jit are not from the console: they are what this tree
// currently fails to reproduce, per column, regenerated from the test binary's
// own dump. Clearing a bit is how a fix lands.
#pragma once
#include <common/Pcsx2Types.h>

namespace console_vuzsign {

enum VuZOp { VZ_NONE, VZ_ADD, VZ_SUB, VZ_MUL, VZ_MADD, VZ_MSUB, VZ_MULA, VZ_MADDA };

// Columns a case can be wrong in, independently.
enum { VZB_VALUE = 1, VZB_MAC = 2, VZB_STAT = 4 };

struct VuZCase {
	u8 op;
	u32 fs, ft, acc;      // seeded into all four lanes of vf1 / vf2 / vf3
	u8 acc_dest;          // 1 = the op writes ACC, and ACC is what was read back
	u32 out;              // vf4 (or ACC) on the console, all four lanes
	u16 mac, stat;        // VI17 and VI16 on the console
	u8 bad_interp, bad_jit;
	const char* what;
};

inline constexpr VuZCase kVuZCases[] = {
	{VZ_ADD,   0x3F800000u, 0x3F800000u, 0x00000000u, 0, 0x40000000u, 0x0000u, 0x0040u, 0, 0, "CONTROL 1.0+1.0"},
	{VZ_ADD,   0x3F800000u, 0xBF800000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "CONTROL liveness 1.0-1.0: value and MAC must both differ from row 0"},
	{VZ_ADD,   0x80000000u, 0x00000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "ANCHOR vusat -0 + +0"},
	{VZ_ADD,   0x80000000u, 0x80000000u, 0x00000000u, 0, 0x80000000u, 0x00FFu, 0x00C3u, 0, 0, "ANCHOR vusat -0 + -0"},
	{VZ_MUL,   0x80800000u, 0x3F000000u, 0x00000000u, 0, 0x80000000u, 0x0FFFu, 0x01C7u, 0, 6, "ANCHOR vusat -2^-126*0.5 flushed result keeps its sign"},
	{VZ_MADD,  0x00800000u, 0x3F000000u, 0x00800000u, 0, 0x00800000u, 0x0000u, 0x0140u, 0, 4, "ANCHOR vusat 2^-126 + 2^-127: product flushes before the accumulate"},
	{VZ_ADD,   0x00800000u, 0x80800001u, 0x00000000u, 0, 0x80000000u, 0x0FFFu, 0x01C7u, 0, 6, "ANCHOR vusat 2^-126 + -(2^-126+1ulp): the adder's own flush keeps its sign"},
	{VZ_ADD,   0x00000000u, 0x00000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "+0 + +0"},
	{VZ_ADD,   0x00000000u, 0x80000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "+0 + -0 -- the pairing vusat lacks; IEEE says +0"},
	{VZ_SUB,   0x00000000u, 0x00000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "+0 - +0"},
	{VZ_SUB,   0x00000000u, 0x80000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "+0 - -0"},
	{VZ_SUB,   0x80000000u, 0x00000000u, 0x00000000u, 0, 0x80000000u, 0x00FFu, 0x00C3u, 0, 0, "-0 - +0"},
	{VZ_SUB,   0x80000000u, 0x80000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "-0 - -0"},
	{VZ_MADD,  0x80800000u, 0x3F000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x01C1u, 0, 4, "SUBJECT madd +0 + -2^-127: the row the DISABLED tripwire asserts is -0"},
	{VZ_MADD,  0x00800000u, 0x3F000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0141u, 0, 4, "madd +0 + +2^-127"},
	{VZ_MADD,  0x80800000u, 0x3F000000u, 0x80000000u, 0, 0x80000000u, 0x00FFu, 0x01C3u, 0, 4, "SEPARATOR madd -0 + -2^-127: signed -0, plain +0"},
	{VZ_MADD,  0x00800000u, 0x3F000000u, 0x80000000u, 0, 0x00000000u, 0x000Fu, 0x01C1u, 0, 4, "CONTROL madd -0 + +2^-127: +0 under both"},
	{VZ_MSUB,  0x00800000u, 0x3F000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x0141u, 0, 4, "msub +0 - +2^-127"},
	{VZ_MSUB,  0x80800000u, 0x3F000000u, 0x00000000u, 0, 0x00000000u, 0x000Fu, 0x01C1u, 0, 4, "msub +0 - -2^-127"},
	{VZ_MSUB,  0x00800000u, 0x3F000000u, 0x80000000u, 0, 0x80000000u, 0x00FFu, 0x01C3u, 0, 4, "CONTROL msub -0 - +2^-127: -0 under both, so the subtractor can reach -0 here"},
	{VZ_MSUB,  0x80800000u, 0x3F000000u, 0x80000000u, 0, 0x00000000u, 0x000Fu, 0x01C1u, 0, 4, "SEPARATOR msub -0 - -2^-127: signed +0, plain -0 -- inverts the madd separator"},
	{VZ_MADD,  0x00800000u, 0x3F000000u, 0x80800000u, 0, 0x80800000u, 0x00F0u, 0x01C2u, 0, 4, "madd -2^-126 + 2^-127: flushed 80800000 | wide 80400000"},
	{VZ_MADD,  0x80800000u, 0x3F000000u, 0x00800000u, 0, 0x00800000u, 0x0000u, 0x01C0u, 0, 4, "madd 2^-126 + -2^-127: flushed 00800000 | wide 00400000"},
	{VZ_ADD,   0x80800000u, 0x00800001u, 0x00000000u, 0, 0x00000000u, 0x0F0Fu, 0x0145u, 0, 6, "-2^-126 + (2^-126+1ulp) = +2^-149: flushes to +0"},
	{VZ_MULA,  0x80000000u, 0x3F800000u, 0x00000000u, 1, 0x80000000u, 0x00FFu, 0x00C3u, 0, 0, "LIVENESS readback: ACC = -0 * 1.0, no flush involved -- +0 here voids every acc row"},
	{VZ_MULA,  0x00000000u, 0x3F800000u, 0x00000000u, 1, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "readback control: ACC = +0 * 1.0"},
	{VZ_NONE,  0x00000000u, 0x00000000u, 0x80000000u, 1, 0x80000000u, 0x00FFu, 0x00C3u, 0, 0, "LIVENESS seed: ACC seeded -0 and read straight back"},
	{VZ_NONE,  0x00000000u, 0x00000000u, 0x00000000u, 1, 0x00000000u, 0x000Fu, 0x0041u, 0, 0, "seed control: ACC seeded +0"},
	{VZ_MULA,  0x80800000u, 0x3F000000u, 0x00000000u, 1, 0x80000000u, 0x0FFFu, 0x01C7u, 0, 6, "mula -2^-127 -> ACC: a flushed product in the accumulator"},
	{VZ_MADDA, 0x80800000u, 0x3F000000u, 0x00000000u, 1, 0x00000000u, 0x000Fu, 0x01C1u, 0, 4, "madda +0 + -2^-127 -> ACC"},
	{VZ_MADDA, 0x80800000u, 0x3F000000u, 0x80000000u, 1, 0x80000000u, 0x00FFu, 0x01C3u, 0, 4, "SEPARATOR madda -0 + -2^-127 -> ACC"},
	{VZ_MADDA, 0x00800000u, 0x3F000000u, 0x80000000u, 1, 0x00000000u, 0x000Fu, 0x01C1u, 0, 4, "CONTROL madda -0 + +2^-127 -> ACC"},
	{VZ_MADD,  0xBF800000u, 0x3F800000u, 0x40000000u, 0, 0x3F800000u, 0x0000u, 0x0080u, 0, 4, "KEY madd 2.0 + -1.0: ordinary negative product, result +1.0"},
	{VZ_MADD,  0x3F800000u, 0x3F800000u, 0x40000000u, 0, 0x40400000u, 0x0000u, 0x0000u, 0, 0, "CONTROL madd 2.0 + 1.0: positive product, no S anywhere"},
	{VZ_MADD,  0x3F800000u, 0xBF800000u, 0x40000000u, 0, 0x3F800000u, 0x0000u, 0x0080u, 0, 4, "KEY madd 2.0 + 1.0*-1.0: the sign is the product's, not fs's"},
	{VZ_MADD,  0x80000000u, 0x3F800000u, 0x3F800000u, 0, 0x3F800000u, 0x0000u, 0x00C0u, 0, 4, "KEY madd 1.0 + -0*1.0: does the zero arm carry S"},
	{VZ_MADD,  0x00000000u, 0x3F800000u, 0x3F800000u, 0, 0x3F800000u, 0x0000u, 0x0040u, 0, 4, "CONTROL madd 1.0 + +0*1.0: sticky Z, no S"},
	{VZ_MADD,  0xBF800000u, 0x3F800000u, 0x3F800000u, 0, 0x00000000u, 0x000Fu, 0x00C1u, 0, 4, "KEY madd 1.0 + -1.0 = exact zero: cause Z, does sticky get S"},
	{VZ_MSUB,  0xBF800000u, 0x3F800000u, 0x40800000u, 0, 0x40A00000u, 0x0000u, 0x0080u, 0, 4, "KEY msub 4.0 - -1.0: ordinary negative product, result +5.0"},
	{VZ_MSUB,  0x3F800000u, 0x3F800000u, 0x40000000u, 0, 0x3F800000u, 0x0000u, 0x0000u, 0, 0, "CONTROL msub 2.0 - 1.0: positive product, result +1.0"},
	{VZ_MADDA, 0xBF800000u, 0x3F800000u, 0x40000000u, 1, 0x3F800000u, 0x0000u, 0x0080u, 0, 4, "KEY madda 2.0 + -1.0 -> ACC: ordinary negative product"},
	{VZ_MSUB,  0xFF800000u, 0x40800000u, 0x3F800000u, 0, 0x7FFFFFFFu, 0xF000u, 0x0288u, 0, 7, "KEY msub 1.0 - -2^130: overflowing NEGATIVE product, result saturates positive"},
	{VZ_MSUB,  0x7F800000u, 0x40800000u, 0x3F800000u, 0, 0xFFFFFFFFu, 0xF0F0u, 0x028Au, 0, 7, "CONTROL msub 1.0 - 2^130: positive product, result saturates negative"},
	{VZ_MADD,  0xFF800000u, 0x40800000u, 0x3F800000u, 0, 0xFFFFFFFFu, 0xF0F0u, 0x028Au, 0, 7, "CONTROL madd 1.0 + -2^130: negative product AND negative result"},
};

inline constexpr int kVuZCaseCount = 44;
inline constexpr int kVuZBadInterp = 0;
inline constexpr int kVuZBadJit = 38;

} // namespace console_vuzsign
