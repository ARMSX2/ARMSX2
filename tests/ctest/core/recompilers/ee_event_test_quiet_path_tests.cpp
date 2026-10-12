// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// What one EE event test (_cpuEventTest_Shared) does to the machine, piece by
// piece, on the "quiet" entries that make up most of them in a game, and on
// the entries next to them that are not quiet.
//
// The quiet path is trimmed for speed: its pieces are skipped when a cheap
// test shows they would do nothing. These tests pin the behaviour every such
// skip must keep:
//
//  - INTC/DMAC: an interrupt is raised exactly when a masked cause is pending
//    and COP0 Status lets it through, and the counter HOLD registers are
//    latched whenever INTC_STAT bit 1 is pending and unmasked, whether or not
//    the interrupt is taken. That latch is a side effect of intcInterrupt()
//    that a gate on Status alone would lose.
//  - VU sync: a running VU that is behind the EE is run; an idle one is not.
//    With VU1 on its own thread (MTVU), the event test consumes the flags the
//    VU thread raised, and never runs VU1 on the EE thread.
//
// The recompiler test environment runs with no VM, so the event test's first
// check (the VM state) would return straight away; each test marks the VM
// Running for its own duration and restores everything it touched.

#include "harness/RecompilerTestEnvironment.h"

#include "Config.h"
#include "Counters.h"
#include "Hardware.h"
#include "IopHw.h"
#include "IopMem.h"
#include "MTVU.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "VUmicro.h"

#include <cstring>
#include <memory>
#include <gtest/gtest.h>

namespace {

constexpr u32 kStatusIE   = 1u << 0;
constexpr u32 kStatusIntc = 1u << 10; // IM2: INTC
constexpr u32 kStatusDmac = 1u << 11; // IM3: DMAC
constexpr u32 kStatusEIE  = 1u << 16;
constexpr u32 kStatusAllEnabled = kStatusIE | kStatusIntc | kStatusDmac | kStatusEIE;

constexpr u32 kHoldSentinel = 0xFFFFFFFFu; // rcntRcount() is 16 bits wide; it can never return this

// VU micro NOP pair: lower NOP, then upper NOP with the E bit, then one more
// NOP pair for the delay slot.
constexpr u32 kVuLowerNop  = 0x8000033Cu;
constexpr u32 kVuUpperNop  = 0x000002FFu;
constexpr u32 kVuUpperNopE = 0x400002FFu;

class EeEventTestQuietPath : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(recompiler_tests::RecompilerTestEnvironment::IsReady());

		m_cpu = std::make_unique<cpuRegisters>(cpuRegs);
		m_psx = std::make_unique<psxRegisters>(psxRegs);
		m_eeHw = std::make_unique<u8[]>(Ps2MemSize::Hardware);
		std::memcpy(m_eeHw.get(), eeHw, Ps2MemSize::Hardware);
		m_iopHw = std::make_unique<u8[]>(Ps2MemSize::IopHardware);
		std::memcpy(m_iopHw.get(), iopHw, Ps2MemSize::IopHardware);
		std::memcpy(m_counters, counters, sizeof(counters));
		std::memcpy(m_vu0Vi, VU0.VI, sizeof(m_vu0Vi));
		std::memcpy(m_vu1Vi, VU1.VI, sizeof(m_vu1Vi));
		std::memcpy(m_vu0Micro, VU0.Micro, sizeof(m_vu0Micro));
		std::memcpy(m_vu1Micro, VU1.Micro, sizeof(m_vu1Micro));
		m_vu0Cycle = VU0.cycle;
		m_vu1Cycle = VU1.cycle;
		m_eesCycle = EEsCycle;
		m_eeoCycle = EEoCycle;
		m_iopEventAction = iopEventAction;
		m_vuThread = EmuConfig.Speedhacks.vuThread;
		m_vu1Instant = EmuConfig.Speedhacks.vu1Instant;
		m_recVu1 = EmuConfig.Cpu.Recompiler.EnableVU1;
		m_vmState = VMManager::GetState();

