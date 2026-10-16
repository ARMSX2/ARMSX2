// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP thread as the VM uses it in Threaded mode (IOP thread design sections 3, 4, 5 and 7): the Threaded
// instance of the EE event test, the IOP thread that paces itself from the EE's watermark, the items that
// cross in each direction, and the SIF pumps that run on the EE thread with the IOP stopped (variant R).
//
// The thread object is tested on its own in iop_thread_tests.cpp. What is tested here is the glue: the
// bridges, the event test, the books the EE keeps for a savestate, the pump. The IOP's CPU is a fake that
// records which thread ran it, with what budget, and can run a function inside each slice or hold the slice
// until the test lets it go. The EE is the test thread, which runs the real Threaded event test.
//
// The event test returns at once unless the VM is Running, and the only way to say that without a VM is
// VMManager::Internal::SetStateForTests, which exists in builds with the recompiler test hooks. The tests
// that need it skip themselves in a build without them.

#include "Common.h"
#include "Config.h"
#include "Counters.h"
#include "Hw.h"
#include "IopCounters.h"
#include "IopHw.h"
#include "IopThread/EeBridge.h"
#include "IopThread/IopBridge.h"
#include "IopThread/IopThreadMode.h"
#include "IopThread/IopThreadVm.h"
#include "IopThread/OwnerAsserts.h"
#include "IopThreadTestSupport.h"
#include "PerformanceMetrics.h"
#include "R3000A.h"
#include "R5900.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "Sif.h"
#include "VMManager.h"
#include "VUmicro.h"

#include "common/MemorySettingsInterface.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef PCSX2_RECOMPILER_TESTS

extern s32 nextDeltaCounter;
extern u64 nextStartCounter;
extern s32 psxNextDeltaCounter;
extern u64 psxNextStartCounter;

using namespace iopt;

namespace
{
	using namespace std::chrono_literals;
	using IopThread::HostCpuInfo;

	HostCpuInfo Arm64Host()
	{
		HostCpuInfo host;
		host.is_arm64 = true;
		host.total_cores = 8;
		host.performance_cores = 4;
		return host;
	}

	// ---- the fake IOP CPU

	struct SliceCall
	{
		std::thread::id thread;
		s32 budget;
	};

	std::mutex s_mutex;
	std::vector<SliceCall> s_calls;
	std::function<void()> s_hook; // run once, inside the next fake slice, on the IOP thread
	std::atomic<bool> s_hold_slice{false}; // the fake slice does not return while this is set
	std::atomic<u32> s_slices_started{0};
	std::atomic<u32> s_hook_runs{0};
	std::atomic<bool> s_gate{false}; // a gated hook waits for this before it runs
	std::atomic<u64> s_fake_advance{0}; // EE cycles the fake IOP has run since SetUp: each slice's budget plus its overshoot

	// Runs the whole budget and a bit more, like the JIT finishing its last block, and gives back what is left (negative
	// when it overshot), as ExecuteBlock does.
	s32 FakeExecuteBlock(s32 ee_cycles)
	{
		std::function<void()> hook;
		{
			std::lock_guard lock(s_mutex);
			s_calls.push_back({std::this_thread::get_id(), ee_cycles});
			hook = std::exchange(s_hook, nullptr);
		}
		s_slices_started.fetch_add(1);
		if (hook)
		{
			hook();
			s_hook_runs.fetch_add(1);
		}
		// Bounded: a test that forgets to release the slice fails instead of hanging.
		for (int i = 0; s_hold_slice.load() && i < 20000; i++)
			std::this_thread::sleep_for(1ms);
		const s32 overshoot = (ee_cycles > 0) ? (ee_cycles % 23) : 0;
		// Before the slice returns, so anything the IOP thread does after the slice sees the clock it ended on.
		if (ee_cycles > 0)
			s_fake_advance.fetch_add(static_cast<u64>(ee_cycles) + overshoot);
		return ee_cycles > 0 ? -overshoot : ee_cycles;
	}

	void FakeClear(u32, u32) {}

	R3000Acpu s_fake_psx = {nullptr, nullptr, FakeExecuteBlock, FakeClear, nullptr};

	std::atomic<u32> s_violations{0};
	void CountViolation(IopThread::Side, const char*)
	{
		s_violations.fetch_add(1);
	}

	// What the event test does for a pause the IOP asked for, in a test with no VM to pause.
	std::atomic<u32> s_pause_actions{0};
	void CountPauseAction()
	{
		s_pause_actions.fetch_add(1);
	}

	// Polls `ready` for up to `limit`. Like the EE's next event test it wakes the IOP, and unless `serve` is false it
	// serves what the IOP asks of the EE; a test that wants to look at the IOP-to-EE queue before the EE has emptied it
	// passes false.
	bool WaitFor(const std::function<bool()>& ready, std::chrono::milliseconds limit = 10s, bool serve = true)
	{
		const auto end = std::chrono::steady_clock::now() + limit;
		while (!ready())
		{
			if (std::chrono::steady_clock::now() > end)
				return false;
			// What the next event test does for a parked IOP: wake it if it has budget (design 3.9, the accepted lost
			// wake-up), and serve what it asks of the EE.
			IopThread::g_thread.Wake();
			if (serve)
				IopThread::g_thread.DrainI2E();
			std::this_thread::sleep_for(100us);
		}
		return true;
	}

	// The machine an event test needs, with the IOP replaced by the fake: quiet counters on both sides, no pending
	// interrupt, both VUs idle and their pointers valid. Saves what it changes and puts it back.
	class ThreadedMachine : public ::testing::Test
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
			m_lag = EmuConfig.Speedhacks.iopThreadLag;
			m_iop_thread = EmuConfig.Speedhacks.iopThread;
			m_lockstep = EmuConfig.Speedhacks.iopThreadLockstep;
			m_next_delta = nextDeltaCounter;
			m_next_start = nextStartCounter;
			m_psx_next_delta = psxNextDeltaCounter;
			m_psx_next_start = psxNextStartCounter;
			m_state = VMManager::GetState();
			m_vpu0 = VU0.VI[REG_VPU_STAT].UL;
			m_vpu1 = VU1.VI[REG_VPU_STAT].UL;
			m_cpu_provider = Cpu;

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

