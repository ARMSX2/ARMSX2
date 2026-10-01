// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Unit suite for the GV-7 front->back SPSC ring (GS/GSBackQueue.h): capacity /
// FIFO / wraparound behavior single-threaded, tag round-trips through the
// tagged record slots, a two-thread producer/consumer stress that checks
// every value crosses in order exactly once, and the same with the producer
// sleeping on SpaceWait against a slow consumer (ring full, pool empty).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#include "GS/GSBackQueue.h"

using namespace GSBackQueue;

TEST(GsBackQueue, FifoAndCapacity)
{
	SpscRing<u32, 8> ring;

	EXPECT_TRUE(ring.IsEmpty());
	EXPECT_EQ(ring.Peek(), nullptr);

	// Fill to capacity.
	for (u32 i = 0; i < 8; i++)
	{
		u32* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		*slot = 100 + i;
		ring.CommitPush();
	}
	EXPECT_EQ(ring.Size(), 8u);
	EXPECT_EQ(ring.BeginPush(), nullptr); // full -> backpressure

	// Pop one, a slot frees up.
	ASSERT_NE(ring.Peek(), nullptr);
	EXPECT_EQ(*ring.Peek(), 100u);
	ring.Pop();
	u32* slot = ring.BeginPush();
	ASSERT_NE(slot, nullptr);
	*slot = 108;
	ring.CommitPush();

	// Drain: strict FIFO.
	for (u32 i = 1; i <= 8; i++)
	{
		u32* p = ring.Peek();
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(*p, 100 + i);
		ring.Pop();
	}
	EXPECT_TRUE(ring.IsEmpty());
}

TEST(GsBackQueue, WraparoundManyTimes)
{
	// Push/pop far past the slot count so the cursors lap the ring repeatedly.
	SpscRing<u32, 4> ring;
	for (u32 i = 0; i < 1000; i++)
	{
		u32* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		*slot = i;
		ring.CommitPush();

		u32* p = ring.Peek();
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(*p, i);
		ring.Pop();
	}
	EXPECT_TRUE(ring.IsEmpty());
}

TEST(GsBackQueue, RecordSlotTagRoundTrip)
{
	RecordRing ring;

	{
		RecordSlot* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		slot->type = RecordType::Vsync;
		VsyncRecord* rec = slot->As<VsyncRecord>();
		rec->field = 1;
		rec->registers_written = true;
		rec->idle_frame = false;
		ring.CommitPush();
	}
	{
		RecordSlot* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		slot->type = RecordType::Transfer;
		TransferRecord* rec = slot->As<TransferRecord>();
		std::memset(rec, 0, sizeof(*rec));
		rec->len = 0x1234;
		rec->draw_serial = 0xdeadbeefcafeull;
		rec->first_slice = true;
		ring.CommitPush();
	}
	{
		RecordSlot* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		slot->type = RecordType::Draw;
		DrawRecord* rec = slot->As<DrawRecord>();
		std::memset(rec, 0, sizeof(*rec));
		rec->draw_serial = 42;
		rec->flush_reason = 7;
		ring.CommitPush();
	}

	EXPECT_EQ(ring.Size(), 3u);

	const RecordSlot* s = ring.Peek();
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, RecordType::Vsync);
	EXPECT_EQ(s->As<VsyncRecord>()->field, 1u);
	EXPECT_TRUE(s->As<VsyncRecord>()->registers_written);
	ring.Pop();

	s = ring.Peek();
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, RecordType::Transfer);
	EXPECT_EQ(s->As<TransferRecord>()->len, 0x1234);
	EXPECT_EQ(s->As<TransferRecord>()->draw_serial, 0xdeadbeefcafeull);
	EXPECT_TRUE(s->As<TransferRecord>()->first_slice);
	ring.Pop();

	s = ring.Peek();
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, RecordType::Draw);
	EXPECT_EQ(s->As<DrawRecord>()->draw_serial, 42u);
	EXPECT_EQ(s->As<DrawRecord>()->flush_reason, 7);
	ring.Pop();

	EXPECT_TRUE(ring.IsEmpty());
}

