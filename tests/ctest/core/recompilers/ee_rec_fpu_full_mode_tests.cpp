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

#include <cfloat>
#include <cmath>
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
//
// Expected values come from the interpreter, not from this emitter. fs = 1.0 is
// a power of two, so every row's product has a zero tail and the multiplier
// deficit reaches all 72 of them; when it landed in iFPUd, 35 rows moved one ULP
// toward zero. Re-pinning them against the emitter that moved them would assert
// nothing, so each value below was re-derived by running the same row through
// FPU.cpp, which models the deficit independently (eeMulRound) -- 71 of 72 agree
// exactly.
//
// The 72nd is row 53 (ft = 0x48b65815), and it is the documented cost of the
// cheap Booth predicate: its mantissa 0x365815 has no bit of 0x2AA set, so only
// the dropped boundary term fires. The interpreter returns 0xc8b65805, this
// emitter 0xc8b65806. MulDefectDropsTheBoundaryTermTheInterpreterModels holds
// that pair on its own, so closing the gap in iFPUd trips a test that names the
// reason rather than silently re-pinning a number here.
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
	{0x3ef73c56u, 0x3f800000u, 0x48b65815u, 1, 0xc8b65806u, 0u},  // boundary term: interp says c8b65805
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
// The one thing this emitter knowingly does not model, held on its own so that
// closing it trips a test that says why.
//
// The measured predicate has two terms: the Booth term `mant & 0x2AA` (bits
// 1,3,5,7,9 -- the sign bits of the five lowest radix-4 Booth digits) and a
// boundary term at the truncation column,
// `bit11 != (8 <= (mant >> 12 & 0xF) <= 13)`. iFPUd emits only the first: the
// second needs a bitfield extract NEON does not have, about eight more
// instructions on every multiply, and it can only change the answer where the
// product's tail is already zero.
//
// ft = 0x48b65815 is a witness. Its mantissa 0x365815 has no bit of 0x2AA set,
// bit 11 is 1, and (0x365815 >> 12) & 0xF is 5, so the boundary term is the
// only one that fires. The interpreter (FPU.cpp eeMulDefectiveFt) models both
// and returns the decremented product; iFPUd returns the IEEE one.
//
// If iFPUd ever grows the boundary term, this test fails and the fix is to
// delete it -- along with the carve-out in kGuardMaskWitnesses row 53.
TEST(EeRecFpuFull, MulDefectDropsTheBoundaryTermTheInterpreterModels)
{
	constexpr u32 kFs = 0x3f800000u; // 1.0: the product is ft, tail always zero
	constexpr u32 kFt = 0x48b65815u;

	EeRecTestHarness hi;
	hi.EnableCop1();
	hi.SetFprBits(0, kFs);
	hi.SetFprBits(1, kFt);
	hi.LoadProgram({MUL_S(2, 0, 1)});
	hi.RunInterpOnly();
	EXPECT_EQ(hi.GetFprBitsInterp(2), 0x48b65814u) << "interp models the boundary term";

	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kFs);
	h.SetFprBits(1, kFt);
	h.LoadProgram({MUL_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), 0x48b65815u) << "iFPUd drops it: one ULP high";

	// The Booth term alone, for contrast: same shape, and here they agree.
	EeRecTestHarness hb;
	hb.EnableCop1();
	hb.EnableFpuFullMode();
	hb.SetFprBits(0, kFs);
	hb.SetFprBits(1, 0x48b65a15u); // mantissa 0x365a15: 0x2AA hits bit 9
	hb.LoadProgram({MUL_S(2, 0, 1)});
	hb.RunJitNoDiff();
	EXPECT_EQ(hb.GetFprBitsJit(2), 0x48b65a14u);
}

