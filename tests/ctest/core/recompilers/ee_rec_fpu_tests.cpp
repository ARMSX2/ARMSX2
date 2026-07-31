// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// FPU / COP1 coverage for the EE recompiler. Single-precision only (the
// PS2 FPU is 32-bit; the `DOUBLE::` namespace is internal accuracy emulation,
// not a user-visible double-precision ISA).
//
// Ops covered: MTC1/MFC1 bit moves, CTC1/CFC1 control-register moves,
// ADD.S/SUB.S/MUL.S/DIV.S, NEG.S/ABS.S/MOV.S, CVT.W.S, compare family
// (C.EQ.S/C.LT.S) + BC1T/BC1F.
//
// Value discipline: tests use small-integer float values and simple
// ratios to avoid PS2 FPU quirks (denormal flush-to-zero, peculiar NaN
// propagation) that only matter for full FPU correctness.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "common/FPControl.h"

#include <cstring>
#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

namespace {
constexpr u32 kPark = RecompilerTestEnvironment::kParkingPc;

// Scoped enable of CHECK_FPU_EXTRA_OVERFLOW (per-game GameDB clampMode>=2),
// which the harness leaves at its default-off. Restores on scope exit so the
// flag never leaks into sibling tests.
struct FpuExtraOverflowGuard
{
	bool saved = EmuConfig.Cpu.Recompiler.fpuExtraOverflow;
	FpuExtraOverflowGuard() { EmuConfig.Cpu.Recompiler.fpuExtraOverflow = true; }
	~FpuExtraOverflowGuard() { EmuConfig.Cpu.Recompiler.fpuExtraOverflow = saved; }
};

// Scoped eeClampMode selector. Sets the two Recompiler bits SetEEClampMode
// derives from the mode: fpuOverflow (== CHECK_FPU_OVERFLOW, mode >= 1) gates
// the result clamp and the MAX/MIN operand clamp; fpuExtraOverflow (mode >= 2)
// gates the arithmetic operand clamp. Restores both on scope exit.
struct FpuClampModeGuard
{
	bool savedO = EmuConfig.Cpu.Recompiler.fpuOverflow;
	bool savedX = EmuConfig.Cpu.Recompiler.fpuExtraOverflow;
	explicit FpuClampModeGuard(int mode)
	{
		EmuConfig.Cpu.Recompiler.fpuOverflow = (mode >= 1);
		EmuConfig.Cpu.Recompiler.fpuExtraOverflow = (mode >= 2);
	}
	~FpuClampModeGuard()
	{
		EmuConfig.Cpu.Recompiler.fpuOverflow = savedO;
		EmuConfig.Cpu.Recompiler.fpuExtraOverflow = savedX;
	}
};

u32 FloatBits(float f)
{
	u32 bits;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

float BitsToFloat(u32 bits)
{
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}
} // namespace

TEST(EeRecFpu, Mtc1MovesGprBitsToFpr)
{
	// MTC1 copies GPR bits verbatim to the FPR; no conversion. 0x40490FDB
	// is the IEEE-754 bit pattern for a value near π.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetGpr64(reg::a0, 0x40490FDBu);
	h.LoadProgram({
		ee::MTC1(reg::a0, 1),            // fpr1 = bits(a0)
	});
	h.Run();
	h.ExpectFpr(1, 0x40490FDBu);
}

TEST(EeRecFpu, Mfc1MovesFprBitsToGprWithSignExtend)
{
	// MFC1 copies the 32-bit FPR bit pattern into rt, sign-extended to 64-bit.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(2, 0x80000001u);       // negative single-precision pattern
	h.LoadProgram({
		ee::MFC1(reg::v0, 2),
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 0xFFFFFFFF80000001ull);
}

// ---------------------------------------------------------------------------
//  LWC1 / SWC1 — FPU 32-bit load/store
//
//  These take the inline fastmem path (LDR/STR off RFASTMEMBASE +
//  backpatch) when CHECK_FASTMEM is set, with the softmem C-call as the
//  faulting-PC fallback. In the test build the harness wires fastmem, so
//  these exercise the fast path. Round-trip + bit-exactness are the spec:
//  LWC1 copies 32 raw bits into fpr[ft] verbatim (no FP conversion), and
//  SWC1 copies fpr[ft]'s 32 bits to memory verbatim.
// ---------------------------------------------------------------------------
namespace {
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;
}

TEST(EeRecFpu, Lwc1LoadsRawBitsIntoFpr)
{
	EeRecTestHarness h;
	h.EnableCop1();
	// Bit pattern with the sign bit set — proves no sign-extend / FP munge.
	h.WriteU32(kScratch, 0x80490FDBu);
	h.SetGpr64(reg::a0, kScratch);
	h.LoadProgram({
		ee::LWC1(2, 0, reg::a0),         // fpr2 = mem32[a0]
	});
	h.Run();
	h.ExpectFpr(2, 0x80490FDBu);
}

TEST(EeRecFpu, Swc1StoresRawFprBitsToMemory)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(3, 0xDEADBEEFu);        // raw pattern, not a clean float
	h.SetGpr64(reg::a0, kScratch);
	h.TrackMemWindow(kScratch, 4);
	h.LoadProgram({
		ee::SWC1(3, 0, reg::a0),         // mem32[a0] = fpr3
	});
	h.Run();
	EXPECT_EQ(h.ReadU32(kScratch), 0xDEADBEEFu);
}

TEST(EeRecFpu, Lwc1Swc1RoundtripWithOffset)
{
	// Load from one slot, store to another via a non-zero immediate offset —
	// exercises recComputeAddr's Add path and the fastmem index register.
	EeRecTestHarness h;
	h.EnableCop1();
	h.WriteU32(kScratch + 4, 0x3F800000u);   // 1.0f bits
	h.SetGpr64(reg::a0, kScratch);
	h.TrackMemWindow(kScratch, 16);
	h.LoadProgram({
		ee::LWC1(4, 4, reg::a0),         // fpr4 = mem32[a0+4]
		ee::SWC1(4, 8, reg::a0),         // mem32[a0+8] = fpr4
	});
	h.Run();
	h.ExpectFpr(4, 0x3F800000u);
	EXPECT_EQ(h.ReadU32(kScratch + 8), 0x3F800000u);
}

TEST(EeRecFpu, AddSInteger)
{
	// 3.0 + 4.0 = 7.0 — no rounding quirks.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),
	});
	h.Run();
	h.ExpectFpr(3, FloatBits(7.0f));
}

TEST(EeRecFpu, SubSInteger)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 10.0f);
	h.SetFpr(2, 3.0f);
	h.LoadProgram({
		ee::SUB_S(3, 1, 2),
	});
	h.Run();
	h.ExpectFpr(3, FloatBits(7.0f));
}

TEST(EeRecFpu, MulSInteger)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 6.0f);
	h.SetFpr(2, 7.0f);
	h.LoadProgram({
		ee::MUL_S(3, 1, 2),
	});
	h.Run();
	h.ExpectFpr(3, FloatBits(42.0f));
}

TEST(EeRecFpu, DivSExactRatio)
{
	// 20 / 4 = 5. Exact IEEE-754 result, no rounding divergence.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 20.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({
		ee::DIV_S(3, 1, 2),
	});
	h.Run();
	h.ExpectFpr(3, FloatBits(5.0f));
}

// ---- Native DIV.S: divide-by-zero corners. interp DIV_S is the oracle, so
//      Run() diffs the value; both snapshots' FCR31 are also asserted directly
//      to pin the sticky flags. ---------------------------------------------

TEST(EeRecFpu, DivSNegativeQuotientExact)
{
	// 6 / -2 = -3, exact — no rounding-mode sensitivity, no D/I flags.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0);
	h.SetFpr(1, 6.0f);
	h.SetFpr(2, -2.0f);
	h.LoadProgram({ee::DIV_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, FloatBits(-3.0f));
	const u32 mask = 0x20000u | 0x10000u; // I | D
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & mask, 0u);
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & mask, 0u);
}

// ---- divide saturation: the two tiers stop in different places -------------
//
// The interpreter saturates a divide at the EE's own maximum and the fast path
// a binade below at FLT_MAX; the console says the interpreter is the correct
// side, and EeFpuTopBinadeConsole has the rest. Run()'s auto-diff cannot
// express a deliberate divergence, so these run the two legs separately.
//
// The signs are unchanged: xor of both operands for DIV.S, Fs alone for
// RSQRT.S.
namespace {

struct DivTiers
{
	u32 jit, interp;
	u32 jit_fcr, interp_fcr;
};

DivTiers RunDivBothTiers(u32 word, u32 fs, u32 ft)
{
	DivTiers t{};
	for (int jit = 0; jit < 2; ++jit)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFcr31(0);
		h.SetFprBits(1, fs);
		h.SetFprBits(2, ft);
		h.LoadProgram({word});
		if (jit)
		{
			h.RunJitNoDiff();
			t.jit = h.GetFprBitsJit(3);
			t.jit_fcr = h.JitSnapshot().fprs.fprc[31];
		}
		else
		{
			h.RunInterpOnly();
			t.interp = h.GetFprBitsInterp(3);
			t.interp_fcr = h.InterpSnapshot().fprs.fprc[31];
		}
	}
	return t;
}

} // namespace

TEST(EeRecFpu, DivSByZeroSetsDenormFlagsAndMax)
{
	// 4 / +0, with D|SD raised (x/0).
	const DivTiers t = RunDivBothTiers(ee::DIV_S(3, 1, 2), 0x40800000u, 0x00000000u);
	EXPECT_EQ(t.jit, 0x7F7FFFFFu)    << "fast path saturates at FLT_MAX";
	EXPECT_EQ(t.interp, 0x7FFFFFFFu) << "interp saturates at the EE maximum";
	const u32 mask = 0x20000u | 0x10000u | 0x40u | 0x20u; // I|D|SI|SD
	EXPECT_EQ(t.jit_fcr & mask,    0x10000u | 0x20u); // D|SD
	EXPECT_EQ(t.interp_fcr & mask, 0x10000u | 0x20u);
}

TEST(EeRecFpu, DivSByZeroNegativeDividendSignedMax)
{
	// -4 / +0 : sign(Fs^Ft) is negative.
	const DivTiers t = RunDivBothTiers(ee::DIV_S(3, 1, 2), 0xC0800000u, 0x00000000u);
	EXPECT_EQ(t.jit, 0xFF7FFFFFu);
	EXPECT_EQ(t.interp, 0xFFFFFFFFu);
	const u32 mask = 0x10000u | 0x20u; // D|SD
	EXPECT_EQ(t.jit_fcr & mask,    0x10000u | 0x20u);
	EXPECT_EQ(t.interp_fcr & mask, 0x10000u | 0x20u);
}

