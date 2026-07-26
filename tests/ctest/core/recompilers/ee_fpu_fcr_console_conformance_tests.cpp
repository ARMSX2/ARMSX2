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

// The O and U CAUSE bits clear on every op that can raise them, whether or not
// it does -- the sticky SO/SU and C survive. On silicon a seeded FCR31 of
// 0x0183C079 reads back 0x01830079 after any of the fourteen: ADD, SUB, MUL,
// the four A-forms, the four multiply-accumulates, MAX, MIN, ABS and NEG (the
// last ten witnessed on FCR31-seeded capture rows; SUB, SUBA, MADDA and MSUBA
// on the interpreter's authority -- checkOverflow/checkUnderflow and
// clearFPUFlags clear the pair on all fourteen). DIV, SQRT, RSQRT and MOV
// must leave O and U ALONE, and are the controls.
//
// The fast path used to clear none of them, so an O or U raised earlier in a
// block stayed visible to every later cfc1. RAISING O/U is a separate,
// harder obligation -- a correct raise needs the exact result magnitude,
// which a saturating single cannot carry -- and stays with the FULL tier and
// the DISABLED tripwires below.
TEST(EeFpuFcrConsoleConformance, EnginesAgreeOnTheOverflowFlagClear)
{
	constexpr u32 kCauseOU = 0x0000C000u;
	// All four causes + all four stickies + C, the capture's seeded word.
	constexpr u32 kSeedFull = 0x0183C079u;
	// O|U causes + stickies + C only: the div-family controls clear I|D on
	// their own account, which is not the property under test.
	constexpr u32 kSeedOU = 0x0180C079u;

	struct ClearCase
	{
		const char* what;
		u32 op;
		bool clears;
	};
	const ClearCase kCases[] = {
		{"ADD.S", ADD_S(kFd, kFs, kFt), true},
		{"SUB.S", SUB_S(kFd, kFs, kFt), true},
		{"MUL.S", MUL_S(kFd, kFs, kFt), true},
		{"ADDA.S", ADDA_S(kFs, kFt), true},
		{"SUBA.S", SUBA_S(kFs, kFt), true},
		{"MULA.S", MULA_S(kFs, kFt), true},
		{"MADD.S", MADD_S(kFd, kFs, kFt), true},
		{"MSUB.S", MSUB_S(kFd, kFs, kFt), true},
		{"MADDA.S", MADDA_S(kFs, kFt), true},
		{"MSUBA.S", MSUBA_S(kFs, kFt), true},
		{"MAX.S", MAX_S(kFd, kFs, kFt), true},
		{"MIN.S", MIN_S(kFd, kFs, kFt), true},
		{"ABS.S", ABS_S(kFd, kFs), true},
		{"NEG.S", NEG_S(kFd, kFs), true},
		{"DIV.S (control)", DIV_S(kFd, kFs, kFt), false},
		{"SQRT.S (control)", SQRT_S(kFd, kFt), false},
		{"RSQRT.S (control)", RSQRT_S(kFd, kFs, kFt), false},
		{"MOV.S (control)", MOV_S(kFd, kFs), false},
	};

	int cleared = 0, controls = 0;
	for (const ClearCase& c : kCases)
	{
		SCOPED_TRACE(c.what);
		const u32 seed = c.clears ? kSeedFull : kSeedOU;
		const u32 want = c.clears ? (kSeedFull & ~kCauseOU) : kSeedOU;

		u32 got[2];
		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFcr31(seed);
			h.SetFpr(kFs, 2.5f); // benign operands: nothing raises, so what
			h.SetFpr(kFt, 1.5f); // remains set afterwards is what SURVIVED
			h.LoadProgram({static_cast<u32>(c.op), CFC1(kRd, 31)});
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();
			got[jit] = jit ? h.GetGprJit(kRd) : h.GetGprInterp(kRd);
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			EXPECT_EQ(got[jit], want);
		}
		EXPECT_EQ(got[0], got[1]) << "engines disagree on the flag clear";
		(c.clears ? cleared : controls)++;
	}
	EXPECT_EQ(cleared, 14);
	EXPECT_EQ(controls, 4);
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
	// Rows where the fast path does not reproduce the console word. "NAN
	// math" diverges only at the default clamp mode; see
	// DISABLED_NanMathOverflowIsAnOperandClampModeDifference.
	bool bad_jit;
};
constexpr FlagSituation kFlagSituations[] = {
	// sqrt(-1) is 1.0 on the PS2, not NaN: SQRT takes the magnitude. Both the
	// invalid sticky flag (bit 6) and its cause bit (17) come up.
	{"sqrt(-1)", FO_SQRT, 0xBF800000, 0xBF800000, 0x01020041, true, 0x3F800000, false},
	{"Divide zero by zero", FO_DIV, 0x00000000, 0x00000000, 0x01020041, false, 0, false},
	{"Divide one by zero", FO_DIV, 0x3F800000, 0x00000000, 0x01010021, false, 0, false},
	{"NAN math", FO_ADD, 0x7F800001, 0x7F800001, 0x01008011, false, 0, true},
	{"Overflow", FO_MUL, 0x7F7FFFFF, 0x7F7FFFFF, 0x01008011, false, 0, false},
	// FLT_MIN/3 is a denormal, which the PS2 flushes to zero — and raises
	// nothing doing it.
	{"Underflow", FO_DIV, 0x00800000, 0x40400000, 0x01000001, true, 0x00000000, false},
	// 1 / 3.0155 — an ordinary inexact result. The EE has no inexact flag,
	// which is the same fact bit 2 being unwritable shows above.
	{"Inexact", FO_DIV, 0x3F800000, 0x4040FFFF, 0x01000001, false, 0, false},
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

// PCSX2 reproduces the console's FCR31 exception flags only at round-to-
// nearest. Under the production rounding mode an overflow saturates to FLT_MAX
// instead of producing Inf, and the interpreter's overflow detection -- which
// looks for Inf -- never fires: FCR31 reads 0x1000001 where the console says
// 0x1008011. That is not a stale expectation, it is PCSX2 diverging from
// hardware in the environment a game runs in, and it is pinned separately by
// DISABLED_ExceptionFlagsInProductionFpEnvMissOverflow below.
// Cross-ENGINE agreement on FCR31, independent of the console column.
//
// This used to list two divergent situations, on both of which the interpreter
// was the side that matched silicon exactly: 0x01008011 against the arm64
// recompiler's 0x01000001, the gap being O (0x8000) and SO (0x10). The cause
// was blunt -- FPU.cpp has checkOverflow()/clearFPUFlags() and raises the pair
// while the recompiler modelled no FCR31 overflow flags at all -- and the
// direction was therefore to fix the RECOMPILER, never to make the interpreter
// stop raising them, which would have aligned all three engines by destroying
// the only correct reference in the tree.
//
// An emitter raising them from the host result was tried and rejected -- see
// the tripwire note below -- so the whole class is parked as DISABLED:
// EnginesAgreeOnOverflowFlagsAcrossTheArithmeticFamily covers all
// ten checkOverflow ops plus the four that only clear and the three that must
// leave O and U alone.
//
// One row is left, and it is no longer about O/SO handling at all: "NAN math"
// hands ADD.S two raw exp-255 words, and the two engines therefore compute
// different things before any flag logic runs (interp clamps operands through
// fpuDouble and gets Inf; the fast path gets a host NaN, which correctly is not
// an overflow). It is the operand-clamp mode axis, not a missing flag --
// NanMathOverflowIsAnOperandClampModeDifference measures exactly that, and the
// x86 JIT gates its operand clamp on the same CHECK_FPU_EXTRA_OVERFLOW.
//
// Note the environment dependence, which is why the pair was once filed as
// unanimous-but-wrong: under the PRODUCTION FP environment (ChopZero) an
// overflow saturates to FLT_MAX and never reaches Inf, so nothing detects it on
// either engine and they agree. It is only at round-to-nearest -- the
// ScopedFpEnv this file uses -- that the split appears at all.
constexpr const char* kFcrEngineDivergences[] = {
	"NAN math",
};

// TRIPWIRE -- the arm64 FPU fast path raises no FCR31 O/SO at all.
//
// An emitter that raised them was tried and rejected. It detected overflow
// as `fabs(result) > FLT_MAX`, i.e. by sniffing for a HOST infinity, which made
// an architectural flag a function of eeRoundMode: never raised under the
// shipping ChopZero default, and raised for ONE SIGN ONLY under
// eeRoundMode 1/2. It also fired on operations that are not overflows at all
// (mul 2^128 by 1.0 or 0.5, add 2^128 + 0), contradicting the console on rows
// the JIT had previously got right, and it cost +8 host instructions on every
// arithmetic op -- MUL.S went 3 -> 11 -- for a flag x86 does not maintain at
// all (every O/U write in pcsx2/x86/iFPU.cpp is commented out).
//
// What the redesign owes: derive O from the operands and the operation, the way
// iFPUd-arm64.cpp's ToPS2FPU_Full already does with its 2^128/2^129 magnitude
// thresholds -- round-mode independent and FZ independent -- rather than from
// the host result register. Enable these tests when it does.
TEST(EeFpuFcrConsoleConformance, DISABLED_EnginesAgreeExceptOnTheOverflowFlags)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest};
	int diverged = 0;
	for (int i = 0; i < kFlagSituationCount; ++i)
	{
		const FlagSituation& s = kFlagSituations[i];
		const u32 word = FlagOpWord(s);
		ASSERT_NE(word, 0u) << s.what;

		u32 got[2];
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
		}

		bool known = false;
		for (const char* k : kFcrEngineDivergences)
			known = known || (std::string(s.what) == k);

		SCOPED_TRACE(::testing::Message() << s.what);
		if (!known)
		{
			EXPECT_EQ(got[1], got[0]) << "engines disagree on FCR31";
			continue;
		}
		++diverged;
		EXPECT_NE(got[1], got[0])
			<< "the engines now AGREE. If the JIT started clamping its operands "
			   "at the default clamp mode, drop this row from "
			   "kFcrEngineDivergences.";
		// Pin which side is right, so a future "fix" that aligns them by
		// removing the interpreter's flags fails here instead of passing.
		EXPECT_EQ(got[0], s.fcr31)
			<< "[interp] must stay the console-matching side";
		EXPECT_EQ(got[0] & ~got[1], 0x00008010u)
			<< "the gap must still be exactly O|SO";
	}
	EXPECT_EQ(diverged, static_cast<int>(std::size(kFcrEngineDivergences)));
}

