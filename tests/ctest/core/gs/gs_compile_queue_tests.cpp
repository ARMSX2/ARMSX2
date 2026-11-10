// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSCompileQueue (GS/GSCompileQueue.h): the worker pool that builds shaders and pipelines off the GS
// thread. A job runs exactly once, on a worker or on the thread that asks for it, never on both; the
// asking thread waits only for a job a worker has already started; the most urgent queued job is
// taken first.

#include "GS/GSCompileQueue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	/// Holds a worker inside a job until released, so the tests can arrange a job that a worker has
	/// started and not finished.
	class Gate
	{
	public:
		void Enter()
		{
			std::unique_lock lock(m_mutex);
			m_entered = true;
			m_cv.notify_all();
			m_cv.wait(lock, [this]() { return m_open; });
		}
		void WaitEntered()
		{
			std::unique_lock lock(m_mutex);
			m_cv.wait(lock, [this]() { return m_entered; });
		}
		void Open()
		{
			std::unique_lock lock(m_mutex);
			m_open = true;
			m_cv.notify_all();
		}

	private:
		std::mutex m_mutex;
		std::condition_variable m_cv;
		bool m_entered = false;
		bool m_open = false;
	};

	using Priority = GSCompileQueue::Priority;
} // namespace

TEST(GSCompileQueue, AJobRunsOnAWorker)
{
	GSCompileQueue q;
	q.Start(2, "test compile");
	std::atomic<std::thread::id> ran_on{};
	const GSCompileQueue::Ticket t = q.Submit(Priority::Ahead, [&]() { ran_on = std::this_thread::get_id(); });
	q.Finish(t);
	EXPECT_TRUE(q.IsDone(t));
	EXPECT_NE(ran_on.load(), std::thread::id());
	q.Stop();
}

TEST(GSCompileQueue, FinishRunsAQueuedJobOnTheCallingThread)
{
	// No worker: the job can only run where it is asked for.
	GSCompileQueue q;
	std::thread::id ran_on{};
	const GSCompileQueue::Ticket t = q.Submit(Priority::Ahead, [&]() { ran_on = std::this_thread::get_id(); });
	EXPECT_FALSE(q.IsDone(t));
	EXPECT_EQ(q.Finish(t), 0u);
	EXPECT_EQ(ran_on, std::this_thread::get_id());
	EXPECT_TRUE(q.IsDone(t));
}

TEST(GSCompileQueue, FinishWaitsForAJobAWorkerStarted)
{
	GSCompileQueue q;
	q.Start(1, "test compile");
	Gate gate;
	std::atomic<int> runs{0};
	const GSCompileQueue::Ticket t = q.Submit(Priority::Ahead, [&]() {
		gate.Enter();
		runs++;
	});
	gate.WaitEntered();
	std::thread opener([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		gate.Open();
	});
	const u64 waited = q.Finish(t);
	opener.join();
	EXPECT_EQ(runs.load(), 1);
	EXPECT_TRUE(q.IsDone(t));
	EXPECT_GT(waited, 0u);
	q.Stop();
}

TEST(GSCompileQueue, TheMostUrgentQueuedJobRunsFirst)
{
	GSCompileQueue q;
	q.Start(1, "test compile");
	Gate gate;
	std::mutex order_mutex;
	std::vector<int> order;
	const auto record = [&](int n) {
		return [&, n]() {
			std::unique_lock lock(order_mutex);
			order.push_back(n);
		};
	};
	const GSCompileQueue::Ticket blocker = q.Submit(Priority::Ahead, [&]() { gate.Enter(); });
	gate.WaitEntered();
	const GSCompileQueue::Ticket idle = q.Submit(Priority::Idle, record(3));
	const GSCompileQueue::Ticket ahead = q.Submit(Priority::Ahead, record(2));
	const GSCompileQueue::Ticket raised = q.Submit(Priority::Idle, record(1));
	q.Raise(raised, Priority::Now);
	gate.Open();
	// Polled, not finished: a Finish would run a still-queued job on this thread, out of the worker's order.
	while (!q.IsDone(idle) || !q.IsDone(ahead) || !q.IsDone(raised))
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	q.Finish(blocker);
	EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
	q.Stop();
}

TEST(GSCompileQueue, ACancelledJobNeverRuns)
{
	GSCompileQueue q;
	std::atomic<int> runs{0};
	const GSCompileQueue::Ticket t = q.Submit(Priority::Ahead, [&]() { runs++; });
	EXPECT_TRUE(q.Cancel(t));
	q.Start(1, "test compile");
	q.Stop();
	EXPECT_EQ(runs.load(), 0);
	EXPECT_FALSE(q.IsDone(t));
}

TEST(GSCompileQueue, CancelRefusesAJobAWorkerStarted)
{
	GSCompileQueue q;
	q.Start(1, "test compile");
	Gate gate;
	const GSCompileQueue::Ticket t = q.Submit(Priority::Ahead, [&]() { gate.Enter(); });
	gate.WaitEntered();
	EXPECT_FALSE(q.Cancel(t));
	gate.Open();
	q.Finish(t);
	EXPECT_TRUE(q.IsDone(t));
	q.Stop();
}

TEST(GSCompileQueue, StopLeavesQueuedJobsToTheirAskers)
{
	GSCompileQueue q;
	q.Start(1, "test compile");
	Gate gate;
	const GSCompileQueue::Ticket blocker = q.Submit(Priority::Ahead, [&]() { gate.Enter(); });
	gate.WaitEntered();
	std::atomic<int> runs{0};
	const GSCompileQueue::Ticket queued = q.Submit(Priority::Ahead, [&]() { runs++; });
	std::thread opener([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		gate.Open();
	});
	q.Stop();
	opener.join();
	EXPECT_TRUE(q.IsDone(blocker));
	EXPECT_EQ(runs.load(), 0);
	q.Finish(queued);
	EXPECT_EQ(runs.load(), 1);
}

TEST(GSCompileQueue, EveryJobRunsExactlyOnceUnderContention)
{
	GSCompileQueue q;
	q.Start(4, "test compile");
	static constexpr int N = 2000;
	std::vector<std::atomic<int>> runs(N);
	std::vector<GSCompileQueue::Ticket> tickets;
	for (int i = 0; i < N; i++)
		tickets.push_back(q.Submit((i % 3 == 0) ? Priority::Idle : Priority::Ahead, [&runs, i]() { runs[i]++; }));
	// The asking thread takes some jobs itself while the workers take others.
	for (int i = N - 1; i >= 0; i -= 2)
		q.Finish(tickets[i]);
	for (int i = 0; i < N; i++)
		q.Finish(tickets[i]);
	for (int i = 0; i < N; i++)
		EXPECT_EQ(runs[i].load(), 1) << i;
	q.Stop();
}
