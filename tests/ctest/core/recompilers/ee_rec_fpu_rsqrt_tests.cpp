// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Native RSQRT.S coverage. Fd = Fs / sqrt(|Ft|).
//
// RSQRT.S used to defer to the interpreter (recFPUCall); it is now native
// (recRSQRT_S_xmm), mirroring x86's xSQRT.SS + xDIV.SS and lrps2's FP-domain
// RSQRT. This file pins the behavior, with the differential/JIT-only split
// established by executed evidence (40k random pairs, JIT vs interp):
//
//   - FCR31 flags (I|D|SI|SD): match the interpreter on every input, so they
//     are always diffed. I|D are cleared each op; SI|SD are sticky.
//   - Zero divisor (Ft exponent field == 0, denormals included): exact
//     sign(Fs) | 0x7f7fffff, D|SD raised. Matches interp exactly.
//   - Every divisor class -- zero, negative, positive -- now matches the
//     interpreter bit-for-bit, in any FP environment, so every case here is
//     differential. Getting there took two independent fixes that this file
//     once pinned as tripwires: the interpreter divided by an unrounded
//     double-precision sqrt (bare libm sqrt returns double), and it never
//     modelled the divide/sqrt unit's own round-to-nearest. Both landed on
//     1.0 rsqrt 1.5 as 0x3F5105EC against hardware's 0x3F5105EB, which made
//     two defects look like one; RSQRT_S in pcsx2/FPU.cpp has both at the
//     divide they share. The same rounding mode over DIV.S and SQRT.S is in
//     ee_rec_fpu_divunit_rounding_tests.cpp.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "common/FPControl.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kI = 0x00020000u, kD = 0x00010000u, kSI = 0x40u, kSD = 0x20u;
constexpr u32 kStickyMask = kI | kD | kSI | kSD;

u32 FprBits(float f)
{
	u32 b;
	std::memcpy(&b, &f, sizeof(b));
	return b;
}

struct Lcg
{
	u64 s;
	u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 32); }
};

// Operand pool for the differential fuzzer: normals across the full exponent
// range, signed zeros, and +/-fMax. Deliberately excludes raw Inf/NaN and
// denormals -- those need the CHECK_FPU_EXTRA_OVERFLOW operand clamp / hit the
// zero path and are pinned by dedicated tests below.
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

} // namespace

// ---------------------------------------------------------------------------
// Differential fuzzer over the exactly-matching domain: zero and negative
// divisors (interp and native both stay single-precision there). Any Fs.
// The result value and the sticky flags are both diffed.
// ---------------------------------------------------------------------------
// No ScopedFpEnv anywhere in this file: tagging these FlushNearest makes
// FPUFPCR and FPUDivFPCR equal, which is the one environment where the divide
// unit's rounding mode cannot be seen.
TEST(EeRecFpuRsqrt, DifferentialFuzzZeroAndNegativeDivisor)
{
	Lcg r{0x123456789ABCDEF0ull};
	for (u32 iter = 0; iter < 3000; ++iter)
	{
		const u32 fsBits = fuzzOperand(r);
		// Force the divisor into the exactly-matching domain: negative (set the
		// sign bit) or zero. Positive normals are covered by the ULP fuzzer.
		const u32 ftBits = fuzzOperand(r) | 0x80000000u;
		// Exercise a sticky-flag pre-state on a fraction of iterations so the
		// clear-I|D / preserve-SI|SD contract is covered under the diff.
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;

		SCOPED_TRACE(::testing::Message()
			<< "iter=" << iter << " Fs=" << std::hex << fsBits << " Ft=" << ftBits << " pre=" << pre);

		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(1, fsBits);
		h.SetFprBits(2, ftBits);
		h.SetFcr31(pre);
		h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
		h.Run(); // auto-diffs the result value

		EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask,
			h.InterpSnapshot().fprs.fprc[31] & kStickyMask);
		if (::testing::Test::HasFailure())
			return; // first failing case is enough for a clean repro
	}
}

