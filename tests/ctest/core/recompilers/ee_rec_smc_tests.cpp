// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Self-modifying-code coverage for the EE recompiler.
//
// Parallels iop_smc_tests.cpp — which caught a real psxRecClearMem
// merge-semantics bug during IOP port testing. The same chain applies to
// the EE: memWrite → vtlb store → Cpu->Clear → recClear invalidates the
// cached block, next dispatch re-compiles.
//
// These tests are architecturally correct and serve as JIT regression gates
// for block compilation.

#include "harness/EeRecTestHarness.h"

#include "Memory.h"
#include "R5900.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);
extern u32 recEeBlockGuestSize(u32 pc_query);

namespace {
constexpr u32 kProgramPc = RecompilerTestEnvironment::kProgramPc;
} // namespace

TEST(EeRecSmc, GuestSwOverwritesInstructionAheadOfPc)
{
	// Block layout:
	//   0x00: LUI a1, hi(ADDIU v0, zero, 0x1337)
	//   0x04: ORI a1, a1, lo(...)
	//   0x08: SW a1, 0(a0)                  — self-modify the word at 0x10
	//   0x0C: NOP (alignment / give SW time to settle)
	//   0x10: ADDIU v0, zero, 0x0BAD        — replaced before we get here
	//   0x14: JR ra / NOP — appended by LoadProgram
	//
	// Interp-only. A guest SW into a page holding compiled code must
	// invalidate the covering block, but the test harness does not wire
	// page-protection SIGSEGV backpatching, so a guest SW into a compiled
	// page does not auto-invalidate. The harness-driven TriggerSmc() case
	// (next test) still works because it calls memWrite32 + recClear from
	// the *host* side; only guest-emitted SW misses here.
	constexpr u32 kNewInstr = ADDIU(reg::v0, reg::zero, 0x1337);
	const u16 hi = static_cast<u16>(kNewInstr >> 16);
	const u16 lo = static_cast<u16>(kNewInstr & 0xFFFFu);

	EeRecTestHarness h;
	h.SetGpr64(reg::a0, kProgramPc + 0x10);
	h.LoadProgram({
		LUI(reg::a1, hi),
		ORI(reg::a1, reg::a1, lo),
		SW(reg::a1, 0, reg::a0),              // overwrite insn @ 0x10
		NOP,
		ADDIU(reg::v0, reg::zero, 0x0BAD),    // becomes ADDIU v0,0x1337
	});
	h.RunInterpOnly();
	h.ExpectGpr64(reg::v0, 0x1337ull);
}

TEST(EeRecSmc, TriggerSmcHelperRewritesMemory)
{
	// Demonstrates the TriggerSmc harness helper. Program reads the word
	// at kProgramPc+0x100 (which the helper rewrites pre-run) into v0 and
	// returns. Tests the "harness rewrites then jits" discipline that
	// iop_smc_tests uses for programmatic SMC fixtures.
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, kProgramPc + 0x100);
	h.LoadProgram({
		LW(reg::v0, 0, reg::a0),
	});
	h.TriggerSmc(kProgramPc + 0x100, 0xDEADBEEFu);
	h.TrackMemWindow(kProgramPc + 0x100, 4);
	h.Run();
	// LW sign-extends, 0xDEADBEEF has bit 31 set, so v0 is sign-extended.
	h.ExpectGpr64(reg::v0, 0xFFFFFFFFDEADBEEFull);
}

TEST(EeRecSmc, RewriteAdjacentWordDoesNotAffectCurrentInstruction)
{
	// Write to the word *after* the last real instruction. No effect on
	// the currently-executing block. Guard against an over-aggressive
	// Cpu->Clear implementation that invalidates unrelated words.
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, kProgramPc + 0x200);
	h.LoadProgram({
		ADDIU(reg::v0, reg::zero, 42),
		SW(reg::v0, 0, reg::a0),                 // write beyond program
	});
	h.TrackMemWindow(kProgramPc + 0x200, 4);
	h.Run();
	h.ExpectGpr64(reg::v0, 42ull);
	EXPECT_EQ(h.ReadU32(kProgramPc + 0x200), 42u);
}