TEST(EeRecFpu, DivSByNegativeZeroDivisorSign)
{
	// 8 / -0 : divisor is -0 (caught by the float==0 compare under FtZ); the
	// sign comes from the divisor.
	const DivTiers t = RunDivBothTiers(ee::DIV_S(3, 1, 2), 0x41000000u, 0x80000000u);
	EXPECT_EQ(t.jit, 0xFF7FFFFFu);
	EXPECT_EQ(t.interp, 0xFFFFFFFFu);
	const u32 mask = 0x10000u | 0x20u; // D|SD
	EXPECT_EQ(t.jit_fcr & mask,    0x10000u | 0x20u);
	EXPECT_EQ(t.interp_fcr & mask, 0x10000u | 0x20u);
}

TEST(EeRecFpu, DivSZeroByZeroSetsInvalidFlags)
{
	// 0 / 0 raises I|SI (invalid), not D|SD.
	const DivTiers t = RunDivBothTiers(ee::DIV_S(3, 1, 2), 0x00000000u, 0x00000000u);
	EXPECT_EQ(t.jit, 0x7F7FFFFFu);
	EXPECT_EQ(t.interp, 0x7FFFFFFFu);
	const u32 mask = 0x20000u | 0x10000u | 0x40u | 0x20u; // I|D|SI|SD
	EXPECT_EQ(t.jit_fcr & mask,    0x20000u | 0x40u); // I|SI
	EXPECT_EQ(t.interp_fcr & mask, 0x20000u | 0x40u);
}

TEST(EeRecFpu, NegSFlipsSignBit)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.5f);
	h.LoadProgram({
		ee::NEG_S(2, 1),
	});
	h.Run();
	h.ExpectFpr(2, FloatBits(-3.5f));
}

// NEG.S of a host-NaN bit pattern. This test has been through three answers,
// and the history is the point:
//
//   0x7FC00000 -> 0x7F7FFFFF   fpuClampResult (Fminnm/Fmaxnm) folded every NaN
//                              to +fMax and lost the sign.
//   0x7FC00000 -> 0xFF7FFFFF   fpuClampCompareOperand (Smin/Umin) kept the
//                              sign, mirroring x86's ClampValues -> fpuFloat3
//                              switch (upstream 4ffbe0bbf). Better, and still
//                              wrong: it was fixing which way a clamp was
//                              wrong rather than asking whether to clamp.
//   0x7FC00000 -> 0xFFC00000   no clamp at all. NEG.S is a sign-bit flip; the
//                              EE has no NaN, 0x7FC00000 is just a large
//                              finite number, and negating it must return its
//                              bit pattern with bit 31 set.
//
// The third is the console's answer, measured: first-party capture, `neg QNAN`,
// hardware 0xFFC00000. It is also what the interpreter has always produced
// (FPU.cpp NEG_S is `^ 0x80000000`) and what the FULL path has always emitted,
// so the previous note here that "neither the pre- nor post-fix rec matches
// [the interpreter]" no longer applies -- which is why this now runs the
// engine diff instead of RunJitNoDiff().
TEST(EeRecFpu, NegSOfANanBitPatternIsAPureSignFlip)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x7FC00000u); // host +qNaN; on the EE, an ordinary big float
	h.LoadProgram({
		ee::NEG_S(2, 1),
	});
	h.Run(); // engine diff: interp and JIT must agree here now
	h.ExpectFpr(2, 0xFFC00000u);
}

TEST(EeRecFpu, AbsSClearsSignBit)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, -4.25f);
	h.LoadProgram({
		ee::ABS_S(2, 1),
	});
	h.Run();
	h.ExpectFpr(2, FloatBits(4.25f));
}

TEST(EeRecFpu, MovSBitCopy)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x12345678u);
	h.LoadProgram({
		ee::MOV_S(2, 1),
	});
	h.Run();
	h.ExpectFpr(2, 0x12345678u);
}

// MOV.S fd,fd aliases the same host reg; the emit is skipped. The
// value must be preserved verbatim (the no-op is a true identity, not a drop).
TEST(EeRecFpu, MovSSelfMoveIsIdentity)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(5, 0xCAFEB0BAu);
	h.LoadProgram({
		ee::MOV_S(5, 5),
	});
	h.Run();
	h.ExpectFpr(5, 0xCAFEB0BAu);
}

TEST(EeRecFpu, CvtWSTruncatesToward)
{
	// 3.7 → 3 (FCR31 rounding mode is RZ/RN/... — use a value where
	// every IEEE-754 rounding mode agrees to avoid harness-dependent
	// results).
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.LoadProgram({
		ee::CVT_W_S(2, 1),
	});
	h.Run();
	h.ExpectFpr(2, 3u);       // integer 3 stored as bit pattern in the FPR
}

// ----- CVT.W NaN saturation ------------------------------------------
//
// ARM64 Fcvtzs converts NaN → 0, but the PS2 (interp CVT_W) saturates a
// NaN input by sign: +NaN → 0x7fffffff, -NaN → 0x80000000 (never 0). Inject
// raw NaN via SetFprBits (MTC1/LWC1 bit-copies bypass the arithmetic clamp).
// Run()'s auto-diff compares the FPR result, so an unfixed bare Fcvtzs (→0)
// diverges from interp; ExpectFpr pins the PS2 spec value on both sides.
TEST(EeRecFpu, CvtWPositiveNanSaturatesToIntMax)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x7FC00000u);   // +NaN
	h.LoadProgram({
		ee::CVT_W_S(2, 1),
	});
	h.Run();
	h.ExpectFpr(2, 0x7fffffffu);    // +NaN → INT_MAX
}

TEST(EeRecFpu, CvtWNegativeNanSaturatesToIntMin)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0xFFC00000u);   // -NaN
	h.LoadProgram({
		ee::CVT_W_S(2, 1),
	});
	h.Run();
	h.ExpectFpr(2, 0x80000000u);    // -NaN → INT_MIN
}

// ----- SQRT.S sticky-flag handling -----------------------------------
//
// PS2 SQRT.S clears the I|D cause flags unconditionally and sets I|SI whenever
// Ft's sign bit is set (interp SQRT_S, FPU.cpp; CHECK_FPU_EXTRA_FLAGS is
// hardcoded on). Run()'s auto-diff does not gate on fprc[31], so assert the
// flag bits directly on both snapshots (they must agree — the JIT matches
// interp). Result value (sqrt(|Ft|)) is unchanged and stays in the auto-diff.
TEST(EeRecFpu, SqrtSNegativeSetsInvalidStickyFlags)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, -4.0f);             // negative non-zero
	h.LoadProgram({
		ee::SQRT_S(2, 1),          // fd=2, ft=1; result sqrt(4)=2.0
	});
	h.Run();
	h.ExpectFpr(2, 0x40000000u);   // 2.0f
	const u32 mask = 0x20000u | 0x10000u | 0x40u;   // I | D | SI
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & mask,    0x20000u | 0x40u);  // I|SI set, D clear
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & mask, 0x20000u | 0x40u);
}

TEST(EeRecFpu, SqrtSPositiveClearsStaleIDFlags)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0x20000u | 0x10000u);   // pre-set stale I|D
	h.SetFpr(1, 4.0f);                  // positive → no flag set
	h.LoadProgram({
		ee::SQRT_S(2, 1),              // result sqrt(4)=2.0
	});
	h.Run();
	h.ExpectFpr(2, 0x40000000u);       // 2.0f
	// I and D must be cleared; SI must NOT have been set (positive input).
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & (0x20000u | 0x10000u | 0x40u), 0u);
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & (0x20000u | 0x10000u | 0x40u), 0u);
}

// ----- SQRT.S of -0.0 discards the sign -------------------------------
// IEEE-754 says sqrt(-0) is -0, and the interpreter used to say so too
// (`_FdValUl_ = _FtValUl_ & 0x80000000`). The EE does not: it returns +0, which
// is what the recompilers have always emitted (recSQRT_S_xmm takes |Ft| before
// the Fsqrt, so the sign is gone before the zero case is reached). Hardware,
// from ps2autotests tests/cpu/ee_fpu/sqrt.expected:
//
//     sqrt 80000000/-0.00: 00000000/+0.00
//     sqrt CF_NEGZERO:     00000000/+0.00
//
// Found by a randomized SQRT.S differential, which is why a case this small
// went unnoticed: every hand-written SQRT.S test above uses +/-4.0.
//
// The flag half used to be asserted here as "no I|SI", on nothing but the two
// engines agreeing; ps2autotests' sqrt.expected prints results only, never
// FCR31. The console does raise I|SI on -0, and the rule now lives in
// SqrtSInvalidFlagFollowsTheSignBitAlone below. This test keeps the value and
// asserts only that D stays clear.
TEST(EeRecFpu, SqrtSOfNegativeZeroIsPositiveZero)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0);
	h.SetFprBits(1, 0x80000000u);   // -0.0
	h.LoadProgram({ee::SQRT_S(2, 1)});
	h.Run();                         // auto-diff catches an engine keeping -0
	h.ExpectFpr(2, 0x00000000u);
	// D is the divide-by-zero flag and SQRT.S never raises it.
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & 0x10000u, 0u);
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & 0x10000u, 0u);
}

// ----- SQRT.S's I flag keys off the sign bit, not the exponent --------
// From a first-party PS2 capture that records FCR31 alongside the result
// (corpus 62dd6882, the 38 SQRT.S cases): SQRT.S raises I|SI whenever Ft's
// sign bit is set, whatever the
// exponent. So -0.0 and the negative denormals, flushed to -0 before the op
// and producing an ordinary +0, still raise invalid-operation. Every capture
// row agrees; the ten below are the sign x exponent matrix:
//
//     case 226  sqrt 00000000  ->  00000000/01000001    +0
//     case 227  sqrt 80000000  ->  00000000/01020041    -0          <- I|SI
//     case 235  sqrt 00000001  ->  00000000/01000001    +MIN_DENORM
//     case 236  sqrt 80000001  ->  00000000/01020041    -MIN_DENORM <- I|SI
//     case 237  sqrt 00400000  ->  00000000/01000001    +mid denorm
//     case 238  sqrt 007FFFFF  ->  00000000/01000001    +MAX_DENORM
//     case 228  sqrt 3F800000  ->  3F800000/01000001    +1.0
//     case 229  sqrt BF800000  ->  3F800000/01020041    -1.0        <- I|SI
//     case 239  sqrt 00800000  ->  20000000/01000001    +MIN_NORMAL
//     case 241  sqrt 80800000  ->  20000000/01020041    -MIN_NORMAL <- I|SI
//
// 0x01020041 is I|SI plus FCR31's two always-set bits (0x01000001); 0x01000001
// is those two bits alone. The positive rows are controls: a deletion that took
// the sign test out along with the exponent gate would raise I on every SQRT.S,
// and nothing but those rows would catch it.
namespace {
struct SqrtFlagRow { u32 ft, result, fcr31; const char* what; };
constexpr SqrtFlagRow kSqrtFlagRows[] = {
	{0x00000000u, 0x00000000u, 0x01000001u, "+0"},
	{0x80000000u, 0x00000000u, 0x01020041u, "-0"},
	{0x00000001u, 0x00000000u, 0x01000001u, "+MIN_DENORM"},
	{0x80000001u, 0x00000000u, 0x01020041u, "-MIN_DENORM"},
	{0x00400000u, 0x00000000u, 0x01000001u, "+mid denormal"},
	{0x007FFFFFu, 0x00000000u, 0x01000001u, "+MAX_DENORM"},
	{0x3F800000u, 0x3F800000u, 0x01000001u, "+1.0"},
	{0xBF800000u, 0x3F800000u, 0x01020041u, "-1.0"},
	{0x00800000u, 0x20000000u, 0x01000001u, "+MIN_NORMAL"},
	{0x80800000u, 0x20000000u, 0x01020041u, "-MIN_NORMAL"},
};
} // namespace

