// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The EE-to-IOP queue in Lockstep: the EE's blanking, its IRQ to the
// IOP and its once-a-frame jobs become items stamped with the EE's cycle, and the IOP thread applies each before
// the EE goes on, so each takes effect at the point of the EE's execution where Off made the direct call.
//
// Lockstep must give the answers Off gives, so each effect has to happen on the IOP thread, in the order the EE
// made them, before the call returns. The tests check that with a table of recording actions standing in for the
// IOP's functions, and then with the real functions where they are safe to run on a bare machine.

#include "Counters.h"
#include "IopThread/IopBridge.h"
#include "IopThreadTestSupport.h"
#include "VMManager.h"
#include "VUmicro.h"

#include <chrono>
#include <memory>

using namespace iopt;

TEST(IopThreadE2I, EveryKindIsMappedToItsAction)
{
	std::vector<Applied> seen;
	s_seen = &seen;
	for (u16 kind : {IopThread::E2IKind::HBlankStart, IopThread::E2IKind::HBlankEnd, IopThread::E2IKind::VBlankStart,
			 IopThread::E2IKind::VBlankEnd})
		IopThread::ApplyE2IItem(IopThread::E2IItem{0, kind, 0, 0}, kRecordingActions);
	IopThread::ApplyE2IItem(IopThread::E2IItem{0, IopThread::E2IKind::IopIrq, 0, 7}, kRecordingActions);
	IopThread::ApplyE2IItem(IopThread::E2IItem{0, IopThread::E2IKind::Call, 0, IopThread::E2IJob::MemcardBusyTick}, kRecordingActions);
	s_seen = nullptr;

	ASSERT_EQ(seen.size(), 6u);
	EXPECT_STREQ(seen[0].what, "hblank_start");
	EXPECT_STREQ(seen[1].what, "hblank_end");
	EXPECT_STREQ(seen[2].what, "vblank_start");
	EXPECT_STREQ(seen[3].what, "vblank_end");
	EXPECT_STREQ(seen[4].what, "iop_irq");
	EXPECT_EQ(seen[4].arg, 7u);
	EXPECT_STREQ(seen[5].what, "job");
	EXPECT_EQ(seen[5].arg, IopThread::E2IJob::MemcardBusyTick);
}

TEST(IopThreadE2I, TheKindsAndJobsHaveDistinctValues)
{
	// The numbers are what the queue carries; two kinds sharing one would apply the wrong function.
	const u16 kinds[] = {IopThread::E2IKind::HBlankStart, IopThread::E2IKind::HBlankEnd, IopThread::E2IKind::VBlankStart,
		IopThread::E2IKind::VBlankEnd, IopThread::E2IKind::IopIrq, IopThread::E2IKind::Call};
	for (size_t i = 0; i < std::size(kinds); i++)
		for (size_t j = i + 1; j < std::size(kinds); j++)
			EXPECT_NE(kinds[i], kinds[j]);
	EXPECT_NE(IopThread::E2IJob::AutoEjectTick, IopThread::E2IJob::MemcardBusyTick);
	EXPECT_NE(IopThread::E2IJob::AutoEjectTick, IopThread::E2IJob::VsyncPatchesIop);
	EXPECT_NE(IopThread::E2IJob::MemcardBusyTick, IopThread::E2IJob::VsyncPatchesIop);
}

TEST(IopThreadE2I, LockstepAppliesEachCrossingOnTheIopThreadBeforeTheCallReturns)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	Recorder recorder;

	IopBridge::HBlankStart<IopThreadMode::Lockstep>();
	IopBridge::HBlankEnd<IopThreadMode::Lockstep>();
	IopBridge::VBlankStart<IopThreadMode::Lockstep>();
	IopBridge::VBlankEnd<IopThreadMode::Lockstep>();
	IopBridge::AutoEjectTick<IopThreadMode::Lockstep>();
	IopBridge::MemcardBusyTick<IopThreadMode::Lockstep>();

	// Every call has been applied by the time it returned: nothing is still pending here.
	const auto& seen = recorder.Items();
	ASSERT_EQ(seen.size(), 6u);
	EXPECT_STREQ(seen[0].what, "hblank_start");
	EXPECT_STREQ(seen[1].what, "hblank_end");
	EXPECT_STREQ(seen[2].what, "vblank_start");
	EXPECT_STREQ(seen[3].what, "vblank_end");
	EXPECT_STREQ(seen[4].what, "job");
	EXPECT_EQ(seen[4].arg, IopThread::E2IJob::AutoEjectTick);
	EXPECT_STREQ(seen[5].what, "job");
	EXPECT_EQ(seen[5].arg, IopThread::E2IJob::MemcardBusyTick);

	// On the IOP thread, which is not this one, and the same one every time.
	for (const Applied& s : seen)
	{
		EXPECT_NE(s.thread, std::this_thread::get_id());
		EXPECT_EQ(s.thread, seen.front().thread);
	}
}

