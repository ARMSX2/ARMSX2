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
#include "IopThread/IopBridge.h"
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
	u64 s_iop_ran = 0; // EE cycles the fake IOP has run, overshoot included

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
		if (ee_cycles > 0)
			s_iop_ran += static_cast<u64>(ee_cycles) + overshoot;
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

namespace
{
	// A skew is a testing knob that outlives the thread, so a test puts it back.
	class SkewGuard
	{
	public:
		~SkewGuard()
		{
			IopThread::SetLockstepSkew(0);
			IopThread::SetLockstepSkewHolds(false);
		}
	};

	// What the skewed IOP's clock was when an item was applied: where it started plus what the fake has run.
	u64 s_probe_start = 0;
	struct Probe
	{
		const char* what;
		u32 arg;
		u64 iop_pos;
		std::thread::id thread;
	};
	std::vector<Probe>* s_probes = nullptr;

	void ProbeItem(const char* what, u32 arg = 0)
	{
		if (s_probes)
			s_probes->push_back({what, arg, s_probe_start + s_iop_ran, std::this_thread::get_id()});
	}

	const IopThread::E2IActions kProbeActions = {
		[] { ProbeItem("hblank_start"); },
		[] { ProbeItem("hblank_end"); },
		[] { ProbeItem("vblank_start"); },
		[] { ProbeItem("vblank_end"); },
		[](u32 line) { ProbeItem("iop_irq", line); },
		[](u32 job) { ProbeItem("job", job); },
		[](u32 channel) { ProbeItem("sif_pump", channel); },
	};

	class ProbeGuard
	{
	public:
		explicit ProbeGuard(std::vector<Probe>& probes, u64 start)
		{
			s_probes = &probes;
			s_probe_start = start;
			m_previous = IopThread::SetE2IActionsForTests(&kProbeActions);
		}
		~ProbeGuard()
		{
			IopThread::SetE2IActionsForTests(m_previous);
			s_probes = nullptr;
		}

	private:
		const IopThread::E2IActions* m_previous;
	};
} // namespace

// The skewed handoff: the IOP is kept D cycles behind the EE, items are applied when the IOP's clock reaches their
// stamp, and the EE's books follow the IOP's position. The fake IOP overshoots every slice by up to 22 cycles.
TEST_F(LockstepMachine, ASkewKeepsTheIopThatManyCyclesBehindTheEeAtEveryHandoff)
{
	REQUIRE_ARM64_HOST();
	constexpr s64 kSkew = 100000;
	SettingsGuard guard;
	SkewGuard skew_guard;
	AskForLockstep();
	IopThread::SetLockstepSkew(kSkew);
	Reset();
	const u64 start = static_cast<u64>(cpuRegs.cycle) - static_cast<u64>(static_cast<s64>(EEsCycle)); // 500 ahead of the EE
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	ASSERT_EQ(IopThread::g_thread.LockstepSkew(), kSkew);
	ASSERT_EQ(IopThread::LockstepSkewCycles(), kSkew);

	std::vector<IopCall> calls;
	s_calls = &calls;
	s_iop_ran = 0;
	const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);
	u32 handoffs_with_budget = 0;
	for (u32 i = 0; i < 400; i++)
	{
		const size_t calls_before = calls.size();
		cpuRegs.cycle += 3072;
		test();
		const s64 behind = static_cast<s64>(cpuRegs.cycle - IopThread::g_thread.PublishedPos());
		if (static_cast<s64>(cpuRegs.cycle) - static_cast<s64>(start) < kSkew)
		{
			// The EE has not yet run a skew's worth from where the IOP started: the IOP has nothing to run.
			EXPECT_EQ(calls.size(), 0u) << "handoff " << i;
			EXPECT_EQ(IopThread::g_thread.PublishedPos(), start) << "handoff " << i;
		}
		else
		{
			// At the goal, or past it by the overshoot of its last block (22 at most); never short of it.
			EXPECT_GE(behind, kSkew - 22) << "handoff " << i;
			EXPECT_LE(behind, kSkew) << "handoff " << i;
		}
		if (calls.size() != calls_before)
			handoffs_with_budget++;
		// The books say the same: the IOP is owed what the EE has run beyond it.
		EXPECT_EQ(EEsCycle, static_cast<s32>(cpuRegs.cycle - IopThread::g_thread.PublishedPos())) << "handoff " << i;
		EXPECT_EQ(EEoCycle, cpuRegs.cycle) << "handoff " << i;
	}
	s_calls = nullptr;
	EXPECT_GT(handoffs_with_budget, 300u);
	for (const IopCall& call : calls)
		EXPECT_NE(call.thread, std::this_thread::get_id());
}

