// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The EE's divide/square-root unit rounds to nearest even when the rest of the
// FPU is chopping toward zero. Both recompilers model that by swapping
// EmuConfig.Cpu.FPUDivFPCR in around the three ops that unit owns -- DIV.S,
// SQRT.S and RSQRT.S. The interpreter did not, so it truncated where the
// recompilers and the console round; ScopedDivRoundMode in pcsx2/FPU.cpp has
// the mechanism.
//
// Two things kept that off the differential. Every pre-existing DIV.S case used
// an exactly-representable ratio -- 20/4, 6/-2, with comments saying "no
// rounding divergence" -- and those are the operands at which the two rounding
// modes agree. And the one test that did probe SQRT.S's rounding mode
// (EeRecFpu.SqrtSRoundsToNearestUnderChopFpcr) asserted the JIT result alone,
// because the old harness ran both engines under a nearest-rounding host FPCR
// where the swap is a no-op; that stopped being true when the harness moved to
// the production environment, and the test kept passing either way because it
// never looked at the interpreter.
//
// So this file covers DIV.S and SQRT.S at inexact operands, randomized, in the
// FP environment a game runs in. RSQRT.S is covered the same way in
// ee_rec_fpu_rsqrt_tests.cpp.
//
// The SQRT.S sweep also turned up an unrelated defect on its first run: the
// interpreter returned -0.0 for sqrt(-0.0) where the EE returns +0.0, fixed
// separately and pinned by EeRecFpu.SqrtSOfNegativeZeroIsPositiveZero.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "common/FPControl.h"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kI = 0x00020000u, kD = 0x00010000u, kSI = 0x40u, kSD = 0x20u;
constexpr u32 kStickyMask = kI | kD | kSI | kSD;

struct Lcg
{
	u64 s;
	u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 32); }
};

// Full-range normals dominate, with the signed zeros and +/-fMax edges mixed in
// so the divide-by-zero and clamp branches stay covered by the same sweep. Raw
// Inf/NaN are excluded -- they belong to the operand-clamp tests.
u32 fuzzOperand(Lcg& r)
{
	switch (r.next() % 8u)
	{
		case 0: return 0x00000000u;  // +0
		case 1: return 0x80000000u;  // -0
		case 2: return 0x7F7FFFFFu;  // +fMax
		case 3: return 0xFF7FFFFFu;  // -fMax
		default:
		{
			const u32 sign = (r.next() & 1u) << 31;
			const u32 exp = 1u + (r.next() % 254u); // 1..254 (normal)
			const u32 man = r.next() & 0x7FFFFFu;
			return sign | (exp << 23) | man;
		}
	}
}

// Overrides the ambient rounding mode alone. ScopedFpEnv rewrites all four
// registers, equalizing FPUFPCR and FPUDivFPCR, which the header rules out.
struct ScopedAmbientRoundMode
{
	FPControlRegister saved_cfg, saved_host;
	explicit ScopedAmbientRoundMode(FPRoundMode mode)
		: saved_cfg(EmuConfig.Cpu.FPUFPCR)
		, saved_host(FPControlRegister::GetCurrent())
	{
		EmuConfig.Cpu.FPUFPCR.SetRoundMode(mode);
	}
	~ScopedAmbientRoundMode()
	{
		EmuConfig.Cpu.FPUFPCR = saved_cfg;
		FPControlRegister::SetCurrent(saved_host);
	}
	ScopedAmbientRoundMode(const ScopedAmbientRoundMode&) = delete;
	ScopedAmbientRoundMode& operator=(const ScopedAmbientRoundMode&) = delete;
};

// The premise every test here rests on.
void RequireDistinctDivideRoundingMode()
{
	ASSERT_NE(EmuConfig.Cpu.FPUFPCR.bitmask, EmuConfig.Cpu.FPUDivFPCR.bitmask)
		<< "FPUFPCR and FPUDivFPCR are equal, so the divide unit's rounding mode "
		   "swap is unobservable and every test in this file is vacuous";
	ASSERT_NE(EmuConfig.Cpu.FPUFPCR.GetRoundMode(), EmuConfig.Cpu.FPUDivFPCR.GetRoundMode())
		<< "the two registers differ, but not in the rounding mode -- this file "
		   "only covers the rounding mode";
}

} // namespace

