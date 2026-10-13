// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP thread object (IOP-thread design sections 3.3 to 3.10, 4, 5 and 11): the thread, its
// pacing loop, the EE's watermark (`ee_wm`) and the IOP's published position (`iop_pos`), holds,
// CallOnEE, the parked flag and wake-ups, and the Lockstep handoff. The VM uses Lockstep through
// IopThreadVm.h; Threaded is not wired in yet.
//
// The object knows nothing about the IOP. It is given two functions, and a few optional ones:
//   run_slice(from, target)  runs the IOP from EE cycle `from` towards `target` and returns the
//                            cycle it reached. It may end up to one block past `target`, as the
//                            JIT does when it checks its budget at block ends.
//   apply_item(item)         applies one EE-to-IOP item (blanking, an IRQ, a job).
// A test hands it a fake IOP; the emulator hands it iopRunSliceOnIopThread() (IopThreadVm.cpp).
//
// ---------------------------------------------------------------------------------------------
// Who calls what
//   EE thread (the CPU thread): Start, Stop, PublishWatermark, CheckLag, PostE2I, KickIop,
//     MayHaveI2E, DrainI2E, Wake, Resync, ScopedHold, LockstepRunSlice, LockstepApplyUpTo.
//   IOP thread: CallOnEE, PostI2E, and the functions the object was given.
//   Any thread: PostHostCommand, IsHostCommandDone, ReapplyFpcr, and the accessors.
//
// The three waits the EE has (design section 4) are CheckLag (W1, the lag bound), ScopedHold (W2
// and W4, the holds) and PostE2I (W3, a full queue). None of them sleeps in the emulation path.
// Each spins with the CPU's yield hint, empties the IOP-to-EE queue and runs CallOnEE functions on
// every iteration, and asks Params::should_stop every 1024 iterations. A ScopedHold of kind
// Lifecycle may fall back to short sleeps after SPIN_TIME_NS, because pause, savestate and reset
// are not on the emulation path.
//
// ---------------------------------------------------------------------------------------------
// States of the IOP thread (Threaded)
//
//   PARKED   no work. iop_parked is set and the thread is in WorkSema::WaitForWorkWithSpin.
//   RUNNING  the pacing loop of design section 5.1: apply host commands, kicks and due items, run
//            a slice to min(ee_wm, next item stamp), publish iop_pos.
//   HELD     the EE asked for a hold, the IOP ran up to the hold's cycle and stopped. It touches
//            nothing until the EE releases it.
//   BLOCKED  inside CallOnEE or PostI2E, waiting for the EE. It leaves when the EE serves the call
//            or empties the queue, and when the object is stopped.
//
// Transitions, with the atomic that makes each one happen:
//   RUNNING -> PARKED   no budget: `iop_parked.store(true, seq_cst)`, then a re-check of ee_wm, the
//                       queues, the kicks, the hold word and the host-command flag with seq_cst
//                       loads. Work found: back to RUNNING. None: WaitForWorkWithSpin.
//   PARKED -> RUNNING   a wake-up (WorkSema::NotifyOfWork) from the EE or a host thread, or from
//                       Stop(). The EE sends the filtered wake only when it reads iop_parked set.
//   RUNNING -> HELD     the EE stores hold_gen (relaxed), then hold_target (release). The IOP
//                       loads hold_target (acquire), runs to it, and stores held_gen = gen
//                       (release). The EE reads held_gen (acquire) and so sees everything the IOP
//                       wrote.
//   HELD -> RUNNING     the EE stores hold_target = kNoHold (release) and notifies the IOP. What
//                       the EE wrote during the hold is visible to the IOP through that pair.
//   RUNNING -> BLOCKED  CallOnEE: the IOP fills the call slot, stores its state Posted (release),
//                       queues an I2E entry (the ring's release store) and waits for state Done
//                       (acquire). PostI2E on a full queue polls.
//   BLOCKED -> RUNNING  the EE claims the slot (CAS Posted -> Running, acquire), runs the
//                       function, stores Done (release) and notifies the IOP. Or Stop() sets the
//                       shutdown flag; a call still Posted is cancelled by the IOP (CAS Posted ->
//                       Idle) and returns false, a call already Running is waited out.
//   any -> exit         Stop() stores shutdown (release), notifies, joins. A hold wait ends on shutdown.
//
// A wake-up can be missed in one narrow case, accepted by the design (section 3.9): the EE
// publishes ee_wm and then reads iop_parked relaxed, and the IOP sets iop_parked and then re-reads
// ee_wm. Neither side orders its store before its load, so both can read the old value and the IOP
// sleeps with budget pending. Every spin wakes a parked IOP on entry and every 1024 iterations,
// and the next event test wakes it too, so the cost is one wake-up latency. Making the EE's store
// and load sequentially consistent would close the race, but the load would then have to wait for
// the store's cache line to leave the IOP's core on every event test, which is the cost the design
// avoids. The wake-ups that must never be lost are not filtered: releasing a hold, finishing a
// CallOnEE, stopping, and posting a host command always notify.
//
// ---------------------------------------------------------------------------------------------
// Why the EE spinning on the IOP cannot deadlock with the IOP waiting on the EE
//
// The IOP waits on the EE in exactly three places: CallOnEE, PostI2E on a full queue, and while
// HELD. The EE waits on the IOP in three places: CheckLag, ScopedHold, PostE2I on a full ring.
//  - The EE's three loops each empty the I2E queue and run CallOnEE functions on every iteration,
//    so an IOP that is BLOCKED in CallOnEE or PostI2E is released by every EE wait.
//  - The IOP is only HELD while the EE holds. An EE that holds does not enter CheckLag (it
//    returns at once), and PostE2I returns false at once on a full ring, because the IOP cannot
//    consume. Nested ScopedHolds are counters.
//  - While a CallOnEE function runs the IOP is BLOCKED at a known point, which is a hold. A
//    ScopedHold taken inside the function is a counter, CheckLag returns at once, and PostE2I
//    returns false on a full ring, as under a hold.
//  - Stop() never waits for the EE: it sets the shutdown flag and every IOP wait checks it.
//  - A missed wake-up is recovered by the spin that follows it (above).
//  - In Lockstep the IOP never waits on the EE at all: it calls EE code directly while the EE is
//    in the handoff.
//
// A function given to CallOnEE therefore must not wait for the IOP. That is a rule on callers.

