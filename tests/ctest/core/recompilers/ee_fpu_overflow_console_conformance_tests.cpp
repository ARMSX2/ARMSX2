// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// EE FPU overflow/underflow against a first-party PS2 capture.
//
// The capture and the rule it establishes are documented in autocases_fpuovf.h.
// The short version, because everything below turns on it:
//
//   The EE FPU's representable maximum is 0x7FFFFFFF == (2 - 2^-23) * 2^128.
//   Exponent 255 is an ordinary exponent; there is no Inf and no NaN. Overflow
//   means exceeding THAT, it saturates there, and only then are O and SO
//   raised.
//
// That is one binade ABOVE what IEEE single can represent, which is the reason
// PCSX2's fast path cannot match the console here no matter how the flag test
// is written: the host cannot hold the EE's top octave at all, so a result the
// console returns exactly (+FLT_MAX + +FLT_MAX == 0x7FFFFFFF, FCR31 untouched)
// necessarily arrives as a host overflow. The FULL double path can hold it,
// and does -- see iFPUd-arm64.cpp ToPS2FPU_Full and the 0x7fffffff constant in
// ee_rec_fpu_full_mode_tests.cpp.
//
// WHAT IS IN SCOPE HERE. Aligning the three engines with each other. A console
// divergence that all engines share is an accepted end state at this stage; an
// engine-vs-engine divergence is not. The console column is therefore carried
// as data and asserted only by the DISABLED tripwire at the bottom.
//
// SQRT.S is the one op that has left this compromise. Exponent 255 is an
// ordinary binade, so its operands never needed saturating at all: both engines
// now compute sqrt(|Ft|/4)*2 and match the console exactly, without widening to
// double and without changing any operand whose exponent field is <= 254. That
// is what moved rows 44 and 45 out of the value-only column below. The same
// argument is available to ABS.S (see the DISABLED EeFpuAbsNegClamp tripwire)
// but NOT to the arithmetic ops, whose results genuinely exceed what the host
// single can hold.
//
// The measured console divergences, all shared by both engines and all
// deliberate, for the record:
//   * 17 rows differ in VALUE only -- the +/-FLT_MAX saturation compromise.
//     DO NOT "fix" posFmax (pcsx2/FPU.cpp:14) globally; that pushes host
//     exponent-255 patterns through every downstream op in the clamp mode
//     nearly every game runs in. ee_fpu_zero_divisor_console_tests.cpp carries
//     the same warning and the serial list behind it.
//   * 3 rows differ in FLAGS only -- underflow U|SU, which needs FZ off. Owned
//     by DISABLED_UnderflowFlagsNeedFzOff in the FCR conformance file.
//   * 15 rows differ in both, the overflow rows, for the binade reason above.
//     Owned by DISABLED_ExceptionFlagsInProductionFpEnvMissOverflow.
//   * 22 rows match exactly.

#include "autocases_fpuovf.h"
#include "harness/EeRecTestHarness.h"

#include "Config.h"

#include <gtest/gtest.h>

#include <ios>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;
using namespace console_fpuovf;

