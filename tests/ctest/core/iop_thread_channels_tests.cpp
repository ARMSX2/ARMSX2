// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The queues and flags the EE thread and the IOP thread talk through (IopThread/IopChannels.h):
// the EE-to-IOP stamped ring, the IOP-to-EE queue, the coalescing kick flags and the host
// command queue.
//
// The first half checks single-thread semantics: order, wrap-around, full, empty, stamps. The
// second half runs each primitive between two real threads for a million items with random
// delays on both sides, so the rings spend time full, empty and in between, and checks that
// nothing is lost, duplicated or reordered. These stress tests also pass under ThreadSanitizer,
// which is what checks the memory orderings: this file includes nothing from the emulator, so it
// builds on its own with -fsanitize=thread.

#include <gtest/gtest.h>

#include "IopThread/IopChannels.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef IOP_CHANNELS_STRESS_ITEMS
#define IOP_CHANNELS_STRESS_ITEMS 1000000
#endif

using namespace IopThread;

namespace
{
	constexpr u64 kStressItems = IOP_CHANNELS_STRESS_ITEMS;

	void CpuRelax()
	{
#if defined(__aarch64__)
		asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
		__builtin_ia32_pause();
#else
		std::this_thread::yield();
#endif
	}

	// A few quick retries, then give the core away: the stress tests must also finish on a runner
	// with fewer cores than threads.
	class Backoff
	{
	public:
		void Wait()
		{
			if (++m_attempts < 64)
				CpuRelax();
			else
				std::this_thread::yield();
		}
		void Reset() { m_attempts = 0; }

	private:
		u32 m_attempts = 0;
	};

	class Rng
	{
	public:
		explicit Rng(u64 seed)
			: m_state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull)
		{
			if (m_state == 0)
				m_state = 1;
		}
		u64 Next()
		{
			m_state ^= m_state >> 12;
			m_state ^= m_state << 25;
			m_state ^= m_state >> 27;
			return m_state * 0x2545F4914F6CDD1Dull;
		}
		u32 Below(u32 n) { return static_cast<u32>((Next() >> 32) % n); }

	private:
		u64 m_state;
	};

	// Random delays for one side of a stress test. The side picks a mode for a stretch of a few
	// thousand operations, then another. With the producer and the consumer choosing separately, a
	// ring gets long runs of "producer faster" (it fills) and "consumer faster" (it drains).
	class Jitter
	{
	public:
		explicit Jitter(u64 seed)
			: m_rng(seed)
		{
			NewPhase();
		}

		void Step()
		{
			if (--m_left == 0)
				NewPhase();
			switch (m_mode)
			{
				case Mode::Fast:
					break;
				case Mode::Short:
					if (m_rng.Below(16) == 0)
						Spin(m_rng.Below(100));
					break;
				case Mode::Slow:
					Spin(m_rng.Below(200));
					if (m_rng.Below(256) == 0)
						std::this_thread::yield();
					break;
				case Mode::Stall:
					if (m_rng.Below(512) == 0)
						std::this_thread::sleep_for(std::chrono::microseconds(20 + m_rng.Below(80)));
					break;
			}
		}

	private:
		enum class Mode
		{
			Fast,
			Short,
			Slow,
			Stall
		};

		static void Spin(u32 n)
		{
			for (u32 i = 0; i < n; i++)
				CpuRelax();
		}

		void NewPhase()
		{
			m_mode = static_cast<Mode>(m_rng.Below(4));
			m_left = 1000 + m_rng.Below(3000);
		}

		Rng m_rng;
		Mode m_mode = Mode::Fast;
		u32 m_left = 1;
	};

	// The first failure either thread finds. gtest assertions are not used off the main thread, and
	// a thread that finds a failure has to be able to stop the other one, which would otherwise wait
	// forever for an item that is not coming.
	class FailureLog
	{
	public:
		bool Failed() const { return m_failed.load(std::memory_order_relaxed); }

		void Fail(const std::string& message)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_failed.exchange(true))
				m_message = message;
		}

		std::string Message()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_message;
		}

	private:
		std::atomic<bool> m_failed{false};
		std::mutex m_mutex;
		std::string m_message;
	};

	// A stress test that waits on the other thread gives up after this long and fails instead of
	// hanging the run.
	class Deadline
	{
	public:
		explicit Deadline(std::chrono::seconds limit)
			: m_end(std::chrono::steady_clock::now() + limit)
		{
		}
		bool Expired()
		{
			if ((++m_calls & 255) != 0)
				return false;
			return std::chrono::steady_clock::now() > m_end;
		}

	private:
		std::chrono::steady_clock::time_point m_end;
		u32 m_calls = 0;
	};

