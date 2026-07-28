// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// End-of-program and mid-program breaks that land ON a branch or jump.
//
// The behavioural model:
//
// A VU microprogram ends when a pair carries the E bit, one delay-slot pair
// later. A branch also has a delay slot. When the E bit sits on the branch
// pair itself the two coincide: the branch pair runs, its delay slot runs, and
// then the program stops — WITHOUT executing the branch target. What the
// branch still decides is the resume point: VI[REG_TPC], which is where the
// next dispatch of this VU picks up.
//
// So the whole observable of an E-bit branch is "which PC did we park at",
// and getting it wrong is not a crash — it is a microprogram that silently
// resumes in the wrong place the next time the EE kicks the VU. microVU
// handles each branch shape with its own hand-written exit stub
// (microVU_Branch-arm64.inl: normBranch, condBranch, normJump), each doing
// its own incPC arithmetic to name the parked PC, and none of the three had
// any test coverage.
//
// The M bit is the same shape with a different terminator: VU0 only, it breaks
// out to the EE for a sync but leaves the program running, so TPC is a genuine
// resume point rather than a final resting place.
//
// Every case asserts the parked TPC as an absolute pair index and pins which
// side of the branch actually executed, since a stub that picks the wrong
// successor still parks at a legal-looking PC.

#include "harness/VuTestHarness.h"

#include "VU.h"
#include "Hw.h"     // INTC_STAT
#include "Dmac.h"   // INTC_VU0
#include "Memory.h" // psHu32

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp LoadViImm(u32 dst, u32 imm) { return LowerOnly(VIADDIU_L(dst, vi::vi0, imm)); }
inline VuOp Nop() { return IBit(VuOp{VLitZero(), VNOP_U()}); }

} // namespace

// =========================================================================
//  E-bit on an unconditional branch — normBranch's eBit exit
// =========================================================================

TEST(VuBranchTerminator, EBitOnUnconditionalBranchParksAtTheBranchTarget)
{
	VuTestHarness h(0);
	h.LoadProgram({
		LoadViImm(vi::vi1, 0x111),          // pair 0
		EBit(LowerOnly(VB_L(+2))),          // pair 1: branch to pair 4, ends here
		LoadViImm(vi::vi2, 0x222),          // pair 2: delay slot — runs
		LoadViImm(vi::vi3, 0x333),          // pair 3: skipped
		LoadViImm(vi::vi4, 0x444),          // pair 4: branch target — NOT executed
		EBitNopPair(),                      // pair 5
	});
	h.Run();

	EXPECT_TRUE(h.HasTerminated());
	EXPECT_EQ(h.GetViJit(vi::vi1), 0x111u);
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x222u) << "the branch delay slot must still run";
	EXPECT_EQ(h.GetViJit(vi::vi3), 0u);
	EXPECT_EQ(h.GetViJit(vi::vi4), 0u)
		<< "an E-bit branch parks at its target, it does not execute it";
	EXPECT_EQ(h.GetViJit(REG_TPC), 4u) << "resume PC must be the branch target";
}

TEST(VuBranchTerminator, EBitOnBackwardUnconditionalBranchParksAtTheBranchTarget)
{
	// Backward target: the same stub with a negative displacement. A stub that
	// computed the parked PC from the fall-through instead of from branchAddr
	// would still look right on the forward case above.
	VuTestHarness h(0);
	h.LoadProgram({
		LoadViImm(vi::vi1, 0x111),          // pair 0 — the branch target
		LoadViImm(vi::vi2, 0x222),          // pair 1
		EBit(LowerOnly(VB_L(-3))),          // pair 2: branch back to pair 0
		LoadViImm(vi::vi3, 0x333),          // pair 3: delay slot — runs
		EBitNopPair(),                      // pair 4
	});
	h.Run();

	EXPECT_TRUE(h.HasTerminated());
	EXPECT_EQ(h.GetViJit(vi::vi3), 0x333u);
	EXPECT_EQ(h.GetViJit(REG_TPC), 0u);
}