		// A quiet machine: nothing pending anywhere, the IOP well ahead of the
		// EE (so no IOP slice runs), no counter due, both VUs idle.
		cpuRegs.cycle = 0x100000;
		cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
		cpuRegs.nextEventCycle = cpuRegs.cycle;
		cpuRegs.interrupt = 0;
		cpuRegs.dmastall = 0;
		cpuRegs.branch = 0;
		cpuRegs.pc = 0x00100000;
		cpuRegs.CP0.n.Status.val = 0;
		cpuRegs.CP0.n.Cause = 0;
		cpuRegs.CP0.n.EPC = 0;
		EEoCycle = cpuRegs.cycle;
		EEsCycle = -1000000;
		iopEventAction = false;
		psxRegs.interrupt = 0;
		psxHu32(HW_ICTRL) = 0;
		psHu32(INTC_STAT) = 0;
		psHu32(INTC_MASK) = 0;
		psHu32(DMAC_STAT) = 0;
		psHu32(DMAC_CTRL) = 1; // DMAE
		psHu32(DMAC_ENABLEW) = 0;
		psHu32(DMAC_ENABLER) = 0;
		counters[0].hold = kHoldSentinel;
		counters[1].hold = kHoldSentinel;
		VU0.VI[REG_VPU_STAT].UL = 0;
		VU0.VI[REG_TPC].UL = 0;
		VU1.VI[REG_TPC].UL = 0;
		VU0.cycle = cpuRegs.cycle - 500;
		VU1.cycle = cpuRegs.cycle - 500;
		LoadNopEnd(VU0.Micro);
		LoadNopEnd(VU1.Micro);
		EmuConfig.Speedhacks.vuThread = false;
		EmuConfig.Speedhacks.vu1Instant = false;
		vu1Thread.mtvuInterrupts.store(0);

		VMManager::Internal::SetStateForTests(VMState::Running);
	}

	void TearDown() override
	{
		VMManager::Internal::SetStateForTests(m_vmState);
		vu1Thread.mtvuInterrupts.store(0);
		EmuConfig.Speedhacks.vuThread = m_vuThread;
		EmuConfig.Speedhacks.vu1Instant = m_vu1Instant;
		EmuConfig.Cpu.Recompiler.EnableVU1 = m_recVu1;
		cpuRegs = *m_cpu;
		psxRegs = *m_psx;
		std::memcpy(eeHw, m_eeHw.get(), Ps2MemSize::Hardware);
		std::memcpy(iopHw, m_iopHw.get(), Ps2MemSize::IopHardware);
		std::memcpy(counters, m_counters, sizeof(counters));
		std::memcpy(VU0.VI, m_vu0Vi, sizeof(m_vu0Vi));
		std::memcpy(VU1.VI, m_vu1Vi, sizeof(m_vu1Vi));
		std::memcpy(VU0.Micro, m_vu0Micro, sizeof(m_vu0Micro));
		std::memcpy(VU1.Micro, m_vu1Micro, sizeof(m_vu1Micro));
		VU0.cycle = m_vu0Cycle;
		VU1.cycle = m_vu1Cycle;
		EEsCycle = m_eesCycle;
		EEoCycle = m_eeoCycle;
		iopEventAction = m_iopEventAction;
	}

	static void LoadNopEnd(u8* micro)
	{
		const u32 prog[] = {kVuLowerNop, kVuUpperNopE, kVuLowerNop, kVuUpperNop};
		std::memcpy(micro, prog, sizeof(prog));
	}

	bool ExceptionTaken() const { return cpuRegs.CP0.n.Status.b.EXL != 0; }

	std::unique_ptr<cpuRegisters> m_cpu;
	std::unique_ptr<psxRegisters> m_psx;
	std::unique_ptr<u8[]> m_eeHw;
	std::unique_ptr<u8[]> m_iopHw;
	Counter m_counters[4];
	REG_VI m_vu0Vi[32];
	REG_VI m_vu1Vi[32];
	u8 m_vu0Micro[16];
	u8 m_vu1Micro[16];
	u64 m_vu0Cycle = 0;
	u64 m_vu1Cycle = 0;
	s32 m_eesCycle = 0;
	u64 m_eeoCycle = 0;
	bool m_iopEventAction = false;
	bool m_vuThread = false;
	bool m_vu1Instant = false;
	bool m_recVu1 = false;
	VMState m_vmState = VMState::Shutdown;
};

} // namespace

// ---------------------------------------------------------------------------
//  Nothing pending: nothing happens
// ---------------------------------------------------------------------------