#ifndef IOP_CHANNELS_STRESS_TIMEOUT_SECONDS
#define IOP_CHANNELS_STRESS_TIMEOUT_SECONDS 120
#endif
	constexpr std::chrono::seconds kStressTimeout{IOP_CHANNELS_STRESS_TIMEOUT_SECONDS};

	struct StressStats
	{
		u64 full_hits = 0; // times the producer found the ring full
		u64 empty_hits = 0; // times the consumer found nothing to do
	};

	// What item `i` of a stress run contains. A wrong field in any position shows up as a mismatch.
	u16 KindOf(u64 i) { return static_cast<u16>(i * 7 + 1); }
	u32 ArgOf(u64 i) { return static_cast<u32>(i * 2654435761u + 12345u); }
	// Pairs of items share a stamp, so the stress covers equal stamps. Stamps start at 1 so that a
	// watermark of 0 means "nothing posted yet".
	u64 StampOf(u64 i) { return 1 + (i / 2) * 5; }

	// The EE posts items and publishes a watermark after them; the IOP loads the watermark and
	// applies every item stamped at or before it. Checked here as: when the watermark covers the
	// next item, the ring must hand it over.
	template <size_t N>
	StressStats RunE2IStress(u64 items, u64 seed, std::string& failure)
	{
		auto ring = std::make_unique<StampedRing<N>>();
		std::atomic<u64> watermark{0};
		FailureLog log;
		StressStats producer_stats, consumer_stats;

		std::thread producer([&] {
			Jitter jitter(seed * 2 + 1);
			Backoff backoff;
			Deadline deadline(kStressTimeout);
			for (u64 i = 0; i < items; i++)
			{
				jitter.Step();
				while (!ring->TryPush(StampOf(i), KindOf(i), ArgOf(i)))
				{
					producer_stats.full_hits++;
					if (log.Failed())
						return;
					if (deadline.Expired())
					{
						log.Fail("producer timed out at item " + std::to_string(i));
						return;
					}
					backoff.Wait();
				}
				backoff.Reset();
				// The watermark goes out after both items of a pair, so it never covers a stamp that
				// still has an item to come.
				if ((i & 1) != 0 || i + 1 == items)
					watermark.store(StampOf(i), std::memory_order_release);
			}
		});

		std::thread consumer([&] {
			Jitter jitter(seed * 2 + 2);
			Backoff backoff;
			Deadline deadline(kStressTimeout);
			u64 next = 0;
			while (next < items)
			{
				jitter.Step();
				if (log.Failed())
					return;
				if (deadline.Expired())
				{
					log.Fail("consumer timed out waiting for item " + std::to_string(next));
					return;
				}

				const u64 now = watermark.load(std::memory_order_acquire);
				bool progressed = false;
				while (next < items)
				{
					E2IItem item{};
					bool got;
					if ((next & 7) == 3)
					{
						// Every eighth item takes the look-then-release path instead.
						const E2IItem* head = ring->Peek();
						got = head != nullptr && head->stamp <= now;
						if (got)
						{
							item = *head;
							ring->Pop();
						}
					}
					else
					{
						got = ring->TryPopDue(now, item);
					}
					if (!got)
						break;

					if (item.stamp != StampOf(next) || item.kind != KindOf(next) || item.arg != ArgOf(next) || item.pad != 0)
					{
						log.Fail("item " + std::to_string(next) + " came out as stamp " + std::to_string(item.stamp) +
								 " kind " + std::to_string(item.kind) + " arg " + std::to_string(item.arg));
						return;
					}
					if (item.stamp > now)
					{
						log.Fail("item " + std::to_string(next) + " was handed over before its stamp");
						return;
					}
					next++;
					progressed = true;
				}

				if (next < items && StampOf(next) <= now)
				{
					log.Fail("item " + std::to_string(next) + " is stamped at or before the watermark and was not visible");
					return;
				}
				if (progressed)
				{
					backoff.Reset();
				}
				else
				{
					consumer_stats.empty_hits++;
					backoff.Wait();
				}
			}
		});

		producer.join();
		consumer.join();

		if (log.Failed())
			failure = log.Message();
		else if (ring->Peek() != nullptr)
			failure = "the ring is not empty at the end";

		StressStats stats;
		stats.full_hits = producer_stats.full_hits;
		stats.empty_hits = consumer_stats.empty_hits;
		return stats;
	}

	template <size_t N>
	StressStats RunI2EStress(u64 items, u64 seed, std::string& failure)
	{
		auto queue = std::make_unique<I2EQueue<N>>();
		FailureLog log;
		StressStats producer_stats, consumer_stats;

		std::thread producer([&] {
			Jitter jitter(seed * 2 + 1);
			Backoff backoff;
			Deadline deadline(kStressTimeout);
			for (u64 i = 0; i < items; i++)
			{
				jitter.Step();
				while (!queue->TryPush(KindOf(i), ArgOf(i)))
				{
					producer_stats.full_hits++;
					if (log.Failed())
						return;
					if (deadline.Expired())
					{
						log.Fail("producer timed out at item " + std::to_string(i));
						return;
					}
					backoff.Wait();
				}
				backoff.Reset();
			}
		});

		// The consumer is the EE's event test: look cheaply, and only then drain.
		std::thread consumer([&] {
			Jitter jitter(seed * 2 + 2);
			Backoff backoff;
			Deadline deadline(kStressTimeout);
			u64 next = 0;
			while (next < items)
			{
				jitter.Step();
				if (log.Failed())
					return;
				if (deadline.Expired())
				{
					log.Fail("consumer timed out waiting for item " + std::to_string(next));
					return;
				}

				if (!queue->MayHaveItems())
				{
					consumer_stats.empty_hits++;
					backoff.Wait();
					continue;
				}
				backoff.Reset();

				while (next < items)
				{
					I2EItem item{};
					bool got;
					if ((next & 7) == 3)
					{
						const I2EItem* head = queue->Peek();
						got = head != nullptr;
						if (got)
						{
							item = *head;
							queue->Pop();
						}
					}
					else
					{
						got = queue->TryPop(item);
					}
					if (!got)
						break;

					if (item.kind != KindOf(next) || item.arg != ArgOf(next) || item.pad != 0)
					{
						log.Fail("item " + std::to_string(next) + " came out as kind " + std::to_string(item.kind) +
								 " arg " + std::to_string(item.arg));
						return;
					}
					next++;
				}
			}
		});

		producer.join();
		consumer.join();

		if (log.Failed())
			failure = log.Message();
		else if (queue->Peek() != nullptr)
			failure = "the queue is not empty at the end";

		StressStats stats;
		stats.full_hits = producer_stats.full_hits;
		stats.empty_hits = consumer_stats.empty_hits;
		return stats;
	}

	// Pushes then pops `count` items through a ring whose indices are `offset` slots in, so that
	// every starting position gets exercised.
	template <size_t N>
	void AdvanceE2I(StampedRing<N>& ring, size_t offset)
	{
		for (size_t i = 0; i < offset; i++)
		{
			ASSERT_TRUE(ring.TryPush(0, 0));
			E2IItem scratch;
			ASSERT_TRUE(ring.TryPopDue(0, scratch));
		}
	}

	template <size_t N>
	void AdvanceI2E(I2EQueue<N>& queue, size_t offset)
	{
		for (size_t i = 0; i < offset; i++)
		{
			ASSERT_TRUE(queue.TryPush(0));
			I2EItem scratch;
			ASSERT_TRUE(queue.TryPop(scratch));
		}
	}
} // namespace

TEST(IopChannelsLayout, TheCapacitiesAreThePowersOfTwoTheDesignNames)
{
	EXPECT_EQ(kE2IRingCapacity, 1024u);
	EXPECT_EQ(kI2EQueueCapacity, 256u);
	EXPECT_EQ(kE2IRingCapacity & (kE2IRingCapacity - 1), 0u);
	EXPECT_EQ(kI2EQueueCapacity & (kI2EQueueCapacity - 1), 0u);
	EXPECT_EQ(StampedRing<kE2IRingCapacity>::Capacity(), kE2IRingCapacity);
	EXPECT_EQ(I2EQueue<kI2EQueueCapacity>::Capacity(), kI2EQueueCapacity);
}

TEST(IopChannelsLayout, TheItemsHaveTheSizesTheDesignGives)
{
	EXPECT_EQ(sizeof(E2IItem), 16u);
	EXPECT_EQ(sizeof(I2EItem), 8u);
}

TEST(IopChannelsLayout, TheLineSizeCoversTheAppleM2AndTheProjectConstant)
{
	EXPECT_GE(kChannelLineSize, 128u);
	EXPECT_GE(kChannelLineSize, static_cast<size_t>(__cachelinesize));
	EXPECT_EQ(kChannelLineSize & (kChannelLineSize - 1), 0u);
}

TEST(IopChannelsLayout, TheProducerIndexTheConsumerIndexAndTheSlotsEachHaveALineOfTheirOwn)
{
	const auto check = [](auto layout) {
		const size_t line = kChannelLineSize;
		EXPECT_EQ(layout.write_offset % line, 0u);
		EXPECT_EQ(layout.read_offset % line, 0u);
		EXPECT_EQ(layout.slots_offset % line, 0u);
		EXPECT_NE(layout.write_offset / line, layout.read_offset / line);
		EXPECT_NE(layout.write_offset / line, layout.slots_offset / line);
		EXPECT_NE(layout.read_offset / line, layout.slots_offset / line);
		// Each index owns its whole line: the next thing starts a line later.
		EXPECT_GE(layout.read_offset - layout.write_offset, line);
		EXPECT_GE(layout.slots_offset - layout.read_offset, line);
		EXPECT_EQ(layout.total_size % line, 0u);
	};
	check(StampedRing<8>::GetLayout());
	check(StampedRing<kE2IRingCapacity>::GetLayout());
	check(I2EQueue<8>::GetLayout());
	check(I2EQueue<kI2EQueueCapacity>::GetLayout());
}

TEST(IopChannelsLayout, TheEeSideCallsCannotThrowAndTheTypesOwnNothingOnTheHeap)
{
	// The EE side never blocks (a full or empty ring is a return value) and never allocates. Both
	// are by construction; these checks pin the parts the compiler can see.
	using Ring = StampedRing<8>;
	using Queue = I2EQueue<8>;
	static_assert(noexcept(std::declval<Ring&>().TryPush(0, 0, 0)));
	static_assert(noexcept(std::declval<Ring&>().Peek()));
	static_assert(noexcept(std::declval<Ring&>().Pop()));
	static_assert(noexcept(std::declval<Ring&>().MayHaveItems()));
	static_assert(noexcept(std::declval<Queue&>().TryPush(0, 0)));
	static_assert(noexcept(std::declval<Queue&>().TryPop(std::declval<I2EItem&>())));
	static_assert(noexcept(std::declval<Queue&>().MayHaveItems()));
	static_assert(noexcept(std::declval<KickBits&>().Raise(1)));
	static_assert(noexcept(std::declval<KickBits&>().Take(1)));

	// Every word the two sides share is a lock-free atomic.
	static_assert(std::atomic<u64>::is_always_lock_free);
	static_assert(std::atomic<u32>::is_always_lock_free);

	// A type that owned heap memory would have a destructor that frees it.
	static_assert(std::is_trivially_destructible_v<Ring>);
	static_assert(std::is_trivially_destructible_v<Queue>);
	static_assert(std::is_trivially_destructible_v<KickBits>);
	SUCCEED();
}

