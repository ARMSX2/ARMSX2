// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Regression: NFS Carbon (SLUS-21493) post-FMV EE↔IOP SBUS deadlock.
//
// Localized (eerunner --stepdiff/--contmem, 2026-07-23) to an EE-JIT
// miscompile of the self-looping byte-fill block at guest pc 0x00174c30:
//
//     0x174c30: sb    a1, 0(a3)     ; *a3 = (byte)a1
//     0x174c34: addiu a2, a2, -1    ; count--
//     0x174c38: addiu a3, a3, 1     ; dst++
//     0x174c3c: nop
//     0x174c40: nop
//     0x174c44: bne   a2, zero, 0x174c30   ; block branches to ITSELF
//
// The block is SL-1 loop-resident (self-branch back-edge, loop-live GPRs
// pinned in a preheader — see ee_rec_loop_residency_tests.cpp). What the
// existing residency coverage never exercises, and what this block does:
//   * the store VALUE (a1) is a static EE pin-table reg (x21), held live
//     across the resident body, and
//   * the store BASE (a3) is itself a loop-carried pointer incremented in
//     the same body and used as the fastmem store address.
// The JIT ran the loop a different length than the interpreter (a2/a3
// diverged; a1 was correct), corrupting memory the later SBUS handshake
// depends on -> the EE never rings F240 -> the IOP stays idle -> deadlock.

#include "harness/EeRecTestHarness.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

namespace {
constexpr u32 kPark = RecompilerTestEnvironment::kParkingPc;
constexpr u32 kDataAddr = 0x00030000; // EE RAM scratch, away from the program
constexpr u8 kFill = 0xAB;
// Distinct from kFill and from 0, so an overrun is visible whatever it writes.
constexpr u8 kGuard = 0x5A;

// Lay a guard pattern over the destination window before the run.
//
// Guest RAM is process-global and nothing resets it between tests, so a test
// that asserts "this byte was not written" must establish that byte itself.
// These tests share kDataAddr and fill different lengths of it —
// ManyIterations writes 100 bytes — so in any order but the declared one, a
// shorter test used to see a neighbour's leftover kFill one past its end.
// Seeding also strengthens the check: expecting 0 could not distinguish an
// overrun that stores zero (the ExactGameEntryState shape stores exactly
// that) from no overrun at all.
void SeedGuard(EeRecTestHarness& h, u32 addr, u32 len)
{
	for (u32 i = 0; i < len; i++)
		h.WriteU8(addr + i, kGuard);
}

// The exact Carbon block shape, parameterized by store-value reg, store-base
// reg (whether it is the loop-carried, in-body-incremented pointer), and count.
// Layout (self-loop at kProgramPc):
//   idx0 sb   <val>, 0(<base>)
//   idx1 addiu a2, a2, -1
//   idx2 addiu <ptr>, <ptr>, 1   ; ptr advanced every iteration
//   idx3 nop
//   idx4 nop
//   idx5 bne  a2, zero, -6       ; -> idx0
//   idx6 nop                     ; delay slot
//   idx7 j park; nop
} // namespace

// Exact reproduction: pinned store value (a1) + loop-carried store base (a3).
TEST(EeRecCarbonSelfLoop, PinnedValueLoopCarriedBaseByteFill)
{
	EeRecTestHarness h;
	h.SetGpr64(reg::a1, kFill);       // store value — PINNED (x21)
	h.SetGpr64(reg::a2, 8);           // loop count
	h.SetGpr64(reg::a3, kDataAddr);   // store base — loop-carried pointer
	h.TrackMemWindow(kDataAddr, 16);
	SeedGuard(h, kDataAddr, 16);
	h.LoadProgramNoTerm({
		SB(reg::a1, 0, reg::a3),
		ADDIU(reg::a2, reg::a2, -1),
		ADDIU(reg::a3, reg::a3, 1),
		NOP,
		NOP,
		BNE(reg::a2, reg::zero, -6), // -> idx0
		NOP,                          // delay slot
		J(kPark), NOP,
	});
	h.Run(); // auto-diffs JIT vs interp — fails on divergence

	// Correct (interp) result, documented for intent:
	for (u32 i = 0; i < 8; i++)
		EXPECT_EQ(h.ReadU8(kDataAddr + i), kFill) << "byte " << i;
	EXPECT_EQ(h.ReadU8(kDataAddr + 8), kGuard) << "one past end must be untouched";
	h.ExpectGpr64(reg::a2, 0ull);
	h.ExpectGpr64(reg::a3, static_cast<u64>(kDataAddr + 8));
}

