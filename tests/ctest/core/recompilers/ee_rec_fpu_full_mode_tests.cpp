// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// FPU "Full" / DOUBLE-precision mode coverage (CHECK_FPU_FULL, GameDB
// eeClampMode:3 — FFX, Max Payne, Dark Cloud 2, Klonoa 2, ~150 serials).
//
// These are JIT-ONLY tests. The shared interpreter (FPU.cpp `fpuDouble`) is
// single-precision and has no double path, so it cannot be the oracle: for the
// inputs that exercise the DOUBLE path the JIT legitimately diverges from the
// interp. Each test therefore uses RunJitNoDiff() and asserts GetFprBitsJit()
// against an independently hand-computed PS2 double-mode result.
//
// CAUTION for future test authors: RunJitNoDiff() sets interp_snapshot_ =
// jit_snapshot_ (the interp is not a valid oracle here). So in THIS file the
// interp-side accessors mirror the JIT — an EXPECT against InterpSnapshot() or
// a both-sides h.ExpectFpr() would pass tautologically. Assert only via the
// *Jit() accessors (GetFprBitsJit / GetAccBitsJit / JitSnapshot).
//
// The discriminator between full and fast mode is a PS2 "pseudo-infinity"
// operand (exp field 0xff, e.g. 0x7f800000 = a finite 2^128-scale number):
// full mode preserves it as 0x7f800000 (ToDouble complex path -> op ->
// ToPS2FPU to_complex path), while the single-precision fast path treats it as
// +Inf and clamps it to 0x7f7fffff. The PseudoInf* tests pin that the DOUBLE
// dispatch is taken: the fast-path value would fail them.

#include "harness/EeRecTestHarness.h"

#include "Config.h"

#include <cstring>
#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {
u32 FloatBits(float f)
{
	u32 bits;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

constexpr u32 kFPUflagO  = 0x00008000;
constexpr u32 kFPUflagSO = 0x00000010;

// A PS2 single with exponent field 0xff is a valid finite number (1.0 * 2^128),
// not an IEEE infinity. Full mode must preserve it through an arithmetic op.
constexpr u32 kPs2HugePos = 0x7f800000; // +1.0 * 2^128
constexpr u32 kPs2MaxPos  = 0x7f7fffff; // +FLT_MAX (what the fast path clamps to)
} // namespace

// ---- Normal-range arithmetic: the DOUBLE pipeline must not corrupt ordinary
//      values (widen -> op -> narrow round-trips exactly for these). ----------

TEST(EeRecFpuFull, AddNormalRange)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(2.5f));
	h.SetFprBits(1, FloatBits(1.25f));
	h.LoadProgram({ADD_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(3.75f));
}

TEST(EeRecFpuFull, SubNormalRange)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(5.0f));
	h.SetFprBits(1, FloatBits(1.5f));
	h.LoadProgram({SUB_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(3.5f));
}

TEST(EeRecFpuFull, MulNormalRange)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(3.0f));
	h.SetFprBits(1, FloatBits(4.0f));
	h.LoadProgram({MUL_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(12.0f));
}

// ---- Pseudo-infinity preservation: the strip-fix discriminator. ------------

TEST(EeRecFpuFull, AddPseudoInfPreserved)
{
	// 0x7f800000 + 0.0 : full mode keeps the PS2 2^128 value; the single-prec
	// fast path would treat it as +Inf and clamp to +FLT_MAX (0x7f7fffff).
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kPs2HugePos);
	h.SetFprBits(1, FloatBits(0.0f));
	h.LoadProgram({ADD_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), kPs2HugePos);
	EXPECT_NE(h.GetFprBitsJit(2), kPs2MaxPos); // would be this on the fast path
}

TEST(EeRecFpuFull, SubPseudoInfPreserved)
{
	// 0x7f800000 - 0.0 : same preservation through the SUB path.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kPs2HugePos);
	h.SetFprBits(1, FloatBits(0.0f));
	h.LoadProgram({SUB_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), kPs2HugePos);
}

// ---- Overflow clamp + sticky flags: ToPS2FPU_Full to_overflow path. --------

TEST(EeRecFpuFull, MulOverflowClampsAndSetsStickyFlags)
{
	// 1.0*2^127 (0x7f000000) * 8.0 = 2^130 > PS2 max -> clamp to the PS2 FPU
	// maximum and raise O|SO in FCR31. Note the full-mode max is 0x7fffffff
	// (exp 0xff is a *valid* PS2 exponent), NOT IEEE FLT_MAX 0x7f7fffff — the
	// fast single-precision path clamps to 0x7f7fffff and never touches FCR31,
	// so both the value and the O flag are full-mode discriminators.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFcr31(0);
	h.SetFprBits(0, 0x7f000000u); // 1.0 * 2^127
	h.SetFprBits(1, FloatBits(8.0f));
	h.LoadProgram({MUL_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x7fffffffu); // PS2 FPU max (not FLT_MAX)
	EXPECT_NE(h.GetFprBitsJit(2), kPs2MaxPos);  // fast path would give this
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (kFPUflagO | kFPUflagSO), 0u);
}

// ---- Accumulator-target ops (ADDA/SUBA/MULA write ACC, not Fd). -------------