namespace {

constexpr u32 kFd = 4, kFs = 5, kFt = 6;

constexpr u32 kFlagO = 0x00008000u;
constexpr u32 kFcr31FixedOnes = 0x01000001u;
constexpr u32 kFastPathMax = 0x7F7FFFFFu;

struct Observed
{
	u32 result;
	u32 fcr31;
};

u32 EncodeOp(FpuOvfOp op)
{
	switch (op)
	{
		case FO_ADD:   return ADD_S(kFd, kFs, kFt);
		case FO_SUB:   return SUB_S(kFd, kFs, kFt);
		case FO_MUL:   return MUL_S(kFd, kFs, kFt);
		case FO_DIV:   return DIV_S(kFd, kFs, kFt);
		case FO_SQRT:  return SQRT_S(kFd, kFt);
		case FO_RSQRT: return RSQRT_S(kFd, kFs, kFt);
		case FO_ADDA:  return ADDA_S(kFs, kFt);
		case FO_SUBA:  return SUBA_S(kFs, kFt);
		case FO_MULA:  return MULA_S(kFs, kFt);
		case FO_MADD:  return MADD_S(kFd, kFs, kFt);
		case FO_MSUB:  return MSUB_S(kFd, kFs, kFt);
		case FO_MADDA: return MADDA_S(kFs, kFt);
		case FO_MSUBA: return MSUBA_S(kFs, kFt);
	}
	return 0;
}

bool ReadsAcc(FpuOvfOp op)
{
	return op == FO_MADD || op == FO_MSUB || op == FO_MADDA || op == FO_MSUBA;
}

Observed RunCase(const FpuOvfCase& c, bool jit, bool extra_overflow = false)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (extra_overflow)
		h.EnableFpuExtraOverflow();
	// The console reached every row through `ctc1 $0, $31`, which reads back as
	// the fixed-ones pattern, so seed that rather than a bare zero -- otherwise
	// every row reports a flag mismatch that is only the harness writing the
	// register more directly than CTC1 can.
	h.SetFcr31(kFcr31FixedOnes);
	h.SetFprBits(kFs, c.fs);
	h.SetFprBits(kFt, c.ft);
	if (ReadsAcc(c.op))
		h.SetAccBits(c.acc);
	h.LoadProgram({EncodeOp(c.op)});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();

	Observed o;
	if (c.acc_dest)
		o.result = jit ? h.GetAccBitsJit() : h.GetAccBitsInterp();
	else
		o.result = jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd);
	o.fcr31 = (jit ? h.JitSnapshot() : h.InterpSnapshot()).fprs.fprc[31];
	return o;
}

bool Agree(const Observed& a, const Observed& b)
{
	return a.result == b.result && a.fcr31 == b.fcr31;
}

// Every row on which the interpreter and the arm64 recompiler disagree, with
// the reason and whether turning on the operand clamp closes it. Two classes,
// and they want different things done about them.
struct EngineDivergence
{
	int row;
	bool healed_by_extra_overflow;
	const char* why;
};

constexpr EngineDivergence kEngineDivergences[] = {
	// CLASS 1 -- the operand-clamp mode axis, not a defect. The interpreter
	// always clamps its sources through fpuDouble; the fast path only does so
	// under CHECK_FPU_EXTRA_OVERFLOW (GameDB eeClampMode >= 2). Every row here
	// feeds the op a raw exponent-255 word, so the two engines are not being
	// asked the same question until the clamp is on. Same axis as the "NAN
	// math" row in ee_fpu_fcr_console_conformance_tests.cpp.
	{12, true, "div +EEMAX, +EEMAX -- interp gets 1.0, JIT divides Inf by Inf"},
	{14, true, "mul 2^128, 0.5 -- same"},
	{17, true, "sub 2^128, 2^128 -- interp gets 0, JIT gets Inf-Inf"},

	// CLASS 3 -- the divide-unit ROUNDING axis, one row, and no clamp mode
	// touches it. sqrt(+EEMAX) is inexact in single precision, and the two
	// engines round it differently: the JIT's Fsqrt runs under FPUDivFPCR
	// (round-to-nearest, the divide unit's mode, and the value silicon
	// returns -- 0x5FB504F3), while the interpreter computes in double and
	// narrows under the ambient ChopZero, landing one ULP low at 0x5FB504F2.
	// The JIT is the console-exact side. Closes when the interpreter models
	// the div-unit rounding law; until then the interp's chop value is pinned
	// where each SQRT test asserts it.
	{44, false, "sqrt +EEMAX -- interp narrows under ChopZero, 1 ULP low"},

	// Rows 3, 11 and 16 used to be listed here and are not divergences any
	// more. They were never the operand-clamp axis: their RESULT words were
	// identical on both engines and only FCR31 differed, because the arm64 fast
	// path raised O|SO off a `fabs(result) > FLT_MAX` predicate -- a host-Inf
	// test, and therefore a function of eeRoundMode rather than of the
	// architecture. It fired here, where the console says no overflow, and
	// could not fire at all under the shipping ChopZero default. That emitter
	// is reverted, both engines now read 0x01000001 on all three, and the O/SO
	// question is deferred to the redesign (see the DISABLED tripwires in
	// ee_fpu_fcr_console_conformance_tests.cpp).

	// CLASS 2 used to live here: rows 44 and 45, sqrt of an exponent-255 Ft,
	// where recSQRT_S_xmm was the one emitter in iFPU-arm64.cpp that never
	// clamped its operand at all. That was a defect rather than a mode axis --
	// it did not close under CHECK_FPU_EXTRA_OVERFLOW because there was no gate
	// to turn on -- and it is fixed twice over: first with a clamp matching x86
	// recSQRT_S_xmm, then by dropping the clamp for the |Ft|/4 scaling that
	// lands on the console value (see SqrtMatchesConsoleOnEveryCapturedOperand).
	// Row 45 (sqrt 2^128) is exact and both engines agree on silicon's value;
	// row 44 remains above as CLASS 3, which is a rounding gap, not this one.
};
constexpr int kEngineDivergenceCount =
	static_cast<int>(sizeof(kEngineDivergences) / sizeof(kEngineDivergences[0]));