TEST(IopChannelsLayout, ARingStartsOnALineAndTheKickWordAndTheCommandFlagsOwnTheirLines)
{
	EXPECT_GE(alignof(StampedRing<8>), kChannelLineSize);
	EXPECT_GE(alignof(I2EQueue<8>), kChannelLineSize);
	EXPECT_GE(alignof(KickBits), kChannelLineSize);
	EXPECT_EQ(sizeof(KickBits) % kChannelLineSize, 0u);
	EXPECT_GE(alignof(HostCommandQueue), kChannelLineSize);
}

TEST(StampedRing, ANewRingIsEmpty)
{
	StampedRing<8> ring;
	EXPECT_EQ(ring.Peek(), nullptr);
	EXPECT_EQ(ring.NextStamp(), StampedRing<8>::kNoStamp);
	EXPECT_FALSE(ring.MayHaveItems());
	EXPECT_EQ(ring.ApproxSize(), 0u);
	E2IItem out{111, 222, 333, 444};
	EXPECT_FALSE(ring.TryPopDue(~u64(0) - 1, out));
	EXPECT_EQ(out.stamp, 111u); // untouched when there is nothing to pop
	EXPECT_EQ(out.kind, 222u);
	EXPECT_EQ(out.arg, 444u);
}

TEST(StampedRing, ItemsComeOutInTheOrderTheyWentIn)
{
	StampedRing<8> ring;
	for (u32 i = 0; i < 5; i++)
		ASSERT_TRUE(ring.TryPush(100 + i, static_cast<u16>(10 + i), 1000 + i));
	EXPECT_TRUE(ring.MayHaveItems());
	EXPECT_EQ(ring.ApproxSize(), 5u);

	for (u32 i = 0; i < 5; i++)
	{
		E2IItem out;
		ASSERT_TRUE(ring.TryPopDue(~u64(0) - 1, out));
		EXPECT_EQ(out.stamp, 100u + i);
		EXPECT_EQ(out.kind, 10u + i);
		EXPECT_EQ(out.arg, 1000u + i);
		EXPECT_EQ(out.pad, 0u);
	}
	EXPECT_EQ(ring.Peek(), nullptr);
	EXPECT_FALSE(ring.MayHaveItems());
}

TEST(StampedRing, ItIsFullAfterCapacityItemsAndSaysSoWithoutDisturbingAnything)
{
	StampedRing<8> ring;
	for (u32 i = 0; i < 8; i++)
		ASSERT_TRUE(ring.TryPush(i + 1, static_cast<u16>(i), i)) << i;
	EXPECT_EQ(ring.ApproxSize(), 8u);

	// A full ring refuses, repeatedly, and keeps what it has.
	EXPECT_FALSE(ring.TryPush(99, 99, 99));
	EXPECT_FALSE(ring.TryPush(99, 99, 99));
	EXPECT_EQ(ring.ApproxSize(), 8u);

	for (u32 i = 0; i < 8; i++)
	{
		E2IItem out;
		ASSERT_TRUE(ring.TryPopDue(~u64(0) - 1, out));
		EXPECT_EQ(out.stamp, i + 1u);
		EXPECT_EQ(out.arg, i);
	}
	EXPECT_EQ(ring.Peek(), nullptr);
}

TEST(StampedRing, OnePopMakesRoomForExactlyOnePush)
{
	StampedRing<4> ring;
	for (u32 i = 0; i < 4; i++)
		ASSERT_TRUE(ring.TryPush(i, 0, i));
	ASSERT_FALSE(ring.TryPush(4, 0, 4));

	E2IItem out;
	ASSERT_TRUE(ring.TryPopDue(100, out));
	EXPECT_EQ(out.arg, 0u);
	EXPECT_TRUE(ring.TryPush(4, 0, 4));
	EXPECT_FALSE(ring.TryPush(5, 0, 5));

	for (u32 expect = 1; expect <= 4; expect++)
	{
		ASSERT_TRUE(ring.TryPopDue(100, out));
		EXPECT_EQ(out.arg, expect);
	}
	EXPECT_FALSE(ring.TryPopDue(100, out));
}

TEST(StampedRing, EveryStartingSlotAndEveryFillLevelWorks)
{
	// The ring is eight slots, so this puts the wrap point at each position in turn and, for each,
	// fills to every level from empty to full.
	for (size_t offset = 0; offset < 8; offset++)
	{
		for (u32 fill = 0; fill <= 8; fill++)
		{
			StampedRing<8> ring;
			AdvanceE2I(ring, offset);
			ASSERT_FALSE(HasFatalFailure());

			for (u32 i = 0; i < fill; i++)
				ASSERT_TRUE(ring.TryPush(1 + i, static_cast<u16>(i), 7000 + i)) << offset << "/" << fill;
			EXPECT_EQ(ring.TryPush(1000, 0, 0), fill < 8) << offset << "/" << fill;
			const u32 held = fill < 8 ? fill + 1 : fill;

			for (u32 i = 0; i < held; i++)
			{
				E2IItem out;
				ASSERT_TRUE(ring.TryPopDue(~u64(0) - 1, out)) << offset << "/" << fill << "/" << i;
				if (i < fill)
				{
					EXPECT_EQ(out.stamp, 1u + i);
					EXPECT_EQ(out.arg, 7000u + i);
				}
				else
				{
					EXPECT_EQ(out.stamp, 1000u);
				}
			}
			EXPECT_EQ(ring.Peek(), nullptr);
		}
	}
}

TEST(StampedRing, ManyTimesRoundTheRingKeepsOrderAndContents)
{
	StampedRing<4> ring;
	u32 pushed = 0, popped = 0;
	// Alternate bursts of one to four so the fill level wanders over the whole range.
	for (u32 round = 0; round < 5000; round++)
	{
		const u32 burst = 1 + (round * 7) % 4;
		for (u32 i = 0; i < burst; i++)
		{
			if (ring.TryPush(pushed + 1, static_cast<u16>(pushed), pushed * 3u))
				pushed++;
		}
		const u32 take = 1 + (round * 5) % 4;
		for (u32 i = 0; i < take; i++)
		{
			E2IItem out;
			if (ring.TryPopDue(~u64(0) - 1, out))
			{
				ASSERT_EQ(out.stamp, popped + 1u);
				ASSERT_EQ(out.kind, static_cast<u16>(popped));
				ASSERT_EQ(out.arg, popped * 3u);
				popped++;
			}
		}
	}
	E2IItem out;
	while (ring.TryPopDue(~u64(0) - 1, out))
	{
		ASSERT_EQ(out.arg, popped * 3u);
		popped++;
	}
	EXPECT_EQ(pushed, popped);
	EXPECT_GT(popped, 5000u);
}

TEST(StampedRing, AnItemIsNotHandedOverBeforeItsStamp)
{
	StampedRing<8> ring;
	ASSERT_TRUE(ring.TryPush(100, 1, 0));
	E2IItem out{};
	EXPECT_FALSE(ring.TryPopDue(0, out));
	EXPECT_FALSE(ring.TryPopDue(99, out));
	EXPECT_EQ(ring.ApproxSize(), 1u); // still there
	EXPECT_TRUE(ring.TryPopDue(100, out)); // at its stamp it is due
	EXPECT_EQ(out.stamp, 100u);
	EXPECT_EQ(out.kind, 1u);
}

TEST(StampedRing, AStampOfZeroIsDueAtZero)
{
	StampedRing<8> ring;
	ASSERT_TRUE(ring.TryPush(0, 5, 6));
	E2IItem out{};
	EXPECT_TRUE(ring.TryPopDue(0, out));
	EXPECT_EQ(out.kind, 5u);
}