TEST(EeRecFpu, SqrtSInvalidFlagFollowsTheSignBitAlone)
{
	for (const SqrtFlagRow& r : kSqrtFlagRows)
	{
		SCOPED_TRACE(::testing::Message() << "sqrt.s " << r.what);

		EeRecTestHarness h;
		h.EnableCop1();
		// The capture's own pre-state: FCR31 at its power-on fixed ones, so the
		// sticky SI below can only have come from this instruction.
		h.SetFcr31(0x01000001u);
		h.SetFprBits(1, r.ft);
		h.LoadProgram({ee::SQRT_S(2, 1)});
		h.Run();                      // auto-diff scores the two engines' values
		h.ExpectFpr(2, r.result);
		// Run()'s diff does not cover fprc[31]; score each engine on the capture.
		EXPECT_EQ(h.JitSnapshot().fprs.fprc[31], r.fcr31)    << "arm64 JIT";
		EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31], r.fcr31) << "interpreter";
	}
}

// ----- SQRT.S rounds to nearest regardless of FCR31 mode -------------
// PS2 SQRT.S (like DIV.S) always rounds to nearest, independent of the
// configured EE rounding mode, so recSQRT_S swaps to the nearest-rounding
// FPUDivFPCR around the Fsqrt. sqrt(5) is rounding-sensitive: nearest
// 0x400F1BBD vs round-toward-zero 0x400F1BBC.
//
// The manual host-FPCR swap this used to carry is gone: the harness runs in the
// production environment, so Run() diffs the interpreter against the JIT here
// instead of leaving it unasserted. The randomized version, and DIV.S, are in
// ee_rec_fpu_divunit_rounding_tests.cpp.
TEST(EeRecFpu, SqrtSRoundsToNearestUnderChopFpcr)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprSingle(1, 5.0f);
	h.LoadProgram({ee::SQRT_S(2, 1)});
	h.Run();                            // auto-diff, in the production FP env
	h.ExpectFpr(2, 0x400F1BBDu);        // nearest-rounded sqrt(5)
}

// ----- RSQRT.S sticky flags (native) ----------------------------------
//
// RSQRT.S sets D|SD when the divisor Ft is zero and I|SI when Ft is negative
// (interp RSQRT_S, FPU.cpp), and its Ft==0 branch returns +/-posFmax keyed off
// the Ft sign. The op is native (recRSQRT_S_xmm); these two cases have exact
// results so they stay differential. Assert the flag bits directly (Run()
// doesn't diff fprc[31]). Broad coverage lives in ee_rec_fpu_rsqrt_tests.cpp.
TEST(EeRecFpu, RsqrtSZeroDivisorSetsDenormFlags)
{
	// fs = 1.0, ft = +0. The two tiers saturate differently -- see
	// RunDivBothTiers above -- so the legs run separately.
	const DivTiers t = RunDivBothTiers(ee::RSQRT_S(3, 1, 2), 0x3F800000u, 0x00000000u);
	EXPECT_EQ(t.jit, 0x7F7FFFFFu)    << "fast path saturates at FLT_MAX";
	EXPECT_EQ(t.interp, 0x7FFFFFFFu) << "interp saturates at the EE maximum";
	const u32 mask = 0x20000u | 0x10000u | 0x40u | 0x20u;   // I | D | SI | SD
	EXPECT_EQ(t.jit_fcr & mask,    0x10000u | 0x20u);  // D|SD set
	EXPECT_EQ(t.interp_fcr & mask, 0x10000u | 0x20u);
}

TEST(EeRecFpu, RsqrtSNegativeDivisorSetsInvalidFlags)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 2.0f);             // fs (dividend)
	h.SetFpr(2, -4.0f);            // ft (divisor) negative
	h.LoadProgram({
		ee::RSQRT_S(3, 1, 2),     // fd=3; result 2/sqrt(4)=1.0
	});
	h.Run();
	h.ExpectFpr(3, 0x3F800000u);   // 1.0f
	const u32 mask = 0x20000u | 0x10000u | 0x40u | 0x20u;
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & mask,    0x20000u | 0x40u);  // I|SI set
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & mask, 0x20000u | 0x40u);
}

TEST(EeRecFpu, CEqSTrueSetsCc)
{
	// Pre: set fpr1 = fpr2 = 5.0. Expect FCR31.CC (bit 23) set to 1.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 5.0f);
	h.SetFpr(2, 5.0f);
	h.LoadProgram({
		ee::C_EQ_S(1, 2),
	});
	h.Run();
	// Assert both sides: Run()'s auto-diff does not gate on fprc[31], so a JIT
	// that never updates FCR31.CC would pass an interp-only assert.
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (1u << 23), 0u);
	EXPECT_NE(h.InterpSnapshot().fprs.fprc[31] & (1u << 23), 0u);
}

TEST(EeRecFpu, CEqSFalseClearsCc)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(1u << 23);                  // pre-set CC to 1
	h.SetFpr(1, 5.0f);
	h.SetFpr(2, 6.0f);
	h.LoadProgram({
		ee::C_EQ_S(1, 2),
	});
	h.Run();
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & (1u << 23), 0u);
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & (1u << 23), 0u);
}

// ----- compare-operand clamping --------------------------------------
//
// The PS2 FPU has no Inf and no NaN, so those bit patterns are ordinary numbers
// to it: 0x7F800000 is 2^128, 0x7FC00000 is 1.5*2^128, 0x7FFFFFFF is the
// largest number the machine has. The two tiers answer differently, by design:
// the interpreter compares the real values and matches silicon on all 86
// compare rows of the capture, the arm64 fast path clamps both operands to
// +-FLT_MAX, sign-preserving, and misses four of them. All 86 rows and all
// three tiers are in ee_fpu_compare_console_tests.cpp.
//
// Inject raw Inf/NaN via SetFprBits (MTC1/LWC1 bit-copies bypass the arithmetic
// clamp in real games). Run()'s auto-diff does not gate on fprc[31], so each
// test asserts the two snapshots separately.

// Both tiers agree here for different reasons: 0xFFC00000 is -1.5*2^128 to the
// interpreter and -FLT_MAX to the fast path, and 0 is greater than both.
// Sign preservation is the fast path's part: a raw Fcmp on the NaN goes
// unordered, where ARM's "lt" (N!=V) is true, so without the clamp CC is
// wrongly set. A sign-stripping clamp (-NaN -> +FLT_MAX, which fpuClampResult
// or Fminnm would give) would also wrongly set CC (0 < +FLT_MAX), so this case
// pins the sign-preserving SMIN/UMIN path. Console rows 446/456 are the same
// shape with representable operands.
TEST(EeRecFpu, CLtSZeroVsNegativeNaNIsFalse)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(2, 0.0f);
	h.SetFprBits(1, 0xFFC00000u);   // -1.5*2^128 (interp) / -FLT_MAX (fast path)
	h.LoadProgram({
		ee::C_LT_S(2, 1),           // 0 < a large negative -> CC clear
	});
	h.Run();
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & (1u << 23), 0u);
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & (1u << 23), 0u);
}

// Equal bit patterns are equal on every tier and on silicon: console row 437
// (`ceq QNAN, QNAN`) is this pair and says CC set. It would pass with the clamp
// gone too, so it pins nothing about clamping.
TEST(EeRecFpu, CEqSPositiveNaNIsEqualToItself)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x7FC00000u);   // 1.5*2^128
	h.SetFprBits(2, 0x7FC00000u);
	h.LoadProgram({
		ee::C_EQ_S(1, 2),           // [fpm] case 437 -- CC set on silicon
	});
	h.Run();
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (1u << 23), 0u);
	EXPECT_NE(h.InterpSnapshot().fprs.fprc[31] & (1u << 23), 0u);
}

// Where the two tiers part company. Console row 434 is these operands and says
// the condition is false: 2^128 and 0x7FFFFFFF are different numbers on a
// machine whose exponent 255 is an ordinary binade. The fast path's CC set is
// asserted rather than fixed, for the reason in the block comment above.
TEST(EeRecFpu, CEqSTopBinadeSplitsTheInterpFromTheFastPath)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x7F800000u);   // 2^128
	h.SetFprBits(2, 0x7FFFFFFFu);   // the EE's largest number
	h.LoadProgram({
		ee::C_EQ_S(1, 2),           // [fpm] case 434 -- CC clear on silicon
	});
	h.Run();
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (1u << 23), 0u)
		<< "the fast path stopped clamping; if that was deliberate, this test "
		   "and ee_fpu_compare_console_tests.cpp both want updating";
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & (1u << 23), 0u)
		<< "the interpreter must give the console's answer, not the clamp's";
}

TEST(EeRecFpu, Bc1tTakenWhenCcSet)
{
	// Layout:
	//   0x00: C.EQ.S   fpr1, fpr2        — equal → CC = 1
	//   0x04: BC1T     +3                — taken, delay+3 words to target
	//   0x08: NOP      delay slot
	//   0x0C: ADDIU v0, zero, 1          — not-taken marker
	//   0x10: J park; NOP; NOP
	//   0x1C: ADDIU v0, zero, 2          — taken target
	//   0x20: J park; NOP
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 5.0f);
	h.SetFpr(2, 5.0f);
	h.LoadProgramNoTerm({
		ee::C_EQ_S(1, 2),
		ee::BC1T(3),
		NOP,
		ADDIU(reg::v0, reg::zero, 1), J(kPark), NOP, NOP,
		ADDIU(reg::v0, reg::zero, 2), J(kPark), NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 2ull);
}

