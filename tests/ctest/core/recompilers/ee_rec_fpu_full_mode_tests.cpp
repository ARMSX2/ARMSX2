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

#include <cstdio>
#include <cstring>
#include <iterator>
#include <gtest/gtest.h>
#include <vector>

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
constexpr u32 kFPUflagU  = 0x00004000;
constexpr u32 kFPUflagSU = 0x00000008;

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
	// ToDouble carries exponent 255 across exactly, so FULL gets the true
	// sqrt(2^128). This no longer discriminates against the fast body: its
	// operand clamp is gone and it scales too, pinned against the console by
	// EeFpuOverflowConsole.SqrtMatchesConsoleOnEveryExponent255Operand.
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
// band (kEeFpuMax, 2^129) was routed into the halving arm instead of
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
// eeRoundToSingle (FPU.cpp) cannot wrap this way: it scales by 2^-4, so the
// exponent it adds back can never carry into the sign.
//
// ONLY RSQRT REACHES THE BAND. A DIV quotient cannot: for 24-bit significands
// with a < b, a/b <= 1 - 2^-24 strictly, and the band's relative width is
// exactly 2^-24 (a sweep of the four reachable exponent differences found no
// hits, and DIV.S(0x7FFFFFFF, 0x3F7FFFFF) lands on 2^129 *exactly*, which the
// >= arm already handled). SQRT halves exponents and cannot get near. RSQRT
// divides by a 53-bit sqrt result, so the argument does not apply.
//
// The operand pairs below were found by solving fs / sqrt(ft) for the band.
// Interpreter column is the reference; its saturate-at-0x7FFFFFFF rule is what
// EeFpuOverflowConsole pins against the console capture.
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
		EXPECT_EQ(i.GetFprBitsInterp(2), 0x7FFFFFFFu)
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
// ACC for MADDA/MSUBA) plus FCR31. `mode` is the clamp mode: 3 and 4 share
// every line of this emitter bar emitDefectiveFmul, so a row needs 4 only when
// its product is one the boundary term decides.
void RunMaddRow(const MaddRow& r, u32* out_val, u32* out_fcr31, int mode = 3)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (mode >= 4)
		h.EnableFpuExactMode();
	else
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
//
// Expected values come from the interpreter, not from this emitter. fs = 1.0 is
// a power of two, so every row's product has a zero tail and the multiplier
// deficit reaches all 72 of them; when it landed in iFPUd, 35 rows moved one ULP
// toward zero. Re-pinning them against the emitter that moved them would assert
// nothing, so each value below was re-derived by running the same row through
// FPU.cpp, which models the deficit independently (eeMulRound).
//
// Row 53 (ft = 0x48b65815) is the one the Booth term alone cannot reach: its
// mantissa 0x365815 has no bit of 0x2AA set, so the boundary term is what
// decides it.
constexpr MaddRow kGuardMaskWitnesses[] = {
	{0x3fb38acau, 0x3f800000u, 0xbacc0111u, 0, 0x3fb357cau, 0u},
	{0x3ab20dd7u, 0x3f800000u, 0xc4195bd9u, 0, 0xc4195bc2u, 0u},
	{0xbe953636u, 0x3f800000u, 0x398a99a5u, 0, 0xbe951390u, 0u},
	{0x33f55005u, 0x3f800000u, 0xac49b3b5u, 0, 0x33f54e72u, 0u},
	{0x3b6825c7u, 0x3f800000u, 0xc2caa569u, 0, 0xc2caa398u, 0u},
	{0x42c7b709u, 0x3f800000u, 0x3c50ef1au, 1, 0x42c7b082u, 0u},
	{0xc481068au, 0x3f800000u, 0xc291ec04u, 1, 0xc46fcf94u, 0u},
	{0x3924ba1bu, 0x3f800000u, 0xbc28556cu, 0, 0xbc25c283u, 0u},
	{0xb5219112u, 0x3f800000u, 0x40c46022u, 0, 0x40c46020u, 0u},
	{0xc36b6e95u, 0x3f800000u, 0xc42bd5ffu, 1, 0x43e1f4b2u, 0u},
	{0xb29a5453u, 0x3f800000u, 0x29324d8au, 0, 0xb29a543du, 0u},
	{0xbdc2a958u, 0x3f800000u, 0x329bcd66u, 0, 0xbdc2a956u, 0u},
	{0xb35354cbu, 0x3f800000u, 0xa9db297eu, 1, 0xb35354b0u, 0u},
	{0xc2074068u, 0x3f800000u, 0xca66524du, 1, 0x4a6651c5u, 0u},
	{0xbc0f2a31u, 0x3f800000u, 0x384f5a7cu, 0, 0xbc0e5ad7u, 0u},
	{0xc3d2b83cu, 0x3f800000u, 0xced89810u, 1, 0x4ed8980du, 0u},
	{0xb5af16b1u, 0x3f800000u, 0xafd2374cu, 1, 0xb5af098eu, 0u},
	{0x3b2d84c4u, 0x3f800000u, 0xc106f1efu, 0, 0xc106e716u, 0u},
	{0x35f587a4u, 0x3f800000u, 0xb4cf4ba1u, 0, 0x35c1b4bcu, 0u},
	{0x41f0dbb2u, 0x3f800000u, 0xc8e14376u, 0, 0xc8e13fb2u, 0u},
	{0xb362649cu, 0x3f800000u, 0xa9f35cf9u, 1, 0xb362647eu, 0u},
	{0xb2fa917bu, 0x3f800000u, 0x39d9d803u, 0, 0x39d9d418u, 0u},
	{0x370518f8u, 0x3f800000u, 0x334e4a63u, 1, 0x37044aaeu, 0u},
	{0xc11469c2u, 0x3f800000u, 0x462a42d6u, 0, 0x462a1dbbu, 0u},
	{0x3649cec6u, 0x3f800000u, 0xbf2b2cabu, 0, 0xbf2b2c78u, 0u},
	{0xb9e9f9d2u, 0x3f800000u, 0xb833060bu, 1, 0xb9d39911u, 0u},
	{0x32ea9db1u, 0x3f800000u, 0xadb7adb2u, 0, 0x32ea6fc6u, 0u},
	{0x3fbcf544u, 0x3f800000u, 0xbc85569au, 0, 0x3fbadfeau, 0u},
	{0xbaa0f0b0u, 0x3f800000u, 0x3f761279u, 0, 0x3f75c200u, 0u},
	{0x3be79b92u, 0x3f800000u, 0xb6c52501u, 0, 0x3be76a49u, 0u},
	{0x32dce714u, 0x3f800000u, 0x2869f708u, 1, 0x32dce70du, 0u},
	{0xbb7b2e3au, 0x3f800000u, 0x42b93816u, 0, 0x42b9361fu, 0u},
	{0x3baf0f6cu, 0x3f800000u, 0xbff04b54u, 0, 0xbfef9c44u, 0u},
	{0xbab47c74u, 0x3f800000u, 0x448b789fu, 0, 0x448b7893u, 0u},
	{0xc3de412bu, 0x3f800000u, 0x4579985cu, 0, 0x455dd036u, 0u},
	{0xb2fa7f73u, 0x3f800000u, 0xa912c471u, 1, 0xb2fa7f61u, 0u},
	{0x4260d9d0u, 0x3f800000u, 0xb9d4bf54u, 0, 0x4260d966u, 0u},
	{0x3292dea1u, 0x3f800000u, 0xbdaacd0bu, 0, 0xbdaacd08u, 0u},
	{0x3c6a6437u, 0x3f800000u, 0x3e339b27u, 1, 0xbe24f4e3u, 0u},
	{0xc1306401u, 0x3f800000u, 0x492f0ebdu, 0, 0x492f0e0cu, 0u},
	{0x3635b3f4u, 0x3f800000u, 0x343227f4u, 1, 0x362a9175u, 0u},
	{0xb400e647u, 0x3f800000u, 0x3cb12651u, 0, 0x3cb12610u, 0u},
	{0x39907517u, 0x3f800000u, 0xbba223b7u, 0, 0xbb991c65u, 0u},
	{0x330ea9edu, 0x3f800000u, 0xbae904eau, 0, 0xbae903ccu, 0u},
	{0xbf6ee4e6u, 0x3f800000u, 0xc1054588u, 1, 0x40ecae72u, 0u},
	{0xc405bbb4u, 0x3f800000u, 0x4627619cu, 0, 0x461f05e0u, 0u},
	{0xb7fa0949u, 0x3f800000u, 0xb8ab876du, 1, 0x385a0a34u, 0u},
	{0x3b48dad0u, 0x3f800000u, 0xb60bbdb1u, 0, 0x3b48b7e1u, 0u},
	{0x418eac80u, 0x3f800000u, 0x4a25b350u, 1, 0xca25b308u, 0u},
	{0x35935179u, 0x3f800000u, 0xb3508e2fu, 0, 0x358ccd08u, 0u},
	{0x452ecb68u, 0x3f800000u, 0xc914f502u, 0, 0xc9144636u, 0u},
	{0x3ef73c56u, 0x3f800000u, 0x48b65815u, 1, 0xc8b65805u, 0u},  // the boundary term decides this one
	{0x3ec3ca47u, 0x3f800000u, 0x33267262u, 1, 0x3ec3ca46u, 0u},
	{0x332a2e5bu, 0x3f800000u, 0xaa055622u, 0, 0x332a2e3au, 0u},
	{0x45855769u, 0x3f800000u, 0xc6cac295u, 0, 0xc6a96cbau, 0u},
	{0x45393be5u, 0x3f800000u, 0x4266ee5au, 1, 0x4535a02cu, 0u},
	{0xb5a78404u, 0x3f800000u, 0xaf843707u, 1, 0xb5a77bc1u, 0u},
	{0x4324653fu, 0x3f800000u, 0xbddf42d3u, 0, 0x43244957u, 0u},
	{0xc494cb38u, 0x3f800000u, 0xcff0bf3du, 1, 0x4ff0bf3au, 0u},
	{0xb95aee1bu, 0x3f800000u, 0x3a201cdau, 0, 0x39d2c2a5u, 0u},
	{0x42cc503du, 0x3f800000u, 0x463f3294u, 1, 0xc63d99f3u, 0u},
	{0xb9bd2a07u, 0x3f800000u, 0x451ddb2eu, 0, 0x451ddb2cu, 0u},
	{0xc2cfc0efu, 0x3f800000u, 0xc5730a69u, 1, 0x456c8c61u, 0u},
	{0xbaaeb833u, 0x3f800000u, 0x3563ab35u, 0, 0xbaae9bbeu, 0u},
	{0xb556d230u, 0x3f800000u, 0xa9b57d1cu, 1, 0xb556d22fu, 0u},
	{0xbfe9553bu, 0x3f800000u, 0x34dcabf8u, 0, 0xbfe95538u, 0u},
	{0x3a2ce961u, 0x3f800000u, 0x4585376eu, 1, 0xc585376cu, 0u},
	{0xc058aa2eu, 0x3f800000u, 0x38a9a118u, 0, 0xc058a8dbu, 0u},
	{0x3daca0f3u, 0x3f800000u, 0x33a45aa6u, 1, 0x3daca0e9u, 0u},
	{0x45f4ede4u, 0x3f800000u, 0xc7fb950bu, 0, 0xc7ec462cu, 0u},
	{0xbcfcd10eu, 0x3f800000u, 0x3560a544u, 0, 0xbcfccf4du, 0u},
	{0xc2b750c8u, 0x3f800000u, 0xb7943082u, 1, 0xc2b750c6u, 0u},
};

} // namespace