TEST(StampedRing, ALaterItemWaitsBehindAnEarlierOneAndEqualStampsKeepTheirOrder)
{
	StampedRing<8> ring;
	ASSERT_TRUE(ring.TryPush(10, 1, 0));
	ASSERT_TRUE(ring.TryPush(10, 2, 0));
	ASSERT_TRUE(ring.TryPush(20, 3, 0));
	ASSERT_TRUE(ring.TryPush(30, 4, 0));

	E2IItem out;
	EXPECT_FALSE(ring.TryPopDue(9, out));

	EXPECT_EQ(ring.NextStamp(), 10u);
	ASSERT_TRUE(ring.TryPopDue(10, out));
	EXPECT_EQ(out.kind, 1u);
	EXPECT_EQ(ring.NextStamp(), 10u);
	ASSERT_TRUE(ring.TryPopDue(10, out));
	EXPECT_EQ(out.kind, 2u);
	EXPECT_FALSE(ring.TryPopDue(19, out));

	EXPECT_EQ(ring.NextStamp(), 20u);
	ASSERT_TRUE(ring.TryPopDue(25, out)); // 25 covers 20 but not 30
	EXPECT_EQ(out.kind, 3u);
	EXPECT_FALSE(ring.TryPopDue(25, out));

	EXPECT_EQ(ring.NextStamp(), 30u);
	ASSERT_TRUE(ring.TryPopDue(1000, out));
	EXPECT_EQ(out.kind, 4u);
	EXPECT_EQ(ring.NextStamp(), StampedRing<8>::kNoStamp);
}

TEST(StampedRing, ApplyingEveryDueItemPopsExactlyThoseAtOrBeforeNow)
{
	// The IOP's "apply every item with stamp <= iop_pos" loop.
	StampedRing<16> ring;
	for (u32 i = 0; i < 10; i++)
		ASSERT_TRUE(ring.TryPush(100 * (i + 1), static_cast<u16>(i), i)); // stamps 100..1000

	u32 applied = 0;
	E2IItem out;
	while (ring.TryPopDue(450, out))
	{
		EXPECT_EQ(out.arg, applied);
		EXPECT_LE(out.stamp, 450u);
		applied++;
	}
	EXPECT_EQ(applied, 4u); // 100, 200, 300, 400
	EXPECT_EQ(ring.NextStamp(), 500u);
}

TEST(StampedRing, PeekDoesNotConsumeAndStaysPutUntilPop)
{
	StampedRing<8> ring;
	ASSERT_TRUE(ring.TryPush(10, 1, 11));
	ASSERT_TRUE(ring.TryPush(20, 2, 22));

	const E2IItem* a = ring.Peek();
	const E2IItem* b = ring.Peek();
	ASSERT_NE(a, nullptr);
	EXPECT_EQ(a, b);
	EXPECT_EQ(a->kind, 1u);
	EXPECT_EQ(ring.ApproxSize(), 2u);

	// A push while the head is held does not move it.
	ASSERT_TRUE(ring.TryPush(30, 3, 33));
	EXPECT_EQ(a->kind, 1u);
	EXPECT_EQ(a->arg, 11u);

	ring.Pop();
	const E2IItem* c = ring.Peek();
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(c->kind, 2u);
	ring.Pop();
	const E2IItem* d = ring.Peek();
	ASSERT_NE(d, nullptr);
	EXPECT_EQ(d->kind, 3u);
	ring.Pop();
	EXPECT_EQ(ring.Peek(), nullptr);
}

TEST(StampedRing, ResetEmptiesItAndLetsAnEarlierStampStartAgain)
{
	StampedRing<8> ring;
	ASSERT_TRUE(ring.TryPush(1000, 1, 1));
	ASSERT_TRUE(ring.TryPush(2000, 2, 2));
	ring.Reset();
	EXPECT_EQ(ring.Peek(), nullptr);
	EXPECT_FALSE(ring.MayHaveItems());
	EXPECT_EQ(ring.ApproxSize(), 0u);

	// After a reset the EE's cycle count may have moved back (a state load), so a smaller stamp
	// is not a backwards step.
	ASSERT_TRUE(ring.TryPush(5, 7, 8));
	E2IItem out;
	ASSERT_TRUE(ring.TryPopDue(5, out));
	EXPECT_EQ(out.kind, 7u);
	EXPECT_EQ(out.arg, 8u);

	// And it still fills to exactly its capacity.
	for (u32 i = 0; i < 8; i++)
		ASSERT_TRUE(ring.TryPush(10 + i, 0, i));
	EXPECT_FALSE(ring.TryPush(99, 0, 0));
}

TEST(StampedRing, TheDesignSizedRingHoldsExactlyItsCapacity)
{
	auto ring = std::make_unique<StampedRing<kE2IRingCapacity>>();
	for (u32 i = 0; i < kE2IRingCapacity; i++)
		ASSERT_TRUE(ring->TryPush(i, static_cast<u16>(i), i)) << i;
	EXPECT_FALSE(ring->TryPush(kE2IRingCapacity, 0, 0));
	E2IItem out;
	for (u32 i = 0; i < kE2IRingCapacity; i++)
	{
		ASSERT_TRUE(ring->TryPopDue(~u64(0) - 1, out));
		ASSERT_EQ(out.arg, i);
	}
	EXPECT_FALSE(ring->TryPopDue(~u64(0) - 1, out));
}

TEST(StampedRing, TheSpscRingBehindItReportsTheItemPushedLast)
{
	detail::SpscRing<E2IItem, 4> ring;
	EXPECT_EQ(ring.LastPushed(), nullptr);
	ASSERT_TRUE(ring.TryPush(E2IItem{5, 1, 0, 9}));
	ASSERT_NE(ring.LastPushed(), nullptr);
	EXPECT_EQ(ring.LastPushed()->stamp, 5u);

	// The producer can still read it after the consumer has popped it.
	E2IItem out;
	ASSERT_TRUE(ring.TryPop(out));
	ASSERT_NE(ring.LastPushed(), nullptr);
	EXPECT_EQ(ring.LastPushed()->stamp, 5u);

	for (u32 i = 0; i < 6; i++)
	{
		ASSERT_TRUE(ring.TryPush(E2IItem{10 + i, 0, 0, i}));
		ASSERT_EQ(ring.LastPushed()->stamp, 10u + i);
		ASSERT_TRUE(ring.TryPop(out));
	}
}

TEST(I2EQueue, ANewQueueIsEmpty)
{
	I2EQueue<8> queue;
	EXPECT_EQ(queue.Peek(), nullptr);
	EXPECT_FALSE(queue.MayHaveItems());
	EXPECT_EQ(queue.ApproxSize(), 0u);
	I2EItem out{1, 2, 3};
	EXPECT_FALSE(queue.TryPop(out));
	EXPECT_EQ(out.kind, 1u); // untouched
	EXPECT_EQ(out.arg, 3u);
}

TEST(I2EQueue, ItemsComeOutInTheOrderTheyWentIn)
{
	I2EQueue<8> queue;
	for (u32 i = 0; i < 6; i++)
		ASSERT_TRUE(queue.TryPush(static_cast<u16>(40 + i), 5000 + i));
	EXPECT_TRUE(queue.MayHaveItems());
	EXPECT_EQ(queue.ApproxSize(), 6u);

	for (u32 i = 0; i < 6; i++)
	{
		I2EItem out;
		ASSERT_TRUE(queue.TryPop(out));
		EXPECT_EQ(out.kind, 40u + i);
		EXPECT_EQ(out.arg, 5000u + i);
		EXPECT_EQ(out.pad, 0u);
	}
	EXPECT_FALSE(queue.MayHaveItems());
}

TEST(I2EQueue, ADefaultArgumentIsZero)
{
	I2EQueue<4> queue;
	ASSERT_TRUE(queue.TryPush(9));
	I2EItem out{};
	ASSERT_TRUE(queue.TryPop(out));
	EXPECT_EQ(out.kind, 9u);
	EXPECT_EQ(out.arg, 0u);
}