// ---------------------------------------------------------------------------
// Randomised differential: mode 3 against the interpreter, which models the
// same multiply law independently and in completely different code (FPU.cpp
// eeMulProduct, an integer tail test on the 48-bit significand product; iFPUd
// lets the narrowing perform the tail test on a double). Agreement across a
// wide operand space is what says the emitter implements the law rather than
// the handful of rows above.
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
// Three divergences are licensed, and each is counted so the sweep cannot pass
// by never reaching them:
//
//   1. the boundary term iFPUd drops, one ULP further from zero;
//   2. an operand in the exponent-0xff binade. fpuDouble() clamps it to
//      +/-Fmax before multiplying, so the two engines multiply different
//      numbers and the row says nothing about the multiplier;
//   3. a product above FLT_MAX. The interpreter saturates there, mode 3 at the
//      EE's own 0x7FFFFFFF, a whole binade higher.
//
// 2 and 3 are the same pre-existing gap seen from two sides: this branch's
// interpreter has no exponent-0xff binade at all. They are recognised from the
// operands and the exact product, never from the results, so a wrong result
// cannot license itself. Anything else -- a wrong predicate, a decrement
// escaping into a non-zero tail, a zero product turned into a NaN -- fails.
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
	// Both terms of the measured predicate, so a divergence can be classified
	// rather than merely counted.
	auto booth = [](u32 ft) { return (ft & 0x2AAu) != 0; };
	auto full = [](u32 ft) {
		const u32 m = ft & 0x7FFFFFu;
		if (m & 0x2AAu)
			return true;
		const u32 h = (m >> 12) & 0xFu;
		return ((m >> 11) & 1u) != ((h >= 8u && h <= 13u) ? 1u : 0u);
	};

	// The two engine gaps this branch has outside the multiplier, both decided
	// from the inputs alone. fpuDouble() clamps an exponent-0xff operand to
	// +/-Fmax, and the interpreter's product saturates at FLT_MAX where mode 3
	// goes on to 0x7FFFFFFF; either way the row is not about the multiply array.
	auto clamped = [](u32 x) { return (x & 0x7F800000u) == 0x7F800000u; };
	auto overflows = [](u32 fs, u32 ft) {
		auto val = [](u32 x) {
			if ((x & 0x7F800000u) == 0) x &= 0x80000000u;              // fpuDouble
			else if ((x & 0x7F800000u) == 0x7F800000u) x = (x & 0x80000000u) | 0x7F7FFFFFu;
			float f;
			std::memcpy(&f, &x, sizeof(f));
			return static_cast<double>(f);
		};
		return !(std::fabs(val(fs) * val(ft)) <= FLT_MAX);
	};

	u64 state = 0x1234567890ABCDEFull;
	int rows = 0, boundary_gap[4] = {0, 0, 0, 0};
	int clamp_gap = 0, saturation_gap = 0;
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
		h.EnableFpuFullMode();
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
		if (jit == interp)
			continue;

		if (clamped(fs) || clamped(ft)) { clamp_gap++; continue; }
		if (overflows(fs, ft)) { saturation_gap++; continue; }

		const bool boundary_only = !booth(ft) && full(ft);
		ASSERT_TRUE(boundary_only && jit == interp + 1u)
			<< "unlicensed divergence: form=" << form << " cs=" << cs << " ct=" << ct
			<< std::hex << " fs=" << fs << " ft=" << ft
			<< " jit=" << jit << " interp=" << interp;
		boundary_gap[form]++;
	}

	EXPECT_EQ(rows, 40000);
	// Liveness. "No unlicensed divergence" is also what a harness that never
	// reached the emitter would report, so each licensed one must actually be
	// observed -- the boundary term at both emit sites, since they narrow
	// through different code.
	EXPECT_GT(clamp_gap, 0) << "no exponent-0xff operand reached the sweep";
	EXPECT_GT(saturation_gap, 0) << "no product overflowed FLT_MAX in the sweep";
	EXPECT_GT(boundary_gap[0] + boundary_gap[1] + boundary_gap[2], 0)
		<< "recMULop never reached the boundary-term class: the sweep is vacuous";
	EXPECT_GT(boundary_gap[3], 0)
		<< "recMaddsub never reached the boundary-term class: the sweep is vacuous";
}


