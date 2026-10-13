// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopChannels.h"

#include <utility>

namespace IopThread
{
	HostCommandQueue::HostCommandQueue() = default;
	HostCommandQueue::~HostCommandQueue() = default;

	HostCommandQueue::Ticket HostCommandQueue::Post(Command command)
	{
		pxAssertMsg(static_cast<bool>(command), "HostCommandQueue::Post() of an empty command");

		std::lock_guard<std::mutex> lock(m_mutex);
		m_queue.push_back(std::move(command));
		const Ticket ticket = ++m_posted;

		// No data travels through this flag; the mutex orders the commands. It is set and cleared
		// only under the mutex, so a Post() after RunPending() has swapped the list out sets it
		// again and the commands in the new list are not missed. Release is the design's choice
		// (section 3.9) and costs nothing here.
		m_pending.store(true, std::memory_order_release);
		return ticket;
	}

	size_t HostCommandQueue::RunPending()
	{
		// A command that calls back in would run the later commands before the rest of this batch.
		if (m_running)
			return 0;

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			// m_batch is empty here (cleared at the end of the last call), so this hands the
			// posters an empty list with the old batch's capacity.
			m_batch.swap(m_queue);
			m_pending.store(false, std::memory_order_relaxed);
		}

		m_running = true;
		u64 done = m_completed.load(std::memory_order_relaxed); // only this thread stores it
		for (Command& command : m_batch)
		{
			command();

			// Pairs with the acquire load in IsDone() and CompletedCount(): what the command wrote
			// is visible to a thread that sees its ticket complete.
			m_completed.store(++done, std::memory_order_release);
		}
		const size_t ran = m_batch.size();
		m_batch.clear();
		m_running = false;
		return ran;
	}
} // namespace IopThread