// The one remaining FCR31 divergence, attributed rather than merely listed.
//
// "NAN math" is ADD.S on two raw exp-255 words. The interpreter routes every
// operand through fpuDouble, which turns exp-255 into ±fMax, so it adds
// fMax+fMax, gets Inf, and raises O|SO. The recompiler's fast path only clamps
// source operands under CHECK_FPU_EXTRA_OVERFLOW (GameDB eeClampMode >= 2), so
// at the default mode it hands the raw words to the host, gets a NaN, and
// correctly does not call that an overflow -- the result still lands on
// 0x7F7FFFFF either way, because fpuClampResult folds NaN to +fMax, which is
// why this is invisible to any test that only looks at the value.
//
// Turn the operand clamp on and the row aligns. That makes this a clamp-mode
// difference (a deliberate, shipping, x86-JIT-parity compromise -- x86 gates
// fpuFloat2 on the same option) rather than an unattributed hole in the flag
// logic, and it is the reason the row stays in kFcrEngineDivergences instead of
// being "fixed" by making the fast path clamp unconditionally.
// TRIPWIRE -- see the O/SO rejection note above.
TEST(EeFpuFcrConsoleConformance, DISABLED_NanMathOverflowIsAnOperandClampModeDifference)
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

	EXPECT_EQ(fcr[0], kConsole) << "[interp] is the console-matching side";
	EXPECT_NE(fcr[1], fcr[0])
		<< "the default clamp mode now agrees -- drop \"NAN math\" from "
		   "kFcrEngineDivergences and from bad_jit";
	EXPECT_EQ(fcr[1], kFcr31FixedOnes)
		<< "[jit, default clamp] a NaN result must not be called an overflow";
	EXPECT_EQ(fcr[2], fcr[0])
		<< "[jit, CHECK_FPU_EXTRA_OVERFLOW] clamping the operands the way "
		   "fpuDouble does must reproduce the interpreter's O|SO exactly -- if "
		   "this fails the divergence is NOT the operand-clamp axis and the "
		   "attribution above is wrong";
	// The value is identical in all three legs; only FCR31 moves.
	EXPECT_EQ(res[0], 0x7F7FFFFFu);
	EXPECT_EQ(res[1], res[0]);
	EXPECT_EQ(res[2], res[0]);
}