// =========================================================================
//  E-bit on a conditional branch — condBranch's eBit exit. Both successors
//  are named by the same stub, so both polarities need pinning.
// =========================================================================

TEST(VuBranchTerminator, EBitOnConditionalBranchTakenParksAtTheTarget)
{
	VuTestHarness h(0);
	h.SetVi(vi::vi1, 0); // IBEQ vi1, vi0 => equal => taken
	h.LoadProgram({
		EBit(LowerOnly(VIBEQ_L(vi::vi1, vi::vi0, +2))), // pair 0: target pair 3
		LoadViImm(vi::vi2, 0x222),                      // pair 1: delay slot
		LoadViImm(vi::vi3, 0x333),                      // pair 2: fall-through side
		LoadViImm(vi::vi4, 0x444),                      // pair 3: taken target
		EBitNopPair(),                                  // pair 4
	});
	h.Run();

	EXPECT_TRUE(h.HasTerminated());
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x222u);
	EXPECT_EQ(h.GetViJit(vi::vi3), 0u);
	EXPECT_EQ(h.GetViJit(vi::vi4), 0u);
	EXPECT_EQ(h.GetViJit(REG_TPC), 3u) << "taken E-bit branch parks at the target";
}

TEST(VuBranchTerminator, EBitOnConditionalBranchNotTakenParksAfterTheDelaySlot)
{
	VuTestHarness h(0);
	h.SetVi(vi::vi1, 1); // IBEQ vi1, vi0 => not equal => not taken
	h.LoadProgram({
		EBit(LowerOnly(VIBEQ_L(vi::vi1, vi::vi0, +2))), // pair 0: target pair 3
		LoadViImm(vi::vi2, 0x222),                      // pair 1: delay slot
		LoadViImm(vi::vi3, 0x333),                      // pair 2: fall-through side
		LoadViImm(vi::vi4, 0x444),                      // pair 3: taken target
		EBitNopPair(),                                  // pair 4
	});
	h.Run();

	EXPECT_TRUE(h.HasTerminated());
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x222u);
	EXPECT_EQ(h.GetViJit(vi::vi3), 0u);
	EXPECT_EQ(h.GetViJit(vi::vi4), 0u);
	EXPECT_EQ(h.GetViJit(REG_TPC), 2u)
		<< "not-taken E-bit branch parks at the pair after the delay slot, "
		   "never at the branch target";
}

// =========================================================================
//  E-bit on an indirect jump — normJump's eBit exit. The parked PC comes
//  from a register read at runtime rather than from a compile-time address,
//  so it is a distinct stub from the two above.
// =========================================================================

TEST(VuBranchTerminator, EBitOnJumpParksAtTheRegisterTarget)
{
	VuTestHarness h(0);
	h.SetVi(vi::vi1, 4); // JR target = pair 4
	h.LoadProgram({
		EBit(LowerOnly(VJR_L(vi::vi1))), // pair 0
		LoadViImm(vi::vi2, 0x222),       // pair 1: delay slot
		LoadViImm(vi::vi3, 0x333),       // pair 2: skipped
		LoadViImm(vi::vi5, 0x555),       // pair 3: skipped
		LoadViImm(vi::vi4, 0x444),       // pair 4: jump target — NOT executed
		EBitNopPair(),                   // pair 5
	});
	h.Run();

	EXPECT_TRUE(h.HasTerminated());
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x222u);
	EXPECT_EQ(h.GetViJit(vi::vi3), 0u);
	EXPECT_EQ(h.GetViJit(vi::vi4), 0u);
	EXPECT_EQ(h.GetViJit(REG_TPC), 4u);
}

// =========================================================================
//  Plain indirect jump on VU1. VU0 and VU1 get separate template
//  instantiations of the runtime jump-compile entry point, and only VU0's
//  had ever been called.
// =========================================================================

