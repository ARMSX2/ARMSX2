// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// GENERATED from a first-party capture taken on a real PS2.  Do not edit.
//
// EE FPU and VU0 overflow/underflow ground truth.  ps2autotests' fpu/fcr.cpp
// runs the same MUL.S(0x7F7FFFFF, 0x7F7FFFFF) on hardware but prints the
// result with %f, so it only ever recorded the string "NaN" -- enough to know
// the exponent came back 255, not enough to know anything else.  Every value
// here is a raw 32-bit pattern.
//
// The capture establishes one rule that governs every row:
//
//   The EE FPU's representable maximum is 0x7FFFFFFF == (2 - 2^-23) * 2^128.
//   Exponent 255 is an ordinary exponent -- there is no Inf and no NaN.
//   Overflow means exceeding THAT, it saturates there, and only then are
//   FCR31 O and SO raised.
//
// So +FLT_MAX + +FLT_MAX is NOT an overflow on this machine: the exact sum
// is (2 - 2^-23) * 2^128, which is representable, and the console returns it
// with FCR31 untouched.  Only a result past that saturates.  Both halves of
// the rule are asserted in exact rational arithmetic by the generator, so a
// single mis-decoded word would have rejected the capture.
//
// Denormal operands are flushed to signed zero before the op; U is raised
// only when the result computed from the flushed operands is nonzero and
// below the smallest normal.
//
// Rig facts from the same run: FCR31 writable mask 0083c078, fixed ones 01000001,
// FCR0 (implementation/revision) 00002e40.  ps2autotests recorded FCR0 as
// 00002e30 on a different console; that register is revision-dependent and
// is not a conformance signal.
#pragma once

#include "common/Pcsx2Types.h"

