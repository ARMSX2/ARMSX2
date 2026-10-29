// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Host threads handing work to the IOP thread: the host command queue's gate, what happens to a
// command that arrives while the IOP thread stops, and the physical-disc thread's new-disc callback,
// which Lockstep and Threaded hand to the IOP thread instead of calling it on the disc thread.

#include "CDVD/CDVDdiscReader.h"
#include "IopThread/IopChannels.h"
#include "IopThreadTestSupport.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace iopt;
using namespace std::chrono_literals;

TEST(HostCommandQueueGate, ANewQueueRefusesUntilItIsOpened)
{
	IopThread::HostCommandQueue queue;
	EXPECT_FALSE(queue.IsOpen());
	int ran = 0;
	EXPECT_EQ(queue.TryPost([&] { ran++; }), 0u);
	EXPECT_FALSE(queue.HasPending());
	EXPECT_EQ(queue.RunPending(), 0u);
	EXPECT_EQ(ran, 0);

	queue.Open();
	EXPECT_TRUE(queue.IsOpen());
	const auto first = queue.TryPost([&] { ran++; });
	const auto second = queue.TryPost([&] { ran += 10; });
	EXPECT_EQ(first, 1u);
	EXPECT_EQ(second, 2u);
	EXPECT_EQ(queue.RunPending(), 2u);
	EXPECT_EQ(ran, 11);
}

TEST(HostCommandQueueGate, ClosingRefusesNewCommandsAndKeepsTheQueuedOnes)
{
	IopThread::HostCommandQueue queue;
	queue.Open();
	std::vector<int> order;
	queue.TryPost([&] { order.push_back(1); });
	queue.Close();
	EXPECT_FALSE(queue.IsOpen());
	EXPECT_EQ(queue.TryPost([&] { order.push_back(2); }), 0u) << "a closed queue refuses";
	EXPECT_EQ(queue.RunPending(), 1u) << "what was queued before the gate closed still runs";
	EXPECT_EQ(order, std::vector<int>{1});

	queue.Open();
	EXPECT_NE(queue.TryPost([&] { order.push_back(3); }), 0u);
	queue.RunPending();
	EXPECT_EQ(order, (std::vector<int>{1, 3}));
}

TEST(HostCommandQueueGate, PostIgnoresTheGateAndSharesTheTicketCount)
{
	IopThread::HostCommandQueue queue;
	// Post() is the unconditional form the lifecycle code uses (the FPCR re-apply).
	const auto a = queue.Post([] {});
	queue.Open();
	const auto b = queue.TryPost([] {});
	queue.Close();
	const auto c = queue.Post([] {});
	EXPECT_EQ(a, 1u);
	EXPECT_EQ(b, 2u);
	EXPECT_EQ(c, 3u);
	EXPECT_EQ(queue.RunPending(), 3u);
}

// Many threads against a gate that opens and closes: every command a poster got a ticket for runs exactly once,
// and every command it was refused for is the poster's to run.
TEST(HostCommandQueueGate, ACommandIsEitherAcceptedOrRefusedNeverBoth)
{
	IopThread::HostCommandQueue queue;
	constexpr int kPosters = 4, kPerPoster = 4000;
	std::atomic<int> accepted{0}, refused{0};
	std::atomic<int> ran_by_queue{0}, ran_by_poster{0};
	std::atomic<bool> done{false};

	std::vector<std::thread> posters;
	for (int p = 0; p < kPosters; p++)
	{
		posters.emplace_back([&] {
			for (int i = 0; i < kPerPoster; i++)
			{
				if (queue.TryPost([&] { ran_by_queue++; }))
					accepted++;
				else
				{
					refused++;
					ran_by_poster++;
				}
			}
		});
	}
	std::thread gate([&] {
		while (!done.load())
		{
			queue.Open();
			std::this_thread::yield();
			queue.Close();
			queue.RunPending();
		}
	});
	for (auto& t : posters)
		t.join();
	done.store(true);
	gate.join();
	queue.Close();
	queue.RunPending();

	EXPECT_EQ(accepted.load() + refused.load(), kPosters * kPerPoster);
	EXPECT_EQ(ran_by_queue.load(), accepted.load()) << "every accepted command ran, once";
	EXPECT_EQ(ran_by_poster.load(), refused.load());
}