// Run at eeClampMode 4: the expected values are the interpreter's, and one
// witness's product is decided by the boundary term that mode 3 does not emit.
// The mask itself is not mode-dependent.
TEST(EeRecFpuFull, MaddGuardMaskAcrossExponentDifferences)
{
	for (const MaddRow& r : kGuardMaskWitnesses)
	{
		u32 val = 0, fcr31 = 0;
		RunMaddRow(r, &val, &fcr31, 4);
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


// ---------------------------------------------------------------------------
// The EE multiplier's one-ULP deficit, in mode 3.
//
// The console's multiply array is not correctly rounding: when the exact
// product has nothing below the single's ULP to absorb it, the result comes
// back exactly one step closer to zero -- and whether it does is decided by
// ft's mantissa alone, so mul.s is not commutative. Measured exhaustively on
// SCPH-90000 (captures/fpmul/): mul.s(1.0, x) is one ULP low for 8257536 of the
// 2^23 significands, mul.s(x, 1.0) is exact for all of them, and nothing ever
// came back high or two ULPs low in 16.8M probes.
//
// FpuMulHack is a one-point sample of this rule, which is why it compares fs
// and ft against their own constants and so does not fire with the operands
// reversed -- exactly what silicon does. iFPUd never had the gamefix; it has
// the general law instead, which subsumes it (the QTR/PIO2 row below is the
// gamefix's pair, reached with the gamefix off).
//
// The interpreter models the same law in FPU.cpp; these rows are the mode-3
// codegen of it, at both multiply sites -- recMULop for MUL/MULA and
// recMaddsub's multiply stage for MADD/MSUB/MADDA/MSUBA, which round through
// different helpers (ToPS2FPU_Full vs ToPS2FPU_Wide) and so are two separate
// narrowings of the same decrement.
namespace {
struct MulRow { const char* name; u32 fs, ft, want; };

// Every `want` below was measured on an SCPH-90000, FCR0 0x2e40.
constexpr MulRow kSiliconMulRows[] = {
	{"1.0 * FLT_MAX",            0x3f800000u, 0x7f7fffffu, 0x7f7ffffeu}, // one ULP low
	{"FLT_MAX * 1.0 (reversed)", 0x7f7fffffu, 0x3f800000u, 0x7f7fffffu}, // exact: ft mantissa 0
	{"2.0 * FLT_MAX",            0x40000000u, 0x7f7fffffu, 0x7ffffffeu}, // corpus case 857
	{"FLT_MAX * 2.0 (reversed)", 0x7f7fffffu, 0x40000000u, 0x7fffffffu}, // corpus case 1
	{"ft mantissa 0x400000",     0x3f800000u, 0x3fc00000u, 0x3fc00000u}, // exact
	{"ft mantissa 0x000001",     0x3f800000u, 0x3f800001u, 0x3f800001u}, // exact
	{"ft mantissa 0x3fffff",     0x3f800000u, 0x3fbfffffu, 0x3fbffffeu}, // low
	{"2^-126 * pseudo-inf",      0x00800000u, 0x7f800001u, 0x40800001u}, // corpus case 876
	{"QTR * PIO2 (FpuMulHack)",  0x3e800000u, 0x40490fdbu, 0x3f490fdau},
	{"PIO2 * QTR (reversed)",    0x40490fdbu, 0x3e800000u, 0x3f490fdbu},
};
} // namespace

TEST(EeRecFpuFull, MulDefectMatchesSiliconInRecMulop)
{
	for (const MulRow& r : kSiliconMulRows)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.EnableFpuFullMode();
		h.SetFprBits(0, r.fs);
		h.SetFprBits(1, r.ft);
		h.LoadProgram({MUL_S(2, 0, 1)});
		h.RunJitNoDiff();
		EXPECT_EQ(h.GetFprBitsJit(2), r.want) << "MUL.S " << r.name;

		EeRecTestHarness ha;
		ha.EnableCop1();
		ha.EnableFpuFullMode();
		ha.SetFprBits(0, r.fs);
		ha.SetFprBits(1, r.ft);
		ha.LoadProgram({MULA_S(0, 1)});
		ha.RunJitNoDiff();
		EXPECT_EQ(ha.GetAccBitsJit(), r.want) << "MULA.S " << r.name;
	}
}

TEST(EeRecFpuFull, MulDefectMatchesSiliconInRecMaddsub)
{
	// ACC = +0 so the accumulate is a no-op on the product's bits: the guard
	// mask reduces a zero operand to its sign and the add leaves the other
	// operand alone, so what lands in fd is the rounded product and nothing
	// else. That is what makes this a test of the multiply stage.
	for (const MulRow& r : kSiliconMulRows)
	{
		const u32 neg = r.want ^ 0x80000000u;
		struct { u32 word; bool is_acc; u32 want; const char* op; } forms[] = {
			{MADD_S(2, 0, 1),  false, r.want, "MADD.S "},
			{MSUB_S(2, 0, 1),  false, neg,    "MSUB.S "},
			{MADDA_S(0, 1),    true,  r.want, "MADDA.S "},
			{MSUBA_S(0, 1),    true,  neg,    "MSUBA.S "},
		};
		for (const auto& f : forms)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			h.EnableFpuFullMode();
			h.SetAccBits(0x00000000u);
			h.SetFprBits(0, r.fs);
			h.SetFprBits(1, r.ft);
			h.LoadProgram({f.word});
			h.RunJitNoDiff();
			const u32 got = f.is_acc ? h.GetAccBitsJit() : h.GetFprBitsJit(2);
			EXPECT_EQ(got, f.want) << f.op << r.name;
		}
	}
}

