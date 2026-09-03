// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The EE's divide/square-root unit is not correctly rounded, and the two
// engines in this tree part company over that on purpose.
//
// The interpreter runs the unit's own radix-2 SRT digit recurrence (FPU.cpp,
// eeSrtDigit and below), which reproduces silicon bit for bit on every capture
// this project has taken, and arm64's eeClampMode 4 calls the same code out of
// line. The fast path this file exercises takes the host's fdiv/fsqrt under
// EmuConfig.Cpu.FPUDivFPCR -- FPUFPCR with round-to-nearest, swapped in around
// the three ops the unit owns; FPU.cpp names the emitters -- which makes it the
// correctly rounded engine.
//
// So this file is the class-level regression test for the shape of that
// divergence. Differing is not enough: the engines must differ by exactly one
// ULP, only on the ops the divide unit owns, and only in the direction silicon
// errs -- never above correct rounding on SQRT.S or on DIV.S's A>=B branch,
// either way on DIV.S's A<B branch. Anything else is still a bug, which is the
// property an "allow a mismatch" filter would throw away. The asymmetry is a
// count over the exhaustive console sweeps: 0 rows above correct rounding in
// 16,777,216 sqrt rows and in all 72,907,916 A>=B div rows, against 3,229,727
// above and 7,197,471 below in the 78,087,028 A<B rows.
//
// The premise guard below is what makes the fast path the correctly rounded
// side of the comparison, and no ScopedFpEnv belongs here: where FPUFPCR and
// FPUDivFPCR are equal the emitters' swap does nothing, the fast path chops,
// and the divergence is a different one. The interpreter reads neither
// register, which TheDivideUnitIgnoresItsRoundingModeKnob at the bottom
// asserts.
//
// The SQRT.S sweep also turned up an unrelated defect on its first run -- the
// interpreter returned -0.0 for sqrt(-0.0) where the EE returns +0.0 -- fixed
// separately and pinned by EeRecFpu.SqrtSOfNegativeZeroIsPositiveZero.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EeFpuModel.h"
#include "common/FPControl.h"

#include <algorithm>
#include <bit>
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

// The twin of the above for the DIVIDE unit's register, used by
// TheDivideUnitIgnoresItsRoundingModeKnob at the bottom -- which needs to move
// the knob for both engines: the interpreter must not respond to it and the
// fast path must.
struct ScopedDivideRoundMode
{
	FPControlRegister saved_cfg, saved_host;
	explicit ScopedDivideRoundMode(FPRoundMode mode)
		: saved_cfg(EmuConfig.Cpu.FPUDivFPCR)
		, saved_host(FPControlRegister::GetCurrent())
	{
		EmuConfig.Cpu.FPUDivFPCR.SetRoundMode(mode);
	}
	~ScopedDivideRoundMode()
	{
		EmuConfig.Cpu.FPUDivFPCR = saved_cfg;
		FPControlRegister::SetCurrent(saved_host);
	}
	ScopedDivideRoundMode(const ScopedDivideRoundMode&) = delete;
	ScopedDivideRoundMode& operator=(const ScopedDivideRoundMode&) = delete;
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

// The predicates are recomputed here rather than exported from FPU.cpp: a
// differential that imports the implementation's arithmetic cannot catch the
// implementation's arithmetic being wrong.
static bool BothNormalOperands(u32 fs, u32 ft)
{
	return ((fs >> 23) & 0xFFu) != 0 && ((ft >> 23) & 0xFFu) != 0;
}

// Which branch of the recurrence a division takes. The two branches err
// differently and the tests below hold them to different shapes.
static bool DivideShiftsTheNumerator(u32 fs, u32 ft)
{
	return (0x800000u | (fs & 0x7FFFFFu)) < (0x800000u | (ft & 0x7FFFFFu));
}

// The two candidates a digit recurrence can land on are adjacent words, so
// "one ULP apart" is "one apart as magnitudes" -- true across a binade boundary
// as well, since the float encoding is monotone in the magnitude.
static bool IsOneUlpApart(u32 interp, u32 jit)
{
	const u32 a = interp & 0x7FFFFFFFu, b = jit & 0x7FFFFFFFu;
	return (interp & 0x80000000u) == (jit & 0x80000000u) &&
	       (a > b ? a - b : b - a) == 1u;
}

// The interpreter's word is the JIT's with one unit taken off the magnitude.
static bool IsOneUlpTowardZero(u32 interp, u32 jit)
{
	return (jit & 0x7FFFFFFFu) != 0 &&
	       interp == ((jit & 0x80000000u) | ((jit & 0x7FFFFFFFu) - 1u));
}

TEST(EeRecFpuDivUnitRounding, DivSDivergesFromTheFastPathByOneUlpAndOnlyThat)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0xD1F5D1F5A5A5A5A5ull};
	int checked = 0, tier_gaps = 0, gaps = 0, alb_low = 0, alb_high = 0;
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
			++gaps;
			EXPECT_TRUE(BothNormalOperands(fsBits, ftBits))
				<< "the engines parted company on an operand pair the divide unit "
				   "never sees the digits of -- a zero or denormal operand is a "
				   "flag question both engines answer the same way";
			EXPECT_TRUE(IsOneUlpApart(res[0], res[1]))
				<< "the recurrence can only ever land on one of the two candidates "
				   "the correctly rounded answer sits between; interp="
				<< std::hex << res[0] << " jit=" << res[1];
			if (DivideShiftsTheNumerator(fsBits, ftBits))
				((res[0] & 0x7FFFFFFFu) < (res[1] & 0x7FFFFFFFu) ? alb_low : alb_high)++;
			else
				EXPECT_TRUE(IsOneUlpTowardZero(res[0], res[1]))
					<< "on the A>=B branch silicon is one ULP LOW or exact and never "
					   "high -- 0 exceptions in 72,907,916 measured rows; interp="
					<< std::hex << res[0] << " jit=" << res[1];
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
	EXPECT_GT(gaps, 0) << "anti-vacuity: the two engines agreed on every operand "
						  "pair, so this test is asserting engine agreement under "
						  "a different name";
	// The A<B branch errs BOTH ways, and a pool that only ever produced one of
	// them would let a one-directional bug through the shape check above.
	EXPECT_GT(alb_low, 0) << "no A<B row came back below correct rounding";
	EXPECT_GT(alb_high, 0) << "no A<B row came back above correct rounding";
}