			{
				std::lock_guard lock(s_mutex);
				s_calls.clear();
				s_hook = nullptr;
			}
			s_hold_slice = false;
			s_gate = false;
			s_fake_advance = 0;
			s_slices_started = 0;
			s_hook_runs = 0;
			s_violations = 0;
			IopThread::g_pause_requested.store(false);
			m_previous_handler = IopThread::SetViolationHandler(&CountViolation);
			m_min_slice = IopThread::ThreadedMinSlice();
			s_pause_actions = 0;
			m_previous_pause_action = IopThread::SetPauseActionForTests(&CountPauseAction);
		}

		void TearDown() override
		{
			s_hold_slice = false;
			IopThread::StopThread();
			IopThread::SetViolationHandler(m_previous_handler);
			IopThread::SetPauseActionForTests(m_previous_pause_action);
			IopThread::SetThreadedMinSlice(m_min_slice);
			IopThread::g_pause_requested.store(false);
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
			Cpu = m_cpu_provider;
			EmuConfig.Speedhacks.vuThread = m_vu_thread;
			EmuConfig.Speedhacks.iopThreadLag = m_lag;
			EmuConfig.Speedhacks.iopThread = m_iop_thread;
			EmuConfig.Speedhacks.iopThreadLockstep = m_lockstep;
			IopThread::ChooseMode();
			nextDeltaCounter = m_next_delta;
			nextStartCounter = m_next_start;
			psxNextDeltaCounter = m_psx_next_delta;
			psxNextStartCounter = m_psx_next_start;
			VU0.VI[REG_VPU_STAT].UL = m_vpu0;
			VU1.VI[REG_VPU_STAT].UL = m_vpu1;
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
		}

		// Threaded, with the thread running, on a host that allows it.
		void StartThreaded(s32 lag = IopThread::kDefaultLag)
		{
			EmuConfig.Speedhacks.iopThread = true;
			EmuConfig.Speedhacks.iopThreadLockstep = false;
			EmuConfig.Speedhacks.iopThreadLag = lag;
			IopThread::ChooseModeFor(Arm64Host(), IopThread::VmConditions());
			ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Threaded);
			IopThread::SyncThread();
			ASSERT_TRUE(IopThread::g_thread.IsRunning());
			ASSERT_EQ(IopThread::g_thread.Mode(), IopThreadMode::Threaded);
		}

		// One EE event test: the EE has run `cycles` more.
		void EventTest(u32 cycles)
		{
			cpuRegs.cycle += cycles;
			cpuEventTestFor(IopThreadMode::Threaded)();
		}

		bool WaitForIop(u64 cycle)
		{
			return WaitFor([cycle] { return static_cast<s64>(IopThread::g_thread.PublishedPos() - cycle) >= 0; });
		}

		std::vector<SliceCall> Calls()
		{
			std::lock_guard lock(s_mutex);
			return s_calls;
		}

		void SetHook(std::function<void()> hook)
		{
			std::lock_guard lock(s_mutex);
			s_hook = std::move(hook);
		}

		// A hook that waits until the test opens the gate (s_gate = true) before it runs. The IOP wakes and runs a
		// slice while the EE is still inside the event test that published its watermark, so a test that looks at what
		// the IOP did before the EE's next event test opens the gate once that event test has returned.
		void SetGatedHook(std::function<void()> hook)
		{
			SetHook([hook = std::move(hook)] {
				for (int i = 0; !s_gate.load() && i < 20000; i++)
					std::this_thread::sleep_for(1ms);
				hook();
			});
		}

		std::unique_ptr<cpuRegisters> m_cpu;
		std::unique_ptr<psxRegisters> m_psx;
		std::unique_ptr<u8[]> m_ee_hw, m_iop_hw;
		s32 m_ees;
		u64 m_eeo;
		bool m_action;
		R3000Acpu* m_psx_cpu;
		R5900cpu* m_cpu_provider;
		BaseVUmicroCPU* m_vu0;
		BaseVUmicroCPU* m_vu1;
		bool m_vu_thread, m_iop_thread, m_lockstep;
		s32 m_lag;
		s32 m_next_delta, m_psx_next_delta;
		u64 m_next_start, m_psx_next_start;
		VMState m_state;
		u32 m_vpu0, m_vpu1;
		IopThread::ViolationHandler m_previous_handler;
		IopThread::PauseAction m_previous_pause_action;
		s32 m_min_slice;
	};
} // namespace

// ---------------------------------------------------------------- the event test

TEST_F(ThreadedMachine, TheThreadedEventTestPublishesTheWatermarkAndLeavesTheIopToItsThread)
{
	REQUIRE_ARM64_HOST();
	Reset();
	// An IOP event due in 100 IOP cycles (800 EE cycles): the Off instance would bring the EE's next event test forward to it.
	psxNextStartCounter = psxRegs.cycle;
	psxNextDeltaCounter = 100;
	StartThreaded();
	// The books are the EE's to keep in Off and Lockstep. With the IOP running beside the EE they are not kept, so a
	// Threaded event test must not touch them.
	EEsCycle = 0x5151;
	EEoCycle = 0x7171;

	cpuRegs.cycle += 3000;
	const u64 wm = cpuRegs.cycle;
	cpuEventTestFor(IopThreadMode::Threaded)();

	EXPECT_EQ(IopThread::g_thread.Watermark(), wm);
	EXPECT_EQ(EEsCycle, 0x5151);
	EXPECT_EQ(EEoCycle, 0x7171u);
	// The EE does not look at the IOP's events: the next event test is eeWaitCycles away, whatever the IOP has due.
	EXPECT_EQ(cpuRegs.nextEventCycle, wm + IopThread::kEeWaitCycles);

	ASSERT_TRUE(WaitForIop(wm));
	const std::vector<SliceCall> calls = Calls();
	ASSERT_FALSE(calls.empty());
	for (const SliceCall& call : calls)
		EXPECT_NE(call.thread, std::this_thread::get_id()) << "the IOP ran on the EE thread";
	EXPECT_EQ(s_violations.load(), 0u);
}

