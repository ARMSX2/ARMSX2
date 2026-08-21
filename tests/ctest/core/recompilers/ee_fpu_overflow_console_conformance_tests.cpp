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
// SQRT.S was the first op to leave this compromise and the arithmetic ops have
// now followed: their operands never needed saturating, so SQRT computes
// sqrt(|Ft|/4)*2 and the rest go through eeToDouble() and eeRoundToSingle().
// That is what moved rows 44/45 and then most of the value-only column out of
// it.
//
// Do not do the same to the fast path by changing posFmax globally. It computes
// in host singles, so handing it exponent-255 words pushes host Inf/NaN patterns
// through every downstream op in the clamp mode nearly every game runs in.
// ee_fpu_zero_divisor_console_tests.cpp carries the same warning and the serial
// list behind it.
//
// The measured console divergences, for the record (re-measured with
// DISABLED_DumpConsoleComparison; earlier readings in parentheses):
//   * 57 rows match exactly (was 51, and 25 before that).
//   * 0 rows differ in VALUE (was 6, all of them DIV or RSQRT). Those two were
//     the last to read their operands through fpuDouble and saturate through
//     checkDivideByZero's posFmax; d71643fb75 took the clamp off them. See
//     EeFpuTopBinadeConsole for the ops that left the class earlier.
//   * 0 rows differ in FLAGS, on either engine. FCR31's O and U used to be
//     wrong on 18 of the 57 -- 15 overflow rows and 3 underflow -- because
//     checkOverflow()/checkUnderflow() inferred them from a host infinity and
//     a host denormal, neither of which the EE's own FP environment (chop, FZ)
//     ever produces. They are magnitude questions now: eeToDouble() and
//     kEeFpuMax in pcsx2/FPU.cpp.
//
// Where the tiers sit over these 57 rows. What the recompiler's column varies
// with is the eeClampMode the row runs in; the interpreter's does not vary --
// it is 57/57 in every one of them, so nothing here is an interpreter gap:
//   * eeClampMode 0, 1: 20 of 57 exact
//   * eeClampMode 2:    22 -- the operand clamp heals two of the splits
//   * eeClampMode 3, 4: 57/57 on value and flags, asserted by
//     FullModeMatchesConsoleOnEveryRow. fpuFullMode is the reference.
//   * fast path:   +/-FLT_MAX wherever the console is in the top binade, and it
//     clears O and U but raises neither. A raise needs the magnitude of the
//     exact result, and a saturating single has thrown that away by the time
//     the emitter could look; buying it back means the double arithmetic
//     fpuFullMode already pays for.
//     EnginesAgreeExceptOnTheDocumentedRows pins the shape of the remaining
//     gap: the fast path's FCR31 must equal the interpreter's with exactly the
//     O|U|SO|SU raise bits removed, and its value may differ only by being
//     sign|FLT_MAX where the interpreter is in the top binade.

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
constexpr u32 kFlagU = 0x00004000u;
constexpr u32 kFlagSO = 0x00000010u;
constexpr u32 kFlagSU = 0x00000008u;
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

