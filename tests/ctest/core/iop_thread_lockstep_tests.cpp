// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP thread as the VM uses it in Lockstep: the EE event test compiled once per mode, the
// handoff that runs the IOP's slice on the IOP thread while the EE waits, the holds the lifecycle
// takes, and the runtime flag that keeps the recompiler test harness out of it.
//
// Lockstep must leave the machine exactly where Off would. The tests check that on a small
// scripted machine: a fake IOP CPU that records which thread ran it and what it was asked to do.
//
// The event test returns at once unless the VM is Running, and without a VM the only way to say
// that is VMManager::Internal::SetStateForTests, which exists only in builds with the recompiler
// test hooks. Tests that run the event test skip themselves in a build without them.

#include "Common.h"
#include "Config.h"
#include "Counters.h"
#include "IopHw.h"
#include "IopThread/IopThreadMode.h"
#include "IopThread/IopThreadVm.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "VUmicro.h"

#include "common/FPControl.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

extern s32 nextDeltaCounter;
extern u64 nextStartCounter;
extern s32 psxNextDeltaCounter;
extern u64 psxNextStartCounter;
#ifdef PCSX2_RECOMPILER_TESTS
extern u64 g_twinIopSlices;
extern u64 g_twinEeEventTests;
#endif

namespace
{
	using IopThread::HostCpuInfo;
	using IopThread::ResolveMode;

	HostCpuInfo Arm64Host()
	{
		HostCpuInfo host;
		host.is_arm64 = true;
		host.total_cores = 8;
		host.performance_cores = 4;
		return host;
	}

	class SettingsGuard
	{
	public:
		SettingsGuard()
			: m_iop_thread(EmuConfig.Speedhacks.iopThread)
			, m_lockstep(EmuConfig.Speedhacks.iopThreadLockstep)
			, m_fpcr(EmuConfig.Cpu.FPUFPCR)
		{
		}
		~SettingsGuard()
		{
			IopThread::StopThread();
			EmuConfig.Speedhacks.iopThread = m_iop_thread;
			EmuConfig.Speedhacks.iopThreadLockstep = m_lockstep;
			EmuConfig.Cpu.FPUFPCR = m_fpcr;
			IopThread::ChooseMode();
		}

	private:
		bool m_iop_thread, m_lockstep;
		FPControlRegister m_fpcr;
	};

	void AskForLockstep()
	{
		EmuConfig.Speedhacks.iopThread = true;
		EmuConfig.Speedhacks.iopThreadLockstep = true;
	}
} // namespace

// Lockstep resolves on ARM64 only (the x86 IOP JIT is not audited for a second thread). A test that
// needs the thread to exist says so first.
#define REQUIRE_ARM64_HOST() \
	do \
	{ \
		if (!IopThread::CurrentHostInfo().is_arm64) \
			GTEST_SKIP() << "Lockstep resolves to Off on a host that is not ARM64"; \
	} while (0)

TEST(IopThreadHarnessFlag, IsOffUnlessTheGtestEnvironmentSetsIt)
{
	// This binary may be built with PCSX2_RECOMPILER_TESTS, as eerunner is. That define must not be
	// what turns the condition on: eerunner has to be able to run Lockstep. Only a call to
	// SetRecompilerTestHarness() does, and only the gtest recompiler environment makes it.
	EXPECT_FALSE(IopThread::IsRecompilerTestHarness());
	EXPECT_FALSE(IopThread::CurrentVmConditions().recompiler_test_harness);
}

TEST(IopThreadHarnessFlag, ReachesTheVmConditionsAndForcesOff)
{
	SettingsGuard guard;
	AskForLockstep();
	const HostCpuInfo host = Arm64Host();

	EXPECT_EQ(ResolveMode(EmuConfig, host, IopThread::CurrentVmConditions()), IopThreadMode::Lockstep);

	IopThread::SetRecompilerTestHarness(true);
	EXPECT_TRUE(IopThread::IsRecompilerTestHarness());
	EXPECT_TRUE(IopThread::CurrentVmConditions().recompiler_test_harness);
	EXPECT_EQ(ResolveMode(EmuConfig, host, IopThread::CurrentVmConditions()), IopThreadMode::Off);

	IopThread::SetRecompilerTestHarness(false);
	EXPECT_FALSE(IopThread::CurrentVmConditions().recompiler_test_harness);
	EXPECT_EQ(ResolveMode(EmuConfig, host, IopThread::CurrentVmConditions()), IopThreadMode::Lockstep);
}