TEST_F(LockstepMachine, TwoSkewedRunsOfTheSameScriptAreTheSameRun)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	SkewGuard skew_guard;
	AskForLockstep();
	IopThread::SetLockstepSkew(50000);

	std::vector<std::vector<s32>> budgets;
	std::vector<std::vector<After>> afters;
	for (int run = 0; run < 2; run++)
	{
		Reset();
		IopThread::StopThread();
		IopThread::ChooseMode();
		IopThread::SyncThread();
		ASSERT_TRUE(IopThread::LockstepActive());
		std::vector<IopCall> calls;
		s_calls = &calls;
		const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);
		std::vector<After> after;
		static constexpr u32 kSteps[] = {1, 40, 3072, 17, 5000, 300, 3072, 3072, 8, 90, 2500, 1};
		for (u32 i = 0; i < 240; i++)
		{
			cpuRegs.cycle += kSteps[i % std::size(kSteps)];
			if (i % 7 == 3)
				iopEventAction = true;
			test();
			after.push_back({EEsCycle, EEoCycle, iopEventAction, cpuRegs.nextEventCycle, psxRegs.iopNextEventCycle});
		}
		s_calls = nullptr;
		std::vector<s32> b;
		for (const IopCall& c : calls)
			b.push_back(c.budget);
		budgets.push_back(std::move(b));
		afters.push_back(std::move(after));
	}
	EXPECT_EQ(budgets[0], budgets[1]);
	ASSERT_EQ(afters[0].size(), afters[1].size());
	for (size_t i = 0; i < afters[0].size(); i++)
		EXPECT_TRUE(afters[0][i] == afters[1][i]) << "after event test " << i;
}

TEST_F(LockstepMachine, ASkewAppliesAnItemWhenTheIopsClockReachesItsStampAndNotWhenItIsPosted)
{
	REQUIRE_ARM64_HOST();
	constexpr s64 kSkew = 60000;
	SettingsGuard guard;
	SkewGuard skew_guard;
	AskForLockstep();
	IopThread::SetLockstepSkew(kSkew);
	Reset();
	const u64 start = static_cast<u64>(cpuRegs.cycle) - static_cast<u64>(static_cast<s64>(EEsCycle));
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	s_iop_ran = 0;
	std::vector<Probe> probes;
	ProbeGuard probe_guard(probes, start);
	const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);

	// Let the IOP get to its D-behind position first.
	for (u32 i = 0; i < 40; i++)
	{
		cpuRegs.cycle += 3072;
		test();
	}
	ASSERT_GT(s_iop_ran, 0u);
	ASSERT_TRUE(probes.empty());

	// Two items, 5000 cycles apart, posted by the EE at its cycle.
	const u64 stamp_a = cpuRegs.cycle;
	IopBridge::HBlankStart<IopThreadMode::Lockstep>();
	cpuRegs.cycle += 5000;
	const u64 stamp_b = cpuRegs.cycle;
	IopBridge::HBlankEnd<IopThreadMode::Lockstep>();
	EXPECT_TRUE(probes.empty()) << "an item was applied at the moment it was posted";

	u64 a_seen_at = 0, b_seen_at = 0;
	for (u32 i = 0; i < 200 && probes.size() < 2; i++)
	{
		cpuRegs.cycle += 700;
		test();
		if (a_seen_at == 0 && !probes.empty())
			a_seen_at = cpuRegs.cycle;
		if (b_seen_at == 0 && probes.size() >= 2)
			b_seen_at = cpuRegs.cycle;
	}
	ASSERT_EQ(probes.size(), 2u);
	EXPECT_STREQ(probes[0].what, "hblank_start");
	EXPECT_STREQ(probes[1].what, "hblank_end");

	// Each was applied by the handoff whose goal (EE cycle less the skew) first reached its stamp...
	EXPECT_GE(static_cast<s64>(a_seen_at) - kSkew, static_cast<s64>(stamp_a));
	EXPECT_LT(static_cast<s64>(a_seen_at) - 700 - kSkew, static_cast<s64>(stamp_a));
	EXPECT_GE(static_cast<s64>(b_seen_at) - kSkew, static_cast<s64>(stamp_b));
	// ...and at the IOP's clock reaching the stamp, within the overshoot of the slice that got it there.
	EXPECT_GE(probes[0].iop_pos, stamp_a);
	EXPECT_LE(probes[0].iop_pos, stamp_a + 22);
	EXPECT_GE(probes[1].iop_pos, stamp_b);
	EXPECT_LE(probes[1].iop_pos, stamp_b + 22);
	for (const Probe& p : probes)
		EXPECT_NE(p.thread, std::this_thread::get_id());
}

