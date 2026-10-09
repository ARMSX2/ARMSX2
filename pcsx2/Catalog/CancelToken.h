// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <atomic>
#include <memory>

namespace Catalog
{
	/// A cancel flag that can be handed to a worker thread. Copies share one flag: the UI keeps
	/// a copy to call Cancel() on, the worker (and the fetch layer) poll IsCancelled() on theirs.
	/// A default-constructed token is valid and simply never cancelled until someone cancels it.
	class CancelToken
	{
	public:
		CancelToken()
			: m_flag(std::make_shared<std::atomic_bool>(false))
		{
		}

		void Cancel() const { m_flag->store(true, std::memory_order_release); }
		bool IsCancelled() const { return m_flag->load(std::memory_order_acquire); }

		/// For code outside Catalog that takes a plain flag, e.g. SHA256Digest::HashFile().
		/// Valid for as long as any copy of the token is alive.
		const std::atomic_bool* Flag() const { return m_flag.get(); }

	private:
		std::shared_ptr<std::atomic_bool> m_flag;
	};
} // namespace Catalog
