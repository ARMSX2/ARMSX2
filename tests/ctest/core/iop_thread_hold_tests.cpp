// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The holds around the EE's entries into IOP state, and the demotion of Lockstep to Off at a PS1 mode switch (IOP
// thread design sections 3.8, 6.2 and 9.6). In Lockstep a hold is only a marker, because the IOP thread is parked
// whenever the EE runs, so the tests look at the hold depth and the hold counter.

#include "IopThread/IopBridge.h"
#include "IopThreadTestSupport.h"

#include <vector>

using namespace iopt;

// ---------------------------------------------------------------- holds

TEST(IopThreadHolds, AnEmuHoldIsALiveHoldInLockstepAndNothingInOff)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	IopThread::ChooseMode();
	{
		IopBridge::EmuHold hold;
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u) << "Off takes no hold";
	}

	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
	{
		IopBridge::EmuHold outer;
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
		{
			IopBridge::EmuHold inner(IopThread::HoldKind::Lifecycle);
			EXPECT_EQ(IopThread::g_thread.HoldDepth(), 2u);
		}
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
	}
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
}

namespace
{
	u32 s_depth_in_handler = ~0u;
	u8 HeldTestRead8(u32 addr)
	{
		s_depth_in_handler = IopThread::g_thread.HoldDepth();
		return static_cast<u8>(addr + 1);
	}
	void HeldTestWrite16(u32 addr, u16 value)
	{
		s_depth_in_handler = IopThread::g_thread.HoldDepth() + (addr == 0x10 && value == 0x2222 ? 100 : 0);
	}
} // namespace

TEST(IopThreadHolds, AHardwareHandlerFromTheBridgeRunsWithTheIopHeld)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	u8 (*read8)(u32) = IopBridge::HwHandler<HeldTestRead8>();
	void (*write16)(u32, u16) = IopBridge::HwHandler<HeldTestWrite16>();
	ASSERT_NE(read8, nullptr);
	ASSERT_NE(read8, &HeldTestRead8) << "the registered handler must be the wrapper, not the handler";

	// Off: the handler runs with nothing held, and gets and returns what it was given.
	IopThread::ChooseMode();
	s_depth_in_handler = ~0u;
	EXPECT_EQ(read8(0x41), 0x42);
	EXPECT_EQ(s_depth_in_handler, 0u);

	// Lockstep: a hold is live inside the handler, and gone after it.
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	s_depth_in_handler = ~0u;
	EXPECT_EQ(read8(0x41), 0x42);
	EXPECT_EQ(s_depth_in_handler, 1u);
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
	write16(0x10, 0x2222); // both arguments reach the handler
	EXPECT_EQ(s_depth_in_handler, 101u);
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
}

namespace
{
	// IOP RAM for the EE-side memory bridge: one 64 KB page behind page 0 of the LUTs, which is all the
	// RAM path of iopMemRead*/iopMemWrite* needs.
	class IopRamPage
	{
	public:
		IopRamPage()
			: m_page(0x10000, 0)
			, m_wlut(0x2000, 0)
			, m_rlut(0x2000, 0)
			, m_cpu(psxCpu)
			, m_rptr(psxMemRLUT)
			, m_wptr(psxMemWLUT)
			, m_status(psxRegs.CP0.n.Status)
		{
			m_wlut[0] = reinterpret_cast<uptr>(m_page.data());
			m_rlut[0] = reinterpret_cast<uptr>(m_page.data());
			psxMemWLUT = m_wlut.data();
			psxMemRLUT = m_rlut.data();
			psxRegs.CP0.n.Status = 0;
			static R3000Acpu fake = {nullptr, nullptr, nullptr, [](u32, u32) {}, nullptr};
			psxCpu = &fake;
		}
		~IopRamPage()
		{
			psxCpu = m_cpu;
			psxMemRLUT = m_rptr;
			psxMemWLUT = m_wptr;
			psxRegs.CP0.n.Status = m_status;
		}

	private:
		std::vector<u8> m_page;
		std::vector<uptr> m_wlut, m_rlut;
		R3000Acpu* m_cpu;
		const uptr* m_rptr;
		uptr* m_wptr;
		u32 m_status;
	};
} // namespace

