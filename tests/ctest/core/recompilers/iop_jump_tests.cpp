// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "harness/JitTestHarness.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

namespace {
constexpr u32 kPark = RecompilerTestEnvironment::kParkingPc;
constexpr u32 kProg = RecompilerTestEnvironment::kProgramPc;
}

// A block that ends with `jr ra` where ra was pre-set to the parking lot.
// Uses the harness's auto-appended terminator; primary purpose is to
// confirm the test-lifecycle plumbing works without the harness's own
// synthetic terminator getting in the way.
TEST(IopJump, JrToParkingLot)
{
	JitTestHarness h;
	h.SetGpr(reg::a0, 0xCAFEu);
	h.LoadProgram({ORI(reg::v0, reg::a0, 0)});   // simple copy
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xCAFEu);
	// After the JR ra; nop terminator, pc should sit inside the parking
	// lot's own `j kPark; nop` infinite loop.
	EXPECT_EQ(h.InterpSnapshot().regs.pc, kPark);
}

TEST(IopJump, JalSetsLinkRegister)
{
	JitTestHarness h;
	// jal writes PC+8 (delay slot return) into r31 = ra.
	// Jump to kPark (the parking lot's `j self` instruction) so execution
	// sticks in a controlled loop instead of drifting into uninitialized
	// memory past the parking lot.
	h.LoadProgramNoTerm({
		JAL(kPark),            // target = parking lot head; returns kProg+8 into ra
		NOP,                    // delay slot (always executed)
		NOP, NOP, NOP,          // filler (not reached)
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::ra), kProg + 8);
}

TEST(IopJump, JalrReturnLinkCustomRd)
{
	JitTestHarness h;
	h.SetGpr(reg::a0, kPark);   // target — parking lot head
	h.LoadProgramNoTerm({
		JALR(reg::v0, reg::a0), // link into v0
		NOP,                     // delay slot
	});
	h.Run();
	// v0 should hold the return address = instruction after delay slot.
	EXPECT_EQ(h.GetGprInterp(reg::v0), kProg + 8);
}

TEST(IopJump, JExactTarget)
{
	JitTestHarness h;
	h.LoadProgramNoTerm({
		J(kPark),
		NOP,
	});
	h.Run();
	EXPECT_EQ(h.InterpSnapshot().regs.pc, kPark);
}

// An immediate jump to address zero is a legal encoding — a call to an
// unresolved weak symbol links as one — and the IOP already has a policy for
// arriving at zero: AX-11 (17c7dd1f95) raises an Address Error on the fetch at
// PC=0 and lets the BIOS handler take over, rather than hard-asserting, because
// PS1 mode drives the IOP into that state through a register jump often enough
// to matter. The immediate form must reach the same policy instead of aborting
// the build while compiling the jump.
//
// JitOnly: the interpreter has no PC=0 model at all — it fetches address zero
// and executes what it finds — so the two arms are meant to disagree here.
TEST(IopJump, ImmediateJumpToZeroTakesTheAddressErrorVector)
{
	JitTestHarness h(JitTestHarness::Mode::JitOnly);
	// Park the exception vector so the handler path terminates like any other
	// test program instead of sledding through low RAM.
	h.LoadProgramAt(0x80000080, {J(kPark), NOP});
	h.LoadProgramNoTerm({J(0), NOP});
	h.Run();

	// ExcCode 4 (AdEL) in Cause, EPC at the faulting fetch, and control landed
	// on the vector — the handler ran rather than the emitter giving up.
	// The vector runs in kseg0, and a J keeps the region it jumps from, so the
	// park is reached through its kseg0 mirror — the same RAM either way.
	EXPECT_EQ((h.JitSnapshot().regs.CP0.n.Cause >> 2) & 0x1Fu, 4u);
	EXPECT_EQ(h.JitSnapshot().regs.CP0.n.EPC, 0u);
	EXPECT_EQ(h.JitSnapshot().regs.pc, 0x80000000u | kPark);
}