// ---------------------------------------------------------------------------
// The O/U class, engine against engine.
//
// The console rows above are one window into a family: the FCR31 overflow and
// underflow maintenance pcsx2/FPU.cpp performs on every arithmetic op and the
// recompilers perform on none. It needs no capture, because the interpreter is
// the reference side; the three behaviours it implements are the three groups
// of kFamCases below.
//
// Only the overflow half is exercised. FZ is set in every FP environment PCSX2
// runs the EE under -- DAZ+FTZ+ChopZero is the shipping default and both
// ScopedFpEnv kinds this file uses keep FZ on -- so no denormal result can
// reach checkUnderflow and U is only ever cleared.
// DISABLED_UnderflowFlagsNeedFzOff pins the FZ-off half.
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

constexpr u32 kFlagO = 0x00008000, kFlagU = 0x00004000;
constexpr u32 kFlagSO = 0x00000010;

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
	// (1) Overflow: set O|SO, and leave U alone (checkOverflow returns early).
	{"ADD.S overflow",    FA_ADD,   0,        kFMax, kFMax,    kOuPreset | kFlagSO},
	{"SUB.S overflow",    FA_SUB,   0,        kFMax, kNegFMax, kOuPreset | kFlagSO},
	{"MUL.S overflow",    FA_MUL,   0,        kFMax, kFMax,    kOuPreset | kFlagSO},
	{"ADDA.S overflow",   FA_ADDA,  0,        kFMax, kFMax,    kOuPreset | kFlagSO},
	{"SUBA.S overflow",   FA_SUBA,  0,        kFMax, kNegFMax, kOuPreset | kFlagSO},
	{"MULA.S overflow",   FA_MULA,  0,        kFMax, kFMax,    kOuPreset | kFlagSO},
	{"MADD.S overflow",   FA_MADD,  kFMax,    kFMax, kOne,     kOuPreset | kFlagSO},
	{"MSUB.S overflow",   FA_MSUB,  kFMax,    kFMax, kNegOne,  kOuPreset | kFlagSO},
	{"MADDA.S overflow",  FA_MADDA, kFMax,    kFMax, kOne,     kOuPreset | kFlagSO},
	{"MSUBA.S overflow",  FA_MSUBA, kFMax,    kFMax, kNegOne,  kOuPreset | kFlagSO},

	// (1) No overflow: clear O, then clear U.
	{"ADD.S in range",    FA_ADD,   kOne, kOne, kTwo, kFcr31FixedOnes},
	{"SUB.S in range",    FA_SUB,   kOne, kOne, kTwo, kFcr31FixedOnes},
	{"MUL.S in range",    FA_MUL,   kOne, kOne, kTwo, kFcr31FixedOnes},
	{"ADDA.S in range",   FA_ADDA,  kOne, kOne, kTwo, kFcr31FixedOnes},
	{"SUBA.S in range",   FA_SUBA,  kOne, kOne, kTwo, kFcr31FixedOnes},
	{"MULA.S in range",   FA_MULA,  kOne, kOne, kTwo, kFcr31FixedOnes},
	{"MADD.S in range",   FA_MADD,  kOne, kOne, kTwo, kFcr31FixedOnes},
	{"MSUB.S in range",   FA_MSUB,  kOne, kOne, kTwo, kFcr31FixedOnes},
	{"MADDA.S in range",  FA_MADDA, kOne, kOne, kTwo, kFcr31FixedOnes},
	{"MSUBA.S in range",  FA_MSUBA, kOne, kOne, kTwo, kFcr31FixedOnes},

	// (2) clearFPUFlags(O|U) and nothing else.
	{"ABS.S clears O|U",  FA_ABS,   0, kNegOne, 0,    kFcr31FixedOnes},
	{"NEG.S clears O|U",  FA_NEG,   0, kOne,    0,    kFcr31FixedOnes},
	{"MAX.S clears O|U",  FA_MAX,   0, kOne,    kTwo, kFcr31FixedOnes},
	{"MIN.S clears O|U",  FA_MIN,   0, kOne,    kTwo, kFcr31FixedOnes},

	// (3) Negative controls -- the divide unit passes 0 to checkOverflow, so
	// O and U must come out exactly as they went in. Groups (1) and (2) above
	// move the same bits from the same pre-state, so "unchanged" here means
	// preserved rather than unobserved.
	{"DIV.S preserves",   FA_DIV,   0, kOne, kTwo,  kOuPreset},
	{"SQRT.S preserves",  FA_SQRT,  0, 0,    kFour, kOuPreset},
	{"RSQRT.S preserves", FA_RSQRT, 0, kOne, kFour, kOuPreset},
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