// A named witness alongside the fuzzer, so a regression reports a value a human
// can check by hand rather than an LCG iteration number. 1.0 / 3.0 is one ULP
// apart between chop and nearest, and the console lands on nearest here -- the
// recurrence agrees with correct rounding on this operand, which is why both
// engines are pinned to the same word.
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
		<< "[interp] 0x3EAAAAAA is the chopped value, which is neither what the "
		   "console returns nor what the recurrence produces";
}

// ---------------------------------------------------------------------------
// SQRT.S
// ---------------------------------------------------------------------------
TEST(EeRecFpuDivUnitRounding, SqrtSDivergesFromTheFastPathOnlyDownward)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0x5011EE5011EE1234ull};
	int gaps = 0;
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
			++gaps;
			EXPECT_TRUE(IsOneUlpTowardZero(res[0], res[1]))
				<< "silicon's square root is one ULP LOW or exact, never high -- 0 "
				   "exceptions in 16,777,216 exhaustive rows; interp="
				<< std::hex << res[0] << " jit=" << res[1];
		}
		EXPECT_EQ(fcr[1] & kStickyMask, fcr[0] & kStickyMask);
		if (::testing::Test::HasFailure())
			return;
	}
	EXPECT_GT(gaps, 0) << "anti-vacuity: the two engines agreed on every operand, "
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
		<< "[interp] 0x400F1BBC is the chopped value, which is neither what the "
		   "console returns nor what the recurrence produces";
}

// ---------------------------------------------------------------------------
// All four divide-unit rounding modes, and the interpreter answering none of
// them. On console the result does not depend on FCR31's rounding mode, on any
// flag, or on the operations before it; how that was sampled is in the block
// above eeSrtDigit() in FPU.cpp.
//
// So the interpreter must return the same word in all four modes, and the word
// has to be the console's. Each operand below is a first-party console row
// whose value differs from the correctly rounded one: an interpreter that went
// back to rounding would still be mode-independent under chop-vs-chop but would
// return the ieee column, and one that started reading the knob would return
// three different words.
//
// The liveness clause is the fast path. The same knob moved across the same
// operand must change what the recompilers produce, or "the interpreter ignores
// it" would be a statement about a knob that reaches nothing at all.
// ---------------------------------------------------------------------------
TEST(EeRecFpuDivUnitRounding, TheDivideUnitIgnoresItsRoundingModeKnob)
{
	enum Which { W_SQRT, W_DIV, W_RSQRT };
	struct Case
	{
		Which op;
		u32 fs, ft;
		u32 console, ieee;
		const char* what;
	};
	// From the SCPH-90000 captures in ee_fpu_divunit_console_tests.cpp, one row
	// per op, each with silicon and correct rounding one ULP apart.
	static constexpr Case kCases[] = {
		{W_SQRT,  0x00000000u, 0x45DAB6CDu, 0x42A75179u, 0x42A7517Au, "sqrt.s, silicon low"},
		{W_DIV,   0x42C654F9u, 0x3C908E7Bu, 0x45AF9DC4u, 0x45AF9DC5u, "div.s, silicon low"},
		{W_DIV,   0x44933C6Bu, 0x3ECD12D0u, 0x4537CCB1u, 0x4537CCB0u, "div.s, silicon high"},
		{W_RSQRT, 0x343DA5A8u, 0x44A43E1Du, 0x31A76B9Bu, 0x31A76B9Cu, "rsqrt.s, silicon high"},
	};

	const auto program = [](const Case& c) {
		switch (c.op)
		{
			case W_SQRT: return ee::SQRT_S(2, 1);
			case W_DIV:  return ee::DIV_S(2, 3, 1);
			default:     return ee::RSQRT_S(2, 3, 1);
		}
	};
	const auto run = [&](const Case& c, bool jit) {
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(1, c.ft);
		h.SetFprBits(3, c.fs);
		h.SetFcr31(0);
		h.LoadProgram({program(c)});
		if (jit)
		{
			h.RunJitNoDiff();
			return h.GetFprBitsJit(2);
		}
		h.RunInterpOnly();
		return h.GetFprBitsInterp(2);
	};

	static constexpr FPRoundMode kModes[] = {FPRoundMode::Nearest, FPRoundMode::NegativeInfinity,
											 FPRoundMode::PositiveInfinity, FPRoundMode::ChopZero};
	static constexpr const char* kModeNames[] = {"nearest", "toward -inf", "toward +inf",
												 "toward zero"};
	int jit_moved = 0;
	for (const Case& c : kCases)
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "fs=" << c.fs << " ft=" << c.ft
										  << " (" << c.what << ")");
		ASSERT_NE(c.console, c.ieee) << "this row cannot tell the two engines apart";
		u32 jit_first = 0;
		for (int m = 0; m < 4; ++m)
		{
			const ScopedDivideRoundMode mode{kModes[m]};
			EXPECT_EQ(run(c, false), c.console)
				<< "[interp] under " << kModeNames[m]
				<< ": the digit recurrence has no rounding step for a mode to reach, "
				   "and the correctly rounded value here would be " << std::hex << c.ieee;
			const u32 jit = run(c, true);
			if (m == 0)
				jit_first = jit;
			else if (jit != jit_first)
				++jit_moved;
		}
	}

	EXPECT_GT(jit_moved, 0)
		<< "liveness: the fast path did not move under any of the four modes either, "
		   "so this test cannot tell a knob the interpreter ignores from a knob that "
		   "reaches nothing";
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