TEST(IopThreadHarnessFlag, ChooseModeHonoursIt)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	AskForLockstep();
	IopThread::SetRecompilerTestHarness(true);
	IopThread::ChooseMode();
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);

	IopThread::SetRecompilerTestHarness(false);
	IopThread::ChooseMode();
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Lockstep);
}

TEST(IopThreadEventTest, EachModeHasItsOwnInstanceAndOffKeepsTheOriginal)
{
	const CpuEventTestFn off = cpuEventTestFor(IopThreadMode::Off);
	const CpuEventTestFn lockstep = cpuEventTestFor(IopThreadMode::Lockstep);
	ASSERT_NE(off, nullptr);
	ASSERT_NE(lockstep, nullptr);
	EXPECT_NE(off, lockstep);
	EXPECT_EQ(off, &_cpuEventTest_Shared<IopThreadMode::Off>);
	EXPECT_EQ(lockstep, &_cpuEventTest_Shared<IopThreadMode::Lockstep>);
	EXPECT_EQ(off, cpuEventTestFor(IopThreadMode::Off));
	EXPECT_EQ(lockstep, cpuEventTestFor(IopThreadMode::Lockstep));
}

TEST(IopThreadEventTest, ThreadedHasItsOwnInstance)
{
	const CpuEventTestFn threaded = cpuEventTestFor(IopThreadMode::Threaded);
	ASSERT_NE(threaded, nullptr);
	EXPECT_NE(threaded, cpuEventTestFor(IopThreadMode::Off));
	EXPECT_NE(threaded, cpuEventTestFor(IopThreadMode::Lockstep));
	EXPECT_EQ(threaded, &_cpuEventTest_Shared<IopThreadMode::Threaded>);
}

TEST(IopThreadEventTest, TheInterpreterCallsTheInstanceOfTheModeInEffect)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	EXPECT_EQ(g_intEventTestShared, cpuEventTestFor(IopThreadMode::Off));

	AskForLockstep();
	IopThread::ChooseMode();
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Lockstep);
	EXPECT_EQ(g_intEventTestShared, cpuEventTestFor(IopThreadMode::Lockstep));

	EmuConfig.Speedhacks.iopThread = false;
	IopThread::ChooseMode();
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	EXPECT_EQ(g_intEventTestShared, cpuEventTestFor(IopThreadMode::Off));
}

TEST(IopThreadEventTest, ThreadedResolvesToThreadedOnAHostWithCoresToSpare)
{
	SettingsGuard guard;
	EmuConfig.Speedhacks.iopThread = true;
	EmuConfig.Speedhacks.iopThreadLockstep = false;
	ASSERT_EQ(ResolveMode(EmuConfig, Arm64Host(), IopThread::VmConditions()), IopThreadMode::Threaded);
	IopThread::ChooseModeFor(Arm64Host(), IopThread::VmConditions());
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Threaded);
	EXPECT_EQ(g_intEventTestShared, cpuEventTestFor(IopThreadMode::Threaded));

	EmuConfig.Speedhacks.iopThreadLockstep = true;
	IopThread::ChooseModeFor(Arm64Host(), IopThread::VmConditions());
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Lockstep);
}

TEST(IopThreadEventTest, ThreadedFallsBackToOffOnAHostWithNoSpareBigCore)
{
	SettingsGuard guard;
	EmuConfig.Speedhacks.iopThread = true;
	EmuConfig.Speedhacks.iopThreadLockstep = false;
	HostCpuInfo host = Arm64Host();
	host.performance_cores = 2; // the EE, the GS and the IOP need three
	IopThread::ChooseModeFor(host, IopThread::VmConditions());
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	EXPECT_EQ(g_intEventTestShared, cpuEventTestFor(IopThreadMode::Off));

	// The conditions that force Off do so for Threaded as well.
	IopThread::VmConditions ps1;
	ps1.ps1_mode = true;
	IopThread::ChooseModeFor(Arm64Host(), ps1);
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	IopThread::VmConditions arcade;
	arcade.arcade = true;
	IopThread::ChooseModeFor(Arm64Host(), arcade);
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
}

