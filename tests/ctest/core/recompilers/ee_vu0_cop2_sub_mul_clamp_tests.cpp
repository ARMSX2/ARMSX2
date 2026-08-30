// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// COP2 macro-mode MUL operand clamping, and why SUB's row is not carried.
//
// microVU_Upper.inl gives these two families an operand clamp that depends on
// the dest mask rather than on isCOP2:
//
//     mVU_SUB{,i,q,x,y,z,w}   (_XYZW_PS) ? (cFs|cFt) : 0
//     mVU_MUL{,i,q,x,y,z,w}   (_XYZW_PS) ? (cFs|cFt) : cFs
//
// x86 routes the macro ops through mVU and inherits both rows. arm64 hand-rolls
// them in iCOP2-arm64.cpp, where MULx/y/z/w carried the row and VMUL, MULq and
// MULi did not. Without cFs an exponent-0xFF operand reaches the host FMUL as
// an infinity, 0*Inf produces a NaN, and the result clamp (Fminnm/Fmaxnm) folds
// that NaN to +FLT_MAX where the VU returns zero.
//
// The oracle here is the VU0 interpreter, which reads every operand through
// vuDouble(). That makes it the right oracle for MUL, whose clamp moves both
// engines the same way the console does, and the wrong one for SUB: clamping
// displaces an exponent-0xFF operand by a whole binade, and on the console rows
// in autocases_vusat.h that costs more than the NaN it avoids. The two SUB
// tests below are therefore DISABLED rather than deleted -- they say what x86
// parity would assert, and they are what to re-enable if the operands ever get
// held at their real magnitude.

#include "harness/EeRecTestHarness.h"

#include "VU.h"
#include "VUmicro.h"
#include "Config.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;
using namespace vu;