TEST_F(LockstepMachine, ASkewDefersAnEeStartedSifPumpToTheStampWhateverTheSetting)
{
	REQUIRE_ARM64_HOST();
	constexpr s64 kSkew = 40000;
	SettingsGuard guard;
	SkewGuard skew_guard;
	AskForLockstep();
	EmuConfig.Speedhacks.iopThreadDeferSif = false;
	IopThread::SetLockstepSkew(kSkew);
	Reset();
	const u64 start = static_cast<u64>(cpuRegs.cycle) - static_cast<u64>(static_cast<s64>(EEsCycle));
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	s_iop_ran = 0;
	std::vector<Probe> probes;
	ProbeGuard probe_guard(probes, start);
	const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);
	for (u32 i = 0; i < 30; i++)
	{
		cpuRegs.cycle += 3072;
		test();
	}

	const u64 stamp = cpuRegs.cycle;
	ASSERT_TRUE(IopThread::DeferSifPump(1)) << "pumping at once would hand the IOP the data a skew early";
	EXPECT_TRUE(probes.empty());
	for (u32 i = 0; i < 30 && probes.empty(); i++)
	{
		cpuRegs.cycle += 3072;
		test();
	}
	ASSERT_EQ(probes.size(), 1u);
	EXPECT_STREQ(probes[0].what, "sif_pump");
	EXPECT_EQ(probes[0].arg, 1u);
	EXPECT_GE(probes[0].iop_pos, stamp);
	EXPECT_NE(probes[0].thread, std::this_thread::get_id());
}