TEST(EeRecFpu, Bc1fTakenWhenCcClear)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 5.0f);
	h.SetFpr(2, 6.0f);
	h.LoadProgramNoTerm({
		ee::C_EQ_S(1, 2),                 // 5 != 6 → CC = 0
		ee::BC1F(3),                      // taken
		NOP,
		ADDIU(reg::v0, reg::zero, 1), J(kPark), NOP, NOP,
		ADDIU(reg::v0, reg::zero, 2), J(kPark), NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 2ull);
}

// The not-taken edge exercises the forward-skip test-bit-and-branch
// (Tbz/Tbnz on fprc[31] bit 23). The "Taken" tests above only prove the skip
// does NOT fire; these prove it fires in the right direction.
TEST(EeRecFpu, Bc1tNotTakenWhenCcClear)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 5.0f);
	h.SetFpr(2, 6.0f);                     // 5 != 6 → CC = 0
	h.LoadProgramNoTerm({
		ee::C_EQ_S(1, 2),
		ee::BC1T(3),                       // CC clear → not taken (skip fires)
		NOP,
		ADDIU(reg::v0, reg::zero, 1), J(kPark), NOP, NOP,
		ADDIU(reg::v0, reg::zero, 2), J(kPark), NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 1ull);
}

TEST(EeRecFpu, Bc1fNotTakenWhenCcSet)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 5.0f);
	h.SetFpr(2, 5.0f);                     // 5 == 5 → CC = 1
	h.LoadProgramNoTerm({
		ee::C_EQ_S(1, 2),
		ee::BC1F(3),                       // CC set → not taken (skip fires)
		NOP,
		ADDIU(reg::v0, reg::zero, 1), J(kPark), NOP, NOP,
		ADDIU(reg::v0, reg::zero, 2), J(kPark), NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 1ull);
}

// ===========================================================================
//  FPU accumulator family — ADDA / SUBA / MULA write to ACC (no Fd field).
//  MADD / MSUB combine multiplication with ACC for Fd. MADDA / MSUBA do the
//  same but write back to ACC. The PS2 ISA mandates two separate roundings
//  (mul then add/sub) — these are NOT fused FMA.
// ===========================================================================

TEST(EeRecFpu, AddaSWritesAccumulator)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetAcc(99.0f);                       // pre-state: ACC should be overwritten, not accumulated
	h.LoadProgram({ee::ADDA_S(1, 2)});
	h.Run();
	h.ExpectAcc(FloatBits(7.0f));
}

TEST(EeRecFpu, SubaSWritesAccumulator)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 10.0f);
	h.SetFpr(2, 3.0f);
	h.SetAcc(99.0f);
	h.LoadProgram({ee::SUBA_S(1, 2)});
	h.Run();
	h.ExpectAcc(FloatBits(7.0f));
}

TEST(EeRecFpu, MulaSWritesAccumulator)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 6.0f);
	h.SetFpr(2, 7.0f);
	h.SetAcc(99.0f);
	h.LoadProgram({ee::MULA_S(1, 2)});
	h.Run();
	h.ExpectAcc(FloatBits(42.0f));
}

TEST(EeRecFpu, MaddSAddsProductToAccumulator)
{
	// fd = ACC + fs * ft = 10 + 3*4 = 22
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAcc(10.0f);
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({ee::MADD_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, FloatBits(22.0f));
}

TEST(EeRecFpu, MsubSSubtractsProductFromAccumulator)
{
	// fd = ACC - fs * ft = 100 - 3*4 = 88
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAcc(100.0f);
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({ee::MSUB_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, FloatBits(88.0f));
}

TEST(EeRecFpu, MaddaSAccumulatesIntoAccumulator)
{
	// ACC = ACC + fs * ft = 10 + 3*4 = 22; Fd field is not used
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAcc(10.0f);
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({ee::MADDA_S(1, 2)});
	h.Run();
	h.ExpectAcc(FloatBits(22.0f));
}

TEST(EeRecFpu, MsubaSSubtractsProductFromAccumulator)
{
	// ACC = ACC - fs * ft = 100 - 3*4 = 88
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAcc(100.0f);
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({ee::MSUBA_S(1, 2)});
	h.Run();
	h.ExpectAcc(FloatBits(88.0f));
}

// ----- MADDA/MSUBA must NOT clamp the intermediate product -----------
//
// A product clamped to +-fMax cancels against an opposite-signed ACC (-> 0)
// instead of overflowing the accumulate (-> +-fMax), and these cases are chosen
// so that only the unclamped behaviour survives. Neither engine clamps it any
// more; the interpreter rounds the product to an EE single and stops there if
// it overflowed (eeMulAccumulate).
// The next four all turn on the raw product reaching the accumulator as Inf,
// which only happens at round-to-nearest: round-toward-zero, the production
// mode, rounds an overflowing product to +/-FLT_MAX instead. Under that mode
// -fMax + (+fMax) = 0 on both engines -- indistinguishable from the clamped
// behaviour these tests exist to rule out, so their subject genuinely does not
// exist there. Tagged rather than deleted because the extra-overflow clamp mode
// they contrast with is still live code.
// The two engines therefore land a binade apart, +-0x7FFFFFFF against +-fMax,
// which Run()'s auto-diff cannot express -- hence the separate legs. Both are
// asserted: the fast path's is what games see, the interpreter's is the
// console's (capture rows 549 and 563).
TEST(EeRecFpu, MaddaSDoesNotClampIntermediateProduct)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest};
	u32 jit_acc, interp_acc;
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0xFF7FFFFFu); // ACC = -fMax
		h.SetFpr(1, 1e20f);
		h.SetFpr(2, 1e20f);        // fs*ft overflows single precision
		h.LoadProgram({ee::MADDA_S(1, 2)});
		h.RunJitNoDiff();
		jit_acc = h.GetAccBitsJit();
	}
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0xFF7FFFFFu);
		h.SetFpr(1, 1e20f);
		h.SetFpr(2, 1e20f);
		h.LoadProgram({ee::MADDA_S(1, 2)});
		h.RunInterpOnly();
		interp_acc = h.GetAccBitsInterp();
	}
	// JIT: -fMax + overflow -> +fMax. With the product clamped it would be
	// -fMax + (+fMax) = 0, which is what this test exists to rule out.
	EXPECT_EQ(jit_acc, 0x7F7FFFFFu) << "the fast path clamped the product";
	EXPECT_EQ(interp_acc, 0x7FFFFFFFu) << "the interpreter must saturate at EEMAX";
}

TEST(EeRecFpu, MsubaSDoesNotClampIntermediateProduct)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest}; // see MaddaSDoesNotClampIntermediateProduct
	u32 jit_acc, interp_acc;
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0x7F7FFFFFu); // ACC = +fMax
		h.SetFpr(1, 1e20f);
		h.SetFpr(2, 1e20f);        // fs*ft overflows single precision
		h.LoadProgram({ee::MSUBA_S(1, 2)});
		h.RunJitNoDiff();
		jit_acc = h.GetAccBitsJit();
	}
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0x7F7FFFFFu);
		h.SetFpr(1, 1e20f);
		h.SetFpr(2, 1e20f);
		h.LoadProgram({ee::MSUBA_S(1, 2)});
		h.RunInterpOnly();
		interp_acc = h.GetAccBitsInterp();
	}
	// JIT: +fMax - overflow -> -fMax. With the product clamped: +fMax - fMax = 0.
	EXPECT_EQ(jit_acc, 0xFF7FFFFFu) << "the fast path clamped the product";
	EXPECT_EQ(interp_acc, 0xFFFFFFFFu) << "the interpreter must saturate at -EEMAX";
}

// ----- CHECK_FPU_EXTRA_OVERFLOW source clamp --------------------------
//
// With the extra-overflow gate on (per-game clampMode>=2), the PS2 FPU recs
// clamp each fpr SOURCE to +-fMax before the op, matching interp fpuDouble and
// x86 recCommutativeOp/recMADDtemp (fpuFloat2). The divergence needs a poisoned
// fpr (raw Inf/NaN bits, reachable via MOV.S/LWC1/MTC1) AND fs*ft -> NaN: e.g.
// Inf*0. Without the input clamp the JIT computes Inf*0 = NaN and the result
// clamp folds it to +fMax; interp clamps Inf->fMax first, so fMax*0 = 0.
// Run()'s auto-diff plus ExpectFpr both pin JIT to interp.
TEST(EeRecFpu, MulSExtraOverflowClampsInfOperand)
{
	FpuExtraOverflowGuard guard;
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x7F800000u); // +Inf raw bits (poisoned fpr)
	h.SetFpr(2, 0.0f);
	h.LoadProgram({ee::MUL_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, 0x00000000u); // clamp(+Inf)*0 = +0; without input clamp -> +fMax
}

TEST(EeRecFpu, MaddSExtraOverflowClampsInfOperand)
{
	FpuExtraOverflowGuard guard;
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAcc(5.0f);
	h.SetFprBits(1, 0x7F800000u); // +Inf raw bits
	h.SetFpr(2, 0.0f);
	h.LoadProgram({ee::MADD_S(3, 1, 2)});
	h.Run();
	// fd = ACC + clamp(+Inf)*0 = 5 + 0 = 5; without input clamp: 5 + (Inf*0=NaN->fMax) -> +fMax
	h.ExpectFpr(3, FloatBits(5.0f));
}

TEST(EeRecFpu, SqrtSPositiveValue)
{
	// PS2 SQRT.S takes sqrt of |ft|; argument is Ft, NOT Fs.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(2, 16.0f);
	h.LoadProgram({ee::SQRT_S(3, 2)});
	h.Run();
	h.ExpectFpr(3, FloatBits(4.0f));
}

TEST(EeRecFpu, SqrtSNegativeArgumentReturnsAbsRoot)
{
	// SQRT.S of a negative value returns sqrt(|ft|) (no NaN) — PS2 quirk.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(2, -25.0f);
	h.LoadProgram({ee::SQRT_S(3, 2)});
	h.Run();
	h.ExpectFpr(3, FloatBits(5.0f));
}