namespace {

// Exponent-0xFF words. As host floats these are Inf and NaN; to the VU they are
// 2^128 scaled by their mantissa.
constexpr u32 kPosInf = 0x7F800000u;
constexpr u32 kNegInf = 0xFF800000u;
constexpr u32 kPosNan = 0x7FC00000u;
constexpr u32 kMaxExpFf = 0x7FFFFFFFu;
constexpr u32 kZero = 0x00000000u;
constexpr u32 kOne = 0x3F800000u;
constexpr u32 kTwo = 0x40000000u;

struct Operands
{
	const char* what;
	u32 fs[4];
	u32 ft[4];
	// Whether the clamp changes this row's answer. Where it does, the
	// interpreter no longer agrees -- see kWhyRangeDiffers. A zero operand
	// leaves nothing for the clamp to change, so those rows still diff plainly
	// and are what says the harness is still comparing anything at all.
	bool rangeDiffers;
};

constexpr const char* kWhyRangeDiffers =
	"the emitters clamp an exp-FF operand to FLT_MAX and the result with it; "
	"the interpreter reads it at full value and saturates at 0x7FFFFFFF, which "
	"is what the console returns. Pinning the clamp row against microVU again "
	"needs a recorded expectation rather than the interpreter";

// The interpreter reads an exp-FF operand at its full value and saturates at
// 0x7FFFFFFF, so it no longer stands in for microVU's clamp row: on any case
// whose result depends on the clamp it now answers something else, and the
// caller says so by passing `whyDiverges`. Where it does not -- a product with
// a zero operand, say -- the plain diff still holds.
void CheckVfCase(const char* opName, u32 code, u32 mask, const Operands& c,
	u32 viReg = 0, u32 viValue = 0)
{
	constexpr u32 kFs = 1, kFt = 2, kFd = 3;
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(kFs, c.fs[0], c.fs[1], c.fs[2], c.fs[3]);
	h.SeedVu0VfBits(kFt, c.ft[0], c.ft[1], c.ft[2], c.ft[3]);
	h.SeedVu0VfBits(kFd, kZero, kZero, kZero, kZero);
	if (viReg != 0)
		h.SeedVu0Vi(viReg, viValue);
	h.LoadProgram({code});
	if (c.rangeDiffers)
		h.RequireVu0Divergence(kWhyRangeDiffers);
	h.Run();

	if (c.rangeDiffers)
		return;

	for (char l : {'x', 'y', 'z', 'w'})
	{
		EXPECT_EQ(h.GetVu0VfBitsJit(kFd, l), h.GetVu0VfBitsInterp(kFd, l))
			<< opName << " (" << c.what << ") dest mask 0x" << std::hex << mask
			<< std::dec << " vf" << kFd << "." << l
			<< ": JIT and interpreter disagree";
	}
}

// Both operands exp-0xFF: the host subtraction is Inf-Inf = NaN, the VU's is a
// difference of two ordinary large numbers. Register-distinct on purpose — the
// fs==ft short circuit in recCOP2_VSUB only covers the same-register case.
const Operands kSubCases[] = {
	{"+inf Fs, +inf Ft",
		{kPosInf, kPosInf, kPosInf, kPosInf},
		{kPosInf, kPosInf, kPosInf, kPosInf}},
	{"largest exp-FF against itself",
		{kMaxExpFf, kMaxExpFf, kMaxExpFf, kMaxExpFf},
		{kMaxExpFf, kMaxExpFf, kMaxExpFf, kMaxExpFf}},
	{"NaN-pattern Fs, in-range Ft",
		{kPosNan, kPosNan, kPosNan, kPosNan},
		{kOne, kOne, kOne, kOne}},
	{"in-range Fs, NaN-pattern Ft",
		{kOne, kOne, kOne, kOne},
		{kPosNan, kPosNan, kPosNan, kPosNan}},
	{"-inf Fs, -inf Ft",
		{kNegInf, kNegInf, kNegInf, kNegInf},
		{kNegInf, kNegInf, kNegInf, kNegInf}},
	{"mixed exp-FF lanes",
		{kPosInf, kNegInf, kPosNan, kMaxExpFf},
		{kMaxExpFf, kPosNan, kNegInf, kPosInf}},
};

// cFs on its own: an exp-0xFF Fs against a zero Ft is 0*Inf = NaN on the host
// and 0 on the VU. cFt needs the full mask, so it gets its own operand set.
const Operands kMulFsCases[] = {
	{"exp-FF Fs, zero Ft",
		{kPosInf, kNegInf, kPosNan, kMaxExpFf},
		{kZero, kZero, kZero, kZero}},
	{"exp-FF Fs, in-range Ft",
		{kPosInf, kNegInf, kPosNan, kMaxExpFf},
		{kTwo, kTwo, kTwo, kTwo}, true},
};

const Operands kMulFtCases[] = {
	{"zero Fs, exp-FF Ft",
		{kZero, kZero, kZero, kZero},
		{kPosInf, kNegInf, kPosNan, kMaxExpFf}},
	{"in-range Fs, exp-FF Ft",
		{kTwo, kTwo, kTwo, kTwo},
		{kPosInf, kNegInf, kPosNan, kMaxExpFf}, true},
};

} // namespace

// Not carried on arm64. mVU_SUB's row would return zero for max - 2^128 where
// the console returns 0x7F7FFFFE and unclamped operands give 0x7F7FFFFF; over
// captures/vusat's 68 rows it gains nothing and loses that row's MAC and status
// columns. Vu0MacroFmacRangeConsole is where that is scored.
TEST(EeVu0Cop2SubMulClamp, DISABLED_SubClampsBothOperandsOnFullMask)
{
	const struct { const char* name; u32 (*enc)(u32, u32, u32, u32); } ops[] = {
		{"VSUB", VSUB_C2}, {"VSUBx", VSUBx_C2}, {"VSUBy", VSUBy_C2},
		{"VSUBz", VSUBz_C2}, {"VSUBw", VSUBw_C2},
	};
	for (const auto& op : ops)
	{
		for (const Operands& c : kSubCases)
		{
			SCOPED_TRACE(::testing::Message() << op.name << " " << c.what);
			CheckVfCase(op.name, op.enc(0xF, /*fd*/3, /*fs*/1, /*ft*/2), 0xF, c);
		}
	}
}

