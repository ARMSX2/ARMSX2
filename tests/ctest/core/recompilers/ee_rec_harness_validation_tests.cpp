// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Harness self-validation. These aren't tests of the EE rec — they're smoke
// tests of the EeRecTestHarness itself: that SetGpr/Run/GetGpr round-trips,
// that each instance starts from clean state, and that the JIT and interp
// paths agree (both ultimately delegate to the interpreter).
//
// Note: EeRecTestHarness's ctor calls ZeroCpuRegs() (a memset of cpuRegs), so
// every test starts from a zeroed cpuRegs regardless of what ran before it.
// The tests are therefore order-independent — there is no cross-test
// contamination to guard against, and no required ordering with other harness
// files.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EeFpuFormat.h"
#include "R5900.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

TEST(EeRecHarnessValidation, TestOne_SetsRegisterToKnownPoison)
{
	// Set r9 (t1) to a recognizable value and confirm it round-trips: the
	// harness seeds it, Run() executes a no-op program, and the interp
	// snapshot reports the same value back.
	EeRecTestHarness h;
	h.SetGpr64(reg::t1, 0xDEADBEEFDEADBEEFull);
	h.LoadProgram({NOP});          // no-op, just exercises Run()
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::t1), 0xDEADBEEFDEADBEEFull);
}

TEST(EeRecHarnessValidation, TestTwo_DoesNotSeePreviousTestResidue)
{
	// New harness, no SetGpr. The ctor's ZeroCpuRegs scrub means r9 (t1)
	// starts at 0 even though the previous test set it to a poison value —
	// confirming each test gets clean cpuRegs — and a simple ADDIU computes
	// correctly. (If the ctor scrub ever regressed, r9 would still hold the
	// poison and the assertion below would fire.)
	EeRecTestHarness h;
	h.LoadProgram({
		ADDIU(reg::t0, reg::zero, 42),
	});
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::t1), 0ull)
		<< "r9 (t1) leaked from previous test's poison";
	EXPECT_EQ(h.GetGpr64Interp(reg::t0), 42ull);
}

TEST(EeRecHarnessValidation, MultipleRunsInSameTestAreIdempotent)
{
	// Same harness, same program, called twice. Second Run() must produce
	// identical results — proves that Run()'s internal state management
	// doesn't accumulate over repeated invocations.
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, 5);
	h.SetGpr64(reg::a1, 7);
	h.LoadProgram({ADDU(reg::v0, reg::a0, reg::a1)});
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::v0), 12ull);

	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::v0), 12ull);
}

TEST(EeRecHarnessValidation, DiffJitVsInterpIsTautologicalUnderDelegatingHarness)
{
	// Sanity: the JIT path emits a real block per guest pc (cached in
	// recBlocks/recLUT), but each block's body is a sequence of
	// `bl <per-insn interp helper>` calls, so both paths ultimately run
	// intCpu.Step(). Diff should be empty on any deterministic program.
	// This catches regressions where real JIT opcode emission is
	// accidentally enabled while the harness still delegates to interp.
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, 100);
	h.SetGpr64(reg::a1, 200);
	h.LoadProgram({
		ADDU(reg::v0, reg::a0, reg::a1),
		AND (reg::v1, reg::a0, reg::a1),
		ee::DADDU(reg::a2, reg::a0, reg::a1),
	});
	h.Run();
	EXPECT_EQ(h.GetGpr64Interp(reg::v0), h.GetGpr64Jit(reg::v0));
	EXPECT_EQ(h.GetGpr64Interp(reg::v1), h.GetGpr64Jit(reg::v1));
	EXPECT_EQ(h.GetGpr64Interp(reg::a2), h.GetGpr64Jit(reg::a2));
}

// ---------------------------------------------------------------------------
// EE recLUT region coverage (AX-06). The harness can't execute code from ROM
// regions (programs are hardwired into EE RAM), so region-mapping fixes are
// pinned by querying page coverage directly via the recEeIsPcMapped test
// hook: a mapped page dispatches to a compile-on-first-hit block, an
// unmapped one to UnmappedRecLUTPage (which errors + pauses the VM).
// ---------------------------------------------------------------------------

extern bool recEeIsPcMapped(u32 pc);