// ----- MAX.S / MIN.S do not clamp their operands, in any mode ---------------
//
// This block used to assert the opposite, and the reversal is worth recording.
//
// The claim was that x86 recCommutativeOp clamps MAX/MIN operands
// (sign-preserving inf/NaN -> ±fMax via fpuFloat2) whenever CHECK_FPU_OVERFLOW,
// that AetherSX2's arm64 rec gates the same clamp on fpuOverflow, and that a
// raw Inf/NaN FPR (reachable via MOV.S/LWC1/MTC1) otherwise survived the
// NaN-eating Fmaxnm/Fminnm as the wrong finite value — the True Crime: New York
// City rainbow. All of that is accurate about those two emulators. It is not
// accurate about the console, and the tests here concluded from it that "interp
// is not the FPU-clamp oracle, the x86 JIT is".
//
// The SCPH-90000 capture says otherwise, on all 132 of its MAX/MIN cases:
// MAX.S/MIN.S are bit SELECTION. The console orders the two raw words by
// (sign, magnitude) and writes the winner through untouched — exactly the
// interpreter's fp_max/fp_min, and exactly what the DOUBLE tier already did.
// Exponent 255 is an ordinary binade there, so +2^128 is a real number that
// wins a max rather than something to be clamped away.
//
// So the clamp came out (recMINMAX, iFPU-arm64.cpp) and the JIT now agrees with
// the interpreter and with silicon in every clamp mode. The True Crime idiom is
// still safe: min(max(uv,0),size) with a pseudo-inf uv now yields the pseudo-inf
// out of the max, which is what the console yields.
//
// Full coverage — the 54 distinct console triples, the aliased register forms,
// and all four clamp modes — lives in ee_fpu_minmax_console_tests.cpp. What is
// kept here is the mode axis of the old claim, inverted: the answer is the same
// in modes 0, 1 and 2, so no clamp fires in any of them.
TEST(EeRecFpu, MaxMinDoNotClampOperandsInAnyClampMode)
{
	struct Row { u32 insn; u32 fs_bits; float ft; u32 want; const char* what; };
	const Row rows[] = {
		{ee::MAX_S(3, 1, 2), 0x7F800000u,  3.0f, 0x7F800000u, "max(+2^128, 3)"},
		{ee::MAX_S(3, 1, 2), 0x7FC00000u, -5.0f, 0x7FC00000u, "max(exp255 qNaN pattern, -5)"},
		{ee::MIN_S(3, 1, 2), 0xFFC00000u,  5.0f, 0xFFC00000u, "min(-exp255 qNaN pattern, 5)"},
		{ee::MIN_S(3, 1, 2), 0x7F800000u,  3.0f, 0x40400000u, "min(+2^128, 3) = 3"},
	};

	int checked = 0;
	for (int mode = 0; mode <= 2; ++mode)
	{
		FpuClampModeGuard guard(mode);
		for (const Row& r : rows)
		{
			SCOPED_TRACE(testing::Message() << r.what << " [eeClampMode " << mode << "]");
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFprBits(1, r.fs_bits);
			h.SetFpr(2, r.ft);
			h.LoadProgram({r.insn});
			// The interpreter agrees on every row, so this could use the
			// auto-diffing Run(); RunJitNoDiff is kept so a future interpreter
			// regression cannot mask a JIT one.
			h.RunJitNoDiff();
			EXPECT_EQ(h.GetFprBitsJit(3), r.want);
			++checked;
		}
	}
	EXPECT_EQ(checked, 3 * 4) << "anti-vacuity";
}

// ===========================================================================
//  Allocator-state interaction patterns — consecutive ops sharing operands.
//  The allocator path keeps operands in NEON across opcodes; an aliasing or
//  writeback bug surfaces here, not in the single-op tests above.
// ===========================================================================

TEST(EeRecFpu, AddSChainSameSourceTwice)
{
	// f3 = f1 + f2; then f4 = f3 + f2 (f2 re-used, allocator should keep it live)
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 1.0f);
	h.SetFpr(2, 2.0f);
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),
		ee::ADD_S(4, 3, 2),
	});
	h.Run();
	h.ExpectFpr(3, FloatBits(3.0f));
	h.ExpectFpr(4, FloatBits(5.0f));
}

TEST(EeRecFpu, AddSWriteSameAsRead)
{
	// f1 = f1 + f2 — destination aliases source.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 10.0f);
	h.SetFpr(2, 5.0f);
	h.LoadProgram({ee::ADD_S(1, 1, 2)});
	h.Run();
	h.ExpectFpr(1, FloatBits(15.0f));
}

TEST(EeRecFpu, AddaThenMaddChain)
{
	// Common geometry pattern: ADDA.S sets up ACC; MADD.S reads it.
	// ACC = f1 + f2 = 7; fd = ACC + f3*f4 = 7 + 6 = 13
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetFpr(3, 2.0f);
	h.SetFpr(4, 3.0f);
	h.LoadProgram({
		ee::ADDA_S(1, 2),
		ee::MADD_S(5, 3, 4),
	});
	h.Run();
	h.ExpectAcc(FloatBits(7.0f));
	h.ExpectFpr(5, FloatBits(13.0f));
}

TEST(EeRecFpu, MaddaChainAccumulates)
{
	// MULA.S then MADDA.S — common dot-product / vertex transform pattern.
	// ACC = f1*f2 = 6; ACC += f3*f4 = 6 + 20 = 26
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 2.0f);
	h.SetFpr(2, 3.0f);
	h.SetFpr(3, 4.0f);
	h.SetFpr(4, 5.0f);
	h.LoadProgram({
		ee::MULA_S(1, 2),
		ee::MADDA_S(3, 4),
	});
	h.Run();
	h.ExpectAcc(FloatBits(26.0f));
}

// ---------------------------------------------------------------------------
// MTC1 / MFC1 — allocator coherence with ADD_S-resident FPRs.
//
// ADD_S (routed through eeFPURecompileCode) leaves its destination FPR live
// in a NEON slot (MODE_WRITE) until block-end flush. MTC1 and MFC1 still
// bypass the allocator and go straight through memory at &fpuRegs.fpr[fs], so:
//   - MFC1 after ADD_S reads stale fpr[fs] from memory.
//   - MTC1 before block-end has its memory write clobbered when the
//     allocator flushes the stale-but-now-live NEON slot.
// ---------------------------------------------------------------------------

TEST(EeRecFpu, AddSThenMfc1ReadsAllocatorLiveResult)
{
	// ADD_S writes f3 (allocator-resident), MFC1 reads bits(f3) -> a0.
	// MFC1 must see 7.0f, not the pre-test memory contents of fpr[3].
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetFprBits(3, 0xDEADBEEFu);   // poison memory so a stale read is obvious
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),
		ee::MFC1(reg::a0, 3),
	});
	h.Run();
	h.ExpectFpr(3, FloatBits(7.0f));
	h.ExpectGpr64(reg::a0, static_cast<u64>(static_cast<s64>(static_cast<s32>(FloatBits(7.0f)))));
}

TEST(EeRecFpu, Mtc1ThenAddSUsesFreshSource)
{
	// MTC1 writes fpr[1] from a0; ADD_S then reads f1.
	// If the allocator had cached f1 from a SetFpr-time prefetch (or any
	// earlier block), ADD_S would read the stale value rather than the
	// MTC1 result.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetGpr64(reg::a0, FloatBits(10.0f));
	h.SetFpr(1, 99.0f);            // pre-state: f1 = 99 in memory
	h.SetFpr(2, 4.0f);
	h.LoadProgram({
		ee::MTC1(reg::a0, 1),       // f1 <- bits(10.0)
		ee::ADD_S(3, 1, 2),         // f3 = f1 + f2 = 14
	});
	h.Run();
	h.ExpectFpr(3, FloatBits(14.0f));
}

TEST(EeRecFpu, Mtc1AfterAddSOverwritesAllocatorCachedFpr)
{
	// ADD_S leaves f3 live in the allocator. MTC1 then writes f3 in
	// memory only. Block-end flush must NOT clobber the MTC1 write with
	// the stale-but-allocator-held ADD_S result.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetGpr64(reg::a0, FloatBits(123.0f));
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),         // f3 = 7 (allocator)
		ee::MTC1(reg::a0, 3),       // f3 = 123 (memory)
	});
	h.Run();
	h.ExpectFpr(3, FloatBits(123.0f));
}

TEST(EeRecFpu, Mtc1ThenAddSReadsMtc1Value)
{
	// ADD_S writes f3 first (so f3 is allocator-resident), then MTC1
	// updates f3 in memory, then a SECOND ADD_S consumes f3. The second
	// ADD_S must see the MTC1 value, not the cached allocator value.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetGpr64(reg::a0, FloatBits(100.0f));
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),         // f3 = 7 (allocator-live)
		ee::MTC1(reg::a0, 3),       // f3 := 100 (memory)
		ee::ADD_S(4, 3, 2),         // f4 = f3 + f2; must read 100, not 7
	});
	h.Run();
	h.ExpectFpr(4, FloatBits(104.0f));
}

// ---------------------------------------------------------------------------
// Direct-memory ops that bypass the FPR allocator — must flush dirty NEON
// slots before reading and invalidate on writes. Each test pairs an ADD_S
// (writes live to allocator) with the op being tested.
// ---------------------------------------------------------------------------

TEST(EeRecFpu, MovSAfterAddSPropagatesLiveValue)
{
	// ADD_S writes f3; MOV_S f4 = f3. MOV_S goes through memory copy.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetFprBits(4, 0xCAFEBABEu);
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),
		ee::MOV_S(4, 3),
	});
	h.Run();
	h.ExpectFpr(4, FloatBits(7.0f));
}

// MFC1 reading an FPR that a preceding ADD_S left allocator-resident must read
// the live value straight from the host reg (the resident-read fast path),
// sign-extended into rt. f3=7.0 -> v0 = 0x0000000040E00000.
TEST(EeRecFpu, Mfc1AfterAddSReadsResidentValue)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),         // f3 = 7 (allocator-resident)
		ee::MFC1(reg::v0, 3),       // v0 = sign_extend(bits(f3))
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 0x0000000040E00000ull);   // 7.0f bits, +ve sign
}

TEST(EeRecFpu, CEqAfterAddSReadsLiveOperand)
{
	// ADD_S writes f3 = 7; C_EQ_S f3, f4 (f4 = 7) -> CC should set.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetFpr(4, 7.0f);
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),
		ee::C_EQ_S(3, 4),
	});
	h.Run();
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (1u << 23), 0u);
	EXPECT_NE(h.InterpSnapshot().fprs.fprc[31] & (1u << 23), 0u);
}

TEST(EeRecFpu, CltAfterAddSReadsLiveOperand)
{
	// ADD_S writes f3 = 7; C_LT_S f3, f4 (f4 = 10) -> CC should set.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetFpr(4, 10.0f);
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),
		ee::C_LT_S(3, 4),
	});
	h.Run();
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (1u << 23), 0u);
	EXPECT_NE(h.InterpSnapshot().fprs.fprc[31] & (1u << 23), 0u);
}

TEST(EeRecFpu, CvtWAfterAddSReadsLiveSource)
{
	// ADD_S writes f3 = 7.0; CVT_W_S f4 = (int)f3 = 7.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),
		ee::CVT_W_S(4, 3),
	});
	h.Run();
	h.ExpectFpr(4, 7u);  // int bits, not float bits
}

TEST(EeRecFpu, DivSAfterAddSReadsLiveOperands)
{
	// ADD_S writes f3 = 20.0; DIV_S f4 = f3 / f1 = 20 / 4 = 5.
	// DIV_S is natively emitted via eeFPURecompileCode (recDIV_S_xmm), which
	// must read the allocator-live operands rather than stale memory.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 16.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),       // f3 = 20
		ee::DIV_S(4, 3, 2),       // f4 = f3 / f2 = 5
	});
	h.Run();
	h.ExpectFpr(4, FloatBits(5.0f));
}