// ---------------------------------------------------------------------------
// What the one ULP is worth.
//
// Mortal Kombat: Shaolin Monks derives a texture's log2 dimension the usual
// way, at EE 0x0026b1c4 and 0x0026b1d8:
//
//     lhu     v0, 8(s1)          ; the dimension, 64
//     cvt.s.w f12, f12
//     jal     logf
//     lwc1    f20, ln2
//     div.s   f0, f0, f20        ; log2(n) = ln(n) / ln(2)
//     cvt.w.s f1, f0             ; truncate
//
// ln(64) and ln(2) are singles, so their quotient is not 6: it is
// 5.99999965603464, and nearest rounding keeps it under 6 while the divide
// unit's A<B branch takes it up to exactly 6.0. cvt.w.s then truncates, and the
// gap between the engines stops being one ULP of a float and becomes a whole
// integer -- the game builds its dialog panel a power of two too small and the
// message overruns the border it is drawn inside.
//
// Both the quotient and its cvt.w.s are asserted. Mode 3 returns the unit's
// word on this row.
// ---------------------------------------------------------------------------
namespace {

enum class Tier { Fast, Full, Exact };

const char* TierName(Tier t)
{
	switch (t)
	{
		case Tier::Fast: return "fast path";
		case Tier::Full: return "eeClampMode 3";
		default: return "eeClampMode 4";
	}
}

void EnableTier(EeRecTestHarness& h, Tier t)
{
	if (t == Tier::Full)
		h.EnableFpuFullMode();
	else if (t == Tier::Exact)
		h.EnableFpuExactMode();
}

} // namespace

TEST(EeRecFpuDivUnitRounding, ATruncatedLog2NeedsTheUnitsOwnRoundUp)
{
	constexpr u32 kLnSixtyFour = 0x40851590u; // logf(64.0f) as the game computes it
	constexpr u32 kLnTwo = 0x3F317216u;
	constexpr u32 kConsole = 0x40C00000u; // 6.0
	constexpr u32 kRounded = 0x40BFFFFFu; // 5.9999995

	const auto run = [](bool jit, Tier tier, bool truncate) {
		EeRecTestHarness h;
		h.EnableCop1();
		if (jit)
			EnableTier(h, tier);
		h.SetFprBits(1, kLnSixtyFour);
		h.SetFprBits(2, kLnTwo);
		if (truncate)
			h.LoadProgram({ee::DIV_S(3, 1, 2), ee::CVT_W_S(3, 3)});
		else
			h.LoadProgram({ee::DIV_S(3, 1, 2)});
		if (jit)
		{
			h.RunJitNoDiff();
			return h.GetFprBitsJit(3);
		}
		h.RunInterpOnly();
		return h.GetFprBitsInterp(3);
	};

	EXPECT_EQ(run(false, Tier::Fast, false), kConsole) << "interp";
	EXPECT_EQ(run(true, Tier::Exact, false), kConsole) << "jit, " << TierName(Tier::Exact);
	EXPECT_EQ(run(true, Tier::Full, false), kConsole)
		<< "jit, " << TierName(Tier::Full) << ": the integer guard fires on this row";
	EXPECT_EQ(run(true, Tier::Fast, false), kRounded)
		<< "the fast path is the nearest-rounding engine and this operand is "
		   "one of the rows where that is not what the console returns";

	EXPECT_EQ(run(false, Tier::Fast, true), 6u) << "interp, truncated";
	EXPECT_EQ(run(true, Tier::Exact, true), 6u) << "jit " << TierName(Tier::Exact) << ", truncated";
	EXPECT_EQ(run(true, Tier::Full, true), 6u) << "jit " << TierName(Tier::Full) << ", truncated";
	EXPECT_EQ(run(true, Tier::Fast, true), 5u)
		<< "fast path, truncated: 6 means the operand no longer witnesses the deficit";
}