TEST(GsBackQueue, ThreadedStress)
{
	// Two real threads across a deliberately small ring so full/empty edges and
	// wraparound get hammered. The consumer checks strict sequence order; both
	// sides spin (never sleep), so the test also proves forward progress under
	// pure backpressure.
	constexpr u64 kValues = 1'000'000;
	SpscRing<u64, 64> ring;
	std::atomic<u64> bad{0};

	std::thread consumer([&ring, &bad]() {
		u64 expected = 0;
		while (expected < kValues)
		{
			u64* p = ring.Peek();
			if (!p)
			{
				std::this_thread::yield();
				continue;
			}
			if (*p != expected)
				bad.fetch_add(1, std::memory_order_relaxed);
			expected++;
			ring.Pop();
		}
	});

	for (u64 i = 0; i < kValues; i++)
	{
		u64* slot;
		while (!(slot = ring.BeginPush()))
			std::this_thread::yield();
		*slot = i;
		ring.CommitPush();
	}

	consumer.join();
	EXPECT_EQ(bad.load(), 0u);
	EXPECT_TRUE(ring.IsEmpty());
}

namespace
{
	// A hang is the failure these tests look for, so run the body under a
	// deadline and abort with a message instead of waiting on the ctest timeout.
	template <typename Body>
	void RunWithDeadline(const char* name, std::chrono::seconds limit, Body&& body)
	{
		std::mutex m;
		std::condition_variable cv;
		bool done = false;
		std::thread watchdog([&]() {
			std::unique_lock lock(m);
			if (!cv.wait_for(lock, limit, [&]() { return done; }))
			{
				std::fprintf(stderr, "%s: no progress after %llds, producer or consumer is stuck\n",
					name, static_cast<long long>(limit.count()));
				std::abort();
			}
		});
		body();
		{
			std::lock_guard lock(m);
			done = true;
		}
		cv.notify_one();
		watchdog.join();
	}

	// Strict: one fixed timeout far longer than any test, so the producer only
	// wakes when the consumer posts and a lost wake-up hangs (and trips the
	// deadline). Shipping: the default adaptive timeouts, which end most waits.
	constexpr u32 kStrictTimeoutUs = 60'000'000;
	u32 TimeoutFor(bool strict, u32 shipping) { return strict ? kStrictTimeoutUs : shipping; }

	void BusyFor(std::chrono::nanoseconds ns)
	{
		const auto end = std::chrono::steady_clock::now() + ns;
		while (std::chrono::steady_clock::now() < end)
		{
		}
	}

	// Per-item delay: none, a short busy wait, or (rarely) a real sleep, so the
	// ring swings between full and empty and both wake paths (per-batch count
	// and going idle) get used.
	struct Pacer
	{
		std::mt19937 rng;
		int mode;

		void Step()
		{
			switch (mode)
			{
				case 0:
					return;
				case 1:
					BusyFor(std::chrono::nanoseconds(rng() % 3000));
					return;
				default:
				{
					const u32 r = rng() % 1000;
					if (r < 2)
						std::this_thread::sleep_for(std::chrono::microseconds(200 + rng() % 800));
					else if (r < 300)
						BusyFor(std::chrono::nanoseconds(rng() % 5000));
					return;
				}
			}
		}
	};

	// The back thread's shape: wait for work, drain, and tell the space waiter
	// per retired item and on going idle.
	template <typename Ring, typename OnItem>
	std::thread StartConsumer(Ring& ring, Threading::WorkSema& work, SpaceWait& space, std::atomic<bool>& exit, OnItem on_item)
	{
		return std::thread([&ring, &work, &space, &exit, on_item]() mutable {
			for (;;)
			{
				work.WaitForWorkWithSpin();
				if (exit.load(std::memory_order_acquire))
					break;
				while (auto* p = ring.Peek())
				{
					on_item(*p);
					ring.Pop();
					space.NotifyRetired([&ring]() { return ring.Size(); });
				}
				space.NotifyIdle();
			}
		});
	}

	// StopBackThread's shape.
	void StopConsumer(std::thread& t, Threading::WorkSema& work, std::atomic<bool>& exit)
	{
		work.WaitForEmpty();
		exit.store(true, std::memory_order_release);
		work.NotifyOfWork();
		t.join();
	}
} // namespace