#pragma once

#include "IopThread/IopChannels.h"
#include "IopThread/IopThreadMode.h"

#include "common/FPControl.h"
#include "common/Pcsx2Defs.h"
#include "common/Threading.h"

#include <atomic>
#include <functional>
#include <thread>
#include <type_traits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace IopThread
{
	// The CPU's spin-wait hint, for the loops that wait for the other thread. It does not sleep and
	// does not give the core away; it only tells the core a loop is spinning.
	__fi inline void SpinHint() noexcept
	{
#if defined(_MSC_VER) && defined(_M_ARM64)
		__yield();
#elif defined(_MSC_VER)
		_mm_pause();
#elif defined(__aarch64__)
		asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
		__builtin_ia32_pause();
#else
		std::this_thread::yield();
#endif
	}

	/// IOP-to-EE item kinds (design section 3.5). Only CallOnEE means anything to the thread
	/// object; the EE hands every other kind to Params::handle_i2e.
	namespace I2EKind
	{
		inline constexpr u16 Sif0EeHalf = 0;
		inline constexpr u16 Sif1EeHalf = 1;
		inline constexpr u16 SbusIrq = 2;
		inline constexpr u16 CallOnEE = 3;
		inline constexpr u16 PauseRequest = 4;
	} // namespace I2EKind

	/// How many iterations a spin makes between asking Params::should_stop and waking a parked IOP.
	inline constexpr u32 kSpinCheckInterval = 1024;
	static_assert((kSpinCheckInterval & (kSpinCheckInterval - 1)) == 0);

	/// What hold_target holds when no hold is requested. No EE cycle reaches it.
	inline constexpr u64 kNoHold = ~u64(0);

	/// What a hold is for. It only changes how the EE waits.
	enum class HoldKind : u8
	{
		Lifecycle, // pause, savestate, reset, shutdown, settings: may sleep after SPIN_TIME_NS
		Emulation, // on the emulation path (EE access to IOP space, SIF pumps): spins, never sleeps
	};

	/// Where an EE-side wake-up came from. Params::drop_wake sees it, so a test can lose chosen
	/// wake-ups the way the race in design 3.9 loses them.
	enum class WakeSite : u8
	{
		Publish, // PublishWatermark
		Post, // PostE2I and Wake()
		Kick, // KickIop
		SpinEntry, // the first thing any spin does
		SpinPeriodic, // every kSpinCheckInterval iterations of a spin
	};

	/// Everything the owner decides at Start().
	struct Params
	{
		/// Lockstep or Threaded. Start() refuses Off: with no IOP thread there is no object to run.
		IopThreadMode mode = IopThreadMode::Threaded;

		/// The lag bound L, in EE cycles: CheckLag lets the EE go on once cycle - iop_pos <= lag.
		/// The caller clamps it (ClampLag); the object takes it as given so a test can use any.
		s32 lag = kDefaultLag;

		/// S_min: the IOP waits until its budget reaches this many cycles before it runs a slice,
		/// unless an item is due at the end of the slice, an IOP event is pending or the EE is
		/// spinning. 0 runs whenever there is budget (the owner's default).
		s32 min_slice = 0;

		/// Where the clocks start: the EE's cycle (the first watermark) and the IOP's position.
		u64 start_ee_cycle = 0;
		u64 start_iop_pos = 0;

		/// The IOP thread's floating-point control register (design 3.10): EmuConfig.Cpu.FPUFPCR.
		FPControlRegister fpcr = FPControlRegister::GetDefault();

		/// 0 takes the platform default. The emulator passes VMManager::EMU_THREAD_STACK_SIZE.
		u32 stack_size = 0;
		const char* name = "IOP";

		// ---- run on the IOP thread

		/// Required. Runs the IOP from `from` towards `target` and returns the cycle it reached,
		/// which is at least `from`. `target` is below `from` only when has_event asked for a slice
		/// with no budget.
		std::function<u64(u64 from, u64 target)> run_slice;

		/// Applies one item once the IOP's position has reached its stamp (Lockstep: when the EE
		/// asks). May be empty if the caller never posts items.
		std::function<void(const E2IItem&)> apply_item;

		/// Optional. True when the IOP has an event of its own pending (iopEventAction), which is
		/// work even with a zero budget.
		std::function<bool()> has_event;

		/// Optional. Receives the E2IKick bits raised since the last call.
		std::function<void(u32 kicks)> apply_kicks;

		// ---- run on the EE thread

		/// Optional. Handles every I2E item except CallOnEE. A handler that waits (a hold, a full
		/// ring) lets later items run inside it; the object does not keep drains from nesting,
		/// because refusing to drain there could deadlock against a full I2E queue.
		std::function<void(const I2EItem&)> handle_i2e;

		/// The EE's current cycle (cpuRegs.cycle). ScopedHold takes its target from it. If empty,
		/// the last published watermark is used.
		std::function<u64()> ee_cycle;

		/// Asked every kSpinCheckInterval iterations of an EE spin. True makes the spin give up:
		/// the emulator passes VMManager::GetState() being Stopping or Shutdown.
		std::function<bool()> should_stop;

		/// Test only. Asked before an EE-side wake-up that was filtered by iop_parked is sent;
		/// true loses it. Never consulted for the wake-ups that must not be lost.
		std::function<bool(WakeSite)> drop_wake;
	};

	/// A snapshot of the counters. The EE-side ones are written by the EE thread, the rest by the
	/// IOP thread; each has one writer, so reading them from anywhere is safe and approximate.
	struct Stats
	{
		// EE thread
		u64 lag_spins; // CheckLag calls that had to wait (W1)
		u64 lag_spin_iterations;
		u64 hold_requests; // holds that went through the handshake (W2/W4)
		u64 hold_spin_iterations;
		u64 hold_sleeps; // polling sleeps of a Lifecycle hold
		u64 e2i_full_spins; // PostE2I calls that found the ring full (W3)
		u64 e2i_full_iterations;
		u64 e2i_full_failfast; // full-ring posts refused because the IOP cannot make room
		u64 i2e_drained;
		u64 calls_served; // CallOnEE functions run
		u64 wakes_sent; // filtered wake-ups that reached a parked IOP
		u64 wakes_dropped; // ... and ones Params::drop_wake lost
		u64 stop_checks; // calls of Params::should_stop
		u64 handoffs; // Lockstep requests
		// IOP thread
		u64 slices;
		u64 items_applied;
		u64 parks; // times the IOP went to sleep
		u64 holds_entered;
		u64 calls_posted;
		u64 calls_cancelled;
		u64 i2e_full_waits; // PostI2E calls that found the queue full
		u64 kicks_applied;
	};

	namespace detail
	{
		/// A statistic with one writing thread: relaxed, and written as a load and a store rather
		/// than a read-modify-write, because nobody else writes it.
		class Counter
		{
		public:
			void Add(u64 n = 1) noexcept { m_value.store(m_value.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
			u64 Get() const noexcept { return m_value.load(std::memory_order_relaxed); }
			void Reset() noexcept { m_value.store(0, std::memory_order_relaxed); }

		private:
			std::atomic<u64> m_value{0};
		};
	} // namespace detail

	class ScopedHold;

	class Thread
	{
	public:
		using Ticket = HostCommandQueue::Ticket;

		Thread();
		/// Stops the thread if it is running.
		~Thread();

		Thread(const Thread&) = delete;
		Thread& operator=(const Thread&) = delete;

		// ------------------------------------------------------------ lifecycle (EE thread)

		/// Creates the IOP thread and returns once its setup is done (design 3.10: the name, ADPF,
		/// the per-thread page-fault handler, the FPCR). The thread is born parked. False if one is
		/// already running, the mode is Off or there is no run_slice.
		///
		/// If a ScopedHold is live on this thread the IOP comes up held, at start_iop_pos, and is
		/// released with that hold.
		bool Start(Params params);

		/// Stops and joins the thread. Cancels a CallOnEE that is still waiting (it returns false
		/// and its function never runs; an entry for it left in the I2E queue is ignored if drained),
		/// lets a hold the caller has live end, and leaves whatever is in the queues until the next
		/// Start(). A ScopedHold taken before Stop() may still be destroyed afterwards.
		void Stop();

		bool IsRunning() const { return m_running; }
		IopThreadMode Mode() const { return m_mode; }

		// ------------------------------------------------------------ EE side, Threaded

		/// The EE's side of an event test: tell the IOP how far the EE has got (release store of
		/// `ee_wm`), and wake it if it is parked and has budget.
		__fi void PublishWatermark(u64 cycle)
		{
			m_ee_wm_local = cycle;
			// Pairs with the acquire load of ee_wm in the IOP's loop: every item stamped at or
			// before `cycle` was pushed before this store, so the IOP sees it.
			m_ee.wm.store(cycle, std::memory_order_release);
			// Relaxed on purpose: see "A wake-up can be missed" above.
			if (m_iop.parked.load(std::memory_order_relaxed)) [[unlikely]]
				WakeAfterPublish(cycle);
		}

		/// W1. Returns once cycle - iop_pos <= lag, so the EE is never more than L ahead after an
		/// event test. Spins (see the top of this file) while it waits. False if the stop predicate
		/// ended the wait; the lag is then unmet and the caller must not carry on as if it held.
		/// Returns true at once when the IOP is held or stopped, since it cannot catch up.
		__fi bool CheckLag(u64 cycle)
		{
			// Pairs with the release store of iop_pos in the IOP's loop.
			const u64 pos = m_iop.pos_pub.load(std::memory_order_acquire);
			m_last_seen_iop_pos = pos;
			if (static_cast<s64>(cycle - pos) <= m_lag) [[likely]]
				return true;
			return LagSpin(cycle);
		}

		/// Queues an item stamped `stamp` (the EE's cycle at the moment) for the IOP, publishes the
		/// stamp as the watermark and wakes a parked IOP. W3 when the ring is full: spins until
		/// there is room. False, with the item not queued, if the stop predicate ended the wait or
		/// the IOP cannot make room (under a hold, inside a CallOnEE function, or not Threaded).
		/// In Lockstep it only queues the item; LockstepApplyUpTo() applies it.
		bool PostE2I(u64 stamp, u16 kind, u32 arg = 0);

		/// Raises SIF kick bits (E2IKick) for the IOP and wakes it if it is parked.
		void KickIop(u32 bits);

		/// The event test's check for IOP-to-EE items: two relaxed loads.
		__fi bool MayHaveI2E() const { return m_i2e.MayHaveItems(); }

		/// Empties the IOP-to-EE queue: runs a CallOnEE function itself, hands any other item to
		/// Params::handle_i2e. Returns how many items it took.
		size_t DrainI2E();

		/// Wakes the IOP if it is parked.
		void Wake();

		/// Moves both clocks and empties the queues. For a savestate load or a mode conversion
		/// (design 9.1), so only under a hold or while the thread is stopped.
		void Resync(u64 ee_cycle, u64 iop_pos);

		// ------------------------------------------------------------ EE side, Lockstep

		/// Hands the IOP thread one slice, "run towards `target`", and waits until it is done.
		/// Returns the cycle the IOP reached. The IOP thread runs it with the EE blocked here, so
		/// it may call EE code directly. Host commands run at the start of each handoff.
		u64 LockstepRunSlice(u64 target);

		/// Hands the IOP thread the order "apply every queued item stamped at or before `stamp`"
		/// whatever the IOP's position, and waits. Returns how many it applied. Lockstep applies an
		/// item at the EE's point of execution, which is when the EE posts it.
		size_t LockstepApplyUpTo(u64 stamp);

		// ------------------------------------------------------------ IOP side

		/// Runs `fn` on the EE thread and blocks until it has finished. The EE runs it from
		/// DrainI2E(), including the drains inside its spins, so the IOP is stopped at a known
		/// point and `fn` may touch both machines. False if the object was stopped first, in which
		/// case `fn` did not run and will not. In Lockstep `fn` is simply called here.
		///
		/// `fn` must not wait for the IOP.
		template <class F>
		bool CallOnEE(F&& fn)
		{
			using Fn = std::remove_reference_t<F>;
			return CallOnEEImpl([](void* p) { (*static_cast<Fn*>(p))(); }, const_cast<std::remove_const_t<Fn>*>(&fn));
		}

		/// Queues an item for the EE. Waits while the queue is full, which the EE empties at its
		/// next event test. False if the object was stopped meanwhile and the item was not queued.
		bool PostI2E(u16 kind, u32 arg = 0);

		// ------------------------------------------------------------ any thread

		/// Queues `command` to run on the IOP thread, in post order, between slices (Lockstep:
		/// during the next handoff, so it never runs while the EE does). Wakes the IOP.
		Ticket PostHostCommand(HostCommandQueue::Command command);
		bool IsHostCommandDone(Ticket ticket) const { return m_host.IsDone(ticket); }

		/// Sets the IOP thread's FPCR after a settings change, through a host command.
		void ReapplyFpcr(FPControlRegister value);

		/// The cycle the IOP has published, with acquire.
		u64 PublishedPos() const { return m_iop.pos_pub.load(std::memory_order_acquire); }
		/// The last watermark the EE published.
		u64 Watermark() const { return m_ee.wm.load(std::memory_order_acquire); }
		bool IsParked() const { return m_iop.parked.load(std::memory_order_acquire); }
		/// The hint the EE sets while it is in a spin.
		bool EeSpinning() const { return m_ee.spinning.load(std::memory_order_relaxed); }
		/// True on the IOP thread, between Start() and Stop().
		bool OnIopThread() const { return m_iop_tid == std::this_thread::get_id(); }
		/// EE thread. How many ScopedHolds are live, nested ones included.
		u32 HoldDepth() const { return m_hold_depth; }

		Stats GetStats() const;

	private:
		friend class ScopedHold;

		enum CallState : u32
		{
			kCallIdle,
			kCallPosted, // the IOP filled the slot and queued the entry
			kCallRunning, // the EE claimed it
			kCallDone,
		};

		enum LockstepKind : u32
		{
			kLsRunSlice,
			kLsApplyUpTo,
		};

		struct Plan
		{
			bool run;
			u64 target;
		};

		// ---- IOP thread: the loops
		void ThreadMain();
		void ThreadedLoop();
		void LockstepLoop();
		bool TryHold();
		void HoldWait(u32 gen);
		Plan MakePlan(u64 wm);
		bool WorkAvailable();
		void RunOneSlice(u64 target);
		void ApplyDueItems();
		void ApplyKicks();
		bool WaitForI2ESpace(u16 kind, u32 arg);
		bool CallOnEEImpl(void (*fn)(void*), void* arg);
		template <class Ready>
		void ParkUntil(Ready&& ready);

		// ---- EE thread: the waits
		bool LagSpin(u64 cycle);
		bool WaitForE2ISpace(u64 stamp, u16 kind, u32 arg);
		bool AcquireHold(HoldKind kind);
		void ReleaseHold();
		void ServeCall();
		void WakeAfterPublish(u64 cycle);
		void WakeIfParked(WakeSite site);
		bool ShouldStop();
		u64 LockstepCall(u32 kind, u64 arg);

		Params m_params;
		IopThreadMode m_mode = IopThreadMode::Off;
		s64 m_lag = 0;
		s64 m_min_slice = 0;

		Threading::Thread m_thread;
		bool m_running = false; // EE thread
		std::atomic<bool> m_started{false}; // the IOP thread's setup is done
		std::thread::id m_iop_tid; // written by the IOP thread before m_started, then read-only

		// The words the EE writes and the IOP reads. One cache line, so the IOP's per-iteration
		// loads cost one miss when the EE has written and none when it has not.
		struct alignas(kChannelLineSize) EeWords
		{
			std::atomic<u64> wm{0}; // ee_wm
			std::atomic<u64> hold_target{kNoHold};
			std::atomic<u32> hold_gen{0};
			std::atomic<bool> spinning{false}; // ee_spinning
			std::atomic<bool> shutdown{false};
			std::atomic<u64> req_seq{0}; // Lockstep: the number of the request
		} m_ee;

		// The words the IOP writes and the EE reads.
		struct alignas(kChannelLineSize) IopWords
		{
			std::atomic<u64> pos_pub{0}; // iop_pos
			std::atomic<bool> parked{false}; // iop_parked
			std::atomic<u32> held_gen{0};
			std::atomic<u64> done_seq{0}; // Lockstep: the last request finished
		} m_iop;

		// The CallOnEE request slot. The IOP fills fn and arg and moves the state to Posted
		// (release); the EE claims it with a CAS, runs it, and moves it to Done (release).
		struct alignas(kChannelLineSize) CallSlot
		{
			void (*fn)(void*) = nullptr;
			void* arg = nullptr;
			std::atomic<u32> state{kCallIdle};
		} m_call;

		StampedRing<kE2IRingCapacity> m_e2i;
		I2EQueue<kI2EQueueCapacity> m_i2e;
		KickBits m_e2i_kicks;
		HostCommandQueue m_host;
		Threading::WorkSema m_sema;

		// EE-thread private.
		u64 m_ee_wm_local = 0;
		u64 m_last_seen_iop_pos = 0;
		u32 m_hold_depth = 0;
		bool m_hold_requested = false; // hold_target is set for a hold this thread owns
		bool m_hold_pending = false; // inside AcquireHold(), waiting to be granted that request
		u32 m_hold_gen_local = 0;
		bool m_in_call = false; // inside a CallOnEE function
		u64 m_req_seq_local = 0;
		struct
		{
			u32 kind = 0;
			u64 arg = 0;
			u64 result = 0;
		} m_ls; // the Lockstep mailbox; ordered by req_seq and done_seq

		// IOP-thread private. The EE writes m_iop_pos only under a hold or while stopped.
		u64 m_iop_pos = 0;

		// Statistics.
		detail::Counter m_lag_spins, m_lag_spin_iterations, m_hold_requests, m_hold_spin_iterations, m_hold_sleeps;
		detail::Counter m_e2i_full_spins, m_e2i_full_iterations, m_e2i_full_failfast, m_i2e_drained, m_calls_served;
		detail::Counter m_wakes_sent, m_wakes_dropped, m_stop_checks, m_handoffs;
		detail::Counter m_slices, m_items_applied, m_parks, m_holds_entered, m_calls_posted, m_calls_cancelled;
		detail::Counter m_i2e_full_waits, m_kicks_applied;
	};

	/// A full stop of the IOP, for the EE thread to touch IOP state safely (design 3.8.1). Re-entrant.
	///
	/// The outermost hold publishes the EE's cycle, asks the IOP to run up to it, waits until it
	/// has, and the IOP then stays still until the hold ends. Holds taken while one is live, or
	/// inside a CallOnEE function, or in Lockstep, or with no thread running, only count: the IOP is
	/// already stopped there. Check Acquired(): it is false if the stop predicate ended the wait, in
	/// which case the hold was withdrawn and the IOP must not be touched.
	///
	/// When a hold is acquired the I2E queue has been emptied once more after the IOP stopped, so
	/// nothing the IOP did before the hold is still waiting for the EE.
	class ScopedHold
	{
	public:
		explicit ScopedHold(Thread& thread, HoldKind kind = HoldKind::Lifecycle)
			: m_thread(thread)
			, m_acquired(thread.AcquireHold(kind))
		{
		}

		~ScopedHold()
		{
			if (m_acquired)
				m_thread.ReleaseHold();
		}

		ScopedHold(const ScopedHold&) = delete;
		ScopedHold& operator=(const ScopedHold&) = delete;

		bool Acquired() const { return m_acquired; }

	private:
		Thread& m_thread;
		bool m_acquired;
	};
} // namespace IopThread