TEST(I2EQueue, ItIsFullAfterCapacityItemsAndThenRefusesWithoutLosingAnything)
{
	I2EQueue<8> queue;
	for (u32 i = 0; i < 8; i++)
		ASSERT_TRUE(queue.TryPush(static_cast<u16>(i), i * 2));
	EXPECT_FALSE(queue.TryPush(77, 77));
	EXPECT_FALSE(queue.TryPush(77, 77));
	EXPECT_EQ(queue.ApproxSize(), 8u);

	for (u32 i = 0; i < 8; i++)
	{
		I2EItem out;
		ASSERT_TRUE(queue.TryPop(out));
		EXPECT_EQ(out.kind, i);
		EXPECT_EQ(out.arg, i * 2);
	}
	EXPECT_FALSE(queue.MayHaveItems());
}

TEST(I2EQueue, EveryStartingSlotAndEveryFillLevelWorks)
{
	for (size_t offset = 0; offset < 8; offset++)
	{
		for (u32 fill = 0; fill <= 8; fill++)
		{
			I2EQueue<8> queue;
			AdvanceI2E(queue, offset);
			ASSERT_FALSE(HasFatalFailure());

			for (u32 i = 0; i < fill; i++)
				ASSERT_TRUE(queue.TryPush(static_cast<u16>(i), 100 + i)) << offset << "/" << fill;
			EXPECT_EQ(queue.TryPush(500, 500), fill < 8) << offset << "/" << fill;
			const u32 held = fill < 8 ? fill + 1 : fill;

			for (u32 i = 0; i < held; i++)
			{
				I2EItem out;
				ASSERT_TRUE(queue.TryPop(out)) << offset << "/" << fill << "/" << i;
				if (i < fill)
					EXPECT_EQ(out.arg, 100u + i);
				else
					EXPECT_EQ(out.kind, 500u);
			}
			EXPECT_EQ(queue.Peek(), nullptr);
		}
	}
}

TEST(I2EQueue, ManyTimesRoundTheQueueKeepsOrderAndContents)
{
	I2EQueue<4> queue;
	u32 pushed = 0, popped = 0;
	for (u32 round = 0; round < 5000; round++)
	{
		for (u32 i = 0; i < 1 + (round * 3) % 4; i++)
		{
			if (queue.TryPush(static_cast<u16>(pushed), pushed + 17))
				pushed++;
		}
		for (u32 i = 0; i < 1 + (round * 5) % 4; i++)
		{
			I2EItem out;
			if (queue.TryPop(out))
			{
				ASSERT_EQ(out.kind, static_cast<u16>(popped));
				ASSERT_EQ(out.arg, popped + 17);
				popped++;
			}
		}
	}
	I2EItem out;
	while (queue.TryPop(out))
	{
		ASSERT_EQ(out.arg, popped + 17);
		popped++;
	}
	EXPECT_EQ(pushed, popped);
}

TEST(I2EQueue, MayHaveItemsTracksPushAndPopOnOneThread)
{
	I2EQueue<4> queue;
	EXPECT_FALSE(queue.MayHaveItems());
	ASSERT_TRUE(queue.TryPush(1));
	EXPECT_TRUE(queue.MayHaveItems());
	ASSERT_TRUE(queue.TryPush(2));
	I2EItem out;
	ASSERT_TRUE(queue.TryPop(out));
	EXPECT_TRUE(queue.MayHaveItems());
	ASSERT_TRUE(queue.TryPop(out));
	EXPECT_FALSE(queue.MayHaveItems());
}

TEST(I2EQueue, PeekThenPopTakesTheSameItemTryPopWould)
{
	I2EQueue<8> queue;
	ASSERT_TRUE(queue.TryPush(1, 10));
	ASSERT_TRUE(queue.TryPush(2, 20));
	const I2EItem* head = queue.Peek();
	ASSERT_NE(head, nullptr);
	EXPECT_EQ(head->kind, 1u);
	EXPECT_EQ(queue.Peek(), head);
	queue.Pop();
	I2EItem out;
	ASSERT_TRUE(queue.TryPop(out));
	EXPECT_EQ(out.kind, 2u);
	EXPECT_EQ(out.arg, 20u);
}

TEST(I2EQueue, ResetEmptiesIt)
{
	I2EQueue<4> queue;
	ASSERT_TRUE(queue.TryPush(1));
	ASSERT_TRUE(queue.TryPush(2));
	queue.Reset();
	EXPECT_EQ(queue.Peek(), nullptr);
	EXPECT_FALSE(queue.MayHaveItems());
	for (u32 i = 0; i < 4; i++)
		ASSERT_TRUE(queue.TryPush(static_cast<u16>(i)));
	EXPECT_FALSE(queue.TryPush(9));
}

TEST(KickBits, ANewWordHasNothingUp)
{
	KickBits kicks;
	EXPECT_EQ(kicks.Pending(), 0u);
	EXPECT_EQ(kicks.Take(~0u), 0u);
	EXPECT_EQ(kicks.TakeAll(), 0u);
}

TEST(KickBits, RaisingAClearBitReportsItAndRaisingItAgainReportsNothing)
{
	KickBits kicks;
	EXPECT_EQ(kicks.Raise(I2EKick::Sif0EeHalf), I2EKick::Sif0EeHalf);
	EXPECT_EQ(kicks.Pending(), I2EKick::Sif0EeHalf);

	// Coalesced: the first raise still covers these.
	EXPECT_EQ(kicks.Raise(I2EKick::Sif0EeHalf), 0u);
	EXPECT_EQ(kicks.Raise(I2EKick::Sif0EeHalf), 0u);
	EXPECT_EQ(kicks.Pending(), I2EKick::Sif0EeHalf);
}

TEST(KickBits, TakingReturnsWhatWasUpAndClearsIt)
{
	KickBits kicks;
	kicks.Raise(I2EKick::Sif1EeHalf);
	EXPECT_EQ(kicks.Take(I2EKick::Sif1EeHalf), I2EKick::Sif1EeHalf);
	EXPECT_EQ(kicks.Pending(), 0u);
	EXPECT_EQ(kicks.Take(I2EKick::Sif1EeHalf), 0u); // nothing left for a second taker
}

TEST(KickBits, ARaiseAfterATakeIsANewRaise)
{
	KickBits kicks;
	EXPECT_EQ(kicks.Raise(E2IKick::Sif1Data), E2IKick::Sif1Data);
	EXPECT_EQ(kicks.Take(E2IKick::Sif1Data), E2IKick::Sif1Data);
	EXPECT_EQ(kicks.Raise(E2IKick::Sif1Data), E2IKick::Sif1Data);
}

TEST(KickBits, BitsAreIndependent)
{
	KickBits kicks;
	EXPECT_EQ(kicks.Raise(E2IKick::Sif0Space), E2IKick::Sif0Space);
	EXPECT_EQ(kicks.Raise(E2IKick::Sif1Data), E2IKick::Sif1Data);
	EXPECT_EQ(kicks.Pending(), E2IKick::Sif0Space | E2IKick::Sif1Data);

	EXPECT_EQ(kicks.Take(E2IKick::Sif0Space), E2IKick::Sif0Space);
	EXPECT_EQ(kicks.Pending(), E2IKick::Sif1Data); // the other one is still up
	EXPECT_EQ(kicks.Raise(E2IKick::Sif1Data), 0u);
	EXPECT_EQ(kicks.Raise(E2IKick::Sif0Space), E2IKick::Sif0Space);
}

TEST(KickBits, AMultiBitRaiseReportsOnlyTheBitsItActuallyRaised)
{
	KickBits kicks;
	EXPECT_EQ(kicks.Raise(0x3), 0x3u);
	EXPECT_EQ(kicks.Raise(0x6), 0x4u); // bit 1 was up, bit 2 was not
	EXPECT_EQ(kicks.Raise(0x7), 0u);
	EXPECT_EQ(kicks.Pending(), 0x7u);
}

TEST(KickBits, AMultiBitTakeReportsOnlyTheBitsItActuallyTook)
{
	KickBits kicks;
	kicks.Raise(0x5);
	EXPECT_EQ(kicks.Take(0x3), 0x1u); // bit 1 was not up
	EXPECT_EQ(kicks.Pending(), 0x4u);
	EXPECT_EQ(kicks.Take(0x6), 0x4u);
	EXPECT_EQ(kicks.Pending(), 0u);
}

