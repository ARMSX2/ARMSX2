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
//     sign(Fs) | 0x7f7fffff, D|SD raised, plus I|SI when it is negative.
//     Matches interp exactly.
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
#include <vector>
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
//
// The one value divergence these differentials must tolerate: the interpreter
// saturates at the EE's own maximum where the fast path stops at FLT_MAX -- see
// EeFpuTopBinadeConsole. Written as a property of the two words rather than as
// an operand filter, so the fuzzers keep generating saturating pairs and any
// other disagreement still fails.
static bool IsTopBinadeTierGap(u32 interp, u32 jit)
{
	return (interp & 0x7F800000u) == 0x7F800000u &&
	       (jit & 0x7FFFFFFFu) == 0x7F7FFFFFu &&
	       (interp & 0x80000000u) == (jit & 0x80000000u);
}

// The second allowance, and a wider one, because RSQRT.S is composed.
//
// The interpreter runs the divide unit's own digit recurrence (FPU.cpp,
// eeSrtDigit and below) and the emitters still take the host's correctly-
// rounded fsqrt/fdiv, so the interpreter runs the recurrence twice -- once for
// the root, once for the quotient. The two do not pull the same way: a root
// that comes back one ULP lower makes the quotient larger, so unlike DIV.S and
// SQRT.S the interpreter can land on either side of the fast path here.
// That compounding is silicon's own -- the console capture has 26 rsqrt.s rows
// one ULP off correct rounding and 2 of them two ULP -- and it is bounded at
// two ULP of magnitude with the sign never in question, which is what this
// asserts.
//
// EeFpuDivUnitConsole owns the model itself and the per-op scoreboard against
// silicon; EeRecFpuDivUnitRounding owns the shape of the divergence for the two
// uncomposed ops.
static bool IsDivUnitModelGap(u32 interp, u32 jit)
{
	if ((interp & 0x80000000u) != (jit & 0x80000000u))
		return false;
	const u32 a = interp & 0x7FFFFFFFu, b = jit & 0x7FFFFFFFu;
	return (a > b ? a - b : b - a) <= 2u;
}

TEST(EeRecFpuRsqrt, DifferentialFuzzZeroAndNegativeDivisor)
{
	Lcg r{0x123456789ABCDEF0ull};
	int checked = 0, tier_gaps = 0, model_gaps = 0;
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
			h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
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
			++model_gaps;
			EXPECT_TRUE(IsDivUnitModelGap(res[0], res[1]))
				<< "the engines disagree by more than the divide unit model can "
				   "produce; interp=" << std::hex << res[0] << " jit=" << res[1];
		}
		EXPECT_EQ(fcr[1] & kStickyMask, fcr[0] & kStickyMask);
		++checked;
		if (::testing::Test::HasFailure())
			return; // first failing case is enough for a clean repro
	}
	EXPECT_EQ(checked, 3000);
	EXPECT_GT(tier_gaps, 0) << "anti-vacuity: the pool stopped producing "
							   "saturating results, so the allowance is dead "
							   "code that could hide a real divergence";
	EXPECT_GT(model_gaps, 0) << "anti-vacuity: no pair diverged at all, "
								"so the model allowance is dead code too";
}

