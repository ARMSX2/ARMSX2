// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The event test that MTC0 Status, EI and ERET schedule 4 cycles on.
//
// These three instructions can let a pending interrupt through, so each asks
// for an event test 4 cycles later, which takes the interrupt. Both engines
// run the same C functions for them (the JIT calls the interpreter's EI, ERET
// and MTC0 Status), so the schedule is pinned once, at that level, and the
// delivery point is pinned end to end in each engine.
//
// The pins:
//  - With an unmasked INTC or DMAC cause pending that Status lets through,
//    the test is scheduled 4 cycles on, and each engine takes the interrupt
//    where it does today: the JIT at the end of the block after MTC0 Status
//    or EI and before the first instruction after ERET, the interpreter at
//    its next branch (it runs an event test at every branch).
//  - The test is also scheduled when INTC_STAT bit 1 is pending and unmasked
//    with interrupts off (intcInterrupt() latches the counter HOLD registers
//    whether or not the interrupt is taken), and when Status.IM7 is set (the
//    COP0 timer is only looked at inside event tests, and not gated on IE).
//  - Every other way a cause becomes pending schedules a test of its own, or
//    is taken in by an event test the EE runs anyway: a device raising an
//    INTC or DMAC cause, the INTC_MASK and D_STAT writes, and the flags the
//    VU1 thread posts, which the gate does not read. (The threaded IOP's
//    queue, which it does read, is pinned beside the other IOP thread tests,
//    in core_test.)
//
// The recompiler test environment runs with no VM, where the shared event test
// returns straight after its first check; the tests that need it to run mark
// the VM Running for their own duration and restore everything they touched.

#include "harness/EeRecTestHarness.h"

#include "COP0.h"
#include "Config.h"
#include "Counters.h"
#include "GS.h"
#include "Hardware.h"
#include "IopHw.h"
#include "MTVU.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "VUmicro.h"

#include <cstring>
#include <functional>
#include <memory>
#include <vector>
#include <gtest/gtest.h>

#ifdef PCSX2_RECOMPILER_TESTS
extern u64 g_twinEeEventTests;
#endif

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace COP0Interp = R5900::Interpreter::OpcodeImpl::COP0;

namespace {

constexpr u32 kStatusIE   = 1u << 0;
constexpr u32 kStatusEXL  = 1u << 1;
constexpr u32 kStatusERL  = 1u << 2;
constexpr u32 kStatusIntc = 1u << 10; // IM2: INTC
constexpr u32 kStatusDmac = 1u << 11; // IM3: DMAC
constexpr u32 kStatusIM7  = 1u << 15; // the COP0 timer
constexpr u32 kStatusEIE  = 1u << 16;
constexpr u32 kStatusAllEnabled = kStatusIE | kStatusIntc | kStatusDmac | kStatusEIE;

constexpr u32 kIntcVblankS = 1u << 2; // an INTC cause other than SBUS: no HOLD latch
constexpr u32 kIntcSbus    = 1u << 1;
constexpr u32 kIntcVu1     = 1u << 7;

constexpr u32 kCp0Status = 12;
constexpr u32 kCp0Cause  = 13;
constexpr u32 kCp0Epc    = 14;

// Saves the machine state the event test and the COP0 functions touch, sets up
// a quiet machine (nothing pending, the IOP far ahead, no counter due, both VUs
// idle, MTVU off) and puts everything back afterwards.
class QuietMachine
{
public:
	QuietMachine()
	{
		m_cpu = std::make_unique<cpuRegisters>(cpuRegs);
		m_psx = std::make_unique<psxRegisters>(psxRegs);
		m_eeHw = std::make_unique<u8[]>(Ps2MemSize::Hardware);
		std::memcpy(m_eeHw.get(), eeHw, Ps2MemSize::Hardware);
		m_iopHw = std::make_unique<u8[]>(Ps2MemSize::IopHardware);
		std::memcpy(m_iopHw.get(), iopHw, Ps2MemSize::IopHardware);
		std::memcpy(m_counters, counters, sizeof(counters));
		m_vpuStat = VU0.VI[REG_VPU_STAT].UL;
		m_eesCycle = EEsCycle;
		m_eeoCycle = EEoCycle;
		m_iopEventAction = iopEventAction;
		m_nextStart = nextStartCounter;
		m_nextDelta = nextDeltaCounter;
		m_vuThread = EmuConfig.Speedhacks.vuThread;
		m_vu1Instant = EmuConfig.Speedhacks.vu1Instant;
		m_recVu1 = EmuConfig.Cpu.Recompiler.EnableVU1;
		m_vmState = VMManager::GetState();

		cpuRegs.cycle = 0x100000;
		cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
		cpuRegs.nextEventCycle = cpuRegs.cycle + kFar;
		cpuRegs.interrupt = 0;
		cpuRegs.dmastall = 0;
		cpuRegs.branch = 0;
		cpuRegs.pc = 0x00100000;
		cpuRegs.CP0.n.Status.val = 0;
		cpuRegs.CP0.n.Cause = 0;
		cpuRegs.CP0.n.EPC = 0x00200000;
		cpuRegs.CP0.n.ErrorEPC = 0x00300000;
		EEoCycle = cpuRegs.cycle;
		EEsCycle = -1000000;
		iopEventAction = false;
		nextStartCounter = cpuRegs.cycle;
		nextDeltaCounter = 0x7FFFFFFF;
		psxRegs.interrupt = 0;
		psxHu32(HW_ICTRL) = 0;
		psHu32(INTC_STAT) = 0;
		psHu32(INTC_MASK) = 0;
		psHu32(DMAC_STAT) = 0;
		psHu32(DMAC_CTRL) = 1; // DMAE
		psHu32(DMAC_ENABLEW) = 0;
		psHu32(DMAC_ENABLER) = 0;
		VU0.VI[REG_VPU_STAT].UL = 0;
		EmuConfig.Speedhacks.vuThread = false;
		EmuConfig.Speedhacks.vu1Instant = false;
		vu1Thread.mtvuInterrupts.store(0);
	}