TEST(IopThreadEventTest, TheHostInfoCountsThePerformanceCoresTheVmManagerFinds)
{
	const IopThread::HostCpuInfo host = IopThread::CurrentHostInfo();
	EXPECT_EQ(host.performance_cores, VMManager::Internal::GetPerformanceCoreCount());
	EXPECT_EQ(host.total_cores, std::thread::hardware_concurrency());
}

TEST(IopThreadEventTest, AHostThatIsNotArm64ResolvesTheSettingsToOff)
{
	if (IopThread::CurrentHostInfo().is_arm64)
		GTEST_SKIP() << "this host is ARM64";
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	IopThread::SyncThread();
	EXPECT_FALSE(IopThread::g_thread.IsRunning());
}

TEST(IopThreadVm, TheThreadStartsAndStopsWithTheMode)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	EXPECT_FALSE(IopThread::g_thread.IsRunning());

	AskForLockstep();
	IopThread::ChooseMode();
	// Choosing does not start anything: the recompilers are reset in between.
	EXPECT_FALSE(IopThread::g_thread.IsRunning());
	IopThread::SyncThread();
	EXPECT_TRUE(IopThread::g_thread.IsRunning());
	EXPECT_EQ(IopThread::g_thread.Mode(), IopThreadMode::Lockstep);
	EXPECT_TRUE(IopThread::LockstepActive());

	IopThread::SyncThread();
	EXPECT_TRUE(IopThread::g_thread.IsRunning());

	EmuConfig.Speedhacks.iopThread = false;
	IopThread::ChooseMode();
	IopThread::SyncThread();
	EXPECT_FALSE(IopThread::g_thread.IsRunning());
	EXPECT_FALSE(IopThread::LockstepActive());
}

TEST(IopThreadVm, StopThreadTurnsTheModeOffToo)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::g_thread.IsRunning());

	IopThread::StopThread();
	EXPECT_FALSE(IopThread::g_thread.IsRunning());
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	EXPECT_EQ(g_intEventTestShared, cpuEventTestFor(IopThreadMode::Off));
}

TEST(IopThreadVm, TheIopThreadRunsWithTheEesFloatingPointControlRegister)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	FPControlRegister wanted = FPControlRegister::GetDefault();
	wanted.SetDenormalsAreZero(true).SetFlushToZero(true);
	EmuConfig.Cpu.FPUFPCR = wanted;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::g_thread.IsRunning());

	// A host command runs on the IOP thread at the start of a handoff; ask it what its FPCR is. The
	// apply-items handoff does that without running an IOP slice, which this test has no machine for.
	FPControlRegister seen = FPControlRegister::GetDefault();
	const auto ticket = IopThread::g_thread.PostHostCommand([&seen] { seen = FPControlRegister::GetCurrent(); });
	IopThread::g_thread.LockstepApplyUpTo(0);
	ASSERT_TRUE(IopThread::g_thread.IsHostCommandDone(ticket));
	EXPECT_EQ(seen.bitmask, wanted.bitmask);
}

TEST(IopThreadVm, ANewFpcrReachesTheRunningThread)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::g_thread.IsRunning());

	FPControlRegister wanted = FPControlRegister::GetDefault();
	wanted.SetDenormalsAreZero(true);
	EmuConfig.Cpu.FPUFPCR = wanted;
	IopThread::OnCpuConfigChanged();

	FPControlRegister seen = FPControlRegister::GetDefault();
	const auto ticket = IopThread::g_thread.PostHostCommand([&seen] { seen = FPControlRegister::GetCurrent(); });
	IopThread::g_thread.LockstepApplyUpTo(0);
	ASSERT_TRUE(IopThread::g_thread.IsHostCommandDone(ticket));
	EXPECT_EQ(seen.bitmask, wanted.bitmask);
}