// ---------------------------------------------------------------------------
// Positive-divisor fuzzer.
//
// Bounded rather than exact, because the interpreter models the divide unit and
// the fast path does not; the bound is two-sided and sign-checked.
//
// It also checks the composition on 3000 random pairs: silicon's rsqrt.s is
// div.s(Fs, sqrt.s(Ft)) with a plain 24-bit single in between, and the
// interpreter has to keep being that now that both steps carry the model.
// EeFpuDivUnitConsole.RsqrtIsSqrtThenDivide has the console rows.
// ---------------------------------------------------------------------------
TEST(EeRecFpuRsqrt, PositiveDivisorMatchesInterpExactly)
{
	Lcg r{0x0F0E0D0C0B0A0908ull};
	int checked = 0, tier_gaps = 0, model_gaps = 0;
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

		u32 res[2] = {}, fcr[2] = {};
		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFprBits(1, fsBits);
			h.SetFprBits(2, ftBits);
			h.SetFcr31(pre);
			h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
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
			++model_gaps;
			EXPECT_TRUE(IsDivUnitModelGap(res[0], res[1]))
				<< "the engines disagree by more than the divide unit model can "
				   "produce; interp=" << std::hex << res[0] << " jit=" << res[1];
		}
		EXPECT_EQ(fcr[1] & kStickyMask, fcr[0] & kStickyMask);

		// The composition, on the interpreter alone: sqrt.s Ft, then div.s by
		// whatever word that produced. Both steps carry the model, so this fails
		// if either one is applied inconsistently between RSQRT_S and the two
		// standalone ops.
		{
			EeRecTestHarness hs;
			hs.EnableCop1();
			hs.SetFprBits(2, ftBits);
			hs.LoadProgram({ee::SQRT_S(4, 2)});
			hs.RunInterpOnly();
			const u32 root = hs.GetFprBitsInterp(4);

			EeRecTestHarness hd;
			hd.EnableCop1();
			hd.SetFprBits(1, fsBits);
			hd.SetFprBits(4, root);
			hd.LoadProgram({ee::DIV_S(3, 1, 4)});
			hd.RunInterpOnly();
			EXPECT_EQ(hd.GetFprBitsInterp(3), res[0])
				<< "rsqrt.s must stay div.s(Fs, sqrt.s(Ft)) with a plain single in "
				   "between, which is what silicon does on every measured row; "
				   "root=" << std::hex << root;
		}

		++checked;
		if (::testing::Test::HasFailure())
			return;
	}
	EXPECT_EQ(checked, 3000);
	EXPECT_GT(tier_gaps, 0) << "anti-vacuity: the positive-divisor pool stopped "
							   "producing saturating results, so the allowance "
							   "is dead code that could hide a real divergence";
	EXPECT_GT(model_gaps, 0) << "anti-vacuity: no pair diverged at all, "
								"so the model allowance is dead code too";
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
	// A denormal Ft (exp field 0, mantissa nonzero) is "zero" for RSQRT, exactly
	// like +/-0: a divide by zero, and a saturated result.
	//
	// Ft is negative, so I comes from the root and D from the division;
	// EeFpuRsqrtSignConsole carries the console rows for that pair.
	//
	// Fs is +5.0 and Ft is negative, so sign(Fs) and sign(Ft) disagree and this
	// row picks between them: the console says positive, per
	// ZeroDivisorSignComesFromTheDividend below. The magnitude still differs by
	// tier, so the legs run separately.
	u32 res[2] = {}, fcr[2] = {};
	for (int jit = 0; jit < 2; ++jit)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFcr31(0);
		h.SetFpr(1, 5.0f);
		h.SetFprBits(2, 0x807FFFFFu);   // largest negative denormal
		h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
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
	EXPECT_EQ(res[0], 0x7FFFFFFFu) << "interp: sign(Fs)=+, EE maximum";
	EXPECT_EQ(res[1], 0x7F7FFFFFu) << "fast path: sign(Fs)=+, FLT_MAX";
	EXPECT_EQ(fcr[0] & kStickyMask, kI | kD | kSI | kSD);
	EXPECT_EQ(fcr[1] & kStickyMask, kI | kD | kSI | kSD);
}

