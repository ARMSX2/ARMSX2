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
// is the report (StubHost keeps it), the post-mortem written to the log (once
// per error, not again on a Resume), that control came back to the test, and
// the guest state left behind.

#include "harness/EeRecTestHarness.h"
#include "harness/JitTestHarness.h"

#include "../StubHostTasks.h"

#include "arm64/RecErrorReport-arm64.h"

#include "R3000A.h"
#include "R5900.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

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

// Each test reports errors at pcs earlier tests used.
void ForgetEarlierErrors()
{
	StubHost::TakeErrorReports();
	RecErrorReport::ResetForTests();
}

// The post-mortem the error wrote to the log has each of these lines.
void ExpectLogged(std::initializer_list<const char*> lines)
{
	const std::string log = RecErrorReport::TakeLoggedForTests();
	for (const char* line : lines)
		EXPECT_NE(log.find(line), std::string::npos) << "missing: " << line << "\nin:\n" << log;
}

void ExpectNothingLogged()
{
	EXPECT_EQ(RecErrorReport::TakeLoggedForTests(), "");
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

	ForgetEarlierErrors();
	const ScopedRecCpu rec_cpu;

	JitTestHarness h(JitTestHarness::Mode::JitOnly);
	h.SetGpr(reg::a0, kIopUnmappedTarget);
	h.LoadProgramNoTerm({
		JR(reg::a0),
		NOP,
	});
	h.Run();

	ExpectOneReport("R3000A Exception", "Jump to unmapped recLUT page (PC: 0x10000000)");
	ExpectLogged({
		"IOP recompiler stopped: jump to unmapped address 0x10000000\n",
		"\n  The word before it is not zero, or not in RAM or ROM.\n",
		"\n  ra 001f0000  sp 00000000\n",
	});
	EXPECT_EQ(psxRegs.pc, kIopUnmappedTarget);
}

// Jump into an unmapped page: the page's every table entry is the unmapped
// stub, which reports and must leave generated code with pc still the target.
TEST(RecErrorExit, EeJumpToUnmappedPageReportsAndLeavesPcAtTarget)
{
	ForgetEarlierErrors();

	EeRecTestHarness h;
	h.SetGpr64(reg::t0, kEeUnmappedTarget);
	h.LoadProgramNoTerm({
		JR(reg::t0),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "Jump to unmapped recLUT page (PC: 0x1e800000)");
	ExpectLogged({
		"EE recompiler stopped: jump to unmapped address 0x1e800000\n",
		"\n  0x1e800000 is the end of ROM2: the guest ran off the end of it.\n",
		"\n  ra 00000000001f0000  sp 0000000000000000\n",
		"\n      t0 000000001e800000 ",
	});
	EXPECT_EQ(cpuRegs.pc, kEeUnmappedTarget);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);

	// Resume: the same error again, and back here; the log has it already.
	ReenterEe();
	ExpectOneReport("R5900 Exception", "Jump to unmapped recLUT page (PC: 0x1e800000)");
	ExpectNothingLogged();
	EXPECT_EQ(cpuRegs.pc, kEeUnmappedTarget);
}

// The three forms SetBranchReg emits a register jump in: a plain jump, the
// JR $ra return, and the JALR $ra call. Each tests the target's low bits before
// transferring. The delay slot writes $v0 and the JALR writes $ra, both pinned
// to host registers, so the values in cpuRegs show that the exit stored the
// pins.
TEST(RecErrorExit, EeJrToUnalignedAddressReportsAndStopsThere)
{
	ForgetEarlierErrors();

	EeRecTestHarness h;
	h.SetGpr64(reg::t0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		JR(reg::t0),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	ExpectLogged({
		"EE recompiler stopped: jump to unaligned address 0x00010102\n",
		"\n  The jump at 0x00010000 (jr ->t0) read t0 = 0x00010102 before its delay slot; t0 now holds 0000000000010102\n",
		"\n  > 00010000  01000008  jr ->t0\n",
	});
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);
}

TEST(RecErrorExit, EeJrRaToUnalignedAddressReportsAndStopsThere)
{
	ForgetEarlierErrors();

	EeRecTestHarness h;
	h.LoadProgramNoTerm({
		ADDIU(reg::ra, reg::zero, static_cast<s16>(0x0103)),
		JR(reg::ra),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00000103)");
	ExpectLogged({
		"\n  The jump at 0x00010004 (jr ->ra) read ra = 0x00000103 before its delay slot; ra now holds 0000000000000103\n",
	});
	EXPECT_EQ(cpuRegs.pc, 0x0103u);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);
}