TEST(IopThreadVm, HoldsInLockstepAreMarkersThatNest)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::g_thread.IsRunning());
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
	{
		IopThread::VmHold outer;
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
		{
			IopThread::VmHold inner;
			EXPECT_EQ(IopThread::g_thread.HoldDepth(), 2u);
		}
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
	}
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
}

TEST(IopThreadVm, APausedVmKeepsAHoldUntilItResumes)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::g_thread.IsRunning());

	IopThread::OnVmPaused(true);
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
	IopThread::OnVmPaused(true);
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
	{
		// A save state taken during the pause nests inside it.
		IopThread::VmHold save;
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 2u);
	}
	IopThread::OnVmPaused(false);
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
	IopThread::OnVmPaused(false);
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
}

TEST(IopThreadVm, StoppingTheThreadReleasesThePauseHold)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	IopThread::OnVmPaused(true);
	ASSERT_EQ(IopThread::g_thread.HoldDepth(), 1u);

	IopThread::StopThread();
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
}

#ifdef PCSX2_RECOMPILER_TESTS
namespace
{
	// What the fake IOP CPU saw: which thread ran it, with what budget.
	struct IopCall
	{
		std::thread::id thread;
		s32 budget;
	};

	std::vector<IopCall>* s_calls = nullptr;
	bool s_pause_in_slice = false;

	// Runs the whole budget and a bit more, like the JIT finishing its last block, and gives the
	// leftover (negative when it overshot) back, as ExecuteBlock does.
	s32 FakeExecuteBlock(s32 ee_cycles)
	{
		if (s_calls)
			s_calls->push_back({std::this_thread::get_id(), ee_cycles});
		if (s_pause_in_slice)
		{
			s_pause_in_slice = false;
			IopThread::RequestPause();
		}
		const s32 overshoot = (ee_cycles > 0) ? (ee_cycles % 23) : 0;
		return ee_cycles > 0 ? -overshoot : ee_cycles;
	}

	void FakeClear(u32, u32) {}

	R3000Acpu s_fake_psx = {nullptr, nullptr, FakeExecuteBlock, FakeClear, nullptr};

	// The machine an event test needs, with the IOP replaced by the fake: quiet counters on both
	// sides, no pending interrupt, both VUs idle and their pointers valid.
	class LockstepMachine : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_cpu = std::make_unique<cpuRegisters>(cpuRegs);
			m_psx = std::make_unique<psxRegisters>(psxRegs);
			m_ee_hw = std::make_unique<u8[]>(Ps2MemSize::Hardware);
			std::memcpy(m_ee_hw.get(), eeHw, Ps2MemSize::Hardware);
			m_iop_hw = std::make_unique<u8[]>(Ps2MemSize::IopHardware);
			std::memcpy(m_iop_hw.get(), iopHw, Ps2MemSize::IopHardware);
			m_ees = EEsCycle;
			m_eeo = EEoCycle;
			m_action = iopEventAction;
			m_psx_cpu = psxCpu;
			m_vu0 = CpuVU0;
			m_vu1 = CpuVU1;
			m_vu_thread = EmuConfig.Speedhacks.vuThread;
			m_next_delta = nextDeltaCounter;
			m_next_start = nextStartCounter;
			m_psx_next_delta = psxNextDeltaCounter;
			m_psx_next_start = psxNextStartCounter;
			m_state = VMManager::GetState();
			m_slices = g_twinIopSlices;
			m_tests = g_twinEeEventTests;
			m_vpu0 = VU0.VI[REG_VPU_STAT].UL;
			m_vpu1 = VU1.VI[REG_VPU_STAT].UL;

