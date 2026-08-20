// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU1 EFU's series against a hardware sweep.
//
// vu1_efu_console_conformance_tests.cpp scores the family against
// ps2autotests' sixteen constants. Sixteen arguments per op reach only
// fourteen distinct reduced arguments for the whole EATAN family, which is
// far too few to say where the constant term joins the sum or which operand
// leads a multiply -- both of those cost a single ULP, and only in regions
// the constants happen to miss. autocases_efu_sweep.h is a sweep taken to
// answer exactly that: 364 rows whose arguments were chosen so that every
// step before the polynomial is exact.
//
// What the sweep pins that the sixteen constants cannot:
//
//   1. The adder inside the EFU masks guard bits the way the EE's does.
//      On the DYAD rows the reduced argument is 2^-k, so all but the leading
//      term fall away and P - pi/4 is one masked coefficient. Every row from
//      k=9 up comes back on an exact half-ULP of pi/4, which is what a mask
//      of (k-1) bits leaves and what no unmasked add produces.
//   2. The constant term is not the last addend. It goes in one slot earlier,
//      between the second-to-last term and the last. Nothing else reaches
//      P(-1) = 0x33E60000: with the constant last, the final sum is a
//      difference of two exponent-126 singles and so a multiple of 2^-24,
//      and 0x33E60000 is not one, whatever the coefficients are.
//   3. That the operand order matters at all. The EE multiplier is not
//      commutative -- its deficit is a property of ft's mantissa -- so
//      c * x^n and x^n * c differ by a ULP wherever the deficit fires. Which
//      way round each of the eight goes is settled in
//      vu1_efu_band_console_tests.cpp; these arguments separate only the
//      first and the last.
//
// EEXP puts its 1.0 in the same slot, one from the end, and ESIN accumulates
// straight through; both reproduce their whole sweep.

#include <gtest/gtest.h>

#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "VU.h"

#include <string>

#include "autocases_efu_sweep.h"

using namespace ps2auto_efu_sweep;

namespace recompiler_tests
{
namespace
{
using namespace vu;

constexpr u32 kFs = vf::vf1, kFt = vf::vf2;
constexpr u32 kFieldZ = 2;

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

u32 Encode(const EfuSweepCase& c)
{
	const std::string op = c.op;
	if (op == "EATANxy") return VEATANXY_L(kFs);
	if (op == "EEXP") return VEEXP_L(kFs, kFieldZ);
	if (op == "ESIN") return VESIN_L(kFs, kFieldZ);
	return 0;
}

u32 RunInterp(const EfuSweepCase& c, u32 word)
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
	h.RunInterpOnly();
	return h.GetVfBitsInterp(kFt, 'x');
}
} // namespace

TEST(Vu1EfuSweepConsole, InterpreterMatchesTheSweep)
{
	int checked = 0, bad = 0;
	for (int i = 0; i < kEfuSweepCaseCount; ++i)
	{
		const EfuSweepCase& c = kEfuSweepCases[i];
		const u32 word = Encode(c);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;

		SCOPED_TRACE(::testing::Message()
		             << c.op << ' ' << c.tag << '[' << c.index << ']');
		const u32 got = RunInterp(c, word);
		++checked;
		if (c.bad_interp)
		{
			++bad;
			EXPECT_NE(got, c.p)
				<< "now MATCHES the console. If the EFU model was fixed, clear "
				   "this row's known-bad flag in autocases_efu_sweep.h.";
		}
		else
		{
			EXPECT_EQ(got, c.p) << "new divergence from the console";
		}
	}
	EXPECT_EQ(checked, kEfuSweepCaseCount);
	EXPECT_EQ(bad, kEfuSweepBadInterp);
}
} // namespace recompiler_tests
