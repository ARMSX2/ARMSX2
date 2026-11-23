// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// microVU's per-instruction IR records (mVU.prog.IRinfo.info) are indexed by
// micro-memory address and are not cleared between compiles: a compile fills
// the records of the instructions it compiles and leaves every other slot as
// the last compile at that address left it. Code that reads a slot this
// compile did not fill reads compile history, so the same block at the same
// address can compile differently depending on what was compiled there before.
//
// Each test here compiles one block twice, after two different earlier
// programs, and requires the same outcome both times.

#include "harness/VuTestHarness.h"

#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_Persist-arm64.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

// Compiles a program whose pair 1 is a branch, so the IR slot of pair 1 holds
// a branch record afterwards; or, with `branch` false, the same program with a
// NOP there.
void CompileHistory(VuTestHarness& h, bool branch)
{
	h.SetStartPc(0);
	h.LoadProgram({
		NopPair(),                                // pair 0
		branch ? LowerOnly(VB_L(+2)) : NopPair(), // pair 1: B to pair 4, or a NOP
		NopPair(),                                // pair 2: delay slot
		NopPair(),                                // pair 3
		EBitNopPair(),                            // pair 4
	});
	h.Run();
}

// Runs a program whose entry block, at pair 2, opens with B to the pair after
// its own delay slot. Outside a delay slot that branch is a NOP, and setBranchA
// folds it into the block. Returns the number of blocks the run compiled.
u64 CompileBranchNopBlock(VuTestHarness& h)
{
	h.SetStartPc(2 * 8);
	h.LoadProgram({
		NopPair(),                                   // pair 0
		NopPair(),                                   // pair 1
		LowerOnly(VB_L(+1)),                         // pair 2: entry; B to pair 4
		LowerOnly(VIADDIU_L(vi::vi1, vi::vi0, 0x11)), // pair 3: delay slot
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi0, 0x22)), // pair 4
		EBitNopPair(),                               // pair 5
	});
	const u64 before = mVUPersist::GetBlockCompileCount(h.VuIndex());
	h.Run();
	EXPECT_EQ(h.GetViJit(vi::vi1), 0x11u);
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x22u);
	return mVUPersist::GetBlockCompileCount(h.VuIndex()) - before;
}

} // namespace

// setBranchA asks whether the previous pair is a branch, to tell a branch in a
// delay slot from one that is not. For a block's first instruction the previous
// pair is not in the block: its record is whatever an earlier compile left.
// After a program that had a branch there, the B was compiled as a real branch
// and the block split in two.
TEST(MvuCompileHistory, BranchNopFoldDoesNotDependOnThePreviousSlot)
{
	VuTestHarness h(1);

	CompileHistory(h, /*branch=*/false);
	const u64 afterNop = CompileBranchNopBlock(h);

	CompileHistory(h, /*branch=*/true);
	const u64 afterBranch = CompileBranchNopBlock(h);

	EXPECT_EQ(afterNop, 1u) << "the B is a NOP here and folds into one block";
	EXPECT_EQ(afterBranch, afterNop)
		<< "the block compiled differently because an earlier program had a branch "
		   "at the address before its first instruction";
}

} // namespace recompiler_tests