// ---------------------------------------------------------------------------
// Positive-divisor fuzzer. An exact differential like every other case in this
// file now that both engines are single-precision and share a rounding mode:
// Run()'s auto-diff checks the value, the flags are diffed on top.
// ---------------------------------------------------------------------------
TEST(EeRecFpuRsqrt, PositiveDivisorMatchesInterpExactly)
{
	Lcg r{0x0F0E0D0C0B0A0908ull};
	for (u32 iter = 0; iter < 3000; ++iter)
	{
		// Positive nonzero divisor: clear sign, force a normal exponent.
		u32 ftBits = fuzzOperand(r) & 0x7FFFFFFFu;
		if ((ftBits & 0x7F800000u) == 0u)
			ftBits |= 0x3F800000u; // lift zero/denormal into the normal range
		const u32 fsBits = fuzzOperand(r);
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;

		SCOPED_TRACE(::testing::Message()
			<< "iter=" << iter << " Fs=" << std::hex << fsBits << " Ft=" << ftBits << " pre=" << pre);

		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(1, fsBits);
		h.SetFprBits(2, ftBits);
		h.SetFcr31(pre);
		h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
		h.Run(); // auto-diffs the result value

		EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask,
			h.InterpSnapshot().fprs.fprc[31] & kStickyMask);
		if (::testing::Test::HasFailure())
			return;
	}
}

// ---- Exact-result differential cases (value + flags both diffed) -----------

TEST(EeRecFpuRsqrt, PositiveExactRatioMatchesInterp)
{
	// 6 / sqrt(4) = 6/2 = 3.0 -- exact, so single and double agree.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0);
	h.SetFpr(1, 6.0f);
	h.SetFpr(2, 4.0f);
	h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, FprBits(3.0f));
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask, 0u);   // positive: no flags
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & kStickyMask, 0u);
}

TEST(EeRecFpuRsqrt, NegativeDivisorInexactMatchesInterp)
{
	// Negative divisor path: interp rounds sqrt(|Ft|) to float before dividing,
	// so an inexact quotient still matches native single-precision. 1/sqrt(2).
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0);
	h.SetFpr(1, 1.0f);
	h.SetFpr(2, -2.0f);           // negative, |Ft| = 2
	h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, 0x3F3504F3u);  // 1/sqrt(2), single-precision
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask, kI | kSI);   // I|SI
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & kStickyMask, kI | kSI);
}

// ---- Register-aliasing cases: EEREC_D may equal EEREC_S and/or EEREC_T -----
// The op copies both operands into temps up front, so the destination write
// cannot corrupt an aliased source. Exact results keep these differential.

TEST(EeRecFpuRsqrt, DestAliasesSource)
{
	// fd == fs. 8 / sqrt(16) = 8/4 = 2.0.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0);
	h.SetFpr(1, 8.0f);
	h.SetFpr(2, 16.0f);
	h.LoadProgram({ee::RSQRT_S(1, 1, 2)}); // fd=fs=1, ft=2
	h.Run();
	h.ExpectFpr(1, FprBits(2.0f));
}

TEST(EeRecFpuRsqrt, DestAliasesDivisor)
{
	// fd == ft. 3 / sqrt(0.25) = 3/0.5 = 6.0. The zero/negative branch and the
	// sqrt both read ft before EEREC_D (== ft) is overwritten.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0);
	h.SetFpr(1, 3.0f);
	h.SetFpr(2, 0.25f);
	h.LoadProgram({ee::RSQRT_S(2, 1, 2)}); // fd=ft=2, fs=1
	h.Run();
	h.ExpectFpr(2, FprBits(6.0f));
}

TEST(EeRecFpuRsqrt, SourceAliasesDivisor)
{
	// fs == ft. x / sqrt(x) = sqrt(x). 4 / sqrt(4) = 2.0.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0);
	h.SetFpr(1, 4.0f);
	h.LoadProgram({ee::RSQRT_S(3, 1, 1)}); // fs=ft=1
	h.Run();
	h.ExpectFpr(3, FprBits(2.0f));
}

// ---- Zero / denormal divisor: exponent field 0 hits the zero path ----------

TEST(EeRecFpuRsqrt, DenormalDivisorTreatedAsZero)
{
	// A denormal Ft (exp field 0, mantissa nonzero) is "zero" for RSQRT: result
	// is sign(FS) | 0x7f7fffff with D|SD, exactly like +/-0. The divisor's sign
	// is irrelevant -- see ZeroDivisorSignComesFromTheDividend below.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(0);
	h.SetFpr(1, 5.0f);
	h.SetFprBits(2, 0x807FFFFFu);   // largest negative denormal
	h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, 0x7F7FFFFFu);    // sign(Fs)=pos -> +fMax
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask, kD | kSD);
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & kStickyMask, kD | kSD);
}