TEST(IopThreadE2I, TheIrqAndTheVsyncPatchesGoThroughTheQueueToo)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	Recorder recorder;

	IopBridge::RaiseIopIrq(1);
	IopBridge::ApplyVsyncPatches();

	const auto& seen = recorder.Items();
	ASSERT_EQ(seen.size(), 2u);
	EXPECT_STREQ(seen[0].what, "iop_irq");
	EXPECT_EQ(seen[0].arg, 1u);
	EXPECT_STREQ(seen[1].what, "job");
	EXPECT_EQ(seen[1].arg, IopThread::E2IJob::VsyncPatchesIop);
	for (const Applied& s : seen)
		EXPECT_NE(s.thread, std::this_thread::get_id());
}

TEST(IopThreadE2I, AnItemIsAppliedAtThePointOfThePostNotLater)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	// An action that takes a while, so a call that returned early would show: the EE's next step
	// must come after the action has finished.
	static std::vector<int>* s_order;
	std::vector<int> order;
	s_order = &order;
	static const IopThread::E2IActions slow = {
		[] {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			s_order->push_back(1);
		},
		[] { s_order->push_back(3); },
		[] {},
		[] {},
		[](u32) {},
		[](u32) {},
	};
	const IopThread::E2IActions* previous = IopThread::SetE2IActionsForTests(&slow);

	order.push_back(0);
	IopBridge::HBlankStart<IopThreadMode::Lockstep>();
	order.push_back(2);
	IopBridge::HBlankEnd<IopThreadMode::Lockstep>();
	order.push_back(4);

	IopThread::SetE2IActionsForTests(previous);
	s_order = nullptr;
	EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(IopThreadE2I, ManyItemsKeepTheirOrderAndAreAllApplied)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	Recorder recorder;
	const u64 before = IopThread::g_thread.GetStats().items_applied;

	// More than the ring holds, with the EE's cycle moving forward as it does: the queue never fills
	// because each item is applied as it is posted.
	const u64 cycle = cpuRegs.cycle;
	for (u32 i = 0; i < 3000; i++)
	{
		cpuRegs.cycle = cycle + i * 7;
		IopBridge::RaiseIopIrq(i & 15);
	}
	cpuRegs.cycle = cycle;

	const auto& seen = recorder.Items();
	ASSERT_EQ(seen.size(), 3000u);
	for (u32 i = 0; i < 3000; i++)
		ASSERT_EQ(seen[i].arg, i & 15u) << "item " << i;
	EXPECT_EQ(IopThread::g_thread.GetStats().items_applied - before, 3000u);
}

TEST(IopThreadE2I, WithNoThreadTheCallerAppliesTheItem)
{
	// A Lockstep dispatcher can outlive the thread (a mode switch, a PS1 switch, shutdown). The
	// bridge then does what Off does: the caller applies the item.
	SettingsGuard guard;
	ASSERT_FALSE(IopThread::LockstepActive());
	Recorder recorder;

	IopBridge::HBlankStart<IopThreadMode::Lockstep>();

	const auto& seen = recorder.Items();
	ASSERT_EQ(seen.size(), 1u);
	EXPECT_STREQ(seen[0].what, "hblank_start");
	EXPECT_EQ(seen[0].thread, std::this_thread::get_id());
}

TEST(IopThreadE2I, OffMakesTheDirectCallOnTheCallersThread)
{
	SettingsGuard guard;
	IopThread::ChooseMode();
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	IopStateGuard iop_state;

	// The real functions. IRQ 5 sets bit 5 of ISTAT; the blanking calls set the blanking flags.
	IopBridge::RaiseIopIrq(5);
	EXPECT_EQ(psxHu32(HW_ISTAT), 1u << 5);

	EXPECT_FALSE(hBlanking);
	IopBridge::HBlankStart<IopThreadMode::Off>();
	EXPECT_TRUE(hBlanking);
	IopBridge::HBlankEnd<IopThreadMode::Off>();
	EXPECT_FALSE(hBlanking);
}