			psxCpu = &s_fake_psx;
			CpuVU0 = &CpuIntVU0;
			CpuVU1 = &CpuIntVU1;
			EmuConfig.Speedhacks.vuThread = false;
			VU0.VI[REG_VPU_STAT].UL = 0;
			VU1.VI[REG_VPU_STAT].UL = 0;
			nextDeltaCounter = 0x7FFFFFFF;
			nextStartCounter = 0;
			psxNextDeltaCounter = 0x7FFFFFFF;
			psxNextStartCounter = 0;
			VMManager::Internal::SetStateForTests(VMState::Running);
		}

		void TearDown() override
		{
			IopThread::StopThread();
			VMManager::Internal::SetStateForTests(m_state);
			cpuRegs = *m_cpu;
			psxRegs = *m_psx;
			std::memcpy(eeHw, m_ee_hw.get(), Ps2MemSize::Hardware);
			std::memcpy(iopHw, m_iop_hw.get(), Ps2MemSize::IopHardware);
			EEsCycle = m_ees;
			EEoCycle = m_eeo;
			iopEventAction = m_action;
			psxCpu = m_psx_cpu;
			CpuVU0 = m_vu0;
			CpuVU1 = m_vu1;
			EmuConfig.Speedhacks.vuThread = m_vu_thread;
			nextDeltaCounter = m_next_delta;
			nextStartCounter = m_next_start;
			psxNextDeltaCounter = m_psx_next_delta;
			psxNextStartCounter = m_psx_next_start;
			g_twinIopSlices = m_slices;
			g_twinEeEventTests = m_tests;
			VU0.VI[REG_VPU_STAT].UL = m_vpu0;
			VU1.VI[REG_VPU_STAT].UL = m_vpu1;
			s_calls = nullptr;
			s_pause_in_slice = false;
		}

		// The same starting point every time: a quiet machine with the IOP a little behind.
		void Reset()
		{
			std::memset(&cpuRegs, 0, sizeof(cpuRegs));
			std::memset(&psxRegs, 0, sizeof(psxRegs));
			std::memset(eeHw, 0, Ps2MemSize::Hardware);
			std::memset(iopHw, 0, Ps2MemSize::IopHardware);
			cpuRegs.cycle = 0x100000;
			cpuRegs.nextEventCycle = cpuRegs.cycle;
			cpuRegs.CP0.n.Status.val = 0;
			psxRegs.cycle = 0x1000;
			psxRegs.iopCycleEE = -1;
			EEoCycle = cpuRegs.cycle;
			EEsCycle = -500;
			iopEventAction = false;
			g_twinIopSlices = 0;
			g_twinEeEventTests = 0;
		}

		struct After
		{
			s32 ees;
			u64 eeo;
			bool action;
			u64 next_event;
			u64 iop_next_event;
			bool operator==(const After& o) const
			{
				return ees == o.ees && eeo == o.eeo && action == o.action && next_event == o.next_event &&
					   iop_next_event == o.iop_next_event;
			}
		};

		struct Run
		{
			std::vector<After> after;
			std::vector<IopCall> calls;
			u64 slices = 0;
			u64 event_tests = 0;
		};

		Run RunScript(IopThreadMode mode)
		{
			Reset();
			Run run;
			s_calls = &run.calls;
			const CpuEventTestFn test = cpuEventTestFor(mode);
			static constexpr u32 kSteps[] = {1, 40, 3072, 17, 5000, 300, 3072, 3072, 8, 90, 2500, 1};
			for (u32 i = 0; i < 240; i++)
			{
				cpuRegs.cycle += kSteps[i % std::size(kSteps)];
				if (i % 7 == 3)
					iopEventAction = true;
				test();
				run.after.push_back({EEsCycle, EEoCycle, iopEventAction, cpuRegs.nextEventCycle, psxRegs.iopNextEventCycle});
			}
			s_calls = nullptr;
			run.slices = g_twinIopSlices;
			run.event_tests = g_twinEeEventTests;
			return run;
		}

		std::unique_ptr<cpuRegisters> m_cpu;
		std::unique_ptr<psxRegisters> m_psx;
		std::unique_ptr<u8[]> m_ee_hw, m_iop_hw;
		s32 m_ees;
		u64 m_eeo;
		bool m_action;
		R3000Acpu* m_psx_cpu;
		BaseVUmicroCPU* m_vu0;
		BaseVUmicroCPU* m_vu1;
		bool m_vu_thread;
		s32 m_next_delta, m_psx_next_delta;
		u64 m_next_start, m_psx_next_start;
		VMState m_state;
		u64 m_slices, m_tests;
		u32 m_vpu0, m_vpu1;
	};
} // namespace

TEST_F(LockstepMachine, TheOffInstanceRunsTheIopOnTheCallingThread)
{
	const Run run = RunScript(IopThreadMode::Off);
	ASSERT_FALSE(run.calls.empty());
	for (const IopCall& call : run.calls)
		EXPECT_EQ(call.thread, std::this_thread::get_id());
}