TEST_F(EeEventTestQuietPath, QuietEntryDeliversNothingAndRunsNoVu)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	_cpuEventTest_Shared();

	EXPECT_FALSE(ExceptionTaken());
	EXPECT_EQ(cpuRegs.pc, 0x00100000u);
	EXPECT_EQ(counters[0].hold, kHoldSentinel);
	EXPECT_EQ(counters[1].hold, kHoldSentinel);
	EXPECT_EQ(VU0.cycle, cpuRegs.cycle - 500);
	EXPECT_EQ(VU1.cycle, cpuRegs.cycle - 500);
	EXPECT_EQ(cpuRegs.lastEventCycle, cpuRegs.cycle);
	// The next test is scheduled in the future, never in the past.
	EXPECT_GT(cpuRegs.nextEventCycle, cpuRegs.cycle);
}

// ---------------------------------------------------------------------------
//  INTC
// ---------------------------------------------------------------------------

TEST_F(EeEventTestQuietPath, IntcPendingAndEnabledRaisesInterruptAndLatchesHold)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(INTC_STAT) = 0x2;
	psHu32(INTC_MASK) = 0x2;
	_cpuEventTest_Shared();

	EXPECT_TRUE(ExceptionTaken());
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0xFFFF, 0x400u);
	EXPECT_EQ(cpuRegs.CP0.n.EPC, 0x00100000u);
	EXPECT_EQ(cpuRegs.pc, 0x80000200u);
	EXPECT_NE(counters[0].hold, kHoldSentinel);
	EXPECT_NE(counters[1].hold, kHoldSentinel);
}

TEST_F(EeEventTestQuietPath, IntcHoldIsLatchedEvenWhenStatusBlocksTheInterrupt)
{
	// Interrupts off in Status: no exception, but intcInterrupt() still
	// latches the counter HOLD registers for a pending, unmasked bit 1.
	cpuRegs.CP0.n.Status.val = 0;
	psHu32(INTC_STAT) = 0x2;
	psHu32(INTC_MASK) = 0x2;
	_cpuEventTest_Shared();

	EXPECT_FALSE(ExceptionTaken());
	EXPECT_EQ(cpuRegs.pc, 0x00100000u);
	EXPECT_NE(counters[0].hold, kHoldSentinel);
	EXPECT_NE(counters[1].hold, kHoldSentinel);
}

TEST_F(EeEventTestQuietPath, IntcCauseWithoutMaskDoesNothing)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(INTC_STAT) = 0x2 | 0x4;
	psHu32(INTC_MASK) = 0x1;
	_cpuEventTest_Shared();

	EXPECT_FALSE(ExceptionTaken());
	EXPECT_EQ(counters[0].hold, kHoldSentinel);
	EXPECT_EQ(counters[1].hold, kHoldSentinel);
}

TEST_F(EeEventTestQuietPath, IntcOtherCauseRaisesWithoutLatchingHold)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(INTC_STAT) = 0x4;
	psHu32(INTC_MASK) = 0x4;
	_cpuEventTest_Shared();

	EXPECT_TRUE(ExceptionTaken());
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0xFFFF, 0x400u);
	EXPECT_EQ(counters[0].hold, kHoldSentinel);
}

// ---------------------------------------------------------------------------
//  DMAC
// ---------------------------------------------------------------------------

TEST_F(EeEventTestQuietPath, DmacChannelCauseWithMaskRaisesInterrupt)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(DMAC_STAT) = (1u << 2) | (1u << (16 + 2)); // CIS2 + CIM2
	_cpuEventTest_Shared();

	EXPECT_TRUE(ExceptionTaken());
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0xFFFF, 0x800u);
}

TEST_F(EeEventTestQuietPath, DmacBusErrorRaisesInterruptUnmasked)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(DMAC_STAT) = 0x8000; // BEIS
	_cpuEventTest_Shared();

	EXPECT_TRUE(ExceptionTaken());
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0xFFFF, 0x800u);
}

TEST_F(EeEventTestQuietPath, DmacCauseWithoutMaskDoesNothing)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(DMAC_STAT) = (1u << 2) | (1u << (16 + 3));
	_cpuEventTest_Shared();

	EXPECT_FALSE(ExceptionTaken());
}

TEST_F(EeEventTestQuietPath, DmacCauseIgnoredWhileDmacDisabled)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(DMAC_STAT) = (1u << 2) | (1u << (16 + 2));
	psHu32(DMAC_CTRL) = 0;
	_cpuEventTest_Shared();

	EXPECT_FALSE(ExceptionTaken());
}

