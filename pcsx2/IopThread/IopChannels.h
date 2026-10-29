// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The queues and flags the EE thread and the IOP thread use to talk to each other.
//
//   StampedRing<N>     EE -> IOP, items carry an EE cycle stamp. One producer, one consumer.
//   I2EQueue<N>        IOP -> EE, ordered. One producer, one consumer.
//   KickBits           a word of atomic flags that coalesces repeated raises, either direction.
//   HostCommandQueue   any host thread -> IOP, a mutex-protected list of commands.
//
// What the EE-side calls of these promise:
//   - they never block and never take a lock;
//   - they never allocate. The rings keep their storage inline, so even constructing one does not;
//   - a full ring is reported to the caller as `false`, and the caller decides what to do about it.
// HostCommandQueue is the exception: it takes a mutex and allocates, so it is for host threads and
// for lifecycle code, never for the emulation path.
//
// Each ring keeps its producer's words and its consumer's words on separate cache lines, and each
// side keeps a private copy of the other side's index, refreshed (with an acquire load) only when
// the ring looks full or empty. The common case therefore touches the other core's line not at all.

#pragma once

#include "common/Assertions.h"
#include "common/Pcsx2Defs.h"
#include "common/Pcsx2Types.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <type_traits>
#include <vector>

namespace IopThread
{
	inline constexpr size_t kE2IRingCapacity = 1024;
	inline constexpr size_t kI2EQueueCapacity = 256;

	// The alignment that keeps one side's hot words off the other side's cache line.
	//
	// __cachelinesize is not enough on its own: the Linux ARM64 build takes it from the kernel,
	// which reports 64 on the M2, while the M2 keeps its caches coherent in 128-byte units. Two
	// words 64 bytes apart would still share a line there. Taking the larger of the two costs a few
	// bytes of padding on a 64-byte machine and nothing else.
	inline constexpr size_t kChannelLineSize = __cachelinesize > 128 ? __cachelinesize : 128;

	namespace detail
	{
		/// A bounded single-producer single-consumer ring of trivially copyable items.
		///
		/// The indices count up forever and are masked on use, so all N slots are usable and
		/// "full" is `write - read == N`. A u64 does not wrap in any run that matters.
		///
		/// Producer-side calls: TryPush, LastPushed. Consumer-side calls: Peek, Pop, TryPop,
		/// MayHaveItems. Reset needs both sides stopped. ApproxSize may be called from anywhere
		/// and is only a hint.
		template <typename T, size_t N>
		class SpscRing
		{
			static_assert(N >= 2 && (N & (N - 1)) == 0, "the capacity must be a power of two");
			static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
				"slots are overwritten without construction or destruction");
			static_assert(std::atomic<u64>::is_always_lock_free);

			static constexpr u64 kMask = N - 1;

		public:
			static constexpr size_t Capacity() { return N; }

			/// Producer. Copies `item` into the ring and publishes it. Returns false, leaving the ring
			/// unchanged, when it is full.
			[[nodiscard]] bool TryPush(const T& item) noexcept
			{
				// Only this thread stores m_write, so it can read its own last value relaxed.
				const u64 w = m_write.load(std::memory_order_relaxed);

				if (w - m_cached_read >= N)
				{
					// Looks full against a possibly stale copy of the read index; look again.
					// Pairs with the release store in Pop(): the consumer's reads of a slot happen
					// before this thread overwrites it.
					m_cached_read = m_read.load(std::memory_order_acquire);
					if (w - m_cached_read >= N)
						return false;
				}

				m_slots[w & kMask] = item;

				// Pairs with the acquire load of m_write in Peek(): the slot's contents are visible
				// to the consumer before the index that announces them.
				m_write.store(w + 1, std::memory_order_release);
				return true;
			}