TEST(EeRecFpuFull, AddaPseudoInfPreservedToAcc)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kPs2HugePos);
	h.SetFprBits(1, FloatBits(0.0f));
	h.LoadProgram({ADDA_S(0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetAccBitsJit(), kPs2HugePos);
}

TEST(EeRecFpuFull, MulaNormalRangeToAcc)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(2.0f));
	h.SetFprBits(1, FloatBits(3.0f));
	h.LoadProgram({MULA_S(0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetAccBitsJit(), FloatBits(6.0f));
}

TEST(EeRecFpuFull, SubaNormalRangeToAcc)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(10.0f));
	h.SetFprBits(1, FloatBits(2.0f));
	h.LoadProgram({SUBA_S(0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetAccBitsJit(), FloatBits(8.0f));
}

// ---- MADD/MSUB family (Fd = ACC +/- Fs*Ft, two roundings) ------------------
//      DOUBLE recMaddsub: full multiply -> guard-mask ACC -> branch on product/
//      ACC overflow -> accumulate in double. ------------------------------------

TEST(EeRecFpuFull, MaddNormalRange)
{
	// 2.0 + 3.0*4.0 = 14.0
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetAccBits(FloatBits(2.0f));
	h.SetFprBits(0, FloatBits(3.0f));
	h.SetFprBits(1, FloatBits(4.0f));
	h.LoadProgram({MADD_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(14.0f));
}

TEST(EeRecFpuFull, MsubNormalRange)
{
	// 20.0 - 3.0*4.0 = 8.0
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetAccBits(FloatBits(20.0f));
	h.SetFprBits(0, FloatBits(3.0f));
	h.SetFprBits(1, FloatBits(4.0f));
	h.LoadProgram({MSUB_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(8.0f));
}

TEST(EeRecFpuFull, MaddPseudoInfProductPreserved)
{
	// ACC=0 + (1.0*2^128)*1.0 : the product is a PS2 pseudo-inf (0x7f800000).
	// Full mode preserves it through the multiply and the (0+x) accumulate;
	// the fast path would clamp the product to FLT_MAX (0x7f7fffff).
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetAccBits(FloatBits(0.0f));
	h.SetFprBits(0, kPs2HugePos); // 1.0 * 2^128
	h.SetFprBits(1, FloatBits(1.0f));
	h.LoadProgram({MADD_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), kPs2HugePos);
	EXPECT_NE(h.GetFprBitsJit(2), kPs2MaxPos);
}

TEST(EeRecFpuFull, MsubPseudoInfNegatesProduct)
{
	// 0.0 - (1.0*2^128)*1.0 = -(2^128) = 0xff800000 (negative pseudo-inf).
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetAccBits(FloatBits(0.0f));
	h.SetFprBits(0, kPs2HugePos);
	h.SetFprBits(1, FloatBits(1.0f));
	h.LoadProgram({MSUB_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0xff800000u);
}

TEST(EeRecFpuFull, MaddProductOverflowClampsAndSetsFlags)
{
	// (1.0*2^127)*8.0 = 2^130 overflows PS2 range -> the multiply saturates on
	// the product-overflow path: result is +PS2-max with O|SO set. (ACC=1.0 is
	// dominated by the saturated product either way, so this pins the clamp +
	// sticky flags, not the accumulate-skip itself.)
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFcr31(0);
	h.SetAccBits(FloatBits(1.0f)); // dominated by the 2^130 product
	h.SetFprBits(0, 0x7f000000u);  // 1.0 * 2^127
	h.SetFprBits(1, FloatBits(8.0f));
	h.LoadProgram({MADD_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x7fffffffu);
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (kFPUflagO | kFPUflagSO), 0u);
}

TEST(EeRecFpuFull, MaddaNormalRangeToAcc)
{
	// MADDA writes ACC: 1.0 + 2.0*3.0 = 7.0
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetAccBits(FloatBits(1.0f));
	h.SetFprBits(0, FloatBits(2.0f));
	h.SetFprBits(1, FloatBits(3.0f));
	h.LoadProgram({MADDA_S(0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetAccBitsJit(), FloatBits(7.0f));
}

TEST(EeRecFpuFull, MsubaNormalRangeToAcc)
{
	// MSUBA writes ACC: 10.0 - 2.0*3.0 = 4.0
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetAccBits(FloatBits(10.0f));
	h.SetFprBits(0, FloatBits(2.0f));
	h.SetFprBits(1, FloatBits(3.0f));
	h.LoadProgram({MSUBA_S(0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetAccBitsJit(), FloatBits(4.0f));
}

TEST(EeRecFpuFull, MaddaProductOverflowSetsAccflag)
{
	// MADDA with an overflowing product: ACC clamps to PS2-max and the sticky
	// ACCflag bit must be set so a *subsequent* op sees the saturated ACC. This
	// is the accumulator-overflow propagation path unique to the *A variants.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFcr31(0);
	h.SetAccBits(FloatBits(1.0f));
	h.SetFprBits(0, 0x7f000000u); // 1.0 * 2^127
	h.SetFprBits(1, FloatBits(8.0f));
	h.LoadProgram({MADDA_S(0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetAccBitsJit(), 0x7fffffffu);
	EXPECT_NE(h.JitSnapshot().fprs.fprc[31] & (kFPUflagO | kFPUflagSO), 0u);
	EXPECT_NE(h.JitSnapshot().fprs.ACCflag & 1u, 0u);
}

// ---- GE-20 slice 1: ABS/NEG/MAX/MIN/C.cond DOUBLE bodies. ------------------
//
// Discriminators: pseudo-infinity operands (exp field 0xff). The fast path
// clamps them to ±FLT_MAX before/after the op; DOUBLE preserves them (ABS/NEG
// are raw sign-bit ops, MAX/MIN order them by magnitude, C.cond compares them
// as the distinct finite numbers they are on PS2). DOUBLE ABS/NEG/MAX/MIN also
// clear the O/U status flags (x86 CLEAR_OU_FLAGS), which the fast path leaves.

TEST(EeRecFpuFull, AbsPreservesPseudoInf)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, 0xff800000u); // -1.0 * 2^128
	h.LoadProgram({ABS_S(2, 0)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), kPs2HugePos); // fast path would clamp to 0x7f7fffff
}

TEST(EeRecFpuFull, NegPreservesPseudoInf)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kPs2HugePos);
	h.LoadProgram({NEG_S(2, 0)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0xff800000u);
}

TEST(EeRecFpuFull, AbsClearsOUFlags)
{
	// Seed O|U into FCR31 via CTC1; DOUBLE ABS must clear both (x86
	// CLEAR_OU_FLAGS), the fast body leaves them set.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(1.0f));
	h.SetGpr64(reg::t0, 0x0000c000u); // FPUflagO | FPUflagU
	h.LoadProgram({
		CTC1(reg::t0, 31),
		ABS_S(2, 0),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGpr64Jit(reg::v0) & 0x0000c000u, 0u);
}

TEST(EeRecFpuFull, MaxOrdersPseudoInfAgainstNormal)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kPs2HugePos);       // 2^128
	h.SetFprBits(1, FloatBits(1.0f));
	h.LoadProgram({MAX_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), kPs2HugePos); // fast path clamps to 0x7f7fffff
}

TEST(EeRecFpuFull, MinOrdersNegPseudoInfAgainstNormal)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, 0xff800000u);       // -2^128
	h.SetFprBits(1, FloatBits(-1.0f));
	h.LoadProgram({MIN_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0xff800000u);
}

TEST(EeRecFpuFull, MaxOfTwoNegativesPicksSmaller)
{
	// Plain-range semantics guard through the integer-ordering construction
	// (negative operands order inversely on raw bits — the constructed upper
	// word must fix the sign ordering).
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(-2.0f));
	h.SetFprBits(1, FloatBits(-8.0f));
	h.LoadProgram({MAX_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(-2.0f));
}

TEST(EeRecFpuFull, CEqDistinctPseudoInfsNotEqual)
{
	// 0x7f800000 and 0x7f800001 are DIFFERENT finite 2^128-scale numbers on
	// PS2. The fast path clamps both to +FLT_MAX and calls them equal.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, 0x7f800000u);
	h.SetFprBits(1, 0x7f800001u);
	h.LoadProgram({
		C_EQ_S(0, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGpr64Jit(reg::v0) & 0x00800000u, 0u) << "distinct pseudo-infs compared equal";
}

TEST(EeRecFpuFull, CLtDistinctPseudoInfsOrdered)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, 0x7f800000u);
	h.SetFprBits(1, 0x7f800001u);
	h.LoadProgram({
		C_LT_S(0, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_NE(h.GetGpr64Jit(reg::v0) & 0x00800000u, 0u) << "fs < ft not detected";
}

TEST(EeRecFpuFull, CLeEqualOperandsTrue)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(4.0f));
	h.SetFprBits(1, FloatBits(4.0f));
	h.LoadProgram({
		C_LE_S(0, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_NE(h.GetGpr64Jit(reg::v0) & 0x00800000u, 0u);
}

TEST(EeRecFpuFull, MaxClearsOUFlags)
{
	// The pseudo-inf value cases pass on the fast body via IEEE Inf handling;
	// the deterministic DOUBLE discriminator for MAX/MIN is CLEAR_OU_FLAGS.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(1.0f));
	h.SetFprBits(1, FloatBits(2.0f));
	h.SetGpr64(reg::t0, 0x0000c000u); // FPUflagO | FPUflagU
	h.LoadProgram({
		CTC1(reg::t0, 31),
		MAX_S(2, 0, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGpr64Jit(reg::v0) & 0x0000c000u, 0u);
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(2.0f));
}

TEST(EeRecFpuFull, MinClearsOUFlags)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(1.0f));
	h.SetFprBits(1, FloatBits(2.0f));
	h.SetGpr64(reg::t0, 0x0000c000u);
	h.LoadProgram({
		CTC1(reg::t0, 31),
		MIN_S(2, 0, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGpr64Jit(reg::v0) & 0x0000c000u, 0u);
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(1.0f));
}

// ---- GE-20 slice 2: DIV/SQRT/RSQRT DOUBLE bodies. --------------------------
//
// The heavy widen->op->narrow ports (x86 iFPUd.cpp recDIVhelper1 /
// recSQRT_S_xmm / recRSQRThelper1). Discriminators: pseudo-inf operands run
// exactly in double (the fast bodies clamp them; the RSQRT interp fallback
// zeroes them), and the RSQRT divide-by-zero result takes the DIVIDEND's
// sign (the interp fallback keys it off the divisor).

TEST(EeRecFpuFull, DivPseudoInfByTwoExact)
{
	// 2^128 / 2.0 = 2^127 = 0x7f000000 — representable, exact in double.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kPs2HugePos);
	h.SetFprBits(1, FloatBits(2.0f));
	h.LoadProgram({DIV_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x7f000000u); // fast path clamps fs -> 0x7effffff
}

// The DOUBLE-mode divide-by-zero result is NOT the single-mode ±FLT_MAX.
// x86 iFPUd.cpp SetMaxValue() branches on FPU_RESULT, which is #defined to 1,
// so the live arm is `xOR.PS(regd, s_const.pos[0])` with pos[0] == 0x7fffffff
// — exponent field 0xff, one ULP band above the 0x7f7fffff that the *dead*
// else-arm (and the single-precision iFPU.cpp recDIVhelper1) uses. On the EE
// that is just a larger finite float (no NaN/Inf encodings), but guest
// softfloat routines classify exp==0xff separately, so the distinction is
// game-visible. See NFS Carbon below.
TEST(EeRecFpuFull, DivByZeroFlagsAndMax)
{
	// x/0: D|SD set, result = (fs ^ ft) | 0x7fffffff.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(-3.0f));
	h.SetFprBits(1, 0x00000000u);
	h.LoadProgram({
		DIV_S(2, 0, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0xffffffffu);
	EXPECT_EQ(h.GetGpr64Jit(reg::v0) & 0x00010020u, 0x00010020u) << "D|SD not set";
}

// NFS Carbon (SLUS-21493, eeClampMode:3) regression. A disabled sine-wobble
// axis leaves both parameters +0.0, so the game divides 0.0/0.0 every time it
// builds the table and relies on the result's exponent field being 0xff: its
// softfloat float->double->int helper classifies that as non-finite and
// returns a value <= 0, which the following `blez` uses to skip the table
// build. Emitting 0x7f7fffff instead makes the helper saturate to INT_MAX, and
// the game then allocates and byte-fills a 2^31-entry table, wiping guest RAM
// until a NULL vtable dispatch lands the EE at PC 0 and the kernel halts.
TEST(EeRecFpuFull, DivZeroOverZeroKeepsPseudoInfExponent)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, 0x00000000u); // +0.0 dividend
	h.SetFprBits(1, 0x00000000u); // +0.0 divisor
	h.LoadProgram({
		DIV_S(2, 0, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x7fffffffu);
	EXPECT_EQ((h.GetFprBitsJit(2) >> 23) & 0xffu, 0xffu) << "exponent must be 0xff";
	EXPECT_EQ(h.GetGpr64Jit(reg::v0) & 0x00020040u, 0x00020040u) << "I|SI not set";
}

TEST(EeRecFpuFull, SqrtPseudoInfExact)
{
	// sqrt(2^128) = 2^64 = 0x5f800000 exactly.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(1, kPs2HugePos);
	h.LoadProgram({SQRT_S(2, 1)});
	h.RunJitNoDiff();
	// Discriminator: ToDouble carries exponent 255 across exactly, so FULL gets
	// the true sqrt(2^128). The single-precision fast body clamps the operand
	// to +FLT_MAX first (recSQRT_S_xmm's CHECK_FPU_OVERFLOW xMIN, matching x86)
	// and lands one ULP low at 0x5f7fffff — measured in
	// EeFpuOverflowConsole.SqrtClampsItsOperandLikeTheRestOfTheFamily.
	EXPECT_EQ(h.GetFprBitsJit(2), 0x5f800000u);
}

TEST(EeRecFpuFull, SqrtNegativeSetsIFlagAndUsesAbs)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(1, FloatBits(-4.0f));
	h.LoadProgram({
		SQRT_S(2, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(2.0f));
	EXPECT_EQ(h.GetGpr64Jit(reg::v0) & 0x00020040u, 0x00020040u) << "I|SI not set";
}

TEST(EeRecFpuFull, RsqrtPseudoInfExact)
{
	// 1.0 / sqrt(2^128) = 2^-64 = 0x1f800000 exactly. The current interp
	// fallback reads 0x7f800000 as IEEE +Inf and returns 0.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(1.0f));
	h.SetFprBits(1, kPs2HugePos);
	h.LoadProgram({RSQRT_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x1f800000u);
}

TEST(EeRecFpuFull, RsqrtDivByZeroSignedMaxFromDividend)
{
	// ft == 0: D|SD and result = FS | 0x7fffffff (x86 DOUBLE keys the sign off
	// the DIVIDEND; the interp fallback keys it off the divisor — x86-JIT is
	// the FULL-mode oracle). Same SetMaxValue constant as DIV above.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(-1.0f));
	h.SetFprBits(1, 0x00000000u);
	h.LoadProgram({
		RSQRT_S(2, 0, 1),
		CFC1(reg::v0, 31),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0xffffffffu);
	EXPECT_EQ(h.GetGpr64Jit(reg::v0) & 0x00010020u, 0x00010020u) << "D|SD not set";
}

// ToPS2FPU_Full's "large but PS2-representable" arm must not be entered by a
// value ABOVE the EE maximum.
//
// That arm (iFPUd-arm64.cpp) halves the double, narrows, and adds 0x00800000
// back to the single. Its guard was |x| >= 2^129 — but the largest number this
// FPU has is 0x7FFFFFFF == (2 - 2^-23) * 2^128, which is BELOW 2^129, so the
// band (EE max, 2^129) was routed into the halving arm instead of
// saturating. Halved, such a value sits just under 2^128; under the divide
// unit's round-to-NEAREST FPCR the narrow rounds it up to a host infinity
// (0x7f800000) and the +0x00800000 carries out of the exponent field into the
// SIGN BIT:
//
//     0x7f800000 + 0x00800000 == 0x80000000
//
// so the largest magnitude the FPU can produce came back as negative zero.
// Under the arithmetic FPCR (ChopZero) the narrow chops to 0x7f7fffff instead
// and the arm is correct, which is why only the ops that swap to FPUDivFPCR
// could see it.
//
// The interpreter cannot wrap this way: it narrows through the host FPU and
// saturates at ±FLT_MAX (checkOverflow, FPU.cpp), so it never adds into the
// exponent field at all. It also stops a binade below the console's
// 0x7FFFFFFF there — a separate, known gap in the interpreter, not this bug.
//
// ONLY RSQRT REACHES THE BAND. A DIV quotient cannot: for 24-bit significands
// with a < b, a/b <= 1 - 2^-24 strictly, and the band's relative width is
// exactly 2^-24 (a sweep of the four reachable exponent differences found no
// hits, and DIV.S(0x7FFFFFFF, 0x3F7FFFFF) lands on 2^129 *exactly*, which the
// >= arm already handled). SQRT halves exponents and cannot get near. RSQRT
// divides by a 53-bit sqrt result, so the argument does not apply.
//
// The operand pairs below were found by solving fs / sqrt(ft) for the band.
// The console saturates at 0x7FFFFFFF, and FULL mode now does the same. The
// interpreter column is pinned too, at its own saturation bound of
// ±FLT_MAX (0x7F7FFFFF) — a binade low against silicon, but positive and
// stable: the point here is that neither engine wraps to negative zero.
TEST(EeRecFpuFull, RsqrtAboveEeMaxSaturatesInsteadOfWrappingToNegativeZero)
{
	static const u32 kPairs[][2] = {
		{0x608073EEu, 0x0080E845u}, {0x60814231u, 0x0082878Du},
		{0x6081A669u, 0x00835244u}, {0x6081B3B0u, 0x00836D2Bu},
		{0x6081F74Du, 0x0083F655u},
	};
	for (const auto& p : kPairs)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.EnableFpuFullMode();
		h.SetFprBits(0, p[0]);
		h.SetFprBits(1, p[1]);
		h.LoadProgram({RSQRT_S(2, 0, 1)});
		h.RunJitNoDiff();

		// RunJitNoDiff does not run the interpreter, and GetFprBitsInterp would
		// then hand back the JIT's own value — the reference needs its own run.
		EeRecTestHarness i;
		i.EnableCop1();
		i.SetFprBits(0, p[0]);
		i.SetFprBits(1, p[1]);
		i.LoadProgram({RSQRT_S(2, 0, 1)});
		i.RunInterpOnly();

		EXPECT_EQ(h.GetFprBitsJit(2), 0x7FFFFFFFu)
			<< "fs=" << p[0] << " ft=" << p[1] << " wrapped";
		EXPECT_EQ(i.GetFprBitsInterp(2), 0x7F7FFFFFu)
			<< "interpreter reference moved";
	}
}

// Liveness for the test above: the halving arm must still be REACHABLE and
// exact for the top binade proper. 1.5*2^128 / 1.0 is in the arm's range and
// below the EE maximum, so it must come back unrounded. Tightening the overflow
// guard too far (down to 2^128) would saturate this to 0x7FFFFFFF and turn the
// test above green for the wrong reason.
TEST(EeRecFpuFull, DivKeepsTopBinadeResultsBelowTheEeMaximum)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, 0x7FC00000u); // 1.5 * 2^128
	h.SetFprBits(1, FloatBits(1.0f));
	h.LoadProgram({DIV_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x7FC00000u);
}

// GE-M2 residency coherence: FPU-full (DOUBLE-mode) ops hand-emit integer scratch
// for the guard-bit alignment (FPU_ADD_SUB) and the min/max bit-pattern build
// (recMINMAX). Those temps were RWARG3/RWARG4 (w2/w3) — EE-allocatable pool
// hosts; the rehome moved them to the reserved load/store scratch x9/x10, because
// the FPU path never flushes the EE GPR allocator, so under the residency flip a
// guest scalar live in x2/x3 would otherwise be clobbered. This keeps a wide band
// of dirty guest scalars live across an ADD.S (FPU_ADD_SUB) and a MAX.S
// (recMINMAX) and asserts they survive. FPU-full is JIT-only (the shared interp
// has no double path), so the bystanders are checked via GetGpr64Jit — their
// values are ordinary EE ALU results, independent of the double FPR result. Green
// on the pre-flip baseline (nothing resident); red under the flip if w2/w3
// scratch ever returned.
TEST(EeRecFpuFull, DoubleModeScratchPreservesLiveGuestScalars)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, FloatBits(2.5f));
	h.SetFprBits(1, FloatBits(1.25f));
	// Distinct sentinels across a broad band of unpinned/allocatable guest regs.
	h.SetGpr64(reg::t0, 0x1010101010101010ull);
	h.SetGpr64(reg::t1, 0x0000000012340000ull);
	h.SetGpr64(reg::t2, 0x0000000000005678ull);
	h.SetGpr64(reg::t3, 0x3030303030303030ull);
	h.SetGpr64(reg::t5, 0x0000000000000005ull);
	h.SetGpr64(reg::t6, 0x00000000FFFFFFFBull);
	h.SetGpr64(reg::s1, 0x0000000000000009ull);
	h.SetGpr64(reg::s2, 0x0000000000000002ull);
	h.LoadProgram({
		// Dirty a broad band right before the FPU ops so several land in the pool
		// slots (x2-x7/x14/x15) as MODE_WRITE residents under the flip.
		ADDU (reg::t4, reg::t5, reg::t6),  // t4 = sext32(5 + -5) = 0
		ADDU (reg::t7, reg::t1, reg::t2),  // t7 = 0x12345678
		DADDU(reg::t8, reg::s1, reg::s2),  // t8 = 0xB (64-bit)
		ADDU (reg::t9, reg::t0, reg::t3),  // t9 = sext32(0x10101010 + 0x30303030)
		ADD_S(2, 0, 1),                    // FPR2 = 3.75; FPU_ADD_SUB guard path (x9/x10)
		MAX_S(3, 0, 1),                    // recMINMAX (x9)
	});
	h.RunJitNoDiff();
	// The FPU ops must not corrupt any live guest scalar.
	EXPECT_EQ(h.GetGpr64Jit(reg::t4), 0ull);
	EXPECT_EQ(h.GetGpr64Jit(reg::t7), 0x0000000012345678ull);
	EXPECT_EQ(h.GetGpr64Jit(reg::t8), 0x000000000000000Bull);
	EXPECT_EQ(h.GetGpr64Jit(reg::t9), 0x0000000040404040ull);
	EXPECT_EQ(h.GetGpr64Jit(reg::t0), 0x1010101010101010ull); // pure source, untouched
	EXPECT_EQ(h.GetGpr64Jit(reg::t3), 0x3030303030303030ull); // pure source, untouched
	// Sanity: the double-mode ADD result is still correct (normal-range round-trip).
	EXPECT_EQ(h.GetFprBitsJit(2), FloatBits(3.75f));
}

// ---- Guard mask and rounding for the MADD family (recMaddsub) --------------
//
// recMaddsub keeps the product wide between its two roundings: the multiply
// stage rounds it to PS2 precision in the double domain (ToPS2FPU_Wide) instead
// of narrowing to a single and widening straight back, and the guard mask then
// runs on doubles (FPU_ADD_SUB_D). Both tests below pin behaviour that predates
// that change -- they pass identically on the narrow implementation.
//
// Why these inputs and not rounder ones: a 1067-row grid built by sweeping the
// ACC/product exponent difference from -32..+32 with all-ones mantissas cannot
// see the guard mask at all (breaking the mask shift by one moved 0 of its
// rows). The masked bits sit strictly below half an ULP of the sum, so they
// only survive the truncation when the exact sum lands within that band of an
// ULP boundary -- which all-ones mantissas never do. The rows below were found
// by searching for that condition against an independent C model of the
// pipeline, which is also where their expected values come from. Breaking the
// mask shift by one moves every one of them by exactly 1 ULP.

namespace {

struct MaddRow
{
	u32 acc, fs, ft;
	int op; // 0=MADD 1=MSUB 2=MADDA 3=MSUBA
	u32 expected;
	u32 expected_fcr31;
};

// Runs one row and returns the destination register's bits (Fd for MADD/MSUB,
// ACC for MADDA/MSUBA) plus FCR31.
void RunMaddRow(const MaddRow& r, u32* out_val, u32* out_fcr31)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFcr31(0);
	h.SetAccBits(r.acc);
	h.SetFprBits(0, r.fs);
	h.SetFprBits(1, r.ft);
	switch (r.op)
	{
		case 0: h.LoadProgram({MADD_S(2, 0, 1)}); break;
		case 1: h.LoadProgram({MSUB_S(2, 0, 1)}); break;
		case 2: h.LoadProgram({MADDA_S(0, 1)}); break;
		default: h.LoadProgram({MSUBA_S(0, 1)}); break;
	}
	h.RunJitNoDiff();
	*out_val = (r.op < 2) ? h.GetFprBitsJit(2) : h.GetAccBitsJit();
	*out_fcr31 = h.JitSnapshot().fprs.fprc[31];
}

// fs is 1.0 throughout, so the rounded product is ft and the ACC/product
// exponent difference -- the only axis the guard mask reads -- is directly
// controllable. The 72 rows span differences -23..+23, covering both the arm
// that masks the product and the arm that masks the ACC.
constexpr MaddRow kGuardMaskWitnesses[] = {
	{0x3fb38acau, 0x3f800000u, 0xbacc0111u, 0, 0x3fb357cau, 0u},
	{0x3ab20dd7u, 0x3f800000u, 0xc4195bd9u, 0, 0xc4195bc3u, 0u},
	{0xbe953636u, 0x3f800000u, 0x398a99a5u, 0, 0xbe951390u, 0u},
	{0x33f55005u, 0x3f800000u, 0xac49b3b5u, 0, 0x33f54e72u, 0u},
	{0x3b6825c7u, 0x3f800000u, 0xc2caa569u, 0, 0xc2caa399u, 0u},
	{0x42c7b709u, 0x3f800000u, 0x3c50ef1au, 1, 0x42c7b082u, 0u},
	{0xc481068au, 0x3f800000u, 0xc291ec04u, 1, 0xc46fcf94u, 0u},
	{0x3924ba1bu, 0x3f800000u, 0xbc28556cu, 0, 0xbc25c284u, 0u},
	{0xb5219112u, 0x3f800000u, 0x40c46022u, 0, 0x40c46021u, 0u},
	{0xc36b6e95u, 0x3f800000u, 0xc42bd5ffu, 1, 0x43e1f4b4u, 0u},
	{0xb29a5453u, 0x3f800000u, 0x29324d8au, 0, 0xb29a543du, 0u},
	{0xbdc2a958u, 0x3f800000u, 0x329bcd66u, 0, 0xbdc2a956u, 0u},
	{0xb35354cbu, 0x3f800000u, 0xa9db297eu, 1, 0xb35354b0u, 0u},
	{0xc2074068u, 0x3f800000u, 0xca66524du, 1, 0x4a6651c6u, 0u},
	{0xbc0f2a31u, 0x3f800000u, 0x384f5a7cu, 0, 0xbc0e5ad7u, 0u},
	{0xc3d2b83cu, 0x3f800000u, 0xced89810u, 1, 0x4ed8980du, 0u},
	{0xb5af16b1u, 0x3f800000u, 0xafd2374cu, 1, 0xb5af098eu, 0u},
	{0x3b2d84c4u, 0x3f800000u, 0xc106f1efu, 0, 0xc106e717u, 0u},
	{0x35f587a4u, 0x3f800000u, 0xb4cf4ba1u, 0, 0x35c1b4bcu, 0u},
	{0x41f0dbb2u, 0x3f800000u, 0xc8e14376u, 0, 0xc8e13fb3u, 0u},
	{0xb362649cu, 0x3f800000u, 0xa9f35cf9u, 1, 0xb362647eu, 0u},
	{0xb2fa917bu, 0x3f800000u, 0x39d9d803u, 0, 0x39d9d419u, 0u},
	{0x370518f8u, 0x3f800000u, 0x334e4a63u, 1, 0x37044aaeu, 0u},
	{0xc11469c2u, 0x3f800000u, 0x462a42d6u, 0, 0x462a1dbcu, 0u},
	{0x3649cec6u, 0x3f800000u, 0xbf2b2cabu, 0, 0xbf2b2c79u, 0u},
	{0xb9e9f9d2u, 0x3f800000u, 0xb833060bu, 1, 0xb9d39911u, 0u},
	{0x32ea9db1u, 0x3f800000u, 0xadb7adb2u, 0, 0x32ea6fc6u, 0u},
	{0x3fbcf544u, 0x3f800000u, 0xbc85569au, 0, 0x3fbadfeau, 0u},
	{0xbaa0f0b0u, 0x3f800000u, 0x3f761279u, 0, 0x3f75c201u, 0u},
	{0x3be79b92u, 0x3f800000u, 0xb6c52501u, 0, 0x3be76a49u, 0u},
	{0x32dce714u, 0x3f800000u, 0x2869f708u, 1, 0x32dce70du, 0u},
	{0xbb7b2e3au, 0x3f800000u, 0x42b93816u, 0, 0x42b93620u, 0u},
	{0x3baf0f6cu, 0x3f800000u, 0xbff04b54u, 0, 0xbfef9c45u, 0u},
	{0xbab47c74u, 0x3f800000u, 0x448b789fu, 0, 0x448b7894u, 0u},
	{0xc3de412bu, 0x3f800000u, 0x4579985cu, 0, 0x455dd037u, 0u},
	{0xb2fa7f73u, 0x3f800000u, 0xa912c471u, 1, 0xb2fa7f61u, 0u},
	{0x4260d9d0u, 0x3f800000u, 0xb9d4bf54u, 0, 0x4260d966u, 0u},
	{0x3292dea1u, 0x3f800000u, 0xbdaacd0bu, 0, 0xbdaacd09u, 0u},
	{0x3c6a6437u, 0x3f800000u, 0x3e339b27u, 1, 0xbe24f4e4u, 0u},
	{0xc1306401u, 0x3f800000u, 0x492f0ebdu, 0, 0x492f0e0du, 0u},
	{0x3635b3f4u, 0x3f800000u, 0x343227f4u, 1, 0x362a9175u, 0u},
	{0xb400e647u, 0x3f800000u, 0x3cb12651u, 0, 0x3cb12611u, 0u},
	{0x39907517u, 0x3f800000u, 0xbba223b7u, 0, 0xbb991c66u, 0u},
	{0x330ea9edu, 0x3f800000u, 0xbae904eau, 0, 0xbae903cdu, 0u},
	{0xbf6ee4e6u, 0x3f800000u, 0xc1054588u, 1, 0x40ecae74u, 0u},
	{0xc405bbb4u, 0x3f800000u, 0x4627619cu, 0, 0x461f05e1u, 0u},
	{0xb7fa0949u, 0x3f800000u, 0xb8ab876du, 1, 0x385a0a36u, 0u},
	{0x3b48dad0u, 0x3f800000u, 0xb60bbdb1u, 0, 0x3b48b7e1u, 0u},
	{0x418eac80u, 0x3f800000u, 0x4a25b350u, 1, 0xca25b309u, 0u},
	{0x35935179u, 0x3f800000u, 0xb3508e2fu, 0, 0x358ccd08u, 0u},
	{0x452ecb68u, 0x3f800000u, 0xc914f502u, 0, 0xc9144637u, 0u},
	{0x3ef73c56u, 0x3f800000u, 0x48b65815u, 1, 0xc8b65806u, 0u},
	{0x3ec3ca47u, 0x3f800000u, 0x33267262u, 1, 0x3ec3ca46u, 0u},
	{0x332a2e5bu, 0x3f800000u, 0xaa055622u, 0, 0x332a2e3au, 0u},
	{0x45855769u, 0x3f800000u, 0xc6cac295u, 0, 0xc6a96cbbu, 0u},
	{0x45393be5u, 0x3f800000u, 0x4266ee5au, 1, 0x4535a02cu, 0u},
	{0xb5a78404u, 0x3f800000u, 0xaf843707u, 1, 0xb5a77bc1u, 0u},
	{0x4324653fu, 0x3f800000u, 0xbddf42d3u, 0, 0x43244957u, 0u},
	{0xc494cb38u, 0x3f800000u, 0xcff0bf3du, 1, 0x4ff0bf3bu, 0u},
	{0xb95aee1bu, 0x3f800000u, 0x3a201cdau, 0, 0x39d2c2a7u, 0u},
	{0x42cc503du, 0x3f800000u, 0x463f3294u, 1, 0xc63d99f4u, 0u},
	{0xb9bd2a07u, 0x3f800000u, 0x451ddb2eu, 0, 0x451ddb2du, 0u},
	{0xc2cfc0efu, 0x3f800000u, 0xc5730a69u, 1, 0x456c8c62u, 0u},
	{0xbaaeb833u, 0x3f800000u, 0x3563ab35u, 0, 0xbaae9bbeu, 0u},
	{0xb556d230u, 0x3f800000u, 0xa9b57d1cu, 1, 0xb556d22fu, 0u},
	{0xbfe9553bu, 0x3f800000u, 0x34dcabf8u, 0, 0xbfe95538u, 0u},
	{0x3a2ce961u, 0x3f800000u, 0x4585376eu, 1, 0xc585376du, 0u},
	{0xc058aa2eu, 0x3f800000u, 0x38a9a118u, 0, 0xc058a8dbu, 0u},
	{0x3daca0f3u, 0x3f800000u, 0x33a45aa6u, 1, 0x3daca0e9u, 0u},
	{0x45f4ede4u, 0x3f800000u, 0xc7fb950bu, 0, 0xc7ec462du, 0u},
	{0xbcfcd10eu, 0x3f800000u, 0x3560a544u, 0, 0xbcfccf4du, 0u},
	{0xc2b750c8u, 0x3f800000u, 0xb7943082u, 1, 0xc2b750c6u, 0u},
};

} // namespace

TEST(EeRecFpuFull, MaddGuardMaskAcrossExponentDifferences)
{
	for (const MaddRow& r : kGuardMaskWitnesses)
	{
		u32 val = 0, fcr31 = 0;
		RunMaddRow(r, &val, &fcr31);
		EXPECT_EQ(val, r.expected)
			<< "acc=" << std::hex << r.acc << " fs=" << r.fs << " ft=" << r.ft
			<< " op=" << std::dec << r.op;
	}
}

// ToPS2FPU_Wide's arms: saturation at the PS2 maximum, the exponent-0xff band
// (whose halve/narrow/re-raise arm the wide form deletes outright -- up there a
// PS2 single is just an ordinary double, so it is a plain chop), the underflow
// flush with its U|SU flags, and signed zero. Expected values are pinned from
// the narrow implementation these replaced.
TEST(EeRecFpuFull, MaddWideRoundArms)
{
	static const MaddRow kRows[] = {
		// product == the EE maximum exactly (FLT_MAX * 2): in range, no O.
		{0x3f800000u, 0x7f7fffffu, 0x40000000u, 0, 0x7fffffffu, 0x00000000u},
		{0x3f800000u, 0x7f7fffffu, 0x40000000u, 1, 0xffffffffu, 0x00000000u},
		{0x3f800000u, 0xff7fffffu, 0x40000000u, 0, 0xffffffffu, 0x00000000u},
		{0x3f800000u, 0xff7fffffu, 0x40000000u, 1, 0x7fffffffu, 0x00000000u},
		// product above the EE maximum: the mulovf arm, O|SO raised.
		{0x3f800000u, 0x7f7fffffu, 0x40000001u, 0, 0x7fffffffu, 0x00008010u},
		{0x3f800000u, 0x7f7fffffu, 0x40000001u, 1, 0xffffffffu, 0x00008010u},
		{0x3f800000u, 0xff7fffffu, 0x40000001u, 1, 0x7fffffffu, 0x00008010u},
		{0x3f800000u, 0x7f7fffffu, 0x40000001u, 2, 0x7fffffffu, 0x00008010u},
		{0x3f800000u, 0x7f7fffffu, 0x40000001u, 3, 0xffffffffu, 0x00008010u},
		// exponent-0xff band: 2^128 exactly, and a product needing a real chop.
		{0x3f800000u, 0x7f000000u, 0x40000000u, 0, 0x7f800000u, 0x00000000u},
		{0x3f800000u, 0x7f000001u, 0x40000001u, 0, 0x7f800002u, 0x00000000u},
		{0x3f800000u, 0x7f000001u, 0x40000001u, 1, 0xff800002u, 0x00000000u},
		{0x3f800000u, 0xff000001u, 0x40000001u, 0, 0xff800002u, 0x00000000u},
		{0x3f800000u, 0x7ffffffeu, 0x3f800000u, 0, 0x7ffffffeu, 0x00000000u},
		{0x3f800000u, 0x7fffffffu, 0x3f800000u, 0, 0x7fffffffu, 0x00000000u},
		// underflow: product below 2^-126 flushes to signed zero, U|SU raised.
		{0x00000000u, 0x00800000u, 0x3f000000u, 0, 0x00000000u, 0x00000008u},
		{0x00000000u, 0x80800000u, 0x3f000000u, 0, 0x00000000u, 0x00000008u},
		{0x3f800000u, 0x00800000u, 0x3f000000u, 0, 0x3f800000u, 0x00000008u},
		// exact zeros and denormal operands (ToDouble flushes under FZ).
		{0x00000000u, 0x00000000u, 0x3f800000u, 0, 0x00000000u, 0x00000000u},
		{0x00000000u, 0x80000000u, 0x3f800000u, 0, 0x00000000u, 0x00000000u},
		{0x3f800000u, 0x00000001u, 0x00000001u, 0, 0x3f800000u, 0x00000000u},
		{0x00000001u, 0x3f800000u, 0x3f800000u, 0, 0x3f800000u, 0x00000000u},
		{0x807fffffu, 0x3f800000u, 0x3f800000u, 0, 0x3f800000u, 0x00000000u},
		{0x807fffffu, 0x3f800000u, 0x3f800000u, 1, 0xbf800000u, 0x00000000u},
		// ACC already at the PS2 maximum; and the accumulate itself overflowing.
		{0x7fffffffu, 0x3f800000u, 0x3f800000u, 0, 0x7fffffffu, 0x00000000u},
		{0xffffffffu, 0x3f800000u, 0x3f800000u, 0, 0xffffffffu, 0x00000000u},
		{0x7f800000u, 0x7f800000u, 0x3f800000u, 0, 0x7fffffffu, 0x00008010u},
		{0x7f800000u, 0x7f800000u, 0x3f800000u, 1, 0x00000000u, 0x00000000u},
		{0x7fffffffu, 0x7f7fffffu, 0x40000000u, 0, 0x7fffffffu, 0x00008010u},
		{0xffffffffu, 0x7f7fffffu, 0x40000000u, 1, 0xffffffffu, 0x00008010u},
	};
	for (const MaddRow& r : kRows)
	{
		u32 val = 0, fcr31 = 0;
		RunMaddRow(r, &val, &fcr31);
		EXPECT_EQ(val, r.expected)
			<< "acc=" << std::hex << r.acc << " fs=" << r.fs << " ft=" << r.ft
			<< " op=" << std::dec << r.op;
		EXPECT_EQ(fcr31, r.expected_fcr31)
			<< "acc=" << std::hex << r.acc << " fs=" << r.fs << " ft=" << r.ft
			<< " op=" << std::dec << r.op;
	}
}
