// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The register allocator's state belongs to the thread that is compiling.
//
// The EE recompiler, the IOP recompiler and mVU's COP2 macro mode share one allocator (iCore-arm64.cpp).
// With the IOP on its own thread an EE block and an IOP block compile at the same time, so all allocator
// state a compile touches between its start and its end has to be per thread: the GPR and NEON maps, the
// copies a branch saves and restores, the current instruction's liveness info, and the allocation
// counters. A compile starts by clearing the maps (_initArm64GPRregs, _initArm64NEONregs), so a shared
// map would be wiped under a compile that is still using it.
//
// Each test lets one thread get part way through a compile, lets a second thread run a whole compile's
// worth of allocator calls, and then checks that the first thread's state is unchanged.

#include "arm64/iCore-arm64.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

namespace
{
	// Two threads take turns: Give() hands the turn to the other and Wait() blocks until it comes back. A turn
	// that does not return in 20 s fails the test instead of hanging it.
	class Turns
	{
	public:
		bool Wait(int who)
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			return m_cv.wait_for(lock, std::chrono::seconds(20), [&] { return m_turn == who; });
		}

		void Give(int who)
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_turn = who;
			}
			m_cv.notify_all();
		}

	private:
		std::mutex m_mutex;
		std::condition_variable m_cv;
		int m_turn = 0;
	};

	// A thread's whole view of the allocator, to compare before and after another thread has used its own.
	struct AllocView
	{
		_arm64gprregs gprs[NUM_ARM_GPR_REGS];
		_arm64gprregs saved_gprs[NUM_ARM_GPR_REGS];
		_arm64neonregs neon[NUM_ARM_NEON_REGS];
		_arm64neonregs saved_neon[NUM_ARM_NEON_REGS];
		EEINST* cur_inst;
		u16 gpr_counter;
		u16 neon_counter;
	};

	AllocView Capture()
	{
		AllocView v;
		std::memcpy(v.gprs, arm64gprs, sizeof(v.gprs));
		std::memcpy(v.saved_gprs, s_saveArm64GPRregs, sizeof(v.saved_gprs));
		std::memcpy(v.neon, arm64neon, sizeof(v.neon));
		std::memcpy(v.saved_neon, s_saveArm64NEONregs, sizeof(v.saved_neon));
		v.cur_inst = g_pCurInstInfo;
		v.gpr_counter = g_arm64AllocCounter;
		v.neon_counter = g_neonAllocCounter;
		return v;
	}

	bool Same(const AllocView& a, const AllocView& b)
	{
		return std::memcmp(a.gprs, b.gprs, sizeof(a.gprs)) == 0 && std::memcmp(a.saved_gprs, b.saved_gprs, sizeof(a.saved_gprs)) == 0 &&
		       std::memcmp(a.neon, b.neon, sizeof(a.neon)) == 0 && std::memcmp(a.saved_neon, b.saved_neon, sizeof(a.saved_neon)) == 0 &&
		       a.cur_inst == b.cur_inst && a.gpr_counter == b.gpr_counter && a.neon_counter == b.neon_counter;
	}

	// What a compile does to the allocator that needs no code buffer: clear the maps, point at the
	// instruction being compiled, allocate registers and keep a copy across a (simulated) branch. The
	// allocations are IOP guest registers 1..ngpr, written and not read, so they emit nothing and have the
	// IOP's 16-register pool to draw from.
	void FakeCompileStart(EEINST* inst, int ngpr, int nneon)
	{
		_initArm64GPRregs();
		_initArm64NEONregs();
		g_pCurInstInfo = inst;
		for (int i = 0; i < ngpr; i++)
			_allocArm64GPR(ARM64TYPE_PSX, 1 + i, MODE_WRITE);
		for (int i = 0; i < nneon; i++)
			_allocTempNEONreg();
		// A branch saves the maps and restores them on the other arm.
		std::memcpy(s_saveArm64GPRregs, arm64gprs, sizeof(arm64gprs));
		std::memcpy(s_saveArm64NEONregs, arm64neon, sizeof(arm64neon));
	}

	// The thread under test starts a compile and stops half way; `other` is what the second thread does while
	// it waits. Returns whether the first thread's state came back unchanged.
	void RunInterleaved(const std::function<void()>& other, bool* unchanged, bool* timed_out)
	{
		Turns turns;
		EEINST first_inst = {};
		AllocView before;
		AllocView after;
		bool first_timed_out = false;
		bool second_timed_out = false;

		std::thread first([&] {
			FakeCompileStart(&first_inst, 5, 3);
			before = Capture();
			turns.Give(2);
			if (!turns.Wait(1))
			{
				first_timed_out = true;
				return;
			}
			after = Capture();
		});
		std::thread second([&] {
			if (!turns.Wait(2))
			{
				second_timed_out = true;
				turns.Give(1);
				return;
			}
			other();
			turns.Give(1);
		});
		first.join();
		second.join();
		*timed_out = first_timed_out || second_timed_out;
		*unchanged = !*timed_out && Same(before, after);
	}
} // namespace

TEST(Arm64AllocatorThreads, AnotherThreadsCompileDoesNotDisturbTheMaps)
{
	bool unchanged = false, timed_out = false;
	RunInterleaved(
		[] {
			EEINST other_inst = {};
			FakeCompileStart(&other_inst, 9, 6);
		},
		&unchanged, &timed_out);
	ASSERT_FALSE(timed_out);
	EXPECT_TRUE(unchanged) << "a compile on another thread changed this thread's allocator state";
}