Observed RunCase(const FpuOvfCase& c, bool jit, bool extra_overflow = false,
	bool full_mode = false)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (extra_overflow)
		h.EnableFpuExtraOverflow();
	if (full_mode)
		h.EnableFpuFullMode();
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
	// CLASS 1 -- the operand-clamp mode axis, not a defect. The fast path clamps
	// its sources only under CHECK_FPU_EXTRA_OVERFLOW (GameDB eeClampMode >= 2).
	// Every row here feeds the op a raw exponent-255 word, so the two engines
	// are not being asked the same question until the clamp is on. Same axis as
	// the "NAN math" row in ee_fpu_fcr_console_conformance_tests.cpp.
	//
	// This class used to cover every op, because the interpreter clamped its
	// sources unconditionally and turning the fast path's clamp on made the two
	// agree. Now the clamp heals a row only where the interpreter still goes
	// through fpuDouble -- DIV and RSQRT. Row 14 just below is the one that
	// moved.
	{12, true, "div +EEMAX, +EEMAX -- interp gets 1.0, JIT divides Inf by Inf"},
	{17, true, "sub 2^128, 2^128 -- both reach 0, from different operands"},

	// CLASS 1b -- was CLASS 1 until 2026-07-31. mul 2^128 by 0.5 is 2^127,
	// which the EE and the host both hold exactly and the interpreter now
	// returns. Turning the fast path's operand clamp on leaves it computing
	// FLT_MAX*0.5, so the clamp opens this gap rather than closing it.
	{14, false, "mul 2^128, 0.5 -- interp is console-exact at 2^127; the fast "
				"path's operand clamp cannot reach it and makes it worse"},

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

	// Rows 44 and 45, sqrt of an exponent-255 Ft, were the class 2 entries.
	// Neither engine clamps that operand any more; both scale and match the
	// console, and SqrtMatchesConsoleOnEveryCapturedOperand pins them below.
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
	// The one FCR31 difference the fast path is allowed, from the tier note at
	// the top of this file. Any other difference, in either direction, is a
	// defect.
	constexpr u32 kRaiseBits = kFlagO | kFlagU | kFlagSO | kFlagSU;
	int raise_rows = 0, saturation_rows = 0;

	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		SCOPED_TRACE(::testing::Message() << "row " << i << ": " << c.what);
		const Observed in = RunCase(c, false);
		const Observed ji = RunCase(c, true);
		const EngineDivergence* d = FindDivergence(i);

		if (d != nullptr)
		{
			EXPECT_FALSE(Agree(in, ji))
				<< "row " << i << " is listed as an engine divergence (" << d->why
				<< ") but the engines now agree -- delete the entry";
			continue;
		}

		// The one value difference the fast path is allowed, and only in this
		// exact shape: the interpreter's answer is in the EE's top binade and
		// the fast path's is sign|FLT_MAX, because it folds the whole binade
		// to FLT_MAX computing in host singles.
		//
		// Written as a property of the two values rather than as a row list, so
		// any other disagreement -- a one-ULP difference, a sign difference, a
		// fast path that lands somewhere else in the binade -- still fails.
		const bool top_binade_tier_gap =
			(in.result & 0x7F800000u) == 0x7F800000u &&
			(ji.result & 0x7FFFFFFFu) == 0x7F7FFFFFu &&
			(in.result & 0x80000000u) == (ji.result & 0x80000000u);
		if (top_binade_tier_gap)
			++saturation_rows;
		else
			EXPECT_EQ(in.result, ji.result) << "result diverges between engines";
		if ((in.fcr31 & kRaiseBits) != 0)
		{
			++raise_rows;
			EXPECT_EQ(ji.fcr31, in.fcr31 & ~kRaiseBits)
				<< "the fast path's FCR31 must be the interpreter's minus the "
				   "O|U|SO|SU raise bits. If it now MATCHES the interpreter the "
				   "fast path learned to raise: delete this branch and assert "
				   "plain equality on every row.";
		}
		else
		{
			EXPECT_EQ(in.fcr31, ji.fcr31) << "FCR31 diverges between engines";
		}
	}

	EXPECT_GT(raise_rows, 0)
		<< "anti-vacuity: no row in the capture raises O or U on the "
		   "interpreter any more, so the branch above is never taken and this "
		   "test cannot see the fast path's gap at all";
	EXPECT_GT(saturation_rows, 0)
		<< "anti-vacuity for the saturation allowance. If this went to zero "
		   "because the FAST PATH learned to saturate at the EE maximum, delete "
		   "the allowance rather than relaxing this.";
}

// ---------------------------------------------------------------------------
// ENABLED. Every listed row must close when the operand clamp is on. The
// else-branch covers a future entry that does not close: that would be a
// defect rather than the mode axis.
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
// ENABLED. The compromise, pinned on the fast path, the only tier that still
// makes it. On every row the console overflowed, the fast path must produce
// sign|0x7F7FFFFF, the non-console value, because it saturates in host singles
// and a host single stops a binade below the EE's maximum.
//
// The interpreter used to be pinned here too and no longer is: it computes the
// exact result and saturates at sign|0x7FFFFFFF, the console's answer. What is
// left keeps an attempt at the console tripwire below from quietly changing
// what the default clamp mode produces for games.
//
// If you are here because the JIT leg failed: you changed the fast path's
// saturation. That is game-visible, and the place to make it is the FULL path,
// which is already console-exact (FullModeMatchesConsoleOnEveryRow).
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, DefaultClampModeSaturatesToFltMaxOnTheFastPath)
{
	ASSERT_FALSE(EmuConfig.Cpu.Recompiler.fpuFullMode)
		<< "this test describes the NON-full path; something enabled FULL mode";

	int checked = 0, interp_exact = 0;
	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		if ((c.fcr31 & kFlagO) == 0)
			continue; // console did not call this row an overflow
		if (FindDivergence(i) != nullptr)
			continue; // covered by the divergence list instead
		++checked;
		SCOPED_TRACE(::testing::Message() << "row " << i << ": " << c.what);
		EXPECT_EQ(RunCase(c, true).result, (c.result & 0x80000000u) | kFastPathMax)
			<< "jit -- the fast path's saturation must not move";
		if (RunCase(c, false).result == c.result)
			++interp_exact;
	}
	EXPECT_GT(checked, 10) << "the console overflow rows vanished from the "
							  "capture; this test would pass vacuously";
	// The interpreter's half, stated as a count rather than row by row so that
	// the DIV/RSQRT rows it has not reached yet do not have to be listed here.
	EXPECT_EQ(interp_exact, checked)
		<< "the interpreter reached the console on " << interp_exact << " of "
		<< checked << " overflow rows; it was 0 before the operand clamp came "
		   "out and it must not go back down";
}