TEST(IopThreadHolds, EeAccessToIopRamHoldsTheIop)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	IopRamPage ram;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	const u64 before = IopThread::g_thread.GetStats().hold_markers;
	IopBridge::MemWrite32(0x100, 0xAABBCCDD);
	EXPECT_EQ(IopThread::g_thread.GetStats().hold_markers - before, 1u);
	IopBridge::MemWrite16(0x104, 0x1234);
	IopBridge::MemWrite8(0x106, 0x56);
	EXPECT_EQ(IopThread::g_thread.GetStats().hold_markers - before, 3u);

	EXPECT_EQ(IopBridge::MemRead32(0x100), 0xAABBCCDDu);
	EXPECT_EQ(IopBridge::MemRead16(0x104), 0x1234u);
	EXPECT_EQ(IopBridge::MemRead8(0x106), 0x56u);
	EXPECT_EQ(IopThread::g_thread.GetStats().hold_markers - before, 6u) << "each of the six accesses holds once";
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
}

TEST(IopThreadHolds, OffTakesNoHoldForTheSameAccesses)
{
	SettingsGuard guard;
	IopThread::ChooseMode();
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	IopRamPage ram;
	const u64 before = IopThread::g_thread.GetStats().hold_markers;
	IopBridge::MemWrite32(0x100, 0x11223344);
	EXPECT_EQ(IopBridge::MemRead32(0x100), 0x11223344u);
	EXPECT_EQ(IopThread::g_thread.GetStats().hold_markers, before);
}

// ---------------------------------------------------------------- the PS1 mode switch

namespace
{
	int s_cpu_resets = 0;
	R5900cpu s_fake_cpu = {nullptr, nullptr, [] { s_cpu_resets++; }, nullptr, nullptr, nullptr, nullptr, nullptr};
} // namespace

TEST(IopThreadPs1Switch, LockstepIsDemotedToOffAndTheThreadStops)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	Cpu = &s_fake_cpu;
	s_cpu_resets = 0;

	{
		IopBridge::Ps1Switch ps1;
		EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
		EXPECT_FALSE(IopThread::g_thread.IsRunning());
		EXPECT_FALSE(IopThread::LockstepActive());
		EXPECT_EQ(g_intEventTestShared, cpuEventTestFor(IopThreadMode::Off));
		// The hold the switch took is live for the reset that follows it.
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
	}
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
	EXPECT_EQ(s_cpu_resets, 1) << "the EE core is asked once to regenerate its dispatcher for Off";
}

TEST(IopThreadPs1Switch, AnOffVmIsLeftAlone)
{
	SettingsGuard guard;
	IopThread::ChooseMode();
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	Cpu = &s_fake_cpu;
	s_cpu_resets = 0;
	{
		IopBridge::Ps1Switch ps1;
		EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	}
	EXPECT_EQ(s_cpu_resets, 0) << "nothing to demote: no reset";
}

TEST(IopThreadPs1Switch, TheHoldSurvivesTheStopAndTheCountersEndAtZero)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	Cpu = &s_fake_cpu;
	{
		IopThread::VmHold outer; // a lifecycle hold the switch happens inside of
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
		{
			IopBridge::Ps1Switch ps1;
			EXPECT_EQ(IopThread::g_thread.HoldDepth(), 2u);
		}
		EXPECT_EQ(IopThread::g_thread.HoldDepth(), 1u);
	}
	EXPECT_EQ(IopThread::g_thread.HoldDepth(), 0u);
}

TEST(IopThreadPs1Switch, TheBlankingCallsStillWorkOnTheOldDispatcherAfterTheSwitch)
{
	// The EE JIT's dispatcher keeps calling the Lockstep instance until it is regenerated. Its bridge calls
	// find no thread and apply the item themselves.
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	Cpu = &s_fake_cpu;
	{
		IopBridge::Ps1Switch ps1;
	}
	ASSERT_FALSE(IopThread::LockstepActive());
	Recorder recorder;
	IopBridge::HBlankStart<IopThreadMode::Lockstep>();
	ASSERT_EQ(recorder.Items().size(), 1u);
	EXPECT_EQ(recorder.Items()[0].thread, std::this_thread::get_id());
}

TEST(IopThreadPs1Switch, AVmResetGivesTheThreadBack)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	Cpu = &s_fake_cpu;
	{
		IopBridge::Ps1Switch ps1;
	}
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	ASSERT_FALSE(IopThread::g_thread.IsRunning());

	// The reset puts the IOP back in PS2 mode, so the settings that ask for Lockstep get it.
	IopThread::ChooseModeForReset();
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Lockstep);
	IopThread::SyncThread();
	EXPECT_TRUE(IopThread::LockstepActive());
}

TEST(IopThreadPs1Switch, AResetDoesNotTurnOnWhatTheSettingsTurnOff)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	EmuConfig.Speedhacks.iopThread = false;
	IopThread::ChooseModeForReset();
	EXPECT_EQ(IopThread::GetMode(), IopThreadMode::Off);
}