TEST(VuBranchTerminator, IndirectJumpOnVu1ReachesTheRegisterTarget)
{
	VuTestHarness h(1);
	h.SetVi(vi::vi1, 4); // JR target = pair 4
	h.LoadProgram({
		LowerOnly(VJR_L(vi::vi1)),  // pair 0
		LoadViImm(vi::vi2, 0x222),  // pair 1: delay slot
		LoadViImm(vi::vi3, 0x333),  // pair 2: skipped
		LoadViImm(vi::vi5, 0x555),  // pair 3: skipped
		LoadViImm(vi::vi4, 0x444),  // pair 4: jump target — executes
		EBitNopPair(),              // pair 5
	});
	h.Run();

	EXPECT_TRUE(h.HasTerminated());
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x222u);
	EXPECT_EQ(h.GetViJit(vi::vi3), 0u);
	EXPECT_EQ(h.GetViJit(vi::vi5), 0u);
	EXPECT_EQ(h.GetViJit(vi::vi4), 0x444u) << "the jump must land on its target";
}

// =========================================================================
//  T-bit on an indirect jump — normJump's T-bit stub. Same contract as the
//  branch-side T-bit handler (Vu0SpecialBits.TBitOnUnconditionalBranch...):
//  it must set VURegs::flags.INTC so the dispatcher epilogue raises the VU0
//  interrupt. The JIT is run one-sided, since the interpreter raises INTC
//  inline and would mask a missing raise on the JIT side.
// =========================================================================

TEST(VuBranchTerminator, TBitOnJumpRaisesVu0Intc)
{
	VuTestHarness h(0);
	vuRegs[0].VI[REG_FBRST].UL = 0x8u; // T-stop for VU0
	h.SetVi(vi::vi1, 3);
	h.LoadProgram({
		TBit(LowerOnly(VJR_L(vi::vi1))), // pair 0
		LoadViImm(vi::vi2, 0x222),       // pair 1: delay slot
		LoadViImm(vi::vi3, 0x333),       // pair 2: skipped
		LoadViImm(vi::vi4, 0x444),       // pair 3: jump target
		EBitNopPair(),                   // pair 4
	});

	h.RunInterpOnly();

	psHu32(INTC_STAT) = 0;
	h.RunJitPreserveBlockCache();
	EXPECT_NE(psHu32(INTC_STAT) & (1u << INTC_VU0), 0u)
		<< "jump-side T-bit handler must set VURegs::flags.INTC so "
		   "recMicroVU0::Execute raises hwIntcIrq(INTC_VU0)";
	EXPECT_EQ((vuRegs[0].VI[REG_VPU_STAT].UL & 0x4u), 0x4u) << "T-finished bit";
	EXPECT_EQ(h.GetViJit(REG_TPC), 3u) << "T-bit jump parks at the register target";
}

// =========================================================================
//  M-bit on a branch — VU0 only. Unlike E, the M bit does not end the
//  program: it breaks out to the EE for a sync and leaves the running bit
//  set, so TPC is a live resume point that the next dispatch continues from.
//  Getting it wrong therefore does not stop anything — it silently restarts
//  the microprogram in the wrong place after every sync.
//
//  These are scored per engine rather than diffed, for the same structural
//  reason as the T-bit-on-branch cases in vu0_e_d_t_m_bit_tests.cpp: the JIT
//  compiles branch and delay slot as one unit, so it resolves the branch and
//  parks at the real successor, while the interpreter's break fires on the
//  branch pair itself and leaves TPC sitting on the delay slot it never ran.
//  The JIT side is the one carrying the behaviour under test, so each case
//  asserts it directly; the interpreter's parked PC is pinned alongside so a
//  change to either model is visible.
// =========================================================================