const EngineDivergence* FindDivergence(int row)
{
	for (int i = 0; i < kEngineDivergenceCount; ++i)
	{
		if (kEngineDivergences[i].row == row)
			return &kEngineDivergences[i];
	}
	return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// ENABLED, and the one that matters at this stage: the engines agree with each
// other on every row that is not on the documented list, and the rows that ARE
// on the list still diverge. The second half is what keeps the list from going
// stale -- a row that gets fixed without being removed here fails loudly
// instead of sitting as a silent allowance.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, EnginesAgreeExceptOnTheDocumentedRows)
{
	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		SCOPED_TRACE(::testing::Message() << "row " << i << ": " << c.what);
		const Observed in = RunCase(c, false);
		const Observed ji = RunCase(c, true);
		const EngineDivergence* d = FindDivergence(i);

		if (d == nullptr)
		{
			EXPECT_EQ(in.result, ji.result) << "result diverges between engines";
			EXPECT_EQ(in.fcr31, ji.fcr31) << "FCR31 diverges between engines";
		}
		else
		{
			EXPECT_FALSE(Agree(in, ji))
				<< "row " << i << " is listed as an engine divergence (" << d->why
				<< ") but the engines now agree -- delete the entry";
		}
	}
}

// ---------------------------------------------------------------------------
// ENABLED. Classifies the divergence list by measurement rather than by
// assertion in a comment: every listed row must close when the operand clamp
// is on. The else-branch is the liveness clause for any future entry that does
// NOT close -- a defect rather than the mode axis, which is what rows 44/45
// were before SQRT gained its clamp.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, OperandClampHealsEveryDocumentedDivergence)
{
	ASSERT_GT(kEngineDivergenceCount, 0) << "nothing left to classify";
	for (int i = 0; i < kEngineDivergenceCount; ++i)
	{
		const EngineDivergence& d = kEngineDivergences[i];
		const FpuOvfCase& c = kCases[d.row];
		SCOPED_TRACE(::testing::Message() << "row " << d.row << ": " << c.what
										  << " -- " << d.why);
		const Observed in = RunCase(c, false);
		const Observed jx = RunCase(c, true, /*extra_overflow=*/true);
		if (d.healed_by_extra_overflow)
		{
			EXPECT_TRUE(Agree(in, jx))
				<< "expected CHECK_FPU_EXTRA_OVERFLOW to close this row";
		}
		else
		{
			EXPECT_FALSE(Agree(in, jx))
				<< "this row is recorded as NOT closing under the operand "
				   "clamp; if it now does, SQRT gained a clamp and the entry "
				   "and its tripwire should go";
		}
	}
}