TEST(GsBackQueue, SpaceWaitRingFullSlowConsumer)
{
	// Producer blocks on a full ring (PushRecord's wait) against consumers of
	// varying speed. Refill 1 on a tiny ring makes the producer arm on almost
	// every push, which is where a lost wake-up would show.
	constexpr u32 kRefills[] = {1, 4, 16};
	for (int run = 0; run < 6; run++)
	{
		const int mode = run % 3;
		const bool strict = run < 3;
		for (u32 refill : kRefills)
		{
			for (u32 seed = 0; seed < 4; seed++)
			{
				RunWithDeadline("SpaceWaitRingFullSlowConsumer", std::chrono::seconds(120), [&]() {
					constexpr u64 kValues = 30'000;
					SpscRing<u64, 16> ring;
					Threading::WorkSema work;
					SpaceWait space(TimeoutFor(strict, 50), TimeoutFor(strict, 20), TimeoutFor(strict, 2000));
					std::atomic<bool> exit{false};
					u64 expected = 0;
					u64 bad = 0;
					Pacer consumer_pace{std::mt19937(seed * 7 + 1), mode};
					Pacer producer_pace{std::mt19937(seed * 13 + 5), mode == 2 ? 2 : 0};

					std::thread consumer = StartConsumer(ring, work, space, exit, [&](u64 v) {
						if (v != expected)
							bad++;
						expected++;
						consumer_pace.Step();
					});

					for (u64 i = 0; i < kValues; i++)
					{
						u64* slot;
						while (!(slot = ring.BeginPush()))
						{
							space.Wait([&]() { return ring.Capacity() - ring.Size() >= refill; });
						}
						*slot = i;
						ring.CommitPush();
						work.NotifyOfWork();
						producer_pace.Step();
					}

					StopConsumer(consumer, work, exit);
					EXPECT_EQ(bad, 0u) << "strict " << strict << " mode " << mode << " refill " << refill << " seed " << seed;
					EXPECT_EQ(expected, kValues) << "strict " << strict << " mode " << mode << " refill " << refill << " seed " << seed;
					EXPECT_TRUE(ring.IsEmpty());
				});
			}
		}
	}
}

TEST(GsBackQueue, SpaceWaitPoolEmptySlowConsumer)
{
	// The draw-node shape: the producer takes a node from a free ring (waiting
	// when it is empty), fills it, and pushes a record naming it; the consumer
	// checks the node, returns it to the free ring, then retires the record.
	// The pool is smaller than the record ring, so the pool is what runs out.
	constexpr u32 kNodes = 8;
	for (int run = 0; run < 6; run++)
	{
		const int mode = run % 3;
		const bool strict = run < 3;
		for (u32 refill : {1u, 2u, kNodes / 4})
		{
			for (u32 seed = 0; seed < 4; seed++)
			{
				RunWithDeadline("SpaceWaitPoolEmptySlowConsumer", std::chrono::seconds(120), [&]() {
					constexpr u64 kValues = 30'000;
					struct Node
					{
						u64 value;
					};
					Node nodes[kNodes] = {};
					SpscRing<Node*, 32> ring;
					SpscRing<Node*, kNodes> free_nodes;
					for (Node& n : nodes)
					{
						*free_nodes.BeginPush() = &n;
						free_nodes.CommitPush();
					}
					Threading::WorkSema work;
					SpaceWait space(TimeoutFor(strict, 50), TimeoutFor(strict, 20), TimeoutFor(strict, 2000));
					std::atomic<bool> exit{false};
					u64 expected = 0;
					u64 bad = 0;
					Pacer consumer_pace{std::mt19937(seed * 3 + 11), mode};

					std::thread consumer = StartConsumer(ring, work, space, exit, [&](Node* n) {
						if (n->value != expected)
							bad++;
						expected++;
						consumer_pace.Step();
						Node** slot = free_nodes.BeginPush();
						if (!slot)
						{
							bad++;
							return;
						}
						*slot = n;
						free_nodes.CommitPush();
					});

					for (u64 i = 0; i < kValues; i++)
					{
						Node* node;
						for (;;)
						{
							if (Node** p = free_nodes.Peek())
							{
								node = *p;
								free_nodes.Pop();
								break;
							}
							space.Wait([&]() { return free_nodes.Size() >= refill; });
						}
						node->value = i;

						Node** slot;
						while (!(slot = ring.BeginPush()))
							space.Wait([&]() { return ring.Capacity() - ring.Size() >= 1; });
						*slot = node;
						ring.CommitPush();
						work.NotifyOfWork();
					}

					StopConsumer(consumer, work, exit);
					EXPECT_EQ(bad, 0u) << "strict " << strict << " mode " << mode << " refill " << refill << " seed " << seed;
					EXPECT_EQ(expected, kValues) << "strict " << strict << " mode " << mode << " refill " << refill << " seed " << seed;
					EXPECT_EQ(free_nodes.Size(), kNodes);
				});
			}
		}
	}
}