// ---- FpuMulHack (Tales of Destiny Remake gamefix) --------------------------
// JIT-only: the interpreter MUL_S has no hack, so a hack-hit legitimately
// diverges from interp — assert GetFprBitsJit() under RunJitNoDiff(). The hack
// patches exactly 0.25 * (π) (0x3e800000 * 0x40490fdb) to 0x3f490fda. The
// shared emitFpuMul helper wires it into all of MUL/MULA/MADD/MSUB/MADDA/MSUBA.

TEST(EeRecFpu, MulSFpuMulHackPatchesMagicProduct)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuMulHack();
	h.SetFprBits(0, 0x3e800000u);
	h.SetFprBits(1, 0x40490fdbu);
	h.LoadProgram({ee::MUL_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x3f490fdau);
}

TEST(EeRecFpu, MulSFpuMulHackOffStillReachesTheConsoleValueOnInterp)
{
	// This test used to assert the opposite -- that with the gamefix off the
	// "ordinary" product comes out, and that JIT and interp agree on it. Its
	// premise was that the IEEE product is the console's. It is not.
	//
	// Measured on SCPH-90000 (captures/fpmul/): mul.s of 0x3E800000 by
	// 0x40490FDB returns 0x3F490FDA, and the same two words in the reverse
	// operand order return 0x3F490FDB. FpuMulHack is a one-point sample of the
	// multiplier's own one-ULP deficit, not a game kludge -- which is why it
	// compares fs and ft against their own constants and so does not fire
	// reversed, exactly as the console behaves.
	//
	// The interpreter models the deficit itself (eeMulRound in FPU.cpp), so it
	// lands on the console value with the gamefix off. The single-precision fast
	// path does not, so the two legitimately diverge here and this cannot be a
	// Run() diff -- and RunJitNoDiff() never runs the interpreter at all, so
	// each engine needs its own harness.
	EeRecTestHarness hi;
	hi.EnableCop1();
	hi.SetFprBits(0, 0x3e800000u);
	hi.SetFprBits(1, 0x40490fdbu);
	hi.LoadProgram({ee::MUL_S(2, 0, 1)});
	hi.RunInterpOnly();
	EXPECT_EQ(hi.GetFprBitsInterp(2), 0x3f490fdau) << "interp should reach silicon unaided";

	// Reversed: the console returns the un-decremented product, because the
	// predicate reads ft alone. This is the half that pins it as an operand
	// order effect rather than a constant fudge.
	EeRecTestHarness hr;
	hr.EnableCop1();
	hr.SetFprBits(0, 0x40490fdbu);
	hr.SetFprBits(1, 0x3e800000u);
	hr.LoadProgram({ee::MUL_S(2, 0, 1)});
	hr.RunInterpOnly();
	EXPECT_EQ(hr.GetFprBitsInterp(2), 0x3f490fdbu) << "reversed operands: no deficit";

	// The fast path, gamefix off, still produces the IEEE product. Recorded so
	// the divergence is pinned rather than discovered later as a surprise.
	EeRecTestHarness hj;
	hj.EnableCop1();
	hj.SetFprBits(0, 0x3e800000u);
	hj.SetFprBits(1, 0x40490fdbu);
	hj.LoadProgram({ee::MUL_S(2, 0, 1)});
	hj.RunJitNoDiff();
	EXPECT_EQ(hj.GetFprBitsJit(2), 0x3f490fdbu);
}

TEST(EeRecFpu, MulSMultiplierDeficitMatchesSilicon)
{
	// Rows measured directly on SCPH-90000. Each is a case where the exact
	// product is representable with nothing below the ULP, so the sub-ULP
	// deficit reaches the result. Corpus cases 857/1 are the 2.0 * FLT_MAX
	// pair -- 0x7FFFFFFF is an ordinary number on the EE and the exact product
	// lands on it, so that is a one-ULP defect and not saturation.
	struct Row { u32 fs, ft, want; };
	static const Row rows[] = {
		{0x3f800000u, 0x7f7fffffu, 0x7f7ffffeu}, // 1.0 * FLT_MAX  -> one ULP low
		{0x7f7fffffu, 0x3f800000u, 0x7f7fffffu}, // reversed       -> exact
		{0x40000000u, 0x7f7fffffu, 0x7ffffffeu}, // corpus case 857
		{0x7f7fffffu, 0x40000000u, 0x7fffffffu}, // corpus case 1
		{0x3f800000u, 0x3fc00000u, 0x3fc00000u}, // ft mantissa 0x400000: exact
		{0x3f800000u, 0x3f800001u, 0x3f800001u}, // ft mantissa 0x000001: exact
		{0x3f800000u, 0x3fbfffffu, 0x3fbffffeu}, // ft mantissa 0x3fffff: low
		{0x00800000u, 0x7f800001u, 0x40800001u}, // corpus case 876: exact
		{0x3e800000u, 0x40490fdbu, 0x3f490fdau}, // the FpuMulHack pair
		{0x40490fdbu, 0x3e800000u, 0x3f490fdbu}, // reversed       -> exact
	};
	for (const Row& r : rows)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(0, r.fs);
		h.SetFprBits(1, r.ft);
		h.LoadProgram({ee::MUL_S(2, 0, 1)});
		h.RunInterpOnly();
		EXPECT_EQ(h.GetFprBitsInterp(2), r.want)
			<< "mul.s fs=" << std::hex << r.fs << " ft=" << r.ft;
	}
}

TEST(EeRecFpu, MultiplierDeficitReachesTheWholeMultiplyFamily)
{
	// MUL/MULA round their product through eeMulRound and the four
	// multiply-accumulates reach the same helper through eeMulAccumulate.
	// ACC = +0 (or the MADD/MSUB accumuland) so what lands in the destination
	// is the rounded product alone.
	constexpr u32 kFs = 0x3f800000u; // 1.0: the product is ft, tail always zero
	constexpr u32 kFt = 0x3fbfffffu; // Booth fires
	constexpr u32 kWant = 0x3fbffffeu;

	struct Form { u32 word; bool is_acc; u32 want; const char* name; };
	static const Form forms[] = {
		{ee::MUL_S(2, 0, 1),  false, kWant,                 "MUL.S"},
		{ee::MULA_S(0, 1),    true,  kWant,                 "MULA.S"},
		{ee::MADD_S(2, 0, 1), false, kWant,                 "MADD.S"},
		{ee::MSUB_S(2, 0, 1), false, kWant ^ 0x80000000u,   "MSUB.S"},
		{ee::MADDA_S(0, 1),   true,  kWant,                 "MADDA.S"},
		{ee::MSUBA_S(0, 1),   true,  kWant ^ 0x80000000u,   "MSUBA.S"},
	};
	for (const Form& f : forms)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0x00000000u);
		h.SetFprBits(0, kFs);
		h.SetFprBits(1, kFt);
		h.LoadProgram({f.word});
		h.RunInterpOnly();
		const u32 got = f.is_acc ? h.GetAccBitsInterp() : h.GetFprBitsInterp(2);
		EXPECT_EQ(got, f.want) << f.name;
	}
}

TEST(EeRecFpu, MaddSFpuMulHackAppliesToProduct)
{
	// MADD routes its multiply through the same helper: ACC=0 + hack(Fs*Ft) ->
	// the patched product. Proves the family-wide wiring, not just MUL_S.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuMulHack();
	h.SetAccBits(0x00000000u); // +0
	h.SetFprBits(0, 0x3e800000u);
	h.SetFprBits(1, 0x40490fdbu);
	h.LoadProgram({ee::MADD_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x3f490fdau);
}

// ---- GE-11: MTC1 allocates a used destination FPR slot ---------------------
// MTC1 followed by consumers of fs must produce the same results whether fs
// went through the newly-allocated write-only slot (used-dest) or memory
// (unused dest). Chains through CVT.S and ADD.S pin the residency handoff.

TEST(EeRecFpu, Mtc1ThenCvtSThenAddSUsesResidentSlot)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetGpr64(reg::a0, 7);          // int bits for CVT.S
	h.SetFprBits(5, 0x3F800000u);    // f5 = 1.0f
	h.LoadProgram({
		ee::MTC1(reg::a0, 1),        // f1 = raw 7 (int bits) — dest USED below
		ee::CVT_S_W(2, 1),           // f2 = 7.0f
		ee::ADD_S(3, 2, 5),          // f3 = 8.0f
		ee::MFC1(reg::v0, 3),
	});
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::v0) & 0xFFFFFFFFull, 0x41000000ull); // 8.0f
}

TEST(EeRecFpu, Mtc1UnusedDestStoresToMemory)
{
	// fs never touched again in-block: the alloc-if-used gate must decline and
	// the value must still land in fpr memory for the post-state diff.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetGpr64(reg::a0, 0xDEADBEEFull);
	h.LoadProgram({
		ee::MTC1(reg::a0, 4),
	});
	h.Run();
	EXPECT_EQ(h.GetFprBitsInterp(4), 0xDEADBEEFu);
}

TEST(EeRecFpu, Mtc1ConstSourceAllocatesUsedDest)
{
	// Const-propagated rt (LUI) through the same alloc-if-used path.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(5, 0x40000000u);    // f5 = 2.0f
	h.LoadProgram({
		LUI(reg::a1, 0x4040),        // a1 = 0x40400000 = 3.0f bits (const)
		ee::MTC1(reg::a1, 1),        // f1 = 3.0f via const path
		ee::ADD_S(2, 1, 5),          // f2 = 5.0f
		ee::MFC1(reg::v0, 2),
	});
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::v0) & 0xFFFFFFFFull, 0x40A00000ull); // 5.0f
}

// ---- GE-13: DIV.S without operand copies — destination aliasing edges -------
// The restructured recDIV_S reads raw fs/ft directly; every zero-path sign
// read precedes the single fd write, so fd==fs / fd==ft must stay exact.

TEST(EeRecFpu, DivSAliasedDestEqualsSource)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x41200000u); // 10.0f
	h.SetFprBits(2, 0x40000000u); // 2.0f
	h.LoadProgram({ee::DIV_S(1, 1, 2), ee::MFC1(reg::v0, 1)}); // f1 = f1/f2 = 5.0
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::v0) & 0xFFFFFFFFull, 0x40A00000ull);
}

TEST(EeRecFpu, DivSAliasedDestEqualsDivisor)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x41200000u); // 10.0f
	h.SetFprBits(2, 0x40000000u); // 2.0f
	h.LoadProgram({ee::DIV_S(2, 1, 2), ee::MFC1(reg::v0, 2)}); // f2 = f1/f2 = 5.0
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::v0) & 0xFFFFFFFFull, 0x40A00000ull);
}

