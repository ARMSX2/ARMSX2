// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE FPU control registers (FCR0 / FCR31) against real PS2 hardware.
//
// Built from the console capture in unknownbrackets/ps2autotests,
// tests/cpu/ee_fpu/fcr.expected. That file is free-form text rather than a
// table, so the values it prints are transcribed here directly.
//
// Three things come out of it:
//
// 1. `cfc1 rt, $N` for N = 0..15 all return FCR0 and N = 16..31 all return
//    FCR31 — the EE COP1 decodes bit 4 of the register field and nothing else.
//
// 2. FCR31 writes follow a single model:
//        readback = (written & 0x0083C078) | 0x01000001
//    Only the four sticky flags (bits 3-6), the four cause bits (14-17) and
//    C (23) are implemented; RM, the enables, FS/FO/FN, FCC1-7 and bits 18-20
//    are not; bits 0 and 24 read as one always.
//
// 3. Which flags exceptional arithmetic actually raises.
//
// The two engines legitimately disagree here — recCFC1
// (pcsx2/arm64/iFPU-arm64.cpp) applies the mask model above and honours the
// >= 16 aliasing, while the shared interpreter CFC1 (pcsx2/FPU.cpp) returns
// the raw word, hardcodes 0x2E00 for FCR0, and returns zero for every other
// index. So each engine is scored on its own rather than through Run()'s diff.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"

#include <optional>
#include <string>
#include <vector>

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee;

constexpr u32 kRt = 8;    // ctc1 source (single-read tests)
constexpr u32 kRd = 9;    // cfc1 destination
constexpr u32 kSrc = 24;  // ctc1 source in the 16-read aliasing sweep
constexpr u32 kFd = 4, kFs = 5, kFt = 6;

// "fcr0: 00002e30" on every line of the capture, before and after a write of
// 0xDEADBEEF. Also what pcsx2/R5900.cpp seeds fpuRegs.fprc[0] with.
constexpr u32 kFcr0 = 0x00002E30;

constexpr u32 kFcr31Writable = 0x0083C078;
constexpr u32 kFcr31FixedOnes = 0x01000001;

// One (ctc1 value -> cfc1 read-back) pair per isolated FCR31 field, in capture
// order. The trailing string is the capture's own name for the field.
struct Fcr31Write { u32 written, readback; const char* what; };
constexpr Fcr31Write kFcr31Writes[] = {
	{0x00000003, 0x01000001, "rounding mode (RM)"},
	{0x0000007C, 0x01000079, "flags"},
	{0x00000F80, 0x01000001, "enables"},
	{0x0001F000, 0x0101C001, "cause"},
	{0x00020000, 0x01020001, "unimplemented (E)"},
	{0x01000000, 0x01000001, "flushing (FS)"},
	{0x00400000, 0x01000001, "flushing (FO)"},
	{0x00200000, 0x01000001, "flushing (FN)"},
	{0x00800000, 0x01800001, "FCC"},
	{0xFE000000, 0x01000001, "FCC1-7"},
	{0x001C0000, 0x01000001, "unknown (bits 18-20)"},
};
constexpr int kFcr31WriteCount =
	static_cast<int>(sizeof(kFcr31Writes) / sizeof(kFcr31Writes[0]));

// Runs `prog` on one engine from a given FCR31 pre-state and returns kRd.
u32 RunAndReadGpr(const std::vector<u32>& prog, u32 fcr31_pre, bool jit,
                  u32 gpr = kRd)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(fcr31_pre);
	h.SetGpr128(gpr, 0, 0);
	h.LoadProgram(prog);
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	return jit ? h.GetGprJit(gpr) : h.GetGprInterp(gpr);
}

// No recorded divergences: both engines reproduce the console FCR model. The
// shared interpreter's CFC1 used to hardcode 0x2E00 rather than reading
// fprc[0], return 0 for every index that is not 0 or 31, and hand back the raw
// FCR31 word; it now applies the same alias-on-bit-4 and mask model the
// recompilers do (iFPU.cpp recCFC1, iFPU-arm64.cpp recCFC1).
} // namespace

// A ctc1 of 0xDEADBEEF into $0 leaves fcr0 at 00002e30.
TEST(EeFpuFcrConsoleConformance, Fcr0IsReadOnly)
{
	for (int jit = 0; jit < 2; ++jit)
	{
		const std::vector<u32> prog = {
			LUI(kRt, 0xDEAD),
			ORI(kRt, kRt, 0xBEEF),
			CTC1(kRt, 0),
			CFC1(kRd, 0),
		};
		const bool ok = RunAndReadGpr(prog, kFcr31FixedOnes, jit != 0) == kFcr0;
		SCOPED_TRACE(jit ? "[jit]" : "[interp]");
		EXPECT_TRUE(ok) << "new divergence from silicon";
	}
}