TEST(RecErrorExit, EeJalrRaToUnalignedAddressReportsAndStopsThere)
{
	ForgetEarlierErrors();

	EeRecTestHarness h;
	h.SetGpr64(reg::t0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		JALR(reg::ra, reg::t0),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	ExpectLogged({
		"\n  The jump at 0x00010000 (jalr ->t0) read t0 = 0x00010102 before its delay slot; t0 now holds 0000000000010102\n",
		" ra 0000000000010008\n",
	});
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);
	EXPECT_EQ(cpuRegs.GPR.n.ra.UD[0], kProgramPc + 8);
}

// The Resume after an unaligned jump. The pc is still the unaligned target,
// which the dispatcher's lookup cannot index: an entry has to report it again
// and come back without running anything.
TEST(RecErrorExit, EeEntryWithUnalignedPcReportsAndDoesNotRun)
{
	ForgetEarlierErrors();

	EeRecTestHarness h;
	h.SetGpr64(reg::t0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		JR(reg::t0),
		NOP,
	});
	h.RunJitNoDiff();
	ASSERT_EQ(StubHost::TakeErrorReports().size(), 1u);
	ASSERT_EQ(cpuRegs.pc, kUnalignedTarget);

	ExpectLogged({"\n  The jump at 0x00010000 (jr ->t0)"});

	const u64 cycle = cpuRegs.cycle;
	ReenterEe();
	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	ExpectNothingLogged();
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
	EXPECT_EQ(cpuRegs.cycle, cycle);

	// And every Resume after it.
	ReenterEe();
	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	ExpectNothingLogged();
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);

	// An unaligned pc that no jump reported (a savestate, say) is logged, with
	// no jump to name.
	cpuRegs.pc = kUnalignedTarget + 4;
	ReenterEe();
	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010106)");
	ExpectLogged({
		"EE recompiler stopped: jump to unaligned address 0x00010106\n",
		"\n  The pc was already unaligned when the recompiler was entered; the jump that set it is not known.\n",
	});
}

// The IOP. Its error paths already leave the slice (they jump to the
// recompiler's exit after the report), but the pc stays at the unaligned
// target, and the next slice starts there: with the IOP on its own thread that
// is the next thing the thread does after asking for the pause.
TEST(RecErrorExit, IopJrToUnalignedAddressReportsAndNextSliceDoesNotRun)
{
	ForgetEarlierErrors();
	const ScopedRecCpu rec_cpu;

	JitTestHarness h(JitTestHarness::Mode::JitOnly);
	h.SetGpr(reg::a0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		JR(reg::a0),
		ADDIU(reg::v0, reg::zero, 7),
	});
	h.Run();

	ExpectOneReport("R3000A Exception", "Jump to unaligned address (PC: 0x00010102)");
	ExpectLogged({
		"IOP recompiler stopped: jump to unaligned address 0x00010102\n",
		"\n  The jump at 0x00010000 (jr ->a0) read a0 = 0x00010102 before its delay slot; a0 now holds 00010102\n",
		"\n      a0 00010102   a1 00000000 ",
		"\n  > 00010000  00800008  jr ->a0\n",
	});
	EXPECT_EQ(psxRegs.pc, kUnalignedTarget);
	EXPECT_EQ(psxRegs.GPR.n.v0, 7u);

	// The next slice. It runs nothing, reports again, and says it used the
	// whole budget: a slice that used none would be run again at once by the
	// IOP thread, which would spin.
	const u32 iop_cycle = psxRegs.cycle;
	EXPECT_EQ(psxRec.ExecuteBlock(1000), 0);
	ExpectOneReport("R3000A Exception", "Jump to unaligned address (PC: 0x00010102)");
	ExpectNothingLogged();
	EXPECT_EQ(psxRegs.pc, kUnalignedTarget);
	EXPECT_EQ(psxRegs.cycle, iop_cycle);
}

// ERET returns to ErrorEPC when Status.ERL is set and to EPC otherwise, and a
// guest can store any value in either. The recompiler runs ERET through the
// interpreter's function and dispatches to the pc it set, so an unaligned
// EPC reached the dispatcher's table lookup unchecked.
namespace {

constexpr u32 kCp0Status = 12;
constexpr u32 kCp0Epc = 14;
constexpr u32 kCp0ErrorEpc = 30;
constexpr u32 kStatusExl = 1u << 1;
constexpr u32 kStatusErl = 1u << 2;

} // namespace