// RSQRT.S's zero-divisor result takes the DIVIDEND's sign alone -- not the
// xor DIV.S uses, and not Ft's. The op divides by sqrt(|Ft|), so the divisor
// has no sign left to contribute by the time the division happens. Console
// witnesses: rsqrt(+0, -0) is positive and rsqrt(-0, -0) is negative; an xor
// rule, or Ft's sign (which both engines used to take), flips both. x86
// recRSQRThelper1 (iFPU.cpp) has always taken Fs's sign. The magnitude stays
// at the fast tier's +/-fMax saturation -- silicon says 0x7FFFFFFF, which is
// the top-binade compromise, not the sign rule.
TEST(EeRecFpuRsqrt, ZeroDivisorSignComesFromTheDividend)
{
	struct Row
	{
		u32 fs, ft, want;
	};
	static const Row kRows[] = {
		{0x00000000u, 0x80000000u, 0x7F7FFFFFu}, // rsqrt(+0, -0) -> +fMax
		{0x80000000u, 0x00000000u, 0xFF7FFFFFu}, // rsqrt(-0, +0) -> -fMax
		{0x80000000u, 0x80000000u, 0xFF7FFFFFu}, // rsqrt(-0, -0) -> -fMax
		{0x00000000u, 0x00000000u, 0x7F7FFFFFu}, // rsqrt(+0, +0) -> +fMax
		{0x40A00000u, 0x80000000u, 0x7F7FFFFFu}, // rsqrt(+5, -0) -> +fMax
		{0xC0A00000u, 0x00000000u, 0xFF7FFFFFu}, // rsqrt(-5, +0) -> -fMax
	};
	for (const Row& r : kRows)
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "fs=" << r.fs
										  << " ft=" << r.ft);
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFcr31(0);
		h.SetFprBits(1, r.fs);
		h.SetFprBits(2, r.ft);
		h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
		h.Run(); // exact on the zero path: auto-diffs the engines
		h.ExpectFpr(3, r.want);
		EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask, kD | kSD);
		EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & kStickyMask, kD | kSD);
	}
}

// ---- FCR31 sticky-bit contract: clear I|D each op, preserve SI|SD -----------

TEST(EeRecFpuRsqrt, ClearsIDPreservesStickyOnPositive)
{
	// Pre-set all four; a clean positive op must clear I|D and leave SI|SD.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(kI | kD | kSI | kSD);
	h.SetFpr(1, 6.0f);
	h.SetFpr(2, 4.0f);              // 6/sqrt(4)=3.0, no new flags
	h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, FprBits(3.0f));
	EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask, kSI | kSD);    // I|D cleared
	EXPECT_EQ(h.InterpSnapshot().fprs.fprc[31] & kStickyMask, kSI | kSD);
}

// ---- Positive-path single-precision value (x86 / hardware parity) ----------
// Both engines round the sqrt to single before dividing, so Run()'s auto-diff
// applies here. A double-precision divide lands 1 ULP high, at 0x3F5105EC.
TEST(EeRecFpuRsqrt, PositivePathSinglePrecisionValue)
{
	// 1 / sqrt(1.5) = 0x3f5105eb in single precision.
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFpr(1, 1.0f);
	h.SetFpr(2, 1.5f);
	h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
	h.Run();
	h.ExpectFpr(3, 0x3F5105EBu); // single-precision (matches x86 and hardware)
}

// ---- The divide/sqrt unit's own rounding mode -------------------------------
// Graduated tripwire for the second of the two fixes in the file header: the
// interpreter truncated where the console rounds and produced 0x3F5105EC for
// this pair, the same value the earlier unrounded-sqrt defect produced.
//
// Separate JIT and interp runs rather than Run()'s auto-diff, so a failure
// names which engine moved.
TEST(EeRecFpuRsqrt, DivideUnitRoundsToNearestInProductionFpEnv)
{
	const auto build = [](EeRecTestHarness& h) {
		h.EnableCop1();
		h.SetFpr(1, 1.0f);
		h.SetFpr(2, 1.5f);
		h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
	};
	EeRecTestHarness hj;
	build(hj);
	hj.RunJitNoDiff();
	EeRecTestHarness hi;
	build(hi);
	hi.RunInterpOnly();

	ASSERT_NE(EmuConfig.Cpu.FPUFPCR.bitmask, EmuConfig.Cpu.FPUDivFPCR.bitmask)
		<< "the production environment must have a distinct divide rounding mode";

	EXPECT_EQ(hj.GetFprBitsJit(3), 0x3F5105EBu) << "[jit] round-to-nearest, matches console";
	EXPECT_EQ(hi.GetFprBitsInterp(3), 0x3F5105EBu)
		<< "[interp] 0x3F5105EC means the FPUDivFPCR swap was lost again";
}