	~QuietMachine()
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
		VU0.VI[REG_VPU_STAT].UL = m_vpuStat;
		EEsCycle = m_eesCycle;
		EEoCycle = m_eeoCycle;
		iopEventAction = m_iopEventAction;
		nextStartCounter = m_nextStart;
		nextDeltaCounter = m_nextDelta;
	}

	// Where the next event test was before the instruction under test ran.
	static constexpr u32 kFar = 1000;

private:
	std::unique_ptr<cpuRegisters> m_cpu;
	std::unique_ptr<psxRegisters> m_psx;
	std::unique_ptr<u8[]> m_eeHw;
	std::unique_ptr<u8[]> m_iopHw;
	Counter m_counters[4];
	u32 m_vpuStat = 0;
	s32 m_eesCycle = 0;
	u64 m_eeoCycle = 0;
	bool m_iopEventAction = false;
	u64 m_nextStart = 0;
	s32 m_nextDelta = 0;
	bool m_vuThread = false;
	bool m_vu1Instant = false;
	bool m_recVu1 = false;
	VMState m_vmState = VMState::Shutdown;
};

class EeEventGate : public ::testing::Test
{
protected:
	void SetUp() override { ASSERT_TRUE(RecompilerTestEnvironment::IsReady()); m_machine = std::make_unique<QuietMachine>(); }
	void TearDown() override { m_machine.reset(); }

	// How far ahead the next event test is now.
	s64 NextTestIn() const { return static_cast<s64>(cpuRegs.nextEventCycle - cpuRegs.cycle); }
	bool TestIn4() const { return NextTestIn() == 4; }
	bool Unchanged() const { return NextTestIn() == QuietMachine::kFar; }

	// The three instructions, as both engines run them.
	static void Mtc0Status(u32 value) { WriteCP0Status(value); }
	static void Ei() { COP0Interp::EI(); }
	static void Eret() { COP0Interp::ERET(); }

	// Status values that, after each instruction, let INTC and DMAC interrupts through.
	static void EnableByMtc0() { Mtc0Status(kStatusAllEnabled); }
	static void EnableByEi() { cpuRegs.CP0.n.Status.val = kStatusAllEnabled & ~kStatusEIE; Ei(); }
	static void EnableByEret() { cpuRegs.CP0.n.Status.val = kStatusAllEnabled | kStatusEXL; Eret(); }
	static void EnableByEretFromErl() { cpuRegs.CP0.n.Status.val = kStatusAllEnabled | kStatusERL; Eret(); }