TEST(VuBranchTerminator, MBitOnUnconditionalBranchParksAtTheTarget)
{
	VuTestHarness h(0);
	vuRegs[0].flags = 0u;
	h.LoadProgram({
		MBit(LowerOnly(VB_L(+2))), // pair 0: branch to pair 3, M-bit break
		LoadViImm(vi::vi2, 0x222), // pair 1: delay slot
		LoadViImm(vi::vi3, 0x333), // pair 2: skipped
		LoadViImm(vi::vi4, 0x444), // pair 3: branch target
		EBitNopPair(),             // pair 4
	});

	h.RunInterpOnly();
	EXPECT_EQ(h.GetViInterp(REG_TPC), 1u)
		<< "interpreter model: the break fires on the branch pair, so TPC rests "
		   "on the delay slot";

	h.RunJitPreserveBlockCache();
	EXPECT_EQ(h.JitSnapshot().regs.flags & VUFLAG_MFLAGSET, VUFLAG_MFLAGSET);
	EXPECT_EQ((h.GetViJit(REG_VPU_STAT) & 0x1u), 0x1u)
		<< "M-bit is a sync, not a terminator — the running bit must stay set";
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x222u) << "the delay slot must run before the break";
	EXPECT_EQ(h.GetViJit(vi::vi3), 0u);
	EXPECT_EQ(h.GetViJit(REG_TPC), 3u)
		<< "M-bit branch must park at the branch target, so the sync resumes there";
}

TEST(VuBranchTerminator, MBitOnConditionalBranchNotTakenParksAfterTheDelaySlot)
{
	VuTestHarness h(0);
	vuRegs[0].flags = 0u;
	h.SetVi(vi::vi1, 1); // IBEQ vi1, vi0 => not taken
	h.LoadProgram({
		MBit(LowerOnly(VIBEQ_L(vi::vi1, vi::vi0, +2))), // pair 0: target pair 3
		LoadViImm(vi::vi2, 0x222),                      // pair 1: delay slot
		LoadViImm(vi::vi3, 0x333),                      // pair 2: fall-through side
		LoadViImm(vi::vi4, 0x444),                      // pair 3: taken target
		EBitNopPair(),                                  // pair 4
	});

	h.RunInterpOnly();
	EXPECT_EQ(h.GetViInterp(REG_TPC), 1u);

	h.RunJitPreserveBlockCache();
	EXPECT_EQ(h.JitSnapshot().regs.flags & VUFLAG_MFLAGSET, VUFLAG_MFLAGSET);
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x222u);
	EXPECT_EQ(h.GetViJit(REG_TPC), 2u)
		<< "not-taken M-bit branch must park on the fall-through side, never at "
		   "the branch target";
}

TEST(VuBranchTerminator, MBitOnConditionalBranchTakenParksAtTheTarget)
{
	// Same program shape as the not-taken case with the condition flipped, so
	// the pair of tests isolates the stub's condition polarity.
	VuTestHarness h(0);
	vuRegs[0].flags = 0u;
	h.SetVi(vi::vi1, 0); // IBEQ vi1, vi0 => taken
	h.LoadProgram({
		MBit(LowerOnly(VIBEQ_L(vi::vi1, vi::vi0, +2))), // pair 0: target pair 3
		LoadViImm(vi::vi2, 0x222),                      // pair 1: delay slot
		LoadViImm(vi::vi3, 0x333),                      // pair 2: fall-through side
		LoadViImm(vi::vi4, 0x444),                      // pair 3: taken target
		EBitNopPair(),                                  // pair 4
	});

	h.RunInterpOnly();
	EXPECT_EQ(h.GetViInterp(REG_TPC), 1u);

	h.RunJitPreserveBlockCache();
	EXPECT_EQ(h.JitSnapshot().regs.flags & VUFLAG_MFLAGSET, VUFLAG_MFLAGSET);
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x222u);
	EXPECT_EQ(h.GetViJit(REG_TPC), 3u)
		<< "taken M-bit branch must park at the branch target";
}

} // namespace recompiler_tests