// ---------------------------------------------------------------------------
// DIV.S
// ---------------------------------------------------------------------------
// The one value divergence this fuzzer must tolerate: the interpreter saturates
// at the EE's own maximum where the fast path stops at FLT_MAX -- see
// EeFpuTopBinadeConsole. Written as a property of the two words rather than as
// an operand filter, so the fuzzer keeps generating saturating pairs and any
// other disagreement on them still fails.
static bool IsTopBinadeTierGap(u32 interp, u32 jit)
{
	return (interp & 0x7F800000u) == 0x7F800000u &&
	       (jit & 0x7FFFFFFFu) == 0x7F7FFFFFu &&
	       (interp & 0x80000000u) == (jit & 0x80000000u);
}

// The second divergence: the interpreter now models the divide unit's
// truncation law (FPU.cpp, eeDivideTruncates / eeSqrtBits) while the emitters
// still take the host's correctly-rounded fdiv/fsqrt. The tests below stop
// asserting that the engines agree and assert instead the shape they may differ
// in: only where the law fires (u > cap), only by one ULP, and only with the
// interpreter on the closer-to-zero side.
//
// The predicates are recomputed here rather than exported from FPU.cpp: a
// differential that imports the implementation's arithmetic cannot catch the
// implementation's arithmetic being wrong.
static bool BothNormalOperands(u32 fs, u32 ft)
{
	return ((fs >> 23) & 0xFFu) != 0 && ((ft >> 23) & 0xFFu) != 0;
}

static bool DivideTruncates(u32 fs, u32 ft)
{
	const u32 ma = 0x800000u | (fs & 0x7FFFFFu);
	const u32 mb = 0x800000u | (ft & 0x7FFFFFu);
	const int lt = ma < mb ? 1 : 0;
	const u64 num = static_cast<u64>(ma) << (23 + lt);
	const u32 rem = static_cast<u32>(num % mb);
	const u32 cap = lt ? std::max<u32>(1u << 23, mb - (1u << 22)) : (1u << 22);
	return (mb - rem) > cap;
}

static bool SqrtTruncates(u32 ft)
{
	const u32 E = (ft >> 23) & 0xFFu;
	if (E == 0)
		return false;
	const u64 X = static_cast<u64>(0x800000u | (ft & 0x7FFFFFu)) << ((E & 1u) ? 23 : 24);
	u64 R = static_cast<u64>(std::sqrt(static_cast<double>(X)));
	while (R > 0 && R * R > X)
		--R;
	while ((R + 1) * (R + 1) <= X)
		++R;
	return (2 * R + 1 - (X - R * R)) > (1u << 23);
}

// The interpreter's word is the JIT's with one unit taken off the magnitude.
static bool IsOneUlpTowardZero(u32 interp, u32 jit)
{
	return (jit & 0x7FFFFFFFu) != 0 &&
	       interp == ((jit & 0x80000000u) | ((jit & 0x7FFFFFFFu) - 1u));
}

TEST(EeRecFpuDivUnitRounding, DivSMatchesInterpExceptWhereTheTruncationLawFires)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0xD1F5D1F5A5A5A5A5ull};
	int checked = 0, tier_gaps = 0, law_gaps = 0;
	for (u32 iter = 0; iter < 3000; ++iter)
	{
		const u32 fsBits = fuzzOperand(r);
		const u32 ftBits = fuzzOperand(r);
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;

		SCOPED_TRACE(::testing::Message()
			<< "iter=" << iter << " Fs=" << std::hex << fsBits << " Ft=" << ftBits << " pre=" << pre);

		// Two harnesses rather than Run()'s auto-diff: the tiers are allowed to
		// disagree on saturation and Run() cannot express that.
		u32 res[2] = {}, fcr[2] = {};
		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFprBits(1, fsBits);
			h.SetFprBits(2, ftBits);
			h.SetFcr31(pre);
			h.LoadProgram({ee::DIV_S(3, 1, 2)});
			if (jit)
			{
				h.RunJitNoDiff();
				res[1] = h.GetFprBitsJit(3);
				fcr[1] = h.JitSnapshot().fprs.fprc[31];
			}
			else
			{
				h.RunInterpOnly();
				res[0] = h.GetFprBitsInterp(3);
				fcr[0] = h.InterpSnapshot().fprs.fprc[31];
			}
		}

		if (IsTopBinadeTierGap(res[0], res[1]))
		{
			++tier_gaps;
		}
		else if (res[0] != res[1])
		{
			++law_gaps;
			EXPECT_TRUE(BothNormalOperands(fsBits, ftBits) &&
						DivideTruncates(fsBits, ftBits))
				<< "the engines parted company where the truncation law does NOT "
				   "fire -- that is a plain quotient disagreement, not the "
				   "modelled one";
			EXPECT_TRUE(IsOneUlpTowardZero(res[0], res[1]))
				<< "the interpreter's model can only ever take the LOWER of the two "
				   "candidates; interp=" << std::hex << res[0] << " jit=" << res[1];
		}
		EXPECT_EQ(fcr[1] & kStickyMask, fcr[0] & kStickyMask);
		++checked;
		if (::testing::Test::HasFailure())
			return; // first failing case is enough for a clean repro
	}
	EXPECT_EQ(checked, 3000);
	EXPECT_GT(tier_gaps, 0) << "anti-vacuity: the operand pool stopped producing "
							   "saturating quotients, so the allowance above is "
							   "dead code that could hide a real divergence";
	EXPECT_GT(law_gaps, 0) << "anti-vacuity: no operand pair reached the truncation "
							  "law, so this test is asserting engine agreement under "
							  "a different name";
}