	std::unique_ptr<QuietMachine> m_machine;
};

void PendIntc(u32 bits) { psHu32(INTC_STAT) |= bits; psHu32(INTC_MASK) |= bits; }
void PendDmacChannel(u32 ch) { psHu32(DMAC_STAT) |= (1u << ch) | (1u << (16 + ch)); }

} // namespace

// ---------------------------------------------------------------------------
//  A cause pending that Status lets through: the test is 4 cycles on
// ---------------------------------------------------------------------------

TEST_F(EeEventGate, Mtc0StatusWithAnEnabledIntcCauseSchedulesTheTest)
{
	PendIntc(kIntcVblankS);
	EnableByMtc0();
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, Mtc0StatusWithAnEnabledDmacChannelCauseSchedulesTheTest)
{
	PendDmacChannel(2);
	EnableByMtc0();
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, Mtc0StatusWithADmacBusErrorSchedulesTheTest)
{
	psHu32(DMAC_STAT) = 0x8000; // BEIS, not maskable
	EnableByMtc0();
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, EiWithAnEnabledIntcCauseSchedulesTheTest)
{
	PendIntc(kIntcVblankS);
	EnableByEi();
	ASSERT_TRUE(cpuRegs.CP0.n.Status.b.EIE);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, EiWithAnEnabledDmacCauseSchedulesTheTest)
{
	PendDmacChannel(5);
	EnableByEi();
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, EretIntoAnEnabledStateWithAnIntcCauseSchedulesTheTest)
{
	PendIntc(kIntcVblankS);
	EnableByEret();
	ASSERT_EQ(cpuRegs.pc, 0x00200000u);
	ASSERT_FALSE(cpuRegs.CP0.n.Status.b.EXL);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, EretFromErlIntoAnEnabledStateWithADmacCauseSchedulesTheTest)
{
	PendDmacChannel(1);
	EnableByEretFromErl();
	ASSERT_EQ(cpuRegs.pc, 0x00300000u);
	ASSERT_FALSE(cpuRegs.CP0.n.Status.b.ERL);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, AnEarlierTestIsNeverPostponed)
{
	cpuRegs.nextEventCycle = cpuRegs.cycle + 2;
	PendIntc(kIntcVblankS);
	EnableByMtc0();
	EXPECT_EQ(NextTestIn(), 2);
}

// ---------------------------------------------------------------------------
//  What the event test does besides taking an interrupt
// ---------------------------------------------------------------------------