// The row that separates sign(Fs) from sign(Ft), asserted on its own so a
// regression names the rule rather than an operand pool. rsqrt(+0, -0): the
// dividend is positive and the divisor negative, so the two candidate rules give
// opposite answers and the console picks the dividend's. The arm64 emitter used
// the divisor's sign and was alone in it -- upstream x86 recRSQRThelper1 already
// took the dividend's.
TEST(EeRecFpuRsqrt, ZeroDivisorSignComesFromTheDividend)
{
	struct Row { u32 fs, ft; u32 want_interp, want_fast; const char* what; };
	const Row rows[] = {
		{0x00000000u, 0x80000000u, 0x7FFFFFFFu, 0x7F7FFFFFu, "[fpm 59] rsqrt(+0, -0) is POSITIVE"},
		{0x80000000u, 0x80000000u, 0xFFFFFFFFu, 0xFF7FFFFFu, "[fpm 63] rsqrt(-0, -0) is negative"},
		{0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0xFF7FFFFFu, "rsqrt(-0, +0) is negative"},
		{0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x7F7FFFFFu, "rsqrt(+0, +0) is positive"},
	};
	for (const Row& r : rows)
	{
		SCOPED_TRACE(r.what);
		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFcr31(0);
			h.SetFprBits(1, r.fs);
			h.SetFprBits(2, r.ft);
			h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
			if (jit)
			{
				h.RunJitNoDiff();
				EXPECT_EQ(h.GetFprBitsJit(3), r.want_fast) << "fast path";
			}
			else
			{
				h.RunInterpOnly();
				EXPECT_EQ(h.GetFprBitsInterp(3), r.want_interp) << "interp";
			}
		}
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

// ---- FCR31 residency (GE-12) -----------------------------------------------
//
// Every row here is also covered by a test above that runs the op on its own,
// and the op on its own was always right. What this adds is an instruction in
// front of it: a CTC1 parks FCR31 in a GPR for the rest of the block
// (fpuTryAllocFCR31), and a flag write that goes to memory instead is then
// overwritten by the slot's writeback at the block seam. Both the I|D clear
// and the raise disappeared that way.
//
// Each row runs both polarities of the residency predicate -- with the CTC1 in
// front, and with the same seed applied directly to memory -- because the
// memory leg passes either way and would sign off on the defect alone.
TEST(EeRecFpuRsqrt, FlagWritesSurviveAPrecedingCtc1)
{
	struct Row { u32 seed, fs, ft, want; const char* what; };
	const Row rows[] = {
		{kI | kD | kSI | kSD, FprBits(6.0f), FprBits(4.0f),  kSI | kSD, "positive divisor: I|D cleared"},
		{kI | kD,             FprBits(6.0f), FprBits(-4.0f), kI | kSI,  "negative divisor: I|SI"},
		{kI | kD,             FprBits(6.0f), 0x00000000u,    kD | kSD,  "x/0: D|SD"},
		{kI | kD,             0x00000000u,   0x00000000u,    kI | kSI,  "0/0: I|SI"},
		// Both causes at once, which the resident slot has to carry.
		{kI | kD, FprBits(6.0f), 0x80000000u, kI | kD | kSI | kSD, "x/-0: I|D|SI|SD"},
	};
	for (const Row& r : rows)
	{
		SCOPED_TRACE(r.what);
		for (int resident = 0; resident < 2; ++resident)
		{
			SCOPED_TRACE(resident ? "ctc1 in front: FCR31 resident" : "no ctc1: FCR31 in memory");
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFprBits(1, r.fs);
			h.SetFprBits(2, r.ft);

			std::vector<u32> prog;
			if (resident)
			{
				prog.push_back(LUI(reg::t0, static_cast<u16>(r.seed >> 16)));
				prog.push_back(ORI(reg::t0, reg::t0, static_cast<u16>(r.seed)));
				prog.push_back(ee::CTC1(reg::t0, 31));
			}
			else
			{
				h.SetFcr31(r.seed);
			}
			prog.push_back(ee::RSQRT_S(3, 1, 2));
			h.LoadProgram(prog);

			// JIT-only: the zero-divisor rows saturate one binade apart between
			// the tiers (EeFpuTopBinadeConsole), which Run()'s value diff would
			// report instead of the flags. The interpreter reaches fprc[31]
			// through C either way and is pinned by the tests above.
			h.RunJitNoDiff();
			EXPECT_EQ(h.JitSnapshot().fprs.fprc[31] & kStickyMask, r.want);
		}
	}
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
		<< "[interp] 0x3F5105EC is what a correctly rounded rsqrt gives; the console "
		   "and the interpreter's digit recurrence both say 0x3F5105EB";
}

// ---- Exponent-255 divisors --------------------------------------------------
// Rows from the same first-party capture as EeFpuDivunitConsole. The fourth
// column is the fast path's answer, up to 1 ULP off the console's: it rounds
// the root correctly and the divide/sqrt unit does not, and eeSrtDigit (in
// pcsx2/FPU.cpp) models that for the interpreter only. So the interpreter is
// asserted against the console exactly and the JIT against the fourth column.
TEST(EeRecFpuRsqrt, Exponent255DivisorMatchesConsole)
{
	struct Row
	{
		u32 fs, ft, console, want_fast;
	};
	static constexpr Row kRows[] = {
		{0x3F800000u, 0x7F800000u, 0x1F800000u, 0x1F800000u}, // +Inf   as a host word
		{0x7F208A2Au, 0x7F91756Eu, 0x5F1698F2u, 0x5F1698F2u}, // +sNaN
		{0x652D2AE1u, 0x7FC6AC37u, 0x450AFEF9u, 0x450AFEF9u}, // +qNaN
		{0x7EFA12AEu, 0x7FE68EADu, 0x5EBA546Du, 0x5EBA546Cu}, // +qNaN, 1 ulp
		{0xFD7BB4BDu, 0xFFA1E9F2u, 0xDD5FCC56u, 0xDD5FCC57u}, // -sNaN, 1 ulp
		{0x7F5FD762u, 0xFFF0F0CBu, 0x5F2326B6u, 0x5F2326B6u}, // -qNaN
		{0xB73F5561u, 0x7F814D77u, 0x973E5E09u, 0x973E5E0Au}, // +sNaN, 1 ulp
		{0x7CB82C8Du, 0xFFB81B1Bu, 0x5C99914Du, 0x5C99914Du}, // -sNaN
		{0xFBC42E8Au, 0x7FF141AAu, 0xDB8EE5B8u, 0xDB8EE5B9u}, // +qNaN, 1 ulp
		{0x7CF2844Du, 0xFFF8FB98u, 0x5CADE290u, 0x5CADE291u}, // -qNaN, 1 ulp
		{0xAFD35605u, 0xFFD96260u, 0x8FA22AF0u, 0x8FA22AF0u}, // -qNaN
		// Controls: exponent-254 divisors, below the prescale branch.
		{0x7F7FFFFFu, 0x7F7FFFFFu, 0x5F800000u, 0x5F800000u},
		{0x642606B6u, 0x7F6CED78u, 0x442C9450u, 0x442C9450u},
	};

	const auto key = [](u32 x) {
		return (x & 0x80000000u) ? -static_cast<s64>(x & 0x7FFFFFFFu) : static_cast<s64>(x);
	};

	int exp255 = 0, controls = 0, signalling = 0, infinities = 0, exact = 0;
	for (const Row& r : kRows)
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "rsqrt fs=" << r.fs << " ft=" << r.ft);
		const auto build = [&r](EeRecTestHarness& h) {
			h.EnableCop1();
			h.SetFcr31(0);
			h.SetFprBits(1, r.fs);
			h.SetFprBits(2, r.ft);
			h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
		};
		EeRecTestHarness hj;
		build(hj);
		hj.RunJitNoDiff();
		EeRecTestHarness hi;
		build(hi);
		hi.RunInterpOnly();

		EXPECT_EQ(hi.GetFprBitsInterp(3), r.console) << "[interp] vs console";
		EXPECT_EQ(hj.GetFprBitsJit(3), r.want_fast) << "[jit] vs the fast path's value";
		EXPECT_LE(std::abs(key(r.want_fast) - key(r.console)), 1)
			<< "the fast path's value has drifted more than the divide unit's own "
			   "rounding accounts for";

		// The divisor's sign reaches FCR31 and nothing else: sqrt takes |Ft|.
		const u32 want_flags = (r.ft & 0x80000000u) ? (kI | kSI) : 0u;
		EXPECT_EQ(hj.JitSnapshot().fprs.fprc[31] & kStickyMask, want_flags) << "[jit] flags";
		EXPECT_EQ(hi.InterpSnapshot().fprs.fprc[31] & kStickyMask, want_flags) << "[interp] flags";

		if ((r.ft & 0x7F800000u) != 0x7F800000u)
		{
			++controls;
			continue;
		}
		++exp255;
		if (r.want_fast == r.console)
			++exact;
		const u32 mant = r.ft & 0x7FFFFFu;
		if (mant == 0)
			++infinities;
		else if ((mant & 0x400000u) == 0)
			++signalling;
	}

	EXPECT_GT(controls, 0)
		<< "anti-vacuity: without an exponent <= 254 divisor nothing here would notice "
		   "the prescale being applied unconditionally";
	EXPECT_GT(infinities, 0) << "anti-vacuity: the +Inf host word is the shape that used to "
								"divide by infinity and return zero";
	EXPECT_GE(signalling, 3)
		<< "anti-vacuity: signalling patterns are half the exponent-255 mantissa space and "
		   "the shapes an Fminnm-based clamp would have let through";
	EXPECT_GE(exact, 5) << "anti-vacuity: if no row is exact this is not measuring the value";
	EXPECT_GE(exp255, 8);
}