TEST(IopThreadE2I, LockstepLeavesTheIopWhereOffDoesForTheRealFunctions)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;

	IopThread::ChooseMode();
	u32 off_istat;
	bool off_h_after_start, off_h_after_end;
	{
		IopStateGuard iop_state;
		IopBridge::RaiseIopIrq(5);
		IopBridge::RaiseIopIrq(9);
		IopBridge::HBlankStart<IopThreadMode::Off>();
		off_h_after_start = hBlanking;
		IopBridge::HBlankEnd<IopThreadMode::Off>();
		off_h_after_end = hBlanking;
		off_istat = psxHu32(HW_ISTAT);
	}

	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	u32 lock_istat;
	bool lock_h_after_start, lock_h_after_end;
	{
		IopStateGuard iop_state;
		IopBridge::RaiseIopIrq(5);
		IopBridge::RaiseIopIrq(9);
		IopBridge::HBlankStart<IopThreadMode::Lockstep>();
		lock_h_after_start = hBlanking;
		IopBridge::HBlankEnd<IopThreadMode::Lockstep>();
		lock_h_after_end = hBlanking;
		lock_istat = psxHu32(HW_ISTAT);
	}

	EXPECT_EQ(off_istat, (1u << 5) | (1u << 9));
	EXPECT_EQ(lock_istat, off_istat);
	EXPECT_TRUE(off_h_after_start);
	EXPECT_EQ(lock_h_after_start, off_h_after_start);
	EXPECT_FALSE(off_h_after_end);
	EXPECT_EQ(lock_h_after_end, off_h_after_end);
}

TEST(IopThreadE2I, TheCountersCallTheInstanceOfTheMode)
{
	// The counters' update is instantiated for both modes; the event test instance calls the one for
	// its own mode (checked through the machine tests). Here: the two instances exist and differ.
	EXPECT_NE(reinterpret_cast<void*>(&rcntUpdate<IopThreadMode::Off>), reinterpret_cast<void*>(&rcntUpdate<IopThreadMode::Lockstep>));
	EXPECT_NE(reinterpret_cast<void*>(&rcntUpdate_hScanline<IopThreadMode::Off>),
		reinterpret_cast<void*>(&rcntUpdate_hScanline<IopThreadMode::Lockstep>));
	EXPECT_NE(reinterpret_cast<void*>(&rcntUpdate_vSync<IopThreadMode::Off>),
		reinterpret_cast<void*>(&rcntUpdate_vSync<IopThreadMode::Lockstep>));
}

#ifdef PCSX2_RECOMPILER_TESTS
namespace
{
	// The machine an event test needs: quiet counters on both sides, the VM Running, both VUs idle. The
	// IOP is the real one with nothing to run (the books are even, so no slice is handed out). The EE's
	// hsync counter is set by the test to be due.
	class OwnerMachine : public ::testing::Test
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
			std::memcpy(m_counters, counters, sizeof(m_counters));
			m_hsync = hsyncCounter;
			m_vsync = vsyncCounter;
			m_ees = EEsCycle;
			m_eeo = EEoCycle;
			m_action = iopEventAction;
			m_vu0 = CpuVU0;
			m_vu1 = CpuVU1;
			m_vu_thread = EmuConfig.Speedhacks.vuThread;
			m_next_delta = nextDeltaCounter;
			m_next_start = nextStartCounter;
			m_psx_next_delta = psxNextDeltaCounter;
			m_psx_next_start = psxNextStartCounter;
			m_state = VMManager::GetState();
			m_vpu0 = VU0.VI[REG_VPU_STAT].UL;
			m_vpu1 = VU1.VI[REG_VPU_STAT].UL;
			m_h = hBlanking;

			CpuVU0 = &CpuIntVU0;
			CpuVU1 = &CpuIntVU1;
			EmuConfig.Speedhacks.vuThread = false;
			VU0.VI[REG_VPU_STAT].UL = 0;
			VU1.VI[REG_VPU_STAT].UL = 0;
			psxNextDeltaCounter = 0x7FFFFFFF;
			psxNextStartCounter = 0;
			VMManager::Internal::SetStateForTests(VMState::Running);