// ---------------------------------------------------------------------------
// At mode 3 the quotient's cvt.w.s integer must be the unit's. The pool puts
// quotients on or one word beside an integer: n times a divisor with a short
// significand is an exact single, and the word either side of it divided by
// the same divisor is one ULP off n. Register aliasing is varied.
// ---------------------------------------------------------------------------
namespace {

struct DivIntCase
{
	u32 fs, ft;
	u32 fd, fi; // fd: the quotient's register; fi: the integer's
	const char* what;
};

// n = m * 2^j and ft = mb * 2^k, m and mb sharing 24 bits so n * ft is exact;
// fs is that product or the word either side of it.
DivIntCase MakeDivIntCase(Lcg& r)
{
	const u32 mbits = 1u + r.next() % 22u;
	const u32 m = (1u << (mbits - 1)) | (r.next() & ((1u << (mbits - 1)) - 1u));
	// n from 2^-2 (its integer is 0) up past 2^33 (its cvt.w.s saturates)
	const int j = -2 - static_cast<int>(mbits - 1) + static_cast<int>(r.next() % 36u);
	const u32 dbits = 2u + r.next() % (23u - mbits); // 2..24-mbits: never a power of two
	const u32 mb = (1u << (dbits - 1)) | (r.next() & ((1u << (dbits - 1)) - 1u));
	const int k = -30 + static_cast<int>(r.next() % 61u);

	// product = m * mb * 2^(j + k), normalised to a 24-bit significand
	u64 pm = static_cast<u64>(m) * mb;
	int pe = j + k;
	while (pm < (1ull << 23)) { pm <<= 1; --pe; }
	// exponent field of a single whose significand is pm (in [2^23, 2^24)) and value pm * 2^pe
	const int field = pe + 23 + 127;
	if (field < 1 || field > 254)
		return MakeDivIntCase(r); // out of range: draw again

	const u32 sign_s = (r.next() & 1u) << 31;
	const u32 sign_t = (r.next() & 1u) << 31;
	u32 fs = sign_s | (static_cast<u32>(field) << 23) | (static_cast<u32>(pm) & 0x7FFFFFu);
	switch (r.next() % 3u)
	{
		case 0: break;          // exact: the quotient is n itself
		case 1: fs += 1u; break; // one word up the magnitude
		default: fs -= 1u; break;
	}
	// ft: mb normalised the same way
	u64 tm = mb;
	int te = k;
	while (tm < (1ull << 23)) { tm <<= 1; --te; }
	const int tfield = te + 23 + 127;
	if (tfield < 1 || tfield > 254)
		return MakeDivIntCase(r);
	const u32 ft = sign_t | (static_cast<u32>(tfield) << 23) | (static_cast<u32>(tm) & 0x7FFFFFu);

	DivIntCase c{fs, ft, 3, 4, "fd, fs, ft distinct"};
	switch (r.next() % 4u)
	{
		case 0: break;
		case 1: c.fd = 1; c.what = "fd == fs"; break;
		case 2: c.fd = 2; c.what = "fd == ft"; break;
		default: c.fi = 3; c.what = "fi == fd"; break;
	}
	return c;
}

u32 TruncateLikeCvtWS(u32 w)
{
	const int e = static_cast<int>((w >> 23) & 0xFFu) - 127;
	if (e < 0)
		return 0;
	if (e >= 31)
		return (w & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
	const u32 mag = (0x800000u | (w & 0x7FFFFFu)) >> (23 - e);
	return (w & 0x80000000u) ? static_cast<u32>(-static_cast<s32>(mag)) : mag;
}

} // namespace

TEST(EeRecFpuDivUnitRounding, FullModeTruncatesTheQuotientLikeTheUnit)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0x1D1E5C7A5E1D1E5Cull};
	int fast_int_gaps = 0, full_word_gaps = 0, guard_answered = 0, exact_rows = 0;
	for (u32 iter = 0; iter < 1200; ++iter)
	{
		// Every fourth row is an arbitrary pair.
		DivIntCase c = (iter % 4u == 3u)
			? DivIntCase{fuzzOperand(r), fuzzOperand(r), 3, 4, "arbitrary"}
			: MakeDivIntCase(r);
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;
		SCOPED_TRACE(::testing::Message()
			<< "iter=" << iter << " Fs=" << std::hex << c.fs << " Ft=" << c.ft
			<< " fd=" << c.fd << " fi=" << c.fi << " (" << c.what << ") pre=" << pre);

		u32 word[3] = {}, integer[3] = {}, fcr[3] = {};
		for (int engine = 0; engine < 3; ++engine) // interp, jit full, jit fast
		{
			EeRecTestHarness h;
			h.EnableCop1();
			if (engine == 1)
				h.EnableFpuFullMode();
			h.SetFprBits(1, c.fs);
			h.SetFprBits(2, c.ft);
			h.SetFcr31(pre);
			h.LoadProgram({ee::DIV_S(c.fd, 1, 2), ee::CVT_W_S(c.fi, c.fd)});
			if (engine == 0)
			{
				h.RunInterpOnly();
				word[0] = h.GetFprBitsInterp(c.fd);
				integer[0] = h.GetFprBitsInterp(c.fi);
				fcr[0] = h.InterpSnapshot().fprs.fprc[31];
			}
			else
			{
				h.RunJitNoDiff();
				word[engine] = h.GetFprBitsJit(c.fd);
				integer[engine] = h.GetFprBitsJit(c.fi);
				fcr[engine] = h.JitSnapshot().fprs.fprc[31];
			}
		}
		// fi == fd overwrites the quotient.
		const bool word_visible = c.fi != c.fd;

		EXPECT_EQ(integer[1], integer[0])
			<< "mode 3's cvt.w.s integer is not the unit's: interp word=" << std::hex
			<< word[0] << " jit word=" << word[1];
		if (word_visible)
		{
			if (word[1] != word[0])
			{
				++full_word_gaps;
				EXPECT_TRUE(IsOneUlpApart(word[0], word[1]))
					<< "mode 3 word is not adjacent to the unit's; interp=" << std::hex << word[0]
					<< " jit=" << word[1];
				EXPECT_EQ(TruncateLikeCvtWS(word[1]), TruncateLikeCvtWS(word[0]))
					<< "mode 3 kept the host word on a row whose integer differs";
			}
			if (integer[2] != integer[0])
			{
				++fast_int_gaps;
				if (word[1] == word[0])
					++guard_answered;
			}
		}
		else if (integer[2] != integer[0])
		{
			++fast_int_gaps;
			++guard_answered;
		}
		EXPECT_EQ(fcr[1] & kStickyMask, fcr[0] & kStickyMask) << "mode 3 moved a sticky flag";
		if (IsTopBinadeTierGap(word[0], word[1]))
			ADD_FAILURE() << "mode 3 holds the EE range; a FLT_MAX word is the fast path's";
		if (c.what[0] != 'a' && (c.fs & 0x7FFFFFFFu) != 0 && word_visible && word[1] == word[0] &&
			integer[2] == integer[0])
			++exact_rows;
		if (::testing::Test::HasFailure())
			return;
	}
	RecordProperty("fast_int_gaps", fast_int_gaps);
	RecordProperty("full_word_gaps", full_word_gaps);
	RecordProperty("guard_answered", guard_answered);
	RecordProperty("exact_rows", exact_rows);
	EXPECT_GT(fast_int_gaps, 0) << "anti-vacuity: no row where the fast path's integer differs";
	EXPECT_GT(full_word_gaps, 0) << "anti-vacuity: mode 3 returned the unit's word on every row";
	EXPECT_GT(guard_answered, 0) << "liveness: the guard never fired";
	EXPECT_GT(exact_rows, 0) << "the pool's exact arm produced nothing";
}