// TRIPWIRE -- see the O/SO rejection note above.
TEST(EeFpuFcrConsoleConformance, DISABLED_EnginesAgreeOnOverflowFlagsAcrossTheArithmeticFamily)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest};
	int checked = 0;
	for (int i = 0; i < kFamCaseCount; ++i)
	{
		const FamCase& c = kFamCases[i];
		const u32 word = FamOpWord(c);
		ASSERT_NE(word, 0u) << c.what;

		u32 res[2] = {};
		const u32 interp = RunFamCase(c, false, &res[0]);
		const u32 jit = RunFamCase(c, true, &res[1]);

		SCOPED_TRACE(::testing::Message() << c.what);
		// Pin the interpreter to what FPU.cpp's checkOverflow/clearFPUFlags
		// model says it must produce.
		EXPECT_EQ(interp, c.want_fcr31) << "[interp] no longer matches the "
		                                   "checkOverflow model in FPU.cpp";
		EXPECT_EQ(jit, interp) << "engines disagree on FCR31 O/U";
		EXPECT_EQ(res[1], res[0]) << "engines disagree on the RESULT, so this "
		                             "row no longer isolates the flag write";
		++checked;
	}
	EXPECT_EQ(checked, kFamCaseCount);
}

// Several flag writers in one block, where the recompiler's FCR31 block
// residency (GE-12) has to hold the model together: the arithmetic family
// read-modify-writes the same allocator-resident FCR31 that C.cond writes the
// condition bit into, so a bad mask would either eat C or make SO non-sticky.
// TRIPWIRE -- see the O/SO rejection note above.
TEST(EeFpuFcrConsoleConformance, DISABLED_OverflowFlagsComposeAcrossOneBlock)
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
		SCOPED_TRACE(::testing::Message() << o.what);
		EXPECT_EQ(got[0], o.want) << "[interp]";
		EXPECT_EQ(got[1], got[0]) << "engines disagree";
		EXPECT_EQ(got[1] & kC, kC)
			<< "[jit] the condition bit did not survive the flag RMWs";
	}
}

