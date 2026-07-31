// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// ABS.S / NEG.S against the console, across the EE clamp modes.
//
// The console says both are pure sign-bit operations: they never clamp, and
// they preserve an exponent-255 operand exactly. From
// unknownbrackets/ps2autotests tests/cpu/ee_fpu/arithmetic.expected:
//
//     abs 7fffffff: 7fffffff     neg 7fffffff: ffffffff
//     abs ffffffff: 7fffffff     neg ffffffff: 7fffffff
//     abs 7f800000: 7f800000     neg 7f800000: ff800000
//
// The interpreter reproduces that. The arm64 recompiler does not: recABS_S_xmm
// and recNEG_S_xmm call fpuClampResultPositive / fpuClampResult with no
// CHECK_FPU_* gate, so every exponent-255 operand comes back as +-0x7F7FFFFF
// and eeClampMode has no effect at all. x86 gates ABS on CHECK_FPU_OVERFLOW;
// arm64 gates neither op on anything.
//
// That is a pre-existing defect -- it is present at the merge-base, not
// introduced by the FP work on this branch -- so the JIT leg is a DISABLED
// tripwire rather than a failing test. The interpreter leg is enabled and is a
// must-not-regress control: it is the side that matches silicon.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include <gtest/gtest.h>

#include <cstdio>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFd = 4, kFs = 5;

struct Operand
{
	u32 bits;
	const char* what;
};

constexpr Operand kOperands[] = {
	{0x7FFFFFFFu, "+EEMAX (exp255, mant max)"},
	{0xFFFFFFFFu, "-EEMAX"},
	{0x7F800000u, "+2^128 (exp255, mant 0)"},
	{0xFF800000u, "-2^128"},
	{0x7FC00000u, "exp255 mant 0x400000 (host qNaN)"},
	{0x7FA00000u, "exp255 mant 0x200000 (host sNaN)"},
	{0x7F7FFFFFu, "+FLT_MAX"},
	{0xFF7FFFFFu, "-FLT_MAX"},
	// Denormals. A separate sub-class from the exponent-255 rows above, and one
	// this pool had no member of until the clamp came out: ABS.S's clamp was an
	// Fminnm -- an ARITHMETIC op -- so FPCR.FZ flushed the operand to zero
	// before the compare. NEG.S's was an integer Smin/Umin and never did, which
	// is why the defect showed on ABS alone and would have hidden here.
	{0x00000001u, "+MIN_DENORM"},
	{0x80000001u, "-MIN_DENORM"},
	{0x007FFFFFu, "+MAX_DENORM"},
	{0x807FFFFFu, "-MAX_DENORM"},
	{0x00001337u, "denormal, mid payload"},
	{0x3F800000u, "+1.0 (control)"},
};
constexpr int kOperandCount = static_cast<int>(sizeof(kOperands) / sizeof(kOperands[0]));

// Console rows from two independent first-party captures. Nothing here is
// derived from either engine.
//
//   [psa] unknownbrackets/ps2autotests tests/cpu/ee_fpu/arithmetic.expected
//   [fpm] the 1147-case EE FPU capture (SCPH-90000), section C-signmove
//
// Only operands that appear verbatim in one of those two are listed, and the
// tag says which. The [fpm] rows were added when the clamp came out: they cover
// the denormal and signalling-NaN sub-classes that ps2autotests does not reach,
// and they are exactly the rows that had been silently wrong.
struct ConsoleCase
{
	u32 in;
	u32 want_abs;
	u32 want_neg;
	const char* what;
};