// ---------------------------------------------------------------------------
// ENABLED. The compromise, pinned. On every row the console overflowed, both
// engines must produce sign|0x7F7FFFFF -- the NON-console value. This exists so
// that an attempt at the console tripwire below cannot quietly change what the
// default clamp mode produces.
//
// If you are here because this failed: you changed the fast path's saturation.
// Scope the change to the FULL path instead.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, DefaultClampModeSaturatesToFltMaxOnBothEngines)
{
	ASSERT_FALSE(EmuConfig.Cpu.Recompiler.fpuFullMode)
		<< "this test describes the NON-full path; something enabled FULL mode";

	int checked = 0;
	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		if ((c.fcr31 & kFlagO) == 0)
			continue; // console did not call this row an overflow
		if (FindDivergence(i) != nullptr)
			continue; // covered by the divergence list instead
		++checked;
		const u32 want = (c.result & 0x80000000u) | kFastPathMax;
		SCOPED_TRACE(::testing::Message() << "row " << i << ": " << c.what);
		EXPECT_EQ(RunCase(c, false).result, want) << "interp";
		EXPECT_EQ(RunCase(c, true).result, want) << "jit";
	}
	EXPECT_GT(checked, 10) << "the console overflow rows vanished from the "
							  "capture; this test would pass vacuously";
}

// ---------------------------------------------------------------------------
// REGRESSION TEST for the defect this capture surfaced, and then for the fix.
//
// Round one: recSQRT_S_xmm was the one emitter in iFPU-arm64.cpp that never
// clamped its operand, so an exponent-255 Ft reached Fsqrt as a host +Inf and
// fpuClampResult flattened the result to 0x7F7FFFFF, while the interpreter's
// sqrt(fpuDouble(Ft)) landed two binades away at 0x5F7FFFFF. SQRT was given a
// clamp to match, and the engines agreed -- on 0x5F7FFFFF, which is not what
// the console returns either. Agreement is a weaker property than accuracy and
// that round bought it at the cost of accuracy.
//
// Round two, what this now pins: neither engine clamps this operand. Both
// compute sqrt(|Ft|/4)*2, which keeps operand and result inside the ordinary
// single range without widening to double -- exponent 255 is an ordinary binade
// on the EE, so there was never anything here to saturate. See SQRT_S
// (pcsx2/FPU.cpp) and recSQRT_S_xmm (pcsx2/arm64/iFPU-arm64.cpp).
//
// Before the fix this failed on rows 44 and 45 (console 5fb504f3 / 5f800000,
// both engines 5f7fffff) and passed on row 46, whose Ft has exponent field 254
// and so never reached the clamp. Row 46 is therefore the negative control for
// the scaling branch's condition: if the branch were simply always taken, or
// the condition inverted, row 46 would move.
//
// The console value is asserted, not merely engine agreement -- agreement can
// always be reached by degrading whichever engine is nearer silicon, which is
// how round one went wrong. Both clamp modes are checked because the old clamp
// was gated on CHECK_FPU_OVERFLOW and the replacement deliberately is not.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, SqrtMatchesConsoleOnEveryCapturedOperand)
{
	int exp255_rows = 0, control_rows = 0, total_rows = 0;
	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		if (c.op != FO_SQRT)
			continue;
		++total_rows;
		SCOPED_TRACE(::testing::Message() << "row " << i << ": " << c.what);

		// CLASS 3 (see kEngineDivergences): where the sqrt is inexact in
		// single precision the interpreter narrows under the ambient ChopZero
		// and lands one ULP below the divide unit's round-to-nearest. The JIT
		// column stays console-exact; the interp's chop value is pinned so the
		// gap closes loudly when the interp models the div-unit rounding law.
		const u32 interp_want =
			(c.result == 0x5FB504F3u) ? 0x5FB504F2u : c.result;

		for (int extra = 0; extra < 2; ++extra)
		{
			SCOPED_TRACE(::testing::Message()
						 << (extra ? "eeClampMode >= 2" : "default clamp mode"));
			const Observed in = RunCase(c, false, extra != 0);
			const Observed ji = RunCase(c, true, extra != 0);
			EXPECT_EQ(in.result, interp_want) << "interp result";
			EXPECT_EQ(ji.result, c.result) << "jit result vs console";
			EXPECT_EQ(in.fcr31, c.fcr31) << "interp FCR31 vs console";
			EXPECT_EQ(ji.fcr31, c.fcr31) << "jit FCR31 vs console";
		}

		if ((c.ft & 0x7F800000u) == 0x7F800000u)
			++exp255_rows;
		else
			++control_rows;
	}

	EXPECT_GT(total_rows, 0) << "no SQRT rows in the capture; vacuous";
	EXPECT_GT(exp255_rows, 0)
		<< "anti-vacuity: no SQRT row feeds an exponent-255 operand any more, "
		   "so the scaling path is never entered";
	EXPECT_GT(control_rows, 0)
		<< "anti-vacuity: no SQRT row with exponent field <= 254 is left, so "
		   "nothing here would notice the scaling being applied unconditionally";
}

