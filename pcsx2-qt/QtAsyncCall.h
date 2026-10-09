// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Runs blocking work (file I/O, a service call) off the UI thread and delivers the answer back on it.
//
//   QtAsyncCall::Run(this,
//       [request]() { return DoSlowThing(request); },        // worker thread
//       [this](Result result) { showResult(result); });      // UI thread, only if `this` still exists
//
// `owner` must live on the UI thread. The worker is a QThread parented to `owner`; call
// QtAsyncCall::WaitForAll(this) first thing in the owner's destructor so a thread never outlives the
// object whose members the callbacks touch. The work function must not touch widgets.

#include <QtCore/QObject>
#include <QtCore/QThread>

#include <memory>
#include <type_traits>
#include <utility>

namespace QtAsyncCall
{
	template <typename Work, typename Done>
	void Run(QObject* owner, Work work, Done done)
	{
		using Result = std::invoke_result_t<Work>;
		auto result = std::make_shared<Result>();

		QThread* thread = QThread::create([work = std::move(work), result]() mutable { *result = work(); });
		thread->setParent(owner);
		QObject::connect(thread, &QThread::finished, owner, [thread, result, done = std::move(done)]() mutable {
			done(std::move(*result));
			thread->deleteLater();
		});
		thread->start();
	}

	/// Blocks until every thread started by Run() for `owner` has finished.
	inline void WaitForAll(QObject* owner)
	{
		const QList<QThread*> threads = owner->findChildren<QThread*>(QString(), Qt::FindDirectChildrenOnly);
		for (QThread* thread : threads)
			thread->wait();
	}
} // namespace QtAsyncCall