constexpr ConsoleCase kConsole[] = {
	{0x00000000u, 0x00000000u, 0x80000000u, "[psa] +0.0"},
	{0x80000000u, 0x00000000u, 0x00000000u, "[psa] -0.0"},
	{0x3F800000u, 0x3F800000u, 0xBF800000u, "[psa] +1.0"},
	{0x3FFFFFFFu, 0x3FFFFFFFu, 0xBFFFFFFFu, "[psa] CF_MAX_MANTISSA"},
	{0x7FFFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu, "[psa] CF_MAX / +EEMAX"},
	{0xFFFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, "[psa] CF_MIN / -EEMAX"},
	{0x7F800000u, 0x7F800000u, 0xFF800000u, "[psa] +2^128"},
	{0xFF800000u, 0x7F800000u, 0x7F800000u, "[psa] -2^128"},
	{0x7F800001u, 0x7F800001u, 0xFF800001u, "[psa] CF_MAX_EXP (exp255 sNaN)"},
	{0xDEADBEEFu, 0x5EADBEEFu, 0x5EADBEEFu, "[psa] CF_GARBAGE2"},
	// Exponent-255 shapes ps2autotests does not carry.
	{0x7FC00000u, 0x7FC00000u, 0xFFC00000u, "[fpm] QNAN (exp255 mant 0x400000)"},
	{0x7FA00000u, 0x7FA00000u, 0xFFA00000u, "[fpm] SNAN (exp255 mant 0x200000)"},
	// Denormals. The console keeps them; ABS.S's Fminnm clamp flushed them.
	{0x00000001u, 0x00000001u, 0x80000001u, "[fpm] MIN_DEN"},
	{0x80000001u, 0x00000001u, 0x00000001u, "[fpm] NMIN_DEN"},
	{0x00400000u, 0x00400000u, 0x80400000u, "[fpm] MID_DEN"},
	{0x007FFFFFu, 0x007FFFFFu, 0x807FFFFFu, "[fpm] MAX_DEN"},
	{0x807FFFFFu, 0x007FFFFFu, 0x007FFFFFu, "[fpm] NMAX_DEN"},
	{0x00001337u, 0x00001337u, 0x80001337u, "[fpm] GARB1 (denormal)"},
};
constexpr int kConsoleCount = static_cast<int>(sizeof(kConsole) / sizeof(kConsole[0]));

enum Mode
{
	MODE_CLAMP0,   // eeClampMode 0 -- fpuOverflow off
	MODE_DEFAULT,  // eeClampMode 1 -- fpuOverflow on (shipping default)
	MODE_EXTRA,    // eeClampMode 2 -- + fpuExtraOverflow
	MODE_COUNT
};

const char* ModeName(Mode m)
{
	switch (m)
	{
		case MODE_CLAMP0:  return "clamp0 ";
		case MODE_DEFAULT: return "default";
		case MODE_EXTRA:   return "extra  ";
		default:           return "?";
	}
}

u32 RunOne(u32 insn, u32 fs_bits, bool jit, Mode mode)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (mode == MODE_CLAMP0)
		h.DisableFpuOverflow();
	if (mode == MODE_EXTRA)
		h.EnableFpuExtraOverflow();
	h.SetFprBits(kFs, fs_bits);
	h.LoadProgram({insn});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	return jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd);
}


} // namespace

// ---------------------------------------------------------------------------
// The interpreter is the console-matching side. Enabled: this must not
// regress, and it is what the JIT has to be brought to.
// ---------------------------------------------------------------------------
TEST(EeFpuAbsNegClamp, InterpMatchesConsoleInEveryClampMode)
{
	int checked = 0;
	for (int m = 0; m < MODE_COUNT; ++m)
	{
		const Mode mode = static_cast<Mode>(m);
		for (int i = 0; i < kConsoleCount; ++i)
		{
			const ConsoleCase& c = kConsole[i];
			SCOPED_TRACE(testing::Message()
				<< c.what << " [" << ModeName(mode) << "]");
			EXPECT_EQ(RunOne(ABS_S(kFd, kFs), c.in, false, mode), c.want_abs)
				<< "abs.s must be a pure sign-bit clear";
			EXPECT_EQ(RunOne(NEG_S(kFd, kFs), c.in, false, mode), c.want_neg)
				<< "neg.s must be a pure sign-bit flip";
			checked += 2;
		}
	}
	EXPECT_EQ(checked, kConsoleCount * MODE_COUNT * 2) << "anti-vacuity";
}

// ---------------------------------------------------------------------------
// Tripwire for the pre-existing arm64 defect. Delete the DISABLED_ prefix once
// recABS_S_xmm / recNEG_S_xmm stop clamping; it should then pass unchanged.
//
// Currently fails on every exponent-255 row, in all three clamp modes,
// because the clamp is emitted with no CHECK_FPU_* gate.
// ---------------------------------------------------------------------------
TEST(EeFpuAbsNegClamp, JitMatchesConsoleInEveryClampMode)
{
	int checked = 0;
	for (int m = 0; m < MODE_COUNT; ++m)
	{
		const Mode mode = static_cast<Mode>(m);
		for (int i = 0; i < kConsoleCount; ++i)
		{
			const ConsoleCase& c = kConsole[i];
			SCOPED_TRACE(testing::Message()
				<< c.what << " [" << ModeName(mode) << "]");
			EXPECT_EQ(RunOne(ABS_S(kFd, kFs), c.in, true, mode), c.want_abs);
			EXPECT_EQ(RunOne(NEG_S(kFd, kFs), c.in, true, mode), c.want_neg);
			checked += 2;
		}
	}
	EXPECT_EQ(checked, kConsoleCount * MODE_COUNT * 2) << "anti-vacuity";
}