// ---------------------------------------------------------------------------
// The same property as above, over the WHOLE exponent-255 class rather than the
// three patterns the capture happens to contain.
//
// This exists because the class splits on an axis the capture cannot see. As
// HOST bit patterns, exponent-255 words are infinities, quiet NaNs and
// signalling NaNs; to the EE they are all just large finite floats. The old
// arm64 clamp had to be an integer Umin rather than an Fminnm precisely because
// of that split -- FMINNM only prefers the number against a QUIET NaN, while a
// signalling operand comes back merely quieted, so half the mantissa space
// (4194303 of the 8388608 positive patterns) would have passed through a clamp
// that was supposed to catch it. Testing the exponent FIELD, as both engines
// now do, never asks the host what kind of NaN it thinks it is holding, so the
// whole taxonomy should be irrelevant -- and this is what proves it.
//
// Expected values are correctly-rounded square roots computed by exact integer
// arithmetic (math.isqrt on the significand, round-to-nearest-even, which is
// the divide unit's mode), NOT by a host float, so they cannot inherit the
// behaviour under test. That model was validated against silicon on the six
// operands the capture does witness -- marked `true` below -- and agreed on all
// six including the exponent-254 control. The three unwitnessed rows are
// therefore computed expectations, not measurements; they are here for class
// coverage and are flagged as such.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, SqrtMatchesConsoleOnEveryExponent255Operand)
{
	struct Operand
	{
		u32 ft;
		u32 want;        // correctly-rounded sqrt(|ft|) as an EE single (JIT)
		u32 interp_want; // the interp's value -- one ULP low where the sqrt is
		                 // inexact, because it narrows under the ambient
		                 // ChopZero rather than the divide unit's nearest.
		                 // CLASS 3 in kEngineDivergences; equal to `want`
		                 // everywhere the sqrt is exact or the roundings agree.
		bool witnessed;  // true == `want` was read off silicon
		const char* what;
	};
	// Every exponent-255 shape, both signs, plus one exponent-254 control.
	static constexpr Operand kOperands[] = {
		{0x7F800000u, 0x5F800000u, 0x5F800000u, true,  "+2^128        (host +Inf)"},
		{0xFF800000u, 0x5F800000u, 0x5F800000u, true,  "-2^128        (host -Inf)"},
		{0x7F800001u, 0x5F800000u, 0x5F800000u, false, "exp255 mant 1 (host +sNaN, smallest)"},
		{0xFF800001u, 0x5F800000u, 0x5F800000u, false, "exp255 mant 1 (host -sNaN, smallest)"},
		{0x7FBFFFFFu, 0x5F9CC470u, 0x5F9CC470u, false, "exp255 mant 0x3FFFFF (host +sNaN, largest)"},
		{0x7FC00000u, 0x5F9CC471u, 0x5F9CC470u, true,  "exp255 mant 0x400000 (host +qNaN, smallest)"},
		{0x7FFFFFFFu, 0x5FB504F3u, 0x5FB504F2u, true,  "+EEMAX        (host +qNaN, largest)"},
		{0xFFFFFFFFu, 0x5FB504F3u, 0x5FB504F2u, true,  "-EEMAX        (host -qNaN, largest)"},
		// CONTROL: exponent field 254, so the scaling branch must NOT fire.
		// If it does, this row comes back one binade low.
		{0xFF7FFFFFu, 0x5F7FFFFFu, 0x5F7FFFFFu, true,  "-FLT_MAX      (exp 254 -- CONTROL)"},
	};

	int signalling = 0, controls = 0, witnessed = 0;
	for (const Operand& o : kOperands)
	{
		const FpuOvfCase c{FO_SQRT, 0u, o.ft, 0u, 0u, 0u, false, o.what};
		SCOPED_TRACE(::testing::Message()
					 << o.what << (o.witnessed ? " [silicon]" : " [computed]"));

		// SQRT.S raises invalid on the sign bit alone -- exponent plays no part.
		const u32 want_fcr31 =
			kFcr31FixedOnes | ((o.ft & 0x80000000u) ? 0x00020040u : 0u);

		for (int extra = 0; extra < 2; ++extra)
		{
			SCOPED_TRACE(::testing::Message()
						 << (extra ? "eeClampMode >= 2" : "default clamp mode"));
			const Observed in = RunCase(c, false, extra != 0);
			const Observed ji = RunCase(c, true, extra != 0);
			EXPECT_EQ(in.result, o.interp_want) << "[interp] result";
			EXPECT_EQ(ji.result, o.want) << "[jit] result";
			EXPECT_EQ(in.fcr31, want_fcr31) << "[interp] FCR31";
			EXPECT_EQ(ji.fcr31, want_fcr31) << "[jit] FCR31";
		}

		const u32 mant = o.ft & 0x7FFFFFu;
		if ((o.ft & 0x7F800000u) != 0x7F800000u)
			++controls;
		else if (mant != 0 && (mant & 0x400000u) == 0)
			++signalling;
		if (o.witnessed)
			++witnessed;
	}

	EXPECT_GE(signalling, 3)
		<< "anti-vacuity: the operand pool must keep signalling-NaN patterns -- "
		   "they are the class a host-NaN-aware implementation would get wrong";
	EXPECT_GT(controls, 0)
		<< "anti-vacuity: without an exponent <= 254 operand nothing here would "
		   "notice the scaling being applied unconditionally";
	EXPECT_GE(witnessed, 5)
		<< "anti-vacuity: most of this pool must stay silicon-witnessed, or the "
		   "test is only checking the model against itself";
}

