// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The E-bit program end under MTVU.
//
// A VU1 microprogram that ends on the E bit has to raise
// VU_Thread::InterruptFlagVUEBit on its way out, or the EE side never learns
// the program finished. The recompiler emits that raise only when MTVU is on,
// and every other test in this suite runs with vuThread pinned off — where the
// raise is not emitted at all and its absence changes no architectural state,
// so no JIT-vs-interp diff here can reach it. This runs a VU1 E-bit program
// with MTVU on and reads the interrupt word back, and again with MTVU off,
// where the same program must leave it alone.

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"
#include "harness/VuEncode.h"

#include "Config.h"
#include "MTVU.h"

#include <gtest/gtest.h>

#include <atomic>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper)
{
	return IBit(VuOp{VLitZero(), upper});
}

// Runs a two-pair VU1 program ending on the E bit and answers the interrupt
// word the JIT pass left behind. The first run is what compiles the program;
// the word is cleared after it so only the JIT's own pass is read back.
u32 EbitRunInterrupts(bool mtvu)
{
	const bool savedThread = EmuConfig.Speedhacks.vuThread;
	EmuConfig.Speedhacks.vuThread = mtvu;

	VuTestHarness h(1);
	h.SetVf(1, 1.5f, -2.25f, 3.0f, 0.0625f);
	h.SetVf(2, 4.0f, 0.5f, -1.0f, 8.0f);
	h.LoadProgram({
		UpperOnly(VADD_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VMUL_U(mask::xyzw, vf::vf5, vf::vf1, vf::vf2)),
	});
	h.RunNoDiff();

	vu1Thread.mtvuInterrupts.store(0, std::memory_order_relaxed);
	h.RunJitPreserveBlockCache();
	const u32 raised = vu1Thread.mtvuInterrupts.load(std::memory_order_relaxed);

	EmuConfig.Speedhacks.vuThread = savedThread;
	return raised;
}

} // namespace

TEST(MvuEbitExit, Vu1ProgramEndRaisesTheMtvuInterrupt)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());

	const u32 savedInterrupts = vu1Thread.mtvuInterrupts.load(std::memory_order_relaxed);
	const u32 withMtvu = EbitRunInterrupts(true);
	const u32 withoutMtvu = EbitRunInterrupts(false);
	vu1Thread.mtvuInterrupts.store(savedInterrupts, std::memory_order_relaxed);

	EXPECT_NE(withMtvu & VU_Thread::InterruptFlagVUEBit, 0u)
		<< "the E-bit end left without raising the interrupt: under MTVU the "
		   "EE never learns the microprogram finished.";

	// MTVU is the switch, and nothing else in the run raises this bit — without
	// this arm a stray raise anywhere would satisfy the one above.
	EXPECT_EQ(withoutMtvu & VU_Thread::InterruptFlagVUEBit, 0u)
		<< "an E-bit end raised the MTVU interrupt with MTVU off.";
}

} // namespace recompiler_tests
