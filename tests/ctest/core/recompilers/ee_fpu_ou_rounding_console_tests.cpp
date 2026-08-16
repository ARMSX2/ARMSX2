// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Where FCR31's O comes from on the EE adder, against the console.
//
// The adder masks the smaller operand's low bits by the exponent difference
// and erases it entirely past 24 (eeGuardedSum in FPU.cpp). O follows the sum
// that mask produced, and follows it after the rounding:
//
//   * an addend 25 or more exponents down is gone, so 0x7FFFFFFF + it is
//     0x7FFFFFFF and nothing is raised, however large the addend was;
//   * at exactly 24 the addend survives as its leading bit, and
//     0x7FFFFFFF + 2^104 is 2^129 - 2^104 -- above the largest EE number, but
//     25 significant bits, so it chops back onto 0x7FFFFFFF. Still nothing
//     raised;
//   * at 23 the sum is 2^129, which no rounding brings back, and O|SO is set.
//
// The corpus cannot see any of this: every in-class corpus row puts its addend
// 128 exponents down, where the mask erases it outright. The rows below are a
// standalone probe of exponents 180 to 255, 1293 cases. Every result and FCR31
// is a console reading.
//
// The single-precision fast path (eeClampMode 1 and 2) is not run here: it
// saturates arithmetic to FLT_MAX, a binade below this whole region.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFd = 4, kFs = 5, kFt = 6;

enum Form
{
	FORM_ADD, FORM_SUB, FORM_ADDA, FORM_SUBA,
	FORM_MADD, FORM_MSUB, FORM_MADDA, FORM_MSUBA,
};

bool WritesAcc(Form f)
{
	return f == FORM_ADDA || f == FORM_SUBA || f == FORM_MADDA || f == FORM_MSUBA;
}

u32 Encode(Form f)
{
	switch (f)
	{
		case FORM_ADD:   return ADD_S(kFd, kFs, kFt);
		case FORM_SUB:   return SUB_S(kFd, kFs, kFt);
		case FORM_ADDA:  return ADDA_S(kFs, kFt);
		case FORM_SUBA:  return SUBA_S(kFs, kFt);
		case FORM_MADD:  return MADD_S(kFd, kFs, kFt);
		case FORM_MSUB:  return MSUB_S(kFd, kFs, kFt);
		case FORM_MADDA: return MADDA_S(kFs, kFt);
		default:         return MSUBA_S(kFs, kFt);
	}
}

const char* FormName(Form f)
{
	static const char* kNames[] = {"add.s", "sub.s", "adda.s", "suba.s",
		"madd.s", "msub.s", "madda.s", "msuba.s"};
	return kNames[f];
}

struct Out { u32 val, fcr; };

// `exact` picks eeClampMode 4 over 3; the two emit the same code for
// everything this file touches, so both are run.
Out RunOne(Form f, u32 fs, u32 ft, u32 acc, u32 pre, bool jit, bool exact)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (exact)
		h.EnableFpuExactMode();
	else
		h.EnableFpuFullMode();
	h.SetFprBits(kFs, fs);
	h.SetFprBits(kFt, ft);
	h.SetAccBits(acc);
	h.SetFcr31(pre);
	h.LoadProgram({Encode(f)});
	Out o{};
	if (jit)
	{
		h.RunJitNoDiff();
		o.fcr = h.JitSnapshot().fprs.fprc[31];
		o.val = WritesAcc(f) ? h.GetAccBitsJit() : h.GetFprBitsJit(kFd);
	}
	else
	{
		h.RunInterpOnly();
		o.fcr = h.InterpSnapshot().fprs.fprc[31];
		o.val = WritesAcc(f) ? h.GetAccBitsInterp() : h.GetFprBitsInterp(kFd);
	}
	return o;
}

constexpr u32 FPUflagO = 0x00008000u;
constexpr u32 FPUflagU = 0x00004000u;
constexpr u32 FPUflagSO = 0x00000010u;
constexpr u32 FPUflagSU = 0x00000008u;
constexpr u32 kOU = FPUflagO | FPUflagU | FPUflagSO | FPUflagSU;

struct ConsoleCase
{
	Form form;
	u32 fs, ft, acc, pre;
	u32 want;      // console result
	u32 wantFcr;   // console FCR31
	int addendExp; // exponent field of the term the mask acts on
	const char* note;
};