// All 32 control-register indices: 0-15 hold FCR0 and 16-31 hold FCR31. The
// capture snapshot reproduced here is the one taken after the `flags` write,
// because it is reachable from a known write rather than from BIOS state.
TEST(EeFpuFcrConsoleConformance, ControlRegisterIndicesAliasOnBit4)
{
	constexpr u32 kWritten = 0x0000007C;
	constexpr u32 kExpectHigh = 0x01000079;

	for (int jit = 0; jit < 2; ++jit)
	{
		bool ok = true;
		for (u32 base = 0; base < 32; base += 16)
		{
			// 16 reads per program, into $t0-$t7 ($8-$15) and $s0-$s7
			// ($16-$23). $24 ($t8) carries the ctc1 source. Nothing above
			// $24 is usable: the harness parks through $ra and reserves the
			// k/gp/sp/fp block.
			std::vector<u32> prog = {
				LUI(kSrc, kWritten >> 16),
				ORI(kSrc, kSrc, kWritten & 0xFFFF),
				CTC1(kSrc, 31),
			};
			for (u32 i = 0; i < 16; ++i)
				prog.push_back(CFC1(8 + i, base + i));

			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFcr31(kFcr31FixedOnes);
			for (u32 i = 0; i < 16; ++i)
				h.SetGpr128(8 + i, 0, 0);
			h.LoadProgram(prog);
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();

			const u32 expect = (base == 0) ? kFcr0 : kExpectHigh;
			for (u32 i = 0; i < 16; ++i)
			{
				const u32 got = jit ? h.GetGprJit(8 + i) : h.GetGprInterp(8 + i);
				if (got != expect)
					ok = false;
			}
		}
		SCOPED_TRACE(jit ? "[jit]" : "[interp]");
		EXPECT_TRUE(ok) << "new divergence from silicon: the 32 control "
		                   "register indices must alias onto FCR0/FCR31";
	}
}

// Eleven ctc1/cfc1 pairs, one per isolated FCR31 field.
TEST(EeFpuFcrConsoleConformance, Fcr31WriteMaskMatchesConsole)
{
	int checked = 0;
	for (int i = 0; i < kFcr31WriteCount; ++i)
	{
		const Fcr31Write& w = kFcr31Writes[i];
		// The captured numbers must all be consistent with the single model
		// they imply; if a line is added later that breaks it, say so here
		// rather than silently widening the mask.
		EXPECT_EQ((w.written & kFcr31Writable) | kFcr31FixedOnes, w.readback)
			<< "capture line `" << w.what << "` does not fit the derived "
			   "FCR31 model";

		const std::vector<u32> prog = {
			LUI(kRt, w.written >> 16),
			ORI(kRt, kRt, w.written & 0xFFFF),
			CTC1(kRt, 31),
			CFC1(kRd, 31),
		};
		for (int jit = 0; jit < 2; ++jit)
		{
			const bool ok =
				RunAndReadGpr(prog, kFcr31FixedOnes, jit != 0) == w.readback;
			SCOPED_TRACE(::testing::Message()
			             << "Update " << w.what
			             << (jit ? " [jit]" : " [interp]"));
			EXPECT_TRUE(ok) << "new divergence from silicon";
		}
		++checked;
	}
	EXPECT_EQ(checked, kFcr31WriteCount);
}

// Which flags the arithmetic raises. Each capture line clears FCR31, runs one
// exceptional operation and prints FCR31. Reproduced with FCR31 preset to
// 0x01000001 (what a `ctc1 $0` write leaves on silicon) rather than to zero,
// so the always-one bits are present on both engines and what is under test is
// purely which flag and cause bits the operation sets.
//
// Two of the seven print a result unambiguous enough to assert as well. The
// rest print through the test program's float formatter, which renders
// anything at exponent 255 as "NaN" — not enough to pin bits, so only FCR31 is
// checked for those.
namespace
{
enum FlagOp { FO_SQRT, FO_DIV, FO_ADD, FO_MUL };
struct FlagSituation
{
	const char* what;
	int op;
	u32 fs, ft;
	u32 fcr31;
	bool check_fd;
	u32 fd;
};
constexpr FlagSituation kFlagSituations[] = {
	// sqrt(-1) is 1.0 on the PS2, not NaN: SQRT takes the magnitude. Both the
	// invalid sticky flag (bit 6) and its cause bit (17) come up.
	{"sqrt(-1)", FO_SQRT, 0xBF800000, 0xBF800000, 0x01020041, true, 0x3F800000},
	{"Divide zero by zero", FO_DIV, 0x00000000, 0x00000000, 0x01020041, false, 0},
	{"Divide one by zero", FO_DIV, 0x3F800000, 0x00000000, 0x01010021, false, 0},
	{"NAN math", FO_ADD, 0x7F800001, 0x7F800001, 0x01008011, false, 0},
	{"Overflow", FO_MUL, 0x7F7FFFFF, 0x7F7FFFFF, 0x01008011, false, 0},
	// FLT_MIN/3 is a denormal, which the PS2 flushes to zero — and raises
	// nothing doing it.
	{"Underflow", FO_DIV, 0x00800000, 0x40400000, 0x01000001, true, 0x00000000},
	// 1 / 3.0155 — an ordinary inexact result. The EE has no inexact flag,
	// which is the same fact bit 2 being unwritable shows above.
	{"Inexact", FO_DIV, 0x3F800000, 0x4040FFFF, 0x01000001, false, 0},
};
constexpr int kFlagSituationCount =
	static_cast<int>(sizeof(kFlagSituations) / sizeof(kFlagSituations[0]));

u32 FlagOpWord(const FlagSituation& s)
{
	switch (s.op)
	{
		case FO_SQRT: return SQRT_S(kFd, kFt);
		case FO_DIV: return DIV_S(kFd, kFs, kFt);
		case FO_ADD: return ADD_S(kFd, kFs, kFt);
		case FO_MUL: return MUL_S(kFd, kFs, kFt);
		default: return 0;
	}
}
} // namespace

