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
TEST(EeRecFpuDivUnitRounding, DivSMatchesInterpAtInexactQuotients)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0xD1F5D1F5A5A5A5A5ull};
	for (u32 iter = 0; iter < 3000; ++iter)
	{
		const u32 fsBits = fuzzOperand(r);
		const u32 ftBits = fuzzOperand(r);
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;

		SCOPED_TRACE(::testing::Message()
			<< "iter=" << iter << " Fs=" << std::hex << fsBits << " Ft=" << ftBits << " pre=" << pre);

		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(1, fsBits);
		h.SetFprBits(2, ftBits);
		h.SetFcr31(pre);
		h.LoadProgram({ee::DIV_S(3, 1, 2)});
		h.Run(); // auto-diffs the result value

		EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask,
			h.InterpSnapshot().fprs.fprc[31] & kStickyMask);
		if (::testing::Test::HasFailure())
			return; // first failing case is enough for a clean repro
	}
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
TEST(EeRecFpuDivUnitRounding, SqrtSMatchesInterpAtInexactRoots)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0x5011EE5011EE1234ull};
	for (u32 iter = 0; iter < 3000; ++iter)
	{
		// Both signs: SQRT.S takes |Ft| on the negative path and raises I|SI.
		const u32 ftBits = fuzzOperand(r);
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;

		SCOPED_TRACE(::testing::Message()
			<< "iter=" << iter << " Ft=" << std::hex << ftBits << " pre=" << pre);

		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(1, ftBits);
		h.SetFcr31(pre);
		h.LoadProgram({ee::SQRT_S(2, 1)});
		h.Run();

		EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask,
			h.InterpSnapshot().fprs.fprc[31] & kStickyMask);
		if (::testing::Test::HasFailure())
			return;
	}
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