TEST(IopThreadHostCommands, OffHasNoThreadSoAPostIsRefused)
{
	SettingsGuard guard;
	IopThread::ChooseMode();
	ASSERT_FALSE(IopThread::g_thread.IsRunning());
	int ran = 0;
	EXPECT_EQ(IopThread::g_thread.TryPostHostCommand([&] { ran++; }), 0u);
	EXPECT_FALSE(IopThread::TryPostToIop([&] { ran++; }));
	EXPECT_EQ(ran, 0) << "a refused command is not run; the caller makes the call itself";
}

TEST(IopThreadHostCommands, ARunningLockstepThreadRunsAPostedCommandInTheNextHandoff)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	std::atomic<int> ran{0};
	std::atomic<bool> on_iop_thread{false};
	const auto ticket = IopThread::g_thread.TryPostHostCommand([&] {
		on_iop_thread = IopThread::g_thread.OnIopThread();
		ran++;
	});
	ASSERT_NE(ticket, 0u);

	// Host commands run only inside a handoff in Lockstep, so the EE is never running at the same time.
	std::this_thread::sleep_for(30ms);
	EXPECT_EQ(ran.load(), 0) << "Lockstep applies host commands between handoffs, not when they arrive";
	EXPECT_FALSE(IopThread::g_thread.IsHostCommandDone(ticket));

	IopThread::g_thread.LockstepApplyUpTo(0);
	EXPECT_EQ(ran.load(), 1);
	EXPECT_TRUE(on_iop_thread.load()) << "the command runs on the IOP thread";
	EXPECT_TRUE(IopThread::g_thread.IsHostCommandDone(ticket));
}

TEST(IopThreadHostCommands, CommandsRunInTheOrderTheyWerePostedFromSeveralThreads)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	std::mutex mutex;
	std::vector<int> order;
	std::vector<std::thread> posters;
	std::atomic<int> next{0};
	for (int t = 0; t < 3; t++)
	{
		posters.emplace_back([&] {
			for (int i = 0; i < 50; i++)
			{
				// The ticket is taken under the same lock as the number, so ticket order is number order.
				std::lock_guard lock(mutex);
				const int n = next++;
				ASSERT_NE(IopThread::g_thread.TryPostHostCommand([&order, n] { order.push_back(n); }), 0u);
			}
		});
	}
	for (auto& t : posters)
		t.join();
	IopThread::g_thread.LockstepApplyUpTo(0);
	ASSERT_EQ(order.size(), 150u);
	for (int i = 0; i < 150; i++)
		EXPECT_EQ(order[i], i);
}

TEST(IopThreadHostCommands, AfterTheThreadStopsAPostIsRefused)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	EXPECT_TRUE(IopThread::TryPostToIop([] {}));

	IopThread::StopThread();
	ASSERT_FALSE(IopThread::g_thread.IsRunning());
	int ran = 0;
	EXPECT_FALSE(IopThread::TryPostToIop([&] { ran++; }));
	EXPECT_EQ(ran, 0);
}

// A command that was accepted before Stop() is not lost: the IOP thread runs it before it exits.
TEST(IopThreadHostCommands, ACommandQueuedWhenTheThreadStopsRunsOnTheIopThreadBeforeItExits)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	std::atomic<int> ran{0};
	std::atomic<bool> on_iop_thread{false};
	ASSERT_TRUE(IopThread::TryPostToIop([&] {
		on_iop_thread = IopThread::g_thread.OnIopThread();
		ran++;
	}));
	EXPECT_EQ(ran.load(), 0);

	IopThread::StopThread();
	EXPECT_EQ(ran.load(), 1) << "the disc change that arrived as the thread stopped is applied, not dropped";
	EXPECT_TRUE(on_iop_thread.load());
}