// The seven console rows, both engines, in both FP environments.
//
// This used to be four DISABLED tests and a divergence list, all premised on
// PCSX2 reproducing these flags only at round-to-nearest. The interpreter
// decides from the magnitude of the exact result now (eeToDouble/kEeFpuMax in
// FPU.cpp), so it matches the console on all seven rows in either environment,
// which is what the two legs below assert.
//
// That settles "NAN math" too, the last listed engine divergence, which was
// attributed to the operand-clamp axis: ADD.S on two raw exp-255 words, which
// the interpreter clamped through fpuDouble into an Inf sum while the fast
// path got a host NaN. eeToDouble does not clamp -- 0x7F800001 is
// (1 + 2^-23) * 2^128 -- so the interpreter reaches O by magnitude, the way it
// does on every other overflow row.
//
// The fast path's remaining gap is described in
// ee_fpu_overflow_console_conformance_tests.cpp.
TEST(EeFpuFcrConsoleConformance, ExceptionFlagsMatchConsoleExceptTheFastPathRaise)
{
	constexpr u32 kRaiseBits = 0x00008000u | 0x00004000u | 0x00000010u | 0x00000008u;

	// leg 0 = production FP environment (ChopZero + DAZ/FZ, what a game gets),
	// leg 1 = FlushNearest, the round-to-nearest environment the old model
	// needed. The flag decision must not depend on which one is in force.
	for (int leg = 0; leg < 2; ++leg)
	{
		std::optional<ScopedFpEnv> fp_env;
		if (leg == 1)
			fp_env.emplace(ScopedFpEnv::FlushNearest);

		int raise_rows = 0;
		for (int i = 0; i < kFlagSituationCount; ++i)
		{
			const FlagSituation& s = kFlagSituations[i];
			const u32 word = FlagOpWord(s);
			ASSERT_NE(word, 0u) << s.what;

			u32 got[2], fd[2];
			for (int jit = 0; jit < 2; ++jit)
			{
				EeRecTestHarness h;
				h.EnableCop1();
				h.SetFcr31(kFcr31FixedOnes);
				h.SetFprBits(kFd, 0x00001337);
				h.SetFprBits(kFs, s.fs);
				h.SetFprBits(kFt, s.ft);
				h.SetGpr128(kRd, 0, 0);
				h.LoadProgram({word, CFC1(kRd, 31)});
				if (jit)
					h.RunJitNoDiff();
				else
					h.RunInterpOnly();
				got[jit] = jit ? h.GetGprJit(kRd) : h.GetGprInterp(kRd);
				fd[jit] = jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd);
			}

			SCOPED_TRACE(::testing::Message()
			             << s.what
			             << (leg ? " (FlushNearest)" : " (production FP env)"));
			EXPECT_EQ(got[0], s.fcr31) << "[interp] vs console";
			if ((s.fcr31 & kRaiseBits) != 0)
			{
				++raise_rows;
				EXPECT_EQ(got[1], s.fcr31 & ~kRaiseBits)
					<< "[jit] must be the console word minus O|U|SO|SU. If it "
					   "now MATCHES the console the fast path learned to raise: "
					   "assert plain equality on every row and delete this "
					   "branch.";
			}
			else
			{
				EXPECT_EQ(got[1], s.fcr31) << "[jit] vs console";
			}
			if (s.check_fd)
			{
				EXPECT_EQ(fd[0], s.fd) << "[interp] result";
				EXPECT_EQ(fd[1], s.fd) << "[jit] result";
			}
		}
		EXPECT_GE(raise_rows, 2)
			<< "anti-vacuity: no row in this table raises O or U any more, so "
			   "the allowance branch is never taken";
	}
}

