// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// COP2 macro-mode MADD/MSUB operand clamping: fd = ACC +/- VF[fs] * VF[ft].
//
// An exponent-0xFF word is an ordinary large number to the VU, so operands are
// clamped to +/-FLT_MAX before a host FMUL/FADD sees them. microVU_Upper-arm64.inl
// sets the clamp per op, and two rows differ under isCOP2:
//
//     mVU_MADDw   (isCOP2) ? (cACC|cFt|cFs) : cFs
//     mVU_MSUB    (isCOP2) ? cFs            : 0
//
// arm64 hand-rolls these ops rather than going through mVU, so those rows are
// retyped in iCOP2-arm64.cpp and were missing. The oracle is the VU0
// interpreter, which reads every operand through vuDouble(); rows the table
// gives no clamp (MADD, MSUBx/y/z/w) diverge by design and are not asserted.

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
// 2^128 scaled by their mantissa, and 0x7FFFFFFF is simply the largest value a
// VU register can hold.
constexpr u32 kPosInf = 0x7F800000u;
constexpr u32 kNegInf = 0xFF800000u;
constexpr u32 kPosNan = 0x7FC00000u;
constexpr u32 kMaxExpFf = 0x7FFFFFFFu;
constexpr u32 kZero = 0x00000000u;
constexpr u32 kOne = 0x3F800000u;

struct Case
{
	const char* what;
	u32 fs[4];
	u32 ft[4];
	u32 acc[4];
	// Whether the clamp changes this row's answer. Where it does, the
	// interpreter no longer agrees -- it reads an exp-FF operand at full value
	// and saturates at 0x7FFFFFFF, which is what the console returns -- so the
	// row records the gap instead. The rows the clamp cannot reach are what
	// says the diff is still comparing anything.
	bool rangeDiffers;
};

constexpr const char* kWhyRangeDiffers =
	"the emitters clamp an exp-FF operand to FLT_MAX and the result with it; "
	"the interpreter reads it at full value and saturates at 0x7FFFFFFF. "
	"Pinning the clamp row against microVU again needs a recorded expectation "
	"rather than the interpreter";

// A zero Fs lane against an exp-0xFF broadcast: 0 * Inf = NaN on the host,
// 0 * 2^128 = 0 on the VU. It is the shape a homogeneous transform produces.
const Case kCases[] = {
	{"zero Fs, exp-FF Ft",
		{kZero, kZero, kZero, kZero},
		{kOne, kOne, kOne, kPosInf},
		{kOne, kOne, kOne, kOne}},
	{"zero Fs, NaN-pattern Ft",
		{kZero, kZero, kZero, kZero},
		{kOne, kOne, kOne, kPosNan},
		{kOne, kOne, kOne, kOne}},
	{"zero Fs, largest exp-FF Ft",
		{kZero, kZero, kZero, kZero},
		{kOne, kOne, kOne, kMaxExpFf},
		{kOne, kOne, kOne, kOne}},
	{"zero Fs, -inf Ft, negative ACC",
		{kZero, kZero, kZero, kZero},
		{kOne, kOne, kOne, kNegInf},
		{0xBF800000u, 0xBF800000u, 0xBF800000u, 0xBF800000u}},
	{"exp-FF Ft and exp-FF ACC",
		{kOne, kOne, kOne, kOne},
		{kPosInf, kPosInf, kPosInf, kPosInf},
		{kNegInf, kNegInf, kNegInf, kNegInf}},
	// cACC on its own. The product overflows to a host infinity and a raw exp-FF
	// ACC is the opposite one, so the add gives NaN. An in-range product does not
	// separate the two — the result clamp folds both answers to the same word.
	{"exp-FF ACC cancelling an overflowed product",
		{0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u},
		{kOne, kOne, kOne, 0x7F000000u},
		{kPosInf, kPosInf, kPosInf, kPosInf}, true},
};

