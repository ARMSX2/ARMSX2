// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// How a block shared between two entry PCs is timed, and what the program
// cache owes on top of that.
//
// microVU finds a block by (startPC, pState). For a block that does not need
// an exact match (everything that does not read a flag instance),
// microBlockManager::search compares only the first eight bytes of the
// 96-byte pipeline state: q, p, flag info, xgkick, the VI backup. The integer
// and vector latencies that follow (VI[], VF[], xgkickcycles) are not part of
// the key, so a second entry that reaches the same startPC with different
// latencies finds the block the first entry compiled and runs its stalls. The
// x86 microVU has the identical search.
//
// The program below has two entries that join at one block. Entry A reaches
// it one cycle after an ILW wrote vi2 (a four-cycle latency), so the shared
// block's first instruction, which reads vi2, stalls one cycle. Entry B
// reaches it with nothing pending. Whichever entry compiles the shared block
// first decides how many cycles both entries charge for it.
//
// That is a property of the lookup, not of the program cache, and it is what
// the 14 Ratchet & Clank: Up Your Arsenal captures `vu1_pc00000080_seq*`
// show when they run with the cache on: the cache hands a capture a program
// holding blocks an earlier capture compiled from another entry, one cycle
// more than a compile from scratch.
//
//  * HydratedProgramTimesLikeTheWarmProgram pins what the cache owes: a
//    hydrated program times exactly like the same program kept in memory.
//  * DISABLED_CyclesDoNotDependOnWhichEntryCompiledFirst states the
//    requirement the lookup does not meet. It fails today; run it with
//    --gtest_also_run_disabled_tests.

#include "harness/VuTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_Persist-arm64.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

constexpr u32 kEntryA = 0x00;
constexpr u32 kEntryB = 0x20;
constexpr u32 kShared = 0x40;
constexpr u32 kBudget = 4096;

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

// Branch displacement in pairs, relative to the pair after the branch.
constexpr s16 BranchTo(u32 from_pc, u32 to_pc)
{
	return static_cast<s16>((static_cast<int>(to_pc) - static_cast<int>(from_pc) - 8) / 8);
}

void LoadJoinProgram(VuTestHarness& h)
{
	h.LoadProgram({
		/* 0x00 entry A */ LowerOnly(VILW_L(mask::x, vi::vi2, vi::vi0, 0)), // vi2 lands 4 cycles on
		/* 0x08 */ LowerOnly(VB_L(BranchTo(0x08, kShared))),
		/* 0x10 */ LowerOnly(0), // delay slot
		/* 0x18 */ LowerOnly(0),
		/* 0x20 entry B */ LowerOnly(0),
		/* 0x28 */ LowerOnly(VB_L(BranchTo(0x28, kShared))),
		/* 0x30 */ LowerOnly(0), // delay slot
		/* 0x38 */ LowerOnly(0),
		/* 0x40 shared */ LowerOnly(VIADDIU_L(vi::vi3, vi::vi2, 5)), // reads vi2
		/* 0x48 */ EBit(LowerOnly(0)), // the harness appends the E-bit delay slot
	});
}

// Dispatch the JIT at `pc` the way the EE would and return the cycles the VU
// charged.
u64 RunAt(u32 pc)
{
	VURegs& vu0 = vuRegs[0];
	vu0.VI[REG_TPC].UL = pc / 8;
	vu0.start_pc = pc;
	CpuMicroVU0.SetStartPC(pc);
	vuRegs[0].VI[REG_VPU_STAT].UL = (vuRegs[0].VI[REG_VPU_STAT].UL & ~0xFFFu) | 0x1u;
	vu0.cycle = 0;
	vu0.ebit = 0;
	vu0.branch = 0;
	vu0.branchpc = 0;
	vu0.delaybranchpc = 0;
	vu0.takedelaybranch = false;
	vu0.flags = 0;
	vu0.fmacreadpos = vu0.fmacwritepos = vu0.fmaccount = 0;
	vu0.ialureadpos = vu0.ialuwritepos = vu0.ialucount = 0;
	std::memset(&vu0.fmac, 0, sizeof(vu0.fmac));
	std::memset(&vu0.ialu, 0, sizeof(vu0.ialu));
	CpuMicroVU0.Execute(kBudget);
	return vu0.cycle;
}

// Drop every compiled block and program, as a block-cache reset does.
void ColdStart()
{
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
}

class MvuBlockSharing : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
		// Recording is on throughout so the compared runs emit the same code
		// forms; the hydration test needs it to have anything to serialize.
		mVUPersist::SetRecordingEnabled(true);
	}

	void TearDown() override
	{
		mVUPersist::SetRecordingEnabled(false);
		RecompilerTestEnvironment::ResetVuBlockCache(0);
	}
};

} // namespace

TEST_F(MvuBlockSharing, HydratedProgramTimesLikeTheWarmProgram)
{
	VuTestHarness h(0);
	LoadJoinProgram(h);
	h.Run(); // JIT-vs-interp correctness pass; also leaves the program in micro memory

	// B on its own: the shared block is compiled for B's state.
	ColdStart();
	u64 mark = mVUPersist::GetBlockCompileCount(0);
	const u64 b_cold = RunAt(kEntryB);
	const u64 b_cold_compiles = mVUPersist::GetBlockCompileCount(0) - mark;

	// A then B in one process: the shared block A compiled serves B.
	ColdStart();
	RunAt(kEntryA);
	std::vector<u8> image;
	ASSERT_TRUE(mVUPersist::TestSerializeNewestProgram(0, image))
		<< "program was not recorded — recorder dropped the episode?";
	const u64 b_warm = RunAt(kEntryB);

	// Precondition: this program does exercise the lookup. B charges more when
	// it runs on the block A compiled. When the lookup is changed to tell the
	// two latency states apart this stops holding: delete this check and the
	// compile-count check below, and enable the DISABLED_ test.
	EXPECT_NE(b_warm, b_cold)
		<< "the join block no longer carries its first entry's stall into the second";

	// A, then a process boundary, then B: the program is hydrated from what A left.
	ColdStart();
	mark = mVUPersist::GetBlockCompileCount(0);
	ASSERT_TRUE(mVUPersist::TestHydrate(0, image.data(), image.size()));
	const u64 b_hydrated = RunAt(kEntryB);
	const u64 b_hydrated_compiles = mVUPersist::GetBlockCompileCount(0) - mark;

	EXPECT_LT(b_hydrated_compiles, b_cold_compiles)
		<< "the shared block must come out of the hydrated program, not be compiled again";
	EXPECT_EQ(b_hydrated, b_warm)
		<< "a hydrated program must time exactly like the same program in memory";
}

TEST_F(MvuBlockSharing, DISABLED_CyclesDoNotDependOnWhichEntryCompiledFirst)
{
	VuTestHarness h(0);
	LoadJoinProgram(h);
	h.Run();

	ColdStart();
	const u64 a_cold = RunAt(kEntryA);
	ColdStart();
	const u64 b_cold = RunAt(kEntryB);

	ColdStart();
	RunAt(kEntryA);
	const u64 b_after_a = RunAt(kEntryB);
	ColdStart();
	RunAt(kEntryB);
	const u64 a_after_b = RunAt(kEntryA);

	EXPECT_EQ(b_after_a, b_cold)
		<< "entry B charged the stall of the block entry A compiled";
	EXPECT_EQ(a_after_b, a_cold)
		<< "entry A lost the stall because entry B compiled the shared block";
}

} // namespace recompiler_tests