// The control for the "NAN math" paragraph above. Three legs: interpreter,
// fast path, and fast path with CHECK_FPU_EXTRA_OVERFLOW (GameDB eeClampMode
// >= 2) so it clamps its operands to +/-fMax the way fpuDouble used to. The
// clamped leg must still come back without O; if it ever closes the gap, the
// divergence really was the clamp axis.
//
// The value is identical in all three legs, which is why this stayed invisible
// until FCR31 was read back.
TEST(EeFpuFcrConsoleConformance, NanMathOverflowIsNotAnOperandClampModeDifference)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest};
	constexpr u32 kRawNan = 0x7F800001;
	constexpr u32 kWord = ADD_S(kFd, kFs, kFt);
	constexpr u32 kConsole = 0x01008011;

	u32 fcr[3], res[3];
	for (int leg = 0; leg < 3; ++leg)  // 0 = interp, 1 = JIT default, 2 = JIT clamped
	{
		EeRecTestHarness h;
		h.EnableCop1();
		if (leg == 2)
			h.EnableFpuExtraOverflow();
		h.SetFcr31(kFcr31FixedOnes);
		h.SetFprBits(kFd, 0x00001337);
		h.SetFprBits(kFs, kRawNan);
		h.SetFprBits(kFt, kRawNan);
		h.SetGpr128(kRd, 0, 0);
		h.LoadProgram({kWord, CFC1(kRd, 31)});
		if (leg == 0)
			h.RunInterpOnly();
		else
			h.RunJitNoDiff();
		fcr[leg] = (leg == 0) ? h.GetGprInterp(kRd) : h.GetGprJit(kRd);
		res[leg] = (leg == 0) ? h.GetFprBitsInterp(kFd) : h.GetFprBitsJit(kFd);
	}

	EXPECT_EQ(fcr[0], kConsole) << "[interp] is the console-matching side, and "
	                               "reaches O by magnitude, not by clamping the "
	                               "operands into a host infinity";
	EXPECT_EQ(fcr[1], kFcr31FixedOnes)
		<< "[jit, default clamp] the fast path raises nothing";
	EXPECT_EQ(fcr[2], kFcr31FixedOnes)
		<< "[jit, CHECK_FPU_EXTRA_OVERFLOW] the operand clamp must NOT close "
		   "this row -- if it does, the missing piece is the clamp after all "
		   "and the attribution above is wrong";
	// The value was identical in all three legs when this was written, which is
	// why the FCR31 defect above stayed invisible. It no longer is: both
	// operands are exponent-255 and their sum is past the EE maximum, so the
	// interpreter saturates there and the fast path a binade below. That is the
	// tier gap, not the clamp-mode axis this test is about.
	EXPECT_EQ(res[0], 0x7FFFFFFFu)
		<< "[interp] saturates at the EE maximum";
	EXPECT_EQ(res[1], 0x7F7FFFFFu)
		<< "[jit, default clamp] saturates at FLT_MAX";
	EXPECT_EQ(res[2], res[1])
		<< "the operand clamp mode must not move the fast path's value either";
}