// ---------------------------------------------------------------------------
// TRIPWIRE for the later hardware-alignment stage. Both engines, every row,
// against the console. Expected to fail on 37 of 57 rows today for the three
// documented reasons at the top of this file.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, DISABLED_AllRowsMatchConsole)
{
	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(::testing::Message()
						 << "row " << i << ": " << c.what
						 << (jit ? " [jit]" : " [interp]"));
			const Observed o = RunCase(c, jit != 0);
			EXPECT_EQ(o.result, c.result);
			EXPECT_EQ(o.fcr31, c.fcr31);
		}
	}
}

// ---------------------------------------------------------------------------
// MEASUREMENT, not an assertion. Prints every row four ways so the console
// divergences can be counted and classified without guessing. Run with
// --gtest_also_run_disabled_tests --gtest_filter=*DumpConsoleComparison*
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, DISABLED_DumpConsoleComparison)
{
	int agree = 0, val_only = 0, flag_only = 0, both = 0;
	int engine_split = 0, split_healed = 0;
	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		const Observed in = RunCase(c, false);
		const Observed ji = RunCase(c, true);
		const Observed jx = RunCase(c, true, /*extra_overflow=*/true);
		const bool vbad = (in.result != c.result);
		const bool fbad = (in.fcr31 != c.fcr31);
		const bool split = !Agree(in, ji);
		const bool split_x = !Agree(in, jx);
		engine_split += split;
		split_healed += (split && !split_x);
		if (!vbad && !fbad)
			++agree;
		else if (vbad && fbad)
			++both;
		else if (vbad)
			++val_only;
		else
			++flag_only;

		printf("%-3d %-34s console %08x/%08x  interp %08x/%08x  jit %08x/%08x  "
			   "jit+xovf %08x/%08x %s%s%s%s\n",
			i, c.what, c.result, c.fcr31, in.result, in.fcr31, ji.result, ji.fcr31,
			jx.result, jx.fcr31, vbad ? "VAL " : "", fbad ? "FLAG " : "",
			split ? "ENGINE-SPLIT " : "", (split && !split_x) ? "(healed by xovf)" : "");
	}
	printf("\n%d rows: %d match console, %d value-only, %d flag-only, %d both\n",
		kCaseCount, agree, val_only, flag_only, both);
	printf("%d engine-split in default mode, %d of them healed by "
		   "CHECK_FPU_EXTRA_OVERFLOW\n", engine_split, split_healed);
}