// Regression gate for recClear straddler-block fnptr reset.
//
// The bug: recClear's per-word reset loop only visited words inside
// [addr, end), missing block STARTs that lie before addr but whose body
// extends into the cleared range. Combined with Arm64BaseBlocks::Remove()
// patching only the compiled-code stub, this left BLOCK(startpc)->fnptr
// pointing at the just-overwritten stub. The next dispatch from startpc
// followed the stub's `B JITCompile` redirect into JITCompile, which then
// tripped the recRecompile fnptr assertion because BLOCK->fnptr was the
// stub address, not JITCompile. BLOCK(startpc)->fnptr must be reset to the
// JIT-compile entry so re-dispatch recompiles cleanly.
//
// The production trigger is the fastmem-backpatch path: vtlb calls
// Cpu->Clear(guest_pc, 1) with a MID-block PC. SimulateFastmemFault()
// mimics that single production entry.
TEST(EeRecSmc, StraddlerBlockRecClearResetsStartFnptr)
{
	EeRecTestHarness h;

	// 31-instruction block (block extent: kProgramPc..kProgramPc+0x7C, plus
	// the harness-appended JR ra/NOP at +0x7C/+0x80). Long enough that any
	// mid-block fault PC < endpc is the straddler-from-below scenario.
	std::initializer_list<u32> program = {
		ADDIU(reg::v0, reg::zero, 0),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
		ADDIU(reg::v0, reg::v0, 0x100), ADDIU(reg::v0, reg::v0, 0x100),
	};
	h.LoadProgram(program);

	// First pass: compile, run JIT + interp from a fresh cache, diff.
	// v0 = 30 * 0x100 = 0x1E00.
	h.Run(EeRecTestHarness::RunMode::FreshCache);
	h.ExpectGpr64(reg::v0, 0x1E00ull);

	// Single-word Clear at instruction index 16 (offset 0x40) — well inside
	// the block (block startpc=0..endpc=0x7C). The straddler-from-below case.
	h.SimulateFastmemFault(kProgramPc + 0x40);

	// Second pass: re-dispatch from kProgramPc. Block was invalidated mid-
	// extent. BLOCK(startpc)->fnptr must be reset to JITCompile so dispatch
	// recompiles cleanly; if it remains stale, the recRecompile fnptr
	// assertion fires (process abort).
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x1E00ull);
}

// ===========================================================================
//  GE-18: recRAMCopy overlap check.
//
//  x86 model (ix86-32/iR5900.cpp:2636-2661): after compiling a block, walk
//  older blocks overlapping its range and memcmp each old block's
//  recRAMCopy snapshot against live memory; a mismatch means that old block
//  went stale through a write no protection path caught — recClear it.
//  The arm64 port disabled the walk (it compared the wrong region and
//  recompile-looped); only the snapshot memcpy survived.
//
//  The harness wires no page protection, so a raw PSM poke of a compiled
//  non-manual block leaves it genuinely stale — exactly the class the walk
//  exists to catch at the next overlapping compile.
// ===========================================================================

namespace {
// A plain RAM page well away from the program/scratch/manual regions.
constexpr u32 kOverlapRoutinePc = 0x00090000;

void SeedOverlapRoutine(EeRecTestHarness& h, u16 imm)
{
	h.WriteU32(kOverlapRoutinePc + 0, ADDIU(reg::v0, reg::zero, imm));
	h.WriteU32(kOverlapRoutinePc + 4, JR(reg::ra));
	h.WriteU32(kOverlapRoutinePc + 8, NOP);
}

// Caller preserving the harness return address around a JAL to `target`.
void LoadOverlapCaller(EeRecTestHarness& h, u32 target, u16 sentinel)
{
	h.LoadProgram({
		OR(reg::t0, reg::ra, reg::zero),
		ADDIU(reg::v0, reg::zero, sentinel),
		JAL(target),
		NOP,
		OR(reg::ra, reg::t0, reg::zero),
	});
}
} // namespace