TEST(IopThreadHostCommands, ARestartedThreadAcceptsCommandsAgain)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	IopThread::StopThread();
	EXPECT_FALSE(IopThread::TryPostToIop([] {}));

	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	int ran = 0;
	EXPECT_TRUE(IopThread::TryPostToIop([&] { ran++; }));
	IopThread::g_thread.LockstepApplyUpTo(0);
	EXPECT_EQ(ran, 1);
}

// A host thread posting while the CPU thread stops and starts the IOP thread, over and over. Every command is
// run exactly once: by the IOP thread if the thread accepted it (in a handoff, or as it stopped), by the poster
// if it did not.
TEST(IopThreadHostCommands, NothingIsLostOrRunTwiceWhileTheThreadStopsAndStarts)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	constexpr size_t kMaxCommands = 1u << 22;
	std::vector<std::atomic<u8>> runs(kMaxCommands);
	std::atomic<size_t> posted{0};
	std::atomic<size_t> by_iop{0}, by_poster{0};
	std::atomic<bool> stop{false};

	std::thread poster([&] {
		for (size_t i = 0; i < kMaxCommands && !stop.load(std::memory_order_relaxed); i++)
		{
			const bool accepted = IopThread::TryPostToIop([&, i] {
				runs[i]++;
				by_iop += IopThread::g_thread.OnIopThread() ? 1 : 0;
			});
			if (!accepted)
			{
				runs[i]++;
				by_poster++;
			}
			posted.store(i + 1, std::memory_order_relaxed);
			if ((i & 15) == 0)
				std::this_thread::yield();
		}
	});

	int cycles = 0;
	while (cycles < 300 && posted.load(std::memory_order_relaxed) < kMaxCommands)
	{
		if (IopThread::g_thread.IsRunning())
		{
			IopThread::g_thread.LockstepApplyUpTo(0);
			IopThread::StopThread();
		}
		else
		{
			IopThread::ChooseMode();
			IopThread::SyncThread();
		}
		cycles++;
		std::this_thread::sleep_for(std::chrono::microseconds(50));
	}
	stop.store(true);
	poster.join();
	// Commands still queued behind the last Start() run at the last Stop().
	IopThread::StopThread();

	const size_t n = posted.load();
	std::printf("[  stats   ] %d start/stop cycles, %zu commands posted: %zu run on the IOP thread, %zu run by the poster after a refusal\n", cycles, n,
		by_iop.load(), by_poster.load());
	size_t wrong = 0;
	for (size_t i = 0; i < n; i++)
		wrong += (runs[i].load() != 1) ? 1 : 0;
	EXPECT_EQ(wrong, 0u) << "commands that ran zero times or more than once, of " << n;
	EXPECT_EQ(by_iop.load() + by_poster.load(), n);
	EXPECT_GT(by_iop.load(), 0u) << "the IOP thread took some";
	EXPECT_GT(by_poster.load(), 0u) << "and the gate refused some while the thread was stopped";
}

// A Threaded loop that has nothing to run sleeps. A command from a host thread must wake it: nothing else would.
// (The object is driven directly with a fake IOP that never has budget, so no VM is involved.)
TEST(IopThreadHostCommands, AParkedThreadedLoopIsWokenByAnAcceptedCommand)
{
	IopThread::Thread thread;
	IopThread::Params params;
	params.mode = IopThreadMode::Threaded;
	params.run_slice = [](u64 from, u64 /*target*/) { return from; };
	ASSERT_TRUE(thread.Start(params));

	auto wait_for = [](auto&& condition) {
		for (int i = 0; i < 2000 && !condition(); i++)
			std::this_thread::sleep_for(1ms);
		return condition();
	};
	ASSERT_TRUE(wait_for([&] { return thread.IsParked(); })) << "the loop has no budget and should sleep";

	std::atomic<bool> on_iop_thread{false};
	const auto ticket = thread.TryPostHostCommand([&] { on_iop_thread = thread.OnIopThread(); });
	ASSERT_NE(ticket, 0u);
	EXPECT_TRUE(wait_for([&] { return thread.IsHostCommandDone(ticket); })) << "the sleeping loop was not woken for the command";
	EXPECT_TRUE(on_iop_thread.load());

	thread.Stop();
	EXPECT_EQ(thread.TryPostHostCommand([] {}), 0u) << "a stopped thread refuses";
}