TEST_F(ThreadedMachine, TheIopRunsExactlyTheBudgetTheWatermarkGivesIt)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	const u64 start = IopThread::g_thread.PublishedPos(); // cycle + 500: the IOP is 500 ahead of the EE
	ASSERT_EQ(start, cpuRegs.cycle + 500);

	// The EE moves 2000 on: the IOP is 1500 short of the watermark.
	cpuRegs.cycle += 2000;
	cpuEventTestFor(IopThreadMode::Threaded)();
	ASSERT_TRUE(WaitForIop(cpuRegs.cycle));

	const std::vector<SliceCall> calls = Calls();
	ASSERT_FALSE(calls.empty());
	// Each slice's budget is what the watermark gave the IOP beyond where it was, and the IOP is where it ended: past the budget
	// by the block it overshot with (the fake's overshoot is budget % 23). The sum of the budgets is the distance from the
	// start to the last watermark, and the final position adds every overshoot.
	s64 total = 0, overshoots = 0;
	for (const SliceCall& call : calls)
	{
		total += call.budget;
		overshoots += (call.budget > 0) ? (call.budget % 23) : 0;
	}
	EXPECT_EQ(total, static_cast<s64>(cpuRegs.cycle - start));
	EXPECT_EQ(IopThread::g_thread.PublishedPos(), start + static_cast<u64>(total + overshoots));
	// Each slice ends with the IOP's own event test, which schedules the IOP's next branch iopWaitCycles (384) IOP cycles on (the
	// counters are quiet here). The fake IOP does not advance psxRegs.cycle.
	EXPECT_EQ(psxRegs.iopNextEventCycle, psxRegs.cycle + 384);
}

TEST_F(ThreadedMachine, TheMinimumSliceKeepsTheIopFromRunningOnASmallBudget)
{
	REQUIRE_ARM64_HOST();
	Reset();
	IopThread::SetThreadedMinSlice(4096);
	StartThreaded();
	ASSERT_TRUE(WaitFor([] { return IopThread::g_thread.IsParked(); }));

	// 500 behind the watermark: well under S_min, so the IOP lets the budget build up.
	EventTest(1000);
	std::this_thread::sleep_for(60ms);
	EXPECT_EQ(s_slices_started.load(), 0u);

	// 5500: over it.
	EventTest(5000);
	ASSERT_TRUE(WaitFor([] { return s_slices_started.load() >= 1; }));
	const std::vector<SliceCall> calls = Calls();
	EXPECT_GE(calls.front().budget, 4096);
}

TEST_F(ThreadedMachine, AThreadStartedInsideAnEventTestStaysHeldUntilTheNextEventTest)
{
	REQUIRE_ARM64_HOST();
	Reset();
	// The settings changed from the message pump at vsync, which runs inside the EE's event test. If the IOP thread was off, that
	// event test is the Off instance's, and what is left of it calls IOP code directly (the blanking, the memory card tick): with
	// the new thread running beside it that is a race. The thread is started, but stays stopped until the next event test.
	eeEventTestIsActive = true;
	{
		IopThread::VmHold apply_settings_holds; // ApplySettings holds around the change
		StartThreaded();
	}
	eeEventTestIsActive = false;
	EXPECT_TRUE(IopThread::g_thread.IopIsStopped()) << "the thread ran on after the settings were applied, inside the event test";

	cpuRegs.cycle += 3000;
	IopThread::g_thread.PublishWatermark(cpuRegs.cycle);
	std::this_thread::sleep_for(60ms);
	EXPECT_EQ(s_slices_started.load(), 0u);

	EventTest(100); // the next event test is the Threaded instance's
	EXPECT_FALSE(IopThread::g_thread.IopIsStopped());
	ASSERT_TRUE(WaitFor([] { return s_slices_started.load() >= 1; }));
	EXPECT_EQ(s_violations.load(), 0u);
}

TEST_F(ThreadedMachine, StoppingAThreadStartedInsideAnEventTestDropsTheHoldItWasKeptWith)
{
	REQUIRE_ARM64_HOST();
	Reset();
	eeEventTestIsActive = true;
	{
		IopThread::VmHold apply_settings_holds;
		StartThreaded();
	}
	eeEventTestIsActive = false;
	ASSERT_TRUE(IopThread::StartHoldPendingForTests());
	IopThread::StopThread();
	EXPECT_FALSE(IopThread::StartHoldPendingForTests());
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u) << "a hold from the stopped thread's life is still counted";
}

TEST_F(ThreadedMachine, AThreadStartedOutsideAnEventTestIsNotHeld)
{
	REQUIRE_ARM64_HOST();
	Reset();
	{
		IopThread::VmHold apply_settings_holds;
		StartThreaded();
	}
	EXPECT_FALSE(IopThread::g_thread.IopIsStopped());
}

TEST_F(ThreadedMachine, AnEventOfTheIopsOwnRunsASliceEvenWithNoBudget)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	EventTest(2000);
	ASSERT_TRUE(WaitForIop(cpuRegs.cycle));
	// Let it go back to sleep, with nothing left to run.
	ASSERT_TRUE(WaitFor([] { return IopThread::g_thread.IsParked(); }));
	const u32 slices_before = s_slices_started.load();
	{
		// The IOP has work of its own now (an interrupt is pending, say): iopEventAction. Set under a hold, which is
		// how the EE touches IOP state.
		IopThread::VmHold hold;
		iopEventAction = true;
	}
	ASSERT_TRUE(WaitFor([&] { return s_slices_started.load() > slices_before; }));
	const std::vector<SliceCall> calls = Calls();
	EXPECT_LE(calls.back().budget, 0) << "no new budget was published: the slice ran for the event alone";
}

TEST_F(ThreadedMachine, TheEeWaitsWhenItIsMoreThanTheLagBoundAhead)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded(IopThread::kDefaultLag);

	// The IOP takes a first budget and stays in its slice.
	s_hold_slice = true;
	EventTest(1000);
	ASSERT_TRUE(WaitFor([] { return s_slices_started.load() >= 1; }));

	// The EE now runs far beyond L. Its event test must not return until the IOP is within L of it.
	cpuRegs.cycle += 5 * IopThread::kDefaultLag;
	const u64 stop_cycle = cpuRegs.cycle;
	std::thread releaser([] {
		std::this_thread::sleep_for(150ms);
		s_hold_slice = false;
	});
	const auto begin = std::chrono::steady_clock::now();
	cpuEventTestFor(IopThreadMode::Threaded)();
	const auto waited = std::chrono::steady_clock::now() - begin;
	releaser.join();

	EXPECT_GE(waited, 100ms) << "the EE did not wait for the IOP";
	EXPECT_LE(static_cast<s64>(stop_cycle - IopThread::g_thread.PublishedPos()), IopThread::kDefaultLag);
	const IopThread::Stats stats = IopThread::GetStats();
	EXPECT_GE(stats.lag_spins, 1u);
	EXPECT_GE(stats.lag_stall_ns, 100'000'000u);
}

