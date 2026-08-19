// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU1 EFU against real PS2 hardware.
//
// autocases_efu.h is generated from unknownbrackets/ps2autotests
// tests/vu/lower/efu.expected: all thirteen EFU opcodes against all sixteen
// constants, 208 cases.
//
// vu1_efu_p_pipeline_tests.cpp already exercises this unit, but only as a
// JIT-vs-interp differential over the architectural happy paths. A
// differential is structurally blind to anything the two engines get wrong
// together, which here is most of the family — so each engine is scored
// against the capture instead, and the cases it does not reproduce are
// recorded per engine in autocases_efu.h.
//
// The capture's program is three instructions, reproduced literally:
//     <efu op> vf01     (scalar forms take FIELD_Z — fs.z, fsf = 2)
//     waitp
//     mfp.xyzw vf02
// and it prints vf02.x. WAITP is what makes the read safe, so no latency pad
// is used here: with a pad the test would be measuring the pad rather than
// the interlock.

#include <gtest/gtest.h>

#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "VU.h"

#include <cstdio>
#include <string>

#include "autocases_efu.h"

using namespace ps2auto_efu;

namespace recompiler_tests
{
namespace
{
using namespace vu;

constexpr u32 kFs = vf::vf1, kFt = vf::vf2;
constexpr u32 kFieldZ = 2; // the capture's VU::FIELD_Z

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

u32 Encode(const EfuCase& c)
{
	const std::string op = c.op;
	if (c.scalar)
	{
		if (op == "EATAN") return VEATAN_L(kFs, kFieldZ);
		if (op == "EEXP") return VEEXP_L(kFs, kFieldZ);
		if (op == "ERCPR") return VERCPR_L(kFs, kFieldZ);
		if (op == "ERSQRT") return VERSQRT_L(kFs, kFieldZ);
		if (op == "ESIN") return VESIN_L(kFs, kFieldZ);
		if (op == "ESQRT") return VESQRT_L(kFs, kFieldZ);
		return 0;
	}
	if (op == "EATANxy") return VEATANXY_L(kFs);
	if (op == "EATANxz") return VEATANXZ_L(kFs);
	if (op == "ELENG") return VELENG_L(kFs);
	if (op == "ERLENG") return VERLENG_L(kFs);
	if (op == "ERSADD") return VERSADD_L(kFs);
	if (op == "ESADD") return VESADD_L(kFs);
	if (op == "ESUM") return VESUM_L(kFs);
	return 0;
}

// Runs one case and reports whether the engine matched silicon.
bool CaseMatches(const EfuCase& c, u32 word, bool jit)
{
	VuTestHarness h(1);
	h.SetVfBits(kFs, c.fs[0], c.fs[1], c.fs[2], c.fs[3]);
	h.SetVfBits(kFt, 0xCCCCCCCCu, 0xCCCCCCCCu, 0xCCCCCCCCu, 0xCCCCCCCCu);
	h.LoadProgram({
		LowerOnly(word),
		LowerOnly(VWAITP_L()),
		LowerOnly(VMFP_L(mask::xyzw, kFt)),
		EBitNopPair(),
	});
	h.RunNoDiff();
	const u32 got = jit ? h.GetVfBitsJit(kFt, 'x') : h.GetVfBitsInterp(kFt, 'x');
	return got == c.p;
}
} // namespace

// Asserts the cases this emulator DOES reproduce, and asserts that the ones it
// does not still fail — so both a regression and a fix trip the test rather
// than quietly shifting the allowance.
TEST(Vu1EfuConsoleConformance, OpsMatchConsole)
{
	int checked = 0, bad_interp = 0, bad_jit = 0;
	for (int i = 0; i < kEfuCaseCount; ++i)
	{
		const EfuCase& c = kEfuCases[i];
		const u32 word = Encode(c);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;

		for (int jit = 0; jit < 2; ++jit)
		{
			const bool known_bad = jit ? c.bad_jit : c.bad_interp;
			const bool ok = CaseMatches(c, word, jit != 0);
			if (!known_bad)
			{
				SCOPED_TRACE(::testing::Message()
				             << c.label << (jit ? " [jit]" : " [interp]"));
				EXPECT_TRUE(ok) << "new divergence from silicon";
			}
			else
			{
				(jit ? bad_jit : bad_interp)++;
				EXPECT_FALSE(ok)
					<< c.label << (jit ? " [jit]" : " [interp]")
					<< " now MATCHES silicon. If the EFU model was fixed, clear "
					   "this case's known-bad flag in autocases_efu.h.";
			}
		}
		++checked;
	}
	EXPECT_EQ(checked, kEfuCaseCount);
	EXPECT_EQ(bad_interp, kEfuBadInterp);
	EXPECT_EQ(bad_jit, kEfuBadJit);
}

// ---------------------------------------------------------------------------
// Cross-ENGINE agreement, which is a different question from the one above.
//
// Every test in this file so far scores each engine against silicon
// SEPARATELY and records what it cannot reproduce per engine
// (`bad_interp` / `bad_jit` in autocases_efu.h). That is deliberate -- the
// header explains that a pure JIT-vs-interp differential is blind to anything
// both engines get wrong together. But it leaves the mirror-image blind spot:
// nothing else asserts that the two engines agree with EACH OTHER, and a case
// where they return different wrong answers is flagged twice as known-bad and
// looks settled, because CaseMatches() reduces each run to a bool and throws
// the value away.
//
// What is left splits two ways.
//
//   1. THE RECOMPILERS RUN HOST ARITHMETIC. On CVF_MAX / CVF_MIN / CVF_*_EXP /
//      CVF_GARBAGE1 they hand a raw exponent-255 pattern to a host divide and
//      a host multiply and get infinities and NaNs back (0x7FC00000,
//      0x7FFFFFFF, 0x7FC00001). The interpreter takes the divide unit's
//      saturation and the FMAC model's range instead, and lands where the
//      console does.
//   2. THE LAST ULP. On ordinary inputs the two evaluate the same series, in
//      the same order, out of the same coefficients, and still part by one:
//      the interpreter rounds each step through the FMAC model, the
//      recompiler through NEON. DISABLED_DumpEatanFamily has the per-row
//      numbers.
//
// Listing them rather than skipping the family keeps the property asserted for
// the 25 that agree, and makes any movement in either direction -- a fix or a
// regression -- fail loudly.
constexpr const char* kEatanEngineDivergences[] = {
	"EATAN CVF_MAX",
	"EATAN CVF_MIN",
	"EATAN CVF_MAX_EXP",
	"EATAN CVF_NEGONE",
	"EATAN CVF_INCREASING",
	"EATAN CVF_PI_OVER2",
	"EATANxy CVF_ZERO",
	"EATANxy CVF_NEGZERO",
	"EATANxy CVF_MAX",
	"EATANxy CVF_MIN",
	"EATANxy CVF_MAX_EXP",
	"EATANxy CVF_MIN_EXP",
	"EATANxy CVF_GARBAGE1",
	"EATANxy CVF_DECREASING",
	"EATANxz CVF_ZERO",
	"EATANxz CVF_NEGZERO",
	"EATANxz CVF_MAX",
	"EATANxz CVF_MIN",
	"EATANxz CVF_MAX_EXP",
	"EATANxz CVF_MIN_EXP",
	"EATANxz CVF_GARBAGE1",
	"EATANxz CVF_INCREASING",
	"EATANxz CVF_DECREASING",
};

namespace
{
bool IsEatanOp(const char* op)
{
	const std::string o = op;
	return o == "EATAN" || o == "EATANxy" || o == "EATANxz";
}

bool EatanDivergenceKnown(const std::string& label)
{
	for (const char* k : kEatanEngineDivergences)
	{
		if (label == k)
			return true;
	}
	return false;
}

// One run yields BOTH engines' values, so this costs nothing over CaseMatches
// and -- unlike CaseMatches -- it keeps them.
void RunBothEngines(const EfuCase& c, u32 word, u32& jit, u32& interp)
{
	VuTestHarness h(1);
	h.SetVfBits(kFs, c.fs[0], c.fs[1], c.fs[2], c.fs[3]);
	h.SetVfBits(kFt, 0xCCCCCCCCu, 0xCCCCCCCCu, 0xCCCCCCCCu, 0xCCCCCCCCu);
	h.LoadProgram({
		LowerOnly(word),
		LowerOnly(VWAITP_L()),
		LowerOnly(VMFP_L(mask::xyzw, kFt)),
		EBitNopPair(),
	});
	h.RunNoDiff();
	jit = h.GetVfBitsJit(kFt, 'x');
	interp = h.GetVfBitsInterp(kFt, 'x');
}
} // namespace

TEST(Vu1EfuConsoleConformance, EatanFamilyEnginesAgreeExceptWhereListed)
{
	int checked = 0, diverged = 0;
	for (int i = 0; i < kEfuCaseCount; ++i)
	{
		const EfuCase& c = kEfuCases[i];
		if (!IsEatanOp(c.op))
			continue;
		const u32 word = Encode(c);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;

		u32 jit = 0, interp = 0;
		RunBothEngines(c, word, jit, interp);
		++checked;

		const std::string label = c.label;
		SCOPED_TRACE(::testing::Message() << label);
		if (EatanDivergenceKnown(label))
		{
			++diverged;
			EXPECT_NE(jit, interp)
				<< "the engines now AGREE here. If that is a fix, drop this "
				   "label from kEatanEngineDivergences.";
		}
		else
		{
			EXPECT_EQ(jit, interp)
				<< "engines disagree: jit=" << std::hex << jit
				<< " interp=" << interp << " (console " << c.p << ")";
		}
	}
	EXPECT_EQ(checked, 48) << "the EATAN family is 3 ops x 16 constants";
	EXPECT_EQ(diverged, static_cast<int>(std::size(kEatanEngineDivergences)));
}

// The argument-reduction defect itself, pinned as arithmetic rather than as a
// cross-engine comparison, so it stays meaningful even if both engines are
// later changed together.
//
// _vuCalculateEATAN ends by adding pi/4, which is only correct as the second
// half of  atan(x) = pi/4 + atan((x-1)/(x+1)).  Feeding it the RAW argument
// adds an unearned pi/4. For Fs = 1.0 the reduced argument is exactly 0, so
// the polynomial contributes nothing and the result is the constant alone --
// while the unreduced form gives 0x3FCA1D99, which is precisely what the
// interpreter returned before the fix.
//
// That makes this row a direct read of the constant, which is how the EFU's
// 0x3F490FDA came to replace the correctly-rounded 0x3F490FDB both engines
// used to add.
TEST(Vu1EfuConsoleConformance, EatanAppliesTheRangeReductionBeforeThePolynomial)
{
	const EfuCase* one = nullptr;
	for (int i = 0; i < kEfuCaseCount; ++i)
	{
		if (std::string(kEfuCases[i].op) == "EATAN"
			&& std::string(kEfuCases[i].label) == "EATAN CVF_ONE")
			one = &kEfuCases[i];
	}
	ASSERT_NE(one, nullptr) << "EATAN CVF_ONE missing from the capture";
	ASSERT_EQ(one->fs[2], 0x3F800000u) << "CVF_ONE's fs.z is no longer 1.0";

	u32 jit = 0, interp = 0;
	RunBothEngines(*one, Encode(*one), jit, interp);
	EXPECT_EQ(one->p, 0x3F490FDAu) << "the console's own constant";
	EXPECT_EQ(interp, one->p)
		<< "[interp] EATAN(1.0) must be the bare constant; 0x3FCA1D99 is the "
		   "unreduced form (polynomial evaluated at 1.0 plus pi/4)";
	EXPECT_EQ(jit, one->p) << "[jit]";
}

// The measurement behind the two lists above. Prints every EATAN-family case
// as interp / jit / console plus each engine's signed ULP distance from the
// capture, so the readings can be re-made from data instead of from the
// emitters' source. ULP is computed over the monotonic ordering of the float
// bit patterns, which is what "hundreds of ULP" in the comments refers to.
TEST(Vu1EfuConsoleConformance, DISABLED_DumpEatanFamily)
{
	auto ordinal = [](u32 b) -> s64 {
		// Map the sign-magnitude float encoding onto a monotonic integer.
		return (b & 0x80000000u) ? -static_cast<s64>(b & 0x7FFFFFFFu)
		                         : static_cast<s64>(b);
	};

	std::printf("\n%-26s %-9s %-9s %-9s %12s %12s\n",
		"case", "interp", "jit", "console", "interp-ulp", "jit-ulp");
	for (int i = 0; i < kEfuCaseCount; ++i)
	{
		const EfuCase& c = kEfuCases[i];
		if (!IsEatanOp(c.op))
			continue;
		u32 jit = 0, interp = 0;
		RunBothEngines(c, Encode(c), jit, interp);
		std::printf("%-26s %08x  %08x  %08x  %12lld %12lld  %s\n",
			c.label, interp, jit, c.p,
			static_cast<long long>(ordinal(interp) - ordinal(c.p)),
			static_cast<long long>(ordinal(jit) - ordinal(c.p)),
			jit == interp ? "" : "<-- ENGINES DIFFER");
	}
}

// What passing looks like once the EFU model is right. Also the way to
// regenerate the known-bad list: run it with --gtest_also_run_disabled_tests
// and take the label plus engine out of each failing SCOPED_TRACE.
TEST(Vu1EfuConsoleConformance, DISABLED_AllOpsMatchConsole)
{
	for (int i = 0; i < kEfuCaseCount; ++i)
	{
		const EfuCase& c = kEfuCases[i];
		const u32 word = Encode(c);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(::testing::Message()
			             << c.label << (jit ? " [jit]" : " [interp]"));
			EXPECT_TRUE(CaseMatches(c, word, jit != 0));
		}
	}
}

} // namespace recompiler_tests
