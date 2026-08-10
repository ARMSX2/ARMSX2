// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// What the EE FPU does when a result underflows, against the console.
//
// A result strictly below 2^-126 and not zero is not flushed by every op. The
// add/sub family leaves the mantissa bits where normalisation put them and
// forces the exponent field to 0; the multiplies and the divide clear them.
//
//     add.s  0x00800003, 0x80800000   ->  console 00400000   (U|SU)
//     mul.s  0x00C00000, 0x3E800000   ->  console 00000000   (U|SU)
//     div.s  0x00C00000, 0x40000000   ->  console 00000000   (no flag at all)
//
// The add.s row is not a rounding: the exact result is 3*2^-149 and 00400000
// is 2^-127, six orders of magnitude larger. It is the raw mantissa.
//
// Provenance: every expected word below is a hardware reading, taken on an
// SCPH-90000 over ps2link on 2026-08-03 by captures/fpuflow/uf.c. Two runs,
// byte-identical; hw-run1.bin/hw-run2.bin and the decoder are archived beside
// the probe. It was written for this question because the 1147-case FP matrix
// corpus has no rows anywhere in the region, on hardware or on any engine --
// the "empty cells fake exact laws" failure shape.
//
// Why the region decides: every EE normal is an integer multiple of 2^(e-23)
// with e >= -126, so any sum or difference of two normals is an integer
// multiple of 2^-149, and a result below 2^-126 is exactly k * 2^-149 with
// 1 <= k < 2^23. Three hypotheses give three different words:
//
//     flush     0                                    signed zero
//     IEEE      k                                    the true value, exact,
//                                                    since k is the denormal
//                                                    single's mantissa
//     mantissa  (k << (24 - bitlen(k))) & 0x7FFFFF   the bits left in place
//
// Hardware, over the 72 rows: 45 say `mantissa` and only `mantissa` (all of
// them add/sub family); 14 agree with `mantissa` and with `flush`, which
// coincide on a power-of-two k (12 add/sub, 2 MUL/DIV); 2 have all three
// coincide (an exact-zero result); 4 say `flush` and only `flush` (all
// MUL/DIV); 7 are controls or results off the 2^-149 grid. No row says IEEE
// and no row matches nothing. The 45 + 4 = 49 uniquely-decided rows are the 49
// the generator counts as separating all three, which is the check on the
// arithmetic. x86 iFPUd.cpp's comment guessed the rule; the quote is in
// ee_rec_fpu_full_mode_tests.cpp.
//
// The model is FPU.cpp's eeRoundToSingle(addsub); mode 3 (iFPUd-arm64.cpp,
// ToPS2FPU_Full's addsub arm) already had it, and the single-precision fast
// path is pinned below as a divergence.
//
// FCR31: an underflow to a nonzero value raises U|SU on add/sub/mul, and the
// divide raises nothing. Comparisons here are masked to the eight cause/sticky
// bits, because the console's FCR31 rests at 0x01000001 -- bits 0 and 24 do
// not accept a CTC1 write -- and the emulator does not model those.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFd = 4, kFs = 5, kFt = 6;

// I|D|O|U|SI|SD|SO|SU -- everything the console and the emulator can both say.
constexpr u32 kFlagMask = 0x0003C078u;

enum Form { FORM_ADD, FORM_SUB, FORM_MADD, FORM_MSUB, FORM_MUL, FORM_DIV };

u32 Encode(Form f)
{
	switch (f)
	{
		case FORM_ADD:  return ADD_S(kFd, kFs, kFt);
		case FORM_SUB:  return SUB_S(kFd, kFs, kFt);
		case FORM_MADD: return MADD_S(kFd, kFs, kFt);
		case FORM_MSUB: return MSUB_S(kFd, kFs, kFt);
		case FORM_MUL:  return MUL_S(kFd, kFs, kFt);
		default:        return DIV_S(kFd, kFs, kFt);
	}
}

const char* FormName(Form f)
{
	switch (f)
	{
		case FORM_ADD:  return "add.s";
		case FORM_SUB:  return "sub.s";
		case FORM_MADD: return "madd.s";
		case FORM_MSUB: return "msub.s";
		case FORM_MUL:  return "mul.s";
		default:        return "div.s";
	}
}

struct ConsoleCase
{
	Form form;
	u32 fs, ft, acc;
	u32 want;       // console result
	u32 want_fcr;   // console FCR31, masked to kFlagMask
};

