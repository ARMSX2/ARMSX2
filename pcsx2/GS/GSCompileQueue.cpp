// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSCompileQueue.h"
#include "GS/GSShaderCompileIndicator.h"

#include "common/Threading.h"
#include "common/Timer.h"

#include <algorithm>

class GSCompileQueue::Job
{
public:
	enum class State : u8
	{
		Queued,
		Running,
		Done,
		Cancelled,
	};

	std::function<void()> work;
	State state = State::Queued; ///< Guarded by the queue's mutex.
	Priority priority = Priority::Idle; ///< The deque it counts in; entries in other deques are stale.
};

GSCompileQueue::GSCompileQueue() = default;

GSCompileQueue::~GSCompileQueue()
{
	Stop();
}

u32 GSCompileQueue::GetDefaultWorkerCount()
{
	const u32 hw = std::max(std::thread::hardware_concurrency(), 2u);
	return std::clamp(hw / 2 - 1, 1u, 4u);
}

void GSCompileQueue::Start(u32 workers, const char* thread_name)
{
	if (!m_workers.empty())
		return;
	{
		std::unique_lock lock(m_mutex);
		m_stop = false;
	}
	for (u32 i = 0; i < workers; i++)
		m_workers.emplace_back(&GSCompileQueue::WorkerThread, this, std::string(thread_name));
}

void GSCompileQueue::Stop()
{
	{
		std::unique_lock lock(m_mutex);
		m_stop = true;
		m_work_cv.notify_all();
	}
	for (std::thread& worker : m_workers)
		worker.join();
	m_workers.clear();
}

GSCompileQueue::Ticket GSCompileQueue::Submit(Priority priority, std::function<void()> work)
{
	Ticket ticket = std::make_shared<Job>();
	ticket->work = std::move(work);
	ticket->priority = priority;
	std::unique_lock lock(m_mutex);
	m_queues[static_cast<size_t>(priority)].push_back(ticket);
	m_work_cv.notify_one();
	return ticket;
}

bool GSCompileQueue::IsDone(const Ticket& ticket) const
{
	std::unique_lock lock(m_mutex);
	return ticket->state == Job::State::Done;
}

u64 GSCompileQueue::Finish(const Ticket& ticket)
{
	std::unique_lock lock(m_mutex);
	if (ticket->state == Job::State::Queued || ticket->state == Job::State::Cancelled)
	{
		// Its queue entry is now stale; workers skip anything not Queued.
		ticket->state = Job::State::Running;
		lock.unlock();
		Run(ticket);
		return 0;
	}
	if (ticket->state == Job::State::Done)
		return 0;

	const Common::Timer wait_timer;
	m_done_cv.wait(lock, [&ticket]() { return ticket->state == Job::State::Done; });
	return static_cast<u64>(wait_timer.GetTimeNanoseconds());
}

bool GSCompileQueue::Cancel(const Ticket& ticket)
{
	std::unique_lock lock(m_mutex);
	if (ticket->state != Job::State::Queued)
		return false;
	ticket->state = Job::State::Cancelled;
	return true;
}

void GSCompileQueue::Raise(const Ticket& ticket, Priority priority)
{
	std::unique_lock lock(m_mutex);
	if (ticket->state != Job::State::Queued || priority >= ticket->priority)
		return;
	// The old entry goes stale and is skipped when it is reached.
	ticket->priority = priority;
	m_queues[static_cast<size_t>(priority)].push_back(ticket);
}

u32 GSCompileQueue::GetQueuedCount() const
{
	std::unique_lock lock(m_mutex);
	u32 count = 0;
	for (size_t i = 0; i < std::size(m_queues); i++)
	{
		for (const Ticket& t : m_queues[i])
			count += (t->state == Job::State::Queued && t->priority == static_cast<Priority>(i)) ? 1 : 0;
	}
	return count;
}

GSCompileQueue::Ticket GSCompileQueue::PopLocked()
{
	for (size_t i = 0; i < std::size(m_queues); i++)
	{
		std::deque<Ticket>& q = m_queues[i];
		while (!q.empty())
		{
			Ticket t = std::move(q.front());
			q.pop_front();
			if (t->state == Job::State::Queued && t->priority == static_cast<Priority>(i))
				return t;
		}
	}
	return {};
}

void GSCompileQueue::Run(const Ticket& ticket)
{
	ticket->work();
	std::unique_lock lock(m_mutex);
	ticket->state = Job::State::Done;
	// The function and whatever it holds go now, not when the last ticket does.
	std::function<void()> work = std::move(ticket->work);
	m_done_cv.notify_all();
	lock.unlock();
}

void GSCompileQueue::WorkerThread(std::string name)
{
	Threading::SetNameOfCurrentThread(name.c_str());
	GSShaderCompileIndicator::t_background = true;
	// A thread inherits its creator's affinity, and the GS thread may be pinned to one core.
	const Threading::ThreadHandle self = Threading::ThreadHandle::GetForCallingThread();
	self.SetAffinity(0);
	self.SetNicePriority(5);

	std::unique_lock lock(m_mutex);
	for (;;)
	{
		Ticket t;
		m_work_cv.wait(lock, [this, &t]() { return m_stop || (t = PopLocked()) != nullptr; });
		if (!t)
			break;
		t->state = Job::State::Running;
		lock.unlock();
		Run(t);
		lock.lock();
	}
}