// ---------------------------------------------------------------------------
// Residency of the predicate mask (d10, NEON_RESERVED_FPU_MULMASK).
//
// The mask is not materialized per multiply. It is parked once per JIT entry by
// _DynGen_EnterRecompiledCode, which is what makes emitDefectiveFmul four
// instructions instead of six -- see the comment on the constant in
// iCore-arm64.h for the contract. These tests pin the two things that contract
// rests on: that no C-call seam, VU dispatch or inline macro-mode emit destroys
// the parked value, and that the allocator never hands q10 out.
//
// Both failures are silent -- no crash, no corrupt value, just a wrong rounding
// decision on a fraction of multiplies -- and they fail in opposite directions,
// which is why every test below checks both polarities:
//
//   mask zeroed        -> Cmtst never fires -> every product correctly rounded
//                         (one ULP high wherever silicon is short)
//   mask overwritten    -> Cmtst fires on operands it must not -> products one
//     with a live value    ULP low where silicon is exact
//
// A test using only predicate-on operands sees the first and is blind to the
// second, because a garbage mask with any of bits 0..9 set gives the same
// answer as the correct mask on exactly those rows. That is not hypothetical:
// removing q10 from _isReservedNEONreg makes the allocator home a guest FPR
// there, and the observed break is the second kind, on the rows where ft's
// mantissa is 0.
//
// The 1147-case hardware corpus cannot see any of this: its cases are single
// ops, so every multiply in it is the first multiply after the entry that
// parked the mask.
namespace {
// Two rows from kSiliconMulRows above, chosen as a matched pair on one pair of
// registers -- f0 = 1.0, f1 = +FLT_MAX -- so a single block can ask the
// question both ways just by swapping the operand order:
//
//   MUL_S(d, 0, 1)   ft = f1, mantissa 0x7fffff  -> predicate on,  0x7f7ffffe
//   MUL_S(d, 1, 0)   ft = f0, mantissa 0         -> predicate off, 0x7f7fffff
//
// Both measured on an SCPH-90000; this is the non-commutativity of mul.s, which
// is the sharpest available probe of the mask because the two answers differ by
// exactly the decrement the mask controls.
constexpr u32 kMaskFprOne    = 0x3f800000u; // f0 = 1.0
constexpr u32 kMaskFprMax    = 0x7f7fffffu; // f1 = +FLT_MAX
constexpr u32 kMaskOnWant    = 0x7f7ffffeu; // 1.0 * FLT_MAX: silicon is one ULP low
constexpr u32 kMaskOffWant   = 0x7f7fffffu; // FLT_MAX * 1.0: silicon is exact

// Seed f0/f1 and assert the matched pair in fd_on / fd_off, at both emit sites.
void ExpectMaskLive(EeRecTestHarness& h, u32 fd_on, u32 fd_off, const char* where)
{
	EXPECT_EQ(h.GetFprBitsJit(fd_on), kMaskOnWant)
		<< where << ": predicate did not fire -- the parked mask read as zero";
	EXPECT_EQ(h.GetFprBitsJit(fd_off), kMaskOffWant)
		<< where << ": predicate fired on ft mantissa 0 -- the parked mask holds garbage";
}
} // namespace

// A VCALLMS in the middle of the block is both runtime hazards at once: it is a
// real in-block C-call seam (the callee may clobber any caller-saved register),
// and it dispatches a VU0 microprogram, whose blocks allocate NEON slots q0-q27
// freely. d10 survives the first because AAPCS64 preserves the low 64 bits of
// d8-d15, and the second because mVUdispatcherAB's prologue Stp/Ldp-saves
// d8-d15 around the dispatch -- the same protection the s8/s9 clamp scalars get
// (see the VE-04 note in microVU-arm64.cpp).
//
// A compile-time liveness flag in a caller-saved register would have had to
// invalidate at this seam and re-materialize after it; this is the assertion
// that the parked register needs no seam handling at all, at both emit sites.
TEST(EeRecFpuFull, MulDefectMaskSurvivesAnInBlockCallSeam)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.EnableVu0Capture();
	h.SeedVu0Vi(REG_VPU_STAT, 0);
	// Trivial immediate-E micro: the VCALLMS is here purely as the seam.
	h.SeedVu0Microprogram(0, {vu::EBitNopPair(), vu::NopPair()});
	h.SetAccBits(0x00000000u);
	h.SetFprBits(0, kMaskFprOne);
	h.SetFprBits(1, kMaskFprMax);
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

	ExpectMaskLive(h, 2, 3, "pre-seam MUL.S");
	ExpectMaskLive(h, 4, 5, "post-seam MUL.S");
	ExpectMaskLive(h, 6, 7, "post-seam MADD.S");
	// Liveness: the two expectations above discriminate only because the two
	// answers differ. If this ever fires the rows stopped being a matched pair
	// and the test is vacuous whatever it reports.
	ASSERT_NE(kMaskOnWant, kMaskOffWant);
}