// The holds variant: an EE-started SIF pump is not deferred. The IOP is run up to the EE's cycle (applying the items due on the
// way), the caller pumps in place, and the skew grows back as the EE runs on.
TEST_F(LockstepMachine, ASkewWithHoldsRunsTheIopUpToTheEeAtAPumpAndTheSkewRegrows)
{
	REQUIRE_ARM64_HOST();
	constexpr s64 kSkew = 60000;
	SettingsGuard guard;
	SkewGuard skew_guard;
	AskForLockstep();
	EmuConfig.Speedhacks.iopThreadDeferSif = true; // the hold wins over the setting
	IopThread::SetLockstepSkew(kSkew);
	IopThread::SetLockstepSkewHolds(true);
	Reset();
	const u64 start = static_cast<u64>(cpuRegs.cycle) - static_cast<u64>(static_cast<s64>(EEsCycle));
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	s_iop_ran = 0;
	std::vector<Probe> probes;
	ProbeGuard probe_guard(probes, start);
	const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);
	for (u32 i = 0; i < 40; i++)
	{
		cpuRegs.cycle += 3072;
		test();
	}
	EXPECT_EQ(static_cast<s64>(cpuRegs.cycle - IopThread::g_thread.PublishedPos()) > kSkew - 23, true);

	// An item the EE posted a little ago: not due for the skewed IOP, due for the EE's cycle.
	IopBridge::HBlankStart<IopThreadMode::Lockstep>();
	cpuRegs.cycle += 700;
	EXPECT_TRUE(probes.empty());

	ASSERT_FALSE(IopThread::DeferSifPump(1)) << "a hold pumps in place";
	ASSERT_EQ(probes.size(), 1u) << "the catch-up applies the items that are due on the way";
	EXPECT_STREQ(probes[0].what, "hblank_start");
	EXPECT_NE(probes[0].thread, std::this_thread::get_id());
	const u64 hold_cycle = cpuRegs.cycle;
	EXPECT_GE(IopThread::g_thread.PublishedPos(), hold_cycle) << "the IOP is at the EE's cycle";
	EXPECT_LE(IopThread::g_thread.PublishedPos(), hold_cycle + 22) << "or past it by the overshoot of its last block";
	EXPECT_LE(EEsCycle, 0);
	EXPECT_EQ(IopThread::g_thread.LockstepCatchUps(), 1u);

	// From there the IOP does not run until the EE is a skew ahead of it, and then it is a skew behind again.
	std::vector<IopCall> calls;
	s_calls = &calls;
	for (u32 i = 0; i < 60; i++)
	{
		const size_t before = calls.size();
		cpuRegs.cycle += 3072;
		test();
		const s64 ahead = static_cast<s64>(cpuRegs.cycle - IopThread::g_thread.PublishedPos());
		if (static_cast<s64>(cpuRegs.cycle) - kSkew <= static_cast<s64>(hold_cycle) + 22)
		{
			EXPECT_EQ(calls.size(), before) << "event test " << i << ": no budget yet";
			EXPECT_LT(ahead, kSkew) << "event test " << i;
		}
		else if (static_cast<s64>(cpuRegs.cycle) - kSkew > static_cast<s64>(hold_cycle) + 22 + 3072)
		{
			EXPECT_GE(ahead, kSkew - 22) << "event test " << i;
			EXPECT_LE(ahead, kSkew) << "event test " << i;
		}
	}
	s_calls = nullptr;
	EXPECT_GT(calls.size(), 10u);
}

TEST_F(LockstepMachine, AHoldOnOneChannelLeavesTheOtherDeferred)
{
	REQUIRE_ARM64_HOST();
	constexpr s64 kSkew = 40000;
	SettingsGuard guard;
	SkewGuard skew_guard;
	AskForLockstep();
	EmuConfig.Speedhacks.iopThreadDeferSif = false;
	IopThread::SetLockstepSkew(kSkew);
	IopThread::SetLockstepSkewHoldChannels(2); // SIF1 held, SIF0 deferred
	Reset();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);
	for (u32 i = 0; i < 30; i++)
	{
		cpuRegs.cycle += 3072;
		test();
	}
	EXPECT_TRUE(IopThread::DeferSifPump(0)) << "SIF0 is still deferred";
	EXPECT_EQ(IopThread::g_thread.LockstepCatchUps(), 0u);
	EXPECT_FALSE(IopThread::DeferSifPump(1)) << "SIF1 is held";
	EXPECT_EQ(IopThread::g_thread.LockstepCatchUps(), 1u);
}

TEST_F(LockstepMachine, HoldsDoNothingWithASkewOfZero)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	SkewGuard skew_guard;
	AskForLockstep();
	EmuConfig.Speedhacks.iopThreadDeferSif = false;
	IopThread::SetLockstepSkew(0);
	IopThread::SetLockstepSkewHolds(true);
	Reset();
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	EXPECT_FALSE(IopThread::DeferSifPump(0));
	EXPECT_EQ(IopThread::g_thread.LockstepCatchUps(), 0u);
}