// ---------------------------------------------------------------------------
// ENABLED. Regression test: the interpreter's FCR31 against the console on
// every row, in the production FP environment (no ScopedFpEnv -- chop, DAZ, FZ,
// which is what a game gets). 18 of these 57 rows used to read back with O or U
// clear where the console raised them, which is what
// DISABLED_ExceptionFlagsInProductionFpEnvMissOverflow said before it was
// deleted.
//
// The value is not asserted here: 32 rows still come back +/-FLT_MAX where the
// console returns its own top binade, which is the saturation compromise
// DefaultClampModeSaturatesToFltMaxOnBothEngines pins. Keeping this test to the
// flag means a later attempt at the value cannot take the flag with it.
//
// The row counts below are the anti-vacuity clause: this capture was built
// around overflow and underflow, and if either class empties out the test
// passes without meaning anything.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, InterpreterRaisesOverflowAndUnderflowLikeTheConsole)
{
	int overflow_rows = 0, underflow_rows = 0, quiet_rows = 0;
	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		SCOPED_TRACE(::testing::Message() << "row " << i << ": " << c.what);
		EXPECT_EQ(RunCase(c, false).fcr31, c.fcr31) << "[interp] FCR31";

		if (c.fcr31 & kFlagO)
			++overflow_rows;
		else if (c.fcr31 & kFlagU)
			++underflow_rows;
		else
			++quiet_rows;
	}
	EXPECT_GE(overflow_rows, 10) << "anti-vacuity: the console overflow rows "
									"are gone from the capture";
	EXPECT_GE(underflow_rows, 3) << "anti-vacuity: the console underflow rows "
									"are gone from the capture";
	EXPECT_GE(quiet_rows, 20)
		<< "anti-vacuity: without rows the console leaves quiet, an "
		   "implementation that raised O on everything would pass this";
}

// ---------------------------------------------------------------------------
// ENABLED. The FULL path against the console on every row, value and flags.
// The tier note at the top of this file rests on it: fpuFullMode carries every
// intermediate as a PS2-widened double (ToPS2FPU_Full, iFPUd-arm64.cpp), so it
// holds the EE's top binade and still has the magnitude of the exact result to
// hand when the flag decision is made. If this test fails the reference column
// moved and that note is stale.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, FullModeMatchesConsoleOnEveryRow)
{
	int exact_top_binade = 0, raised = 0;
	for (int i = 0; i < kCaseCount; ++i)
	{
		const FpuOvfCase& c = kCases[i];
		SCOPED_TRACE(::testing::Message() << "row " << i << ": " << c.what);
		const Observed o = RunCase(c, true, /*extra_overflow=*/false,
			/*full_mode=*/true);
		EXPECT_EQ(o.result, c.result) << "[jit, fpuFullMode] result";
		EXPECT_EQ(o.fcr31, c.fcr31) << "[jit, fpuFullMode] FCR31";

		// A result in the EE's top binade is one the fast path cannot return,
		// so these rows are what separates the tiers.
		if ((c.result & 0x7F800000u) == 0x7F800000u &&
			(c.result & 0x007FFFFFu) != 0)
			++exact_top_binade;
		if (c.fcr31 & (kFlagO | kFlagU))
			++raised;
	}
	EXPECT_GE(exact_top_binade, 10)
		<< "anti-vacuity: no row returns a value outside host single range any "
		   "more, so this no longer distinguishes FULL from the fast path";
	EXPECT_GE(raised, 10) << "anti-vacuity: no row raises O or U";
}