TEST_F(ThreadedMachine, TheEeDoesNotWaitWhileTheIopIsWithinTheLagBound)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	// The IOP is held in a slice and cannot move, but the EE is not yet L ahead of it: no wait.
	s_hold_slice = true;
	EventTest(1000);
	ASSERT_TRUE(WaitFor([] { return s_slices_started.load() >= 1; }));
	const auto begin = std::chrono::steady_clock::now();
	for (int i = 0; i < 20; i++)
		EventTest(100); // 3000 cycles in all: 500 ahead of the IOP's start position plus 3000 is well under L
	EXPECT_LT(std::chrono::steady_clock::now() - begin, 500ms);
	EXPECT_EQ(IopThread::GetStats().lag_spins, 0u);
	s_hold_slice = false;
}

TEST_F(ThreadedMachine, WithNoIopThreadTheThreadedInstanceRunsTheSliceItselfAsOffDoes)
{
	REQUIRE_ARM64_HOST();
	// A Threaded dispatcher can outlive the thread for a moment (a mode switch or a shutdown in progress). The slice is run
	// on the EE thread from the books, and the EE schedules for the IOP's events: exactly what the Off instance does.
	auto script = [this](IopThreadMode mode) {
		Reset();
		psxNextStartCounter = psxRegs.cycle; // an IOP event 100 IOP cycles on: the EE's next event test comes forward to it
		psxNextDeltaCounter = 100;
		{
			std::lock_guard lock(s_mutex);
			s_calls.clear();
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
				return ees == o.ees && eeo == o.eeo && action == o.action && next_event == o.next_event && iop_next_event == o.iop_next_event;
			}
		};
		std::vector<After> after;
		const CpuEventTestFn test = cpuEventTestFor(mode);
		static constexpr u32 kSteps[] = {1, 40, 3072, 17, 5000, 300, 3072, 3072, 8, 90, 2500, 1};
		for (u32 i = 0; i < 120; i++)
		{
			cpuRegs.cycle += kSteps[i % std::size(kSteps)];
			if (i % 7 == 3)
				iopEventAction = true;
			test();
			after.push_back({EEsCycle, EEoCycle, iopEventAction, cpuRegs.nextEventCycle, psxRegs.iopNextEventCycle});
		}
		std::vector<std::thread::id> threads;
		for (const SliceCall& c : Calls())
			threads.push_back(c.thread);
		return std::make_pair(after, threads);
	};
	ASSERT_FALSE(IopThread::g_thread.IsRunning());
	const auto off = script(IopThreadMode::Off);
	const auto threaded = script(IopThreadMode::Threaded);
	ASSERT_FALSE(off.second.empty());
	EXPECT_EQ(off.second.size(), threaded.second.size());
	for (const std::thread::id& id : threaded.second)
		EXPECT_EQ(id, std::this_thread::get_id());
	ASSERT_EQ(off.first.size(), threaded.first.size());
	for (size_t i = 0; i < off.first.size(); i++)
		EXPECT_TRUE(off.first[i] == threaded.first[i]) << "event test " << i;
}

// ---------------------------------------------------------------- IOP to EE

TEST_F(ThreadedMachine, TheIopRaisesTheSbusInterruptThroughTheQueueAndTheEeWritesIntcStat)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	const u32 sbus = 1u << INTC_SBUS;
	ASSERT_EQ(psHu32(INTC_STAT) & sbus, 0u);

	SetGatedHook([] { EeBridge::RaiseSbusIrq(); });
	EventTest(2000);
	s_gate = true;
	ASSERT_TRUE(WaitFor([] { return s_hook_runs.load() >= 1; }, 10s, false));
	ASSERT_TRUE(WaitFor([] { return IopThread::g_thread.MayHaveI2E(); }, 10s, false));

	// INTC_STAT is written on the EE thread only (the EE JIT loads it inline): the IOP has queued the interrupt and
	// has not touched the register.
	EXPECT_EQ(psHu32(INTC_STAT) & sbus, 0u);
	EXPECT_EQ(s_violations.load(), 0u);

	EventTest(100); // the EE takes it at the top of its next event test
	EXPECT_NE(psHu32(INTC_STAT) & sbus, 0u);
	EXPECT_EQ(s_violations.load(), 0u);
}

TEST_F(ThreadedMachine, TheEeTakesTheIopsInterruptBeforeItChecksItsInterruptsInTheSameEventTest)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	// INTC interrupts enabled and the SBUS cause unmasked: an interrupt the event test sees raises an exception.
	cpuRegs.CP0.n.Status.val = 0x10001 | 0x400; // EIE, IE, the INTC master bit
	psHu32(INTC_MASK) = 1u << INTC_SBUS;

	SetGatedHook([] { EeBridge::RaiseSbusIrq(); });
	EventTest(2000);
	s_gate = true;
	ASSERT_TRUE(WaitFor([] { return s_hook_runs.load() >= 1 && IopThread::g_thread.MayHaveI2E(); }, 10s, false));
	ASSERT_EQ(cpuRegs.CP0.n.Status.b.EXL, 0u);

	EventTest(100);
	EXPECT_EQ(cpuRegs.CP0.n.Status.b.EXL, 1u) << "the interrupt waited for the event test after this one";
}

TEST_F(ThreadedMachine, APauseTheIopAsksForIsMadeByTheEeAtItsNextEventTestAndNotInsideTheDrain)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();

	SetGatedHook([] { EeBridge::RequestPause(); });
	EventTest(2000);
	s_gate = true;
	ASSERT_TRUE(WaitFor([] { return s_hook_runs.load() >= 1 && IopThread::g_thread.MayHaveI2E(); }, 10s, false));
	EXPECT_FALSE(IopThread::g_pause_requested.load()) << "the IOP thread must only queue the request";
	EXPECT_EQ(s_pause_actions.load(), 0u);

	// A drain inside an EE spin (the thread object's) only records it: SetPaused takes a hold, which cannot happen
	// inside a hold's own spin.
	IopThread::g_thread.DrainI2E();
	EXPECT_TRUE(IopThread::g_pause_requested.load());
	EXPECT_EQ(s_pause_actions.load(), 0u) << "the pause was made inside the drain";

	// The next event test makes it, though no item is waiting any more (the drain above took it).
	ASSERT_FALSE(IopThread::g_thread.MayHaveI2E());
	EventTest(100);
	EXPECT_EQ(s_pause_actions.load(), 1u);
	EXPECT_FALSE(IopThread::g_pause_requested.load());
	EventTest(100);
	EXPECT_EQ(s_pause_actions.load(), 1u) << "one request is one pause";
}

