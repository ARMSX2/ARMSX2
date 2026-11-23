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

// A T bit on an unconditional branch ends the program at the branch target:
// normBranch moves the IR cursor to the target and mVUDTendProgram emits the
// end there. The end finalises a pending divide's status flags when the slot
// it reads says one is due. That slot is the target's, which this compile did
// not fill; an earlier program's DIV left one marked due there.
TEST(MvuCompileHistory, TBitBranchEndDoesNotReadTheTargetSlot)
{
	VuTestHarness h(0);
	const u32 savedFbrst = vuRegs[0].VI[REG_FBRST].UL;

	for (u32 target = 2; target <= 5; target++)
	{
		SCOPED_TRACE(target);

		// DIV 1/0 at pair 0: Q is due a few pairs later, so the compile marks a
		// pair in the loop's range with the divide flag, and mVU.divFlag holds
		// D. Only the compile matters here (the engines' zero-divisor Q differs
		// at this clamp mode), so the run is not diffed.
		h.SetStartPc(0);
		h.LoadProgram({
			LowerOnly(VDIV_L(vf::vf0, 3, vf::vf0, 0)), // pair 0: Q = vf0.w / vf0.x
			NopPair(), NopPair(), NopPair(), NopPair(), NopPair(),
			EBitNopPair(),
		});
		h.RunNoDiff();

		// B with the T bit to `target`; T stops the program there. No divide
		// runs, so the status flags stay as seeded.
		std::vector<VuOp> p;
		p.push_back(TBit(LowerOnly(VB_L(static_cast<s16>(target - 1))))); // pair 0
		p.push_back(NopPair());                                           // pair 1: delay slot
		for (u32 i = 2; i < 6; i++)
			p.push_back(NopPair());
		p.push_back(EBitNopPair());
		h.LoadProgram(std::move(p));
		h.SetVi(REG_STATUS_FLAG, 0);
		vuRegs[0].VI[REG_FBRST].UL = 0x8u; // T-stop for VU0

		// The T bit on a branch orders the delay slot differently in the two
		// engines, so each runs alone and only the flags are compared.
		h.RunInterpOnly();
		h.RunJitPreserveBlockCache();
		vuRegs[0].VI[REG_FBRST].UL = savedFbrst;

		EXPECT_EQ(h.GetViInterp(REG_STATUS_FLAG) & 0x30u, 0u);
		EXPECT_EQ(h.GetViJit(REG_STATUS_FLAG) & 0x30u, 0u)
			<< "the T-bit end set D/I from a divide an earlier program ran";
	}
}

} // namespace recompiler_tests