TEST(KickBits, TakeAllClearsEverythingAndReportsWhatWasUp)
{
	KickBits kicks;
	kicks.Raise(0x9);
	EXPECT_EQ(kicks.TakeAll(), 0x9u);
	EXPECT_EQ(kicks.Pending(), 0u);
	EXPECT_EQ(kicks.TakeAll(), 0u);
}

TEST(KickBits, TakingABitYouNeverRaisedDoesNotDisturbTheOthers)
{
	KickBits kicks;
	kicks.Raise(0x2);
	EXPECT_EQ(kicks.Take(0x1), 0u);
	EXPECT_EQ(kicks.Pending(), 0x2u);
}

TEST(KickBits, TheSifPatternQueuesOneEntryNoMatterHowManyRaises)
{
	// Producer: if (Raise(bit)) push an entry.  Consumer: pop the entry, then Take(bit), then act.
	KickBits kicks;
	I2EQueue<8> queue;
	const auto kick = [&](u32 bit, u16 kind) {
		if (kicks.Raise(bit))
			ASSERT_TRUE(queue.TryPush(kind));
	};

	for (int i = 0; i < 50; i++)
		kick(I2EKick::Sif0EeHalf, 1);
	kick(I2EKick::Sif1EeHalf, 2);
	for (int i = 0; i < 50; i++)
		kick(I2EKick::Sif1EeHalf, 2);
	EXPECT_EQ(queue.ApproxSize(), 2u); // 101 raises, two channels, two entries

	I2EItem item;
	ASSERT_TRUE(queue.TryPop(item));
	EXPECT_EQ(item.kind, 1u);
	EXPECT_EQ(kicks.Take(I2EKick::Sif0EeHalf), I2EKick::Sif0EeHalf);

	// A raise after the take, while the consumer is "acting", queues a fresh entry.
	kick(I2EKick::Sif0EeHalf, 1);
	EXPECT_EQ(queue.ApproxSize(), 2u); // channel 1's entry and the new channel 0 one

	ASSERT_TRUE(queue.TryPop(item));
	EXPECT_EQ(item.kind, 2u);
	EXPECT_EQ(kicks.Take(I2EKick::Sif1EeHalf), I2EKick::Sif1EeHalf);
	ASSERT_TRUE(queue.TryPop(item));
	EXPECT_EQ(item.kind, 1u);
	EXPECT_EQ(kicks.Take(I2EKick::Sif0EeHalf), I2EKick::Sif0EeHalf);

	EXPECT_EQ(kicks.Pending(), 0u);
	EXPECT_FALSE(queue.MayHaveItems());
}

TEST(HostCommandQueue, ANewQueueHasNothingToRun)
{
	HostCommandQueue queue;
	EXPECT_FALSE(queue.HasPending());
	EXPECT_EQ(queue.RunPending(), 0u);
	EXPECT_EQ(queue.CompletedCount(), 0u);
	EXPECT_FALSE(queue.IsDone(1)); // tickets start at 1
}

TEST(HostCommandQueue, CommandsRunInTheOrderTheyWerePosted)
{
	HostCommandQueue queue;
	std::vector<int> order;
	for (int i = 0; i < 100; i++)
		queue.Post([&order, i] { order.push_back(i); });
	EXPECT_TRUE(queue.HasPending());
	EXPECT_TRUE(order.empty()); // posting runs nothing

	EXPECT_EQ(queue.RunPending(), 100u);
	ASSERT_EQ(order.size(), 100u);
	for (int i = 0; i < 100; i++)
		EXPECT_EQ(order[i], i);
	EXPECT_FALSE(queue.HasPending());
	EXPECT_EQ(queue.RunPending(), 0u);
	EXPECT_EQ(order.size(), 100u); // nothing ran twice
}

TEST(HostCommandQueue, TicketsCountFromOneInQueueOrder)
{
	HostCommandQueue queue;
	EXPECT_EQ(queue.Post([] {}), 1u);
	EXPECT_EQ(queue.Post([] {}), 2u);
	EXPECT_EQ(queue.Post([] {}), 3u);
	queue.RunPending();
	EXPECT_EQ(queue.Post([] {}), 4u); // numbering carries across batches
}

TEST(HostCommandQueue, ACommandIsNotDoneUntilItHasRun)
{
	HostCommandQueue queue;
	const auto t1 = queue.Post([] {});
	const auto t2 = queue.Post([] {});
	EXPECT_FALSE(queue.IsDone(t1));
	EXPECT_FALSE(queue.IsDone(t2));

	queue.RunPending();
	EXPECT_TRUE(queue.IsDone(t1));
	EXPECT_TRUE(queue.IsDone(t2));
	EXPECT_EQ(queue.CompletedCount(), 2u);
	EXPECT_FALSE(queue.IsDone(t2 + 1));
}

TEST(HostCommandQueue, TheCompletionCountAdvancesOneCommandAtATime)
{
	// A command can look at how many have finished before it: command k sees k.
	HostCommandQueue queue;
	std::vector<u64> seen;
	for (int i = 0; i < 5; i++)
		queue.Post([&] { seen.push_back(queue.CompletedCount()); });
	queue.RunPending();
	EXPECT_EQ(seen, (std::vector<u64>{0, 1, 2, 3, 4}));
	EXPECT_EQ(queue.CompletedCount(), 5u);
}

TEST(HostCommandQueue, ACommandMayPostAnotherAndItRunsInTheNextCall)
{
	HostCommandQueue queue;
	std::vector<std::string> order;
	queue.Post([&] {
		order.push_back("first");
		queue.Post([&] { order.push_back("second"); });
	});

	EXPECT_EQ(queue.RunPending(), 1u);
	EXPECT_EQ(order, (std::vector<std::string>{"first"}));
	EXPECT_TRUE(queue.HasPending()); // the posted command is waiting for the next pass

	EXPECT_EQ(queue.RunPending(), 1u);
	EXPECT_EQ(order, (std::vector<std::string>{"first", "second"}));
	EXPECT_FALSE(queue.HasPending());
}

TEST(HostCommandQueue, ACommandThatCallsRunPendingDoesNotLetLaterCommandsOvertakeIt)
{
	HostCommandQueue queue;
	std::vector<std::string> order;
	size_t inner = 99;
	queue.Post([&] {
		inner = queue.RunPending();
		order.push_back("A");
	});
	queue.Post([&] { order.push_back("B"); });

	EXPECT_EQ(queue.RunPending(), 2u);
	EXPECT_EQ(inner, 0u);
	EXPECT_EQ(order, (std::vector<std::string>{"A", "B"}));
}

TEST(HostCommandQueue, CommandsThatAreStillQueuedWhenItIsDestroyedAreDroppedNotRun)
{
	auto token = std::make_shared<int>(0);
	int ran = 0;
	{
		HostCommandQueue queue;
		queue.Post([token, &ran] { ran++; });
		queue.Post([token, &ran] { ran++; });
		EXPECT_EQ(token.use_count(), 3);
	}
	EXPECT_EQ(ran, 0);
	EXPECT_EQ(token.use_count(), 1); // the captured copies were destroyed with the queue
}

TEST(HostCommandQueue, ACommandIsDestroyedAfterItRuns)
{
	auto token = std::make_shared<int>(0);
	HostCommandQueue queue;
	queue.Post([token] {});
	EXPECT_EQ(token.use_count(), 2);
	queue.RunPending();
	EXPECT_EQ(token.use_count(), 1);
}

TEST(HostCommandQueue, ACommandCanOwnItsStateThroughASharedPointer)
{
	// Commands are std::function, so they must be copyable; shared ownership is how a job keeps
	// what it needs alive.
	HostCommandQueue queue;
	auto state = std::make_shared<int>(41);
	int result = 0;
	queue.Post([state, &result] { result = ++*state; });
	queue.RunPending();
	EXPECT_EQ(result, 42);
	EXPECT_EQ(*state, 42);
}