TEST_F(LockstepMachine, ASkewDoesNotMakeTheEeLookAgainEvery48CyclesBecauseTheIopIsBehind)
{
	REQUIRE_ARM64_HOST();
	constexpr s64 kSkew = 100000;
	for (const s64 skew : {s64(0), kSkew})
	{
		SettingsGuard guard;
		SkewGuard skew_guard;
		AskForLockstep();
		IopThread::SetLockstepSkew(skew);
		Reset();
		IopThread::ChooseMode();
		IopThread::SyncThread();
		ASSERT_TRUE(IopThread::LockstepActive());
		const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);
		// An IOP event 2500 IOP cycles (20000 EE cycles) away, and the EE running in 3072-cycle steps.
		for (u32 i = 0; i < 120; i++)
		{
			psxRegs.iopNextEventCycle = psxRegs.cycle + 2500;
			cpuRegs.cycle += 3072;
			test();
			if (skew > 0 && i > 60)
				EXPECT_GT(cpuRegs.nextEventCycle - cpuRegs.cycle, 48u) << "event test " << i << " skew " << skew;
		}
		IopThread::StopThread();
	}
}

TEST_F(LockstepMachine, ASkewKeepsMoreItemsThanTheRingHoldsAndAppliesThemInOrder)
{
	REQUIRE_ARM64_HOST();
	// The IOP does not run until the EE is a skew ahead, so a burst of posts inside that stretch (a state whose counters catch up,
	// a guest loop that starts a SIF DMA every few hundred cycles while the deferred pump is pending) can hold more items than
	// the ring has slots. They go to a spill queue, behind the ring's, and come out in the order they went in.
	constexpr s64 kSkew = 900000;
	constexpr u32 kItems = IopThread::kE2IRingCapacity * 2 + 123;
	SettingsGuard guard;
	SkewGuard skew_guard;
	AskForLockstep();
	IopThread::SetLockstepSkew(kSkew);
	Reset();
	const u64 start = static_cast<u64>(cpuRegs.cycle) - static_cast<u64>(static_cast<s64>(EEsCycle));
	IopThread::ChooseMode();
	IopThread::SyncThread();
	ASSERT_TRUE(IopThread::LockstepActive());
	s_iop_ran = 0;
	std::vector<Probe> probes;
	ProbeGuard probe_guard(probes, start);

	// One item every 3 cycles: all of them well inside the first skew's worth of EE time, where the IOP has no budget.
	for (u32 i = 0; i < kItems; i++)
	{
		cpuRegs.cycle += 3;
		IopThread::ApplyOnIop(IopThread::E2IKind::SifPump, i & 1);
	}
	EXPECT_TRUE(probes.empty()) << "an item was applied at the moment it was posted";
	EXPECT_EQ(IopThread::g_thread.E2ISpillTotal(), kItems - IopThread::kE2IRingCapacity);

	const CpuEventTestFn test = cpuEventTestFor(IopThreadMode::Lockstep);
	for (u32 i = 0; i < 2000 && probes.size() < kItems; i++)
	{
		cpuRegs.cycle += 3072;
		test();
	}
	ASSERT_EQ(probes.size(), kItems);
	for (u32 i = 0; i < kItems; i++)
	{
		EXPECT_STREQ(probes[i].what, "sif_pump") << "item " << i;
		ASSERT_EQ(probes[i].arg, i & 1) << "item " << i << " came out of order";
	}
	EXPECT_EQ(IopThread::g_thread.E2ISpillMax(), kItems - IopThread::kE2IRingCapacity);
}

TEST_F(LockstepMachine, TheSkewIsClampedToTheLagCeilingSoTheEeToIopRingCannotFill)
{
	SkewGuard skew_guard;
	IopThread::SetLockstepSkew(-5);
	EXPECT_EQ(IopThread::LockstepSkewSetting(), 0);
	IopThread::SetLockstepSkew(IopThread::kMaxLag + 1000);
	EXPECT_EQ(IopThread::LockstepSkewSetting(), IopThread::kMaxLag);
	IopThread::SetLockstepSkew(1200000);
	EXPECT_EQ(IopThread::LockstepSkewSetting(), 1200000);
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