TEST_F(EeEventGate, Mtc0StatusWithInterruptsOffStillSchedulesTheTestForTheHoldLatch)
{
	// intcInterrupt() latches the counter HOLD registers whenever INTC_STAT
	// bit 1 is pending and some INTC cause is unmasked, whatever Status says.
	PendIntc(kIntcSbus);
	Mtc0Status(0);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, Mtc0StatusThatSetsTheTimerMaskSchedulesTheTest)
{
	// The COP0 timer is only checked inside event tests (_cpuTestTIMR), and
	// that check does not look at IE or EIE.
	Mtc0Status(kStatusIM7);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

// ---------------------------------------------------------------------------
//  The other ways a cause becomes pending schedule their own test
// ---------------------------------------------------------------------------

TEST_F(EeEventGate, ADeviceRaisingAnUnmaskedIntcCauseSchedulesTheTest)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(INTC_MASK) = kIntcVblankS;
	hwIntcIrq(2);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, ADeviceRaisingAnIntcCauseWhileInterruptsAreOffSchedulesNothing)
{
	psHu32(INTC_MASK) = kIntcVblankS;
	hwIntcIrq(2);
	EXPECT_TRUE(Unchanged()) << NextTestIn();
	// ...and the Status write that later lets it through does.
	EnableByMtc0();
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, ADeviceRaisingAnUnmaskedDmacCauseSchedulesTheTest)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(DMAC_STAT) = 1u << (16 + 2); // CIM2
	hwDmacIrq(2);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, TheGsSignalOnTheEeThreadSchedulesTheTest)
{
	// gsIrq() is how SIGNAL, FINISH and the vsync/hsync CSR bits reach the
	// INTC (GS.cpp, Gif_Unit.cpp, Counters.cpp), all on the EE thread.
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(INTC_MASK) = 1u << INTC_GS;
	gsIrq();
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, UnmaskingAPendingIntcCauseSchedulesTheTest)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(INTC_STAT) = kIntcVblankS;
	memWrite32(INTC_MASK, kIntcVblankS); // the register toggles the bits written
	ASSERT_EQ(psHu32(INTC_MASK), kIntcVblankS);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, UnmaskingAPendingDmacCauseSchedulesTheTest)
{
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(DMAC_STAT) = 1u << 2; // CIS2, masked
	memWrite32(DMAC_STAT, 1u << (16 + 2)); // upper half toggles the mask
	ASSERT_EQ(psHu32(DMAC_STAT), (1u << 2) | (1u << (16 + 2)));
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, ABusErrorSetsItsCauseSilentlyAndTheStatusWriteSeesIt)
{
	// throwBusError() sets D_STAT.BEIS without scheduling a test, so it waits
	// for whichever event test comes next. A Status write looks at the
	// register, so it finds the cause and asks for the test.
	throwBusError("test");
	ASSERT_NE(psHu32(DMAC_STAT) & 0x8000, 0u);
	EXPECT_TRUE(Unchanged()) << NextTestIn();
	EnableByMtc0();
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, AFlagTheVu1ThreadPostedIsTakenInByTheNextEventTestWhichSchedulesTheDelivery)
{
	// The VU1 thread (MTVU) never writes INTC_STAT. It posts a flag; the
	// event test applies it on the EE thread (Get_MTVUChanges), and the
	// INTC raise there schedules the test that takes the interrupt.
	EmuConfig.Cpu.Recompiler.EnableVU1 = true;
	EmuConfig.Speedhacks.vuThread = true;
	ASSERT_TRUE(THREAD_VU1);
	cpuRegs.CP0.n.Status.val = kStatusAllEnabled;
	psHu32(INTC_MASK) = kIntcVu1;
	vu1Thread.mtvuInterrupts.store(VU_Thread::InterruptFlagVUTBit);
	VMManager::Internal::SetStateForTests(VMState::Running);

	_cpuEventTest_Shared();
	EXPECT_EQ(vu1Thread.mtvuInterrupts.load(), 0u);
	EXPECT_NE(psHu32(INTC_STAT) & kIntcVu1, 0u);
	EXPECT_EQ(cpuRegs.CP0.n.Status.b.EXL, 0u) << "the INTC check ran before the VU sync";
	EXPECT_TRUE(TestIn4()) << NextTestIn();

	cpuRegs.cycle += 4;
	_cpuEventTest_Shared();
	EXPECT_EQ(cpuRegs.CP0.n.Status.b.EXL, 1u);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0xFFFF, 0x400u);
}

// ---------------------------------------------------------------------------
//  End to end: where each engine takes the interrupt
// ---------------------------------------------------------------------------

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kParkPc = RecompilerTestEnvironment::kParkingPc;
constexpr int kSled = 16; // instructions per straight-line block

std::vector<u32> Sled()
{
	return std::vector<u32>(kSled, ADDIU(reg::v1, reg::v1, 1));
}

void Append(std::vector<u32>& prog, const std::vector<u32>& more)
{
	prog.insert(prog.end(), more.begin(), more.end());
}

// What one engine did: whether it took the interrupt, where (EPC), and how
// many sled instructions had run by then ($v1).
struct Taken
{
	bool exl;
	u32 epc;
	u32 sled;
	u32 cause;
};

enum class Engine { Jit, Interp };

// Runs `prog` in one engine on a machine with the given hardware state, the VM
// marked Running so that the shared event test really runs.
class Delivery
{
public:
	Delivery(std::vector<u32> prog, u32 status, u32 epc)
		: m_prog(std::move(prog)), m_status(status), m_epc(epc) {}

	std::function<void()> pend = [] {};