void CheckVfCase(const char* opName, u32 code, u32 fdReg, u32 mask, const Case& c)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(1, c.fs[0], c.fs[1], c.fs[2], c.fs[3]);
	h.SeedVu0VfBits(2, c.ft[0], c.ft[1], c.ft[2], c.ft[3]);
	h.SeedVu0VfBits(fdReg, kZero, kZero, kZero, kZero);
	h.SeedVu0AccBits(c.acc[0], c.acc[1], c.acc[2], c.acc[3]);
	// A multiply-accumulate's product stage raises flags of its own, and the
	// interpreter now carries them into the sticky field where the emitters do
	// not. That is pinned against the console in vu_sticky_console_conformance
	// and vu0_macro_fmac_range_console; these tests pin the operand clamp's
	// VALUE.
	h.IgnoreVu0Vi(REG_STATUS_FLAG);
	h.IgnoreVu0Vi(REG_MAC_FLAG);
	h.LoadProgram({code});
	if (c.rangeDiffers)
		h.RequireVu0Divergence(kWhyRangeDiffers);
	h.Run();

	if (c.rangeDiffers)
		return;

	for (char l : {'x', 'y', 'z', 'w'})
	{
		EXPECT_EQ(h.GetVu0VfBitsJit(fdReg, l), h.GetVu0VfBitsInterp(fdReg, l))
			<< opName << " (" << c.what << ") dest mask 0x" << std::hex << mask
			<< std::dec << " vf" << fdReg << "." << l
			<< ": JIT and interpreter disagree";
	}
}

} // namespace

// One case per half of MADDw's set, so dropping either cFt or cACC fails here.
TEST(EeVu0Cop2MaddClamp, MaddwClampsFtAndAccLikeInterp)
{
	for (const Case& c : kCases)
	{
		for (u32 mask = 1; mask <= 0xF; mask++)
		{
			SCOPED_TRACE(::testing::Message()
				<< "VMADDw mask=0x" << std::hex << mask << " " << c.what);
			CheckVfCase("VMADDw", VMADDw_C2(mask, /*fd*/3, /*fs*/1, /*ft*/2), 3, mask, c);
		}
	}
}

// Scope control: the sweep above is exotic enough that a harness fault would
// read as a fix. Broadcast lanes stay in range — MADDx/y/z have no cFt, so an
// exp-FF lane through them is a by-design divergence and is not assertable.
TEST(EeVu0Cop2MaddClamp, MaddxyzKeepTheirOwnClampSet)
{
	const struct { const char* name; u32 (*enc)(u32, u32, u32, u32); } ops[] = {
		{"VMADDx", VMADDx_C2}, {"VMADDy", VMADDy_C2}, {"VMADDz", VMADDz_C2},
	};
	const Case c = {"in-range operands",
		{kOne, kOne, kOne, kOne},
		{kOne, kOne, kOne, kPosInf},
		{kOne, kOne, kOne, kOne}};
	for (const auto& op : ops)
	{
		for (u32 mask = 1; mask <= 0xF; mask++)
		{
			SCOPED_TRACE(::testing::Message()
				<< op.name << " mask=0x" << std::hex << mask);
			CheckVfCase(op.name, op.enc(mask, /*fd*/3, /*fs*/1, /*ft*/2), 3, mask, c);
		}
	}
}

// The table's other isCOP2 row. MSUBx/y/z/w keep clampType 0 and are not
// asserted.
TEST(EeVu0Cop2MaddClamp, MsubClampsFsLikeInterp)
{
	const Case fsCases[] = {
		{"exp-FF Fs against a zero Ft",
			{kPosInf, kNegInf, kPosNan, kMaxExpFf},
			{kZero, kZero, kZero, kZero},
			{kOne, kOne, kOne, kOne}},
		{"exp-FF Fs against a one Ft",
			{kPosInf, kNegInf, kPosNan, kMaxExpFf},
			{kOne, kOne, kOne, kOne},
			{kZero, kZero, kZero, kZero}, true},
	};
	for (const Case& c : fsCases)
	{
		for (u32 mask = 1; mask <= 0xF; mask++)
		{
			SCOPED_TRACE(::testing::Message()
				<< "VMSUB mask=0x" << std::hex << mask << " " << c.what);
			CheckVfCase("VMSUB", VMSUB_C2(mask, /*fd*/3, /*fs*/1, /*ft*/2), 3, mask, c);
		}
	}
}

} // namespace recompiler_tests
