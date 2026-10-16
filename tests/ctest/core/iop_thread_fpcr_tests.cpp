// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP thread's floating-point control register (IOP thread design 3.10): it starts with the EE thread's, and a
// CPU settings change reapplies it through the host command queue.

#include "IopThreadTestSupport.h"
#include "common/FPControl.h"

#include <mutex>
#include <vector>

using namespace iopt;

// ================================================================ the IOP thread's FPCR

namespace
{
	std::mutex s_fpcr_mutex;
	std::vector<u32> s_fpcr_seen;

	const IopThread::E2IActions kFpcrActions = {
		[] {
			std::lock_guard lock(s_fpcr_mutex);
			s_fpcr_seen.push_back(FPControlRegister::GetCurrent().bitmask);
		},
		[] {},
		[] {},
		[] {},
		[](u32) {},
		[](u32) {},
	};

	// Puts the item table, the FPCR setting and the EE thread's FPCR back.
	class FpcrGuard
	{
	public:
		FpcrGuard()
			: m_config(EmuConfig.Cpu.FPUFPCR)
			, m_thread(FPControlRegister::GetCurrent())
		{
			s_fpcr_seen.clear();
			m_actions = IopThread::SetE2IActionsForTests(&kFpcrActions);
		}
		~FpcrGuard()
		{
			IopThread::SetE2IActionsForTests(m_actions);
			EmuConfig.Cpu.FPUFPCR = m_config;
			FPControlRegister::SetCurrent(m_thread);
		}

	private:
		FPControlRegister m_config, m_thread;
		const IopThread::E2IActions* m_actions;
	};

	// Flush-to-zero and denormals-are-zero are one bit on ARM64, so the third value differs in its rounding mode.
	FPControlRegister FpcrWith(bool flush_denormals, FPRoundMode round_mode)
	{
		// SetDenormalsAreZero and SetFlushToZero return a copy, so they are not chained.
		FPControlRegister value = FPControlRegister::GetDefault();
		value.SetDenormalsAreZero(flush_denormals);
		value.SetFlushToZero(flush_denormals);
		value.SetRoundMode(round_mode);
		return value;
	}

	// The FPCR the IOP thread has when it applies one item: the item runs on the IOP thread, after the host commands.
	u32 FpcrOnIopThreadNow()
	{
		IopThread::ApplyOnIop(IopThread::E2IKind::HBlankStart);
		std::lock_guard lock(s_fpcr_mutex);
		return s_fpcr_seen.empty() ? ~0u : s_fpcr_seen.back();
	}
} // namespace

TEST(IopThreadFpcr, ACpuSettingsChangeThatOnlyChangesTheFpcrIsACpuSettingsChange)
{
	// VMManager::CheckForCPUConfigChanges returns early when EmuConfig.Cpu is unchanged, and that is the only
	// place the EE thread's FPCR and the IOP thread's FPCR are reapplied.
	Pcsx2Config::CpuOptions before, after;
	after = before;
	EXPECT_TRUE(before == after);
	after.FPUFPCR = FpcrWith(false, FPRoundMode::Nearest); // the default is ChopZero with flush-to-zero
	ASSERT_NE(before.FPUFPCR.bitmask, after.FPUFPCR.bitmask);
	EXPECT_FALSE(before == after);
}

TEST(IopThreadFpcr, TheFirstIopCodeAfterAnFpcrChangeRunsWithTheNewValue)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	FpcrGuard fpcr;

	const FPControlRegister a = FpcrWith(true, FPRoundMode::Nearest);
	const FPControlRegister b = FpcrWith(false, FPRoundMode::ChopZero);
	const FPControlRegister c = FpcrWith(true, FPRoundMode::PositiveInfinity);
	ASSERT_NE(a.bitmask, b.bitmask);
	ASSERT_NE(b.bitmask, c.bitmask);
	ASSERT_NE(a.bitmask, c.bitmask);

	EmuConfig.Cpu.FPUFPCR = a;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	EXPECT_EQ(FpcrOnIopThreadNow(), a.bitmask) << "the thread starts with the EE's FPCR";

	// A settings change mid-run, as CheckForCPUConfigChanges makes it: set the EE thread's FPCR, tell the IOP thread.
	EmuConfig.Cpu.FPUFPCR = b;
	FPControlRegister::SetCurrent(b);
	IopThread::OnCpuConfigChanged();
	EXPECT_EQ(FpcrOnIopThreadNow(), b.bitmask) << "the first IOP code after the change must already see the new FPCR";

	EmuConfig.Cpu.FPUFPCR = c;
	FPControlRegister::SetCurrent(c);
	IopThread::OnCpuConfigChanged();
	IopThread::OnCpuConfigChanged(); // a second call with nothing new changes nothing
	EXPECT_EQ(FpcrOnIopThreadNow(), c.bitmask);

	// Back to the first value: the last change wins.
	EmuConfig.Cpu.FPUFPCR = a;
	FPControlRegister::SetCurrent(a);
	IopThread::OnCpuConfigChanged();
	EXPECT_EQ(FpcrOnIopThreadNow(), a.bitmask);
}

TEST(IopThreadFpcr, AThreadStartedAfterTheChangeTakesTheNewValueFromTheSettings)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	FpcrGuard fpcr;

	EmuConfig.Cpu.FPUFPCR = FpcrWith(true, FPRoundMode::Nearest);
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	// The thread is turned off by a settings change, and the FPCR changes at the same time: nothing to
	// reapply, and no thread to crash.
	EmuConfig.Speedhacks.iopThread = false;
	const FPControlRegister later = FpcrWith(false, FPRoundMode::NegativeInfinity);
	EmuConfig.Cpu.FPUFPCR = later;
	IopThread::OnCpuConfigChanged();
	EXPECT_FALSE(IopThread::g_thread.IsRunning());

	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	EXPECT_EQ(FpcrOnIopThreadNow(), later.bitmask);
}

TEST(IopThreadFpcr, AChangeDoesNotTouchTheCallersOwnFpcr)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	FpcrGuard fpcr;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	const FPControlRegister mine = FPControlRegister::GetCurrent();
	FPControlRegister other = FpcrWith(true, FPRoundMode::ChopZero);
	ASSERT_NE(mine.bitmask, other.bitmask);
	EmuConfig.Cpu.FPUFPCR = other;
	IopThread::OnCpuConfigChanged();
	IopThread::g_thread.LockstepApplyUpTo(0);
	EXPECT_EQ(FPControlRegister::GetCurrent().bitmask, mine.bitmask)
		<< "OnCpuConfigChanged reapplies the IOP thread's FPCR; the EE thread's is the caller's to set";
}