// The tail test is performed by the rounding, not by an integer tail extract:
// the emitter decrements the double product's bit pattern unconditionally once
// the predicate fires, and one double ULP is strictly below one single ULP, so
// only a product that was exactly representable moves. These rows are the two
// sides of that -- same ft (predicate on for both), fs chosen so the product's
// tail is zero in one row and non-zero in the next. If the decrement ever
// reached a non-zero-tail product it would show up here as an off-by-one.
TEST(EeRecFpuFull, MulDefectOnlyReachesProductsWithAZeroTail)
{
	struct Row { const char* name; u32 fs, ft, want; };
	static const Row kRows[] = {
		// ft = 0x3fbfffff (mantissa 0x3fffff, predicate on).
		{"fs = 2^0,  tail 0",   0x3f800000u, 0x3fbfffffu, 0x3fbffffeu},
		{"fs = 2^-4, tail 0",   0x3d800000u, 0x3fbfffffu, 0x3dbffffeu},
		{"fs = 1+2^-23, tail!=0", 0x3f800001u, 0x3fbfffffu, 0x3fc00000u},
		{"fs = 3.0,  tail!=0",  0x40400000u, 0x3fbfffffu, 0x408fffffu},
	};
	for (const Row& r : kRows)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.EnableFpuFullMode();
		h.SetFprBits(0, r.fs);
		h.SetFprBits(1, r.ft);
		h.LoadProgram({MUL_S(2, 0, 1)});
		h.RunJitNoDiff();
		EXPECT_EQ(h.GetFprBitsJit(2), r.want) << r.name;
	}
}

// A zero product must never be decremented. Under FZ a zero or denormal operand
// widens to +/-0, the product is exactly +/-0, and 0x0000000000000000 - 1 is
// 0xFFFFFFFFFFFFFFFF -- a NaN, which would then narrow to garbage. The Fcmeq
// against the product is the guard, and it covers the interpreter's "zero
// operand" and "flushed result" cases at once, because a product of two EE
// normals is at least ~2^-252 and so is never exactly zero.
//
// The ft values here all have the Booth predicate set, so the guard is the only
// thing standing between these rows and a NaN.
//
// Liveness: unlike the rest of this block this test also passed before the
// deficit landed -- nothing decremented, so nothing could corrupt a zero -- so
// its bidirectional witness is the guard, not the feature. Deleting the
// Fcmeq/Bic pair from emitDefectiveFmul and rebuilding was checked to fail it:
// +0 comes back 0xFFFFFFFF and -0 comes back 0x7FFFFFFF, which is the NaN
// narrowing this pins.
TEST(EeRecFpuFull, MulDefectNeverDecrementsAZeroProduct)
{
	struct Row { const char* name; u32 fs, ft, want; };
	static const Row kRows[] = {
		{"+0 * predicate-on",       0x00000000u, 0x3fbfffffu, 0x00000000u},
		{"-0 * predicate-on",       0x80000000u, 0x3fbfffffu, 0x80000000u},
		{"predicate-on * +0",       0x3fbfffffu, 0x00000000u, 0x00000000u},
		{"denormal ft (flushed)",   0x3f800000u, 0x000002aau, 0x00000000u},
		{"denormal fs (flushed)",   0x000002aau, 0x3fbfffffu, 0x00000000u},
		{"underflow to zero",       0x00800000u, 0x00bfffffu, 0x00000000u},
	};
	for (const Row& r : kRows)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.EnableFpuFullMode();
		h.SetFprBits(0, r.fs);
		h.SetFprBits(1, r.ft);
		h.LoadProgram({MUL_S(2, 0, 1)});
		h.RunJitNoDiff();
		EXPECT_EQ(h.GetFprBitsJit(2), r.want) << r.name;
	}
}



// ---------------------------------------------------------------------------
// The boundary term, the one line of this emitter the two clamp modes do not
// share.
//
// The predicate has two terms: the Booth term `mant & 0x2AA` (bits 1,3,5,7,9,
// the sign bits of the five lowest radix-4 Booth digits) and a boundary term at
// the truncation column, `bit11 != (8 <= (mant >> 12 & 0xF) <= 13)`. They are
// Or'd into one register, so only the operands tell them apart.
//
// Three ft values with the same exponent and the same top mantissa nibble
// (5, so `8 <= nib <= 13` is false throughout and the boundary term reduces to
// bit 11), fs = 1.0 so the product is ft and the tail is zero on all of them:
//
//   0x365015  Booth off, bit11 0 -> neither term    -> exact
//   0x365815  Booth off, bit11 1 -> boundary only   -> one ULP low
//   0x365215  Booth on,  bit11 0 -> Booth only      -> one ULP low
//
// The first two are one bit of ft apart, so together they say the boundary
// term is computed and not always on; the third pins the Booth term on its own.
// The middle row is the only one where the modes answer differently, and both
// answers are asserted.
//
// The interpreter (FPU.cpp eeMulArray) reaches all three independently and is
// asserted alongside the emitters.
TEST(EeRecFpuFull, MulDefectBoundaryTermSeparatesTheClampModes)
{
	constexpr u32 kFs = 0x3f800000u; // 1.0: the product is ft, tail always zero
	struct Row
	{
		u32 ft;
		u32 want;    // interpreter and eeClampMode 4
		u32 want3;   // eeClampMode 3: the Booth term alone
		const char* what;
	};
	constexpr Row kRows[] = {
		{0x48b65015u, 0x48b65015u, 0x48b65015u, "neither term: exact"},
		{0x48b65815u, 0x48b65814u, 0x48b65815u, "boundary term alone: mode 4 only"},
		{0x48b65215u, 0x48b65214u, 0x48b65214u, "Booth term alone: both modes"},
	};

	// One leg per scope: a harness restores the clamp mode in its destructor,
	// so a mode-4 harness still alive carries mode 4 into the next leg.
	auto run = [](u32 ft, int mode, bool interp) {
		EeRecTestHarness h;
		h.EnableCop1();
		if (mode >= 4)
			h.EnableFpuExactMode();
		else if (mode >= 3)
			h.EnableFpuFullMode();
		h.SetFprBits(0, kFs);
		h.SetFprBits(1, ft);
		h.LoadProgram({MUL_S(2, 0, 1)});
		if (interp)
		{
			h.RunInterpOnly();
			return h.GetFprBitsInterp(2);
		}
		h.RunJitNoDiff();
		return h.GetFprBitsJit(2);
	};

	for (const Row& r : kRows)
	{
		SCOPED_TRACE(r.what);
		EXPECT_EQ(run(r.ft, 1, true), r.want) << "interp";
		EXPECT_EQ(run(r.ft, 4, false), r.want) << "eeClampMode 4";
		EXPECT_EQ(run(r.ft, 3, false), r.want3) << "eeClampMode 3";
	}
	// Liveness: exactly one of the three rows separates the modes.
	int split = 0;
	for (const Row& r : kRows)
		split += (r.want != r.want3);
	ASSERT_EQ(split, 1);
}