namespace console_fpuovf
{
enum FpuOvfOp
{
	FO_ADD = 1,
	FO_SUB = 2,
	FO_MUL = 3,
	FO_DIV = 4,
	FO_SQRT = 5,
	FO_RSQRT = 6,
	FO_ADDA = 7,
	FO_SUBA = 8,
	FO_MULA = 9,
	FO_MADD = 10,
	FO_MSUB = 11,
	FO_MADDA = 12,
	FO_MSUBA = 13,
};

// `acc` is the value ADDA.S placed in the accumulator before a MADD/MSUB/
// MADDA/MSUBA row, and is unused otherwise.  `result` is Fd, or the
// accumulator for the ops in which the accumulator is the destination.
struct FpuOvfCase
{
	FpuOvfOp op;
	u32 fs, ft, acc;
	u32 result;
	u32 fcr31;
	bool acc_dest;
	const char* what;
};

static constexpr FpuOvfCase kCases[] = {
	{FO_MUL, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, false, "mul +FLT_MAX, +FLT_MAX"},
	{FO_MUL, 0x7F7FFFFFu, 0x40000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, false, "mul +FLT_MAX, 2.0"},
	{FO_MUL, 0x7F000000u, 0x40800000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, false, "mul 2^127, 4.0"},
	{FO_MUL, 0x7F800000u, 0x40000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, false, "mul 2^128, 2.0"},
	{FO_MUL, 0x7F7FFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, false, "mul +FLT_MAX, +EEMAX"},
	{FO_MUL, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, false, "mul +EEMAX, +EEMAX"},
	{FO_MUL, 0xFF7FFFFFu, 0x7F7FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, false, "mul -FLT_MAX, +FLT_MAX"},
	{FO_MUL, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, false, "mul +FLT_MAX, -FLT_MAX"},
	{FO_MUL, 0xFF7FFFFFu, 0xFF7FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, false, "mul -FLT_MAX, -FLT_MAX"},
	{FO_ADD, 0xFF7FFFFFu, 0xFF7FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, false, "add -FLT_MAX, -FLT_MAX"},
	{FO_MUL, 0x7FFFFFFFu, 0x3F800000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, false, "mul +EEMAX, 1.0"},
	{FO_MUL, 0x7F800000u, 0x3F800000u, 0x00000000u, 0x7F800000u, 0x01000001u, false, "mul 2^128, 1.0"},
	{FO_DIV, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x3F800000u, 0x01000001u, false, "div +EEMAX, +EEMAX"},
	{FO_MUL, 0x7F000000u, 0x40000000u, 0x00000000u, 0x7F800000u, 0x01000001u, false, "mul 2^127, 2.0"},
	{FO_MUL, 0x7F800000u, 0x3F000000u, 0x00000000u, 0x7F000000u, 0x01000001u, false, "mul 2^128, 0.5"},
	{FO_ADD, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, false, "add +EEMAX, +EEMAX"},
	{FO_ADD, 0x7F800000u, 0x00000000u, 0x00000000u, 0x7F800000u, 0x01000001u, false, "add 2^128, +0"},
	{FO_SUB, 0x7F800000u, 0x7F800000u, 0x00000000u, 0x00000000u, 0x01000001u, false, "sub 2^128, 2^128"},
	{FO_ADD, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, false, "add +FLT_MAX, +FLT_MAX"},
	{FO_SUB, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, false, "sub +FLT_MAX, -FLT_MAX"},
	{FO_MUL, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, false, "mul +FLT_MAX, +FLT_MAX"},
	{FO_ADDA, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, true, "adda +FLT_MAX, +FLT_MAX"},
	{FO_SUBA, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, true, "suba +FLT_MAX, -FLT_MAX"},
	{FO_MULA, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, true, "mula +FLT_MAX, +FLT_MAX"},
	{FO_MADD, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7FFFFFFFu, 0x01008011u, false, "madd +FLT_MAX, +FLT_MAX acc +FLT_MAX"},
	{FO_MSUB, 0x7F7FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0xFFFFFFFFu, 0x01008011u, false, "msub +FLT_MAX, +FLT_MAX acc -FLT_MAX"},
	{FO_MADDA, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7FFFFFFFu, 0x01008011u, true, "madda +FLT_MAX, +FLT_MAX acc +FLT_MAX"},
	{FO_MSUBA, 0x7F7FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0xFFFFFFFFu, 0x01008011u, true, "msuba +FLT_MAX, +FLT_MAX acc -FLT_MAX"},
	{FO_ADD, 0x3F800000u, 0x40000000u, 0x00000000u, 0x40400000u, 0x01000001u, false, "add 1.0, 2.0"},
	{FO_SUB, 0x40800000u, 0x3F800000u, 0x00000000u, 0x40400000u, 0x01000001u, false, "sub 4.0, 1.0"},
	{FO_MUL, 0x40000000u, 0x40000000u, 0x00000000u, 0x40800000u, 0x01000001u, false, "mul 2.0, 2.0"},
	{FO_ADDA, 0x3F800000u, 0x40000000u, 0x00000000u, 0x40400000u, 0x01000001u, true, "adda 1.0, 2.0"},
	{FO_SUBA, 0x40800000u, 0x3F800000u, 0x00000000u, 0x40400000u, 0x01000001u, true, "suba 4.0, 1.0"},
	{FO_MULA, 0x40000000u, 0x40000000u, 0x00000000u, 0x40800000u, 0x01000001u, true, "mula 2.0, 2.0"},
	{FO_MADD, 0x40000000u, 0x40000000u, 0x3F800000u, 0x40A00000u, 0x01000001u, false, "madd 2.0, 2.0 acc 1.0"},
	{FO_MSUB, 0x40000000u, 0x40000000u, 0x3F800000u, 0xC0400000u, 0x01000001u, false, "msub 2.0, 2.0 acc 1.0"},
	{FO_MADDA, 0x40000000u, 0x40000000u, 0x3F800000u, 0x40A00000u, 0x01000001u, true, "madda 2.0, 2.0 acc 1.0"},
	{FO_MSUBA, 0x40000000u, 0x40000000u, 0x3F800000u, 0xC0400000u, 0x01000001u, true, "msuba 2.0, 2.0 acc 1.0"},
	{FO_DIV, 0x3F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01010021u, false, "div 1.0, +0"},
	{FO_RSQRT, 0x3F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01010021u, false, "rsqrt 1.0, +0"},
	{FO_DIV, 0x7F7FFFFFu, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x01010021u, false, "div +FLT_MAX, MIN_DENORM"},
	{FO_DIV, 0x7F7FFFFFu, 0x00800000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, false, "div +FLT_MAX, MIN_NORMAL"},
	{FO_DIV, 0xFF7FFFFFu, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x01010021u, false, "div -FLT_MAX, MIN_DENORM"},
	{FO_RSQRT, 0x7F7FFFFFu, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x01010021u, false, "rsqrt +FLT_MAX, MIN_DENORM"},
	{FO_SQRT, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x5FB504F3u, 0x01000001u, false, "sqrt +EEMAX, +EEMAX"},
	{FO_SQRT, 0x00000000u, 0x7F800000u, 0x00000000u, 0x5F800000u, 0x01000001u, false, "sqrt +0, 2^128"},
	{FO_SQRT, 0x00000000u, 0xFF7FFFFFu, 0x00000000u, 0x5F7FFFFFu, 0x01020041u, false, "sqrt +0, -FLT_MAX"},
	{FO_MUL, 0x00800000u, 0x00800000u, 0x00000000u, 0x00000000u, 0x01004009u, false, "mul MIN_NORMAL, MIN_NORMAL"},
	{FO_MUL, 0x00800000u, 0x3F000000u, 0x00000000u, 0x00000000u, 0x01004009u, false, "mul MIN_NORMAL, 0.5"},
	{FO_MUL, 0x3F800000u, 0x00000001u, 0x00000000u, 0x00000000u, 0x01000001u, false, "mul 1.0, MIN_DENORM"},
	{FO_ADD, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000001u, false, "add MIN_DENORM, +0"},
	{FO_ADD, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u, 0x01000001u, false, "add MIN_DENORM, MIN_DENORM"},
	{FO_ADD, 0x00400000u, 0x00400000u, 0x00000000u, 0x00000000u, 0x01000001u, false, "add DENORM, DENORM"},
	{FO_MUL, 0x00800001u, 0x3F000000u, 0x00000000u, 0x00000000u, 0x01004009u, false, "mul MIN_NORMAL+1, 0.5"},
	{FO_MUL, 0xBF800000u, 0x00000001u, 0x00000000u, 0x80000000u, 0x01000001u, false, "mul -1.0, MIN_DENORM"},
	{FO_MUL, 0x00800000u, 0x00000001u, 0x00000000u, 0x00000000u, 0x01000001u, false, "mul MIN_NORMAL, MIN_DENORM"},
	{FO_SUB, 0x00800000u, 0x00800000u, 0x00000000u, 0x00000000u, 0x01000001u, false, "sub MIN_NORMAL, MIN_NORMAL"},
};
static constexpr int kCaseCount = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));

