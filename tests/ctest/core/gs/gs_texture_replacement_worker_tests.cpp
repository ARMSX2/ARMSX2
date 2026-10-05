// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that SyncWorkerThread waits for the texture-pack worker's current job, not just for its
// queue to empty.
//
// The worker pops a job and runs it with the queue unlocked, so an empty queue does not mean the
// worker is idle. ReloadReplacementMap uses SyncWorkerThread as a barrier before it clears the
// loader's state; a load still running past it can put the old file's contents back afterwards.

#include "GS/Renderers/HW/GSTextureReplacements.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

TEST(GSTextureReplacementWorker, SyncWaitsForTheJobInFlight)
{
	GSTextureReplacements::StartWorkerThread();

	std::atomic<bool> started{false};
	std::atomic<bool> finished{false};
	GSTextureReplacements::QueueWorkerThreadItem([&started, &finished]() {
		started = true;
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		finished = true;
	}, false);

	// Once the job has started, the worker has taken it off the queue.
	while (!started)
		std::this_thread::yield();

	GSTextureReplacements::SyncWorkerThread();
	EXPECT_TRUE(finished);

	GSTextureReplacements::StopWorkerThread();
}