// ---------------------------------------------------------------------------
// What the SQRT.S and RSQRT.S guards rely on, checked against the interpreter's
// models. SQRT.S: the unit's root is the nearest single or one word below it,
// and nearest whenever nearest rounds down or is exact. Every input of the
// recurrence: 2^23 significands at both exponent parities.
// ---------------------------------------------------------------------------
namespace {

// floor(sqrt(x)) for x below 2^48, from a double seed corrected both ways.
u64 ISqrt48(u64 x)
{
	u64 r = static_cast<u64>(std::sqrt(static_cast<double>(x)));
	while (r > 0 && r * r > x)
		--r;
	while ((r + 1) * (r + 1) <= x)
		++r;
	return r;
}

// The nearest single root of a normal word, and whether nearest rounded up.
// Placed as the recurrence places it: the significand shifted one place on
// an odd exponent field and two on an even one, rooted at 2^22 scale.
u32 NearestSqrtWord(u32 ft, bool* rounded_up, bool* exact)
{
	const u32 E = (ft >> 23) & 0xFFu;
	const u64 m = static_cast<u64>(0x800000u | (ft & 0x7FFFFFu)) << ((E & 1u) ? 1 : 2);
	const u64 x = m << 22;
	const u64 R = ISqrt48(x);
	const u64 rem = x - R * R;
	*exact = rem == 0;
	*rounded_up = rem > R; // the exact root exceeds R + 1/2
	const u32 sig = static_cast<u32>(R) + (*rounded_up ? 1u : 0u);
	return (((E + 127u) >> 1) << 23) | (sig & 0x7FFFFFu);
}

} // namespace

TEST(EeRecFpuDivUnitRounding, TheUnitsRootIsNearestOrTheWordBelow)
{
	u64 equal = 0, below = 0, above = 0, other = 0, down_rows = 0, down_and_below = 0;
	for (u32 E : {127u, 128u})
	{
		for (u32 man = 0; man < (1u << 23); ++man)
		{
			const u32 ft = (E << 23) | man;
			bool up = false, exact = false;
			const u32 nearest = NearestSqrtWord(ft, &up, &exact);
			const u32 unit = EeFpuModel::SqrtBits(ft);
			if (unit == nearest)
				++equal;
			else if (unit + 1u == nearest)
				++below;
			else if (unit == nearest + 1u)
				++above;
			else
				++other;
			if (!up)
			{
				++down_rows;
				if (unit != nearest)
					++down_and_below;
			}
		}
	}
	EXPECT_EQ(above, 0u) << "a root above nearest: the recurrence has no such row";
	EXPECT_EQ(other, 0u) << "a root more than one word from nearest";
	EXPECT_EQ(down_and_below, 0u) << "nearest rounded down or was exact and the unit differs";
	EXPECT_GT(below, 0u) << "liveness: no row where the unit sits below nearest";
	EXPECT_GT(equal, 0u);
	EXPECT_GT(down_rows, 0u);
	RecordProperty("equal", static_cast<int>(equal));
	RecordProperty("below", static_cast<int>(below));
}