TEST_F(LockstepMachine, TheLockstepInstanceRunsTheIopOnTheIopThreadAndTheEeWaitsForIt)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());

	const Run run = RunScript(IopThreadMode::Lockstep);
	ASSERT_FALSE(run.calls.empty());
	// One thread ran every slice, it is not this one, and it is the one the object started.
	const std::thread::id iop = run.calls.front().thread;
	EXPECT_NE(iop, std::this_thread::get_id());
	for (const IopCall& call : run.calls)
		EXPECT_EQ(call.thread, iop);
	// The EE waited: every slice's effect is in the books by the time the event test returned
	// (checked below against Off), and the handoff count is the event test count.
	EXPECT_EQ(IopThread::g_thread.GetStats().handoffs, run.event_tests);
}

TEST_F(LockstepMachine, LockstepLeavesTheMachineWhereOffDoes)
{
	REQUIRE_ARM64_HOST();
	const Run off = RunScript(IopThreadMode::Off);

	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	const Run lockstep = RunScript(IopThreadMode::Lockstep);

	ASSERT_EQ(off.after.size(), lockstep.after.size());
	for (size_t i = 0; i < off.after.size(); i++)
		EXPECT_TRUE(off.after[i] == lockstep.after[i]) << "after event test " << i;

	// The IOP is asked the same questions in the same order.
	ASSERT_EQ(off.calls.size(), lockstep.calls.size());
	for (size_t i = 0; i < off.calls.size(); i++)
		EXPECT_EQ(off.calls[i].budget, lockstep.calls[i].budget) << "slice " << i;

	// And the twin counters, which the whole-machine fingerprint mixes in, match: they are counted
	// on the EE side, once per slice that ran.
	EXPECT_EQ(off.slices, lockstep.slices);
	EXPECT_EQ(off.event_tests, lockstep.event_tests);
	EXPECT_GT(off.slices, 0u);
}

TEST_F(LockstepMachine, TheIopIsHandedWhatTheEeOwesItAsItsBudgetInBothModes)
{
	// Concrete values, so that a fault in the books the two instances share cannot hide by being the
	// same fault in both: the IOP is 500 cycles ahead (EEsCycle -500), the EE runs 3072 cycles, so
	// the IOP is owed 2572; the fake runs them and overshoots by 2572 % 23 = 19.
	for (const IopThreadMode mode : {IopThreadMode::Off, IopThreadMode::Lockstep})
	{
		SettingsGuard guard;
		if (mode == IopThreadMode::Lockstep)
		{
			REQUIRE_ARM64_HOST();
			AskForLockstep();
			IopThread::ChooseMode();
			IopThread::SyncThread();
			ASSERT_TRUE(IopThread::LockstepActive());
		}

		Reset();
		std::vector<IopCall> calls;
		s_calls = &calls;
		cpuRegs.cycle += 3072;
		cpuEventTestFor(mode)();
		s_calls = nullptr;

		ASSERT_EQ(calls.size(), 1u) << "mode " << static_cast<int>(mode);
		EXPECT_EQ(calls[0].budget, 2572) << "mode " << static_cast<int>(mode);
		EXPECT_EQ(EEsCycle, -19) << "mode " << static_cast<int>(mode);
		EXPECT_EQ(EEoCycle, cpuRegs.cycle) << "mode " << static_cast<int>(mode);
		EXPECT_FALSE(iopEventAction) << "mode " << static_cast<int>(mode);
	}
}