TEST(EeVu0Cop2SubMulClamp, DISABLED_SubQiClampBothOperandsOnFullMask)
{
	const Operands fsOnly[] = {
		{"exp-FF Fs", {kPosInf, kNegInf, kPosNan, kMaxExpFf}, {kZero, kZero, kZero, kZero}},
	};
	for (const Operands& c : fsOnly)
	{
		SCOPED_TRACE(::testing::Message() << "VSUBi " << c.what);
		CheckVfCase("VSUBi", VSUBi_C2(0xF, /*fd*/3, /*fs*/1), 0xF, c, REG_I, kPosInf);
		SCOPED_TRACE(::testing::Message() << "VSUBq " << c.what);
		CheckVfCase("VSUBq", VSUBq_C2(0xF, /*fd*/3, /*fs*/1), 0xF, c, REG_Q, kPosInf);
	}
}

// cFs is unconditional in the table, so every mask is asserted.
TEST(EeVu0Cop2SubMulClamp, MulClampsFsOnEveryMask)
{
	for (const Operands& c : kMulFsCases)
	{
		for (u32 mask = 1; mask <= 0xF; mask++)
		{
			SCOPED_TRACE(::testing::Message()
				<< "VMUL mask=0x" << std::hex << mask << " " << c.what);
			CheckVfCase("VMUL", VMUL_C2(mask, /*fd*/3, /*fs*/1, /*ft*/2), mask, c);
		}
	}
}

TEST(EeVu0Cop2SubMulClamp, MulClampsFtOnFullMask)
{
	for (const Operands& c : kMulFtCases)
	{
		SCOPED_TRACE(::testing::Message() << "VMUL " << c.what);
		CheckVfCase("VMUL", VMUL_C2(0xF, /*fd*/3, /*fs*/1, /*ft*/2), 0xF, c);
	}
}

TEST(EeVu0Cop2SubMulClamp, MulQiClampFsOnEveryMask)
{
	const Operands c = {"exp-FF Fs against an in-range broadcast",
		{kPosInf, kNegInf, kPosNan, kMaxExpFf}, {kZero, kZero, kZero, kZero}};
	for (u32 mask = 1; mask <= 0xF; mask++)
	{
		SCOPED_TRACE(::testing::Message() << "VMULi mask=0x" << std::hex << mask);
		CheckVfCase("VMULi", VMULi_C2(mask, /*fd*/3, /*fs*/1), mask, c, REG_I, kZero);
		SCOPED_TRACE(::testing::Message() << "VMULq mask=0x" << std::hex << mask);
		CheckVfCase("VMULq", VMULq_C2(mask, /*fd*/3, /*fs*/1), mask, c, REG_Q, kZero);
	}
}

// Scope control: the broadcast MUL forms already carry the row, so they must
// agree everywhere the non-broadcast ones are asserted. A harness fault that
// made every case pass would show up here as no signal at all, so these run on
// the same operands as the failing set above.
TEST(EeVu0Cop2SubMulClamp, MulBroadcastFormsAlreadyAgree)
{
	const struct { const char* name; u32 (*enc)(u32, u32, u32, u32); } ops[] = {
		{"VMULx", VMULx_C2}, {"VMULy", VMULy_C2}, {"VMULz", VMULz_C2}, {"VMULw", VMULw_C2},
	};
	for (const auto& op : ops)
	{
		for (const Operands& c : kMulFsCases)
		{
			for (u32 mask = 1; mask <= 0xF; mask++)
			{
				SCOPED_TRACE(::testing::Message()
					<< op.name << " mask=0x" << std::hex << mask << " " << c.what);
				CheckVfCase(op.name, op.enc(mask, /*fd*/3, /*fs*/1, /*ft*/2), mask, c);
			}
		}
	}
}

} // namespace recompiler_tests