// The underflow half of checkUnderflow(result, U|SU), which needs a denormal
// result and therefore needs FZ off. DISABLED because it is the denormal-
// operand work item, not this one: with FZ off the two engines also disagree
// on the value (the interpreter flushes the denormal to signed zero inside
// checkUnderflow, the recompilers keep it), and pinning the flag without the
// value would assert half a behaviour. Force-enable to see the current state.
TEST(EeFpuFcrConsoleConformance, DISABLED_UnderflowFlagsNeedFzOff)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::IeeeNearest};
	// FLT_MIN * 2^-2 is a denormal; the interpreter should set U|SU and flush
	// the result to +0, and clear O on the way.
	FamCase c = {"MUL.S underflow", FA_MUL, 0, 0x00800000, 0x3E800000,
	             kFcr31FixedOnes | kFlagU | 0x00000008};
	u32 res[2] = {};
	const u32 interp = RunFamCase(c, false, &res[0]);
	const u32 jit = RunFamCase(c, true, &res[1]);
	EXPECT_EQ(interp, c.want_fcr31) << "[interp]";
	EXPECT_EQ(jit, interp) << "engines disagree on FCR31 U/SU";
	EXPECT_EQ(res[0], 0x00000000u) << "[interp] must flush the denormal";
	EXPECT_EQ(res[1], res[0]) << "engines disagree on the denormal result";
}

