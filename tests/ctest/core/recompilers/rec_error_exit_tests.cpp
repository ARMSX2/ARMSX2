// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The errors the recompilers detect themselves: a jump into a page with no
// mapping, and a register jump to an address that is not a multiple of 4.
// Either one reports an error, asks for the VM to pause, and leaves generated
// code. The guest pc is left at the bad address, so the next entry (a Resume)
// meets it again, and that entry must report again rather than run the
// dispatcher's table lookup on a pc it cannot index.
//
// No VM exists here, so VMManager::SetPaused does nothing. What the tests see
// is the report (StubHost keeps it), that control came back to the test, and
// the guest state left behind.

#include "harness/EeRecTestHarness.h"
#include "harness/JitTestHarness.h"

#include "../StubHostTasks.h"

#include "R3000A.h"
#include "R5900.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

extern s32 recEeExecuteBlock(s32 cycles, u32 park_pc);

namespace {

constexpr u32 kProgramPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kParkingPc = RecompilerTestEnvironment::kParkingPc;

// A mapped page, two bytes into it.
constexpr u32 kUnalignedTarget = kProgramPc + 0x102;
// No recLUT mapping on the EE side (the page past ROM2, see
// EeRecHarnessValidation.RecLutMapsRom2AndMirrors).
constexpr u32 kEeUnmappedTarget = 0x1e800000u;

void ExpectOneReport(const char* title, const char* message)
{
	const std::vector<StubHost::ErrorReport> reports = StubHost::TakeErrorReports();
	ASSERT_EQ(reports.size(), 1u);
	EXPECT_EQ(reports[0].title, title);
	EXPECT_EQ(reports[0].message, message);
}

// Cpu on the recompiler, as it is in a running VM; the test environment's
// default is the interpreter. The error handlers ask for the pause through
// Cpu->ExitExecution, and the interpreter's version jumps to a buffer only its
// own execute loop arms.
struct ScopedRecCpu
{
	ScopedRecCpu() { Cpu = &recCpu; }
	~ScopedRecCpu() { Cpu = &intCpu; }
};

// Enters the EE recompiler at cpuRegs.pc, as a Resume does.
void ReenterEe()
{
	const ScopedRecCpu rec_cpu;
	recEeExecuteBlock(100000, kParkingPc);
}

} // namespace

// The IOP's unmapped-page stub. It leaves the slice after the report, with the
// pc still the target.
TEST(RecErrorExit, IopJumpToUnmappedPageReportsAndLeavesPcAtTarget)
{
	constexpr u32 kIopUnmappedTarget = 0x10000000u;

	StubHost::TakeErrorReports();
	const ScopedRecCpu rec_cpu;

	JitTestHarness h(JitTestHarness::Mode::JitOnly);
	h.SetGpr(reg::a0, kIopUnmappedTarget);
	h.LoadProgramNoTerm({
		JR(reg::a0),
		NOP,
	});
	h.Run();

	ExpectOneReport("R3000A Exception", "Jump to unmapped recLUT page (PC: 0x10000000)");
	EXPECT_EQ(psxRegs.pc, kIopUnmappedTarget);
}

// Jump into an unmapped page: the page's every table entry is the unmapped
// stub, which reports and must leave generated code with pc still the target.
TEST(RecErrorExit, EeJumpToUnmappedPageReportsAndLeavesPcAtTarget)
{
	StubHost::TakeErrorReports();

	EeRecTestHarness h;
	h.SetGpr64(reg::t0, kEeUnmappedTarget);
	h.LoadProgramNoTerm({
		JR(reg::t0),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "Jump to unmapped recLUT page (PC: 0x1e800000)");
	EXPECT_EQ(cpuRegs.pc, kEeUnmappedTarget);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);

	// Resume: the same error again, and back here.
	ReenterEe();
	ExpectOneReport("R5900 Exception", "Jump to unmapped recLUT page (PC: 0x1e800000)");
	EXPECT_EQ(cpuRegs.pc, kEeUnmappedTarget);
}

// The three forms SetBranchReg emits a register jump in: a plain jump, the
// JR $ra return, and the JALR $ra call. Each tests the target's low bits before
// transferring. The delay slot writes $v0 and the JALR writes $ra, both pinned
// to host registers, so the values in cpuRegs show that the exit stored the
// pins.
TEST(RecErrorExit, EeJrToUnalignedAddressReportsAndStopsThere)
{
	StubHost::TakeErrorReports();

	EeRecTestHarness h;
	h.SetGpr64(reg::t0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		JR(reg::t0),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);
}

TEST(RecErrorExit, EeJrRaToUnalignedAddressReportsAndStopsThere)
{
	StubHost::TakeErrorReports();

	EeRecTestHarness h;
	h.LoadProgramNoTerm({
		ADDIU(reg::ra, reg::zero, static_cast<s16>(0x0103)),
		JR(reg::ra),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00000103)");
	EXPECT_EQ(cpuRegs.pc, 0x0103u);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);
}

TEST(RecErrorExit, EeJalrRaToUnalignedAddressReportsAndStopsThere)
{
	StubHost::TakeErrorReports();

	EeRecTestHarness h;
	h.SetGpr64(reg::t0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		JALR(reg::ra, reg::t0),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);
	EXPECT_EQ(cpuRegs.GPR.n.ra.UD[0], kProgramPc + 8);
}

// The Resume after an unaligned jump. The pc is still the unaligned target,
// which the dispatcher's lookup cannot index: an entry has to report it again
// and come back without running anything.
TEST(RecErrorExit, EeEntryWithUnalignedPcReportsAndDoesNotRun)
{
	StubHost::TakeErrorReports();

	EeRecTestHarness h;
	h.SetGpr64(reg::t0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		JR(reg::t0),
		NOP,
	});
	h.RunJitNoDiff();
	ASSERT_EQ(StubHost::TakeErrorReports().size(), 1u);
	ASSERT_EQ(cpuRegs.pc, kUnalignedTarget);

	const u64 cycle = cpuRegs.cycle;
	ReenterEe();
	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
	EXPECT_EQ(cpuRegs.cycle, cycle);

	// And every Resume after it.
	ReenterEe();
	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
}

// The IOP. Its error paths already leave the slice (they jump to the
// recompiler's exit after the report), but the pc stays at the unaligned
// target, and the next slice starts there: with the IOP on its own thread that
// is the next thing the thread does after asking for the pause.
TEST(RecErrorExit, IopJrToUnalignedAddressReportsAndNextSliceDoesNotRun)
{
	StubHost::TakeErrorReports();
	const ScopedRecCpu rec_cpu;

	JitTestHarness h(JitTestHarness::Mode::JitOnly);
	h.SetGpr(reg::a0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		JR(reg::a0),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.Run();

	ExpectOneReport("R3000A Exception", "Jump to unaligned address (PC: 0x00010102)");
	EXPECT_EQ(psxRegs.pc, kUnalignedTarget);
	EXPECT_EQ(psxRegs.GPR.n.v0, 7u);

	// The next slice. It runs nothing, reports again, and says it used the
	// whole budget: a slice that used none would be run again at once by the
	// IOP thread, which would spin.
	const u32 iop_cycle = psxRegs.cycle;
	EXPECT_EQ(psxRec.ExecuteBlock(1000), 0);
	ExpectOneReport("R3000A Exception", "Jump to unaligned address (PC: 0x00010102)");
	EXPECT_EQ(psxRegs.pc, kUnalignedTarget);
	EXPECT_EQ(psxRegs.cycle, iop_cycle);
}