TEST(RecErrorExit, EeEretToUnalignedEpcReportsAndStopsThere)
{
	ForgetEarlierErrors();

	EeRecTestHarness h;
	h.SetCp0(kCp0Status, kStatusExl);
	h.SetGpr64(reg::t0, kUnalignedTarget);
	h.LoadProgramNoTerm({
		MTC0(reg::t0, kCp0Epc),
		ADDIU(reg::v0, reg::zero, 7),
		ee::ERET,
		NOP,
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "ERET to unaligned address (PC: 0x00010102)");
	ExpectLogged({
		"EE recompiler stopped: ERET to unaligned address 0x00010102\n",
		"\n  The ERET at 0x00010008 returned to EPC = 0x00010102 (Status.ERL was clear)\n",
		"\n  > 00010008  42000018  eret\n",
	});
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
	EXPECT_EQ(cpuRegs.GPR.n.v0.UD[0], 7u);
	EXPECT_EQ(cpuRegs.CP0.n.Status.b.EXL, 0u);

	// Resume: the entry check reports it, and the log has it already.
	ReenterEe();
	ExpectOneReport("R5900 Exception", "Jump to unaligned address (PC: 0x00010102)");
	ExpectNothingLogged();
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget);
}

TEST(RecErrorExit, EeEretToUnalignedErrorEpcReportsAndStopsThere)
{
	ForgetEarlierErrors();

	EeRecTestHarness h;
	h.SetCp0(kCp0Status, kStatusErl);
	h.SetGpr64(reg::t0, kUnalignedTarget + 4);
	h.SetGpr64(reg::t1, kProgramPc + 0x100);
	h.LoadProgramNoTerm({
		MTC0(reg::t0, kCp0ErrorEpc),
		MTC0(reg::t1, kCp0Epc),
		ee::ERET,
		NOP,
	});
	h.RunJitNoDiff();

	ExpectOneReport("R5900 Exception", "ERET to unaligned address (PC: 0x00010106)");
	ExpectLogged({
		"EE recompiler stopped: ERET to unaligned address 0x00010106\n",
		"\n  The ERET at 0x00010008 returned to ErrorEPC = 0x00010106 (Status.ERL was set)\n",
		"  EPC 0x00010100  ErrorEPC 0x00010106\n",
	});
	EXPECT_EQ(cpuRegs.pc, kUnalignedTarget + 4);
	EXPECT_EQ(cpuRegs.CP0.n.Status.b.ERL, 0u);
}

// The aligned returns, against the interpreter: ERL set goes to ErrorEPC,
// clear goes to EPC. Each lands on its pad, which marks $s6 and leaves for the
// parking lot; the other address is the other pad.
TEST(RecErrorExit, EeEretToAlignedAddressesMatchesTheInterpreter)
{
	constexpr u32 kPadA = kProgramPc + 0x100;
	constexpr u32 kPadB = kProgramPc + 0x10c;
	for (const bool erl : {false, true})
	{
		SCOPED_TRACE(erl ? "ERL" : "EXL");
		EeRecTestHarness h;
		h.SetCp0(kCp0Status, erl ? kStatusErl : kStatusExl);
		h.SetGpr64(reg::t0, erl ? kPadB : kPadA); // EPC
		h.SetGpr64(reg::t1, erl ? kPadA : kPadB); // ErrorEPC
		h.SetGpr64(reg::s7, kParkingPc);
		std::vector<u32> code = {
			MTC0(reg::t0, kCp0Epc),
			MTC0(reg::t1, kCp0ErrorEpc),
			ee::ERET,
			NOP,
		};
		code.resize((kPadA - kProgramPc) / 4, NOP);
		for (const u32 mark : {0x111u, 0x222u})
		{
			code.push_back(ORI(reg::s6, reg::zero, static_cast<u16>(mark)));
			code.push_back(JR(reg::s7));
			code.push_back(NOP);
		}
		h.LoadProgramNoTerm(code);
		h.Run();
		EXPECT_EQ(h.GetGpr64Jit(reg::s6), 0x111u);
		EXPECT_EQ(h.GetGpr64Interp(reg::s6), 0x111u);
	}
}
