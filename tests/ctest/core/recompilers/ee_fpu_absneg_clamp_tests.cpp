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

constexpr u32 kFd = 4, kFs = 5, kFt = 6;

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
	{0x7FC00000u, "exp255 mant 0x400000"},
	{0x7F7FFFFFu, "+FLT_MAX"},
	{0xFF7FFFFFu, "-FLT_MAX"},
	{0x3F800000u, "+1.0 (control)"},
};
constexpr int kOperandCount = static_cast<int>(sizeof(kOperands) / sizeof(kOperands[0]));

// Console rows, transcribed from ps2autotests arithmetic.expected. Only
// operands that appear verbatim in that capture are listed -- nothing here is
// derived from either engine.
struct ConsoleCase
{
	u32 in;
	u32 want_abs;
	u32 want_neg;
	const char* what;
};

constexpr ConsoleCase kConsole[] = {
	{0x00000000u, 0x00000000u, 0x80000000u, "+0.0"},
	{0x80000000u, 0x00000000u, 0x00000000u, "-0.0"},
	{0x3F800000u, 0x3F800000u, 0xBF800000u, "+1.0"},
	{0x3FFFFFFFu, 0x3FFFFFFFu, 0xBFFFFFFFu, "CF_MAX_MANTISSA"},
	{0x7FFFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu, "CF_MAX / +EEMAX"},
	{0xFFFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, "CF_MIN / -EEMAX"},
	{0x7F800000u, 0x7F800000u, 0xFF800000u, "+2^128"},
	{0xFF800000u, 0x7F800000u, 0x7F800000u, "-2^128"},
	{0x7F800001u, 0x7F800001u, 0xFF800001u, "CF_MAX_EXP (exp255 sNaN)"},
	{0xDEADBEEFu, 0x5EADBEEFu, 0x5EADBEEFu, "CF_GARBAGE2"},
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

// Two-operand form, for the liveness witness below. RunOne cannot express it
// because it only ever loads Fs.
u32 RunTwo(u32 insn, u32 fs_bits, u32 ft_bits, bool jit, Mode mode)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (mode == MODE_CLAMP0)
		h.DisableFpuOverflow();
	if (mode == MODE_EXTRA)
		h.EnableFpuExtraOverflow();
	h.SetFprBits(kFs, fs_bits);
	h.SetFprBits(kFt, ft_bits);
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
TEST(EeFpuAbsNegClamp, DISABLED_JitMatchesConsoleInEveryClampMode)
{
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
// Liveness witness for the harness knob itself. DisableFpuOverflow() is
// observationally a no-op on ABS.S/NEG.S (they ignore the mode), so without
// this the switch would ship with nothing proving it reaches the emitter, and
// every clamp0 leg in this file would be measuring the default mode twice.
//
// This used to ride on SQRT.S, which gated its operand clamp on
// CHECK_FPU_OVERFLOW. SQRT.S no longer clamps that operand at all -- exponent
// 255 is an ordinary binade on the EE, so it scales by a power of two and gets
// the console's answer in every mode (see recSQRT_S_xmm in iFPU-arm64.cpp and
// EeFpuOverflowConsole.SqrtMatchesConsoleOnEveryExponent255Operand). That left
// MAX.S / MIN.S as the only remaining CHECK_FPU_OVERFLOW-gated emitter path
// (fpuClampMinMaxOperand, iFPU-arm64.cpp), so the witness moved there.
//
// recMAX_S_xmm is a bare Fmaxnm over two optionally-clamped operands with no
// result clamp behind it, so the knob is directly observable: clamped, +2^128
// arrives as +FLT_MAX and loses to nothing; unclamped it arrives as a host
// +Inf and survives Fmaxnm intact.
//
// Neither value is asserted as correct -- MAX.S has its own console divergence
// (it is an integer op on silicon; see fp_max in FPU.cpp). All this test claims
// is that the two modes are distinguishable.
// ---------------------------------------------------------------------------
TEST(EeFpuAbsNegClamp, DisableFpuOverflowReachesTheEmitter)
{
	constexpr u32 kExp255 = 0x7F800000u; // +2^128, a host +Inf
	constexpr u32 kOne = 0x3F800000u;

	const u32 clamped = RunTwo(MAX_S(kFd, kFs, kFt), kExp255, kOne, true, MODE_DEFAULT);
	const u32 unclamped = RunTwo(MAX_S(kFd, kFs, kFt), kExp255, kOne, true, MODE_CLAMP0);

	EXPECT_NE(clamped, unclamped)
		<< "DisableFpuOverflow() changed nothing -- the knob is not reaching "
		   "the MAX.S operand clamp, so every clamp0 leg in this file is "
		   "measuring the default mode twice";
	EXPECT_EQ(clamped, 0x7F7FFFFFu)
		<< "[clamp on] fpuClampMinMaxOperand pulls +2^128 down to +FLT_MAX";
	EXPECT_EQ(unclamped, kExp255)
		<< "[clamp off] the raw operand reaches Fmaxnm and wins";
}

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