// ---------------------------------------------------------------------------
// The O/U class, engine against engine.
//
// The two console rows above are one window into a whole family: FCR31's
// overflow and underflow maintenance, which pcsx2/FPU.cpp performs on EVERY
// arithmetic op and the recompilers performed on none. Three distinct
// behaviours live in the interpreter and all three are testable without a
// capture, because the interpreter is the reference side here:
//
//   1. the ten ops that call raiseOrClearOU() -- ADD/SUB/MUL, the A-forms
//      ADDA/SUBA/MULA, and the multiply-accumulates MADD/MSUB/MADDA/MSUBA.
//      Each clears both causes and then raises whichever the magnitude of the
//      exact result calls for, so an overflow brings a preset U down. The four
//      multiply-accumulates do it twice, once per rounding step.
//   2. ABS/NEG/MAX/MIN, which clearFPUFlags(O|U) and nothing else.
//   3. DIV/SQRT/RSQRT, which touch I and D but must leave O and U alone.
//      These are the negative controls, and they are live ones: rows 1 and 2
//      in the same table prove the probe can see an FCR31 change at all, so
//      "unchanged" here means preserved rather than unobserved.
//
// The clear in (1) and (2) is observable on its own -- preset O and U through
// ctc1 (both are in the writable mask) and run a non-overflowing op. That is
// why the pre-state below is 0x0100C001 rather than the bare fixed-ones word:
// it makes set, clear and preserve three distinguishable outcomes instead of
// two. The fast path now performs that clear on all fourteen emitters.
//
// Why the table below no longer says what it did: it used to call all ten
// +/-FLT_MAX rows overflows, because the old detection asked the host whether
// the single had reached Inf and 2 * FLT_MAX does. It is asked of the exact
// result in double now, so those rows moved to the clear class, where silicon
// has always had them.
//
// The underflow half of (1) is exercised now, and does not need FZ off: FZ
// destroys the host's denormal, but `exact` is computed in double where the
// operands were only flushed, not the result. What still needs FZ off is the
// denormal-result question (the engines disagree on the value there) --
// a separate work item, pinned by DISABLED_UnderflowFlagsNeedFzOff below.
namespace
{
enum FamOp
{
	FA_ADD, FA_SUB, FA_MUL,
	FA_ADDA, FA_SUBA, FA_MULA,
	FA_MADD, FA_MSUB, FA_MADDA, FA_MSUBA,
	FA_ABS, FA_NEG, FA_MAX, FA_MIN,
	FA_DIV, FA_SQRT, FA_RSQRT,
};

constexpr u32 kFMax = 0x7F7FFFFF, kNegFMax = 0xFF7FFFFF;
constexpr u32 kOne = 0x3F800000, kNegOne = 0xBF800000;
constexpr u32 kTwo = 0x40000000, kFour = 0x40800000;
constexpr u32 kMinNormal = 0x00800000;

constexpr u32 kFlagO = 0x00008000, kFlagU = 0x00004000;
constexpr u32 kFlagSO = 0x00000010, kFlagSU = 0x00000008;

// Pre-state: the always-one bits plus O and U already raised, so a row that
// clears them is distinguishable from a row that leaves them alone.
constexpr u32 kOuPreset = kFcr31FixedOnes | kFlagO | kFlagU;

struct FamCase
{
	const char* what;
	FamOp op;
	u32 acc, fs, ft;
	// What pcsx2/FPU.cpp produces, derived from the source and confirmed by
	// running the interpreter leg below.
	u32 want_fcr31;
	// true == this exact operand triple appears in the first-party console
	// capture (autocases_fpuovf.h) and silicon agrees on which of O/U the op
	// raises. It does not witness the clear: that capture always starts from
	// the fixed-ones word, so it cannot tell "cleared" from "never set". The
	// clear is witnessed instead by the FCR31-seeded rows of the FP matrix
	// capture, which cover ABS, NEG, ADD, ADDA, MADD, MSUB, MUL, MULA, MAX and
	// MIN -- and not SUB, SUBA, MADDA or MSUBA, which are here on the
	// interpreter's authority alone.
	bool console_agrees_on_raise;
};

// No row here overflows the intermediate product of a multiply-accumulate:
// that corner is a deliberate default-clamp-mode divergence between the
// engines (see recMADD_S_xmm in iFPU-arm64.cpp, pinned by
// EeRecFpu.MaddSProductOverflowDefaultModeMatchesX86Jit), and pulling it in
// here would mix a known value divergence into a flag measurement. fMax*1.0
// overflows the accumulate without overflowing the product.
//
// Nor does any row trip the guard-bit masking in fpuEmitGuardedAddSub -- every
// add/sub below has an operand exponent difference of 0 or 1 -- so the JIT and
// the interpreter compute the same result and only the flags are under test.
constexpr FamCase kFamCases[] = {
	// (1a) Clear: an in-range op must bring O and U back down. Fourteen rows,
	// one per emitter that owes the clear -- this is the whole of what the fast
	// path is expected to do with these two bits, so every op is listed rather
	// than a representative few.
	{"ADD.S in range",    FA_ADD,   kOne, kOne, kTwo, kFcr31FixedOnes, true},
	{"SUB.S in range",    FA_SUB,   kOne, kOne, kTwo, kFcr31FixedOnes, false},
	{"MUL.S in range",    FA_MUL,   kOne, kTwo, kTwo, kFcr31FixedOnes, true},
	{"ADDA.S in range",   FA_ADDA,  kOne, kOne, kTwo, kFcr31FixedOnes, true},
	{"SUBA.S in range",   FA_SUBA,  kOne, kFour, kOne, kFcr31FixedOnes, true},
	{"MULA.S in range",   FA_MULA,  kOne, kTwo, kTwo, kFcr31FixedOnes, true},
	{"MADD.S in range",   FA_MADD,  kOne, kTwo, kTwo, kFcr31FixedOnes, true},
	{"MSUB.S in range",   FA_MSUB,  kOne, kTwo, kTwo, kFcr31FixedOnes, true},
	{"MADDA.S in range",  FA_MADDA, kOne, kTwo, kTwo, kFcr31FixedOnes, true},
	{"MSUBA.S in range",  FA_MSUBA, kOne, kTwo, kTwo, kFcr31FixedOnes, true},
	// (2) clearFPUFlags(O|U) and nothing else -- same obligation, no arithmetic.
	{"ABS.S clears O|U",  FA_ABS,   0, kNegOne, 0,    kFcr31FixedOnes, false},
	{"NEG.S clears O|U",  FA_NEG,   0, kOne,    0,    kFcr31FixedOnes, false},
	{"MAX.S clears O|U",  FA_MAX,   0, kOne,    kTwo, kFcr31FixedOnes, false},
	{"MIN.S clears O|U",  FA_MIN,   0, kOne,    kTwo, kFcr31FixedOnes, false},

	// (1b) Clear, at the boundary. +FLT_MAX + +FLT_MAX is exactly 0x7FFFFFFF,
	// the largest EE single -- representable, so not an overflow, and silicon
	// returns it with FCR31 untouched. These four rows are here because they
	// are the ones an implementation that tests for a host infinity gets wrong,
	// and because ADD/SUB/ADDA/SUBA have no overflow row at all: no pair of
	// host-representable operands can push their result past the maximum.
	{"ADD.S lands on EEMAX",   FA_ADD,  0, kFMax, kFMax,    kFcr31FixedOnes, true},
	{"SUB.S lands on EEMAX",   FA_SUB,  0, kFMax, kNegFMax, kFcr31FixedOnes, true},
	{"ADDA.S lands on EEMAX",  FA_ADDA, 0, kFMax, kFMax,    kFcr31FixedOnes, true},
	{"SUBA.S lands on EEMAX",  FA_SUBA, 0, kFMax, kNegFMax, kFcr31FixedOnes, true},

	// (1c) Raise, overflow: both causes are cleared and then O|SO go up, so a
	// preset U comes down on an overflow, which the old early-return structure
	// did not do. Only the multiplies can get here.
	{"MUL.S overflow",    FA_MUL,   0,        kFMax, kFMax, kFcr31FixedOnes | kFlagO | kFlagSO, true},
	{"MULA.S overflow",   FA_MULA,  0,        kFMax, kFMax, kFcr31FixedOnes | kFlagO | kFlagSO, true},
	{"MADD.S overflow",   FA_MADD,  kFMax,    kFMax, kFMax, kFcr31FixedOnes | kFlagO | kFlagSO, true},
	{"MSUB.S overflow",   FA_MSUB,  kNegFMax, kFMax, kFMax, kFcr31FixedOnes | kFlagO | kFlagSO, true},
	{"MADDA.S overflow",  FA_MADDA, kFMax,    kFMax, kFMax, kFcr31FixedOnes | kFlagO | kFlagSO, true},
	{"MSUBA.S overflow",  FA_MSUBA, kNegFMax, kFMax, kFMax, kFcr31FixedOnes | kFlagO | kFlagSO, true},

	// (1d) Raise, underflow: O is cleared first, then U|SU go up. The result is
	// nonzero and below 2^-126 in exact arithmetic even though FZ has already
	// flushed the host's single to +0, which is why the decision comes from
	// `exact` rather than from the result register.
	{"MUL.S underflow",   FA_MUL,   0, kMinNormal, kMinNormal,
		kFcr31FixedOnes | kFlagU | kFlagSU, true},
	{"MULA.S underflow",  FA_MULA,  0, kMinNormal, kMinNormal,
		kFcr31FixedOnes | kFlagU | kFlagSU, false},

	// (3) Negative controls -- the divide unit must leave O and U alone, so
	// they come out exactly as they went in.
	{"DIV.S preserves",   FA_DIV,   0, kOne, kTwo,  kOuPreset, false},
	{"SQRT.S preserves",  FA_SQRT,  0, 0,    kFour, kOuPreset, false},
	{"RSQRT.S preserves", FA_RSQRT, 0, kOne, kFour, kOuPreset, false},
};
constexpr int kFamCaseCount = static_cast<int>(std::size(kFamCases));

u32 FamOpWord(const FamCase& c)
{
	switch (c.op)
	{
		case FA_ADD:   return ADD_S(kFd, kFs, kFt);
		case FA_SUB:   return SUB_S(kFd, kFs, kFt);
		case FA_MUL:   return MUL_S(kFd, kFs, kFt);
		case FA_ADDA:  return ADDA_S(kFs, kFt);
		case FA_SUBA:  return SUBA_S(kFs, kFt);
		case FA_MULA:  return MULA_S(kFs, kFt);
		case FA_MADD:  return MADD_S(kFd, kFs, kFt);
		case FA_MSUB:  return MSUB_S(kFd, kFs, kFt);
		case FA_MADDA: return MADDA_S(kFs, kFt);
		case FA_MSUBA: return MSUBA_S(kFs, kFt);
		case FA_ABS:   return ABS_S(kFd, kFs);
		case FA_NEG:   return NEG_S(kFd, kFs);
		case FA_MAX:   return MAX_S(kFd, kFs, kFt);
		case FA_MIN:   return MIN_S(kFd, kFs, kFt);
		case FA_DIV:   return DIV_S(kFd, kFs, kFt);
		case FA_SQRT:  return SQRT_S(kFd, kFt);
		case FA_RSQRT: return RSQRT_S(kFd, kFs, kFt);
		default:       return 0;
	}
}

// Runs one row on one engine from the O|U preset and returns the FCR31 word a
// following cfc1 reads back, with the op's own result in `result` (fd for the
// d-form ops, ACC for the a-forms).
u32 RunFamCase(const FamCase& c, bool jit, u32* result)
{
	const bool writes_acc = (c.op == FA_ADDA || c.op == FA_SUBA || c.op == FA_MULA ||
	                         c.op == FA_MADDA || c.op == FA_MSUBA);
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(kOuPreset);
	h.SetAccBits(c.acc);
	h.SetFprBits(kFd, 0x00001337);
	h.SetFprBits(kFs, c.fs);
	h.SetFprBits(kFt, c.ft);
	h.SetGpr128(kRd, 0, 0);
	h.LoadProgram({FamOpWord(c), CFC1(kRd, 31)});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();

	if (writes_acc)
		*result = jit ? h.GetAccBitsJit() : h.GetAccBitsInterp();
	else
		*result = jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd);
	return jit ? h.GetGprJit(kRd) : h.GetGprInterp(kRd);
}
} // namespace