// Isolate the PIN axis: same shape, but the store value is a non-pin-table
// reg (t5). If this passes while the a1 version fails, the static-pin ×
// loop-residency interaction is the culprit.
TEST(EeRecCarbonSelfLoop, NonPinnedValueLoopCarriedBaseByteFill)
{
	EeRecTestHarness h;
	h.SetGpr64(reg::t5, kFill);       // store value — NOT pinned
	h.SetGpr64(reg::a2, 8);
	h.SetGpr64(reg::a3, kDataAddr);
	h.TrackMemWindow(kDataAddr, 16);
	SeedGuard(h, kDataAddr, 16);
	h.LoadProgramNoTerm({
		SB(reg::t5, 0, reg::a3),
		ADDIU(reg::a2, reg::a2, -1),
		ADDIU(reg::a3, reg::a3, 1),
		NOP,
		NOP,
		BNE(reg::a2, reg::zero, -6),
		NOP,
		J(kPark), NOP,
	});
	h.Run();
	for (u32 i = 0; i < 8; i++)
		EXPECT_EQ(h.ReadU8(kDataAddr + i), kFill) << "byte " << i;
	EXPECT_EQ(h.ReadU8(kDataAddr + 8), kGuard) << "one past end must be untouched";
	h.ExpectGpr64(reg::a2, 0ull);
	h.ExpectGpr64(reg::a3, static_cast<u64>(kDataAddr + 8));
}

// Isolate the loop-carried-base axis: pinned store value (a1), but the base
// (t4) is fixed — the in-body increment advances a separate pointer (a3).
// If this passes while the first test fails, using the loop-carried pointer
// as the fastmem store base is the culprit.
TEST(EeRecCarbonSelfLoop, PinnedValueFixedBaseByteFill)
{
	EeRecTestHarness h;
	h.SetGpr64(reg::a1, kFill);
	h.SetGpr64(reg::a2, 8);
	h.SetGpr64(reg::a3, 0);           // loop-carried counter-ish, unused as base
	h.SetGpr64(reg::t4, kDataAddr);   // fixed store base
	h.TrackMemWindow(kDataAddr, 16);
	SeedGuard(h, kDataAddr, 16);
	h.LoadProgramNoTerm({
		SB(reg::a1, 0, reg::t4),
		ADDIU(reg::a2, reg::a2, -1),
		ADDIU(reg::a3, reg::a3, 1),
		NOP,
		NOP,
		BNE(reg::a2, reg::zero, -6),
		NOP,
		J(kPark), NOP,
	});
	h.Run();
	EXPECT_EQ(h.ReadU8(kDataAddr), kFill);
	EXPECT_EQ(h.ReadU8(kDataAddr + 1), kGuard) << "fixed base must not advance";
	h.ExpectGpr64(reg::a2, 0ull);
	h.ExpectGpr64(reg::a3, 8ull);
}

// The EXACT entry state eerunner captured at guest 0x174c30 (a1=0, a2=4,
// a3=0x1fffb98 near top of RAM). If this passes, the JIT computes this block
// correctly and the stepdiff "divergence" at 0x174c30 is a self-loop sampling
// artifact, not a codegen bug.
TEST(EeRecCarbonSelfLoop, ExactGameEntryStateFillsCorrectly)
{
	constexpr u32 kTop = 0x01fffb98;
	EeRecTestHarness h;
	h.SetGpr64(reg::a1, 0);
	h.SetGpr64(reg::a2, 4);
	h.SetGpr64(reg::a3, kTop);
	h.TrackMemWindow(kTop, 8);
	SeedGuard(h, kTop, 8);
	h.LoadProgramNoTerm({
		SB(reg::a1, 0, reg::a3),
		ADDIU(reg::a2, reg::a2, -1),
		ADDIU(reg::a3, reg::a3, 1),
		NOP,
		NOP,
		BNE(reg::a2, reg::zero, -6),
		NOP,
		J(kPark), NOP,
	});
	h.Run();
	for (u32 i = 0; i < 4; i++)
		EXPECT_EQ(h.ReadU8(kTop + i), 0u) << "byte " << i;
	EXPECT_EQ(h.ReadU8(kTop + 4), kGuard) << "one past end must be untouched";
	h.ExpectGpr64(reg::a2, 0ull);
	h.ExpectGpr64(reg::a3, static_cast<u64>(kTop + 4));
}

// Longer loop crossing several event-exit reschedules (matches the game's
// large fill), exact shape.
TEST(EeRecCarbonSelfLoop, PinnedValueLoopCarriedBaseManyIterations)
{
	EeRecTestHarness h;
	h.SetGpr64(reg::a1, kFill);
	h.SetGpr64(reg::a2, 100);
	h.SetGpr64(reg::a3, kDataAddr);
	h.TrackMemWindow(kDataAddr, 128);
	SeedGuard(h, kDataAddr, 128);
	h.LoadProgramNoTerm({
		SB(reg::a1, 0, reg::a3),
		ADDIU(reg::a2, reg::a2, -1),
		ADDIU(reg::a3, reg::a3, 1),
		NOP,
		NOP,
		BNE(reg::a2, reg::zero, -6),
		NOP,
		J(kPark), NOP,
	});
	h.Run();
	for (u32 i = 0; i < 100; i++)
		EXPECT_EQ(h.ReadU8(kDataAddr + i), kFill) << "byte " << i;
	EXPECT_EQ(h.ReadU8(kDataAddr + 100), kGuard) << "one past end must be untouched";
	h.ExpectGpr64(reg::a2, 0ull);
	h.ExpectGpr64(reg::a3, static_cast<u64>(kDataAddr + 100));
}