// ---------------------------------------------------------------------------
// The two classes that separate the clamp modes, against silicon.
//
// Rows from the fpmul3 capture (SCPH-90000, eight fs significands crossed with
// every one of the 2^23 ft significands). `console` is what the console
// returned; `rounded` is the correctly-rounded product the probe computed on
// the EE beside it. Nothing else came back across all 8 x 2^23 rows, so a row
// is described by which of the two it is.
//
// The 8137-case hardware corpus scores the two modes identically: none of its
// 328 zero-tail multiplies reaches the boundary term, their ft mantissas being
// 0x000000, 0x400000 or 0x000001 where the term reads bits 11..15, and of its
// 31 rows inside the array's band 30 saturate past the EE maximum and the last
// is exact.
//
// Neither class can fire mode 3's predicate -- class A has the Booth term off,
// class B has a non-zero tail -- so on every row here mode 3 owes `rounded` and
// mode 4 owes `console`. Half of each table has the two equal.
namespace {
struct MulTierRow
{
	u32 fs, ft, console, rounded;
};

// Class A: the exact product has a zero tail and ft's Booth bits are clear, so
// the boundary term at the truncation column is the only thing that can move
// the row.
constexpr MulTierRow kBoundaryTermRows[] = {
	{0x3f900000u, 0x3f8fa000u, 0x3fa193ffu, 0x3fa19400u},
	{0x3f900000u, 0x3f8fa800u, 0x3fa19d00u, 0x3fa19d00u},
	{0x3f900000u, 0x3ff33000u, 0x4008cb00u, 0x4008cb00u},
	{0x3f900000u, 0x3ff33800u, 0x4008cf7fu, 0x4008cf80u},
	{0x3fa00000u, 0x3f87d000u, 0x3fa9c3ffu, 0x3fa9c400u},
	{0x3fa00000u, 0x3f87d800u, 0x3fa9ce00u, 0x3fa9ce00u},
	{0x3fa00000u, 0x3fdc6500u, 0x4009bf20u, 0x4009bf20u},
	{0x3fa00000u, 0x3fdc7800u, 0x4009caffu, 0x4009cb00u},
	{0x3fc00000u, 0x3f87d000u, 0x3fcbb7ffu, 0x3fcbb800u},
	{0x3fc00000u, 0x3f87d800u, 0x3fcbc400u, 0x3fcbc400u},
	{0x3fc00000u, 0x3fb27400u, 0x4005d700u, 0x4005d700u},
	{0x3fc00000u, 0x3fb28000u, 0x4005dfffu, 0x4005e000u},
	{0x3fe00000u, 0x3f87d000u, 0x3fedabffu, 0x3fedac00u},
	{0x3fe00000u, 0x3f87d800u, 0x3fedba00u, 0x3fedba00u},
	{0x3fe00000u, 0x3fa1e940u, 0x400dac17u, 0x400dac18u},
	{0x3fe00000u, 0x3fa1f000u, 0x400db200u, 0x400db200u},
	{0x3ff00000u, 0x3f982100u, 0x400e9ef0u, 0x400e9ef0u},
	{0x3ff00000u, 0x3f983800u, 0x400eb47fu, 0x400eb480u},
	{0x3fff0000u, 0x3fbf0000u, 0x403e4100u, 0x403e4100u},
	{0x3fff0000u, 0x3fbf0900u, 0x403e49f6u, 0x403e49f7u},
};

// Class B: the tail is non-zero but below the array's 2^15 borrow, so no
// function of ft can decide the row and only reconstructing the array's
// truncated columns does.
constexpr MulTierRow kArrayBandRows[] = {
	{0x3fbfffffu, 0x3fbf8961u, 0x400fa708u, 0x400fa708u},
	{0x3fbfffffu, 0x3fbf92d1u, 0x400fae1cu, 0x400fae1cu},
	{0x3fbfffffu, 0x3fbfb47du, 0x400fc75cu, 0x400fc75du},
	{0x3fbfffffu, 0x3fbfbf69u, 0x400fcf8du, 0x400fcf8eu},
	{0x3fd2b4c1u, 0x3f83fa5cu, 0x3fd9411eu, 0x3fd9411eu},
	{0x3fd2b4c1u, 0x3f85cb9cu, 0x3fdc3efbu, 0x3fdc3efcu},
	{0x3fd2b4c1u, 0x3f87f17cu, 0x3fdfc828u, 0x3fdfc828u},
	{0x3fd2b4c1u, 0x3f8bb3c4u, 0x3fe5f834u, 0x3fe5f835u},
	{0x3fd2b4c1u, 0x3fa398c7u, 0x4006a6d6u, 0x4006a6d6u},
	{0x3fd2b4c1u, 0x3fa6df9cu, 0x40095940u, 0x40095941u},
	{0x3fd2b4c1u, 0x3fab9889u, 0x400d3c49u, 0x400d3c49u},
	{0x3fd2b4c1u, 0x3fb23f42u, 0x4012b5beu, 0x4012b5bfu},
};

// One leg per scope: a harness restores the clamp mode in its destructor, so
// one still alive carries its mode into the next leg.
u32 RunMulTierRow(const MulTierRow& r, int mode, bool interp, bool madd)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (mode >= 4)
		h.EnableFpuExactMode();
	else if (mode >= 3)
		h.EnableFpuFullMode();
	h.SetAccBits(0x00000000u); // +0, so MADD lands on the product alone
	h.SetFprBits(0, r.fs);
	h.SetFprBits(1, r.ft);
	h.LoadProgram({madd ? MADD_S(2, 0, 1) : MUL_S(2, 0, 1)});
	if (interp)
	{
		h.RunInterpOnly();
		return h.GetFprBitsInterp(2);
	}
	h.RunJitNoDiff();
	return h.GetFprBitsJit(2);
}

// Both emit sites, since recMULop and recMaddsub's multiply stage narrow
// through different code.
void ExpectMulTier(const MulTierRow& r)
{
	for (bool madd : {false, true})
	{
		SCOPED_TRACE(madd ? "MADD.S" : "MUL.S");
		EXPECT_EQ(RunMulTierRow(r, 1, true, madd), r.console) << "interp";
		EXPECT_EQ(RunMulTierRow(r, 4, false, madd), r.console) << "eeClampMode 4";
		EXPECT_EQ(RunMulTierRow(r, 3, false, madd), r.rounded) << "eeClampMode 3";
	}
}

int SeparatingRows(const MulTierRow* rows, size_t n)
{
	int k = 0;
	for (size_t i = 0; i < n; i++)
		k += (rows[i].console != rows[i].rounded);
	return k;
}
} // namespace

TEST(EeRecFpuFull, MulDeficitBoundaryTermAgainstTheConsole)
{
	for (const MulTierRow& r : kBoundaryTermRows)
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "fs=" << r.fs << " ft=" << r.ft);
		ExpectMulTier(r);
	}
	// Liveness, both ways: rows the term moves, and rows it must not.
	const int sep = SeparatingRows(kBoundaryTermRows, std::size(kBoundaryTermRows));
	EXPECT_GT(sep, 0) << "no row left where the boundary term decides";
	EXPECT_LT(sep, static_cast<int>(std::size(kBoundaryTermRows)))
		<< "every row decides, so an emitter that always decremented would pass";
}

TEST(EeRecFpuFull, MulDeficitArrayBandAgainstTheConsole)
{
	for (const MulTierRow& r : kArrayBandRows)
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "fs=" << r.fs << " ft=" << r.ft);
		ExpectMulTier(r);
	}
	const int sep = SeparatingRows(kArrayBandRows, std::size(kArrayBandRows));
	EXPECT_GT(sep, 0) << "no row left inside the band that the array moves";
	EXPECT_LT(sep, static_cast<int>(std::size(kArrayBandRows)))
		<< "every row moves, so an emitter that always decremented would pass";
}