// The O/U class across the whole family, engine against engine.
//
// The interpreter is the reference side: it matches the console's FCR31 on
// every row of the capture
// (EeFpuOverflowConsole.InterpreterRaisesOverflowAndUnderflowLikeTheConsole).
// So it is pinned to the model, and the fast path to the interpreter's word
// minus the four raise bits -- a mask rather than a row list, so a fast path
// that dropped a clear or raised a bit the interpreter did not still fails.
TEST(EeFpuFcrConsoleConformance, EnginesAgreeOnTheOverflowFlagClear)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest};
	constexpr u32 kRaiseBits = kFlagO | kFlagU | kFlagSO | kFlagSU;
	int checked = 0, cleared = 0, raised = 0, witnessed = 0, saturation_rows = 0;

	for (int i = 0; i < kFamCaseCount; ++i)
	{
		const FamCase& c = kFamCases[i];
		const u32 word = FamOpWord(c);
		ASSERT_NE(word, 0u) << c.what;

		u32 res[2] = {};
		const u32 interp = RunFamCase(c, false, &res[0]);
		const u32 jit = RunFamCase(c, true, &res[1]);

		SCOPED_TRACE(::testing::Message() << c.what);
		EXPECT_EQ(interp, c.want_fcr31)
			<< "[interp] no longer matches the raiseOrClearOU model in FPU.cpp";

		// A row is a raise row iff the interpreter ends with a raise bit the
		// preset did not already carry: O|U come in preset, so SO|SU are the
		// tell, and the underflow rows clear O on the way and cannot be
		// confused with a preserve.
		const bool raises = (c.want_fcr31 & (kFlagSO | kFlagSU)) != 0;
		if (raises)
		{
			++raised;
			EXPECT_EQ(jit, interp & ~kRaiseBits)
				<< "[jit] must be the interpreter's word minus O|U|SO|SU. If it "
				   "now EQUALS the interpreter, the fast path learned to raise: "
				   "collapse this branch into a plain equality and retire the "
				   "tier note above.";
		}
		else
		{
			EXPECT_EQ(jit, interp) << "engines disagree on FCR31 O/U";
			if (c.want_fcr31 == kFcr31FixedOnes)
				++cleared;
		}

		// Only the flags are supposed to be under test -- if the arithmetic
		// diverged too, the row is measuring the wrong thing. One shape is
		// allowed: a saturating row, where the interpreter stops at the EE's
		// own maximum and the fast path a binade below at FLT_MAX. See
		// EnginesAgreeExceptOnTheDocumentedRows in
		// ee_fpu_overflow_console_conformance_tests.cpp, which makes the same
		// allowance and says why it is a value pair and not a row list.
		const bool saturation_tier_gap =
			(res[0] & 0x7FFFFFFFu) == 0x7FFFFFFFu &&
			(res[1] & 0x7FFFFFFFu) == 0x7F7FFFFFu &&
			(res[0] & 0x80000000u) == (res[1] & 0x80000000u);
		if (saturation_tier_gap)
			++saturation_rows;
		else
			EXPECT_EQ(res[1], res[0]) << "engines disagree on the RESULT, so this "
			                             "row no longer isolates the flag write";
		witnessed += c.console_agrees_on_raise ? 1 : 0;
		++checked;
	}

	EXPECT_EQ(checked, kFamCaseCount);
	// Anti-vacuity. The clear count is the one that matters for the fast path:
	// it was 0 before the emitters got fpuClearOUFlags(), then 2 (ABS/NEG).
	EXPECT_GE(cleared, 14) << "fewer clear rows than emitters that owe the "
	                          "clear -- an op lost its coverage";
	EXPECT_GE(raised, 8) << "anti-vacuity: no raise rows left, so the "
	                        "allowance branch is never exercised";
	EXPECT_GE(saturation_rows, 4)
		<< "anti-vacuity for the saturation allowance: if no row saturates any "
		   "more, the allowance is dead code hiding a future divergence. If it "
		   "dropped to zero because the FAST PATH learned to saturate at the EE "
		   "maximum, delete the allowance instead of lowering this floor.";
	EXPECT_GE(witnessed, 20) << "anti-vacuity: most of this table must stay "
	                            "silicon-witnessed on the raise";
}