TEST(Arm64AllocatorThreads, TheMapsOfBothThreadsHoldWhatEachAllocated)
{
	std::atomic<int> a_in_use{-1}, b_in_use{-1};
	Turns turns;
	std::atomic<bool> timed_out{false};

	std::thread a([&] {
		EEINST inst = {};
		FakeCompileStart(&inst, 4, 0);
		turns.Give(2);
		if (!turns.Wait(1))
		{
			timed_out = true;
			return;
		}
		int n = 0;
		for (int i = 0; i < NUM_ARM_GPR_REGS; i++)
			n += arm64gprs[i].inuse ? 1 : 0;
		a_in_use = n;
	});
	std::thread b([&] {
		if (!turns.Wait(2))
		{
			timed_out = true;
			turns.Give(1);
			return;
		}
		EEINST inst = {};
		FakeCompileStart(&inst, 7, 0);
		int n = 0;
		for (int i = 0; i < NUM_ARM_GPR_REGS; i++)
			n += arm64gprs[i].inuse ? 1 : 0;
		b_in_use = n;
		turns.Give(1);
	});
	a.join();
	b.join();
	ASSERT_FALSE(timed_out);
	EXPECT_EQ(a_in_use.load(), 4);
	EXPECT_EQ(b_in_use.load(), 7);
}

TEST(Arm64AllocatorThreads, TheInstructionInfoPointerIsPerThread)
{
	EEINST first_inst = {}, second_inst = {};
	Turns turns;
	EEINST* seen_by_first = nullptr;
	bool timed_out = false;

	std::thread first([&] {
		g_pCurInstInfo = &first_inst;
		turns.Give(2);
		if (!turns.Wait(1))
		{
			timed_out = true;
			return;
		}
		seen_by_first = g_pCurInstInfo;
	});
	std::thread second([&] {
		if (!turns.Wait(2))
		{
			timed_out = true;
			turns.Give(1);
			return;
		}
		g_pCurInstInfo = &second_inst;
		turns.Give(1);
	});
	first.join();
	second.join();
	ASSERT_FALSE(timed_out);
	EXPECT_EQ(seen_by_first, &first_inst);
}

TEST(Arm64AllocatorThreads, TheAllocationCountersArePerThread)
{
	// _initArm64GPRregs zeroes the counter and every allocation bumps it: after this thread's five
	// allocations and another thread's nine, this thread's counter is still five.
	bool unchanged = false, timed_out = false;
	u16 gpr_counter_seen = 0xffff, neon_counter_seen = 0xffff;
	Turns turns;
	EEINST first_inst = {};

	std::thread first([&] {
		FakeCompileStart(&first_inst, 5, 3);
		turns.Give(2);
		if (!turns.Wait(1))
		{
			timed_out = true;
			return;
		}
		gpr_counter_seen = g_arm64AllocCounter;
		neon_counter_seen = g_neonAllocCounter;
	});
	std::thread second([&] {
		if (!turns.Wait(2))
		{
			timed_out = true;
			turns.Give(1);
			return;
		}
		EEINST inst = {};
		FakeCompileStart(&inst, 9, 6);
		turns.Give(1);
	});
	first.join();
	second.join();
	(void)unchanged;
	ASSERT_FALSE(timed_out);
	EXPECT_EQ(gpr_counter_seen, 5);
	EXPECT_EQ(neon_counter_seen, 3);
}

TEST(Arm64AllocatorThreads, TheNextFreeRegisterSearchStartsWhereThisThreadLeftIt)
{
	// _getFreeArm64GPR rotates its starting point after every allocation (g_arm64checknext). A compile on
	// another thread must not move this thread's rotation, or the same block compiles to different
	// registers depending on what the other thread was doing, which breaks the recompiler's
	// reproducibility.
	auto next_alloc_after = [](bool with_other_thread) {
		int result = -2;
		Turns turns;
		bool timed_out = false;
		std::thread first([&] {
			EEINST inst = {};
			FakeCompileStart(&inst, 3, 0);
			turns.Give(2);
			if (!turns.Wait(1))
			{
				timed_out = true;
				return;
			}
			result = _allocArm64GPR(ARM64TYPE_PSX, 20, MODE_WRITE);
		});
		std::thread second([&] {
			if (!turns.Wait(2))
			{
				timed_out = true;
				turns.Give(1);
				return;
			}
			if (with_other_thread)
			{
				EEINST inst = {};
				FakeCompileStart(&inst, 11, 0);
			}
			turns.Give(1);
		});
		first.join();
		second.join();
		EXPECT_FALSE(timed_out);
		return result;
	};
	const int alone = next_alloc_after(false);
	const int disturbed = next_alloc_after(true);
	EXPECT_GE(alone, 0);
	EXPECT_EQ(alone, disturbed);
}

TEST(Arm64AllocatorThreads, AThreadThatNeverCompiledStartsEmpty)
{
	// A new thread's allocator is empty, as the first compile on the main thread found it.
	bool empty = false;
	std::thread t([&] {
		AllocView v = Capture();
		empty = (v.cur_inst == nullptr) && v.gpr_counter == 0 && v.neon_counter == 0;
		for (int i = 0; i < NUM_ARM_GPR_REGS; i++)
			empty = empty && !v.gprs[i].inuse && !v.saved_gprs[i].inuse;
		for (int i = 0; i < NUM_ARM_NEON_REGS; i++)
			empty = empty && !v.neon[i].inuse && !v.saved_neon[i].inuse;
	});
	t.join();
	EXPECT_TRUE(empty);
}