// Both tables above are exponent 127 with both operands positive, because that
// is what the fpmul3 sweep covered. Every model of the deficit in the tree
// reads the significands and nothing else, so these rows put the other two
// fields on the console: captures/fpmulsign, four sign combinations across five
// exponent placements for eight separating operand pairs. All 160 came back one
// ULP low, and the extremes are kept here.
//
// The operands are the ones above with their exponent fields moved, so the
// significands are unchanged; a row that fails here and passes there is an
// emitter reading the exponent or the sign.
constexpr MulTierRow kExponentAndSignRows[] = {
	{0x12C00000u, 0x30F5A104u, 0x043838C2u, 0x043838C3u},
	{0x92C00000u, 0x30F5A104u, 0x843838C2u, 0x843838C3u},
	{0x12C00000u, 0xB0F5A104u, 0x843838C2u, 0x843838C3u},
	{0x92C00000u, 0xB0F5A104u, 0x043838C2u, 0x043838C3u},
	{0x00C00000u, 0x7EF5A104u, 0x403838C2u, 0x403838C3u},
	{0x80C00000u, 0x7EF5A104u, 0xC03838C2u, 0xC03838C3u},
	{0x00C00000u, 0xFEF5A104u, 0xC03838C2u, 0xC03838C3u},
	{0x80C00000u, 0xFEF5A104u, 0x403838C2u, 0x403838C3u},
	{0x00E00000u, 0x7EA2EC50u, 0x400E8EC5u, 0x400E8EC6u},
	{0x80E00000u, 0x7EA2EC50u, 0xC00E8EC5u, 0xC00E8EC6u},
	{0x00E00000u, 0xFEA2EC50u, 0xC00E8EC5u, 0xC00E8EC6u},
	{0x80E00000u, 0xFEA2EC50u, 0x400E8EC5u, 0x400E8EC6u},
	{0x00A00000u, 0x7ED47C40u, 0x4004CDA7u, 0x4004CDA8u},
	{0x80A00000u, 0x7ED47C40u, 0xC004CDA7u, 0xC004CDA8u},
	{0x00A00000u, 0xFED47C40u, 0xC004CDA7u, 0xC004CDA8u},
	{0x80A00000u, 0xFED47C40u, 0x4004CDA7u, 0x4004CDA8u},
};

TEST(EeRecFpuFull, MulDeficitIgnoresTheExponentAndTheSigns)
{
	int signs = 0;
	for (const MulTierRow& r : kExponentAndSignRows)
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "fs=" << r.fs << " ft=" << r.ft);
		ExpectMulTier(r);
		signs |= 1 << (((r.fs >> 31) << 1) | (r.ft >> 31));
	}
	EXPECT_EQ(signs, 0xF) << "not all four sign combinations are still here";
	// Every row here separates the two modes; the control against an emitter
	// that decrements unconditionally is ExpectMulTier()'s mode 3 leg.
	EXPECT_EQ(SeparatingRows(kExponentAndSignRows, std::size(kExponentAndSignRows)),
		static_cast<int>(std::size(kExponentAndSignRows)));
}