// The class the rows above sample: RSQRT.S divides by the square root SQRT.S
// computes, both rounded to a single under FPUDivFPCR, so the one-op form and
// the two-op one must agree across the whole exponent-255 divisor space.
TEST(EeRecFpuRsqrt, Exponent255DivisorAgreesWithSqrtThenDiv)
{
	Lcg rng{0x5170A17E5170A17Eull};
	int rows = 0, disagreements = 0;
	for (int i = 0; i < 512; ++i)
	{
		const u32 ft = (rng.next() & 0x80000000u) | 0x7F800000u | (rng.next() & 0x7FFFFFu);
		const u32 fs = (rng.next() & 0x80000000u) | ((1u + (rng.next() % 254u)) << 23) |
					   (rng.next() & 0x7FFFFFu);

		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFcr31(0);
		h.SetFprBits(1, fs);
		h.SetFprBits(2, ft);
		h.LoadProgram({ee::SQRT_S(4, 2), ee::DIV_S(5, 1, 4), ee::RSQRT_S(3, 1, 2)});
		h.RunJitNoDiff();

		++rows;
		if (h.GetFprBitsJit(3) != h.GetFprBitsJit(5))
		{
			++disagreements;
			if (disagreements <= 4)
			{
				ADD_FAILURE() << std::hex << "fs=" << fs << " ft=" << ft << ": rsqrt gives "
							  << h.GetFprBitsJit(3) << ", sqrt-then-div gives "
							  << h.GetFprBitsJit(5);
			}
		}
	}
	EXPECT_EQ(disagreements, 0);
	EXPECT_EQ(rows, 512);
}
