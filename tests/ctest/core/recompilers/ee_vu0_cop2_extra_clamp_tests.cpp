// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// COP2 macro-mode products at VU clamp modes 2 and up (GameDB vuClampMode 2-4).
//
// The VU has no infinities and no NaNs: an exponent-255 word is an ordinary
// number, so zero times it is a zero carrying the XOR of the two signs. The
// host multiply sees a NaN or an infinity there and returns NaN, which the
// result clamp then turns into +FLT_MAX. Clamp mode 2 and up bounds both
// multiplicands of every product before the multiply, so the zero survives.
//
// Another Century's Episode 3 (SLPS-25784, vuClampMode 3) runs VMADDAy with
// Fs = (0, 1, 0, 0) and Ft.y = 0xFFFFFFFF. With no clamp on Ft, every ACC
// lane came back +FLT_MAX, and a few seconds later the game stopped in the EE
// kernel's "DECI2 Panic".
//
// Every lane of every case below is zero times an exponent-255 word, so the
// whole result is exact and the interpreter is the oracle for it and for MAC.
// The ACC addends are ordinary numbers, so the accumulating forms
// return ACC unchanged.

#include "harness/EeRecTestHarness.h"

#include "VU.h"
#include "VUmicro.h"
#include "Config.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFs = 1, kFt = 2, kFd = 3;

// SPECIAL1: funct is the op. SPECIAL2: funct 0x3C-0x3F, the index's upper
// five bits in the fd field.
constexpr u32 Spec1(u32 funct, u32 m) { return COP2_FMAC(m, kFd, kFs, kFt, funct); }
constexpr u32 Spec2(u32 idx, u32 m) { return COP2_FMAC(m, idx >> 2, kFs, kFt, 0x3Cu | (idx & 3u)); }

struct ProductOp
{
	const char* name;
	bool special2;
	u32 index; // SPECIAL1 funct or SPECIAL2 table index
};

const ProductOp kOps[] = {
	// SPECIAL1, result in VF[fd]
	{"MADDx", false, 0x08}, {"MADDy", false, 0x09}, {"MADDz", false, 0x0A}, {"MADDw", false, 0x0B},
	{"MSUBx", false, 0x0C}, {"MSUBy", false, 0x0D}, {"MSUBz", false, 0x0E}, {"MSUBw", false, 0x0F},
	{"MULx", false, 0x18}, {"MULy", false, 0x19}, {"MULz", false, 0x1A}, {"MULw", false, 0x1B},
	{"MULq", false, 0x1C}, {"MULi", false, 0x1E},
	{"MADDq", false, 0x21}, {"MADDi", false, 0x23}, {"MSUBq", false, 0x25}, {"MSUBi", false, 0x27},
	{"MADD", false, 0x29}, {"MUL", false, 0x2A}, {"MSUB", false, 0x2D}, {"OPMSUB", false, 0x2E},
	// SPECIAL2, result in ACC
	{"MADDAx", true, 0x08}, {"MADDAy", true, 0x09}, {"MADDAz", true, 0x0A}, {"MADDAw", true, 0x0B},
	{"MSUBAx", true, 0x0C}, {"MSUBAy", true, 0x0D}, {"MSUBAz", true, 0x0E}, {"MSUBAw", true, 0x0F},
	{"MULAx", true, 0x18}, {"MULAy", true, 0x19}, {"MULAz", true, 0x1A}, {"MULAw", true, 0x1B},
	{"MULAq", true, 0x1C}, {"MULAi", true, 0x1E},
	{"MADDAq", true, 0x21}, {"MADDAi", true, 0x23}, {"MSUBAq", true, 0x25}, {"MSUBAi", true, 0x27},
	{"MADDA", true, 0x29}, {"MULA", true, 0x2A}, {"MSUBA", true, 0x2D}, {"OPMULA", true, 0x2E},
};

// Exponent-255 words of both signs, with and without a mantissa (a NaN and an
// infinity to the host). Every lane of Fs, Ft, Q and I is one of these or a
// zero, never both in the same product.
constexpr u32 kBig[4] = {0x7FFFFFFFu, 0xFFFFFFFFu, 0x7F800000u, 0xFF800001u};
constexpr u32 kZero[4] = {0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u};
constexpr u32 kAcc[4] = {0x3FC00000u, 0xC0200000u, 0x40600000u, 0xC0900000u}; // 1.5 -2.5 3.5 -4.5

void RunCase(const ProductOp& op, int mode, u32 mask, bool bigFt)
{
	const u32* fs = bigFt ? kZero : kBig;
	const u32* ft = bigFt ? kBig : kZero;

	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SetVu0ClampMode(mode);
	// The interpreter's STATUS takes sticky Z and S from the zero product as
	// well as from the result; the recompiler has no product sticky yet
	// (ee_vu0_cop2_guard_mask_tests.cpp). The values and MAC are what this
	// file owns.
	h.IgnoreVu0Vi(REG_STATUS_FLAG);
	h.SeedVu0VfBits(kFs, fs[0], fs[1], fs[2], fs[3]);
	h.SeedVu0VfBits(kFt, ft[0], ft[1], ft[2], ft[3]);
	h.SeedVu0VfBits(kFd, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);
	h.SeedVu0AccBits(kAcc[0], kAcc[1], kAcc[2], kAcc[3]);
	h.SeedVu0Vi(REG_Q, ft[1]);
	h.SeedVu0Vi(REG_I, ft[2]);
	h.LoadProgram({op.special2 ? Spec2(op.index, mask) : Spec1(op.index, mask)});

	SCOPED_TRACE(testing::Message() << op.name << " mode " << mode << " dest 0x" << std::hex << mask
										<< (bigFt ? " (zero Fs, big Ft)" : " (big Fs, zero Ft)"));
	h.Run();
}

} // namespace

TEST(EeVu0Cop2ExtraClamp, ZeroTimesExponent255IsZeroAtModesTwoAndUp)
{
	for (const ProductOp& op : kOps)
		for (int mode : {2, 3, 4})
			for (u32 mask : {0xFu, 0x4u, 0xAu})
				for (bool bigFt : {true, false})
					RunCase(op, mode, mask, bigFt);
}

// The game's own operands, pinned on their own so a failure names it.
TEST(EeVu0Cop2ExtraClamp, Ace3MaddaYKeepsTheAccumulator)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SetVu0ClampMode(3);
	h.SeedVu0VfBits(23, 0x00000000u, 0x3F800000u, 0x00000000u, 0x00000000u);
	h.SeedVu0VfBits(16, 0x00000000u, 0xFFFFFFFFu, 0x00000000u, 0x00000000u);
	h.SeedVu0AccBits(0xC6800000u, 0x80000000u, 0x80000000u, 0x80000000u);
	h.LoadProgram({COP2_FMAC(0xF, 0x09 >> 2, 23, 16, 0x3Cu | (0x09 & 3u))}); // vmaddaxyzw ACC, vf23, vf16y
	h.RunJitNoDiff();

	EXPECT_EQ(h.GetVu0AccBitsJit('x'), 0xC6800000u);
	EXPECT_EQ(h.GetVu0AccBitsJit('z'), 0x80000000u);
	EXPECT_EQ(h.GetVu0AccBitsJit('w'), 0x80000000u);
	// 1.0 * 0xFFFFFFFF: the console keeps a magnitude the host has no binade
	// for, so the bounded operand comes back as -FLT_MAX. The sign is the point.
	EXPECT_EQ(h.GetVu0AccBitsJit('y'), 0xFF7FFFFFu);
}

} // namespace recompiler_tests