TEST(EeRecSmc, OverlappingCompileClearsStaleBlock)
{
	EeRecTestHarness h;

	// 1. Compile + run the routine: v0 = 5. Its source is snapshotted into
	//    recRAMCopy at compile time.
	SeedOverlapRoutine(h, 5);
	LoadOverlapCaller(h, kOverlapRoutinePc, 0x111);
	h.Run(EeRecTestHarness::RunMode::FreshCache);
	h.ExpectGpr64(reg::v0, 5);

	// 2. RAW poke (no memWrite32, no recClear — the write class no
	//    protection path sees in the harness): v0 = 7 semantics now in RAM,
	//    but the compiled block still encodes v0 = 5.
	*(u32*)PSM(kOverlapRoutinePc) = ADDIU(reg::v0, reg::zero, 7);

	// 3. Compile an OVERLAPPING block by entering mid-routine (at the JR).
	//    The overlap walk must see the stale older block and recClear it.
	//    v0 keeps the caller sentinel through the JR ra block.
	LoadOverlapCaller(h, kOverlapRoutinePc + 4, 0x222);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x222);

	// 4. Re-enter the routine at its head: a cleared block recompiles from
	//    current memory (v0 = 7); a stale survivor still executes v0 = 5.
	LoadOverlapCaller(h, kOverlapRoutinePc, 0x333);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 7);
}

// Producer (248/248 of the reuses in a DBZ3 eerunner boot): the GE-18
// stale-overlap walk fires recClear MID-COMPILE. The walk skips the
// in-progress block (so its BASEBLOCKEX survives), but the removed stale
// straddler-from-below spans the in-progress startpc, so the post-walk
// ClearRecLUT tail wipes BLOCK(startpc)->fnptr. The arm64 port set the LUT
// fnptr at the START of recRecompile (x86 sets it at the END, after the
// walk — ix86-32/iR5900.cpp:2664), so nothing restored it. The next
// dispatch recompiled into the surviving entry, which never refreshed
// BASEBLOCKEX::fnptr: the linker (recBlocks.Link) then targets the OLD
// copy while the LUT dispatches the NEW one, and x86size (end − stale
// fnptr) spans both copies (162KB of duplicate bodies in a battle dump).
//
// The caller uses JALR (indirect) so every entry to the victim goes
// through the LUT dispatcher, never a patched direct link. The target
// comes from t1 (seeded per-Run by the harness), so the caller block is
// compiled ONCE and never needs invalidating — the harness wires no page
// protection, so a reloaded program is not reliably re-compiled.
TEST(EeRecSmc, MidCompileOverlapClearKeepsLutAndLinkerCoherent)
{
	EeRecTestHarness h;

	// Straddler A at S: entered at S it sets v0 = 5+1+1 = 7. The victim
	// block is entered at V = S+4 (inside A), so A straddles V from below.
	constexpr u32 kS = 0x000A0000;
	constexpr u32 kV = kS + 4;
	h.WriteU32(kS + 0x00, ADDIU(reg::v0, reg::zero, 5));
	h.WriteU32(kS + 0x04, ADDIU(reg::v0, reg::v0, 1));
	h.WriteU32(kS + 0x08, ADDIU(reg::v0, reg::v0, 1));
	h.WriteU32(kS + 0x0C, JR(reg::ra));
	h.WriteU32(kS + 0x10, NOP);

	h.LoadProgram({
		OR(reg::t0, reg::ra, reg::zero),
		ADDIU(reg::v0, reg::zero, 0x40),
		JALR(reg::ra, reg::t1),
		NOP,
		OR(reg::ra, reg::t0, reg::zero),
	});

	// 1. Compile + run A from its head; its source is snapshotted into
	//    recRAMCopy. v0 = 7.
	h.SetGpr64(reg::t1, kS);
	h.Run(EeRecTestHarness::RunMode::FreshCache);
	h.ExpectGpr64(reg::v0, 7);

	// 2. RAW poke of A's first word (the write class no protection path
	//    sees in the harness) — A's snapshot is now stale. V's own words
	//    (S+4..) are untouched.
	*(u32*)PSM(kS) = ADDIU(reg::v0, reg::zero, 6);

	// 3. Compile the victim by entering at V. At the end of that compile
	//    the overlap walk sees stale A → recClear(V, ...) → A removed, and
	//    the ClearRecLUT tail over A's extent [S, S+0x14) wipes LUT[V].
	//    v0 = 0x40 + 2 = 0x42.
	h.SetGpr64(reg::t1, kV);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x42);

	// Inertness guard: prove the mid-compile overlap walk actually fired.
	// Everything below only means something if recClear ran and removed A;
	// without this, a future change that stops the walk from firing turns
	// the whole test green-but-inert instead of failing.
	{
		uptr afn = 0, alut = 0;
		u32 asz = 0;
		EXPECT_FALSE(recEeBlockHostInfo(kS, &afn, &asz, &alut))
			<< "PROBE: straddler A survived — the overlap walk never fired";
	}

	// THE PIN: after the mid-compile clear, the victim's LUT dispatch
	// target and its BASEBLOCKEX fnptr must agree (recRecompile must
	// restore the LUT after the walk, as x86 does).
	uptr fn1 = 0, lut1 = 0;
	u32 sz1 = 0;
	ASSERT_TRUE(recEeBlockHostInfo(kV, &fn1, &sz1, &lut1));
	EXPECT_EQ(lut1, fn1) << "LUT and BASEBLOCKEX disagree after mid-compile recClear";

	// 4. Re-dispatch V. On the broken baseline this recompiles into the
	//    surviving entry: a duplicate adjacent body, stale fnptr, x86size
	//    spanning both copies.
	h.SetGpr64(reg::t1, kV);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x42);
	uptr fn2 = 0, lut2 = 0;
	u32 sz2 = 0;
	ASSERT_TRUE(recEeBlockHostInfo(kV, &fn2, &sz2, &lut2));
	EXPECT_EQ(lut2, fn2) << "linker (BASEBLOCKEX) and dispatcher (LUT) diverged";
	EXPECT_EQ(fn2, fn1) << "victim was needlessly recompiled";
	EXPECT_EQ(sz2, sz1) << "x86size grew — duplicate block body emitted";
}