TEST(EeRecFpu, DivSByZeroAliasedDestSignFromBothOperands)
{
	// x/0 with fd==ft: the ±fMax result sign = sign(fs^ft), read from the RAW
	// operands — a premature fd write would corrupt the ft sign read.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0xC1200000u); // -10.0f
	h.SetFprBits(2, 0x80000000u); // -0.0f
	h.LoadProgram({ee::DIV_S(2, 1, 2), ee::MFC1(reg::v0, 2)}); // -10/-0 -> +max
	h.RunInterpOnly(); // the tiers saturate differently; the sign is the subject
	EXPECT_EQ(h.GetGpr64Interp(reg::v0) & 0xFFFFFFFFull, 0x7FFFFFFFull);
}

TEST(EeRecFpu, DivSZeroOverZeroAliasedDest)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(1, 0x00000000u); // +0.0f
	h.SetFprBits(2, 0x80000000u); // -0.0f
	h.LoadProgram({ee::DIV_S(1, 1, 2), ee::MFC1(reg::v0, 1)}); // 0/0 -> -max (sign fs^ft)
	h.RunInterpOnly(); // the tiers saturate differently; the sign is the subject
	EXPECT_EQ(h.GetGpr64Interp(reg::v0) & 0xFFFFFFFFull, 0xFFFFFFFFull);
}

// ---- GE-19: MADD/MSUB-family intermediate-product clamp mirrors the x86 JIT -
// Acceptance bar (user decision 2026-07-13): x86-JIT parity, NOT interp parity.
// x86 recMADDtemp/recMSUBtemp clamp the fs*ft product (and pre-add ACC) only
// under CHECK_FPU_EXTRA_OVERFLOW; in the default clamp mode the raw product
// rides to the accumulate as Inf and only the final result clamp applies.
//
// The interpreter changed on 2026-07-31: it used to clamp the product to
// +-fMax, so an overflowing one cancelled against an opposite-signed ACC and
// the corner came back 0. It now ends the instruction at the EE's own maximum,
// so the corner comes back +-0x7FFFFFFF -- the console's answer, on capture
// rows 521, 526, 535, 549, 554 and 563.
//
// The divergence below is by design: games are tuned against the fast path, and
// the fast path's value is the one that must not move.

TEST(EeRecFpu, MaddSProductOverflowDefaultModeMatchesX86Jit)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest}; // needs Inf -- see MaddaSDoesNotClampIntermediateProduct
	// Interp leg: the product overflows, saturates at the EE maximum and ends
	// the instruction. The console agrees (capture row 521 and friends).
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0xFF7FFFFFu);    // ACC = -fMax
		h.SetFprBits(1, 0x7F000000u); // 1.70141e38; product overflows float
		h.SetFprBits(2, 0x7F000000u);
		h.LoadProgram({ee::MADD_S(3, 1, 2)});
		h.RunInterpOnly();
		EXPECT_EQ(h.GetFprBitsInterp(3), 0x7FFFFFFFu)
			<< "the interpreter must saturate at the EE maximum, not at FLT_MAX "
			   "and not at the 0 an accumulated +fMax used to give";
	}
	// JIT leg (x86 parity): raw product +Inf → -fMax + Inf = +Inf → final
	// result clamp → +fMax. Intentionally != interp.
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0xFF7FFFFFu);
		h.SetFprBits(1, 0x7F000000u);
		h.SetFprBits(2, 0x7F000000u);
		h.LoadProgram({ee::MADD_S(3, 1, 2)});
		h.RunJitNoDiff();
		EXPECT_EQ(h.GetFprBitsJit(3), 0x7F7FFFFFu)
			<< "the fast path's saturation compromise must not move";
	}
}

TEST(EeRecFpu, MsubSProductOverflowDefaultModeMatchesX86Jit)
{
	const ScopedFpEnv fp_env{ScopedFpEnv::FlushNearest}; // needs Inf -- see MaddaSDoesNotClampIntermediateProduct
	// JIT (x86 parity): fd = +fMax - (+Inf) = -Inf → final clamp → -fMax.
	// (Interp clamps the product: +fMax - fMax = 0.)
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAccBits(0x7F7FFFFFu);    // ACC = +fMax
	h.SetFprBits(1, 0x7F000000u);
	h.SetFprBits(2, 0x7F000000u);
	h.LoadProgram({ee::MSUB_S(3, 1, 2)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(3), 0xFF7FFFFFu);
}

TEST(EeRecFpu, MaddSProductOverflowExtraModeClampsProduct)
{
	// Extra mode: x86 clamps the product pre-add → -fMax + fMax = 0. That used
	// to match the interpreter and Run() pinned both at once; the interpreter
	// now saturates at the EE maximum and ends the instruction there, so the
	// two legs are asserted separately. The JIT value is the subject: it is what
	// the extra-overflow gate exists to produce.
	FpuExtraOverflowGuard guard;
	u32 jit_bits, interp_bits;
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0xFF7FFFFFu);
		h.SetFprBits(1, 0x7F000000u);
		h.SetFprBits(2, 0x7F000000u);
		h.LoadProgram({ee::MADD_S(3, 1, 2)});
		h.RunJitNoDiff();
		jit_bits = h.GetFprBitsJit(3);
	}
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetAccBits(0xFF7FFFFFu);
		h.SetFprBits(1, 0x7F000000u);
		h.SetFprBits(2, 0x7F000000u);
		h.LoadProgram({ee::MADD_S(3, 1, 2)});
		h.RunInterpOnly();
		interp_bits = h.GetFprBitsInterp(3);
	}
	EXPECT_EQ(jit_bits, 0x00000000u)
		<< "extra mode must clamp the product pre-add, cancelling against -fMax";
	EXPECT_EQ(interp_bits, 0x7FFFFFFFu)
		<< "the interpreter does not have a product clamp to gate any more";
}