TEST_F(ThreadedMachine, APauseRequestTheEeFindsInTheQueueAtTheTopOfTheEventTestIsMadeThere)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();

	SetGatedHook([] { EeBridge::RequestPause(); });
	EventTest(2000);
	s_gate = true;
	ASSERT_TRUE(WaitFor([] { return s_hook_runs.load() >= 1 && IopThread::g_thread.MayHaveI2E(); }, 10s, false));
	EventTest(100);
	EXPECT_EQ(s_pause_actions.load(), 1u);
}

TEST_F(ThreadedMachine, TheIopSchedulesItsOwnEventsAndWritesNothingOfTheEesScheduling)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	cpuRegs.nextEventCycle = 0x4242;

	struct Seen
	{
		u64 ee_next_event_before, ee_next_event_after;
		s32 iop_cycle_ee_before, iop_cycle_ee_after, iop_break_before, iop_break_after;
		bool action_after;
		u32 iop_next_event_after;
		u64 iop_cycle;
	};
	std::atomic<bool> done{false};
	Seen seen = {};
	SetGatedHook([&] {
		psxRegs.iopCycleEE = 777;
		psxRegs.iopBreak = 5;
		seen.ee_next_event_before = cpuRegs.nextEventCycle;
		seen.iop_cycle_ee_before = psxRegs.iopCycleEE;
		seen.iop_break_before = psxRegs.iopBreak;
		iopEventAction = false;
		psxRegs.iopNextEventCycle = psxRegs.cycle + 5000;
		EeBridge::ScheduleEeEvent(10); // C3: PSX_INT
		EeBridge::IopInterruptPending(); // C4: iopTestIntc
		EeBridge::BreakIopSlice(); // C5: an EE event inside a slice
		seen.ee_next_event_after = cpuRegs.nextEventCycle;
		seen.iop_cycle_ee_after = psxRegs.iopCycleEE;
		seen.iop_break_after = psxRegs.iopBreak;
		seen.action_after = iopEventAction;
		seen.iop_next_event_after = static_cast<u32>(psxRegs.iopNextEventCycle);
		seen.iop_cycle = psxRegs.cycle;
		psxRegs.iopCycleEE = -1;
		psxRegs.iopBreak = 0;
		done = true;
	});
	EventTest(2000);
	s_gate = true;
	ASSERT_TRUE(WaitFor([&] { return done.load(); }));

	EXPECT_EQ(seen.ee_next_event_after, seen.ee_next_event_before) << "the IOP wrote the EE's next event";
	EXPECT_EQ(seen.iop_cycle_ee_after, seen.iop_cycle_ee_before) << "an EE event cut the IOP slice short";
	EXPECT_EQ(seen.iop_break_after, seen.iop_break_before);
	EXPECT_TRUE(seen.action_after) << "an interrupt pending on the IOP gives it work of its own";
	EXPECT_LE(static_cast<s32>(seen.iop_next_event_after - static_cast<u32>(seen.iop_cycle)), 2) << "and its next branch comes forward";
	EXPECT_EQ(s_violations.load(), 0u);
}

// ---------------------------------------------------------------- EE to IOP

TEST_F(ThreadedMachine, AnItemTheEePostsIsAppliedByTheIopThreadAndTheEeDoesNotWaitForIt)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	Recorder recorder;

	// The IOP is busy in a slice that will not end. The EE posts three items at three cycles and goes on.
	s_hold_slice = true;
	EventTest(1000);
	ASSERT_TRUE(WaitFor([] { return s_slices_started.load() >= 1; }));

	const auto begin = std::chrono::steady_clock::now();
	cpuRegs.cycle += 100;
	IopBridge::HBlankStart<IopThreadMode::Threaded>();
	cpuRegs.cycle += 100;
	IopBridge::VBlankStart<IopThreadMode::Threaded>();
	cpuRegs.cycle += 100;
	IopBridge::RaiseIopIrq(1);
	EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s) << "the EE waited for the IOP";
	EXPECT_TRUE(recorder.Items().empty()) << "an item was applied while the IOP was inside a slice";

	s_hold_slice = false;
	ASSERT_TRUE(WaitFor([&] {
		std::lock_guard lock(s_seen_mutex);
		return s_seen && s_seen->size() >= 3;
	}));
	std::lock_guard lock(s_seen_mutex);
	ASSERT_EQ(recorder.Items().size(), 3u);
	EXPECT_STREQ(recorder.Items()[0].what, "hblank_start");
	EXPECT_STREQ(recorder.Items()[1].what, "vblank_start");
	EXPECT_STREQ(recorder.Items()[2].what, "iop_irq");
	EXPECT_EQ(recorder.Items()[2].arg, 1u);
	for (const Applied& item : recorder.Items())
		EXPECT_NE(item.thread, std::this_thread::get_id()) << item.what << " ran on the EE thread";
}

namespace
{
	// The fake IOP's clock when the vblank_end item was applied, for the test below.
	std::atomic<u64> s_advance_at_apply{~0ull};
	const IopThread::E2IActions kClockProbeActions = {
		[] { Record("hblank_start"); },
		[] { Record("hblank_end"); },
		[] { Record("vblank_start"); },
		[] {
			s_advance_at_apply.store(s_fake_advance.load());
			Record("vblank_end");
		},
		[](u32 line) { Record("iop_irq", line); },
		[](u32 job) { Record("job", job); },
	};

	// A Recorder whose vblank_end action also reads the fake IOP's clock.
	class ClockProbeRecorder
	{
	public:
		ClockProbeRecorder()
		{
			s_seen = &m_seen;
			m_previous = IopThread::SetE2IActionsForTests(&kClockProbeActions);
		}
		~ClockProbeRecorder()
		{
			IopThread::SetE2IActionsForTests(m_previous);
			s_seen = nullptr;
		}
		const std::vector<Applied>& Items() const { return m_seen; }

	private:
		std::vector<Applied> m_seen;
		const IopThread::E2IActions* m_previous;
	};
} // namespace

