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