	Taken Run(Engine e, u64* event_tests = nullptr)
	{
		QuietMachine machine;
		EeRecTestHarness h;
		h.LoadProgram(m_prog);
		h.SetCp0(kCp0Status, m_status);
		h.SetCp0(kCp0Epc, m_epc);
		h.SetCp0(kCp0Cause, 0);
		h.SetGpr(reg::v1, 0);
		pend();
		VMManager::Internal::SetStateForTests(VMState::Running);
#ifdef PCSX2_RECOMPILER_TESTS
		const u64 tests_before = g_twinEeEventTests;
#endif
		if (e == Engine::Jit)
			h.RunJitNoDiff();
		else
			h.RunInterpOnly();
#ifdef PCSX2_RECOMPILER_TESTS
		if (event_tests)
			*event_tests = g_twinEeEventTests - tests_before;
#endif
		const bool jit = e == Engine::Jit;
		const u32 status = jit ? h.GetCp0Jit(kCp0Status) : h.GetCp0Interp(kCp0Status);
		return Taken{(status & kStatusEXL) != 0, jit ? h.GetCp0Jit(kCp0Epc) : h.GetCp0Interp(kCp0Epc),
			jit ? h.GetGprJit(reg::v1) : h.GetGprInterp(reg::v1), jit ? h.GetCp0Jit(kCp0Cause) : h.GetCp0Interp(kCp0Cause)};
	}

private:
	std::vector<u32> m_prog;
	u32 m_status;
	u32 m_epc;
};

// block 1: a branch, so the harness's first, forced event test is out of the
// way before the instruction under test; block 2: `insn` and a sled, ended by
// a branch; block 3: another sled, then the harness's `jr ra` to the parking lot.
std::vector<u32> AroundInstruction(u32 insn)
{
	std::vector<u32> p = {BEQ(reg::zero, reg::zero, 1), NOP, insn};
	Append(p, Sled());
	p.push_back(BEQ(reg::zero, reg::zero, 1));
	p.push_back(NOP);
	Append(p, Sled());
	return p;
}
constexpr u32 kBlock3Pc = kProgPc + 4 * (3 + kSled + 2);

// EI ends the JIT block it is in; the interrupt is taken at the end of the next one.
std::vector<u32> AroundEi()
{
	std::vector<u32> p = {BEQ(reg::zero, reg::zero, 1), NOP, EI};
	Append(p, Sled());
	p.push_back(BEQ(reg::zero, reg::zero, 1));
	p.push_back(NOP);
	Append(p, Sled());
	return p;
}

} // namespace

TEST(EeEventGateDelivery, Mtc0StatusTakesAPendingIntcInterruptWhereEachEngineDoesToday)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	Delivery d(AroundInstruction(MTC0(reg::t0, kCp0Status)), 0, 0);
	d.pend = [] { cpuRegs.GPR.n.t0.UD[0] = kStatusAllEnabled; PendIntc(kIntcVblankS); };
	for (Engine e : {Engine::Jit, Engine::Interp})
	{
		const Taken t = d.Run(e);
		SCOPED_TRACE(e == Engine::Jit ? "jit" : "interp");
		EXPECT_TRUE(t.exl);
		EXPECT_EQ(t.cause & 0xFFFF, 0x400u);
		// both: at the branch that ends the block after the MTC0
		EXPECT_EQ(t.epc, kBlock3Pc);
		EXPECT_EQ(t.sled, static_cast<u32>(kSled));
	}
}

TEST(EeEventGateDelivery, Mtc0StatusTakesAPendingDmacInterruptWhereEachEngineDoesToday)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	Delivery d(AroundInstruction(MTC0(reg::t0, kCp0Status)), 0, 0);
	d.pend = [] { cpuRegs.GPR.n.t0.UD[0] = kStatusAllEnabled; PendDmacChannel(2); };
	for (Engine e : {Engine::Jit, Engine::Interp})
	{
		const Taken t = d.Run(e);
		SCOPED_TRACE(e == Engine::Jit ? "jit" : "interp");
		EXPECT_TRUE(t.exl);
		EXPECT_EQ(t.cause & 0xFFFF, 0x800u);
		EXPECT_EQ(t.epc, kBlock3Pc);
		EXPECT_EQ(t.sled, static_cast<u32>(kSled));
	}
}