TEST(HostCommandQueueThreads, ACommandPostedOnAnotherThreadRunsOnTheCallingThread)
{
	HostCommandQueue queue;
	std::thread::id ran_on;
	HostCommandQueue::Ticket ticket = 0;
	std::thread poster([&] { ticket = queue.Post([&] { ran_on = std::this_thread::get_id(); }); });
	poster.join();

	EXPECT_TRUE(queue.HasPending());
	EXPECT_EQ(queue.RunPending(), 1u);
	EXPECT_EQ(ran_on, std::this_thread::get_id());
	EXPECT_TRUE(queue.IsDone(ticket));
}

TEST(HostCommandQueueThreads, WhoeverSeesATicketDoneAlsoSeesWhatTheCommandWrote)
{
	// The commands write plain memory. The poster reads it only after IsDone(), so ThreadSanitizer
	// would flag a missing release/acquire pair, and a stale read would fail the check.
	constexpr u64 kRounds = 20000;
	HostCommandQueue queue;
	std::atomic<bool> stop{false};
	u64 value = 0; // plain, written on the consumer thread

	std::thread consumer([&] {
		Backoff backoff;
		while (!stop.load(std::memory_order_acquire))
		{
			if (queue.HasPending())
			{
				queue.RunPending();
				backoff.Reset();
			}
			else
			{
				backoff.Wait();
			}
		}
		queue.RunPending();
	});

	bool ok = true;
	Deadline deadline(kStressTimeout);
	for (u64 round = 1; round <= kRounds && ok; round++)
	{
		const auto ticket = queue.Post([&value, round] { value = round * 3; });
		Backoff backoff;
		while (!queue.IsDone(ticket))
		{
			if (deadline.Expired())
			{
				ok = false;
				break;
			}
			backoff.Wait();
		}
		if (ok && value != round * 3)
			ok = false;
	}
	stop.store(true, std::memory_order_release);
	consumer.join();
	EXPECT_TRUE(ok);
}

TEST(HostCommandQueueThreads, SeveralPostingThreadsLoseNothingAndKeepEachOnesOrder)
{
	constexpr int kPosters = 4;
	constexpr u32 kPerPoster = 50000;

	HostCommandQueue queue;
	std::atomic<bool> posters_done{false};
	FailureLog log;
	std::vector<u32> next_expected(kPosters, 0); // touched only by the consumer thread
	u64 ran = 0;

	std::vector<std::vector<HostCommandQueue::Ticket>> tickets(kPosters);
	std::vector<std::thread> posters;
	for (int p = 0; p < kPosters; p++)
	{
		tickets[p].reserve(kPerPoster);
		posters.emplace_back([&, p] {
			Jitter jitter(100 + p);
			for (u32 n = 0; n < kPerPoster; n++)
			{
				jitter.Step();
				tickets[p].push_back(queue.Post([&, p, n] {
					if (next_expected[p] != n)
						log.Fail("poster " + std::to_string(p) + " ran command " + std::to_string(n) + " when " +
								 std::to_string(next_expected[p]) + " was due");
					next_expected[p] = n + 1;
					ran++;
				}));
			}
		});
	}

	std::thread consumer([&] {
		Jitter jitter(7);
		Backoff backoff;
		Deadline deadline(kStressTimeout);
		while (!log.Failed())
		{
			jitter.Step();
			if (queue.HasPending())
			{
				queue.RunPending();
				backoff.Reset();
				continue;
			}
			if (posters_done.load(std::memory_order_acquire))
			{
				// Everything has been posted: one more pass picks up the rest.
				queue.RunPending();
				return;
			}
			if (deadline.Expired())
			{
				log.Fail("consumer timed out");
				return;
			}
			backoff.Wait();
		}
	});

	for (std::thread& t : posters)
		t.join();
	posters_done.store(true, std::memory_order_release);
	consumer.join();

	ASSERT_FALSE(log.Failed()) << log.Message();
	EXPECT_EQ(ran, u64(kPosters) * kPerPoster);
	for (int p = 0; p < kPosters; p++)
		EXPECT_EQ(next_expected[p], kPerPoster) << p;

	// Each poster's tickets rise, and together they are exactly 1..N with no gap and no repeat.
	std::vector<HostCommandQueue::Ticket> all;
	for (int p = 0; p < kPosters; p++)
	{
		// is_sorted with "<=" as the comparison is true only when each ticket is strictly larger.
		EXPECT_TRUE(std::is_sorted(tickets[p].begin(), tickets[p].end(), std::less_equal<>())) << p;
		all.insert(all.end(), tickets[p].begin(), tickets[p].end());
	}
	std::sort(all.begin(), all.end());
	ASSERT_EQ(all.size(), size_t(kPosters) * kPerPoster);
	for (size_t i = 0; i < all.size(); i++)
	{
		if (all[i] != i + 1)
		{
			ADD_FAILURE() << "ticket " << i + 1 << " is missing or repeated";
			break;
		}
	}
	EXPECT_EQ(queue.CompletedCount(), u64(kPosters) * kPerPoster);
}

TEST(IopChannelsStress, StampedRingTinyRingNeverLosesOrReordersAnything)
{
	std::string failure;
	const StressStats stats = RunE2IStress<16>(kStressItems, 1, failure);
	EXPECT_TRUE(failure.empty()) << failure;
	// With sixteen slots and random pauses the ring has to have been full and empty many times,
	// or this test did not reach the boundary cases.
	EXPECT_GT(stats.full_hits, 100u);
	EXPECT_GT(stats.empty_hits, 100u);
	RecordProperty("full_hits", static_cast<int>(std::min<u64>(stats.full_hits, INT32_MAX)));
	RecordProperty("empty_hits", static_cast<int>(std::min<u64>(stats.empty_hits, INT32_MAX)));
}

TEST(IopChannelsStress, StampedRingAtTheDesignCapacityNeverLosesOrReordersAnything)
{
	std::string failure;
	const StressStats stats = RunE2IStress<kE2IRingCapacity>(kStressItems, 2, failure);
	EXPECT_TRUE(failure.empty()) << failure;
	RecordProperty("full_hits", static_cast<int>(std::min<u64>(stats.full_hits, INT32_MAX)));
	RecordProperty("empty_hits", static_cast<int>(std::min<u64>(stats.empty_hits, INT32_MAX)));
}

TEST(IopChannelsStress, I2EQueueTinyQueueNeverLosesOrReordersAnything)
{
	std::string failure;
	const StressStats stats = RunI2EStress<8>(kStressItems, 3, failure);
	EXPECT_TRUE(failure.empty()) << failure;
	EXPECT_GT(stats.full_hits, 100u);
	EXPECT_GT(stats.empty_hits, 100u);
	RecordProperty("full_hits", static_cast<int>(std::min<u64>(stats.full_hits, INT32_MAX)));
	RecordProperty("empty_hits", static_cast<int>(std::min<u64>(stats.empty_hits, INT32_MAX)));
}

TEST(IopChannelsStress, I2EQueueAtTheDesignCapacityNeverLosesOrReordersAnything)
{
	std::string failure;
	const StressStats stats = RunI2EStress<kI2EQueueCapacity>(kStressItems, 4, failure);
	EXPECT_TRUE(failure.empty()) << failure;
	RecordProperty("full_hits", static_cast<int>(std::min<u64>(stats.full_hits, INT32_MAX)));
	RecordProperty("empty_hits", static_cast<int>(std::min<u64>(stats.empty_hits, INT32_MAX)));
}

TEST(IopChannelsStress, StampedRingNeverShowsAWatermarkAheadOfItsItems)
{
	for (u64 seed = 10; seed < 13; seed++)
	{
		std::string failure;
		(void)RunE2IStress<32>(kStressItems / 10, seed, failure);
		EXPECT_TRUE(failure.empty()) << "seed " << seed << ": " << failure;
	}
}