// Generated from captures/fpuflow/hw-run1.bin; see the provenance note above.
constexpr ConsoleCase kConsole[] = {
	{FORM_SUB, 0x00800001u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // A sub +1*2^-149
	{FORM_ADD, 0x00800001u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // A add +1*2^-149
	{FORM_SUB, 0x80800001u, 0x80800000u, 0x00000000u, 0x80000000u, 0x00004008u},  // A sub -1*2^-149
	{FORM_SUB, 0x00800002u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // A sub +2*2^-149
	{FORM_ADD, 0x00800002u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // A add +2*2^-149
	{FORM_SUB, 0x80800002u, 0x80800000u, 0x00000000u, 0x80000000u, 0x00004008u},  // A sub -2*2^-149
	{FORM_SUB, 0x00800003u, 0x00800000u, 0x00000000u, 0x00400000u, 0x00004008u},  // A sub +3*2^-149
	{FORM_ADD, 0x00800003u, 0x80800000u, 0x00000000u, 0x00400000u, 0x00004008u},  // A add +3*2^-149
	{FORM_SUB, 0x80800003u, 0x80800000u, 0x00000000u, 0x80400000u, 0x00004008u},  // A sub -3*2^-149
	{FORM_SUB, 0x00800005u, 0x00800000u, 0x00000000u, 0x00200000u, 0x00004008u},  // A sub +5*2^-149
	{FORM_ADD, 0x00800005u, 0x80800000u, 0x00000000u, 0x00200000u, 0x00004008u},  // A add +5*2^-149
	{FORM_SUB, 0x80800005u, 0x80800000u, 0x00000000u, 0x80200000u, 0x00004008u},  // A sub -5*2^-149
	{FORM_SUB, 0x00800007u, 0x00800000u, 0x00000000u, 0x00600000u, 0x00004008u},  // A sub +7*2^-149
	{FORM_ADD, 0x00800007u, 0x80800000u, 0x00000000u, 0x00600000u, 0x00004008u},  // A add +7*2^-149
	{FORM_SUB, 0x80800007u, 0x80800000u, 0x00000000u, 0x80600000u, 0x00004008u},  // A sub -7*2^-149
	{FORM_SUB, 0x0080000Fu, 0x00800000u, 0x00000000u, 0x00700000u, 0x00004008u},  // A sub +15*2^-149
	{FORM_ADD, 0x0080000Fu, 0x80800000u, 0x00000000u, 0x00700000u, 0x00004008u},  // A add +15*2^-149
	{FORM_SUB, 0x8080000Fu, 0x80800000u, 0x00000000u, 0x80700000u, 0x00004008u},  // A sub -15*2^-149
	{FORM_SUB, 0x00800100u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // A sub +256*2^-149
	{FORM_ADD, 0x00800100u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // A add +256*2^-149
	{FORM_SUB, 0x80800100u, 0x80800000u, 0x00000000u, 0x80000000u, 0x00004008u},  // A sub -256*2^-149
	{FORM_SUB, 0x00923456u, 0x00800000u, 0x00000000u, 0x0011A2B0u, 0x00004008u},  // A sub +1193046*2^-149
	{FORM_ADD, 0x00923456u, 0x80800000u, 0x00000000u, 0x0011A2B0u, 0x00004008u},  // A add +1193046*2^-149
	{FORM_SUB, 0x80923456u, 0x80800000u, 0x00000000u, 0x8011A2B0u, 0x00004008u},  // A sub -1193046*2^-149
	{FORM_SUB, 0x00955555u, 0x00800000u, 0x00000000u, 0x002AAAA8u, 0x00004008u},  // A sub +1398101*2^-149
	{FORM_ADD, 0x00955555u, 0x80800000u, 0x00000000u, 0x002AAAA8u, 0x00004008u},  // A add +1398101*2^-149
	{FORM_SUB, 0x80955555u, 0x80800000u, 0x00000000u, 0x802AAAA8u, 0x00004008u},  // A sub -1398101*2^-149
	{FORM_SUB, 0x00AAAAABu, 0x00800000u, 0x00000000u, 0x002AAAACu, 0x00004008u},  // A sub +2796203*2^-149
	{FORM_ADD, 0x00AAAAABu, 0x80800000u, 0x00000000u, 0x002AAAACu, 0x00004008u},  // A add +2796203*2^-149
	{FORM_SUB, 0x80AAAAABu, 0x80800000u, 0x00000000u, 0x802AAAACu, 0x00004008u},  // A sub -2796203*2^-149
	{FORM_SUB, 0x00C00000u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // A sub +4194304*2^-149
	{FORM_ADD, 0x00C00000u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // A add +4194304*2^-149
	{FORM_SUB, 0x80C00000u, 0x80800000u, 0x00000000u, 0x80000000u, 0x00004008u},  // A sub -4194304*2^-149
	{FORM_SUB, 0x00C00001u, 0x00800000u, 0x00000000u, 0x00000002u, 0x00004008u},  // A sub +4194305*2^-149
	{FORM_ADD, 0x00C00001u, 0x80800000u, 0x00000000u, 0x00000002u, 0x00004008u},  // A add +4194305*2^-149
	{FORM_SUB, 0x80C00001u, 0x80800000u, 0x00000000u, 0x80000002u, 0x00004008u},  // A sub -4194305*2^-149
	{FORM_SUB, 0x00D55555u, 0x00800000u, 0x00000000u, 0x002AAAAAu, 0x00004008u},  // A sub +5592405*2^-149
	{FORM_ADD, 0x00D55555u, 0x80800000u, 0x00000000u, 0x002AAAAAu, 0x00004008u},  // A add +5592405*2^-149
	{FORM_SUB, 0x80D55555u, 0x80800000u, 0x00000000u, 0x802AAAAAu, 0x00004008u},  // A sub -5592405*2^-149
	{FORM_SUB, 0x00FFFFFFu, 0x00800000u, 0x00000000u, 0x007FFFFEu, 0x00004008u},  // A sub +8388607*2^-149
	{FORM_ADD, 0x00FFFFFFu, 0x80800000u, 0x00000000u, 0x007FFFFEu, 0x00004008u},  // A add +8388607*2^-149
	{FORM_SUB, 0x80FFFFFFu, 0x80800000u, 0x00000000u, 0x807FFFFEu, 0x00004008u},  // A sub -8388607*2^-149
	{FORM_SUB, 0x01000000u, 0x00FFFFFDu, 0x00000000u, 0x00400000u, 0x00004008u},  // B sub +3*2^-149
	{FORM_SUB, 0x01000000u, 0x00FFFFFBu, 0x00000000u, 0x00200000u, 0x00004008u},  // B sub +5*2^-149
	{FORM_SUB, 0x01000000u, 0x00EDCBAAu, 0x00000000u, 0x0011A2B0u, 0x00004008u},  // B sub +1193046*2^-149
	{FORM_SUB, 0x01000000u, 0x00800001u, 0x00000000u, 0x007FFFFEu, 0x00004008u},  // B sub +8388607*2^-149
	{FORM_SUB, 0x08000000u, 0x07FFFFFDu, 0x00000000u, 0x00400000u, 0x00004008u},  // C sub +3*2^-136
	{FORM_SUB, 0x08000000u, 0x07FFFFFBu, 0x00000000u, 0x00200000u, 0x00004008u},  // C sub +5*2^-136
	{FORM_MADD, 0x80800000u, 0x3F800000u, 0x00800003u, 0x00400000u, 0x00004008u},  // D madd +3*2^-149
	{FORM_MSUB, 0x00800000u, 0x3F800000u, 0x00800003u, 0x00400000u, 0x00004008u},  // D msub +3*2^-149
	{FORM_MADD, 0x00800000u, 0x3F800000u, 0x80800003u, 0x80400000u, 0x00004008u},  // D madd -3*2^-149
	{FORM_MADD, 0x80800000u, 0x3F800000u, 0x00923456u, 0x0011A2B0u, 0x00004008u},  // D madd +1193046*2^-149
	{FORM_MSUB, 0x00800000u, 0x3F800000u, 0x00923456u, 0x0011A2B0u, 0x00004008u},  // D msub +1193046*2^-149
	{FORM_MADD, 0x00800000u, 0x3F800000u, 0x80923456u, 0x8011A2B0u, 0x00004008u},  // D madd -1193046*2^-149
	{FORM_MADD, 0x80800000u, 0x3F800000u, 0x00FFFFFFu, 0x007FFFFEu, 0x00004008u},  // D madd +8388607*2^-149
	{FORM_MSUB, 0x00800000u, 0x3F800000u, 0x00FFFFFFu, 0x007FFFFEu, 0x00004008u},  // D msub +8388607*2^-149
	{FORM_MADD, 0x00800000u, 0x3F800000u, 0x80FFFFFFu, 0x807FFFFEu, 0x00004008u},  // D madd -8388607*2^-149
	{FORM_MUL, 0x00C00000u, 0x3E800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // E mul 1.5*2^-128
	{FORM_MUL, 0x00800000u, 0x3E800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // E mul 2^-128
	{FORM_MUL, 0x00C00000u, 0x3D800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // E mul 1.5*2^-130
	{FORM_MUL, 0x00FFFFFFu, 0x3F000000u, 0x00000000u, 0x00000000u, 0x00004008u},  // E mul off-grid (needs 24 bits)
	{FORM_MUL, 0x00800000u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00004008u},  // E mul 2^-252 (far below)
	{FORM_DIV, 0x00C00000u, 0x40000000u, 0x00000000u, 0x00000000u, 0x00000000u},  // F div 1.5*2^-127
	{FORM_DIV, 0x00800000u, 0x41000000u, 0x00000000u, 0x00000000u, 0x00000000u},  // F div 2^-129
	{FORM_DIV, 0x00C00000u, 0x41000000u, 0x00000000u, 0x00000000u, 0x00000000u},  // F div 1.5*2^-129
	{FORM_SUB, 0x00800000u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00000000u},  // G ctl exact +0 cancellation
	{FORM_ADD, 0x00800000u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00000000u},  // G ctl exact +0 add
	{FORM_SUB, 0x01000000u, 0x00800000u, 0x00000000u, 0x00800000u, 0x00000000u},  // G ctl result == 2^-126
	{FORM_ADD, 0x3F800000u, 0x3F800000u, 0x00000000u, 0x40000000u, 0x00000000u},  // G ctl result == 2.0
	{FORM_ADD, 0x00400000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},  // G ctl denormal operand
	{FORM_MUL, 0x3F800000u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u},  // G ctl denormal operand mul
	{FORM_SUB, 0x00000003u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u},  // G ctl two denormal operands
};
constexpr int kConsoleCount = static_cast<int>(sizeof(kConsole) / sizeof(kConsole[0]));

enum Engine { ENGINE_INTERP, ENGINE_JIT_FULL, ENGINE_JIT_FAST };

void RunOne(const ConsoleCase& c, Engine e, u32* out_val, u32* out_fcr)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (e == ENGINE_JIT_FULL)
		h.EnableFpuFullMode();
	h.SetFcr31(0);
	h.SetAccBits(c.acc);
	h.SetFprBits(kFs, c.fs);
	h.SetFprBits(kFt, c.ft);
	h.LoadProgram({Encode(c.form)});

	if (e == ENGINE_INTERP)
	{
		h.RunInterpOnly();
		*out_val = h.GetFprBitsInterp(kFd);
		*out_fcr = h.InterpSnapshot().fprs.fprc[31] & kFlagMask;
		return;
	}
	h.RunJitNoDiff();
	*out_val = h.GetFprBitsJit(kFd);
	*out_fcr = h.JitSnapshot().fprs.fprc[31] & kFlagMask;
}

} // namespace

TEST(EeFpuUnderflowConsole, InterpreterMatchesTheConsoleOnEveryRow)
{
	for (int i = 0; i < kConsoleCount; i++)
	{
		const ConsoleCase& c = kConsole[i];
		u32 val = 0, fcr = 0;
		RunOne(c, ENGINE_INTERP, &val, &fcr);
		EXPECT_EQ(val, c.want) << "row " << i << " " << FormName(c.form);
		EXPECT_EQ(fcr, c.want_fcr) << "row " << i << " " << FormName(c.form);
	}
}

TEST(EeFpuUnderflowConsole, FullModeRecompilerMatchesTheConsoleOnEveryRow)
{
	for (int i = 0; i < kConsoleCount; i++)
	{
		const ConsoleCase& c = kConsole[i];
		u32 val = 0, fcr = 0;
		RunOne(c, ENGINE_JIT_FULL, &val, &fcr);
		EXPECT_EQ(val, c.want) << "row " << i << " " << FormName(c.form);
		EXPECT_EQ(fcr, c.want_fcr) << "row " << i << " " << FormName(c.form);
	}
}

// The tier line, pinned rather than fixed.
//
// The fast path computes in single precision, so an underflowing result is
// already +/-0 by the time any model could look at it -- the mantissa the
// console keeps was never in a register. It also raises no O/U at all. Both
// are deliberate (fpuClearOUFlags in iFPU-arm64.cpp says why), so this test is
// what a change to either has to edit.
//
// Liveness: the rows below are exactly the ones where the console says
// something other than signed zero, so a fast path that modelled this would
// fail here at once.
TEST(EeFpuUnderflowConsole, FastPathFlushesAndRaisesNothingDivergingFromTheConsole)
{
	int diverged = 0;
	for (int i = 0; i < kConsoleCount; i++)
	{
		const ConsoleCase& c = kConsole[i];
		u32 val = 0, fcr = 0;
		RunOne(c, ENGINE_JIT_FAST, &val, &fcr);
		EXPECT_EQ(fcr, 0u) << "row " << i << " " << FormName(c.form);
		if (c.want != val || c.want_fcr != fcr)
			diverged++;
	}
	// 62 of the 72 rows differ: exactly the rows where the console raises U|SU,
	// which the fast path never does. The 10 where the console raises nothing
	// are controls it gets right (exact zero, 2^-126, 2.0, denormal operands,
	// and the three divides).
	EXPECT_EQ(diverged, 62);
}