// A named witness alongside the fuzzer: 1.0 / 3.0 is one ULP apart between the
// two rounding modes.
TEST(EeRecFpuDivUnitRounding, DivSOneOverThreeRoundsToNearest)
{
	RequireDistinctDivideRoundingMode();

	const auto build = [](EeRecTestHarness& h) {
		h.EnableCop1();
		h.SetFcr31(0);
		h.SetFpr(1, 1.0f);
		h.SetFpr(2, 3.0f);
		h.LoadProgram({ee::DIV_S(3, 1, 2)});
	};
	EeRecTestHarness hj;
	build(hj);
	hj.RunJitNoDiff();
	EeRecTestHarness hi;
	build(hi);
	hi.RunInterpOnly();

	// 1/3 = 0x3EAAAAAB to nearest, 0x3EAAAAAA chopped.
	EXPECT_EQ(hj.GetFprBitsJit(3), 0x3EAAAAABu) << "[jit] round-to-nearest, matches console";
	EXPECT_EQ(hi.GetFprBitsInterp(3), 0x3EAAAAABu)
		<< "[interp] 0x3EAAAAAA means the FPUDivFPCR swap was lost again";
}

// ---------------------------------------------------------------------------
// SQRT.S
// ---------------------------------------------------------------------------
TEST(EeRecFpuDivUnitRounding, SqrtSMatchesInterpExceptWhereTheTruncationLawFires)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0x5011EE5011EE1234ull};
	int law_gaps = 0;
	for (u32 iter = 0; iter < 3000; ++iter)
	{
		// Both signs: SQRT.S takes |Ft| on the negative path and raises I|SI.
		const u32 ftBits = fuzzOperand(r);
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;

		SCOPED_TRACE(::testing::Message()
			<< "iter=" << iter << " Ft=" << std::hex << ftBits << " pre=" << pre);

		// Two harnesses, not Run(): the engines now differ on purpose, and
		// Run()'s auto-diff cannot express "differ in exactly this shape".
		u32 res[2] = {}, fcr[2] = {};
		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFprBits(1, ftBits);
			h.SetFcr31(pre);
			h.LoadProgram({ee::SQRT_S(2, 1)});
			if (jit)
			{
				h.RunJitNoDiff();
				res[1] = h.GetFprBitsJit(2);
				fcr[1] = h.JitSnapshot().fprs.fprc[31];
			}
			else
			{
				h.RunInterpOnly();
				res[0] = h.GetFprBitsInterp(2);
				fcr[0] = h.InterpSnapshot().fprs.fprc[31];
			}
		}

		if (res[0] != res[1])
		{
			++law_gaps;
			EXPECT_TRUE(SqrtTruncates(ftBits))
				<< "the engines parted company on a root the truncation law does "
				   "NOT settle";
			EXPECT_TRUE(IsOneUlpTowardZero(res[0], res[1]))
				<< "silicon's square root is one ULP LOW or exact, never high; "
				   "interp=" << std::hex << res[0] << " jit=" << res[1];
		}
		EXPECT_EQ(fcr[1] & kStickyMask, fcr[0] & kStickyMask);
		if (::testing::Test::HasFailure())
			return;
	}
	EXPECT_GT(law_gaps, 0) << "anti-vacuity: no operand reached the truncation law, "
							  "so this test is asserting engine agreement under a "
							  "different name";
}

