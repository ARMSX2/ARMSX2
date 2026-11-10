// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/// The worker pool that builds shaders and pipelines off the GS thread. It knows nothing of what a
/// job builds: the job's function writes its result where its submitter will look.
///
/// A job runs exactly once, either on a worker or on a thread that calls Finish, never on both. A
/// thread that needs a job's result calls Finish: if no worker has started the job it runs it itself
/// (no slower than waiting behind the queue), and it waits only for a job a worker is already running.
/// A submitter that has something else to draw meanwhile polls IsDone instead and never waits.
///
/// Queued jobs are taken most urgent first, in submission order within a priority.
class GSCompileQueue
{
public:
	enum class Priority : u8
	{
		Now, ///< A draw is waiting, or will at once.
		Ahead, ///< Expected soon: a recorded key list, a run's own program.
		Idle, ///< Worth having, needed by nothing yet.
		Count
	};

	class Job;
	using Ticket = std::shared_ptr<Job>;

	GSCompileQueue();
	~GSCompileQueue();

	GSCompileQueue(const GSCompileQueue&) = delete;
	GSCompileQueue& operator=(const GSCompileQueue&) = delete;

	/// Starts the workers: named thread_name, any core, a lower priority than the emulator's threads.
	void Start(u32 workers, const char* thread_name);
	/// Lets running jobs finish and joins the workers. Queued jobs stay queued and do not run unless
	/// someone calls Finish on them; Start again picks them up.
	void Stop();
	bool IsRunning() const { return !m_workers.empty(); }

	/// The number of workers for this machine: half the cores less one, at most four, so the EE, GS and
	/// VU threads keep theirs.
	static u32 GetDefaultWorkerCount();

	Ticket Submit(Priority priority, std::function<void()> work);

	/// The job has run. Never blocks.
	bool IsDone(const Ticket& ticket) const;
	/// Returns when the job has run: here if no worker had started it, else after waiting for the
	/// worker. Returns the nanoseconds spent waiting for a worker.
	u64 Finish(const Ticket& ticket);
	/// Takes a queued job out of the queue for good. False if a worker has started it or it has run.
	bool Cancel(const Ticket& ticket);
	/// Moves a queued job up to priority if that is more urgent than where it sits.
	void Raise(const Ticket& ticket, Priority priority);

	u32 GetQueuedCount() const;

private:
	void WorkerThread(std::string name);
	/// Pops the most urgent job still queued. Caller holds m_mutex.
	Ticket PopLocked();
	void Run(const Ticket& ticket);

	mutable std::mutex m_mutex;
	std::condition_variable m_work_cv; ///< A job was queued, or the workers are to stop.
	std::condition_variable m_done_cv; ///< A job finished.
	std::deque<Ticket> m_queues[static_cast<size_t>(Priority::Count)];
	std::vector<std::thread> m_workers;
	bool m_stop = false;
};