TEST(EeEventGateDelivery, EiTakesAPendingIntcInterruptWhereEachEngineDoesToday)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	Delivery d(AroundEi(), kStatusAllEnabled & ~kStatusEIE, 0);
	d.pend = [] { PendIntc(kIntcVblankS); };
	for (Engine e : {Engine::Jit, Engine::Interp})
	{
		const Taken t = d.Run(e);
		SCOPED_TRACE(e == Engine::Jit ? "jit" : "interp");
		EXPECT_TRUE(t.exl);
		EXPECT_EQ(t.cause & 0xFFFF, 0x400u);
		// both: at the branch after the sled that follows EI
		EXPECT_EQ(t.epc, kBlock3Pc);
		EXPECT_EQ(t.sled, static_cast<u32>(kSled));
	}
}

TEST(EeEventGateDelivery, EretAloneTakesAPendingIntcInterruptWhereEachEngineDoesToday)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	// block 1: a branch; block 2: ERET to a sled, which ends in the harness's
	// `jr ra` to the parking lot.
	std::vector<u32> p = {BEQ(reg::zero, reg::zero, 1), NOP, ERET};
	const u32 ret = kProgPc + 4 * static_cast<u32>(p.size());
	Append(p, Sled());
	Delivery d(p, kStatusAllEnabled | kStatusEXL, ret);
	d.pend = [] { PendIntc(kIntcVblankS); };

	// Both at the branch that ends the sled: the JIT's test 4 cycles after
	// ERET is not due yet at ERET's own block end, and the interpreter runs
	// its test at every branch.
	for (Engine e : {Engine::Jit, Engine::Interp})
	{
		const Taken t = d.Run(e);
		SCOPED_TRACE(e == Engine::Jit ? "jit" : "interp");
		EXPECT_TRUE(t.exl);
		EXPECT_EQ(t.cause & 0xFFFF, 0x400u);
		EXPECT_EQ(t.epc, kParkPc);
		EXPECT_EQ(t.sled, static_cast<u32>(kSled));
	}
}

TEST(EeEventGateDelivery, EretAfterAStatusWriteInItsBlockTakesThePendingInterruptAtTheReturnAddressInTheJit)
{
	// The shape of the BIOS's exception return: the handler writes Status
	// (EXL still set, so nothing is let through yet) and returns with ERET
	// in the same block. The test the Status write asks for falls due at the
	// end of ERET's block, so the JIT takes a waiting interrupt before the
	// first instruction at the return address, as the console does. A gate
	// that dropped the Status write's test because that write does not
	// enable the interrupt would move it to the end of the next block.
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	std::vector<u32> p = {BEQ(reg::zero, reg::zero, 1), NOP, MTC0(reg::t0, kCp0Status), ADDIU(reg::v1, reg::v1, 0x100),
		ADDIU(reg::v1, reg::v1, 0x100), ADDIU(reg::v1, reg::v1, 0x100), ADDIU(reg::v1, reg::v1, 0x100), ERET};
	const u32 ret = kProgPc + 4 * static_cast<u32>(p.size());
	Append(p, Sled());
	Delivery d(p, kStatusAllEnabled | kStatusEXL, ret);
	d.pend = [] { cpuRegs.GPR.n.t0.UD[0] = kStatusAllEnabled | kStatusEXL; PendIntc(kIntcVblankS); };

	const Taken jit = d.Run(Engine::Jit);
	EXPECT_TRUE(jit.exl);
	EXPECT_EQ(jit.cause & 0xFFFF, 0x400u);
	EXPECT_EQ(jit.epc, ret);
	EXPECT_EQ(jit.sled, 0x400u);

	const Taken interp = d.Run(Engine::Interp);
	EXPECT_TRUE(interp.exl);
	EXPECT_EQ(interp.epc, kParkPc);
	EXPECT_EQ(interp.sled, 0x400u + kSled);
}