constexpr ConsoleCase kConsole[] = {
	{FORM_ADD,   0x7FFFFFFFu, 0x65800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "window"},
	{FORM_SUB,   0x7FFFFFFFu, 0xE5800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "window"},
	{FORM_ADDA,  0x7FFFFFFFu, 0x65800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "window"},
	{FORM_SUBA,  0x7FFFFFFFu, 0xE5800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "window"},
	{FORM_MADD,  0x65800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "window"},
	{FORM_MSUB,  0xE5800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "window"},
	{FORM_MADDA, 0x65800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "window"},
	{FORM_MSUBA, 0xE5800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "window"},
	{FORM_ADD,   0x7FFFFFFFu, 0x6B800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 215, "window"},
	{FORM_SUB,   0x7FFFFFFFu, 0xEB800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 215, "window"},
	{FORM_ADDA,  0x7FFFFFFFu, 0x6B800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 215, "window"},
	{FORM_SUBA,  0x7FFFFFFFu, 0xEB800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 215, "window"},
	{FORM_MADD,  0x6B800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 215, "window"},
	{FORM_MSUB,  0xEB800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 215, "window"},
	{FORM_MADDA, 0x6B800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 215, "window"},
	{FORM_MSUBA, 0xEB800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 215, "window"},
	{FORM_ADD,   0x7FFFFFFFu, 0x72800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "window"},
	{FORM_SUB,   0x7FFFFFFFu, 0xF2800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "window"},
	{FORM_ADDA,  0x7FFFFFFFu, 0x72800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "window"},
	{FORM_SUBA,  0x7FFFFFFFu, 0xF2800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "window"},
	{FORM_MADD,  0x72800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "window"},
	{FORM_MSUB,  0xF2800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "window"},
	{FORM_MADDA, 0x72800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "window"},
	{FORM_MSUBA, 0xF2800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "window"},
	{FORM_ADD,   0x7FFFFFFFu, 0x73000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "window"},
	{FORM_SUB,   0x7FFFFFFFu, 0xF3000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "window"},
	{FORM_ADDA,  0x7FFFFFFFu, 0x73000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "window"},
	{FORM_SUBA,  0x7FFFFFFFu, 0xF3000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "window"},
	{FORM_MADD,  0x73000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "window"},
	{FORM_MSUB,  0xF3000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "window"},
	{FORM_MADDA, 0x73000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "window"},
	{FORM_MSUBA, 0xF3000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "window"},
	{FORM_ADD,   0x7FFFFFFFu, 0x73800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "boundary"},
	{FORM_SUB,   0x7FFFFFFFu, 0xF3800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "boundary"},
	{FORM_ADDA,  0x7FFFFFFFu, 0x73800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "boundary"},
	{FORM_SUBA,  0x7FFFFFFFu, 0xF3800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "boundary"},
	{FORM_MADD,  0x73800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "boundary"},
	{FORM_MSUB,  0xF3800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "boundary"},
	{FORM_MADDA, 0x73800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "boundary"},
	{FORM_MSUBA, 0xF3800000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "boundary"},
	{FORM_ADD,   0x7FFFFFFFu, 0x74000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "raises"},
	{FORM_SUB,   0x7FFFFFFFu, 0xF4000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "raises"},
	{FORM_ADDA,  0x7FFFFFFFu, 0x74000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "raises"},
	{FORM_SUBA,  0x7FFFFFFFu, 0xF4000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "raises"},
	{FORM_MADD,  0x74000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "raises"},
	{FORM_MSUB,  0xF4000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "raises"},
	{FORM_MADDA, 0x74000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "raises"},
	{FORM_MSUBA, 0xF4000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "raises"},
	{FORM_ADD,   0x7FFFFFFFu, 0x78000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 240, "raises"},
	{FORM_SUB,   0x7FFFFFFFu, 0xF8000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 240, "raises"},
	{FORM_ADDA,  0x7FFFFFFFu, 0x78000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 240, "raises"},
	{FORM_SUBA,  0x7FFFFFFFu, 0xF8000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 240, "raises"},
	{FORM_MADD,  0x78000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 240, "raises"},
	{FORM_MSUB,  0xF8000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 240, "raises"},
	{FORM_MADDA, 0x78000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 240, "raises"},
	{FORM_MSUBA, 0xF8000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 240, "raises"},
	{FORM_ADD,   0xFFFFFFFFu, 0xF3000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 230, "mirrored"},
	{FORM_SUB,   0xFFFFFFFFu, 0x73000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 230, "mirrored"},
	{FORM_ADDA,  0xFFFFFFFFu, 0xF3000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 230, "mirrored"},
	{FORM_SUBA,  0xFFFFFFFFu, 0x73000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 230, "mirrored"},
	{FORM_MADD,  0xF3000000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 230, "mirrored"},
	{FORM_MSUB,  0x73000000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 230, "mirrored"},
	{FORM_MADDA, 0xF3000000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 230, "mirrored"},
	{FORM_MSUBA, 0x73000000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 230, "mirrored"},
	{FORM_ADD,   0xFFFFFFFFu, 0xF3800000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 231, "mirrored"},
	{FORM_SUB,   0xFFFFFFFFu, 0x73800000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 231, "mirrored"},
	{FORM_ADDA,  0xFFFFFFFFu, 0xF3800000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 231, "mirrored"},
	{FORM_SUBA,  0xFFFFFFFFu, 0x73800000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 231, "mirrored"},
	{FORM_MADD,  0xF3800000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 231, "mirrored"},
	{FORM_MSUB,  0x73800000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 231, "mirrored"},
	{FORM_MADDA, 0xF3800000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 231, "mirrored"},
	{FORM_MSUBA, 0x73800000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01000001u, 231, "mirrored"},
	{FORM_ADD,   0xFFFFFFFFu, 0xF4000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, 232, "mirrored"},
	{FORM_SUB,   0xFFFFFFFFu, 0x74000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, 232, "mirrored"},
	{FORM_ADDA,  0xFFFFFFFFu, 0xF4000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, 232, "mirrored"},
	{FORM_SUBA,  0xFFFFFFFFu, 0x74000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, 232, "mirrored"},
	{FORM_MADD,  0xF4000000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, 232, "mirrored"},
	{FORM_MSUB,  0x74000000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, 232, "mirrored"},
	{FORM_MADDA, 0xF4000000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, 232, "mirrored"},
	{FORM_MSUBA, 0x74000000u, 0x3F800000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x01008011u, 232, "mirrored"},
	{FORM_ADD,   0x7FFFFFFFu, 0x737FFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "mantissa"},
	{FORM_MADD,  0x737FFFFFu, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "mantissa"},
	{FORM_ADD,   0x7FFFFFFFu, 0x73FFFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "mantissa"},
	{FORM_MADD,  0x73FFFFFFu, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 231, "mantissa"},
	{FORM_ADD,   0x7FFFFFFFu, 0x747FFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "mantissa"},
	{FORM_MADD,  0x747FFFFFu, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 232, "mantissa"},
	{FORM_ADD,   0x7F800000u, 0x73000000u, 0x00000000u, 0x00000000u, 0x7F800000u, 0x01000001u, 230, "big shape"},
	{FORM_MADD,  0x73000000u, 0x3F800000u, 0x7F800000u, 0x00000000u, 0x7F800000u, 0x01000001u, 230, "big shape"},
	{FORM_ADD,   0x7F800000u, 0x73800000u, 0x00000000u, 0x00000000u, 0x7F800000u, 0x01000001u, 231, "big shape"},
	{FORM_MADD,  0x73800000u, 0x3F800000u, 0x7F800000u, 0x00000000u, 0x7F800000u, 0x01000001u, 231, "big shape"},
	{FORM_ADD,   0x7FC00000u, 0x73000000u, 0x00000000u, 0x00000000u, 0x7FC00000u, 0x01000001u, 230, "big shape"},
	{FORM_MADD,  0x73000000u, 0x3F800000u, 0x7FC00000u, 0x00000000u, 0x7FC00000u, 0x01000001u, 230, "big shape"},
	{FORM_ADD,   0x7FC00000u, 0x73800000u, 0x00000000u, 0x00000000u, 0x7FC00000u, 0x01000001u, 231, "big shape"},
	{FORM_MADD,  0x73800000u, 0x3F800000u, 0x7FC00000u, 0x00000000u, 0x7FC00000u, 0x01000001u, 231, "big shape"},
	{FORM_ADD,   0x7FFFFFFEu, 0x73000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFEu, 0x01000001u, 230, "big shape"},
	{FORM_MADD,  0x73000000u, 0x3F800000u, 0x7FFFFFFEu, 0x00000000u, 0x7FFFFFFEu, 0x01000001u, 230, "big shape"},
	{FORM_ADD,   0x7FFFFFFEu, 0x73800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFEu, 0x01000001u, 231, "big shape"},
	{FORM_MADD,  0x73800000u, 0x3F800000u, 0x7FFFFFFEu, 0x00000000u, 0x7FFFFFFEu, 0x01000001u, 231, "big shape"},
	{FORM_ADD,   0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 255, "control: saturates"},
	{FORM_ADD,   0x7F800000u, 0x7F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 255, "control: saturates"},
	{FORM_SUB,   0x7FFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 255, "control: saturates"},
	{FORM_MADD,  0x7FFFFFFFu, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 255, "control: saturates"},
	{FORM_MADDA, 0x7FFFFFFFu, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01008011u, 255, "control: saturates"},
	{FORM_ADD,   0x7FFFFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u,   0, "control: nothing to raise"},
	{FORM_ADD,   0x7FFFFFFFu, 0x80000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u,   0, "control: nothing to raise"},
	{FORM_SUB,   0x7FFFFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u,   0, "control: nothing to raise"},
	{FORM_ADD,   0x7F7FFFFFu, 0x6E000000u, 0x00000000u, 0x00000000u, 0x7F7FFFFFu, 0x01000001u, 220, "control: nothing to raise"},
	{FORM_ADD,   0x7F000000u, 0x6E000000u, 0x00000000u, 0x00000000u, 0x7F000000u, 0x01000001u, 220, "control: nothing to raise"},
	{FORM_MADD,  0x00000000u, 0x3F800000u, 0x7FFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x01000001u,   0, "control: nothing to raise"},
	{FORM_SUB,   0x7FFFFFFFu, 0x65800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "control: magnitudes subtract"},
	{FORM_ADD,   0x7FFFFFFFu, 0xE5800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 203, "control: magnitudes subtract"},
	{FORM_SUB,   0x7FFFFFFFu, 0x69000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 210, "control: magnitudes subtract"},
	{FORM_ADD,   0x7FFFFFFFu, 0xE9000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 210, "control: magnitudes subtract"},
	{FORM_SUB,   0x7FFFFFFFu, 0x72800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "control: magnitudes subtract"},
	{FORM_ADD,   0x7FFFFFFFu, 0xF2800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 229, "control: magnitudes subtract"},
	{FORM_SUB,   0x7FFFFFFFu, 0x73000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "control: magnitudes subtract"},
	{FORM_ADD,   0x7FFFFFFFu, 0xF3000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x01000001u, 230, "control: magnitudes subtract"},
	{FORM_SUB,   0x7FFFFFFFu, 0x73800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFEu, 0x01000001u, 231, "control: magnitudes subtract"},
	{FORM_ADD,   0x7FFFFFFFu, 0xF3800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFEu, 0x01000001u, 231, "control: magnitudes subtract"},
	{FORM_SUB,   0x7FFFFFFFu, 0x78000000u, 0x00000000u, 0x00000000u, 0x7FFFFEFFu, 0x01000001u, 240, "control: magnitudes subtract"},
	{FORM_ADD,   0x7FFFFFFFu, 0xF8000000u, 0x00000000u, 0x00000000u, 0x7FFFFEFFu, 0x01000001u, 240, "control: magnitudes subtract"},
};
constexpr int kConsoleCount = static_cast<int>(std::size(kConsole));

} // namespace