// sqrt(5): 0x400F1BBD to nearest, 0x400F1BBC chopped.
TEST(EeRecFpuDivUnitRounding, SqrtSOfFiveRoundsToNearest)
{
	RequireDistinctDivideRoundingMode();

	const auto build = [](EeRecTestHarness& h) {
		h.EnableCop1();
		h.SetFcr31(0);
		h.SetFprSingle(1, 5.0f);
		h.LoadProgram({ee::SQRT_S(2, 1)});
	};
	EeRecTestHarness hj;
	build(hj);
	hj.RunJitNoDiff();
	EeRecTestHarness hi;
	build(hi);
	hi.RunInterpOnly();

	EXPECT_EQ(hj.GetFprBitsJit(2), 0x400F1BBDu) << "[jit] round-to-nearest, matches console";
	EXPECT_EQ(hi.GetFprBitsInterp(2), 0x400F1BBDu)
		<< "[interp] 0x400F1BBC means the FPUDivFPCR swap was lost again";
}

// ---------------------------------------------------------------------------
// The negative control. ADD.S does not belong to the divide unit and must keep
// chopping under the ambient mode, so a fix that widened the swap to the whole
// FPU fails here, and nothing else in the suite would catch it.
//
// Every other test in this file was validated by reverting the fix and
// watching it fail. A negative control passes in both directions by
// construction, so it gets the liveness clause at the bottom instead.
//
// The operands sum exactly to 2 - 2^-24, halfway between 0x3FFFFFFF (= 2 -
// 2^-23, the largest float below 2) and 0x40000000, so chop-toward-zero keeps
// the lower and round-to-nearest ties-to-even takes 2.0. Their one-bit
// exponent difference means guard-bit masking (fpuGuardedAddSub, on by
// default, fpuEmitGuardedAddSub in iFPU-arm64.cpp) masks off (diff - 1) = 0
// bits, so the pair discriminates the same with that option on or off, on both
// engines.
// ---------------------------------------------------------------------------
TEST(EeRecFpuDivUnitRounding, ArithmeticStillChopsUnderTheAmbientMode)
{
	RequireDistinctDivideRoundingMode();
	ASSERT_EQ(EmuConfig.Cpu.FPUFPCR.GetRoundMode(), FPRoundMode::ChopZero)
		<< "this control assumes the default chop-toward-zero ambient mode";

	constexpr u32 kOne = 0x3F800000u;        // 1.0
	constexpr u32 kJustBelowOne = 0x3F7FFFFFu; // 1 - 2^-24
	constexpr u32 kChopped = 0x3FFFFFFFu;    // 2 - 2^-23
	constexpr u32 kRounded = 0x40000000u;    // 2.0

	const auto run = [](bool jit) {
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFcr31(0);
		h.SetFprBits(1, kOne);
		h.SetFprBits(2, kJustBelowOne);
		h.LoadProgram({ee::ADD_S(3, 1, 2)});
		if (jit)
			h.RunJitNoDiff();
		else
			h.RunInterpOnly();
		return jit ? h.GetFprBitsJit(3) : h.GetFprBitsInterp(3);
	};

	EXPECT_EQ(run(true), kChopped)
		<< "[jit] ADD.S must chop; 0x40000000 means the divide-unit swap leaked";
	EXPECT_EQ(run(false), kChopped)
		<< "[interp] ADD.S must chop; 0x40000000 means the divide-unit swap leaked";

	// Liveness: under round-to-nearest the same operands must give the other
	// value, or the assertions above pin a constant rather than a mode.
	{
		const ScopedAmbientRoundMode nearest{FPRoundMode::Nearest};
		EXPECT_EQ(run(true), kRounded)
			<< "[jit] control is DEAD -- these operands are insensitive to the "
			   "ambient rounding mode, so the chop assertions above prove nothing";
		EXPECT_EQ(run(false), kRounded)
			<< "[interp] control is DEAD -- see above";
	}
}