TEST(EeEventGateDelivery, Mtc0StatusThatLeavesTheCauseBlockedTakesNothing)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	Delivery d(AroundInstruction(MTC0(reg::t0, kCp0Status)), 0, 0);
	d.pend = [] { cpuRegs.GPR.n.t0.UD[0] = kStatusAllEnabled & ~kStatusEIE; PendIntc(kIntcVblankS); };
	for (Engine e : {Engine::Jit, Engine::Interp})
	{
		const Taken t = d.Run(e);
		SCOPED_TRACE(e == Engine::Jit ? "jit" : "interp");
		EXPECT_FALSE(t.exl);
		EXPECT_EQ(t.sled, static_cast<u32>(2 * kSled));
	}
}

// ---------------------------------------------------------------------------
//  The gate: with nothing an event test could act on, no test is scheduled
// ---------------------------------------------------------------------------

TEST_F(EeEventGate, Mtc0StatusWithNothingPendingSchedulesNothing)
{
	EnableByMtc0();
	EXPECT_TRUE(Unchanged()) << NextTestIn();
}

TEST_F(EeEventGate, EiWithNothingPendingSchedulesNothing)
{
	EnableByEi();
	ASSERT_TRUE(cpuRegs.CP0.n.Status.b.EIE);
	EXPECT_TRUE(Unchanged()) << NextTestIn();
}

TEST_F(EeEventGate, EretWithNothingPendingSchedulesNothing)
{
	EnableByEret();
	ASSERT_EQ(cpuRegs.pc, 0x00200000u);
	EXPECT_TRUE(Unchanged()) << NextTestIn();
}

TEST_F(EeEventGate, EretFromErlWithNothingPendingSchedulesNothing)
{
	EnableByEretFromErl();
	EXPECT_TRUE(Unchanged()) << NextTestIn();
}

TEST_F(EeEventGate, MaskedCausesAreNothingPending)
{
	psHu32(INTC_STAT) = kIntcVblankS | kIntcSbus; // no mask bit
	psHu32(INTC_MASK) = kIntcVu1;
	psHu32(DMAC_STAT) = (1u << 2) | (1u << (16 + 3)); // CIS2, CIM3
	EnableByMtc0();
	EXPECT_TRUE(Unchanged()) << NextTestIn();
	EnableByEi();
	EXPECT_TRUE(Unchanged()) << NextTestIn();
	EnableByEret();
	EXPECT_TRUE(Unchanged()) << NextTestIn();
}

TEST_F(EeEventGate, APendingCauseSchedulesTheTestWhateverTheNewStatusSays)
{
	// Not gated on Status: see EretAfterAStatusWriteInItsBlock... below.
	const u32 blocking[] = {
		0,
		kStatusAllEnabled & ~kStatusIE,
		kStatusAllEnabled & ~kStatusEIE,
		kStatusAllEnabled | kStatusEXL,
		kStatusAllEnabled | kStatusERL,
		kStatusAllEnabled & ~kStatusIntc,
	};
	for (u32 status : blocking)
	{
		SCOPED_TRACE(status);
		cpuRegs.nextEventCycle = cpuRegs.cycle + QuietMachine::kFar;
		psHu32(INTC_STAT) = kIntcVblankS;
		psHu32(INTC_MASK) = kIntcVblankS;
		Mtc0Status(status);
		EXPECT_TRUE(TestIn4()) << NextTestIn();
	}
	// The same for a DMAC cause with the DMAC bit off.
	psHu32(INTC_STAT) = 0;
	cpuRegs.nextEventCycle = cpuRegs.cycle + QuietMachine::kFar;
	PendDmacChannel(4);
	Mtc0Status(kStatusAllEnabled & ~kStatusDmac);
	EXPECT_TRUE(TestIn4()) << NextTestIn();
}

