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
// both engines get wrong together. But it left the mirror-image blind spot,
// and at this stage that is the one that matters: NOTHING asserted that the
// two engines agree with EACH OTHER. A case where interp and the arm64 JIT
// return different wrong answers is flagged twice as "known bad" and looks
// settled, because CaseMatches() reduces each run to a bool and throws the
// value away.
//
// Measured when this test was written: 111 of the 208 cases had the two
// engines disagreeing. The first root cause is fixed in the same commit --
// the interpreter's EATAN family was missing the range-reduction identity's
// argument transform (see _vuCalculateEATAN's callers in VUops.cpp) -- which
// brought 15 cases into agreement and left 96.
//
// The remaining EATAN-family divergences are listed below and fall into two
// further classes, neither of which is this commit's subject:
//
//   1. OPERAND CLAMPING. On CVF_MAX / CVF_MIN / CVF_*_EXP / CVF_GARBAGE1 the
//      recompilers hand raw exponent-255 patterns to the polynomial and get
//      NaNs back (0x7FC00000, 0x7FFFFFFF, 0x7FC00001), where the interpreter
//      runs every operand through vuDouble first and gets a finite clamp. The
//      interpreter is the side nearer the console here.
//   2. POLYNOMIAL PRECISION. The interpreter evaluates the series with
//      double-precision pow() while both recompilers use a single-precision
//      Horner chain, so the two drift by a few ULP on ordinary inputs
//      (CVF_INCREASING, CVF_DECREASING, CVF_PI*). Again the interpreter is
//      usually nearer the console.
//
// Listing them rather than skipping the family keeps the property asserted for
// the 15 that now agree, and makes any movement in either direction -- a fix
// or a regression -- fail loudly.
constexpr const char* kEatanEngineDivergences[] = {
	"EATAN CVF_ZERO",
	"EATAN CVF_NEGZERO",
	"EATAN CVF_MAX",
	"EATAN CVF_MIN",
	"EATAN CVF_MAX_MANTISSA",
	"EATAN CVF_MAX_EXP",
	"EATAN CVF_MIN_EXP",
	"EATAN CVF_NEGONE",
	"EATAN CVF_GARBAGE1",
	"EATAN CVF_GARBAGE2",
	"EATAN CVF_INCREASING",
	"EATAN CVF_DECREASING",
	"EATAN CVF_PI_OVER2",
	"EATAN CVF_PI",
	"EATAN CVF_3PI_OVER2",
	"EATANxy CVF_ZERO",
	"EATANxy CVF_NEGZERO",
	"EATANxy CVF_MAX",
	"EATANxy CVF_MIN",
	"EATANxy CVF_MAX_EXP",
	"EATANxy CVF_MIN_EXP",
	"EATANxy CVF_GARBAGE1",
	"EATANxy CVF_INCREASING",
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
// the polynomial contributes nothing and the result is exactly the pi/4
// constant, 0x3F490FDB -- while the unreduced form gives 0x3FCA1D99, which is
// precisely what the interpreter returned before the fix.
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
	EXPECT_EQ(interp, 0x3F490FDBu)
		<< "[interp] EATAN(1.0) must be the bare pi/4 constant; 0x3FCA1D99 is "
		   "the unreduced form (polynomial evaluated at 1.0 plus pi/4)";
	EXPECT_EQ(jit, 0x3F490FDBu) << "[jit]";
	EXPECT_EQ(one->p, 0x3F490FDAu) << "console, one ULP below both engines";
}

// What passing looks like once the EFU model is right. Also the way to// What passing looks like once the EFU model is right. Also the way to
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