// ---------------------------------------------------------------------------
// Randomised differential: mode 4 against the interpreter, which reaches the
// same answers in completely different code (FPU.cpp eeMulArray reconstructs
// the array's truncated low columns in integers; iFPUd reads the predicate off
// ft's slot bits and the tail off the double product).
//
// Dimensions varied and crossed: six operand classes on each side (arbitrary
// words, random normals, powers of two -- which force a zero tail and so make
// the predicate decide every row, the top binade where exp == 0xff is an
// ordinary EE number, the minimum-normal binade, and denormal/zero), operand
// order (the law is not commutative, and both sides draw from the same classes),
// register aliasing (fd == fs and fd == ft), and both emit sites (recMULop and
// recMaddsub's multiply stage, reached with ACC = +0 so the accumulate is a
// no-op on the product's bits).
//
// Mode 4 answers every row the interpreter's way, the inline predicate
// deciding the zero-tail rows and the island's call to eeMulOneUlpLow deciding
// the band below the array's borrow. The two classes that need those parts are
// counted and asserted non-empty, classified from the exact 48-bit significand
// product so that neither engine can license itself.
TEST(EeRecFpuFull, MulDefectRandomisedDifferentialAgainstTheInterpreter)
{
	auto splitmix = [](u64& state) {
		u64 z = (state += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	};
	auto pick = [&splitmix](u64& state, int cls) -> u32 {
		const u64 r = splitmix(state);
		const u32 sign = (r >> 63) ? 0x80000000u : 0u;
		const u32 mant = static_cast<u32>(r) & 0x7FFFFFu;
		switch (cls)
		{
			case 0: return static_cast<u32>(r);                                    // anything
			case 1: return sign | ((static_cast<u32>(r >> 32) % 254u + 1u) << 23) | mant; // normal
			case 2: return sign | ((static_cast<u32>(r >> 32) % 254u + 1u) << 23); // power of two
			case 3: return sign | 0x7f800000u | mant;                              // top binade
			case 4: return sign | (1u << 23) | mant;                               // min normal
			default: return sign | mant;                                           // denormal/zero
		}
	};
	// Both terms of the zero-tail closed form, so a divergence can be classified
	// rather than merely counted.
	auto booth = [](u32 ft) { return (ft & 0x2AAu) != 0; };
	auto full = [](u32 ft) {
		const u32 m = ft & 0x7FFFFFu;
		if (m & 0x2AAu)
			return true;
		const u32 h = (m >> 12) & 0xFu;
		return ((m >> 11) & 1u) != ((h >= 8u && h <= 13u) ? 1u : 0u);
	};
	// The tail below the single ULP, from the exact 48-bit significand product.
	// A zero operand contributes none: the product is zero.
	auto tailBelowUlp = [](u32 fs, u32 ft) -> u64 {
		if ((fs & 0x7F800000u) == 0 || (ft & 0x7F800000u) == 0)
			return 0;
		const u64 a = 0x800000u | (fs & 0x7FFFFFu);
		const u64 b = 0x800000u | (ft & 0x7FFFFFu);
		const u64 p = a * b;
		return p & ((1ull << ((p >> 47) ? 24 : 23)) - 1u);
	};

	u64 state = 0x1234567890ABCDEFull;
	int rows = 0, boundary_rows[4] = {0, 0, 0, 0}, subulp_rows = 0;
	for (int i = 0; i < 40000; i++)
	{
		const int cs = static_cast<int>(splitmix(state) % 6);
		const int ct = static_cast<int>(splitmix(state) % 6);
		const u32 fs = pick(state, cs), ft = pick(state, ct);

		const int form = i % 4;
		u32 word = 0;
		int dst = 2;
		switch (form)
		{
			case 0: word = MUL_S(2, 0, 1); dst = 2; break;   // recMULop, distinct fd
			case 1: word = MUL_S(0, 0, 1); dst = 0; break;   // recMULop, fd == fs
			case 2: word = MUL_S(1, 0, 1); dst = 1; break;   // recMULop, fd == ft
			default: word = MADD_S(2, 0, 1); dst = 2; break; // recMaddsub multiply stage
		}

		EeRecTestHarness h;
		h.EnableCop1();
		h.EnableFpuExactMode();
		h.SetAccBits(0);
		h.SetFprBits(0, fs);
		h.SetFprBits(1, ft);
		h.LoadProgram({word});
		h.RunJitNoDiff();
		const u32 jit = h.GetFprBitsJit(dst);

		EeRecTestHarness hi;
		hi.EnableCop1();
		hi.SetAccBits(0);
		hi.SetFprBits(0, fs);
		hi.SetFprBits(1, ft);
		hi.LoadProgram({word});
		hi.RunInterpOnly();
		const u32 interp = hi.GetFprBitsInterp(dst);

		rows++;
		const u64 tail = tailBelowUlp(fs, ft);
		// The class the Booth term cannot see, and the class no function of ft
		// can see at all. Both are counted whether they agreed or not.
		if (tail == 0 && !booth(ft) && full(ft))
			boundary_rows[form]++;
		if (tail != 0 && tail < 0x8000u)
			subulp_rows++;

		ASSERT_EQ(jit, interp)
			<< "form=" << form << " cs=" << cs << " ct=" << ct
			<< std::hex << " fs=" << fs << " ft=" << ft << " tail=" << tail;
	}

	EXPECT_EQ(rows, 40000);
	// Liveness, per class. The boundary term is asserted at both emit sites
	// because they narrow through different code.
	EXPECT_GT(boundary_rows[0] + boundary_rows[1] + boundary_rows[2], 0)
		<< "recMULop never reached the boundary-term class: the sweep is vacuous";
	EXPECT_GT(boundary_rows[3], 0)
		<< "recMaddsub never reached the boundary-term class: the sweep is vacuous";
	EXPECT_GT(subulp_rows, 0)
		<< "the sweep never reached a non-zero tail below the borrow, so it "
		   "never entered the island and says nothing about the call";
}

// ---------------------------------------------------------------------------
// The island's call, with the allocator holding as much as it can. The tests
// above reach it from one-instruction programs, where almost nothing is
// resident. The call is plain AAPCS -- it clobbers x0-x18 and every vector
// register bar the low halves of q8-q15 -- so a block's live values are
// protected by the spill emitted around it and the pin reload after it.
//
// Each register below is used once before the multiply and once after, so the
// allocator has a reason to hold it across. FPRs and GPRs both, saved by
// different loops.
//
// $at, $s0 and $k0 are the caller-saved EE pin mirrors rather than allocator
// state: the island flushes them before the call and reloads them after, and a
// reload without the flush loses the block's writes to them.
TEST(EeRecFpuFull, MulArrayIslandPreservesTheAllocatorsLiveRegisters)
{
	// tail 0x2: below the array's borrow, so this is a row that calls out.
	constexpr u32 kFs = 0x3F800001u, kFt = 0x3F800002u;
	static const u32 kPinned[] = {1, 16, 26}; // $at, $s0, $k0

	std::vector<u32> prog;
	auto body = [&prog]() {
		for (u32 f = 4; f < 16; f += 2)
			prog.push_back(ADD_S(f, f, f + 1));
		for (u32 r = 8; r < 14; r += 2)
			prog.push_back(DADDU(r, r, r + 1));
		for (u32 r : kPinned)
			prog.push_back(DADDU(r, r, 8));
	};
	body();
	prog.push_back(MUL_S(2, 0, 1));
	body();

	auto seed = [&](EeRecTestHarness& h) {
		h.EnableCop1();
		h.SetFprBits(0, kFs);
		h.SetFprBits(1, kFt);
		for (u32 f = 4; f < 16; f++)
			h.SetFprBits(f, 0x3F800000u + (f << 16)); // distinct, exactly representable
		for (u32 r = 8; r < 14; r++)
			h.SetGpr64(r, 0x0123456789ABCDEFull * (r + 1));
		for (u32 r : kPinned)
			h.SetGpr64(r, 0xFEDCBA9876543210ull * (r + 1));
	};

	EeRecTestHarness hi;
	seed(hi);
	hi.LoadProgram(prog);
	hi.RunInterpOnly();

	EeRecTestHarness h;
	h.EnableFpuExactMode();
	seed(h);
	h.LoadProgram(prog);
	h.RunJitNoDiff();

	EXPECT_EQ(h.GetFprBitsJit(2), 0x3F800002u) << "the island's own result";
	for (u32 f = 4; f < 16; f++)
		EXPECT_EQ(h.GetFprBitsJit(f), hi.GetFprBitsInterp(f)) << "f" << f;
	for (u32 r = 8; r < 14; r++)
		EXPECT_EQ(h.GetGpr64Jit(r), hi.GetGpr64Interp(r)) << "r" << r;
	for (u32 r : kPinned)
		EXPECT_EQ(h.GetGpr64Jit(r), hi.GetGpr64Interp(r)) << "pinned r" << r;
}


// ---------------------------------------------------------------------------
// The predicate's operand: the allocator-resident guest ft.
//
// emitDefectiveFmul indexes bits 30..44 of ft's slot, read straight out of
// whatever NEON register the allocator has it in. That is a 64-bit read of a
// register the emitter does not own, so it is only correct while the full slot
// is there -- through a C-call seam (iFlushCall retains FPR-class slots in the
// callee-saved range, where AAPCS64 preserves the low 64 bits and nothing
// above), through inline macro-mode emit, and through a spill and reload.
//
// A 32-bit fill, a retention that kept only the single, or an allocator that
// let something else into the register leaves those bits wrong with no crash
// and no corrupt value -- just a wrong rounding decision on a fraction of
// multiplies. Every test below checks both polarities:
//
//   bits 30..44 read as zero  -> the predicate never fires -> every product
//                                correctly rounded, one ULP high wherever
//                                silicon is short
//   bits 30..44 read garbage  -> it fires on operands it must not -> products
//                                one ULP low where silicon is exact
//
// The 1147-case hardware corpus is single ops, so no multiply in it is far
// enough into a block to have been moved.
namespace {
// Two rows from kSiliconMulRows above, chosen as a matched pair on one pair of
// registers -- f0 = 1.0, f1 = +FLT_MAX -- so a single block can ask the
// question both ways just by swapping the operand order:
//
//   MUL_S(d, 0, 1)   ft = f1, mantissa 0x7fffff  -> predicate on,  0x7f7ffffe
//   MUL_S(d, 1, 0)   ft = f0, mantissa 0         -> predicate off, 0x7f7fffff
//
// Both measured on an SCPH-90000; this is the non-commutativity of mul.s, which
// is the sharpest available probe because the two answers differ by exactly the
// decrement the predicate controls.
constexpr u32 kPredFprOne  = 0x3f800000u; // f0 = 1.0
constexpr u32 kPredFprMax  = 0x7f7fffffu; // f1 = +FLT_MAX
constexpr u32 kPredOnWant  = 0x7f7ffffeu; // 1.0 * FLT_MAX: silicon is one ULP low
constexpr u32 kPredOffWant = 0x7f7fffffu; // FLT_MAX * 1.0: silicon is exact

// Assert the matched pair in fd_on / fd_off.
void ExpectPredicateLive(EeRecTestHarness& h, u32 fd_on, u32 fd_off, const char* where)
{
	EXPECT_EQ(h.GetFprBitsJit(fd_on), kPredOnWant)
		<< where << ": predicate did not fire -- ft's slot bits read as zero";
	EXPECT_EQ(h.GetFprBitsJit(fd_off), kPredOffWant)
		<< where << ": predicate fired on ft mantissa 0 -- ft's slot bits hold garbage";
}
} // namespace

// A VCALLMS in the middle of the block is both runtime hazards at once: it is a
// real in-block C-call seam (the callee may clobber any caller-saved register),
// and it dispatches a VU0 microprogram, whose blocks allocate NEON slots q0-q27
// freely. A retained ft survives the first because AAPCS64 preserves the low 64
// bits of v8-v15, the whole slot, and the second because mVUdispatcherAB's
// prologue Stp/Ldp-saves d8-d15 around the dispatch (see the VE-04 note in
// microVU-arm64.cpp).
//
// Both emit sites, since recMULop and recMaddsub reach the seam with different
// registers live.
TEST(EeRecFpuFull, MulDefectPredicateSurvivesAnInBlockCallSeam)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.EnableVu0Capture();
	h.SeedVu0Vi(REG_VPU_STAT, 0);
	// Trivial immediate-E micro: the VCALLMS is here purely as the seam.
	h.SeedVu0Microprogram(0, {vu::EBitNopPair(), vu::NopPair()});
	h.SetAccBits(0x00000000u);
	h.SetFprBits(0, kPredFprOne);
	h.SetFprBits(1, kPredFprMax);
	h.LoadProgram({
		MUL_S(2, 0, 1),
		MUL_S(3, 1, 0),
		VCALLMS(0),
		MUL_S(4, 0, 1),  // recMULop, post-seam
		MUL_S(5, 1, 0),
		VCALLMS(0),
		MADD_S(6, 0, 1), // recMaddsub's multiply stage, post-seam (ACC = +0)
		MADD_S(7, 1, 0),
	});
	h.RunJitNoDiff();

	ExpectPredicateLive(h, 2, 3, "pre-seam MUL.S");
	ExpectPredicateLive(h, 4, 5, "post-seam MUL.S");
	ExpectPredicateLive(h, 6, 7, "post-seam MADD.S");
	// Liveness: the two expectations above discriminate only because the two
	// answers differ. If this ever fires the rows stopped being a matched pair
	// and the test is vacuous whatever it reports.
	ASSERT_NE(kPredOnWant, kPredOffWant);
}