			/// Producer. The item pushed last, or nullptr if there is none since construction or
			/// Reset. The slot is the producer's own write and is not touched again until the ring
			/// has gone round, so the producer may read it even if the consumer has popped it.
			const T* LastPushed() const noexcept
			{
				const u64 w = m_write.load(std::memory_order_relaxed);
				return w == 0 ? nullptr : &m_slots[(w - 1) & kMask];
			}

			/// Consumer. The oldest item, or nullptr if the ring is empty. The pointer stays valid
			/// until Pop(): the producer cannot reuse a slot before the consumer releases it.
			const T* Peek() noexcept
			{
				// Only this thread stores m_read, so it can read its own last value relaxed.
				const u64 r = m_read.load(std::memory_order_relaxed);

				if (r == m_cached_write)
				{
					// Looks empty against a possibly stale copy of the write index; look again.
					// Pairs with the release store in TryPush(): once the index is seen, so are the
					// slot contents behind it.
					m_cached_write = m_write.load(std::memory_order_acquire);
					if (r == m_cached_write)
						return nullptr;
				}

				return &m_slots[r & kMask];
			}

			/// Consumer. Releases the item Peek() returned. Calling it without a successful Peek()
			/// since the last Pop() is a bug.
			void Pop() noexcept
			{
				const u64 r = m_read.load(std::memory_order_relaxed);
				pxAssertMsg(r != m_cached_write, "Pop() without an item from Peek()");

				// Pairs with the acquire load of m_read in TryPush(): the slot is free to reuse
				// only after the consumer has finished reading it.
				m_read.store(r + 1, std::memory_order_release);
			}

			[[nodiscard]] bool TryPop(T& out) noexcept
			{
				const T* item = Peek();
				if (item == nullptr)
					return false;
				out = *item;
				Pop();
				return true;
			}

			/// Consumer. A cheap test for "worth calling Peek()": two relaxed loads, no acquire.
			/// True is exact (the producer has published something this thread has not popped).
			/// False can be stale by the item being published right now, so a caller that must not
			/// miss one tries again at its next opportunity, as the EE's event test does.
			bool MayHaveItems() const noexcept
			{
				// No pairing, and nothing is read on the strength of it: a true answer is followed by
				// Peek() or TryPop(), whose acquire load of m_write is what makes the slot visible.
				return m_write.load(std::memory_order_relaxed) != m_read.load(std::memory_order_relaxed);
			}

			/// Any thread. How many items are in the ring, give or take what is in flight. For
			/// statistics and tests only: the two indices are read separately and relaxed.
			size_t ApproxSize() const noexcept
			{
				const u64 r = m_read.load(std::memory_order_relaxed);
				const u64 w = m_write.load(std::memory_order_relaxed);
				const s64 d = static_cast<s64>(w - r);
				return d < 0 ? 0 : (d > static_cast<s64>(N) ? N : static_cast<size_t>(d));
			}

			/// Empties the ring. Both sides must be stopped, or the caller must otherwise know that
			/// neither is inside a call.
			void Reset() noexcept
			{
				// Relaxed: the hold or start handshake that stopped both sides orders these stores
				// against the first call after it.
				m_write.store(0, std::memory_order_relaxed);
				m_cached_read = 0;
				m_read.store(0, std::memory_order_relaxed);
				m_cached_write = 0;
			}

			/// Byte offsets of the parts of the ring, for the layout test.
			struct Layout
			{
				size_t write_offset;
				size_t read_offset;
				size_t slots_offset;
				size_t total_size;
			};
			static constexpr Layout GetLayout()
			{
				return {offsetof(SpscRing, m_write), offsetof(SpscRing, m_read), offsetof(SpscRing, m_slots), sizeof(SpscRing)};
			}

		private:
			// The producer's line: the index it publishes, and its last look at the consumer's.
			alignas(kChannelLineSize) std::atomic<u64> m_write{0};
			u64 m_cached_read = 0;

			// The consumer's line: the index it publishes, and its last look at the producer's.
			alignas(kChannelLineSize) std::atomic<u64> m_read{0};
			u64 m_cached_write = 0;