// ---------------------------------------------------------------------------
// The two sub-classes the clamp corrupted, asserted as classes rather than as
// whichever operands the console captures happen to contain. Both engines, so
// this also pins that they agree.
//
// Kept separate from the table-driven test above because these are properties,
// not transcriptions: ABS.S clears bit 31 and changes nothing else, NEG.S flips
// it and changes nothing else. Every operand in the pool must satisfy that, and
// the pool deliberately contains patterns the console captures do not.
// ---------------------------------------------------------------------------
TEST(EeFpuAbsNegClamp, AbsAndNegAreExactBitOperationsOnEveryOperand)
{
	int exp255 = 0, denorm = 0;
	for (int m = 0; m < MODE_COUNT; ++m)
	{
		const Mode mode = static_cast<Mode>(m);
		for (int i = 0; i < kOperandCount; ++i)
		{
			const u32 in = kOperands[i].bits;
			SCOPED_TRACE(testing::Message()
				<< kOperands[i].what << " [" << ModeName(mode) << "]");
			for (int jit = 0; jit < 2; ++jit)
			{
				SCOPED_TRACE(jit ? "jit" : "interp");
				EXPECT_EQ(RunOne(ABS_S(kFd, kFs), in, jit != 0, mode), in & 0x7FFFFFFFu);
				EXPECT_EQ(RunOne(NEG_S(kFd, kFs), in, jit != 0, mode), in ^ 0x80000000u);
			}
			if (m != 0)
				continue;
			if ((in & 0x7F800000u) == 0x7F800000u)
				++exp255;
			else if ((in & 0x7F800000u) == 0 && (in & 0x7FFFFFu) != 0)
				++denorm;
		}
	}
	EXPECT_GT(exp255, 4) << "anti-vacuity: the pool must keep exponent-255 rows "
							"-- the class the clamp folded to +/-fMax";
	EXPECT_GT(denorm, 3) << "anti-vacuity: the pool must keep denormals -- the "
							"class ABS.S's Fminnm flushed to zero, and the one "
							"a pool built only from exponent-255 rows misses";
}

// ---------------------------------------------------------------------------
// Second, independent defect in the same two emitters, found while removing the
// clamp: the fast path never cleared the O and U cause flags. Interp ABS_S and
// NEG_S both call clearFPUFlags(FPUflagO | FPUflagU) (FPU.cpp) and the FULL
// path emits ClearOUFlags (iFPUd-arm64.cpp); only the fast path skipped it, so
// an overflow flag raised by an earlier op survived an ABS.S that the manual
// says clears it.
//
// It never showed in the console captures because every row there starts from a
// clean FCR31, which is why this seeds the flags instead. The value column is
// asserted too, so a regression that clears the flags by recomputing the result
// wrongly cannot pass.
// ---------------------------------------------------------------------------
TEST(EeFpuAbsNegClamp, AbsAndNegClearOverflowFlags)
{
	constexpr u32 kO = 0x00008000u, kU = 0x00004000u;
	constexpr u32 kSticky = 0x00000010u | 0x00000008u; // SO|SU: NOT cleared
	constexpr u32 kSeed = 0x01000001u | kO | kU | kSticky;

	for (int jit = 0; jit < 2; ++jit)
	{
		for (u32 insn : {ABS_S(kFd, kFs), NEG_S(kFd, kFs)})
		{
			SCOPED_TRACE(testing::Message()
				<< (insn == ABS_S(kFd, kFs) ? "abs.s" : "neg.s")
				<< (jit ? " [jit]" : " [interp]"));
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFcr31(kSeed);
			h.SetFprBits(kFs, 0xC0800000u); // -4.0, an ordinary operand
			h.LoadProgram({insn});
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();

			const u32 got = (jit ? h.JitSnapshot() : h.InterpSnapshot()).fprs.fprc[31];
			EXPECT_EQ(got & (kO | kU), 0u) << "O|U must be cleared";
			EXPECT_EQ(got & kSticky, kSticky)
				<< "the sticky SO|SU flags must survive -- only the cause bits clear";
			// abs(-4.0) and neg(-4.0) are both +4.0, so one expectation covers
			// both instructions here.
			EXPECT_EQ(jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd), 0x40800000u);
		}
	}
}