// COP2 macro mode is the one context that emits mVU code inline in an EE block,
// with no dispatcher save around it, so an EE FPR left resident across it is
// protected by nothing but the mVU allocator's own bound: macro emit is limited
// to NEON slots 0-3 by kMacroVFEvictHighWater, which mVUmacroEmitEpilogue
// asserts on every macro op. Two macro FMACs between the multiplies give that
// allocator something to spend registers on.
TEST(EeRecFpuFull, MulDefectPredicateSurvivesInlineCop2MacroMode)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.EnableVu0Capture();
	h.SeedVu0Vi(REG_VPU_STAT, 0);
	h.SeedVu0Vf(1, 1.0f, 2.0f, 3.0f, 4.0f);
	h.SeedVu0Vf(2, 5.0f, 6.0f, 7.0f, 8.0f);
	h.SetFprBits(0, kPredFprOne);
	h.SetFprBits(1, kPredFprMax);
	h.LoadProgram({
		MUL_S(2, 0, 1),
		MUL_S(3, 1, 0),
		VMUL_C2(0xf, 3, 1, 2),
		VADD_C2(0xf, 4, 1, 2),
		MUL_S(4, 0, 1), // post-macro
		MUL_S(5, 1, 0),
	});
	h.RunJitNoDiff();

	ExpectPredicateLive(h, 2, 3, "pre-macro MUL.S");
	ExpectPredicateLive(h, 4, 5, "MUL.S after inline COP2 macro emit");
	ASSERT_NE(kPredOnWant, kPredOffWant);
}

// Sixteen simultaneously live FPRs is more than the pool has, so ft goes out to
// its slot in fpuRegs and comes back. A 32-bit fill and spill would round-trip
// the word and lose the domain, leaving bits 30..44 clear on the reload.
TEST(EeRecFpuFull, MulDefectPredicateSurvivesHeavyFprPressure)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kPredFprOne);
	h.SetFprBits(1, kPredFprMax);

	std::vector<u32> prog;
	for (u32 r = 6; r < 22; r++)
	{
		h.SetFprBits(r, 0x3f800000u + r);
		prog.push_back(ADD_S(r, r, 0)); // touch it: r = r + 1.0
	}
	prog.push_back(MUL_S(2, 0, 1));
	prog.push_back(MUL_S(3, 1, 0));
	for (u32 r = 6; r < 22; r++)
		prog.push_back(ADD_S(r, r, 0)); // keep every one live past the multiplies
	prog.push_back(MUL_S(4, 0, 1));
	prog.push_back(MUL_S(5, 1, 0));
	h.LoadProgram(prog);
	h.RunJitNoDiff();

	ExpectPredicateLive(h, 2, 3, "MUL.S under FPR pressure");
	ExpectPredicateLive(h, 4, 5, "MUL.S after 16 live FPRs");
	ASSERT_NE(kPredOnWant, kPredOffWant);
}

// ---------------------------------------------------------------------------
// The deficit in the upper binade, and with fs not a power of two.
//
// Runs 1 and 2 swept four fs significands and established the law, but look at
// which cells they actually populated. The predicate can only change a result
// when the exact product has nothing below the ULP (T == 0), and across all
// 33,554,432 of their rows:
//
//     product <  2^47 (lower binade), T == 0   8,388,608 rows, all at fs = 2^23
//     product >= 2^47 (upper binade), T == 0           0 rows
//
// So the region where this emitter's decrement actually fires when the product
// lands in the upper binade had never been observed on hardware -- and it is
// not exotic: fs = 1.5 puts 1,398,101 of 2^23 ft values there. It mattered
// because the truncation column moves one bit between the binades, so a
// predicate keyed to fixed bit positions of ft has no a-priori reason to
// survive the shift.
//
// Settled from the fpmul3 capture (8 further fs sweeps, SCPH-90000, FCR0
// 0x2e40), which had been taken for a different question and never analysed by
// binade. Pooled over its 7,196,506 T == 0 rows -- 3,354,792 of them in the
// upper binade, at six fs values with odd parts 3, 5, 7, 9, 15 and 255:
//
//     emitted Booth-only predicate   6,738,214 correct   229,142 missed   0 wrong
//     interpreter's full predicate   6,967,356 correct           0 missed 0 wrong
//
// Zero wrong in either binade: the emitter never claims a deficit where silicon
// is exact, which is the one direction that would be a regression. The rows
// below are three witnesses per fs, generated from that capture rather than
// typed, so provenance is mechanical.
namespace {
struct BinadeRow { u32 fs, ft, want; };

// Upper binade, T == 0, Booth fires -> silicon is one ULP low. The emitter must
// reproduce every one of these.
constexpr BinadeRow kTopBinadeFires[] = {
	{0x3fc00000u, 0x3faaaaacu, 0x40000000u}, // fs 1.5:    M = 2^47 + 2^24 exactly
	{0x3fe00000u, 0x3f924928u, 0x40000002u}, // fs 1.75
	{0x3fa00000u, 0x3fccccd0u, 0x40000001u}, // fs 1.25
	{0x3f900000u, 0x3fe38e40u, 0x40000003u}, // fs 1.125
	{0x3ff00000u, 0x3f888890u, 0x40000006u}, // fs 1.875
	{0x3fff0000u, 0x3f808200u, 0x4000017du}, // fs ~1.996
};

// Upper binade, T == 0, neither term of the predicate fires -> silicon is
// exact. This is the anti-regression direction: a predicate that over-fires in
// the upper binade would move these one ULP away from hardware.
constexpr BinadeRow kTopBinadeSilent[] = {
	{0x3fc00000u, 0x3faaac00u, 0x40000100u},
	{0x3fe00000u, 0x3f925000u, 0x40000600u},
	{0x3fa00000u, 0x3fcccd00u, 0x40000020u},
	{0x3f900000u, 0x3fe39800u, 0x40000580u},
	{0x3ff00000u, 0x3f888900u, 0x40000070u},
	{0x3fff0000u, 0x3f808800u, 0x40000778u},
};

// Upper binade, T == 0, only the boundary term fires -> silicon is one ULP low.
// The truncation column moves one bit between the binades, so a term keyed to
// fixed bit positions of ft need not survive the shift.
constexpr BinadeRow kTopBinadeBoundary[] = {
	{0x3fc00000u, 0x3faab000u, 0x400003ffu},
	{0x3fe00000u, 0x3f924940u, 0x40000017u},
	{0x3fa00000u, 0x3fccd000u, 0x400001ffu},
	{0x3f900000u, 0x3fe39000u, 0x400000ffu},
	{0x3ff00000u, 0x3f889000u, 0x400006ffu},
	{0x3fff0000u, 0x3f808100u, 0x4000007eu},
};

// Both emit sites: recMULop narrows with ToPS2FPU_Full's Fcvt, recMaddsub's
// multiply stage with ToPS2FPU_Wide's mask-off-the-low-29. Those two land on
// different sides of the mantissa boundary that the binade shifts, so the
// upper binade has to be checked through both.
//
// `mode` is the clamp mode the rows need: the Booth term is on 3 and 4, the
// boundary term only on 4.
void ExpectBothSites(const BinadeRow& r, u32 want, const char* what, int mode = 3)
{
	auto tier = [mode](EeRecTestHarness& h) {
		if (mode >= 4)
			h.EnableFpuExactMode();
		else
			h.EnableFpuFullMode();
	};

	EeRecTestHarness h;
	h.EnableCop1();
	tier(h);
	h.SetFprBits(0, r.fs);
	h.SetFprBits(1, r.ft);
	h.LoadProgram({MUL_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), want)
		<< what << " MUL.S fs=" << std::hex << r.fs << " ft=" << r.ft;

	EeRecTestHarness hm;
	hm.EnableCop1();
	tier(hm);
	hm.SetAccBits(0x00000000u); // ACC = +0 -> fd is the rounded product alone
	hm.SetFprBits(0, r.fs);
	hm.SetFprBits(1, r.ft);
	hm.LoadProgram({MADD_S(2, 0, 1)});
	hm.RunJitNoDiff();
	EXPECT_EQ(hm.GetFprBitsJit(2), want)
		<< what << " MADD.S fs=" << std::hex << r.fs << " ft=" << r.ft;
}
} // namespace