// ---------------------------------------------------------------------------
// RSQRT.S at mode 3 divides by the double root, so the unit's word is within a
// window of the host's rather than adjacent to it. The bound: the single root
// is at most 1.5 words below the true root and 0.5 above; one root word is at
// most two quotient words; the recurrence adds T or T+1. That gives [-2, +4].
// Flag paths and flushed or saturated quotients are skipped and counted.
// ---------------------------------------------------------------------------
TEST(EeRecFpuDivUnitRounding, RsqrtSAtFullModeStaysInsideTheWindow)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0x5E1DC0DE5E1DC0DEull};
	constexpr int kLo = -2, kHi = 4;
	int hist[kHi - kLo + 1] = {};
	int measured = 0, skipped = 0, outside = 0, min_d = 0, max_d = 0;
	// Normal operands; every fourth row is the widest-window corner, a root
	// just above a power of two (odd exponent field, small significand) under
	// a quotient just below one.
	const auto normal = [&](u32 lo_exp, u32 n_exp) {
		return (lo_exp + r.next() % n_exp) << 23 | (r.next() & 0x7FFFFFu);
	};
	for (u32 iter = 0; iter < 20000; ++iter)
	{
		u32 fsBits, ftBits;
		if (iter % 4u == 3u)
		{
			ftBits = ((1u + 2u * (r.next() % 127u)) << 23) | (r.next() & 0xFFu);
			fsBits = normal(1u, 254u) | (0x7FFFFFu - (r.next() & 0xFFu));
		}
		else
		{
			fsBits = normal(1u, 254u);
			ftBits = normal(1u, 254u);
		}
		fsBits |= (r.next() & 1u) << 31;
		SCOPED_TRACE(::testing::Message()
			<< "iter=" << iter << " Fs=" << std::hex << fsBits << " Ft=" << ftBits);

		u32 res[2] = {};
		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			if (jit)
				h.EnableFpuFullMode();
			h.SetFprBits(1, fsBits);
			h.SetFprBits(2, ftBits);
			h.SetFcr31(0);
			h.LoadProgram({ee::RSQRT_S(3, 1, 2)});
			if (jit)
			{
				h.RunJitNoDiff();
				res[1] = h.GetFprBitsJit(3);
			}
			else
			{
				h.RunInterpOnly();
				res[0] = h.GetFprBitsInterp(3);
			}
		}
		const u32 um = res[0] & 0x7FFFFFFFu, hm = res[1] & 0x7FFFFFFFu;
		if (((fsBits >> 23) & 0xFFu) == 0 || ((ftBits >> 23) & 0xFFu) == 0 || um == 0 || hm == 0 ||
			um >= 0x7F800000u || hm >= 0x7F800000u)
		{
			++skipped;
			continue;
		}
		EXPECT_EQ(res[0] & 0x80000000u, res[1] & 0x80000000u) << "signs differ";
		const s64 d = static_cast<s64>(um) - static_cast<s64>(hm);
		++measured;
		if (measured == 1)
			min_d = max_d = static_cast<int>(d);
		min_d = std::min<int>(min_d, static_cast<int>(d));
		max_d = std::max<int>(max_d, static_cast<int>(d));
		if (d < kLo || d > kHi)
		{
			++outside;
			ADD_FAILURE() << "unit word " << std::hex << res[0] << " is " << std::dec << d
						  << " words from the mode-3 word " << std::hex << res[1];
		}
		else
			++hist[d - kLo];
		if (::testing::Test::HasFailure())
			return;
	}
	for (int i = 0; i < kHi - kLo + 1; ++i)
		RecordProperty((std::string("d_") + std::to_string(i + kLo)).c_str(), hist[i]);
	RecordProperty("measured", measured);
	RecordProperty("skipped", skipped);
	RecordProperty("min_d", min_d);
	RecordProperty("max_d", max_d);
	EXPECT_GT(measured, 10000);
	EXPECT_GT(hist[0 - kLo], 0) << "no row where the engines agree";
	EXPECT_LT(min_d, 0) << "liveness: the unit was never below the host word";
	EXPECT_GT(max_d, 1) << "liveness: the unit was never more than one word above the host";
}

