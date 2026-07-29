// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE↔VU0 run-ahead floor semantics, pinned per-engine.
//
// Settles the 2026-07-25 "COP2 interlock shuffle divergence": with VU0 left
// running and fewer than 16 cycles from its E-bit, a following NON-interlocked
// COP2 transfer diverges JIT-vs-interp BY DESIGN:
//
//   - interp transfer ops sync exactly: vu0Sync() grants
//     (cpuRegs.cycle - VU0.cycle) VU cycles, no floor (VU0.cpp).
//   - the arm64 JIT's non-interlocked sites BL the RunAhead stub →
//     vu0SyncRunAheadThin(), which floors the grant at 16 cycles
//     (iCOP2-arm64.cpp; mirrors x86 ExecuteBlockJIT →
//     CalculateMinRunCycles(delta, false) = std::max(16U, cycles)).
//
// So when 0 <= delta < remaining <= 16, the JIT's floored grant reaches the
// E-bit (VPU_STAT.0 clears, TPC advances past the end) while the interp
// leaves the program in flight. The x86 JIT floors identically — the two
// recompilers agree and the interpreter is the odd engine out, exactly like
// the I-bit immediate clamp (vu_minmax_order_tests.cpp precedent). NOT a
// bug; do not "fix" either engine to match the other.
//
// The original repro inherited this state by accident —
// Vu0SpecialBits.MBitDoesNotClearVpuStatRunningBit left the global vuRegs[0]
// mid-program for whichever EeVu0* test ran next under --gtest_shuffle.
// EnableVu0Capture() now resets VU0 control state (VI[24..31], flags, cycle,
// interpreter resume sentinels) so inheritance is impossible, and these
// tests construct the window deliberately instead.
//
// The memory's hypothesis 1 (the EEINST analysis marking FINISH on
// non-interlocked reads) is refuted for this repro: a lone QMFC2 gets
// EEINST_COP2_SYNC_VU0. The analysis hoists a FINISH onto a leading
// transfer only when a macro-arithmetic op follows in the same block
// (iR5900Analysis.cpp COP2MicroFinishPass), and the interpreter finishes
// unconditionally at that arithmetic op anyway — end states converge; only
// the floor produces a lasting divergence.

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

TEST(EeVu0RunAheadFloor, NonInterlockedReadInsideFloorWindowDivergesByDesign)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	// 12 NOP pairs + E-bit pair + delay pair ≈ 14 VU cycles remaining —
	// inside the 16-cycle floor, beyond any single-op block's delta.
	ConstructRunningVu0(h, 12);
	// The engines legitimately disagree on these two after the floored
	// overshoot; assert them per-engine below instead of via the auto-diff.
	h.IgnoreVu0Vi(REG_VPU_STAT);
	h.IgnoreVu0Vi(REG_TPC);
	h.LoadProgram({QMFC2(r_t0, 1)});
	h.Run();

	// JIT: max(delta, 16) >= remaining → drained to the E-bit.
	EXPECT_EQ(h.GetVu0ViJit(REG_VPU_STAT), 0u);
	// interp: exact delta < remaining → still running.
	EXPECT_EQ(h.GetVu0ViInterp(REG_VPU_STAT), 1u);
	// The transferred data is identical — the NOP program writes nothing.
	EXPECT_EQ(h.GetGpr64Jit(r_t0), h.GetGpr64Interp(r_t0));
}

TEST(EeVu0RunAheadFloor, InterlockedReadFinishesOnBothEngines)
{
	// Same construction, interlock bit set: both engines run the program to
	// its E-bit (interp _vu0FinishMicro, JIT SyncFinish stub). The
	// divergence tracks the run-ahead floor, not the interlock bit.
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

TEST(EeVu0RunAheadFloor, LargeDeltaConvergesOnBothEngines)
{
	// With the EE genuinely ahead of VU0 by more than the remaining program,
	// the exact sync alone reaches the E-bit — the divergence window exists
	// only where delta < remaining <= 16.
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