TEST(EeRecHarnessValidation, RecLutMapsRom2AndMirrors)
{
	// Force a full rec reset so the LUT is populated.
	EeRecTestHarness h;
	h.LoadProgram({ADDU(reg::v0, reg::a0, reg::a1)});
	h.Run();

	// Sanity: RAM program page is mapped.
	EXPECT_TRUE(recEeIsPcMapped(RecompilerTestEnvironment::kProgramPc));

	// BIOS ROM + ROM1 (regression guards for the existing loops).
	EXPECT_TRUE(recEeIsPcMapped(0x1fc00000u)); // ROM
	EXPECT_TRUE(recEeIsPcMapped(0x1e000000u)); // ROM1 first page
	EXPECT_TRUE(recEeIsPcMapped(0x1e3f0000u)); // ROM1 last page

	// ROM2 (EROM2, Chinese BIOS extension): full 0x1e40-0x1e80 across all
	// three mirror windows, exactly like x86 EE iR5900.cpp:580-584.
	EXPECT_TRUE(recEeIsPcMapped(0x1e400000u)); // first page, physical
	EXPECT_TRUE(recEeIsPcMapped(0x1e7f0000u)); // last page, physical
	EXPECT_TRUE(recEeIsPcMapped(0x9e400000u)); // kseg0 mirror
	EXPECT_TRUE(recEeIsPcMapped(0xbe7f0000u)); // kseg1 mirror

	// Guard: the page just past ROM2 stays unmapped.
	EXPECT_FALSE(recEeIsPcMapped(0x1e800000u));
}

// An FPR slot does not say which format it is in (R5900.h, EeFpuFormat.h) and
// the global that does moves with eeClampMode, so a snapshot carries its own.
// Two harnesses alive at once separate them: the interpreter runs with raw
// words in the low half of each slot, the mode-3 harness relocates the file,
// and the interpreter's results are read after that.
TEST(EeRecHarnessValidation, ASnapshotKeepsTheFprFormatItWasCapturedIn)
{
	constexpr u32 kA = 0x40490FDBu, kB = 0x3FB504F3u;
	constexpr u32 kSum = 0x4091C92Au; // pi + sqrt(2)

	EeRecTestHarness hi;
	hi.EnableCop1();
	hi.SetFprBits(0, kA);
	hi.SetFprBits(1, kB);
	hi.LoadProgram({ee::ADD_S(2, 0, 1)});
	hi.RunInterpOnly();
	ASSERT_EQ(hi.GetFprBitsInterp(2), kSum);

	// Deliberately outlives the reads below: the destructor puts the clamp mode
	// and the file's format back, which would hide exactly what this pins.
	EeRecTestHarness h;
	h.EnableCop1();
	h.EnableFpuFullMode();
	h.SetFprBits(0, kA);
	h.SetFprBits(1, kB);
	h.LoadProgram({ee::ADD_S(2, 0, 1)});
	h.RunJitNoDiff();

	EXPECT_EQ(h.GetFprBitsJit(2), kSum) << "mode 3, read under mode 3";
	EXPECT_EQ(hi.GetFprBitsInterp(2), kSum) << "mode 0 snapshot, read under mode 3";
}

// Restoring means putting the snapshot's words back, so when the file has
// changed format since the capture the slots are re-encoded on the way in.
// A plain memcpy would write mode-0 words into a relocated file and every one
// of them would read as a different number.
TEST(EeRecHarnessValidation, RestoringASnapshotConvertsToTheFilesFormat)
{
	constexpr u32 kWord = 0x40490FDBu;

	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(3, kWord);
	h.LoadProgram({ee::ADD_S(2, 3, 3)});
	h.RunInterpOnly();
	ASSERT_FALSE(h.InterpSnapshot().fprs_relocated);

	const bool saved = EmuConfig.Cpu.Recompiler.fpuFullMode;
	EmuConfig.Cpu.Recompiler.fpuFullMode = true;
	eeFprSyncSlotFormat();
	ASSERT_TRUE(g_eeFprSlotsRelocated);

	h.InterpSnapshot().Restore();
	EXPECT_EQ(fpuRegs.fpr[3].Word(), kWord);
	EXPECT_EQ(fpuRegs.fpr[3].UD, eeFprWidenBits(kWord));

	EmuConfig.Cpu.Recompiler.fpuFullMode = saved;
	eeFprSyncSlotFormat();
}
