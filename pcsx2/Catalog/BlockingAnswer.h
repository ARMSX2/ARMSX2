// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace Catalog
{
	// One answer to a question that one thread asks and another thread answers: the CPU thread asks
	// the user something while the VM is starting, the GS thread draws the question and takes the
	// answer.
	//
	//   auto question = std::make_shared<BlockingAnswer<bool>>();
	//   hand_to_the_other_thread([responder = BlockingAnswer<bool>::Responder(question, false)] {
	//       show the question; when it is answered, call responder(answer);
	//   });
	//   bool answer = question->Wait(false, 16ms, [] { keep the machine running; return still_open; });
	//
	// Wait() always returns. It returns the answer if one was given, and otherwise the fallback when
	//   - the Responder (and every copy of it) is destroyed without answering, which is what happens
	//     to a dialog's callback when something else replaces the dialog or the UI shuts down, or
	//   - the keep-waiting function returns false, for a UI that went away without destroying its
	//     callbacks.
	// The first answer wins; a later one is ignored, so a late click after Wait() gave up is harmless.
	//
	// The thread that waits must not need the thread that answers to wait for it: nothing here
	// queues work onto either thread.
	template <typename T>
	class BlockingAnswer
	{
	public:
		/// The answering side. Copyable, so it can sit in a std::function. Call it with the answer.
		class Responder
		{
		public:
			Responder(std::shared_ptr<BlockingAnswer> answer, T fallback)
				: m_guard(std::make_shared<Guard>(std::move(answer), std::move(fallback)))
			{
			}

			void operator()(T value) const { m_guard->answer->Set(std::move(value)); }

		private:
			struct Guard
			{
				Guard(std::shared_ptr<BlockingAnswer> a, T f)
					: answer(std::move(a))
					, fallback(std::move(f))
				{
				}
				// The last copy of the Responder is gone and nobody answered.
				~Guard() { answer->Set(fallback); }

				std::shared_ptr<BlockingAnswer> answer;
				T fallback;
			};

			std::shared_ptr<Guard> m_guard;
		};

		/// Gives the answer. Any thread. Returns true if this was the answer that counted.
		bool Set(T value)
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_value.has_value())
					return false;
				m_value = std::move(value);
			}
			m_cv.notify_all();
			return true;
		}

		bool IsAnswered() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_value.has_value();
		}

		/// Blocks until answered. Every `interval` it calls `keep_waiting()` on the waiting thread, so
		/// the caller can service whatever must go on while it waits (input polling); if that returns
		/// false the wait ends with `fallback`, which is recorded as the answer.
		T Wait(T fallback, std::chrono::milliseconds interval, const std::function<bool()>& keep_waiting)
		{
			for (;;)
			{
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					if (m_cv.wait_for(lock, interval, [this] { return m_value.has_value(); }))
						return *m_value;
				}

				if (keep_waiting && !keep_waiting())
				{
					// A responder may answer between the check above and here; its answer stands.
					Set(std::move(fallback));
					std::lock_guard<std::mutex> lock(m_mutex);
					return *m_value;
				}
			}
		}

		/// Wait() that also gives up, with `fallback`, once `timeout` has passed (checked every
		/// `interval`, so it can be late by that much). `keep_waiting` still runs until then. If
		/// `timed_out` is given it is set to whether the deadline ended the wait; an answer that
		/// arrives in the same instant still wins.
		T WaitFor(std::chrono::milliseconds timeout, T fallback, std::chrono::milliseconds interval,
			const std::function<bool()>& keep_waiting, bool* timed_out = nullptr)
		{
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			bool expired = false;
			T value = Wait(std::move(fallback), interval, [&]() {
				if (std::chrono::steady_clock::now() >= deadline)
				{
					expired = true;
					return false;
				}
				return !keep_waiting || keep_waiting();
			});
			if (timed_out)
				*timed_out = expired;
			return value;
		}

	private:
		mutable std::mutex m_mutex;
		std::condition_variable m_cv;
		std::optional<T> m_value;
	};
} // namespace Catalog