TEST(EeRecFpu, MaddaSProductOverflowExtraModeClampsProduct)
{
	// x86 serves MADDA through the same recMADDtemp, so extra mode clamps the
	// A-form product too: ACC = -fMax + clamp(+Inf) = 0. Interp MADDA adds the
	// raw product (no fpuDouble temp) → +fMax — divergence by design, mirror x86.
	FpuExtraOverflowGuard guard;
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAccBits(0xFF7FFFFFu);
	h.SetFprBits(1, 0x7F000000u);
	h.SetFprBits(2, 0x7F000000u);
	h.LoadProgram({ee::MADDA_S(1, 2)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetAccBitsJit(), 0x00000000u);
}

TEST(EeRecFpu, MsubaSProductOverflowExtraModeClampsProduct)
{
	// ACC = +fMax - clamp(+Inf) = 0 under extra mode (x86 recMSUBtemp).
	FpuExtraOverflowGuard guard;
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAccBits(0x7F7FFFFFu);
	h.SetFprBits(1, 0x7F000000u);
	h.SetFprBits(2, 0x7F000000u);
	h.LoadProgram({ee::MSUBA_S(1, 2)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetAccBitsJit(), 0x00000000u);
}

// (Default-mode A-form raw-product behavior is already pinned by
// MaddaSDoesNotClampIntermediateProduct / MsubaSDoesNotClampIntermediateProduct
// above — x86 and interp agree there, no extra test needed.)

// ---- GE-12: FCR31 block residency — cross-op coherence corners ---------------
// fprc[31] lives in a host GPR (ARM64TYPE_FPRC) across ops inside a block in
// the default clamp mode. Every architectural observer — CFC1 read, BC1
// branch, DIV/SQRT flag RMW, C-call seams, block end — must see the value the
// memory image would have carried. Run()'s auto-diff does not gate on
// fprc[31], so flag-value asserts go through JitSnapshot (see the DIV flag
// tests above).

TEST(EeRecFpu, CompareThenCfc1SeesFreshConditionBit)
{
	// C.LT writes the resident flag; CFC1 in the same block must read it back
	// through the resident copy (stale-memory read would miss the fresh bit).
	//
	// JIT-only assert: CFC1 emulates FCR31's fixed bits on the READ side
	// (And 0x0083c078 / Orr 0x01000001 — exact x86 recCFC1 shape), while the
	// interpreter returns the raw word. With fprc[31] starting at 0 the two
	// legitimately diverge (JIT 0x01800001 vs interp 0x00800000) — x86-JIT
	// parity is the bar (see the GE-19 block above), so pin the JIT value.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 1.0f);
	h.SetFpr(2, 2.0f);
	h.LoadProgram({
		ee::C_LT_S(1, 2),          // 1 < 2 → CC = 1
		ee::CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGpr64Jit(reg::v0), 0x01800001ull); // CC | always-one bits
}

TEST(EeRecFpu, Ctc1ThenBc1BranchesOnWrittenFlag)
{
	// CTC1 writes FCR31 (write-only resident slot); BC1T in the same block
	// must branch on the just-written bit.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetGpr64(reg::a0, 1u << 23);
	h.LoadProgramNoTerm({
		ee::CTC1(reg::a0, 31),
		ee::BC1T(3),
		NOP,
		ADDIU(reg::v0, reg::zero, 1), J(kPark), NOP, NOP,
		ADDIU(reg::v0, reg::zero, 2), J(kPark), NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 2ull);
}

TEST(EeRecFpu, CompareSurvivesInterposedGuardedAddCfc1)
{
	// GE-12 × guard-bit masking (17c4adb9e) interaction — the SotC glitch.
	//
	// The resident FCR31 lives in the ARM64TYPE_FPRC pool {x2-x7, x14, x15}
	// (iCore-arm64.cpp _allocArm64GPR: x0/x1/x28 are excluded precisely
	// because FPU/MULT emitters raw-clobber RWARG1/RWARG2). But
	// fpuEmitGuardedAddSub raw-clobbers RWARG3 (w2) on EVERY ADD/SUB-family
	// emit (Ubfx expd / Sub diff) and RWARG4 (w3) on the mask paths — and
	// x2/x3 ARE in the FPRC pool. _initArm64GPRregs resets the round-robin
	// cursor per block, so a block whose first int alloc is the FPU compare
	// deterministically parks FCR31 in x2 → any following guarded ADD.S in
	// the same block destroys the condition flag before BC1x/CFC1 reads it.
	//
	// Shape: C.LT (false → C=0, FCR31 resident) → ADD.S with |expdiff| ≥ 2
	// (both w2 and w3 end bit-23-set: w2 = negative diff, w3 = mask
	// 0xff..fc) → CFC1 must still read C=0.
	//
	// JIT-only assert: CFC1's fixed-bit emulation diverges from interp by
	// design (see CompareThenCfc1SeesFreshConditionBit above).
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 2.0f);
	h.SetFpr(2, 1.0f);
	h.SetFpr(4, 1.0f);            // exp 127
	h.SetFpr(5, 8.0f);            // exp 130 → diff = -3 → maskS path
	h.LoadProgram({
		ee::C_LT_S(1, 2),         // 2 < 1 → false → C = 0; FCR31 resident (x2)
		ee::ADD_S(3, 4, 5),       // guarded add: clobbers w2 (always) + w3 (mask path)
		ee::CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGpr64Jit(reg::v0), 0x01000001ull); // C clear + always-one bits
}

TEST(EeRecFpu, CompareSurvivesInterposedGuardedAddBc1)
{
	// Same clobber as CompareSurvivesInterposedGuardedAddCfc1 but observed
	// through the branch — the game-visible mechanism (compare → arithmetic →
	// conditional branch is the canonical FPU idiom): C=0, an interposed
	// guarded ADD.S leaves bit 23 set in the clobbered host reg, and BC1F
	// (must-take on C clear) falls through instead.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 2.0f);
	h.SetFpr(2, 1.0f);
	h.SetFpr(4, 1.0f);
	h.SetFpr(5, 8.0f);
	h.LoadProgramNoTerm({
		ee::C_LT_S(1, 2),         // false → C = 0
		ee::ADD_S(3, 4, 5),       // guarded add clobber
		ee::BC1F(3),              // C clear → must be taken
		NOP,
		ADDIU(reg::v0, reg::zero, 1), J(kPark), NOP, NOP,
		ADDIU(reg::v0, reg::zero, 2), J(kPark), NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 2ull);
}

TEST(EeRecFpu, CompareSurvivesInterposedMult)
{
	// Same bug class as CompareSurvivesInterposedGuardedAddCfc1, pre-existing
	// instance: the MULT/DIV emitters (iR5900MultDiv-arm64.cpp) raw-clobbered
	// w2 (loadRt32 fallback) and w3 (DIV remainder / MADD LO load) — both in
	// the FPRC pool. rt's value 0x00800000 lands bit 23 in the clobbered reg,
	// flipping a C=0 flag to set.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 2.0f);
	h.SetFpr(2, 1.0f);
	h.SetGpr64(reg::t0, 3);
	h.SetGpr64(reg::t1, 0x00800000ull); // bit 23 set — poison signature
	h.LoadProgram({
		ee::C_LT_S(1, 2),         // false → C = 0; FCR31 resident (x2)
		MULT(reg::t0, reg::t1),   // rt materialization must not touch x2/x3
		ee::CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGpr64Jit(reg::v0), 0x01000001ull); // C still clear
}

TEST(EeRecFpu, CompareSurvivesInterposedDiv)
{
	// DIV flavor: quotient/remainder of 0x00900000/0x00800001 both carry
	// bit-23-relevant garbage through the old w2/w3 scratches.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 2.0f);
	h.SetFpr(2, 1.0f);
	h.SetGpr64(reg::t0, 0x00900000ull);
	h.SetGpr64(reg::t1, 0x00800001ull);
	h.LoadProgram({
		ee::C_LT_S(1, 2),         // false → C = 0
		DIV(reg::t0, reg::t1),
		ee::CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGpr64Jit(reg::v0), 0x01000001ull);
}

TEST(EeRecFpu, Ctc1ThenDivByZeroAccumulatesStickyFlags)
{
	// CTC1 seeds sticky bits; DIV x/0 RMWs D|SD on the resident copy; the
	// block-end writeback must carry BOTH the seeded and the new bits.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetGpr64(reg::a0, 0x00000040u);       // SI sticky pre-seeded via CTC1
	h.SetFpr(1, 10.0f);
	h.SetFprBits(2, 0x00000000u);           // +0.0f divisor
	h.LoadProgram({
		ee::CTC1(reg::a0, 31),
		ee::DIV_S(3, 1, 2),                 // x/0 -> D|SD
	});
	// JIT-only: the flag writeback is the subject, and Run()'s auto-diff would
	// trip on the divide's saturation.
	h.RunJitNoDiff();
	const u32 mask = 0x00030060u;           // I|D|SI|SD
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & mask,    0x10000u | 0x20u | 0x40u); // D|SD + kept SI
}

TEST(EeRecFpu, ComparePreservesStickyFlagBits)
{
	// C.cond must touch ONLY bit 23 (single Bfi) — pre-seeded sticky I|D|SI|SD
	// bits must survive the compare and land in memory at block end.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0x00030060u);                // I|D|SI|SD all set
	h.SetFpr(1, 5.0f);
	h.SetFpr(2, 5.0f);
	h.LoadProgram({
		ee::C_EQ_S(1, 2),                   // sets CC, must not clear stickies
	});
	h.Run();
	const u32 want = 0x00030060u | (1u << 23);
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & (0x00030060u | (1u << 23)), want);
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & (0x00030060u | (1u << 23)), want);
}

TEST(EeRecFpu, CompareReadsWriteOnlyResidentOperand)
{
	// ADD.S leaves f3 NEON-resident MODE_WRITE-only; C.EQ must read the live
	// host value (authoritative), not stale fpr memory.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 4.0f);
	h.SetFpr(4, 7.0f);
	h.SetFprBits(3, 0xDEADBEEFu);           // stale memory image for f3
	h.LoadProgram({
		ee::ADD_S(3, 1, 2),                 // f3 = 7.0 (resident, dirty)
		ee::C_EQ_S(3, 4),                   // 7 == 7 → CC set
	});
	h.Run();
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (1u << 23), 0u);
	EXPECT_NE(h.InterpSnapshot().fprs.fprc[31] & (1u << 23), 0u);
}

// ---- GE-15: FPR-class NEON slots retained across C-helper seams --------------
// iFlushCall keeps 32-bit FPR/ACC slots in q10-q15 mapped (writeback-if-dirty)
// across plain C seams, and FLUSH_FREE_XMM vetoes retention where C code can
// touch fpr[]/ACC memory. These chains pin both directions.

TEST(EeRecFpu, FprValueCorrectAcrossLqSeam)
{
	// recLQ emits an unconditional iFlushCall(FLUSH_CONSTANT_REGS) — a
	// retention-eligible seam. The dirty f6 must survive it (retained or
	// reloaded, the VALUE must be right), and the quad load must be intact.
	EeRecTestHarness h;
	h.EnableCop1();
	h.WriteU64(RecompilerTestEnvironment::kScratchAddr + 0, 0x1122334455667788ull);
	h.WriteU64(RecompilerTestEnvironment::kScratchAddr + 8, 0x99AABBCCDDEEFF00ull);
	h.SetFprBits(4, 0x40000000u); // 2.0f
	h.SetFprBits(5, 0x3F800000u); // 1.0f
	h.SetGpr64(reg::a0, RecompilerTestEnvironment::kScratchAddr);
	h.LoadProgram({
		ee::ADD_S(6, 4, 5),          // f6 = 3.0f, dirty in a (preferably) q10-q15 slot
		ee::LQ(reg::t0, 0, reg::a0), // seam: iFlushCall(FLUSH_CONSTANT_REGS)
		ee::ADD_S(7, 6, 5),          // f7 = 4.0f from the surviving f6
	});
	h.Run();
	EXPECT_EQ(h.GetFprBitsInterp(6), 0x40400000u);
	EXPECT_EQ(h.GetFprBitsInterp(7), 0x40800000u);
	EXPECT_EQ(h.GetGpr64Interp(reg::t0), 0x1122334455667788ull);
}

TEST(EeRecFpu, InterpFallbackWriteInvalidatesRetainedFpr)
{
	// recRSQRT_S defers to the interpreter (FLUSH_INTERPRETER carries
	// FLUSH_FREE_XMM → retention VETO). The interp body REWRITES f7 in C;
	// a wrongly-retained dirty slot would either clobber the interp result
	// on writeback or feed the stale 4.0f into the final ADD.S.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 2.0f);
	h.SetFpr(2, 2.0f);
	h.SetFpr(5, 6.0f);
	h.SetFpr(6, 4.0f);
	h.LoadProgram({
		ee::ADD_S(7, 1, 2),          // f7 = 4.0f, dirty resident
		ee::RSQRT_S(7, 5, 6),        // interp: f7 = 6/sqrt(4) = 3.0f
		ee::ADD_S(8, 7, 1),          // f8 = 5.0f from interp's f7, not stale 4.0f
	});
	h.Run();
	EXPECT_EQ(h.GetFprBitsInterp(7), 0x40400000u); // 3.0f
	EXPECT_EQ(h.GetFprBitsInterp(8), 0x40A00000u); // 5.0f
}

TEST(EeRecFpu, MmiQuadNotRetainedAcrossSeam)
{
	// 128-bit NEONTYPE_GPRREG quads must still fully flush at every seam —
	// only their LOWER 64 bits survive a C call. Dirty quad → LQ seam →
	// quad consumer; a wrongly-retained quad risks garbage upper lanes.
	EeRecTestHarness h;
	h.EnableCop1();
	h.WriteU64(RecompilerTestEnvironment::kScratchAddr + 0, 5ull);
	h.WriteU64(RecompilerTestEnvironment::kScratchAddr + 8, 7ull);
	h.SetGpr128(reg::t1, 0x0000000100000002ull, 0x0000000300000004ull);
	h.SetGpr128(reg::t2, 0x0000001000000020ull, 0x0000003000000040ull);
	h.SetGpr64(reg::a0, RecompilerTestEnvironment::kScratchAddr);
	h.LoadProgram({
		ee::PADDW(reg::t3, reg::t1, reg::t2), // t3 quad dirty in NEON
		ee::LQ(reg::t0, 0, reg::a0),          // seam
		ee::PADDW(reg::v0, reg::t3, reg::t1), // consumes all 128 bits of t3
	});
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::t0), 5ull);
}

TEST(EeRecFpu, ConditionFlagSurvivesInterpFallbackSeam)
{
	// RSQRT.S defers to the interpreter (recCall → FLUSH_INTERPRETER), which
	// fully evicts the resident FCR31 slot and lets C code RMW fprc memory.
	// The C bit set before the seam must survive into the BC1T after it, and
	// the interp fallback's own flag writes must not be clobbered by a stale
	// resident copy. (interp RSQRT preserves bit 23; 4/1 sets no new flags.)
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 5.0f);
	h.SetFpr(2, 5.0f);
	h.SetFpr(5, 4.0f);
	h.SetFpr(6, 1.0f);
	h.LoadProgramNoTerm({
		ee::C_EQ_S(1, 2),                   // CC = 1 (resident)
		ee::RSQRT_S(7, 5, 6),               // interp fallback seam
		ee::BC1T(3),
		NOP,
		ADDIU(reg::v0, reg::zero, 1), J(kPark), NOP, NOP,
		ADDIU(reg::v0, reg::zero, 2), J(kPark), NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 2ull);
}