// ---------------------------------------------------------------------------
// Both engines against the console on every SQRT row in the capture.
//
// The capture surfaced this as a clamp defect: recSQRT_S_xmm was the one
// emitter in iFPU-arm64.cpp that never clamped its operand, so an exponent-255
// Ft reached Fsqrt as a host +Inf and fpuClampResult flattened the result to
// 0x7F7FFFFF, while the interpreter's sqrt(fpuDouble(Ft)) gave 0x5F7FFFFF.
// Clamping SQRT too made both engines say 0x5F7FFFFF, which is not the console
// value either. They scale instead now -- see SQRT_S (pcsx2/FPU.cpp) and
// recSQRT_S_xmm (pcsx2/arm64/iFPU-arm64.cpp).
//
// Rows 44 and 45 failed until then (console 5fb504f3 / 5f800000, both engines
// 5f7fffff); row 46's Ft has exponent field 254, never reached the clamp, and
// passed throughout. Both clamp modes are checked because the clamp this
// replaced was gated on CHECK_FPU_OVERFLOW.
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

		for (int extra = 0; extra < 2; ++extra)
		{
			SCOPED_TRACE(::testing::Message()
						 << (extra ? "eeClampMode >= 2" : "default clamp mode"));
			const Observed in = RunCase(c, false, extra != 0);
			const Observed ji = RunCase(c, true, extra != 0);
			EXPECT_TRUE(Agree(in, ji))
				<< "interp " << std::hex << in.result << "/" << in.fcr31
				<< " vs jit " << ji.result << "/" << ji.fcr31;
			EXPECT_EQ(in.result, c.result) << "interp result vs console";
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
// The same property as above, over the whole exponent-255 class rather than the
// three patterns the capture happens to contain.
//
// The class splits on an axis the capture cannot see: as host bit patterns,
// exponent-255 words are infinities, quiet NaNs and signalling NaNs, while to
// the EE they are all large finite floats. The old arm64 clamp had to be an
// integer Umin rather than an Fminnm because of that split -- Fminnm prefers
// the number only against a quiet NaN, and a signalling operand comes back
// merely quieted, so half the mantissa space (4194303 of the 8388608 positive
// patterns) would have passed through a clamp that was supposed to catch it.
// Testing the exponent field, as both engines now do, never asks the host what
// kind of NaN it thinks it is holding; the pool below covers every shape either
// way.
//
// Expected values are correctly-rounded square roots computed by exact integer
// arithmetic (math.isqrt on the significand, round-to-nearest-even) rather than
// by a host float, so they cannot inherit the behaviour under test. The divide
// unit does not round in general -- see eeSrtDigit in FPU.cpp -- but correct
// rounding is what it returns on these operands: the model was checked against
// the six the capture does witness, marked `true` below, and agreed on all six,
// including the exponent-254 control. The other three rows are computed, not
// read off silicon, and are here for class coverage.
// ---------------------------------------------------------------------------
TEST(EeFpuOverflowConsole, SqrtMatchesConsoleOnEveryExponent255Operand)
{
	struct Operand
	{
		u32 ft;
		u32 want;      // correctly-rounded sqrt(|ft|) as an EE single
		bool witnessed; // true == this exact value was read off silicon
		const char* what;
	};
	// Every exponent-255 shape, both signs, plus one exponent-254 control.
	static constexpr Operand kOperands[] = {
		{0x7F800000u, 0x5F800000u, true,  "+2^128        (host +Inf)"},
		{0xFF800000u, 0x5F800000u, true,  "-2^128        (host -Inf)"},
		{0x7F800001u, 0x5F800000u, false, "exp255 mant 1 (host +sNaN, smallest)"},
		{0xFF800001u, 0x5F800000u, false, "exp255 mant 1 (host -sNaN, smallest)"},
		{0x7FBFFFFFu, 0x5F9CC470u, false, "exp255 mant 0x3FFFFF (host +sNaN, largest)"},
		{0x7FC00000u, 0x5F9CC471u, true,  "exp255 mant 0x400000 (host +qNaN, smallest)"},
		{0x7FFFFFFFu, 0x5FB504F3u, true,  "+EEMAX        (host +qNaN, largest)"},
		{0xFFFFFFFFu, 0x5FB504F3u, true,  "-EEMAX        (host -qNaN, largest)"},
		// Control: exponent field 254, below the scaling branch.
		{0xFF7FFFFFu, 0x5F7FFFFFu, true,  "-FLT_MAX      (exp 254 -- CONTROL)"},
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
			EXPECT_EQ(in.result, o.want) << "[interp] result";
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
// against the console. Expected to fail on 35 of 57 rows today for the three
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
		const Observed jf = RunCase(c, true, false, /*full_mode=*/true);
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
			   "jit+xovf %08x/%08x  jit+full %08x/%08x %s%s%s%s\n",
			i, c.what, c.result, c.fcr31, in.result, in.fcr31, ji.result, ji.fcr31,
			jx.result, jx.fcr31, jf.result, jf.fcr31,
			vbad ? "VAL " : "", fbad ? "FLAG " : "",
			split ? "ENGINE-SPLIT " : "", (split && !split_x) ? "(healed by xovf)" : "");
	}
	printf("\n%d rows: %d match console, %d value-only, %d flag-only, %d both\n",
		kCaseCount, agree, val_only, flag_only, both);
	printf("%d engine-split in default mode, %d of them healed by "
		   "CHECK_FPU_EXTRA_OVERFLOW\n", engine_split, split_healed);
}