TEST_F(ThreadedMachine, AnItemIsNotAppliedBeforeTheIopsClockReachesItsStamp)
{
	REQUIRE_ARM64_HOST();
	Reset();
	// Where the IOP starts: the EE's cycle less what the IOP is owed (design 9.1).
	const u64 start = static_cast<u64>(cpuRegs.cycle) - static_cast<u64>(static_cast<s64>(EEsCycle));
	StartThreaded();
	s_advance_at_apply = ~0ull;
	ClockProbeRecorder recorder;

	// A far stamp: the watermark follows it, so the IOP runs up to it and applies it then, and not before the slice that reaches it.
	cpuRegs.cycle += 40000;
	IopBridge::VBlankEnd<IopThreadMode::Threaded>();
	const u64 stamp = cpuRegs.cycle;
	ASSERT_TRUE(WaitFor([&] {
		std::lock_guard lock(s_seen_mutex);
		return s_seen && !s_seen->empty();
	}));
	// The item was applied after the IOP reached the stamp. The IOP's clock at the moment of the apply is what the fake slices had
	// added up by then. (Not PublishedPos(): the thread applies the items that fall due in a slice and publishes its position
	// after them, IopThread.cpp RunOneSlice, so a published position read when the item is first seen can still be the previous
	// slice's. A test that compared with it failed once in 40 repeats of the suites on a loaded machine.)
	ASSERT_NE(s_advance_at_apply.load(), ~0ull);
	EXPECT_GE(static_cast<s64>(start + s_advance_at_apply.load() - stamp), 0);
	// And the position does get published, after the item.
	EXPECT_TRUE(WaitForIop(stamp));
	std::lock_guard lock(s_seen_mutex);
	EXPECT_STREQ(recorder.Items()[0].what, "vblank_end");
}

// ---------------------------------------------------------------- SIF, variant R

namespace
{
	struct PumpLog
	{
		std::mutex mutex;
		std::vector<std::string> order;
		std::thread::id pump_thread;
		bool allowed_inside = false;
	};
	PumpLog* s_pump_log = nullptr;

	void LoggedPump()
	{
		std::lock_guard lock(s_pump_log->mutex);
		s_pump_log->order.push_back("pump");
		s_pump_log->pump_thread = std::this_thread::get_id();
		s_pump_log->allowed_inside = IopThread::IopSideAllowed();
	}

	void LogStep(const char* what)
	{
		std::lock_guard lock(s_pump_log->mutex);
		s_pump_log->order.push_back(what);
	}
} // namespace

TEST_F(ThreadedMachine, AnIopStartedSifPumpRunsOnTheEeThreadAndTheIopWaitsForIt)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	PumpLog log;
	s_pump_log = &log;

	SetHook([] {
		LogStep("iop: before");
		IopThread::RunSifPumpOnEe(&LoggedPump);
		LogStep("iop: after");
	});
	EventTest(2000);
	// The EE serves the request at its next event test, or sooner inside any of its waits.
	ASSERT_TRUE(WaitFor([&] {
		std::lock_guard lock(log.mutex);
		return log.order.size() >= 3;
	}));
	s_pump_log = nullptr;

	ASSERT_EQ(log.order.size(), 3u);
	EXPECT_EQ(log.order[0], "iop: before");
	EXPECT_EQ(log.order[1], "pump");
	EXPECT_EQ(log.order[2], "iop: after");
	EXPECT_EQ(log.pump_thread, std::this_thread::get_id()) << "the pump touches both machines: it runs on the EE thread";
	EXPECT_TRUE(log.allowed_inside) << "the IOP is blocked at a known point inside a CallOnEE function: the IOP-side steps may run";
	EXPECT_FALSE(IopThread::IopSideAllowed()) << "outside it, the EE thread has no business with the IOP";
	const IopThread::Stats stats = IopThread::GetStats();
	EXPECT_EQ(stats.calls_posted, 1u);
	EXPECT_EQ(stats.calls_served, 1u);
}

TEST_F(ThreadedMachine, TheIopsSifBridgesRunTheRealPumpsOnTheEeThreadWithNoOwnershipViolation)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();

	SetHook([] {
		EeBridge::Sif0Pump();
		EeBridge::Sif1Pump();
	});
	EventTest(2000);
	ASSERT_TRUE(WaitFor([] { return s_hook_runs.load() >= 1; }));
	const IopThread::Stats stats = IopThread::GetStats();
	EXPECT_EQ(stats.calls_posted, 2u);
	EXPECT_EQ(stats.calls_served, 2u);
	// The pump asserts the IOP side (a hold or a CallOnEE function on the EE thread) and the EE side (not the IOP thread)
	// of every step it takes.
	EXPECT_EQ(s_violations.load(), 0u);
}

#if defined(PCSX2_DEBUG) || defined(PCSX2_DEVBUILD)
TEST_F(ThreadedMachine, ThePumpRunOnTheIopThreadItselfIsWhatTheOwnershipAssertsExistToCatch)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();

	// The control for the test above: the same pump called without the bridge, on the IOP thread, runs an EE step there.
	SetHook([] { SIF0Dma(); });
	EventTest(2000);
	ASSERT_TRUE(WaitFor([] { return s_hook_runs.load() >= 1; }));
	EXPECT_GE(s_violations.load(), 1u);
}
#endif

TEST_F(ThreadedMachine, AnEeStartedSifPumpHoldsTheIopWhileItRunsAndIsCounted)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	cpuRegs.cycle += 5000; // the IOP is behind

	IopBridge::Sif0Pump();
	IopBridge::Sif1Pump();

	EXPECT_EQ(s_violations.load(), 0u) << "the IOP-side steps ran without the IOP held";
	const IopThread::Stats stats = IopThread::GetStats();
	EXPECT_EQ(stats.sif_ee_pumps, 2u);
	EXPECT_GE(stats.emu_hold_requests, 2u);
	// The hold made the IOP run up to the EE's cycle before the pump started, and it is still there.
	EXPECT_GE(static_cast<s64>(IopThread::g_thread.PublishedPos() - cpuRegs.cycle), 0);
}

