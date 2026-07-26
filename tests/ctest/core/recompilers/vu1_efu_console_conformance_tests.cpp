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