TEST(EeRecSmc, OverlapWalkIgnoresUnmodifiedNeighbors)
{
	// Guard for the disable-comment's recompile-loop concern: compiling an
	// overlapping block over an UNMODIFIED older block must NOT clear it.
	// (The old bug compared the new block's not-yet-snapshotted region —
	// always-mismatch — which recClear-looped. The x86 walk compares old
	// blocks' own snapshots, which match untouched memory.)
	EeRecTestHarness h;

	SeedOverlapRoutine(h, 9);
	LoadOverlapCaller(h, kOverlapRoutinePc, 0x111);
	h.Run(EeRecTestHarness::RunMode::FreshCache);
	h.ExpectGpr64(reg::v0, 9);

	// Overlapping compile with NO modification anywhere.
	LoadOverlapCaller(h, kOverlapRoutinePc + 4, 0x222);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x222);

	// The head block still runs (recompiled or cached — result identical
	// either way; the real assertion is we didn't recompile-loop above and
	// the block graph stayed sane).
	LoadOverlapCaller(h, kOverlapRoutinePc, 0x333);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 9);
}

namespace {
// A second RAM page, for the four-block skip geometry below.
constexpr u32 kSkipRoutinePc = 0x000B0000;

// v0 = head_imm, then nine increments, then JR ra. Entering at any word
// yields a distinct v0, so which compiled body actually ran is observable.
//
//   +0x00  ADDIU v0, zero, head_imm
//   +0x04 .. +0x24  ADDIU v0, v0, 1        (9 words)
//   +0x28  JR ra
//   +0x2C  NOP                             extent = [S, S+0x30), 12 words
void SeedSkipRoutine(EeRecTestHarness& h, u16 head_imm)
{
	h.WriteU32(kSkipRoutinePc + 0x00, ADDIU(reg::v0, reg::zero, head_imm));
	for (u32 off = 0x04; off <= 0x24; off += 4)
		h.WriteU32(kSkipRoutinePc + off, ADDIU(reg::v0, reg::v0, 1));
	h.WriteU32(kSkipRoutinePc + 0x28, JR(reg::ra));
	h.WriteU32(kSkipRoutinePc + 0x2C, NOP);
}
} // namespace