// Both clamp modes of the DOUBLE engine, and the interpreter, against the
// console.
TEST(EeFpuOuRoundingConsole, EveryEngineMatchesTheConsole)
{
	struct Col { const char* name; bool jit, exact; };
	constexpr Col kCols[] = {
		{"jit clamp mode 3", true, false},
		{"jit clamp mode 4", true, true},
		{"interpreter", false, true},
	};

	for (const Col& c : kCols)
	{
		for (int i = 0; i < kConsoleCount; i++)
		{
			const ConsoleCase& k = kConsole[i];
			SCOPED_TRACE(testing::Message()
				<< c.name << " case " << i << " " << FormName(k.form)
				<< " fs=" << std::hex << k.fs << " ft=" << k.ft
				<< " acc=" << k.acc << " addend exp " << std::dec << k.addendExp
				<< " (" << k.note << ")");
			const Out o = RunOne(k.form, k.fs, k.ft, k.acc, k.pre, c.jit, c.exact);
			EXPECT_EQ(o.val, k.want);
			EXPECT_EQ(o.fcr & kOU, k.wantFcr & kOU);
		}
	}
}

// Both polarities, or an engine that never raises O passes the table.
TEST(EeFpuOuRoundingConsole, TableCarriesBothPolarities)
{
	int raised = 0, clear = 0;
	for (const ConsoleCase& k : kConsole)
		((k.wantFcr & FPUflagO) ? raised : clear)++;
	EXPECT_GE(raised, 8);
	EXPECT_GE(clear, 8);

	// And both boundaries: the erasure one at 24, and the rounding one at 23.
	int at231 = 0, at232 = 0;
	for (const ConsoleCase& k : kConsole)
	{
		if (k.addendExp == 231 && !(k.wantFcr & FPUflagO))
			at231++;
		if (k.addendExp == 232 && (k.wantFcr & FPUflagO))
			at232++;
	}
	EXPECT_GE(at231, 8);
	EXPECT_GE(at232, 8);
}