TEST_F(ThreadedMachine, AnEeStartedSifPumpStillHoldsTheIopWhenTheVmIsStoppingAndTheIopIsSlow)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();

	// The VM has been asked to stop and the IOP is in the middle of a slice that takes much longer than the first stop check
	// of a spin. The EE thread is still in guest code (it leaves at its next event test), so a SIF pump can start in that
	// window. It must wait for the IOP like any other: a pump that went on without the hold would run the IOP's half of the
	// transfer beside the IOP thread. Found by stopping armsx2-sdl while a game was running.
	VMManager::Internal::SetStateForTests(VMState::Stopping);

	// The hold itself.
	s_hold_slice = true;
	std::thread release([] {
		std::this_thread::sleep_for(50ms);
		s_hold_slice = false;
	});
	cpuRegs.cycle += 5000; // the IOP is behind, so the hold needs a slice
	{
		IopBridge::EmuHold hold;
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u) << "the hold gave up while the IOP was still in its slice";
	}
	release.join();

	// A pump, with the IOP slow again. The ownership asserts count a violation if the IOP's half runs unheld.
	s_hold_slice = true;
	std::thread release_again([] {
		std::this_thread::sleep_for(50ms);
		s_hold_slice = false;
	});
	cpuRegs.cycle += 5000;
	const u32 violations_before = s_violations.load();
	IopBridge::Sif1Pump();
	release_again.join();
	EXPECT_EQ(s_violations.load(), violations_before) << "the IOP's half of the pump ran with the IOP not held";
}

// ---------------------------------------------------------------- the books

TEST_F(ThreadedMachine, AStartedThreadTakesItsPositionFromTheBooks)
{
	REQUIRE_ARM64_HOST();
	Reset();
	EEsCycle = -500;
	StartThreaded();
	EXPECT_EQ(IopThread::g_thread.PublishedPos(), cpuRegs.cycle + 500);
}

TEST_F(ThreadedMachine, AHoldRewritesTheBooksFromTheIopsPosition)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	EventTest(4000);
	ASSERT_TRUE(WaitForIop(cpuRegs.cycle));

	EEsCycle = 111;
	EEoCycle = 222;
	{
		IopThread::VmHold hold;
		// What a savestate stores: the IOP is owed what the EE has run beyond it, as of this cycle.
		EXPECT_EQ(EEsCycle, static_cast<s32>(cpuRegs.cycle - IopThread::g_thread.PublishedPos()));
		EXPECT_EQ(EEoCycle, cpuRegs.cycle);
		EXPECT_LE(EEsCycle, 0) << "the IOP ran at least up to the EE, and overshot";
	}
}

TEST_F(ThreadedMachine, AHoldMakesTheIopCatchUpToTheEesCycleFirst)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	// The EE runs on without an event test, as it does between two: the IOP's watermark is stale.
	cpuRegs.cycle += 2500;
	IopThread::VmHold hold;
	EXPECT_GE(static_cast<s64>(IopThread::g_thread.PublishedPos() - cpuRegs.cycle), 0);
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
}

TEST_F(ThreadedMachine, StoppingTheThreadLeavesTheBooksWhereTheIopWas)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	EventTest(4000);
	ASSERT_TRUE(WaitForIop(cpuRegs.cycle));
	const u64 stopped_at = [&] {
		IopThread::VmHold hold; // the callers hold: shutdown, a settings change, the PS1 switch
		EEsCycle = 31337;
		IopThread::StopThread();
		return IopThread::g_thread.PublishedPos();
	}();
	EXPECT_EQ(EEsCycle, static_cast<s32>(cpuRegs.cycle - stopped_at));
	EXPECT_EQ(EEoCycle, cpuRegs.cycle);
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
}

TEST_F(ThreadedMachine, AnEventTestUnderAHoldDoesNotWaitForTheHeldIop)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	IopThread::OnVmPaused(true); // a paused VM holds the IOP
	ASSERT_EQ(IopThread::g_thread.HoldDepth(), 1u);
	const u64 held_at = IopThread::g_thread.PublishedPos();

	const auto begin = std::chrono::steady_clock::now();
	EventTest(20 * IopThread::kDefaultLag); // far beyond L, and the held IOP cannot catch up
	EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
	EXPECT_EQ(IopThread::g_thread.PublishedPos(), held_at);
	IopThread::OnVmPaused(false);
}

TEST_F(ThreadedMachine, ANewLagBoundReachesTheRunningThread)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded(8000);
	EXPECT_EQ(IopThread::g_thread.Lag(), 8000);
	// What a settings change does (OnCpuConfigChanged), with the host described to ChooseModeFor.
	EmuConfig.Speedhacks.iopThreadLag = 30000;
	IopThread::ChooseModeFor(Arm64Host(), IopThread::VmConditions());
	IopThread::SyncThread();
	EXPECT_TRUE(IopThread::g_thread.IsRunning());
	EXPECT_EQ(IopThread::g_thread.Lag(), 30000);
	EmuConfig.Speedhacks.iopThreadLag = 1; // below the minimum: clamped
	IopThread::ChooseModeFor(Arm64Host(), IopThread::VmConditions());
	IopThread::SyncThread();
	EXPECT_EQ(IopThread::g_thread.Lag(), IopThread::kMinLag);
}

// ---------------------------------------------------------------- the pad's analog button

TEST_F(ThreadedMachine, TheAnalogButtonsToggleRunsOnTheIopThreadInThreaded)
{
	REQUIRE_ARM64_HOST();
	Reset();
	MemorySettingsInterface si;
	Pad::LoadConfig(si);
	PadBase* const pad = Pad::GetPad(0);
	ASSERT_NE(pad, nullptr);
	ASSERT_EQ(pad->GetType(), Pad::ControllerType::DualShock2);
	const bool before = pad->IsAnalogLightEnabled();

	StartThreaded();
	// The IOP's mode-switch command writes the same two fields, so the host thread's toggle is posted to the IOP thread:
	// with the IOP busy in a slice it has not happened yet.
	s_hold_slice = true;
	EventTest(1000);
	ASSERT_TRUE(WaitFor([] { return s_slices_started.load() >= 1; }));
	pad->Set(16 /* PAD_ANALOG */, 1.0f);
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ(pad->IsAnalogLightEnabled(), before) << "the host thread wrote the pad's mode itself";

	s_hold_slice = false;
	ASSERT_TRUE(WaitFor([&] { return pad->IsAnalogLightEnabled() != before; }));

	// With no IOP thread (the mode says Threaded, the thread is not started or has just been stopped) nothing else writes the pad's
	// mode and the toggle is made at once.
	IopThread::StopThread();
	IopThread::ChooseModeFor(Arm64Host(), IopThread::VmConditions());
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Threaded);
	ASSERT_FALSE(IopThread::g_thread.IsRunning());
	const bool now = pad->IsAnalogLightEnabled();
	pad->Set(16, 0.0f);
	pad->Set(16, 1.0f);
	EXPECT_NE(pad->IsAnalogLightEnabled(), now) << "a toggle posted to a thread that is not there was lost";

	// Off: the toggle is the direct call it always was.
	IopThread::StopThread();
	const bool then = pad->IsAnalogLightEnabled();
	pad->Set(16, 0.0f);
	pad->Set(16, 1.0f);
	EXPECT_NE(pad->IsAnalogLightEnabled(), then);

	Pad::Shutdown();
}