TEST(EeRecFpuFull, MulDefectFiresInTheUpperBinade)
{
	for (const BinadeRow& r : kTopBinadeFires)
		ExpectBothSites(r, r.want, "upper-binade deficit");
}

// The direction that would be a regression, and the reason this capture was
// analysed at all: 3,092,991 upper-binade rows where the emitter fires, 0 of
// them wrong. These pin the complement -- it must stay silent where silicon is.
TEST(EeRecFpuFull, MulDefectStaysSilentInTheUpperBinadeWhereSiliconIsExact)
{
	for (const BinadeRow& r : kTopBinadeSilent)
		ExpectBothSites(r, r.want, "upper-binade exact");
}

// The boundary term reaches the upper binade too: rows the Booth term cannot
// see, so they pin it where exp == 0xff is an ordinary EE number.
TEST(EeRecFpuFull, MulDefectBoundaryTermAlsoFiresInTheUpperBinade)
{
	for (const BinadeRow& r : kTopBinadeBoundary)
	{
		ExpectBothSites(r, r.want, "upper-binade boundary term", 4);

		EeRecTestHarness hi;
		hi.EnableCop1();
		hi.SetFprBits(0, r.fs);
		hi.SetFprBits(1, r.ft);
		hi.LoadProgram({MUL_S(2, 0, 1)});
		hi.RunInterpOnly();
		EXPECT_EQ(hi.GetFprBitsInterp(2), r.want)
			<< "interp models the boundary term in the upper binade too";
	}
}

// ---------------------------------------------------------------------------
// ADD/SUB-family underflow: FULL mode keeps the mantissa bits.
//
// When a full-mode ADD/SUB/ADDA/SUBA/MADD/MSUB/MADDA/MSUBA result lands below
// 2^-126 but is not zero, ToPS2FPU_Full's `addsub` arm does NOT flush it: it
// copies the double result's mantissa bits [51:29] into the single's [22:0]
// with an exponent field of 0, keeping the sign. x86 iFPUd.cpp does the same
// and says why:
//
//     //On ADD/SUB, the PS2 simply leaves the mantissa bits as they are
//     //(after normalization)
//     //IEEE either clears them (FtZ) or returns the denormalized result.
//     //not thoroughly tested : other operations such as MUL and DIV seem to
//     //clear all mantissa bits?
//
// So this is a deliberate model of the EE's adder -- not a rounding artifact.
// It is NOT a value conversion: 0x00800003 + 0x80800000 is exactly 3*2^-149,
// and the model returns 0x00400000 == 2^-127, six orders of magnitude larger.
// The bits, not the value, are what it claims to reproduce.
//
// The console says this path is right: EeFpuUnderflowConsole
// (ee_fpu_underflow_console_tests.cpp) carries the hardware rows and scores all
// three engines against them. Mode 3 was the only engine that had it; the
// interpreter has since been fixed and the fast path is pinned there as a
// divergence.
//
// These rows stay here as the mode-3 pin: the format-churn work moves values
// between the single and double domains and must not flatten this on the way.
//
// MUL/MULA are the negative control: addsub=false, so they flush to signed zero
// on all three engines and their rows must NOT show a mantissa.
TEST(EeRecFpuFull, AddSubUnderflowKeepsTheMantissaBits)
{
	struct Row { const char* name; u32 acc, fs, ft; u32 word; bool is_acc; u32 expected; };
	// Every operand pair below is two normal singles whose exact result is a few
	// ULPs of 2^-149: 0x00800003 == (1 + 3*2^-23) * 2^-126, so subtracting
	// 2^-126 leaves 3*2^-149, whose double form is 1.1b * 2^-148 -- mantissa
	// bit 51 set, everything under it clear, hence 0x00400000. The 0x00FFFFFF
	// row is the top of the range: the exact sum is 0x7FFFFF*2^-149 and the
	// model returns 0x007FFFFE, one ULP low.
	static const Row kRows[] = {
		{"ADD.S   3*2^-149",          0, 0x00800003u, 0x80800000u, ADD_S(2, 0, 1),   false, 0x00400000u},
		{"ADD.S   -3*2^-149",         0, 0x80800003u, 0x00800000u, ADD_S(2, 0, 1),   false, 0x80400000u},
		{"ADD.S   1*2^-149 (mant 0)", 0, 0x00800001u, 0x80800000u, ADD_S(2, 0, 1),   false, 0x00000000u},
		{"ADD.S   0x7FFFFF*2^-149",   0, 0x00FFFFFFu, 0x80800000u, ADD_S(2, 0, 1),   false, 0x007FFFFEu},
		{"SUB.S   3*2^-149",          0, 0x00800003u, 0x00800000u, SUB_S(2, 0, 1),   false, 0x00400000u},
		{"SUB.S   -3*2^-149",         0, 0x80800003u, 0x80800000u, SUB_S(2, 0, 1),   false, 0x80400000u},
		{"ADDA.S  3*2^-149",          0, 0x00800003u, 0x80800000u, ADDA_S(0, 1),     true,  0x00400000u},
		{"SUBA.S  3*2^-149",          0, 0x00800003u, 0x00800000u, SUBA_S(0, 1),     true,  0x00400000u},
		{"MADD.S  3*2^-149",  0x80800000u, 0x00800003u, 0x3f800000u, MADD_S(2, 0, 1),  false, 0x00400000u},
		{"MSUB.S  -3*2^-149", 0x00800000u, 0x00800003u, 0x3f800000u, MSUB_S(2, 0, 1),  false, 0x80400000u},
		{"MADDA.S 3*2^-149",  0x80800000u, 0x00800003u, 0x3f800000u, MADDA_S(0, 1),    true,  0x00400000u},
		{"MSUBA.S -3*2^-149", 0x00800000u, 0x00800003u, 0x3f800000u, MSUBA_S(0, 1),    true,  0x80400000u},
		// Negative controls: the multiplies take addsub=false and must flush.
		{"MUL.S   2^-252",            0, 0x00800000u, 0x00800000u, MUL_S(2, 0, 1),   false, 0x00000000u},
		{"MULA.S  2^-252",            0, 0x00800000u, 0x00800000u, MULA_S(0, 1),     true,  0x00000000u},
		{"MUL.S   ~2^-251",           0, 0x00FFFFFFu, 0x00800000u, MUL_S(2, 0, 1),   false, 0x00000000u},
	};

	for (const Row& r : kRows)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.EnableFpuFullMode();
		h.SetFcr31(0);
		h.SetAccBits(r.acc);
		h.SetFprBits(0, r.fs);
		h.SetFprBits(1, r.ft);
		h.LoadProgram({r.word});
		h.RunJitNoDiff();

		const u32 got = r.is_acc ? h.GetAccBitsJit() : h.GetFprBitsJit(2);
		EXPECT_EQ(got, r.expected) << r.name;
		// Every row underflows to a nonzero double, so U|SU is raised even on
		// the rows whose reconstructed mantissa happens to be zero.
		EXPECT_EQ(h.JitSnapshot().fprs.fprc[31], kFPUflagU | kFPUflagSU) << r.name;
	}
}