// ---------------------------------------------------------------------------
// The same class for the root and the reciprocal root. SQRT.S: the radicand
// is an exact square n^2 (n of at most twelve significant bits) or the word
// either side of it. RSQRT.S: the divisor is such a square or its neighbour,
// the dividend n times the root or its neighbour.
// ---------------------------------------------------------------------------
namespace {

// The single nearest to m * 2^e for m below 2^24, or 0 when the exponent
// field would leave [1, 254].
u32 SingleFromScaled(u64 m, int e)
{
	if (m == 0)
		return 0;
	while (m < (1ull << 23)) { m <<= 1; --e; }
	while (m >= (1ull << 24)) { m >>= 1; ++e; } // only reached by the product below
	const int field = e + 23 + 127;
	if (field < 1 || field > 254)
		return 0;
	return (static_cast<u32>(field) << 23) | (static_cast<u32>(m) & 0x7FFFFFu);
}

// A square n^2 as an exact single, with n = mn * 2^j, mn of `bits` bits.
u32 ExactSquare(Lcg& r, u32 bits, int j, u32* n_word)
{
	const u32 mn = (1u << (bits - 1)) | (r.next() & ((1u << (bits - 1)) - 1u));
	*n_word = SingleFromScaled(mn, j);
	return SingleFromScaled(static_cast<u64>(mn) * mn, 2 * j);
}

u32 Nudge(Lcg& r, u32 w)
{
	switch (r.next() % 3u)
	{
		case 0: return w;
		case 1: return w + 1u;
		default: return w - 1u;
	}
}

struct RootCase
{
	u32 fs, ft, fd, fi;
	const char* what;
};

RootCase MakeSqrtCase(Lcg& r)
{
	u32 n = 0;
	const u32 bits = 1u + r.next() % 12u;
	const int j = -2 - static_cast<int>(bits - 1) + static_cast<int>(r.next() % 36u);
	const u32 sq = ExactSquare(r, bits, j, &n);
	if (sq == 0 || n == 0)
		return MakeSqrtCase(r);
	RootCase c{0, Nudge(r, sq), 3, 4, "fd, ft distinct"};
	switch (r.next() % 3u)
	{
		case 0: break;
		case 1: c.fd = 2; c.what = "fd == ft"; break;
		default: c.fi = 3; c.what = "fi == fd"; break;
	}
	return c;
}

RootCase MakeRsqrtCase(Lcg& r)
{
	u32 t = 0;
	const u32 tbits = 1u + r.next() % 12u;
	const int k = -20 + static_cast<int>(r.next() % 41u);
	const u32 sq = ExactSquare(r, tbits, k, &t);
	if (sq == 0 || t == 0)
		return MakeRsqrtCase(r);
	// n * t exactly: n's bits and t's share the 24
	const u32 nbits = 1u + r.next() % (24u - tbits);
	const u32 mn = (1u << (nbits - 1)) | (r.next() & ((1u << (nbits - 1)) - 1u));
	const int j = -2 - static_cast<int>(nbits - 1) + static_cast<int>(r.next() % 36u);
	const u64 mt = 0x800000u | (t & 0x7FFFFFu);
	const int et = static_cast<int>((t >> 23) & 0xFFu) - 127 - 23;
	const u32 prod = SingleFromScaled(static_cast<u64>(mn) * mt, j + et);
	if (prod == 0)
		return MakeRsqrtCase(r);
	RootCase c{Nudge(r, prod) | ((r.next() & 1u) << 31), Nudge(r, sq), 3, 4, "fd, fs, ft distinct"};
	switch (r.next() % 4u)
	{
		case 0: break;
		case 1: c.fd = 1; c.what = "fd == fs"; break;
		case 2: c.fd = 2; c.what = "fd == ft"; break;
		default: c.fi = 3; c.what = "fi == fd"; break;
	}
	return c;
}

// Runs one op on the three engines: interp, jit at mode 3, jit on the fast
// path. word[] is the op's result, integer[] its cvt.w.s.
void RunRootCase(const RootCase& c, bool rsqrt, u32 pre, u32 word[3], u32 integer[3], u32 fcr[3])
{
	for (int engine = 0; engine < 3; ++engine)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		if (engine == 1)
			h.EnableFpuFullMode();
		h.SetFprBits(1, c.fs);
		h.SetFprBits(2, c.ft);
		h.SetFcr31(pre);
		h.LoadProgram({rsqrt ? ee::RSQRT_S(c.fd, 1, 2) : ee::SQRT_S(c.fd, 2), ee::CVT_W_S(c.fi, c.fd)});
		if (engine == 0)
		{
			h.RunInterpOnly();
			word[0] = h.GetFprBitsInterp(c.fd);
			integer[0] = h.GetFprBitsInterp(c.fi);
			fcr[0] = h.InterpSnapshot().fprs.fprc[31];
		}
		else
		{
			h.RunJitNoDiff();
			word[engine] = h.GetFprBitsJit(c.fd);
			integer[engine] = h.GetFprBitsJit(c.fi);
			fcr[engine] = h.JitSnapshot().fprs.fprc[31];
		}
	}
}

} // namespace

TEST(EeRecFpuDivUnitRounding, FullModeTruncatesTheRootLikeTheUnit)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0x5011EE7A5011EE7Aull};
	int fast_int_gaps = 0, full_word_gaps = 0, guard_answered = 0;
	for (u32 iter = 0; iter < 1200; ++iter)
	{
		const RootCase c = (iter % 4u == 3u) ? RootCase{0, fuzzOperand(r), 3, 4, "arbitrary"} : MakeSqrtCase(r);
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;
		SCOPED_TRACE(::testing::Message() << "iter=" << iter << " Ft=" << std::hex << c.ft
										  << " fd=" << c.fd << " fi=" << c.fi << " (" << c.what << ")");
		u32 word[3] = {}, integer[3] = {}, fcr[3] = {};
		RunRootCase(c, false, pre, word, integer, fcr);
		const bool word_visible = c.fi != c.fd;

		EXPECT_EQ(integer[1], integer[0])
			<< "mode 3's cvt.w.s integer is not the unit's: interp word=" << std::hex << word[0]
			<< " jit word=" << word[1];
		if (word_visible && word[1] != word[0])
		{
			++full_word_gaps;
			EXPECT_TRUE(IsOneUlpTowardZero(word[0], word[1]))
				<< "the unit's root is nearest or the word below; interp=" << std::hex << word[0]
				<< " jit=" << word[1];
			EXPECT_EQ(TruncateLikeCvtWS(word[1]), TruncateLikeCvtWS(word[0]))
				<< "mode 3 kept the host word on a row whose integer differs";
		}
		if (integer[2] != integer[0])
		{
			++fast_int_gaps;
			if (!word_visible || word[1] == word[0])
				++guard_answered;
		}
		EXPECT_EQ(fcr[1] & kStickyMask, fcr[0] & kStickyMask) << "mode 3 moved a sticky flag";
		if (::testing::Test::HasFailure())
			return;
	}
	RecordProperty("fast_int_gaps", fast_int_gaps);
	RecordProperty("full_word_gaps", full_word_gaps);
	RecordProperty("guard_answered", guard_answered);
	EXPECT_GT(fast_int_gaps, 0) << "anti-vacuity: no row where the fast path's integer differs";
	EXPECT_GT(full_word_gaps, 0) << "anti-vacuity: mode 3 returned the unit's word on every row";
	EXPECT_GT(guard_answered, 0) << "liveness: the guard never fired";
}