// ---------------------------------------------------------------- placement (step 14)

TEST_F(ThreadedMachine, AThreadedThreadIsOneMoreThreadToPlace)
{
	REQUIRE_ARM64_HOST();
	Reset();
	EXPECT_FALSE(IopThread::PlacesThread());
	EXPECT_EQ(IopThread::PlacementToken(), 0u);

	StartThreaded();
	EXPECT_TRUE(IopThread::PlacesThread());
	const u32 first = IopThread::PlacementToken();
	EXPECT_NE(first, 0u);
	EXPECT_TRUE(static_cast<bool>(IopThread::g_thread.GetThreadHandle()));

	// A restart is a new thread, which begins with the creator's affinity again: the token must differ so the
	// VM places it again.
	IopThread::StopThread();
	EXPECT_FALSE(IopThread::PlacesThread());
	EXPECT_EQ(IopThread::PlacementToken(), 0u);
	StartThreaded();
	EXPECT_NE(IopThread::PlacementToken(), first);
}

TEST_F(ThreadedMachine, AThreadedThreadCanBeMovedToAnotherProcessorWhileItRuns)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	const Threading::ThreadHandle& handle = IopThread::g_thread.GetThreadHandle();
	const u64 allowed = Threading::ThreadHandle::GetForCallingThread().GetAffinity();
	if (allowed == 0)
		GTEST_SKIP() << "this platform cannot read thread affinities";

	const u64 one = allowed & (~allowed + 1); // the lowest processor this process may use
	ASSERT_TRUE(handle.SetAffinity(one));
	EXPECT_EQ(handle.GetAffinity(), one);

	// It still works from there: the EE publishes and the IOP follows.
	EventTest(5000);
	ASSERT_TRUE(WaitForIop(cpuRegs.cycle - 1000));
	ASSERT_TRUE(handle.SetAffinity(allowed));
	EXPECT_EQ(handle.GetAffinity(), allowed);
}

// ---------------------------------------------------------------- a settings change while the VM is paused

TEST_F(ThreadedMachine, TurningTheThreadOnWhilePausedStartsItHeldAndTheResumeLetsItRun)
{
	REQUIRE_ARM64_HOST();
	Reset();

	// The VM is paused: SetState took the pause hold on the way in (design 9.3). The player opens the settings and ticks
	// the box; ApplySettings holds around the change and the thread starts.
	IopThread::OnVmPaused(true);
	{
		IopThread::VmHold apply_settings_holds;
		StartThreaded();
	}
	ASSERT_TRUE(IopThread::g_thread.IsRunning());
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Threaded);
	EXPECT_TRUE(IopThread::PlacesThread());
	EXPECT_TRUE(IopThread::g_thread.IopIsStopped()) << "the thread ran while the VM was paused";

	// The EE is not running, but a stray publish must not let the IOP run either.
	cpuRegs.cycle += 3000;
	IopThread::g_thread.PublishWatermark(cpuRegs.cycle);
	std::this_thread::sleep_for(60ms);
	EXPECT_EQ(s_slices_started.load(), 0u);

	// Resume: the hold goes, and the next event test hands the IOP its budget.
	IopThread::OnVmPaused(false);
	EXPECT_FALSE(IopThread::g_thread.IopIsStopped());
	EventTest(2000);
	ASSERT_TRUE(WaitFor([] { return s_slices_started.load() >= 1; })) << "the toggle made while paused never took effect";
	EXPECT_EQ(s_violations.load(), 0u);
}

TEST_F(ThreadedMachine, TurningTheThreadOffWhilePausedStopsItAndAnotherToggleBringsItBack)
{
	REQUIRE_ARM64_HOST();
	Reset();
	StartThreaded();
	ASSERT_TRUE(IopThread::PlacesThread());
	const u32 first = IopThread::PlacementToken();

	IopThread::OnVmPaused(true);
	{
		IopThread::VmHold apply_settings_holds;
		EmuConfig.Speedhacks.iopThread = false;
		IopThread::ChooseModeFor(Arm64Host(), IopThread::VmConditions());
		IopThread::SyncThread();
	}
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	EXPECT_FALSE(IopThread::g_thread.IsRunning());
	EXPECT_FALSE(IopThread::PlacesThread());
	EXPECT_EQ(IopThread::PlacementToken(), 0u);

	// Ticked again, still paused: a new thread, held, with a new token for the VM to place.
	{
		IopThread::VmHold apply_settings_holds;
		StartThreaded();
	}
	ASSERT_TRUE(IopThread::g_thread.IsRunning());
	EXPECT_TRUE(IopThread::g_thread.IopIsStopped());
	EXPECT_NE(IopThread::PlacementToken(), first);

	IopThread::OnVmPaused(false);
	EventTest(2000);
	ASSERT_TRUE(WaitFor([] { return s_slices_started.load() >= 1; }));
	EXPECT_EQ(s_violations.load(), 0u);
}

// ---------------------------------------------------------------- the thread's CPU time in the metrics

TEST_F(ThreadedMachine, AThreadedThreadIsRegisteredWithTheMetricsFromItsStartToItsStopAndAgainAfterARestart)
{
	REQUIRE_ARM64_HOST();
	Reset();
	EXPECT_FALSE(PerformanceMetrics::HasIOPThread());

	StartThreaded();
	EXPECT_TRUE(PerformanceMetrics::HasIOPThread());

	// Turned off while paused, as a settings dialog does: the metrics let go of the thread before it is joined.
	IopThread::OnVmPaused(true);
	{
		IopThread::VmHold apply_settings_holds;
		EmuConfig.Speedhacks.iopThread = false;
		IopThread::ChooseModeFor(Arm64Host(), IopThread::VmConditions());
		IopThread::SyncThread();
	}
	EXPECT_FALSE(PerformanceMetrics::HasIOPThread());
	EXPECT_EQ(PerformanceMetrics::GetIOPThreadUsage(), 0.0f);

	{
		IopThread::VmHold apply_settings_holds;
		StartThreaded();
	}
	EXPECT_TRUE(PerformanceMetrics::HasIOPThread());
	IopThread::OnVmPaused(false);
}

#endif // PCSX2_RECOMPILER_TESTS