			std::memset(&cpuRegs, 0, sizeof(cpuRegs));
			std::memset(&psxRegs, 0, sizeof(psxRegs));
			std::memset(eeHw, 0, Ps2MemSize::Hardware);
			std::memset(iopHw, 0, Ps2MemSize::IopHardware);
			std::memset(counters, 0, sizeof(counters));
			cpuRegs.cycle = 0x100000;
			cpuRegs.nextEventCycle = cpuRegs.cycle;
			cpuRegs.CP0.n.Status.val = 0;
			psxRegs.cycle = 0x1000;
			psxRegs.iopCycleEE = -1;
			EEoCycle = cpuRegs.cycle;
			EEsCycle = 0;
			iopEventAction = false;
			// The vsync counter is far from due; the hsync counter is in its blanking and due to end it.
			vsyncCounter = {MODE_VRENDER, cpuRegs.cycle, 0x7FFFFFFF};
			hsyncCounter = {MODE_HBLANK, cpuRegs.cycle - 8, 4};
			nextStartCounter = cpuRegs.cycle - 16;
			nextDeltaCounter = 4;
			hBlanking = true;
		}

		void TearDown() override
		{
			IopThread::StopThread();
			VMManager::Internal::SetStateForTests(m_state);
			cpuRegs = *m_cpu;
			psxRegs = *m_psx;
			std::memcpy(eeHw, m_ee_hw.get(), Ps2MemSize::Hardware);
			std::memcpy(iopHw, m_iop_hw.get(), Ps2MemSize::IopHardware);
			std::memcpy(counters, m_counters, sizeof(m_counters));
			hsyncCounter = m_hsync;
			vsyncCounter = m_vsync;
			EEsCycle = m_ees;
			EEoCycle = m_eeo;
			iopEventAction = m_action;
			CpuVU0 = m_vu0;
			CpuVU1 = m_vu1;
			EmuConfig.Speedhacks.vuThread = m_vu_thread;
			nextDeltaCounter = m_next_delta;
			nextStartCounter = m_next_start;
			psxNextDeltaCounter = m_psx_next_delta;
			psxNextStartCounter = m_psx_next_start;
			VU0.VI[REG_VPU_STAT].UL = m_vpu0;
			VU1.VI[REG_VPU_STAT].UL = m_vpu1;
			hBlanking = m_h;
		}

		std::unique_ptr<cpuRegisters> m_cpu;
		std::unique_ptr<psxRegisters> m_psx;
		std::unique_ptr<u8[]> m_ee_hw, m_iop_hw;
		Counter m_counters[4];
		SyncCounter m_hsync, m_vsync;
		s32 m_ees;
		u64 m_eeo;
		bool m_action, m_vu_thread, m_h;
		BaseVUmicroCPU* m_vu0;
		BaseVUmicroCPU* m_vu1;
		s32 m_next_delta, m_psx_next_delta;
		u64 m_next_start, m_psx_next_start;
		VMState m_state;
		u32 m_vpu0, m_vpu1;
	};
} // namespace

TEST_F(OwnerMachine, TheOffEventTestEndsTheIopsBlankingWithTheDirectCall)
{
	SettingsGuard guard;
	IopThread::ChooseMode();
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	ASSERT_TRUE(hBlanking);

	cpuEventTestFor(IopThreadMode::Off)();

	EXPECT_EQ(hsyncCounter.Mode, static_cast<u32>(MODE_HRENDER)) << "the EE's hsync counter reached the end of its blanking";
	EXPECT_FALSE(hBlanking) << "psxHBlankEnd() ran";
}

TEST_F(OwnerMachine, TheLockstepEventTestEndsTheIopsBlankingOnTheIopThread)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	Recorder recorder;
	const u64 items = IopThread::g_thread.GetStats().items_applied;

	cpuEventTestFor(IopThreadMode::Lockstep)();

	EXPECT_EQ(hsyncCounter.Mode, static_cast<u32>(MODE_HRENDER));
	ASSERT_EQ(recorder.Items().size(), 1u) << "the Lockstep instance's counters posted the blank end";
	EXPECT_STREQ(recorder.Items()[0].what, "hblank_end");
	EXPECT_NE(recorder.Items()[0].thread, std::this_thread::get_id());
	EXPECT_EQ(IopThread::g_thread.GetStats().items_applied - items, 1u);
	EXPECT_TRUE(hBlanking) << "the real psxHBlankEnd() did not run: the recording table stood in for it";
}

TEST_F(OwnerMachine, TheLockstepEventTestLeavesTheIopWhereOffDoes)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;

	IopThread::ChooseMode();
	cpuEventTestFor(IopThreadMode::Off)();
	const bool off_h = hBlanking;
	const u32 off_mode = hsyncCounter.Mode;
	const u64 off_start = hsyncCounter.startCycle;
	const s32 off_delta = nextDeltaCounter;
	const s32 off_ees = EEsCycle;

	TearDown();
	SetUp();
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	cpuEventTestFor(IopThreadMode::Lockstep)();

	EXPECT_EQ(hBlanking, off_h);
	EXPECT_EQ(hsyncCounter.Mode, off_mode);
	EXPECT_EQ(hsyncCounter.startCycle, off_start);
	EXPECT_EQ(nextDeltaCounter, off_delta);
	EXPECT_EQ(EEsCycle, off_ees);
}
#endif // PCSX2_RECOMPILER_TESTS