// The walk descends recBlocks by startpc and `break`s at the first block that
// ends before the new block starts. That assumes end addresses rise with start
// addresses — recBlocks guarantees no such thing. A long block at a LOW address
// can jump clean over a short one lying between it and the new block, and the
// break stops the walk before the long one is ever compared, so a genuinely
// stale block survives.
//
// Geometry (one 4K page, so no page-boundary split interferes):
//
//   S+0x00  A ──────────────────────────────────────────┐  [S,      S+0x30)
//   S+0x10  B ────────┐                                 │  [S+0x10, S+0x18)
//   S+0x18  C ────────┴──────────────────────┐          │  [S+0x18, S+0x30)
//   S+0x20  N ───────────────────┐           │          │  [S+0x20, S+0x30)
//   S+0x28  JR ra                            │          │
//
// B is short because the boundary scan stops at the first address that already
// holds a compiled block (`pblock->GetFnptr() != JITCompile`) and C is compiled
// first. Compiling N then walks: C (overlaps, matches) → B (does not reach N) →
// and must keep going to reach the stale A underneath it.
//
// A goes stale only AFTER B and C compiled, so no earlier walk clears it: that
// is what makes this reachable rather than merely theoretical.
TEST(EeRecSmc, OverlapWalkScansPastNonOverlappingNeighbor)
{
	EeRecTestHarness h;

	SeedSkipRoutine(h, 5);

	// JALR through t1 (seeded per-Run) so every entry re-dispatches through
	// the LUT and the caller block itself is compiled once, as in
	// MidCompileOverlapClearKeepsLutAndLinkerCoherent.
	h.LoadProgram({
		OR(reg::t0, reg::ra, reg::zero),
		ADDIU(reg::v0, reg::zero, 0x40),
		JALR(reg::ra, reg::t1),
		NOP,
		OR(reg::ra, reg::t0, reg::zero),
	});

	// 1. A, from the head: snapshotted into recRAMCopy over [S, S+0x30).
	h.SetGpr64(reg::t1, kSkipRoutinePc);
	h.Run(EeRecTestHarness::RunMode::FreshCache);
	h.ExpectGpr64(reg::v0, 5 + 9);

	// 2. C, before B, so B has something to terminate against.
	h.SetGpr64(reg::t1, kSkipRoutinePc + 0x18);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x40 + 4);

	// 3. B — truncated at C, so it ends below N and the walk will meet it.
	h.SetGpr64(reg::t1, kSkipRoutinePc + 0x10);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x40 + 6); // falls through C's words to the JR

	// Fixture guard: without the short B between A and N there is nothing for
	// the walk to break on, and the whole test passes green-but-inert.
	ASSERT_EQ(recEeBlockGuestSize(kSkipRoutinePc), 12u)
		<< "A did not span the routine — fixture geometry broken";
	ASSERT_EQ(recEeBlockGuestSize(kSkipRoutinePc + 0x10), 2u)
		<< "B was not truncated at C — fixture geometry broken";
	ASSERT_LE(kSkipRoutinePc + 0x10 + 2 * 4, kSkipRoutinePc + 0x20)
		<< "B reaches N — nothing for the walk to break on";

	// 4. RAW poke of A's head only (the write class no protection path sees in
	//    the harness). B's and C's own words are untouched, so only A is stale.
	*(u32*)PSM(kSkipRoutinePc) = ADDIU(reg::v0, reg::zero, 6);

	// 5. Compile N. Its walk must reach past B and clear stale A.
	h.SetGpr64(reg::t1, kSkipRoutinePc + 0x20);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x40 + 2);

	// THE PIN: A overlaps N and is stale, so the walk must have cleared it.
	{
		uptr afn = 0, alut = 0;
		u32 asz = 0;
		EXPECT_FALSE(recEeBlockHostInfo(kSkipRoutinePc, &afn, &asz, &alut))
			<< "stale A survived: the walk stopped at non-overlapping B "
			   "instead of scanning past it";
	}

	// 6. Re-enter A's head. Cleared → recompiles from current memory (6+9);
	//    a stale survivor still executes the pre-poke body (5+9).
	h.SetGpr64(reg::t1, kSkipRoutinePc);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 6 + 9);
}