			// The slots start on a line of their own, so neither index shares one with an item.
			alignas(kChannelLineSize) T m_slots[N];
		};
	} // namespace detail

	struct E2IItem
	{
		u64 stamp; // the EE cycle at which the IOP applies the item
		u16 kind;
		u16 pad;
		u32 arg;
	};
	static_assert(sizeof(E2IItem) == 16);

	/// The EE-to-IOP queue: a ring of items stamped with the EE cycle they belong to.
	///
	/// Producer: the EE thread only. Consumer: the IOP thread only. Stamps must not go backwards
	/// from one push to the next (the EE posts at `cpuRegs.cycle`, which does not), so the ring is
	/// ordered by stamp as well as by arrival. A push with an earlier stamp than the last is a bug
	/// that Devel builds assert on.
	///
	/// The IOP applies an item once its own clock has reached the stamp: TryPopDue() hands out the
	/// head only if it is due, and NextStamp() says when the next one will be.
	template <size_t N>
	class StampedRing
	{
	public:
		/// What NextStamp() returns when the ring is empty. No real stamp reaches it.
		static constexpr u64 kNoStamp = ~u64(0);

		static constexpr size_t Capacity() { return N; }

		[[nodiscard]] bool TryPush(u64 stamp, u16 kind, u32 arg = 0) noexcept
		{
			pxAssertMsg(StampDoesNotGoBackwards(stamp), "E2I stamps must not go backwards");
			return m_ring.TryPush(E2IItem{stamp, kind, 0, arg});
		}

		/// Consumer. The head item, or nullptr if empty. Valid until Pop().
		const E2IItem* Peek() noexcept { return m_ring.Peek(); }

		void Pop() noexcept { m_ring.Pop(); }

		u64 NextStamp() noexcept
		{
			const E2IItem* head = m_ring.Peek();
			return head ? head->stamp : kNoStamp;
		}

		/// Consumer. Pops the head into `out` if its stamp is at or before `now`. Returns false,
		/// leaving the ring unchanged, if it is empty or the head is not due yet.
		[[nodiscard]] bool TryPopDue(u64 now, E2IItem& out) noexcept
		{
			const E2IItem* head = m_ring.Peek();
			if (head == nullptr || head->stamp > now)
				return false;
			out = *head;
			m_ring.Pop();
			return true;
		}

		/// Consumer. Cheap test for "worth calling Peek()"; see SpscRing::MayHaveItems.
		bool MayHaveItems() const noexcept { return m_ring.MayHaveItems(); }

		size_t ApproxSize() const noexcept { return m_ring.ApproxSize(); }

		/// Both threads stopped.
		void Reset() noexcept { m_ring.Reset(); }

		static constexpr auto GetLayout() { return detail::SpscRing<E2IItem, N>::GetLayout(); }

	private:
		// Producer. For the Devel assert in TryPush: the previous item's stamp is still in its slot,
		// which only the producer writes.
		bool StampDoesNotGoBackwards(u64 stamp) const noexcept
		{
			const E2IItem* last = m_ring.LastPushed();
			return last == nullptr || stamp >= last->stamp;
		}

		detail::SpscRing<E2IItem, N> m_ring;
	};

	struct I2EItem
	{
		u16 kind;
		u16 pad;
		u32 arg;
	};
	static_assert(sizeof(I2EItem) == 8);

	/// The IOP-to-EE queue: an ordered ring, so the EE sees the IOP's effects in the order the IOP
	/// produced them (an SBUS interrupt raised after SIF data was produced cannot overtake it).
	///
	/// Producer: the IOP thread only. Consumer: the EE thread only. The EE side never blocks and
	/// never allocates. A full queue is the IOP's to wait out: the EE empties it at its next event
	/// test, which is the direction the design allows waiting in.
	template <size_t N>
	class I2EQueue
	{
	public:
		static constexpr size_t Capacity() { return N; }

		[[nodiscard]] bool TryPush(u16 kind, u32 arg = 0) noexcept
		{
			return m_ring.TryPush(I2EItem{kind, 0, arg});
		}

		[[nodiscard]] bool TryPop(I2EItem& out) noexcept { return m_ring.TryPop(out); }

		/// Consumer (EE). The oldest item, or nullptr if empty. Valid until Pop().
		const I2EItem* Peek() noexcept { return m_ring.Peek(); }

		void Pop() noexcept { m_ring.Pop(); }

		/// Consumer (EE). The check the event test makes on every pass: a relaxed load of the write
		/// index against the EE's own read index. False can lag by the item being published right
		/// now; the next pass sees it.
		bool MayHaveItems() const noexcept { return m_ring.MayHaveItems(); }

		size_t ApproxSize() const noexcept { return m_ring.ApproxSize(); }

		/// Both threads stopped.
		void Reset() noexcept { m_ring.Reset(); }

		static constexpr auto GetLayout() { return detail::SpscRing<I2EItem, N>::GetLayout(); }

	private:
		detail::SpscRing<I2EItem, N> m_ring;
	};

	/// Bits of the EE-to-IOP kick word. Used only by the split SIF.
	namespace E2IKick
	{
		inline constexpr u32 Sif0Space = 1u << 0;
		inline constexpr u32 Sif1Data = 1u << 1;
	} // namespace E2IKick

	/// Bits of the IOP-to-EE "kick pending" word: one per SIF channel, set when the IOP has
	/// queued a SIF entry the EE has not yet taken. Used only by the split SIF.
	namespace I2EKick
	{
		inline constexpr u32 Sif0EeHalf = 1u << 0;
		inline constexpr u32 Sif1EeHalf = 1u << 1;
	} // namespace I2EKick

	/// A word of atomic flags that coalesces: raising a flag that is already up does nothing, and
	/// says so. One side raises, the other takes.
	///
	/// The pattern for a notification that must not pile up (the SIF entries):
	///
	///     producer:  write the data;  if (kicks.Raise(bit)) queue.TryPush(kind);   // retry if full
	///     consumer:  queue.TryPop(item);  kicks.Take(bit);  read the data;  act on it
	///
	/// However many times the producer raises between two takes, the queue gets one entry. The
	/// consumer takes the bit BEFORE it acts, so a raise that arrives while it is acting finds the
	/// bit clear and queues a fresh entry. The producer must see its entry into the queue: if the
	/// push fails because the queue is full it has to retry, or the bit stays up with nothing
	/// queued and every later raise is swallowed.
	///
	/// For flags with no queue behind them (the EE-to-IOP kicks) the producer wakes the consumer
	/// when Raise() returns nonzero and the consumer polls Pending().
	class alignas(kChannelLineSize) KickBits
	{
	public:
		/// Sets `bits`. Returns the subset that was clear before, that is, the bits this call
		/// raised. Zero means every bit was already up and the earlier raise still covers this one.
		u32 Raise(u32 bits) noexcept
		{
			// Release pairs with the acquire in Take(): whatever the producer wrote before raising
			// (the FIFO contents) is visible to the consumer that takes the bit. It also holds for a
			// raise that returns zero, because that raise is a read-modify-write of the same word, so
			// a Take that reads its value synchronises with it.
			return bits & ~m_bits.fetch_or(bits, std::memory_order_release);
		}

		/// Clears `bits`. Returns the subset that was set, that is, the bits this call took.
		u32 Take(u32 bits) noexcept
		{
			// Pairs with the release in Raise().
			return bits & m_bits.fetch_and(~bits, std::memory_order_acquire);
		}

		u32 TakeAll() noexcept
		{
			// Pairs with the release in Raise().
			return m_bits.exchange(0, std::memory_order_acquire);
		}

		/// The bits currently up. Relaxed: a hint for "worth calling Take()", which does the
		/// acquire. It orders nothing, so do not read data on the strength of it.
		u32 Pending() const noexcept { return m_bits.load(std::memory_order_relaxed); }

	private:
		std::atomic<u32> m_bits{0};
	};

	/// Commands from any host thread to the IOP thread: the CDVD disc thread's new-disc callback,
	/// the FPCR re-apply after a settings change.
	///
	/// Commands run on the IOP thread, in the order they were posted (the order they won the
	/// mutex, for two posting threads), outside the lock, so a command may post another. A command
	/// posted from inside RunPending() runs in the next call. Commands must not throw, and as they are
	/// std::function they must be copyable (share state through a shared_ptr).
	///
	/// Completion: Post() returns a ticket. IsDone(ticket) turns true when that command and every
	/// one before it has run, and everything the command wrote is then visible to the thread that
	/// saw it. There is no blocking wait: a poster that needs one polls IsDone(), or captures its
	/// own signal in the command.
	class HostCommandQueue
	{
	public:
		using Command = std::function<void()>;
		using Ticket = u64;

		HostCommandQueue();

		/// Commands still queued are destroyed without being run.
		~HostCommandQueue();

		HostCommandQueue(const HostCommandQueue&) = delete;
		HostCommandQueue& operator=(const HostCommandQueue&) = delete;

		/// Any thread. Queues `command` (which must not be empty) and returns its ticket. Tickets
		/// count 1, 2, 3 ... in queue order. Ignores the gate below: this is the form the lifecycle
		/// code uses for a command it knows the IOP thread will take.
		Ticket Post(Command command);

		/// The gate for the threads that cannot know whether an IOP thread exists (the physical-disc
		/// thread). A new queue is closed. The owner opens it as its thread starts and closes it before
		/// the thread stops, then runs what is still queued: a command that TryPost() accepted is never
		/// left behind, and one it refused is the caller's to run. Open and close need no other
		/// synchronisation; they take the same mutex as TryPost.
		void Open();
		void Close();
		bool IsOpen() const noexcept { return m_open.load(std::memory_order_acquire); }

		/// Any thread. Post() while the gate is open; 0 (no ticket, `command` not queued) while it is
		/// closed.
		Ticket TryPost(Command command);

		/// IOP thread: the per-slice check. One relaxed load. True can lag a Post() on another
		/// thread by a moment, which the next slice makes up. Relaxed and pairing with nothing:
		/// it is only a hint to call RunPending(), and the mutex in RunPending() is what orders the
		/// commands themselves.
		bool HasPending() const noexcept { return m_pending.load(std::memory_order_relaxed); }

		/// IOP thread only. Runs every command queued at the moment it takes the lock, in order,
		/// and returns how many it ran. Returns 0 straight away if called from inside a command.
		size_t RunPending();

		bool IsDone(Ticket ticket) const noexcept
		{
			// Pairs with the release store of m_completed in RunPending().
			return m_completed.load(std::memory_order_acquire) >= ticket;
		}

		u64 CompletedCount() const noexcept { return m_completed.load(std::memory_order_acquire); }

	private:
		// The flag the IOP thread reads every slice, alone on its line so that posting threads
		// taking the mutex beside it do not invalidate that line.
		alignas(kChannelLineSize) std::atomic<bool> m_pending{false};

		// Written by the IOP thread after each command; read by whoever waits on a ticket.
		alignas(kChannelLineSize) std::atomic<u64> m_completed{0};

		alignas(kChannelLineSize) std::mutex m_mutex;
		std::vector<Command> m_queue; // guarded by m_mutex
		Ticket m_posted = 0; // guarded by m_mutex
		std::atomic<bool> m_open{false}; // written under m_mutex

		// The IOP thread's own list. RunPending swaps it with m_queue, so the IOP thread reuses
		// one buffer and does not allocate once it has grown to the working size.
		std::vector<Command> m_batch;
		bool m_running = false;
	};
} // namespace IopThread