// COP2 macro mode is the one context that emits mVU code inline in an EE block,
// with no dispatcher save around it. It is safe for d10 for a structural reason
// rather than an ABI one: macro emit is bounded to NEON slots 0-3 by
// kMacroVFEvictHighWater, which mVUmacroEmitEpilogue asserts on every macro op.
// That is why d10 needs no microVU pool gate, unlike SL-13's q25/q26 -- and
// this is the end-to-end check on it, with two macro FMACs between the
// multiplies to give the mVU allocator something to spend registers on.
TEST(EeRecFpuFull, MulDefectMaskSurvivesInlineCop2MacroMode)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.EnableVu0Capture();
	h.SeedVu0Vi(REG_VPU_STAT, 0);
	h.SeedVu0Vf(1, 1.0f, 2.0f, 3.0f, 4.0f);
	h.SeedVu0Vf(2, 5.0f, 6.0f, 7.0f, 8.0f);
	h.SetFprBits(0, kMaskFprOne);
	h.SetFprBits(1, kMaskFprMax);
	h.LoadProgram({
		MUL_S(2, 0, 1),
		MUL_S(3, 1, 0),
		VMUL_C2(0xf, 3, 1, 2),
		VADD_C2(0xf, 4, 1, 2),
		MUL_S(4, 0, 1), // post-macro
		MUL_S(5, 1, 0),
	});
	h.RunJitNoDiff();

	ExpectMaskLive(h, 2, 3, "pre-macro MUL.S");
	ExpectMaskLive(h, 4, 5, "MUL.S after inline COP2 macro emit");
	ASSERT_NE(kMaskOnWant, kMaskOffWant);
}

// The other half of the contract: nothing in EE codegen may be handed q10.
// EeVu0Cop2ClampResidency.EeAllocatorReservesClampRegs pins the reservation at
// the predicate; this pins it through the allocator, with enough simultaneously
// live FP values to drive allocation into the callee-saved range where the mask
// sits. That range is not a last resort -- the FPR class prefers it (GE-15,
// _getFreeArm64NEONInRangeNoEvict(NEON_CALLEE_SAVED_START, ...)), so q10 is one
// of the first homes an unreserved pool would reach for, not one of the last.
TEST(EeRecFpuFull, MulDefectMaskSurvivesHeavyFprPressure)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kMaskFprOne);
	h.SetFprBits(1, kMaskFprMax);

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

	ExpectMaskLive(h, 2, 3, "MUL.S under FPR pressure");
	ExpectMaskLive(h, 4, 5, "MUL.S after 16 live FPRs");
	ASSERT_NE(kMaskOnWant, kMaskOffWant);
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

// Upper binade, T == 0, only the dropped boundary term fires -> silicon is one
// ULP low and this emitter is one ULP high. `want` is the silicon value, so
// the emitter is asserted at want + 1: the licensed divergence, in the binade
// where it had never been measured.
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
void ExpectBothSites(const BinadeRow& r, u32 want, const char* what)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, r.fs);
	h.SetFprBits(1, r.ft);
	h.LoadProgram({MUL_S(2, 0, 1)});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetFprBitsJit(2), want)
		<< what << " MUL.S fs=" << std::hex << r.fs << " ft=" << r.ft;

	EeRecTestHarness hm;
	hm.EnableCop1();
	hm.EnableFpuFullMode();
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

// The dropped boundary term reaches the upper binade too, at the same 1/64 of
// significands. Asserting emitter == silicon + 1 keeps the gap measured rather
// than latent: closing it in iFPUd trips this and names the reason.
TEST(EeRecFpuFull, MulDefectBoundaryTermGapAlsoExistsInTheUpperBinade)
{
	for (const BinadeRow& r : kTopBinadeBoundary)
	{
		ExpectBothSites(r, r.want + 1u, "upper-binade boundary term");

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