// Several flag writers in one block, which is where the recompiler's FCR31
// block residency (GE-12) has to hold the whole model together: the
// arithmetic family now read-modify-writes the same allocator-resident FCR31
// that C.cond writes the condition bit into, so a bad mask would either eat C
// or make SO non-sticky. Both orderings are checked because they exercise
// different halves: O has to come back down when a later op does not overflow,
// and it has to stay up when the last one does. The sticky SO stays up through
// both, and neither ordering may disturb C.
TEST(EeFpuFcrConsoleConformance, OverflowFlagsComposeAcrossOneBlock)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest};
	constexpr u32 kA = 7, kB = 8;   // compare operands, 1.0 and 2.0
	constexpr u32 kC = 0x00800000;  // FCR31 condition bit

	struct Ordering { const char* what; bool overflow_last; u32 want; };
	const Ordering orders[] = {
		// C set, then overflow raises O|SO, then an in-range op clears O and
		// leaves SO: C | SO.
		{"overflow then in-range", false, kFcr31FixedOnes | kC | kFlagSO},
		// C set, in-range op clears O, then the overflow raises it again:
		// C | O | SO.
		{"in-range then overflow", true,
		 kFcr31FixedOnes | kC | kFlagO | kFlagSO},
	};

	for (const Ordering& o : orders)
	{
		const u32 ovf = MUL_S(kFd, kFs, kFt);
		const u32 tame = ADD_S(kFd, kA, kB);
		u32 got[2];
		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFcr31(kFcr31FixedOnes);
			h.SetFprBits(kFs, kFMax);
			h.SetFprBits(kFt, kFMax);
			h.SetFprBits(kA, kOne);
			h.SetFprBits(kB, kTwo);
			h.SetGpr128(kRd, 0, 0);
			h.LoadProgram({C_LT_S(kA, kB),  // 1.0 < 2.0 -> C = 1
			               o.overflow_last ? tame : ovf,
			               o.overflow_last ? ovf : tame,
			               CFC1(kRd, 31)});
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();
			got[jit] = jit ? h.GetGprJit(kRd) : h.GetGprInterp(kRd);
		}
		constexpr u32 kRaiseBits = kFlagO | kFlagU | kFlagSO | kFlagSU;
		SCOPED_TRACE(::testing::Message() << o.what);
		EXPECT_EQ(got[0], o.want) << "[interp]";
		EXPECT_EQ(got[1], o.want & ~kRaiseBits)
			<< "[jit] must be the interpreter's word minus O|U|SO|SU -- it does "
			   "the clear in both orderings and the raise in neither";
		EXPECT_EQ(got[1] & kC, kC)
			<< "[jit] the condition bit did not survive the flag RMWs";
	}
}

