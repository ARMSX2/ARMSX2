// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP recompiler runs inside a C call from whoever hands it a slice -- the EE recompiler's event
// test in Off mode, which holds its own pins in x22-x25, or the IOP thread -- so ExecuteBlock must give
// back every callee-saved register it was called with, whatever blocks ran in between. These tests run
// slices through the real entry point from a trampoline that fills x19-x28 with a pattern and reads
// them back. (The registers the IOP code itself uses are saved by its stack frame; this is what stops a
// change to that frame, or a pool that grows into a register the frame does not save, from corrupting
// the caller.)

#include "harness/JitTestHarness.h"

#include <gtest/gtest.h>

#include <vector>

using namespace recompiler_tests;
using namespace mips;

namespace {

// u64 in[10] -> x19..x28, call fn(budget), x19..x28 -> out[10]. Restores the caller's registers.
extern "C" __attribute__((naked, noinline)) s32 IopTestCallWithSentinels(const u64* in, u64* out, s32 (*fn)(s32), s32 budget)
{
	asm volatile(
		"stp x29, x30, [sp, #-112]!\n\t"
		"mov x29, sp\n\t"
		"stp x19, x20, [sp, #16]\n\t"
		"stp x21, x22, [sp, #32]\n\t"
		"stp x23, x24, [sp, #48]\n\t"
		"stp x25, x26, [sp, #64]\n\t"
		"stp x27, x28, [sp, #80]\n\t"
		"str x1, [sp, #96]\n\t"
		"mov x16, x2\n\t"
		"mov w9, w3\n\t"
		"ldp x19, x20, [x0, #0]\n\t"
		"ldp x21, x22, [x0, #16]\n\t"
		"ldp x23, x24, [x0, #32]\n\t"
		"ldp x25, x26, [x0, #48]\n\t"
		"ldp x27, x28, [x0, #64]\n\t"
		"mov w0, w9\n\t"
		"blr x16\n\t"
		"ldr x9, [sp, #96]\n\t"
		"stp x19, x20, [x9, #0]\n\t"
		"stp x21, x22, [x9, #16]\n\t"
		"stp x23, x24, [x9, #32]\n\t"
		"stp x25, x26, [x9, #48]\n\t"
		"stp x27, x28, [x9, #64]\n\t"
		"ldp x19, x20, [sp, #16]\n\t"
		"ldp x21, x22, [sp, #32]\n\t"
		"ldp x23, x24, [sp, #48]\n\t"
		"ldp x25, x26, [sp, #64]\n\t"
		"ldp x27, x28, [sp, #80]\n\t"
		"ldp x29, x30, [sp], #112\n\t"
		"ret\n\t");
}

void ExpectCalleeSavedRegistersSurvive(const std::vector<u32>& prog, s32 budget, std::initializer_list<std::pair<u32, u32>> gpr = {})
{
	JitTestHarness h(JitTestHarness::Mode::JitOnly);
	h.LoadProgramAt(RecompilerTestEnvironment::kProgramPc, prog.data(), prog.size(), false);
	psxRec.Clear(RecompilerTestEnvironment::kProgramPc, static_cast<u32>(prog.size()));
	psxRegs.pc = RecompilerTestEnvironment::kProgramPc;
	psxRegs.GPR.n.ra = RecompilerTestEnvironment::kParkingPc;
	psxRegs.cycle = 100;
	psxRegs.iopNextEventCycle = 10'000'000;
	for (const auto& g : gpr)
		psxRegs.GPR.r[g.first] = g.second;

	u64 in[10], out[10];
	for (int i = 0; i < 10; i++)
		in[i] = 0x5A5A'0000'0000'0000ull | (static_cast<u64>(19 + i) << 32) | (0xC0DE0000u + i);
	IopTestCallWithSentinels(in, out, psxRec.ExecuteBlock, budget);
	for (int i = 0; i < 10; i++)
		EXPECT_EQ(out[i], in[i]) << "x" << (19 + i) << " was changed across ExecuteBlock";
}

} // namespace

TEST(IopFrame, CalleeSavedRegistersSurviveASliceThatRunsBlocksAndLeavesThroughTheExit)
{
	// A few blocks with loads, stores (RAM and device), a branch and the idle loop at the end.
	const std::vector<u32> prog = {
		LUI(reg::t1, 0x1f80), ADDIU(reg::t0, reg::zero, 3), LW(reg::v0, 0x1070, reg::t1), SW(reg::t0, 0x40, reg::a0),
		ADDIU(reg::t0, reg::t0, -1), BNE(reg::t0, reg::zero, -2), NOP, LW(reg::v1, 0, reg::a0), SW(reg::v1, 8, reg::a0), JR(reg::ra), NOP};
	ExpectCalleeSavedRegistersSurvive(prog, 400, {{reg::a0, RecompilerTestEnvironment::kScratchAddr}});
}

TEST(IopFrame, CalleeSavedRegistersSurviveASliceThatExitsAtOnce)
{
	const std::vector<u32> prog = {ADDIU(reg::v0, reg::zero, 1), JR(reg::ra), NOP};
	ExpectCalleeSavedRegistersSurvive(prog, 1);
	ExpectCalleeSavedRegistersSurvive(prog, 0);
	ExpectCalleeSavedRegistersSurvive(prog, -50);
}