TEST_F(LockstepMachine, ThePlainNameTheX86RecompilersCallIsTheOffInstance)
{
	// _cpuEventTest_Shared() with no template argument is what the x86 recompilers (left untouched, to stay
	// close to upstream) call. It must do exactly what the Off instance does, on the calling thread.
	const Run off = RunScript(IopThreadMode::Off);

	Reset();
	Run plain;
	s_calls = &plain.calls;
	const CpuEventTestFn test = static_cast<void (*)()>(&_cpuEventTest_Shared);
	static constexpr u32 kSteps[] = {1, 40, 3072, 17, 5000, 300, 3072, 3072, 8, 90, 2500, 1};
	for (u32 i = 0; i < 240; i++)
	{
		cpuRegs.cycle += kSteps[i % std::size(kSteps)];
		if (i % 7 == 3)
			iopEventAction = true;
		test();
		plain.after.push_back({EEsCycle, EEoCycle, iopEventAction, cpuRegs.nextEventCycle, psxRegs.iopNextEventCycle});
	}
	s_calls = nullptr;

	ASSERT_EQ(off.after.size(), plain.after.size());
	for (size_t i = 0; i < off.after.size(); i++)
		EXPECT_TRUE(off.after[i] == plain.after[i]) << "after event test " << i;
	ASSERT_EQ(off.calls.size(), plain.calls.size());
	for (const IopCall& call : plain.calls)
		EXPECT_EQ(call.thread, std::this_thread::get_id());
}

TEST_F(LockstepMachine, TheThreadsViewOfTheIopPositionFollowsTheBooksPastTwoToThe32)
{
	REQUIRE_ARM64_HOST();
	// The EE's cycle counter is 64 bits. The IOP position the thread object publishes is the EE's cycle less the
	// IOP time still owed (EEsCycle), and must keep following it where the low 32 bits wrap.
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	Reset();
	cpuRegs.cycle = 0xFFFFF000ull;
	EEoCycle = cpuRegs.cycle;
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	EXPECT_EQ(IopThread::g_thread.PublishedPos(), cpuRegs.cycle - static_cast<s64>(EEsCycle));

	const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);
	for (u32 i = 0; i < 40; i++)
	{
		cpuRegs.cycle += 3072;
		test();
		const u64 expected = cpuRegs.cycle - static_cast<s64>(EEsCycle);
		ASSERT_EQ(IopThread::g_thread.PublishedPos(), expected) << "after event test " << i << ", EE cycle " << cpuRegs.cycle;
	}
	EXPECT_GT(cpuRegs.cycle, 0x100000000ull); // the run did cross 2^32

	// A reset or a state load moves the books by itself and tells the thread: the position goes back with them.
	cpuRegs.cycle = 0x1000;
	EEsCycle = -40;
	EEoCycle = cpuRegs.cycle;
	{
		IopThread::VmHold hold;
		IopThread::ResyncClocks();
	}
	EXPECT_EQ(IopThread::g_thread.PublishedPos(), 0x1000u + 40u);
}

TEST_F(LockstepMachine, TheLockstepInstanceWithNoThreadRunsTheSliceItself)
{
	// A Lockstep dispatcher can outlive the thread for a moment (a mode switch, a shutdown). The
	// instance then does what Off does.
	ASSERT_FALSE(IopThread::LockstepActive());
	const Run off = RunScript(IopThreadMode::Off);
	const Run alone = RunScript(IopThreadMode::Lockstep);
	ASSERT_EQ(off.after.size(), alone.after.size());
	for (size_t i = 0; i < off.after.size(); i++)
		EXPECT_TRUE(off.after[i] == alone.after[i]) << "after event test " << i;
	for (const IopCall& call : alone.calls)
		EXPECT_EQ(call.thread, std::this_thread::get_id());
}

TEST_F(LockstepMachine, APauseTheIopAsksForIsNotMadeOnTheIopThread)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	AskForLockstep();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	Reset();
	iopEventAction = true;

	// The IOP code (an unrecognised opcode, a breakpoint) asks the VM to pause from inside its slice.
	// SetPaused and ExitExecution belong to the CPU thread, so the request waits for the EE.
	s_pause_in_slice = true;
	IopThread::g_thread.LockstepRunSlice(0);
	EXPECT_EQ(VMManager::GetState(), VMState::Running);
	EXPECT_TRUE(IopThread::DeferredPausePending());
	IopThread::ClearDeferredPause();
	EXPECT_FALSE(IopThread::DeferredPausePending());
}
#else
TEST(IopThreadLockstepMachine, NeedsABuildWithTheRecompilerTestHooks)
{
	GTEST_SKIP() << "the event test cannot be run without a VM unless the build has the recompiler test hooks";
}
#endif
