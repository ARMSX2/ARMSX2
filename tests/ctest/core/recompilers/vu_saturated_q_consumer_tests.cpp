// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// DIV by zero, then a multiply by Q. A ceiling the FMAC cannot read comes back
// from the multiply as +FLT_MAX with the sign gone; Sly 3 drew the protagonist
// white on that. Asserted on the product, not on Q, so the rows outlive a
// change of ceiling. VuDivUnitConsole scores which ceiling each mode reaches.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "VU.h"

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee;

constexpr u32 kFs = 1; // dividend
constexpr u32 kFt = 2; // divisor, zero
constexpr u32 kFm = 3; // multiplicand
constexpr u32 kFd = 4; // product

constexpr u32 kNegOne = 0xBF800000u; // -1.0
constexpr u32 kZero   = 0x00000000u;
constexpr u32 kTiny   = 0x0D800000u; // 2^-100

constexpr u32 kMacroLanes = 0xF;            // COP2 takes the dest field unshifted
constexpr u32 kMicroLanes = vu::mask::xyzw; // the micro encoder takes it in place

// 2^-100 against either candidate ceiling lands in 2^27..2^29, so the exponent
// bound clears both edges and does not say which ceiling ran.
void ExpectProductKeptTheOperand(u32 bits, const char* who)
{
	const u32 exp = (bits >> 23) & 0xFFu;
	EXPECT_EQ(bits & 0x80000000u, 0x80000000u)
		<< who << ": quotient's sign lost (0x" << std::hex << bits << ")";
	EXPECT_GT(exp, 0x80u) << who << ": product underflowed (0x" << std::hex << bits << ")";
	EXPECT_LT(exp, 0xA0u)
		<< who << ": product saturated, so the multiply read Q as a NaN (0x"
		<< std::hex << bits << ")";
}

void RunMicro(VuTestHarness& h, u32 upper)
{
	h.SetVfBits(kFs, kNegOne, kNegOne, kNegOne, kNegOne);
	h.SetVfBits(kFt, kZero, kZero, kZero, kZero);
	h.SetVfBits(kFm, kTiny, kTiny, kTiny, kTiny);
	h.LoadProgram({
		vu::VuOp{vu::VDIV_L(kFs, 0, kFt, 0), vu::VNOP_U()},
		vu::VuOp{vu::VWAITQ_L(), vu::VNOP_U()},
		vu::IBit(vu::VuOp{vu::VLitZero(), upper}),
		vu::EBitNopPair(),
	});
	h.RunNoDiff(); // the ceilings differ below mode 3, the claims do not
}

void BuildMacro(EeRecTestHarness& h, u32 op)
{
	h.EnableVu0Capture();
	h.ExpectVu0Divergence(); // as RunMicro
	h.SeedVu0VfBits(kFs, kNegOne, kNegOne, kNegOne, kNegOne);
	h.SeedVu0VfBits(kFt, kZero, kZero, kZero, kZero);
	h.SeedVu0VfBits(kFm, kTiny, kTiny, kTiny, kTiny);
	h.SeedVu0AccBits(kZero, kZero, kZero, kZero); // so MADDq's sum is its product
	h.LoadProgram({VDIV_C2(0, 0, kFs, kFt), VWAITQ_C2(), op});
}
} // namespace

TEST(VuSaturatedQConsumer, MicroMultiplyKeepsTheSaturatedQuotient)
{
	for (int mode = 1; mode <= 4; ++mode)
	{
		SCOPED_TRACE(::testing::Message() << "vuClampMode " << mode);

		VuTestHarness hm(0);
		hm.SetVuClampMode(mode);
		RunMicro(hm, vu::VMULq_U(kMicroLanes, kFd, kFm));
		ExpectProductKeptTheOperand(hm.GetVfBitsJit(kFd, 'x'), "micro jit MULq");
		ExpectProductKeptTheOperand(hm.GetVfBitsInterp(kFd, 'x'), "micro interp MULq");

		VuTestHarness ha(0);
		ha.SetVuClampMode(mode);
		RunMicro(ha, vu::VMADDq_U(kMicroLanes, kFd, kFm));
		ExpectProductKeptTheOperand(ha.GetVfBitsJit(kFd, 'x'), "micro jit MADDq");
		ExpectProductKeptTheOperand(ha.GetVfBitsInterp(kFd, 'x'), "micro interp MADDq");
	}
}

// VMADDq, not VMULq: MULq's Q goes through cop2ClampOperandInto, so the q-forms
// that reach the arithmetic unbounded are MADDq, MSUBq, MADDAq, MSUBAq, ADDAq
// and SUBAq. Mode 4 is the test below.
TEST(VuSaturatedQConsumer, MacroAccumulateKeepsTheSaturatedQuotient)
{
	for (int mode = 1; mode <= 3; ++mode)
	{
		SCOPED_TRACE(::testing::Message() << "vu0ClampMode " << mode);

		EeRecTestHarness h;
		h.SetVu0ClampMode(mode);
		BuildMacro(h, VMADDq_C2(kMacroLanes, kFd, kFm));
		h.Run();

		ExpectProductKeptTheOperand(h.GetVu0VfBitsJit(kFd, 'x'), "macro jit");
		ExpectProductKeptTheOperand(h.GetVu0VfBitsInterp(kFd, 'x'), "macro interp");
	}
}

// The macro accumulate has no ceiling model of its own and, at mode 4, no
// operand clamp either. microVU's MADDq is held at the same mode above.
TEST(VuSaturatedQConsumer, MacroAccumulateCannotHoldTheCeilingAtClampModeFour)
{
	EeRecTestHarness h;
	h.SetVu0ClampMode(4);
	BuildMacro(h, VMADDq_C2(kMacroLanes, kFd, kFm));
	h.Run();

	EXPECT_EQ(h.GetVu0VfBitsJit(kFd, 'x'), 0x7F7FFFFFu)
		<< "the macro accumulate now holds the console ceiling";
	ExpectProductKeptTheOperand(h.GetVu0VfBitsInterp(kFd, 'x'), "macro interp");
}
} // namespace recompiler_tests
