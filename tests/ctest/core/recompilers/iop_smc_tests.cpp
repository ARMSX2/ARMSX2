// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Self-modifying code coverage for the IOP recompiler.
//
// The chain: `iopMemWrite*` → `psxCpu->Clear(addr, 1)` → `recClearIOP` →
// `psxRecClearMem(pc)` → walk recBlocks, merge overlapping BASEBLOCKEX
// entries, `iopClearRecLUT` zeroes the LUT slots for the cleared range. The
// next dispatcher lookup misses and compilation re-fires via the JIT-compile
// trampoline.
//
// The SMC invalidation path requires tests that overwrite and re-dispatch;
// a single immutable program would not cover this.

#include "harness/JitTestHarness.h"

#include "IopMem.h"
#include "R3000A.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

namespace {
constexpr u32 kProgramPc = RecompilerTestEnvironment::kProgramPc;    // 0x00010000
constexpr u32 kParkingPc = RecompilerTestEnvironment::kParkingPc;    // 0x001F0000

// Block 2 well outside block 1's 4KB page so merging logic doesn't
// touch it across the tests that stay in block 1.
constexpr u32 kBlock2Pc = 0x00014000;
} // namespace

TEST(IopSmc, HarnessOverwriteThenRunProducesNewResult)
{
	// 1) Compile a 100-producing program. 2) Rewrite the first word in
	// place with a 200-producing ADDIU. 3) Re-run and verify the new
	// opcode executes.
	JitTestHarness h;
	h.LoadProgram({
		ADDIU(reg::v0, reg::zero, 100),
	});
	h.Run();
	ASSERT_EQ(h.GetGprInterp(reg::v0), 100u);

	// Overwrite the ADDIU in place. iopMemWrite32 calls psxCpu->Clear,
	// which invalidates the cached block at kProgramPc.
	iopMemWrite32(kProgramPc, ADDIU(reg::v0, reg::zero, 200));

	// Re-enter. SetPc restores the program's entry point; SetRa keeps
	// the `jr ra; nop` terminator going to the parking lot.
	h.SetPc(kProgramPc);
	h.SetRa(kParkingPc);
	h.RunResume();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 200u);
}

TEST(IopSmc, GuestSwIntoOwnProgramRegionTriggersRecompile)
{
	// Block 1 builds an ADDIU instruction into a GPR (lui+ori), stores
	// it to block 2's address via SW (which calls psxCpu->Clear), then
	// jumps to block 2. Block 2 is pre-loaded with a different ADDIU
	// but should execute the just-written one.
	//
	// MIPS ADDIU opcode: 0x09 in top 6 bits.
	//   ADDIU v0, zero, 0x1337
	//     = (0x09 << 26) | (0 << 21) | (2 << 16) | 0x1337
	//     = 0x24020000 | 0x1337
	//     = 0x24021337
	JitTestHarness h;
	constexpr u32 kNewInstr = ADDIU(reg::v0, reg::zero, 0x1337);
	h.SetGpr(reg::a0, kBlock2Pc);
	// Pre-state: a1 holds the new ADDIU encoding. LUI+ORI to materialize
	// the 32-bit constant into a1.
	const u16 hi = static_cast<u16>(kNewInstr >> 16);
	const u16 lo = static_cast<u16>(kNewInstr & 0xFFFF);
	h.LoadProgramAt(kProgramPc, {
		LUI(reg::a1, hi),
		ORI(reg::a1, reg::a1, lo),
		SW(reg::a1, 0, reg::a0),            // overwrites block 2's 1st word
		J(kBlock2Pc),
		NOP,                                 // delay slot
	}, /*append_jr_ra_term=*/false);
	// Pre-load block 2 with a POISON opcode that the SW should replace.
	h.LoadProgramAt(kBlock2Pc, {
		ADDIU(reg::v0, reg::zero, 0x0BAD),   // should not execute
	}, /*append_jr_ra_term=*/true);
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0x1337u);
}

TEST(IopSmc, OverlappingBlockClearInvalidatesBoth)
{
	// Two blocks in the same 4KB page: kProgramPc + 0x000 and
	// kProgramPc + 0x100. Run each once so both are compiled. Then
	// overwrite a word inside the first block's range. `psxRecClearMem`
	// will merge overlapping entries; both should be invalidated and
	// recompiled on the next dispatch.
	JitTestHarness h;
	constexpr u32 kProgA = kProgramPc;
	constexpr u32 kProgB = kProgramPc + 0x100;

	h.LoadProgramAt(kProgA, {
		ADDIU(reg::v0, reg::zero, 1),
	}, /*append_jr_ra_term=*/true);
	h.LoadProgramAt(kProgB, {
		ADDIU(reg::v1, reg::zero, 2),
	}, /*append_jr_ra_term=*/true);

	// First run enters at block A (default kProgramPc).
	h.Run();
	ASSERT_EQ(h.GetGprInterp(reg::v0), 1u);

	// Second run entered at block B — proves block B was compiled too.
	h.SetPc(kProgB);
	h.SetRa(kParkingPc);
	h.RunResume();
	ASSERT_EQ(h.GetGprInterp(reg::v1), 2u);

	// Now overwrite block A's first word with a different instruction.
	// The SMC clear may merge-and-invalidate block B as well (depends
	// on the block's size tracking). Regardless, re-entering block A
	// should execute the NEW instruction.
	iopMemWrite32(kProgA, ADDIU(reg::v0, reg::zero, 99));
	h.SetPc(kProgA);
	h.SetRa(kParkingPc);
	h.RunResume();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 99u);

	// Block B should still work — whether it was silently recompiled
	// or left cached, the correct instruction must run. Poison v1 first so
	// the assertion forces block B to actively WRITE 2 (RunResume does not
	// reset GPRs; without this, a silently-skipped block B would leave the
	// stale 2 from the second run and pass vacuously).
	h.SetGpr(reg::v1, 0xDEADBEEFu);
	h.SetPc(kProgB);
	h.SetRa(kParkingPc);
	h.RunResume();
	EXPECT_EQ(h.GetGprInterp(reg::v1), 2u);
}