TEST(IopThreadHostCommands, AThreadedLoopRunsACommandAcceptedAsItStops)
{
	IopThread::Thread thread;
	IopThread::Params params;
	params.mode = IopThreadMode::Threaded;
	params.run_slice = [](u64 from, u64 /*target*/) { return from; };
	ASSERT_TRUE(thread.Start(params));

	std::atomic<int> ran{0};
	ASSERT_NE(thread.TryPostHostCommand([&] { ran++; }), 0u);
	thread.Stop(); // whether or not the loop got to it first, it has run by the time Stop() returns
	EXPECT_EQ(ran.load(), 1);
}

namespace
{
	std::atomic<int> s_disc_calls{0};
	std::thread::id s_disc_call_thread;
	std::atomic<bool> s_disc_call_on_iop_thread{false};
	std::atomic<bool> s_flag_during_call{false};

	void RecordNewDisc()
	{
		s_disc_call_thread = std::this_thread::get_id();
		s_disc_call_on_iop_thread = IopThread::g_thread.OnIopThread();
		s_flag_during_call = weAreInNewDiskCB;
		s_disc_calls++;
	}

	class DiscReaderGuard
	{
	public:
		DiscReaderGuard()
			: m_callback(newDiscCB)
		{
			newDiscCB = &RecordNewDisc;
			s_disc_calls = 0;
			s_disc_call_on_iop_thread = false;
			s_flag_during_call = false;
		}
		~DiscReaderGuard()
		{
			newDiscCB = m_callback;
			weAreInNewDiskCB = false;
		}

	private:
		void (*m_callback)();
	};
} // namespace

TEST(NewDiscCallback, OffCallsItOnTheDiscThreadAsItAlwaysDid)
{
	SettingsGuard guard;
	IopThread::ChooseMode();
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	DiscReaderGuard reader;

	std::thread disc_thread([] { cdvdCallNewDiscCB(); });
	const std::thread::id disc_thread_id = disc_thread.get_id();
	disc_thread.join();

	EXPECT_EQ(s_disc_calls.load(), 1);
	EXPECT_EQ(s_disc_call_thread, disc_thread_id) << "Off calls it on the thread that noticed the disc";
	EXPECT_TRUE(s_flag_during_call.load()) << "with the flag the reader's reads check, as before";
	EXPECT_FALSE(weAreInNewDiskCB);
}

TEST(NewDiscCallback, LockstepHandsItToTheIopThreadInsteadOfCallingItOnTheDiscThread)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	DiscReaderGuard reader;

	std::thread::id disc_thread_id;
	std::thread disc_thread([&] {
		disc_thread_id = std::this_thread::get_id();
		cdvdCallNewDiscCB();
	});
	disc_thread.join();

	EXPECT_EQ(s_disc_calls.load(), 0) << "the disc thread must not run the IOP's disc change itself";
	EXPECT_FALSE(weAreInNewDiskCB);

	IopThread::g_thread.LockstepApplyUpTo(0);
	EXPECT_EQ(s_disc_calls.load(), 1);
	EXPECT_TRUE(s_disc_call_on_iop_thread.load());
	EXPECT_NE(s_disc_call_thread, disc_thread_id);
	EXPECT_FALSE(s_flag_during_call.load())
		<< "the IOP thread reads through the disc thread's queue, which is free once the callback has returned";
}

TEST(NewDiscCallback, TheCallbackStillRunsWhenTheThreadIsStoppedMeanwhile)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	DiscReaderGuard reader;

	std::thread disc_thread([] { cdvdCallNewDiscCB(); });
	disc_thread.join();
	EXPECT_EQ(s_disc_calls.load(), 0);
	IopThread::StopThread(); // the settings change that turns the IOP thread off
	EXPECT_EQ(s_disc_calls.load(), 1) << "queued, then run as the thread stopped";

	// Off again: the callback is made directly.
	std::thread again([] { cdvdCallNewDiscCB(); });
	again.join();
	EXPECT_EQ(s_disc_calls.load(), 2);
	EXPECT_FALSE(s_disc_call_on_iop_thread.load());
}