TEST(EeRecFpuDivUnitRounding, FullModeTruncatesTheReciprocalRootLikeTheUnit)
{
	RequireDistinctDivideRoundingMode();
	Lcg r{0x25C1B00725C1B007ull};
	int fast_int_gaps = 0, full_word_gaps = 0, guard_answered = 0, min_d = 0, max_d = 0;
	for (u32 iter = 0; iter < 1200; ++iter)
	{
		const RootCase c = (iter % 4u == 3u)
			? RootCase{fuzzOperand(r), fuzzOperand(r) & 0x7FFFFFFFu, 3, 4, "arbitrary"}
			: MakeRsqrtCase(r);
		const u32 pre = (r.next() % 4u == 0u) ? (kSI | kSD) : 0u;
		SCOPED_TRACE(::testing::Message() << "iter=" << iter << " Fs=" << std::hex << c.fs << " Ft=" << c.ft
										  << " fd=" << c.fd << " fi=" << c.fi << " (" << c.what << ")");
		u32 word[3] = {}, integer[3] = {}, fcr[3] = {};
		RunRootCase(c, true, pre, word, integer, fcr);
		const bool word_visible = c.fi != c.fd;

		EXPECT_EQ(integer[1], integer[0])
			<< "mode 3's cvt.w.s integer is not the unit's: interp word=" << std::hex << word[0]
			<< " jit word=" << word[1];
		if (word_visible && word[1] != word[0] && !IsTopBinadeTierGap(word[0], word[1]))
		{
			++full_word_gaps;
			const u32 um = word[0] & 0x7FFFFFFFu, hm = word[1] & 0x7FFFFFFFu;
			const int d = static_cast<int>(static_cast<s64>(um) - static_cast<s64>(hm));
			EXPECT_EQ(word[0] & 0x80000000u, word[1] & 0x80000000u);
			if (um != 0 && hm != 0 && um < 0x7F800000u && hm < 0x7F800000u)
			{
				EXPECT_TRUE(d >= -2 && d <= 4)
					<< "outside the window the guard spans: interp=" << std::hex << word[0]
					<< " jit=" << word[1];
				min_d = std::min(min_d, d);
				max_d = std::max(max_d, d);
			}
			EXPECT_EQ(TruncateLikeCvtWS(word[1]), TruncateLikeCvtWS(word[0]))
				<< "mode 3 kept the host word on a row whose integer differs";
		}
		if (integer[2] != integer[0])
		{
			++fast_int_gaps;
			if (!word_visible || word[1] == word[0])
				++guard_answered;
		}
		EXPECT_EQ(fcr[1] & kStickyMask, fcr[0] & kStickyMask) << "mode 3 moved a sticky flag";
		if (::testing::Test::HasFailure())
			return;
	}
	RecordProperty("fast_int_gaps", fast_int_gaps);
	RecordProperty("full_word_gaps", full_word_gaps);
	RecordProperty("guard_answered", guard_answered);
	RecordProperty("min_d", min_d);
	RecordProperty("max_d", max_d);
	EXPECT_GT(fast_int_gaps, 0) << "anti-vacuity: no row where the fast path's integer differs";
	EXPECT_GT(full_word_gaps, 0) << "anti-vacuity: mode 3 returned the unit's word on every row";
	EXPECT_GT(guard_answered, 0) << "liveness: the guard never fired";
}

// ---------------------------------------------------------------------------
// A temp allocated inside a runtime arm. _allocTempNEONreg evicts when the pool
// is full, and an eviction writes the evicted guest register back at the point
// of allocation while the allocator forgets it unconditionally. Inside the
// divide's normal arm that store is skipped whenever the divisor is zero, and
// the guest register's newest value is lost. The block below fills the pool
// with dirty FPRs, divides by zero, and reads every one of them back.
// ---------------------------------------------------------------------------
TEST(EeRecFpuDivUnitRounding, DivideByZeroWithAFullPoolKeepsEveryDirtyFpr)
{
	constexpr int kFirst = 4, kLast = 29; // f4..f29 dirtied, f0..f3 are the divide's
	const auto run = [&](bool zero_divisor, bool exact_mode) {
		std::vector<u32> prog;
		for (int i = kFirst; i <= kLast; ++i)
			prog.push_back(ee::ADD_S(i, i, 1)); // f_i = i + 1.0
		prog.push_back(ee::DIV_S(3, 2, 0));
		EeRecTestHarness h;
		h.EnableCop1();
		if (exact_mode)
			h.EnableFpuExactMode();
		else
			h.EnableFpuFullMode();
		h.SetFpr(0, zero_divisor ? 0.0f : 2.0f);
		h.SetFpr(1, 1.0f);
		h.SetFpr(2, 6.0f);
		for (int i = kFirst; i <= kLast; ++i)
			h.SetFpr(i, static_cast<float>(i));
		h.LoadProgram(prog);
		h.RunJitNoDiff();
		int lost = 0;
		for (int i = kFirst; i <= kLast; ++i)
		{
			const u32 expect = std::bit_cast<u32>(static_cast<float>(i + 1));
			const u32 got = h.GetFprBitsJit(i);
			if (got != expect)
				++lost;
			EXPECT_EQ(got, expect) << "f" << i << (zero_divisor ? " after a divide by zero" : "")
								   << (exact_mode ? " at eeClampMode 4" : " at eeClampMode 3");
		}
		return lost;
	};

	EXPECT_EQ(run(false, false), 0) << "control: the normal arm ran, so every eviction's store ran";
	EXPECT_EQ(run(true, true), 0) << "control: mode 4 allocates nothing inside the arm";
	EXPECT_EQ(run(true, false), 0)
		<< "a dirty FPR evicted for a temp inside the normal arm was never written back";
}