// ---------------------------------------------------------------------------
// eeClampMode is inert for ABS.S / NEG.S on the JIT: all three modes produce
// the same word. Pinning it enabled means the day someone wires the gate up,
// this fails and points at the tripwire above rather than going unnoticed.
// ---------------------------------------------------------------------------
TEST(EeFpuAbsNegClamp, JitIgnoresEeClampModeForAbsAndNeg)
{
	int exp255_rows = 0;
	for (int i = 0; i < kOperandCount; ++i)
	{
		const u32 in = kOperands[i].bits;
		SCOPED_TRACE(kOperands[i].what);
		for (u32 insn : {ABS_S(kFd, kFs), NEG_S(kFd, kFs)})
		{
			const u32 at0 = RunOne(insn, in, true, MODE_CLAMP0);
			EXPECT_EQ(at0, RunOne(insn, in, true, MODE_DEFAULT))
				<< "eeClampMode 0 vs 1 differ -- the gate now exists, enable "
				   "DISABLED_JitMatchesConsoleInEveryClampMode";
			EXPECT_EQ(at0, RunOne(insn, in, true, MODE_EXTRA))
				<< "eeClampMode 0 vs 2 differ -- see above";
		}
		if ((in & 0x7F800000u) == 0x7F800000u)
			++exp255_rows;
	}
	EXPECT_GT(exp255_rows, 4) << "anti-vacuity: the operand pool must keep "
								 "exponent-255 rows, which are the only ones "
								 "the clamp can act on";
}

// ---------------------------------------------------------------------------
// Liveness witness for the harness knob itself -- RETIRED, and deliberately not
// replaced by a weaker one. Read this before adding a clamp0 leg anywhere.
//
// DisableFpuOverflow() is observationally a no-op on ABS.S/NEG.S (they ignore
// the mode), so this file needs some independent proof that the switch reaches
// the emitter at all; otherwise every MODE_CLAMP0 leg here is measuring the
// default mode twice and the mode axis of this file is decoration.
//
// The witness rode on SQRT.S until SQRT.S stopped clamping its operand
// (exponent 255 is an ordinary binade on the EE, so it scales by a power of two
// and gets the console's answer in every mode -- recSQRT_S_xmm in
// iFPU-arm64.cpp, EeFpuOverflowConsole.SqrtMatchesConsoleOnEveryExponent255Operand).
// It then moved to MAX.S / MIN.S, the last CHECK_FPU_OVERFLOW-gated emitter
// path. Those stopped clamping too, for the same reason and against the same
// capture -- see EeRecFpu.MaxMinDoNotClampOperandsInAnyClampMode and
// ee_fpu_minmax_console_tests.cpp.
//
// CHECK_FPU_OVERFLOW now gates NO arm64 emitter path. grep says the macro has
// exactly one use left in pcsx2/arm64/ and it is inside a comment. The knob is
// still live in the x86 recompiler, still set by GameDatabase.cpp from
// eeClampMode >= 1, and still shown in the UI -- but on this port eeClampMode 0
// and 1 emit identical code. There is therefore nothing left in the EE FPU that
// can witness it, and a witness that asserted "the two modes agree" would prove
// only that the knob is dead, which is what this comment says instead.
//
// Consequences, stated so nobody re-derives them: MODE_CLAMP0 legs in this file
// and in ee_fpu_minmax_console_tests.cpp are redundant with MODE_DEFAULT today.
// They are kept because they cost microseconds and because they will start
// carrying information the moment some emitter gates on the flag again -- at
// which point this witness must come back, riding on that emitter.
// ---------------------------------------------------------------------------



// ---------------------------------------------------------------------------
// The measurement that produced the table above. Kept so the reading can be
// re-made from data rather than from the emitter source.
// ---------------------------------------------------------------------------
TEST(EeFpuAbsNegClamp, DISABLED_DumpAllLegs)
{
	struct Op { u32 insn; const char* name; } ops[] = {
		{ABS_S(kFd, kFs), "abs.s"},
		{NEG_S(kFd, kFs), "neg.s"},
	};

	for (const Op& op : ops)
	{
		std::printf("\n%-6s %-28s %-8s %-9s %-9s %s\n",
			"op", "operand", "mode", "interp", "jit", "");
		for (int i = 0; i < kOperandCount; ++i)
		{
			for (int m = 0; m < MODE_COUNT; ++m)
			{
				const u32 in = RunOne(op.insn, kOperands[i].bits, false, static_cast<Mode>(m));
				const u32 ji = RunOne(op.insn, kOperands[i].bits, true, static_cast<Mode>(m));
				std::printf("%-6s %-28s %-8s %08x  %08x  %s\n",
					op.name, kOperands[i].what, ModeName(static_cast<Mode>(m)),
					in, ji, in == ji ? "" : "<-- DIVERGE");
			}
		}
	}
}