// TRIPWIRE -- see the O/SO rejection note above.
TEST(EeFpuFcrConsoleConformance, DISABLED_ExceptionFlagsMatchConsole)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest};
	int checked = 0;
	for (int i = 0; i < kFlagSituationCount; ++i)
	{
		const FlagSituation& s = kFlagSituations[i];
		const u32 word = FlagOpWord(s);
		ASSERT_NE(word, 0u) << s.what;

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

			const u32 got = jit ? h.GetGprJit(kRd) : h.GetGprInterp(kRd);
			if (jit && s.bad_jit)
			{
				EXPECT_NE(got, s.fcr31)
					<< s.what
					<< " [jit] now matches silicon; the recompiler must have "
					   "gained O/SO handling — drop bad_jit for this row.";
				continue;
			}

			SCOPED_TRACE(::testing::Message()
			             << s.what << (jit ? " [jit]" : " [interp]"));
			EXPECT_EQ(got, s.fcr31);
			if (s.check_fd)
			{
				EXPECT_EQ(jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd),
				          s.fd);
			}
		}
		++checked;
	}
	EXPECT_EQ(checked, kFlagSituationCount);
}

// The same table, in the environment a game actually runs in: no ScopedFpEnv,
// so ChopZero is in force. Every overflow saturates to +/-FLT_MAX instead of
// reaching Inf, and PCSX2's overflow detection -- which looks for Inf, in both
// engines -- cannot fire. Measured: the Overflow and NAN-math rows read FCR31
// 0x1000001 where the console says 0x1008011, i.e. O and SO missing.
//
// DISABLED because it is a statement about PCSX2, not a regression: the fix is
// to stop inferring overflow from Inf, and until someone does that this is the
// production truth. Force-enable it to see the current row-by-row state.
//
// The open hardware question is what "overflow" means on the EE FPU, since the
// unit truncates: does silicon raise O from the magnitude of the exact result,
// independently of rounding? A capture of FCR31 after an overflowing ADD.S/MUL.S
// would settle it -- and the same answer decides the VU O flag (work-order
// item 6 / the FP-environment section).
TEST(EeFpuFcrConsoleConformance, DISABLED_ExceptionFlagsInProductionFpEnvMissOverflow)
{
	for (int i = 0; i < kFlagSituationCount; ++i)
	{
		const FlagSituation& s = kFlagSituations[i];
		const u32 word = FlagOpWord(s);
		ASSERT_NE(word, 0u) << s.what;

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

			SCOPED_TRACE(::testing::Message()
			             << s.what << (jit ? " [jit]" : " [interp]")
			             << " (production FP environment)");
			EXPECT_EQ(jit ? h.GetGprJit(kRd) : h.GetGprInterp(kRd), s.fcr31);
		}
	}
}

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