TEST_F(EeEventTestQuietPath, IntcAndDmacTogetherRaiseBothCauses)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(INTC_STAT) = 0x4;
	psHu32(INTC_MASK) = 0x4;
	psHu32(DMAC_STAT) = 0x8000;
	_cpuEventTest_Shared();

	EXPECT_TRUE(ExceptionTaken());
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0xFFFF, 0xC00u);
}

// ---------------------------------------------------------------------------
//  VU sync
// ---------------------------------------------------------------------------

TEST_F(EeEventTestQuietPath, RunningVu0BehindTheEeIsRun)
{
	VU0.VI[REG_VPU_STAT].UL = 0x1;
	_cpuEventTest_Shared();

	EXPECT_NE(VU0.cycle, cpuRegs.cycle - 500);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL & 0x1, 0u) << "the NOP[E] program should have ended";
	EXPECT_EQ(VU1.cycle, cpuRegs.cycle - 500);
}

TEST_F(EeEventTestQuietPath, RunningVu1BehindTheEeIsRunWithoutMtvu)
{
	VU0.VI[REG_VPU_STAT].UL = 0x100;
	_cpuEventTest_Shared();

	EXPECT_NE(VU1.cycle, cpuRegs.cycle - 500);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL & 0x100, 0u) << "the NOP[E] program should have ended";
	EXPECT_EQ(VU0.cycle, cpuRegs.cycle - 500);
}

TEST_F(EeEventTestQuietPath, MtvuNeverRunsVu1OnTheEeThread)
{
	EmuConfig.Cpu.Recompiler.EnableVU1 = true;
	EmuConfig.Speedhacks.vuThread = true;
	ASSERT_TRUE(THREAD_VU1);
	VU0.VI[REG_VPU_STAT].UL = 0x100;
	_cpuEventTest_Shared();

	EXPECT_EQ(VU1.cycle, cpuRegs.cycle - 500);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL, 0x100u);
}

TEST_F(EeEventTestQuietPath, MtvuFlagsRaisedByTheVuThreadAreConsumed)
{
	// A T-bit stop reported by the VU thread: the event test clears the flag,
	// marks VU1 stopped on a T bit, and raises INTC cause 7.
	EmuConfig.Cpu.Recompiler.EnableVU1 = true;
	EmuConfig.Speedhacks.vuThread = true;
	ASSERT_TRUE(THREAD_VU1);
	VU0.VI[REG_VPU_STAT].UL = 0x100;
	vu1Thread.mtvuInterrupts.store(VU_Thread::InterruptFlagVUTBit);
	_cpuEventTest_Shared();

	EXPECT_EQ(vu1Thread.mtvuInterrupts.load(), 0u);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL, 0x400u);
	EXPECT_NE(psHu32(INTC_STAT) & (1u << 7), 0u);
	EXPECT_EQ(VU1.cycle, cpuRegs.cycle - 500);
}

TEST_F(EeEventTestQuietPath, MtvuWithNoFlagsLeavesEverythingAlone)
{
	EmuConfig.Cpu.Recompiler.EnableVU1 = true;
	EmuConfig.Speedhacks.vuThread = true;
	ASSERT_TRUE(THREAD_VU1);
	_cpuEventTest_Shared();

	EXPECT_EQ(vu1Thread.mtvuInterrupts.load(), 0u);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL, 0u);
	EXPECT_EQ(psHu32(INTC_STAT), 0u);
}

TEST_F(EeEventTestQuietPath, MtvuFlagsAreConsumedWhileVu1ReadsIdle)
{
	// The VU thread's flags do not wait on VPU_STAT: the busy bit is dropped
	// on the EE side by its own timer event, and a GS label, signal or finish
	// can arrive after that. A VU1 sync gated on VPU_STAT would strand them.
	EmuConfig.Cpu.Recompiler.EnableVU1 = true;
	EmuConfig.Speedhacks.vuThread = true;
	ASSERT_TRUE(THREAD_VU1);
	VU0.VI[REG_VPU_STAT].UL = 0;
	vu1Thread.mtvuInterrupts.store(VU_Thread::InterruptFlagVUTBit);
	_cpuEventTest_Shared();

	EXPECT_EQ(vu1Thread.mtvuInterrupts.load(), 0u);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL, 0x400u);
	EXPECT_NE(psHu32(INTC_STAT) & (1u << 7), 0u);
}