// The denormal-result question, with FZ off.
//
// The flag half no longer belongs to this test: U|SU come from the magnitude of
// the exact result, so the interpreter raises them with FZ on or off, and
// "MUL.S underflow" in kFamCases covers that in the environment a game runs in.
// What is left here is the value -- with FZ off the host produces a real
// denormal, the interpreter flushes it to signed zero (clampToEeRange) and the
// recompilers keep it.
//
// It stays DISABLED because pinning either side would be picking a winner
// without silicon: the capture has no FZ-off row, and it cannot have one, since
// the console's own FPU has no denormal results to capture. Force-enable to see
// the current state.
TEST(EeFpuFcrConsoleConformance, DISABLED_UnderflowFlagsNeedFzOff)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::IeeeNearest};
	// FLT_MIN * 2^-2 is a denormal; the interpreter should set U|SU and flush
	// the result to +0, and clear O on the way.
	FamCase c = {"MUL.S underflow", FA_MUL, 0, 0x00800000, 0x3E800000,
	             kFcr31FixedOnes | kFlagU | kFlagSU, false};
	u32 res[2] = {};
	const u32 interp = RunFamCase(c, false, &res[0]);
	const u32 jit = RunFamCase(c, true, &res[1]);
	EXPECT_EQ(interp, c.want_fcr31) << "[interp]";
	EXPECT_EQ(res[0], 0x00000000u) << "[interp] must flush the denormal";
	EXPECT_EQ(jit, interp) << "engines disagree on FCR31 U/SU -- the fast path "
	                          "raise, same gap as everywhere else";
	EXPECT_EQ(res[1], res[0]) << "engines disagree on the denormal result -- "
	                             "this is the part that is actually open";
}

// DISABLED_ExceptionFlagsMatchConsole and
// DISABLED_ExceptionFlagsInProductionFpEnvMissOverflow used to sit here, one
// per FP environment. ExceptionFlagsMatchConsoleExceptTheFastPathRaise above
// asserts the one table in both.

// Both engines model the hardware: every control-register index aliases onto
// FCR0/FCR31 and every FCR31 write comes back through the mask model.
TEST(EeFpuFcrConsoleConformance, BothEnginesMatchConsoleFcrModel)
{
	for (int jit = 0; jit < 2; ++jit)
	{
		SCOPED_TRACE(jit ? "[jit]" : "[interp]");
		EXPECT_EQ(RunAndReadGpr({CFC1(kRd, 0)}, kFcr31FixedOnes, jit != 0),
		          kFcr0);
		EXPECT_EQ(RunAndReadGpr({CFC1(kRd, 7)}, kFcr31FixedOnes, jit != 0),
		          kFcr0);
		EXPECT_EQ(RunAndReadGpr({CFC1(kRd, 20)}, kFcr31FixedOnes, jit != 0),
		          kFcr31FixedOnes);
		for (int i = 0; i < kFcr31WriteCount; ++i)
		{
			const Fcr31Write& w = kFcr31Writes[i];
			SCOPED_TRACE(w.what);
			EXPECT_EQ(RunAndReadGpr({LUI(kRt, w.written >> 16),
			                         ORI(kRt, kRt, w.written & 0xFFFF),
			                         CTC1(kRt, 31), CFC1(kRd, 31)},
			                        kFcr31FixedOnes, jit != 0),
			          w.readback);
		}
	}
}

} // namespace recompiler_tests
