// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU1 EFU's atan series against a hardware sweep of the cancelling band.
//
// vu1_efu_sweep_console_tests.cpp answers where the constant term joins the
// sum. What it cannot answer is which operand leads each of the eight
// coefficient multiplies: on the arguments it sweeps, six of the eight terms
// sit at least two exponents below the running sum, so the guard mask clears
// the bit the two orders differ in and the choice never reaches P.
//
// The band this table is drawn from does reach it. For a scalar EATAN argument
// under 2^-15 the reduced argument sits just above -1, where the coefficients
// are tuned so that pi/4 - sum(c1..c7) is within an ULP or so of -c8: the last
// add is then a cancellation of two near-equal values and the low bits of both
// survive into a result several binades smaller. Over 1966080 arguments the
// eight orders separate on 63357, 574, 285, 82, 41, 56, 15 and 794 rows, and
// every one of them reads the same way -- the power leads the multiply, except
// on the first term, where the coefficient does.
//
// The same band answers one more, which is the order of the multiply that
// steps the power: the square leads it the first time round and trails it
// after. That one is visible in a narrower place still. An operand order only
// reaches the result when the product's tail below the kept ULP is short
// enough for the array's borrow to survive it, which is 0.16% of these
// arguments; the 224 that read on it all fall in that set and all read the
// same way. _vuESIN already steps its power that way, so the two series
// agree once EATAN does.

#include <gtest/gtest.h>

#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "VU.h"

#include "autocases_efu_band.h"

using namespace ps2auto_efu_band;

namespace recompiler_tests
{
namespace
{
using namespace vu;

constexpr u32 kFs = vf::vf1, kFt = vf::vf2;
constexpr u32 kFieldZ = 2;

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

u32 RunInterp(const EfuBandCase& c)
{
	VuTestHarness h(1);
	h.SetVfBits(kFs, c.fs, c.fs, c.fs, c.fs);
	h.SetVfBits(kFt, 0xCCCCCCCCu, 0xCCCCCCCCu, 0xCCCCCCCCu, 0xCCCCCCCCu);
	h.LoadProgram({
		LowerOnly(VEATAN_L(kFs, kFieldZ)),
		LowerOnly(VWAITP_L()),
		LowerOnly(VMFP_L(mask::xyzw, kFt)),
		EBitNopPair(),
	});
	h.RunInterpOnly();
	return h.GetVfBitsInterp(kFt, 'x');
}
} // namespace

TEST(Vu1EfuBandConsole, InterpreterMatchesTheCancellingBand)
{
	int checked = 0, bad = 0;
	for (int i = 0; i < kEfuBandCaseCount; ++i)
	{
		const EfuBandCase& c = kEfuBandCases[i];
		SCOPED_TRACE(::testing::Message() << "EATAN " << c.tag << '[' << c.index
		                                  << "] fs=" << std::hex << c.fs);
		const u32 got = RunInterp(c);
		++checked;
		if (c.bad_interp)
		{
			++bad;
			EXPECT_NE(got, c.p)
				<< "now MATCHES the console. If the EFU model was fixed, clear "
				   "this row's known-bad flag in autocases_efu_band.h.";
		}
		else
		{
			EXPECT_EQ(got, c.p) << "new divergence from the console";
		}
	}
	EXPECT_EQ(checked, kEfuBandCaseCount);
	EXPECT_EQ(bad, kEfuBandBadInterp);
}
} // namespace recompiler_tests
