// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE↔VU0 sync of a running VU0 program at a COP2 transfer.
//
// Both engines grant VU0 exactly the EE cycles since its last sync: the
// interpreter's transfer ops through vu0Sync() (VU0.cpp), the arm64 JIT's
// sync sites through vu0SyncThin() (iCOP2-arm64.cpp). A non-interlocked
// transfer leaves a program that needs more than that running; an interlocked
// one runs it to its E-bit.
//
// The JIT used to floor a non-interlocked grant at 16 cycles, as x86 PCSX2
// does, and drain such a program where the interpreter left it running. The
// floor ran VU0 past the EE's clock, which loses an EE write to a VU0 wait
// loop (EeVu0WaitRelease); it is gone.
//
// EnableVu0Capture() resets VU0 control state (VI[24..31], flags, cycle,
// interpreter resume sentinels) so a test cannot inherit a running program
// from the one before it; these tests construct it deliberately.

#include "harness/EeRecTestHarness.h"

#include "R5900.h"
#include "VU.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 r_t0 = 8;

// Leave VU0 mid-program: busy, TPC=0, `nop_pairs`+2 instruction-pairs from
// the E-bit drain, with VU0's clock even with the EE's — the delta at the
// transfer's sync accrues only the few cycles the single-op EE block charges
// ahead of it, comfortably below the remaining program length.
void ConstructRunningVu0(EeRecTestHarness& h, u32 nop_pairs)
{
	u32 off = 0;
	for (u32 i = 0; i < nop_pairs; i++, off += 8)
		h.SeedVu0Microprogram(off, {vu::NopPair()});
	h.SeedVu0Microprogram(off, {
		vu::EBitNopPair(),
		vu::NopPair(), // E-bit delay pair
	});
	h.SeedVu0Vi(REG_TPC, 0);
	h.SeedVu0Vi(REG_VPU_STAT, 1);
	vuRegs[0].flags = 0;
	vuRegs[0].cycle = cpuRegs.cycle;
}

} // namespace

TEST(EeVu0NonInterlockedSync, ReadLeavesALongerProgramRunning)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	// 12 NOP pairs + E-bit pair + delay pair: about 14 VU cycles left, more
	// than a single-op block charges ahead of its transfer.
	ConstructRunningVu0(h, 12);
	// How far each engine got differs: the harness runs its interpreter leg
	// under the recompiler's settings, where _vu0run does not charge the
	// interpreter's pending cycles first, so that VU0 runs none.
	h.IgnoreVu0Vi(REG_TPC);
	h.LoadProgram({QMFC2(r_t0, 1)});
	h.Run();

	EXPECT_EQ(h.GetVu0ViJit(REG_VPU_STAT), 1u);
	EXPECT_EQ(h.GetVu0ViInterp(REG_VPU_STAT), 1u);
	EXPECT_EQ(h.GetGpr64Jit(r_t0), h.GetGpr64Interp(r_t0));
}

TEST(EeVu0NonInterlockedSync, InterlockedReadFinishesOnBothEngines)
{
	// Same construction, interlock bit set: both engines run the program to
	// its E-bit (interp _vu0FinishMicro, JIT SyncFinish stub).
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	ConstructRunningVu0(h, 12);
	h.LoadProgram({QMFC2_I(r_t0, 1)});
	h.Run();

	EXPECT_EQ(h.GetVu0ViJit(REG_VPU_STAT), 0u);
	EXPECT_EQ(h.GetVu0ViInterp(REG_VPU_STAT), 0u);
	EXPECT_EQ(h.GetVu0ViJit(REG_TPC), h.GetVu0ViInterp(REG_TPC));
	EXPECT_EQ(h.GetGpr64Jit(r_t0), h.GetGpr64Interp(r_t0));
}

TEST(EeVu0NonInterlockedSync, LargeDeltaConvergesOnBothEngines)
{
	// With the EE ahead of VU0 by more than the remaining program, the
	// non-interlocked sync alone reaches the E-bit.
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	ConstructRunningVu0(h, 12);
	vuRegs[0].cycle = cpuRegs.cycle - 64;
	h.LoadProgram({QMFC2(r_t0, 1)});
	h.Run();

	EXPECT_EQ(h.GetVu0ViJit(REG_VPU_STAT), 0u);
	EXPECT_EQ(h.GetVu0ViInterp(REG_VPU_STAT), 0u);
	EXPECT_EQ(h.GetVu0ViJit(REG_TPC), h.GetVu0ViInterp(REG_TPC));
}

} // namespace recompiler_tests