// VU0 through COP2 macro mode, same questions asked of the other unit.
// MAC nibbles are O=0xF000 U=0x0F00 S=0x00F0 Z=0x000F, and within a nibble
// x=8 y=4 z=2 w=1 -- the layout PCSX2 uses in VUflags.cpp, confirmed here by
// a row that overflows x, y and z while leaving w in range.
enum VuOvfOp
{
	VO_VADD = 1,
	VO_VSUB = 2,
	VO_VMUL = 3,
};

struct VuOvfCase
{
	VuOvfOp op;
	u32 fs[4], ft[4];
	u32 result[4];
	u32 mac, status, clip;
	const char* what;
};

static constexpr VuOvfCase kVuCases[] = {
	{VO_VMUL, {0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F000000u, 0x7F800000u}, {0x7F7FFFFFu, 0x40000000u, 0x40800000u, 0x40000000u}, {0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu}, 0xB000u, 0x0208u, 0x0000u, "vmul lanes: +FLT_MAX*+FLT_MAX +FLT_MAX*2.0 2^127*4.0 2^128*2.0"},
	{VO_VADD, {0x7F7FFFFFu, 0x7FFFFFFFu, 0x7F800000u, 0x3F800000u}, {0x7F7FFFFFu, 0x7FFFFFFFu, 0x7F800000u, 0x3F800000u}, {0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x40000000u}, 0x6000u, 0x0208u, 0x0000u, "vadd lanes: +FLT_MAX++FLT_MAX +EEMAX++EEMAX 2^128+2^128 1.0+1.0"},
	{VO_VMUL, {0x7FFFFFFFu, 0x7F800000u, 0x7F000000u, 0x7F800000u}, {0x3F800000u, 0x3F800000u, 0x40000000u, 0x3F000000u}, {0x7FFFFFFFu, 0x7F800000u, 0x7F800000u, 0x7F000000u}, 0x0000u, 0x0000u, 0x0000u, "vmul lanes: +EEMAX*1.0 2^128*1.0 2^127*2.0 2^128*0.5"},
	{VO_VMUL, {0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F7FFFFFu, 0x3F800000u}, {0x7F7FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x3F800000u}, {0x7FFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x3F800000u}, 0xE060u, 0x028Au, 0x0000u, "vmul lanes: +FLT_MAX*+FLT_MAX -FLT_MAX*+FLT_MAX +FLT_MAX*-FLT_MAX 1.0*1.0"},
	{VO_VMUL, {0x00800000u, 0x00800000u, 0x3F800000u, 0x00000001u}, {0x00800000u, 0x3F000000u, 0x00000001u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}, 0x0C0Fu, 0x0145u, 0x0000u, "vmul lanes: MIN_NORMAL*MIN_NORMAL MIN_NORMAL*0.5 1.0*MIN_DENORM MIN_DENORM*1.0"},
	{VO_VADD, {0x00000000u, 0x80000000u, 0x3F800000u, 0xFF7FFFFFu}, {0x00000000u, 0x00000000u, 0xBF800000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0xFF7FFFFFu}, 0x001Eu, 0x00C3u, 0x0000u, "vadd lanes: +0++0 -0++0 1.0+-1.0 -FLT_MAX+1.0"},
	{VO_VMUL, {0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u}, {0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u}, {0x40800000u, 0x40800000u, 0x40800000u, 0x40800000u}, 0x0000u, 0x0000u, 0x0000u, "vmul lanes: 2.0*2.0 2.0*2.0 2.0*2.0 2.0*2.0"},
	{VO_VSUB, {0x7F7FFFFFu, 0xFF7FFFFFu, 0x3F800000u, 0x00800000u}, {0xFF7FFFFFu, 0x7F7FFFFFu, 0x3F800000u, 0x00800000u}, {0x7FFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000000u}, 0x0043u, 0x00C3u, 0x0000u, "vsub lanes: +FLT_MAX--FLT_MAX -FLT_MAX-+FLT_MAX 1.0-1.0 MIN_NORMAL-MIN_NORMAL"},
};
static constexpr int kVuCaseCount = static_cast<int>(sizeof(kVuCases) / sizeof(kVuCases[0]));

} // namespace console_fpuovf
