// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pad and USB vibration are calls that only the CPU thread may make and nothing waits for. Off and
// Lockstep make the call where the IOP code runs; Threaded posts it to the CPU thread.

#include "IopThread/EeBridge.h"
#include "IopThreadTestSupport.h"

#include <atomic>
#include <vector>

using namespace iopt;

TEST(CallOnCpuThread, OffAndLockstepCallItHere)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StubHost::TakeCpuThreadTasks();

	IopThread::ChooseMode();
	int ran = 0;
	IopThread::CallOnCpuThread([&] { ran++; });
	EXPECT_EQ(ran, 1);
	EXPECT_TRUE(StubHost::TakeCpuThreadTasks().empty()) << "Off posts nothing";

	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	// In Lockstep the caller is the IOP thread and the CPU thread is blocked in the handoff, so a direct call is
	// legal and keeps the order Off has.
	std::atomic<int> ran_on_iop{0};
	IopThread::g_thread.PostHostCommand([&] {
		IopThread::CallOnCpuThread([&] { ran_on_iop += IopThread::g_thread.OnIopThread() ? 1 : 0; });
	});
	IopThread::g_thread.LockstepApplyUpTo(0);
	EXPECT_EQ(ran_on_iop.load(), 1);
	EXPECT_TRUE(StubHost::TakeCpuThreadTasks().empty()) << "Lockstep posts nothing";
}

TEST(CallOnCpuThread, ThreadedPostsItWithoutBlockingAndDoesNotCallItOnTheCallersThread)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	StubHost::TakeCpuThreadTasks();

	// Threaded is not built, so the mode variable is set by hand while the call is made on the IOP thread.
	std::atomic<int> ran{0};
	IopThread::s_mode = IopThreadMode::Threaded;
	IopThread::g_thread.PostHostCommand([&] { IopThread::CallOnCpuThread([&] { ran++; }); });
	IopThread::g_thread.LockstepApplyUpTo(0);
	IopThread::s_mode = IopThreadMode::Lockstep;

	EXPECT_EQ(ran.load(), 0) << "the IOP thread must not make the call";
	std::vector<StubHost::CpuThreadTask> tasks = StubHost::TakeCpuThreadTasks();
	ASSERT_EQ(tasks.size(), 1u);
	EXPECT_FALSE(tasks[0].block) << "asynchronous: the EE may be spinning on the IOP";
	tasks[0].function();
	EXPECT_EQ(ran.load(), 1);
}

TEST(CallOnCpuThread, PadVibrationFollowsTheSamePathAsAnyOtherCpuThreadCall)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	StubHost::TakeCpuThreadTasks();

	// Off and Lockstep: InputManager::SetPadVibrationIntensity is called where the pad runs, and nothing is posted.
	IopThread::g_thread.PostHostCommand([] {
		EeBridge::SetPadVibration(0, 0.5f, 0.25f);
		EeBridge::SetUsbVibration(1, 0.5f, 0.0f);
	});
	IopThread::g_thread.LockstepApplyUpTo(0);
	EXPECT_TRUE(StubHost::TakeCpuThreadTasks().empty());

	// Threaded: both are posted, neither blocks, and running the tasks makes the call on the CPU thread.
	IopThread::s_mode = IopThreadMode::Threaded;
	IopThread::g_thread.PostHostCommand([] {
		EeBridge::SetPadVibration(0, 0.5f, 0.25f);
		EeBridge::SetUsbVibration(1, 0.5f, 0.0f);
	});
	IopThread::g_thread.LockstepApplyUpTo(0);
	IopThread::s_mode = IopThreadMode::Lockstep;
	std::vector<StubHost::CpuThreadTask> tasks = StubHost::TakeCpuThreadTasks();
	ASSERT_EQ(tasks.size(), 2u);
	for (auto& task : tasks)
	{
		EXPECT_FALSE(task.block);
		task.function(); // no bindings in the test binary, so the call finds nothing to rumble
	}
}