TEST_F(EeEventGate, AFlagTheVu1ThreadHasPostedDoesNotKeepTheTest)
{
	// The flag word is written by the VU1 thread whenever it gets there. A
	// schedule that read it would make an MTVU run depend on host timing
	// (two runs of the same build stopped matching when the gate read it).
	// The flags become causes only when an event test applies them, and the
	// event tests that do that are not the gate's (see
	// AFlagTheVu1ThreadPostedIsTakenInByTheNextEventTest... above).
	EmuConfig.Cpu.Recompiler.EnableVU1 = true;
	EmuConfig.Speedhacks.vuThread = true;
	ASSERT_TRUE(THREAD_VU1);
	const u32 flags[] = {VU_Thread::InterruptFlagFinish, VU_Thread::InterruptFlagSignal, VU_Thread::InterruptFlagLabel,
		VU_Thread::InterruptFlagVUEBit, VU_Thread::InterruptFlagVUTBit};
	for (u32 f : flags)
	{
		SCOPED_TRACE(f);
		vu1Thread.mtvuInterrupts.store(f);
		EnableByMtc0();
		EXPECT_TRUE(Unchanged()) << NextTestIn();
		EnableByEi();
		EXPECT_TRUE(Unchanged()) << NextTestIn();
		EnableByEret();
		EXPECT_TRUE(Unchanged()) << NextTestIn();
	}
}

TEST(EeEventGateDelivery, Mtc0StatusWithNothingPendingAddsNoEventTestInTheJit)
{
#ifndef PCSX2_RECOMPILER_TESTS
	GTEST_SKIP() << "needs the twin event-test counter";
#else
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	// The same program with the Status write and with a NOP in its place.
	Delivery with(AroundInstruction(MTC0(reg::t0, kCp0Status)), 0, 0);
	with.pend = [] { cpuRegs.GPR.n.t0.UD[0] = kStatusAllEnabled; };
	Delivery without(AroundInstruction(NOP), 0, 0);
	u64 n_with = 0, n_without = 0;
	const Taken a = with.Run(Engine::Jit, &n_with);
	const Taken b = without.Run(Engine::Jit, &n_without);
	EXPECT_FALSE(a.exl);
	EXPECT_FALSE(b.exl);
	EXPECT_EQ(a.sled, static_cast<u32>(2 * kSled));
	EXPECT_EQ(n_with, n_without);

	// The interpreter runs an event test at every branch, so the schedule
	// never decides how many it runs.
	with.Run(Engine::Interp, &n_with);
	without.Run(Engine::Interp, &n_without);
	EXPECT_EQ(n_with, n_without);
#endif
}

TEST(EeEventGateDelivery, EiAndEretWithNothingPendingAddNoEventTestInTheJit)
{
#ifndef PCSX2_RECOMPILER_TESTS
	GTEST_SKIP() << "needs the twin event-test counter";
#else
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	// EI ends its JIT block either way, so the NOP control is an unconditional branch to the next instruction.
	Delivery ei(AroundEi(), kStatusAllEnabled & ~kStatusEIE, 0);
	std::vector<u32> nb = AroundEi();
	nb[2] = BEQ(reg::zero, reg::zero, 1);
	nb.insert(nb.begin() + 3, NOP);
	nb.erase(nb.begin() + 4); // keep the sled length
	Delivery noei(nb, kStatusAllEnabled & ~kStatusEIE, 0);
	u64 n_ei = 0, n_noei = 0;
	ei.Run(Engine::Jit, &n_ei);
	noei.Run(Engine::Jit, &n_noei);
	EXPECT_EQ(n_ei, n_noei);

	// ERET into the sled against a jump into the same sled.
	std::vector<u32> p = {BEQ(reg::zero, reg::zero, 1), NOP, ERET};
	const u32 ret = kProgPc + 4 * static_cast<u32>(p.size() + 1);
	p.push_back(NOP);
	Append(p, Sled());
	std::vector<u32> q = p;
	q[2] = BEQ(reg::zero, reg::zero, 1); // to ret, with q[3] as its delay slot
	Delivery eret(p, kStatusAllEnabled | kStatusEXL, ret);
	Delivery noeret(q, kStatusAllEnabled | kStatusEXL, ret);
	u64 n_eret = 0, n_noeret = 0;
	const Taken a = eret.Run(Engine::Jit, &n_eret);
	const Taken b = noeret.Run(Engine::Jit, &n_noeret);
	EXPECT_EQ(a.sled, static_cast<u32>(kSled));
	EXPECT_EQ(b.sled, static_cast<u32>(kSled));
	EXPECT_EQ(n_eret, n_noeret);
#endif
}
