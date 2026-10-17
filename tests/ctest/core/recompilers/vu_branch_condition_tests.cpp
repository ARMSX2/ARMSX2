// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// IBEQ / IBNE compare two VIs, which are 16 bits wide.
//
// microVU keeps a VI in a 32-bit host register, and an add that carries out
// of bit 15 leaves the carry in bit 16 of that register until the value is
// stored with STRH. So two VIs can be equal while the registers holding them
// differ above bit 15, and the branch has to decide on the low halfword. The
// cases below build exactly that: IADDIU carries a VI from 0xFFFF to 0, or
// from 0x0004 to 0x0005, in its register, and the very next op branches on
// it, one pair later, against vi00 (in either operand position) or against
// another VI that is zero. vi00 is the hardwired zero.
//
// Each case runs both engines (Run() diffs them) and checks which side of
// the branch ran.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp LoadViImm(u32 dst, u32 imm) { return LowerOnly(VIADDIU_L(dst, vi::vi0, imm)); }
inline VuOp Nop() { return IBit(VuOp{VLitZero(), VNOP_U()}); }

enum class Against
{
	Vi0AsIt, // IBxx vi1 vs vi00, vi00 in the It field
	Vi0AsIs, // vi00 in the Is field
	ZeroVi,  // vi1 vs vi2, vi2 == 0
};

// vi1 starts at `start`, IADDIU adds 1 in its register, then the branch.
// Taken lands on pair 7 (vi6 = 0x444), not taken falls to pair 4 (vi5 = 0x333).
void RunCase(int vu, bool isEq, Against against, u32 start, bool expectTaken)
{
	VuTestHarness h(vu);
	h.SetVi(vi::vi1, start);
	h.SetVi(vi::vi2, 0);

	u32 it = vi::vi0, is = vi::vi1;
	if (against == Against::Vi0AsIs)
	{
		it = vi::vi1;
		is = vi::vi0;
	}
	else if (against == Against::ZeroVi)
	{
		it = vi::vi2;
	}
	const s16 toPair7 = 4; // from pair 2: target = 2 + 1 + 4
	const u32 branch = isEq ? VIBEQ_L(it, is, toPair7) : VIBNE_L(it, is, toPair7);

	h.LoadProgram({
		LowerOnly(VIADDIU_L(vi::vi1, vi::vi1, 1)), // pair 0
		Nop(),                                     // pair 1: a branch right after would read vi1's old value
		LowerOnly(branch),                         // pair 2
		Nop(),                                     // pair 3: delay slot
		LoadViImm(vi::vi5, 0x333),                 // pair 4: not taken
		EBitNopPair(),                             // pair 5
		Nop(),                                     // pair 6
		LoadViImm(vi::vi6, 0x444),                 // pair 7: taken
		EBitNopPair(),                             // pair 8
	});
	h.Run();

	const char* what = isEq ? "IBEQ" : "IBNE";
	EXPECT_EQ(h.GetViJit(vi::vi1), (start + 1) & 0xFFFFu) << what;
	EXPECT_EQ(h.GetViJit(vi::vi6), expectTaken ? 0x444u : 0u)
		<< "vu" << vu << " " << what << " against " << static_cast<int>(against) << " start " << start;
	EXPECT_EQ(h.GetViJit(vi::vi5), expectTaken ? 0u : 0x333u)
		<< "vu" << vu << " " << what << " against " << static_cast<int>(against) << " start " << start;
}

} // namespace

TEST(VuBranchCondition, EqualityIsDecidedOnTheLowHalfword)
{
	for (int vu = 0; vu < 2; vu++)
	{
		for (Against against : {Against::Vi0AsIt, Against::Vi0AsIs, Against::ZeroVi})
		{
			// 0xFFFF + 1 carries out: the VI is zero, its register is not.
			RunCase(vu, true, against, 0xFFFF, true);
			RunCase(vu, false, against, 0xFFFF, false);
			// 4 + 1: nonzero in both.
			RunCase(vu, true, against, 0x0004, false);
			RunCase(vu, false, against, 0x0004, true);
		}
	}
}

} // namespace recompiler_tests