TEST(IopChannelsStress, KickBitsRaisedUnderRandomDelaysQueueOneEntryPerRaiseThatWon)
{
	// The SIF kick protocol between two threads. The IOP raises a channel's bit and, if the
	// raise took the bit from clear, queues one entry for it; if the queue is full it retries, so
	// the bit never sits up with nothing queued. The EE pops an entry, takes that channel's bit,
	// and counts it.
	//
	// What has to hold:
	//  - when the EE pops a channel's entry, that channel's bit is up (the entry follows its raise);
	//  - at the end, entries seen equal the raises that took a bit from clear, per channel;
	//  - at the end no bit is left up (nothing was swallowed by an earlier raise and never queued);
	//  - some raises were coalesced, or the test never exercised coalescing.
	constexpr u32 kChannels = 2;
	const u32 bits[kChannels] = {I2EKick::Sif0EeHalf, I2EKick::Sif1EeHalf};

	KickBits kicks;
	auto queue = std::make_unique<I2EQueue<8>>();
	FailureLog log;
	u64 raises_won[kChannels] = {};
	u64 raises_coalesced[kChannels] = {};
	u64 entries_seen[kChannels] = {};

	std::thread iop([&] {
		Jitter jitter(21);
		Rng rng(22);
		Backoff backoff;
		Deadline deadline(kStressTimeout);
		for (u64 i = 0; i < kStressItems; i++)
		{
			jitter.Step();
			const u32 channel = rng.Below(kChannels);
			if (kicks.Raise(bits[channel]) == 0)
			{
				raises_coalesced[channel]++;
				continue;
			}
			raises_won[channel]++;
			while (!queue->TryPush(static_cast<u16>(channel)))
			{
				if (log.Failed() || deadline.Expired())
				{
					log.Fail("the IOP side gave up waiting for room in the queue");
					return;
				}
				backoff.Wait();
			}
			backoff.Reset();
		}
	});

	std::atomic<bool> iop_done{false};
	std::thread ee([&] {
		Jitter jitter(23);
		Backoff backoff;
		Deadline deadline(kStressTimeout);

		// Handles one entry. False if it is wrong.
		const auto handle = [&](const I2EItem& item) {
			if (item.kind >= kChannels)
			{
				log.Fail("an entry for a channel that does not exist");
				return false;
			}
			if (kicks.Take(bits[item.kind]) != bits[item.kind])
			{
				log.Fail("an entry came out of the queue but its bit was not up");
				return false;
			}
			entries_seen[item.kind]++;
			return true;
		};

		for (;;)
		{
			jitter.Step();
			if (log.Failed())
				return;
			I2EItem item;
			if (queue->TryPop(item))
			{
				backoff.Reset();
				if (!handle(item))
					return;
				continue;
			}
			// iop_done is set after the IOP thread has been joined, so once it reads true every
			// entry that will ever exist is in the queue. Drain those and stop.
			if (iop_done.load(std::memory_order_acquire))
			{
				while (queue->TryPop(item))
				{
					if (!handle(item))
						return;
				}
				return;
			}
			if (deadline.Expired())
			{
				log.Fail("the EE side timed out");
				return;
			}
			backoff.Wait();
		}
	});

	iop.join();
	iop_done.store(true, std::memory_order_release);
	ee.join();

	ASSERT_FALSE(log.Failed()) << log.Message();
	for (u32 c = 0; c < kChannels; c++)
	{
		EXPECT_EQ(entries_seen[c], raises_won[c]) << "channel " << c;
		EXPECT_GT(raises_coalesced[c], 0u) << "channel " << c;
	}
	EXPECT_EQ(raises_won[0] + raises_won[1] + raises_coalesced[0] + raises_coalesced[1], kStressItems);
	EXPECT_EQ(kicks.Pending(), 0u) << "a bit was left up with nothing queued for it";
}

TEST(IopChannelsStress, DataWrittenBeforeARaiseIsVisibleAfterTheTakeThatSeesIt)
{
	// Message passing through the kick word. The producer writes plain data, then raises. The
	// consumer takes the bit and reads the data. Then it acknowledges, and only then does the
	// producer write the next round. The plain accesses are race-free only if Raise releases and
	// Take acquires, which is what ThreadSanitizer checks and what a stale read would show.
	constexpr u64 kRounds = 200000;
	KickBits kicks;
	std::atomic<u64> ack{0};
	u64 data_a = 0, data_b = 0; // plain
	FailureLog log;

	std::thread producer([&] {
		Backoff backoff;
		Deadline deadline(kStressTimeout);
		for (u64 round = 1; round <= kRounds; round++)
		{
			while (ack.load(std::memory_order_acquire) != round - 1)
			{
				if (log.Failed() || deadline.Expired())
				{
					log.Fail("producer timed out");
					return;
				}
				backoff.Wait();
			}
			backoff.Reset();
			data_a = round;
			data_b = ~round;
			if (kicks.Raise(E2IKick::Sif1Data) != E2IKick::Sif1Data)
			{
				log.Fail("a raise in a fresh round found its bit already up");
				return;
			}
		}
	});

	std::thread consumer([&] {
		Backoff backoff;
		Deadline deadline(kStressTimeout);
		for (u64 round = 1; round <= kRounds; round++)
		{
			while (kicks.Take(E2IKick::Sif1Data) == 0)
			{
				if (log.Failed() || deadline.Expired())
				{
					log.Fail("consumer timed out");
					return;
				}
				backoff.Wait();
			}
			backoff.Reset();
			if (data_a != round || data_b != ~round)
			{
				log.Fail("round " + std::to_string(round) + " read stale data after taking its kick");
				return;
			}
			ack.store(round, std::memory_order_release);
		}
	});

	producer.join();
	consumer.join();
	EXPECT_FALSE(log.Failed()) << log.Message();
}

TEST(IopChannelsStress, HostCommandQueueNeverLosesOrReordersACommand)
{
	HostCommandQueue queue;
	FailureLog log;
	// Written by the commands on the consumer thread and read by the poster, so it is atomic; every
	// access is relaxed. What the poster checks with it needs the release/acquire on the completion
	// count: after IsDone(t) the counter cannot read below t.
	std::atomic<u64> executed{0};
	std::atomic<bool> posted_all{false};

	std::thread consumer([&] {
		Jitter jitter(31);
		Backoff backoff;
		Deadline deadline(kStressTimeout);
		for (;;)
		{
			jitter.Step();
			if (log.Failed())
				return;
			if (queue.HasPending())
			{
				queue.RunPending();
				backoff.Reset();
				continue;
			}
			if (posted_all.load(std::memory_order_acquire))
			{
				queue.RunPending(); // the last Post() returned before the flag was set
				return;
			}
			if (deadline.Expired())
			{
				log.Fail("consumer timed out");
				return;
			}
			backoff.Wait();
		}
	});

	HostCommandQueue::Ticket last_ticket = 0;
	std::thread poster([&] {
		Jitter jitter(32);
		for (u64 i = 0; i < kStressItems && !log.Failed(); i++)
		{
			jitter.Step();
			// Three words of capture: depending on the standard library this is beyond
			// std::function's inline storage, and then each Post() allocates. That is a cost the
			// class documents, and this is the side that pays it.
			last_ticket = queue.Post([&executed, &log, i] {
				const u64 before = executed.load(std::memory_order_relaxed);
				if (before != i)
					log.Fail("command " + std::to_string(i) + " ran when " + std::to_string(before) + " commands had run");
				executed.store(i + 1, std::memory_order_relaxed);
			});

			if ((i & 1023) == 1023)
			{
				const u64 completed = queue.CompletedCount();
				if (completed > i + 1)
					log.Fail("more commands completed than were posted");
				if (executed.load(std::memory_order_relaxed) < completed)
					log.Fail("a completed ticket was visible before its effects");
			}
		}
		posted_all.store(true, std::memory_order_release);
	});

	poster.join();
	consumer.join();

	ASSERT_FALSE(log.Failed()) << log.Message();
	EXPECT_EQ(executed.load(), kStressItems);
	EXPECT_EQ(last_ticket, kStressItems);
	EXPECT_TRUE(queue.IsDone(last_ticket));
	EXPECT_EQ(queue.CompletedCount(), kStressItems);
	EXPECT_FALSE(queue.HasPending());
}