TEST(IopSmc, ClearOutsideCodeRegionLeavesActiveBlockIntact)
{
	// Compile a block, then store at an address far from the code. The
	// store triggers a Clear(addr, 1) but the block's LUT slot is
	// unaffected, so a resume uses the cached block.
	JitTestHarness h;
	h.LoadProgram({
		ADDIU(reg::v0, reg::zero, 42),
	});
	h.Run();
	ASSERT_EQ(h.GetGprInterp(reg::v0), 42u);

	// Far-from-code store. kScratchAddr is in a different page from
	// kProgramPc, so Clear(kScratchAddr, 1) can't touch the program's
	// BASEBLOCK entry.
	iopMemWrite32(RecompilerTestEnvironment::kScratchAddr, 0xDEADBEEF);

	h.SetPc(kProgramPc);
	h.SetRa(kParkingPc);
	h.RunResume();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 42u);
}

TEST(IopSmc, OverwritePastCoverageGranuleBoundaryTriggersRecompile)
{
	// Regression coverage for the s_iopCodeCov fast path in psxRecClearMem
	// (iR3000A-arm64.cpp): a zero coverage counter lets a store skip the
	// recBlocks search entirely. A block longer than one 256-byte granule
	// must bump the counter for EVERY granule its span overlaps — if only
	// the head granule were counted, a store into the block's tail would
	// early-out and leak the SMC through to stale compiled code.
	//
	// 70 body words = 280 bytes from kProgramPc (granule-aligned), so the
	// block spans two granules and the overwritten word (offset 268) sits
	// in the second one.
	JitTestHarness h;
	std::vector<u32> body;
	body.push_back(ADDIU(reg::v0, reg::zero, 0));
	for (int i = 0; i < 69; i++)
		body.push_back(ADDIU(reg::v0, reg::v0, 1));
	h.LoadProgramAt(kProgramPc, body.data(), body.size(), /*append_jr_ra_term=*/true);
	h.Run();
	ASSERT_EQ(h.GetGprInterp(reg::v0), 69u);

	// Word 67 (byte offset 268 >= 256): +1 becomes +100 → 68 + 100.
	iopMemWrite32(kProgramPc + 67 * 4, ADDIU(reg::v0, reg::v0, 100));
	h.SetPc(kProgramPc);
	h.SetRa(kParkingPc);
	h.RunResume();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 168u);
}

TEST(IopSmc, MirrorAddressStoreClearsBlock)
{
	// Store through the KSEG0 mirror (0x8001xxxx) of a block compiled at
	// its KUSEG address. The clear path and the s_iopCodeCov fast path
	// both key on HWADDR (mirror prefix stripped), so the mirror store
	// must invalidate the same block. If the mirrored address were used
	// raw, its granule counter would read zero and the SMC would leak.
	JitTestHarness h;
	h.LoadProgram({
		ADDIU(reg::v0, reg::zero, 100),
	});
	h.Run();
	ASSERT_EQ(h.GetGprInterp(reg::v0), 100u);

	iopMemWrite32(0x80000000u | kProgramPc, ADDIU(reg::v0, reg::zero, 200));
	h.SetPc(kProgramPc);
	h.SetRa(kParkingPc);
	h.RunResume();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 200u);
}

TEST(IopSmc, OverwriteLastWordOfBlockBeforeTerminator)
{
	// Edge case: write at the exact last instruction of a block's body
	// (just before the `jr ra; nop` terminator). The entire block should
	// re-compile with the new instruction, without the terminator being
	// disturbed. Regression coverage for the LUT-fnptr early-exit in
	// psxRecClearMem (mid-block words still hold iopJITCompile so a
	// fnptr-based check would silently leak the SMC through to stale
	// compiled code).
	JitTestHarness h;
	h.LoadProgram({
		ADDIU(reg::t0, reg::zero, 10),
		ADDIU(reg::t1, reg::zero, 20),
		ADDU(reg::v0, reg::t0, reg::t1),         // last body word: v0 = 30
	});
	h.Run();
	ASSERT_EQ(h.GetGprInterp(reg::v0), 30u);

	// The 3rd body word sits at kProgramPc + 8. Replace with ADDU that
	// sums t0+t1 and leaves it in v1 instead.
	iopMemWrite32(kProgramPc + 8, ADDU(reg::v1, reg::t0, reg::t1));
	h.SetPc(kProgramPc);
	h.SetRa(kParkingPc);
	h.SetGpr(reg::v0, 0xDEAD);   // sentinel — should stay 0xDEAD since the
	                              // new opcode writes v1, not v0.
	h.SetGpr(reg::v1, 0);
	h.RunResume();
	EXPECT_EQ(h.GetGprInterp(reg::v1), 30u);
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xDEADu);
}
