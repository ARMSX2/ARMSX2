// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP thread object: the thread, its pacing loop, the EE's watermark (`ee_wm`) and the IOP's
// published position (`iop_pos`), holds, CallOnEE, the parked flag and wake-ups, and the Lockstep
// handoff. The VM drives it through IopThreadVm.h.
//
// The object knows nothing about the IOP. It is given two functions, and a few optional ones:
//   run_slice(from, target)  runs the IOP from EE cycle `from` towards `target` and returns the
//                            cycle it reached. It may end up to one block past `target`, as the
//                            JIT does when it checks its budget at block ends.
//   apply_item(item)         applies one EE-to-IOP item (blanking, an IRQ, a job).
// A test hands it a fake IOP; the emulator hands it iopRunSliceOnIopThread() (IopThreadVm.cpp).
//
// Who calls what
//   EE thread (the CPU thread): Start, Stop, PublishWatermark, CheckLag, PostE2I, KickIop,
//     MayHaveI2E, DrainI2E, Wake, Resync, ScopedHold, LockstepRunSlice, LockstepApplyUpTo.
//   IOP thread: CallOnEE, PostI2E, and the functions the object was given.
//   Any thread: PostHostCommand, TryPostHostCommand, IsHostCommandDone, ReapplyFpcr, and the accessors.
//
// The EE has three waits: CheckLag (the lag bound), ScopedHold (the holds) and PostE2I (a full queue).
// None of them sleeps in the emulation path. Each spins with the CPU's yield hint, empties the
// IOP-to-EE queue and runs CallOnEE functions on every iteration. The lag wait and the E2I-full wait
// ask Params::should_stop every kSpinCheckInterval iterations; a hold never gives up (see ScopedHold).
// A ScopedHold of kind Lifecycle may fall back to short sleeps after SPIN_TIME_NS, because pause,
// savestate and reset are not on the emulation path.
//
// States of the IOP thread (Threaded)
//
//   IDLE     no work, awake. The pacing loop watches the EE's words (the watermark's cache line) for
//            Params::idle_spin_ns, waiting on that line with WFE where the CPU has it. `idle` is set.
//            A publish is a plain store the IOP sees directly: the EE sends no wake-up.
//   SLEEPING no work for idle_spin_ns. `sleeping` is set and the thread blocks in the kernel
//            (WorkSema::WaitForWork). Holds, CallOnEE and Lockstep park the same way (ParkUntil),
//            spinning first on the semaphore.
//   RUNNING  the pacing loop: apply host commands, kicks and due items, run a slice to
//            min(ee_wm, next item stamp), publish iop_pos.
//   HELD     the EE asked for a hold, the IOP ran up to the hold's cycle and stopped. It touches
//            nothing until the EE releases it.
//   BLOCKED  inside CallOnEE or PostI2E, waiting for the EE. It leaves when the EE serves the call
//            or empties the queue, and when the object is stopped.
//
// Transitions, with the atomic that makes each one happen:
//   RUNNING -> IDLE     no budget. Plain stores; the IOP re-checks for work on every change of the
//                       EE's words.
//   IDLE -> RUNNING     work: a new watermark with budget, an item, a hold, a host command...
//   IDLE -> SLEEPING    idle_spin_ns without work: the sleep handshake below.
//   SLEEPING -> RUNNING a wake-up (WorkSema::NotifyOfWork) from the EE or a host thread, or from
//                       Stop(). The EE sends the filtered wake only when it reads `sleeping` set.
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
// Going to sleep: why no wake-up is lost
//
// The EE gives the IOP work by a store: the watermark (PublishWatermark, and PostE2I after its push),
// the ring push, the kick bits, ee_spinning. Then it reads `sleeping` and wakes the IOP only if it is
// set. The IOP, before it sleeps, stores `sleeping = true` and then reads all of those again. That is
// Dekker's pattern: each side stores, then loads what the other stored. It is safe only if, for each
// side, its store is ordered before its load; then at least one of the two loads sees the other's
// store, so either the EE sees `sleeping` and wakes the IOP, or the IOP's re-check sees the work and
// it does not sleep. Without that order both loads can read the old values (store buffering, which
// ARM allows), and the IOP sleeps with work pending.
//
// The order costs a full barrier on each side, and a full barrier after the EE's watermark store has
// to wait for that store to leave the core: a cache-line transfer to the IOP's core on every event
// test. So the barrier is made asymmetric (SleepBarrier):
//   - Linux and Android: the IOP calls membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED) between its
//     store and its re-check. The kernel interrupts every core running a thread of this process and
//     runs a full barrier there, so the EE thread passes a full barrier at some point P between the
//     IOP's store of `sleeping` and the IOP's re-check. If P comes before the EE's store of the work
//     in program order, the EE's later load of `sleeping` sees true. If P comes after it, the work is
//     visible before membarrier returns, and the IOP's re-check sees it. The EE needs only a compiler
//     barrier between its store and its load. (A descheduled EE thread passed a full barrier when it
//     was switched out.) This is the pattern of the membarrier(2) manual.
//   - Elsewhere, or when the kernel refuses MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, or when
//     Params::sleep_barrier says Fence: a seq_cst fence on both sides, the EE's after every store it
//     wakes for. Correct, and as slow as the line transfer.
// The barrier is chosen at Start() and fixed for the thread's life. A membarrier call that fails
// after a successful registration (it should not) makes the IOP go back to watching instead of
// sleeping, which is always safe.
//
// The IOP sleeps only after idle_spin_ns of watching with nothing to do, so membarrier runs about
// once per EE stall (a frame-limiter sleep, a GS sync, a pause), not per event test. The wake-ups
// that are not filtered by `sleeping` stay unfiltered: releasing a hold, finishing a CallOnEE,
// stopping, and posting a host command always notify.
//
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
	/// Which instruction the spin loops use between looks: yield, or isb as Threading::SpinWait does.
	/// A testing knob (eerunner --iop-spin-hint), set before the thread starts, so a relaxed load is all
	/// a spin pays for it.
	enum class SpinHintKind : u8
	{
		Yield,
		Isb,
	};
	inline std::atomic<SpinHintKind> g_spin_hint{SpinHintKind::Yield};

	// The CPU's spin-wait hint, for the loops that wait for the other thread. It does not sleep or
	// give the core away.
	__fi inline void SpinHint() noexcept
	{
#if defined(_MSC_VER) && defined(_M_ARM64)
		__yield();
#elif defined(_MSC_VER)
		_mm_pause();
#elif defined(__aarch64__)
		if (g_spin_hint.load(std::memory_order_relaxed) == SpinHintKind::Isb)
			asm volatile("isb" ::: "memory");
		else
			asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
		__builtin_ia32_pause();
#else
		std::this_thread::yield();
#endif
	}

	/// IOP-to-EE item kinds. Only CallOnEE means anything to the thread object; the EE hands every
	/// other kind to Params::handle_i2e.
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

	/// How long an idle IOP watches the watermark before it sleeps (Params::idle_spin_ns): 500 us. The EE
	/// publishes every 0.1 to 6 us while it runs, so the IOP sleeps only when the EE stops publishing: a
	/// frame-limiter sleep, a GS or VU sync, a pause. A short EE stall then ends with the IOP still watching,
	/// and the EE does not have to wake it. The watch waits in WFE, so it holds the core without spinning it.
	inline constexpr u32 kDefaultIdleSpinNs = 500'000;

	/// The barrier of the sleep handshake (see "Going to sleep" at the top of this file).
	enum class SleepBarrier : u8
	{
		Auto, // membarrier(PRIVATE_EXPEDITED) on the IOP and a compiler barrier on the EE where the kernel has it; else Fence
		Fence, // a seq_cst fence on both sides
	};

	/// What a hold is for. It only changes how the EE waits.
	enum class HoldKind : u8
	{
		Lifecycle, // pause, savestate, reset, shutdown, settings: may sleep after SPIN_TIME_NS
		Emulation, // on the emulation path (EE access to IOP space, SIF pumps): spins, never sleeps
	};

	/// Where an EE-side wake-up came from. Params::drop_wake sees it, so a test can lose chosen wake-ups.
	enum class WakeSite : u8
	{
		Publish,
		Post, // PostE2I and Wake()
		Kick,
		SpinEntry, // the first thing any spin does
		SpinPeriodic, // every kSpinCheckInterval iterations of a spin
	};

	/// Settings fixed at Start().
	struct Params
	{
		/// Lockstep or Threaded. Start() refuses Off: with no IOP thread there is no object to run.
		IopThreadMode mode = IopThreadMode::Threaded;

		/// The lag bound L, in EE cycles: CheckLag lets the EE go on once cycle - iop_pos <= lag.
		/// The caller clamps it (ClampLag); the object takes it as given so a test can use any.
		s32 lag = kDefaultLag;

		/// S_min: the IOP waits until its budget reaches this many cycles before it runs a slice,
		/// unless an item is due at the end of the slice, an IOP event is pending or the EE is
		/// spinning. 0, the default, runs whenever there is budget.
		s32 min_slice = 0;

		/// Where the clocks start: the EE's cycle (the first watermark) and the IOP's position.
		u64 start_ee_cycle = 0;
		u64 start_iop_pos = 0;

		/// The IOP thread's floating-point control register: EmuConfig.Cpu.FPUFPCR.
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

		/// Optional. Called on the EE thread when an outermost hold has been granted, with the IOP stopped
		/// at its position and the I2E queue emptied once more. The VM uses it to rewrite the EE-side books
		/// (EEsCycle, EEoCycle) from the IOP's position, which a savestate or a mode change reads.
		std::function<void()> on_hold_acquired;

		/// Asked every kSpinCheckInterval iterations of the lag wait and of a wait for room in the E2I ring,
		/// whose callers can return to the event test. True makes the spin give up: the emulator passes
		/// VMManager::GetState() being Stopping or Shutdown. A hold never asks.
		std::function<bool()> should_stop;

		/// Test only. Asked before an EE-side wake-up that was filtered by `sleeping` is sent;
		/// true loses it. Never consulted for the wake-ups that must not be lost.
		std::function<bool(WakeSite)> drop_wake;

		/// How long an idle IOP watches the EE's words before it goes to sleep in the kernel. While it
		/// watches, a publish costs the EE nothing but its store. 0 sleeps at once (a test of the sleep
		/// handshake).
		u32 idle_spin_ns = kDefaultIdleSpinNs;

		/// Which barrier pairs the EE's stores with the IOP's announcement that it is going to sleep (see
		/// "Going to sleep"). Auto takes membarrier where the kernel has it. Fence is for the tests.
		SleepBarrier sleep_barrier = SleepBarrier::Auto;
	};

	/// A snapshot of the counters. The EE-side ones are written by the EE thread, the rest by the
	/// IOP thread; each has one writer, so reading them from anywhere is safe and approximate.
	struct Stats
	{
		// EE thread
		u64 lag_spins; // CheckLag calls that had to wait
		u64 lag_spin_iterations;
		u64 lag_stall_ns; // wall time spent in lag spins
		u64 lag_spins_parked; // lag spins that found the IOP sleeping (or about to) when they began
		u64 lag_parked_stall_ns; // ... and the wall time those spins took
		u64 hold_requests; // holds that went through the handshake
		u64 emu_hold_requests; // ... of which on the emulation path: EE access to IOP space, SIF pumps, the PS1 switch
		u64 emu_hold_stall_ns; // wall time the EE spent waiting for those
		u64 sif_ee_pumps; // SIF pumps the EE started (each one took an Emulation hold)
		u64 hold_markers; // holds that were only a counter: Lockstep, nested, inside a CallOnEE function, no thread
		u64 hold_spin_iterations;
		u64 hold_sleeps; // polling sleeps of a Lifecycle hold
		u64 e2i_full_spins; // PostE2I calls that found the ring full
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
		u64 calls_posted; // CallOnEE requests: the IOP-started SIF pumps (the IOP blocks until the EE has run each)
		u64 calls_cancelled;
		u64 i2e_full_waits; // PostI2E calls that found the queue full
		u64 kicks_applied;
		u64 lag_spins_idle; // EE: lag spins that found the IOP idle and awake (watching the watermark)
		u64 lag_idle_stall_ns; // EE: ... and their wall time
		u64 idle_waits; // IOP: times the pacing loop went idle
		u64 idle_ns; // IOP: wall time idle, sleeping included
		u64 sleep_barrier_membarrier; // 1 if the sleep handshake uses membarrier, 0 if the fence
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

	// Aligned to the channel line so that whatever follows the object in memory does not share the IOP's
	// last line.
	class alignas(kChannelLineSize) Thread
	{
	public:
		using Ticket = HostCommandQueue::Ticket;

		Thread();
		~Thread();

		Thread(const Thread&) = delete;
		Thread& operator=(const Thread&) = delete;

		/// Creates the IOP thread and returns once its setup is done (the name, ADPF, the per-thread
		/// page-fault handler, the FPCR). The thread is born parked. False if one is already running,
		/// the mode is Off or there is no run_slice.
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

		/// The OS thread, for placing it (SetAffinity) and reading its CPU time. Empty before the first Start()
		/// and after Stop(). EE (CPU) thread only: Start() and Stop() are the writers.
		const Threading::ThreadHandle& GetThreadHandle() const { return m_thread; }

		/// How many times Start() has succeeded. A new thread begins with the mask of the thread that created it, so a
		/// placement made for one thread says nothing about the next; the VM compares this to know it must place again.
		u32 StartCount() const { return m_start_count; }

		/// The EE's side of an event test: tell the IOP how far the EE has got (release store of
		/// `ee_wm`), and wake it if it is parked and has budget.
		__fi void PublishWatermark(u64 cycle)
		{
			m_ee_wm_local = cycle;
			// Pairs with the acquire load of ee_wm in the IOP's loop: every item stamped at or
			// before `cycle` was pushed before this store, so the IOP sees it.
			m_ee.wm.store(cycle, std::memory_order_release);
			// The EE half of the sleep handshake ("Going to sleep"): the store above, then the load of
			// `sleeping`. `sleeping` has a cache line of its own that the IOP writes only when it goes to
			// sleep, so the load hits the EE's cache.
			EeStoreLoadBarrier();
			if (m_sleep.sleeping.load(std::memory_order_relaxed)) [[unlikely]]
				WakeAfterPublish(cycle);
		}

		/// Returns once cycle - iop_pos <= lag, so the EE is never more than L ahead after an
		/// event test. Spins (see the top of this file) while it waits. False if the stop predicate
		/// ended the wait; the lag is then unmet and the caller must not carry on as if it held.
		/// Returns true at once when the IOP is held or stopped, since it cannot catch up.
		__fi bool CheckLag(u64 cycle)
		{
			// The position only moves forward (Resync moves it only under a hold, and updates the copy), so
			// a bound that holds against the last position seen holds against the current one, and the EE
			// does not have to read the IOP's line.
			if (static_cast<s64>(cycle - m_last_seen_iop_pos) <= m_lag) [[likely]]
				return true;
			// Pairs with the release store of iop_pos in the IOP's loop: the EE goes on past the bound, and
			// trusts this position at later event tests, only after the IOP's work up to it. A relaxed load
			// plus an acquire fence, not an acquire load: the event test has just made a release store (STLR)
			// to a line the IOP is reading, and an acquire load (LDAR) may not complete before that store, so
			// it would wait for the line; the fence (DMB ISHLD) does not wait for earlier stores. A relaxed
			// load that reads a release store, then an acquire fence, still synchronizes with it.
			const u64 pos = m_iop.pos_pub.load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			m_last_seen_iop_pos = pos;
			if (static_cast<s64>(cycle - pos) <= m_lag) [[likely]]
				return true;
			return LagSpin(cycle);
		}

		/// Queues an item stamped `stamp` (the EE's cycle at the moment) for the IOP, publishes the
		/// stamp as the watermark and wakes a parked IOP. When the ring is full it spins until
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

		/// Moves both clocks and empties the queues. For a savestate load or a mode conversion,
		/// so only under a hold or while the thread is stopped.
		void Resync(u64 ee_cycle, u64 iop_pos);

		/// Hands the IOP thread one slice, "run towards `target`", and waits until it is done.
		/// Returns the cycle the IOP reached. The IOP thread runs it with the EE blocked here, so
		/// it may call EE code directly. Host commands run at the start of each handoff.
		u64 LockstepRunSlice(u64 target);

		/// Hands the IOP thread the order "apply every queued item stamped at or before `stamp`"
		/// whatever the IOP's position, and waits. Returns how many it applied. Lockstep applies an
		/// item at the EE's point of execution, which is when the EE posts it.
		size_t LockstepApplyUpTo(u64 stamp);

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

		/// Queues `command` to run on the IOP thread, in post order, between slices (Lockstep:
		/// during the next handoff, so it never runs while the EE does). Wakes the IOP.
		Ticket PostHostCommand(HostCommandQueue::Command command);
		bool IsHostCommandDone(Ticket ticket) const { return m_host.IsDone(ticket); }

		/// For a host thread that does not know whether an IOP thread exists (the physical-disc thread).
		/// Like PostHostCommand while the thread is running, and 0, with the command not
		/// queued, while it is not (Off, or between Stop() and Start()): the caller then makes the call
		/// itself, as it always did. The decision and the queueing are one step, so a Stop() that races
		/// the post either refuses it or, having accepted it, runs it on the IOP thread before the thread
		/// exits. Nothing is dropped and nothing runs twice.
		Ticket TryPostHostCommand(HostCommandQueue::Command command);

		/// Sets the IOP thread's FPCR after a settings change, through a host command.
		void ReapplyFpcr(FPControlRegister value);

		u64 PublishedPos() const { return m_iop.pos_pub.load(std::memory_order_acquire); }
		u64 Watermark() const { return m_ee.wm.load(std::memory_order_acquire); }
		/// The IOP has nothing to do: idle (watching the watermark) or asleep, in the pacing loop or in any park.
		bool IsParked() const { return m_iop.idle.load(std::memory_order_acquire) || IsSleeping(); }
		/// The IOP has announced it is going to sleep in the kernel, so a publish has to wake it.
		bool IsSleeping() const { return m_sleep.sleeping.load(std::memory_order_acquire); }
		/// The hint the EE sets while it is in a spin.
		bool EeSpinning() const { return m_ee.spinning.load(std::memory_order_relaxed); }
		/// True on the IOP thread, between Start() and Stop().
		bool OnIopThread() const { return m_iop_tid == std::this_thread::get_id(); }
		/// EE thread. How many ScopedHolds are live, nested ones included.
		u32 HoldDepth() const { return m_hold_depth; }
		/// EE thread. True while the EE runs a CallOnEE function: the IOP is blocked at a known point, as under a hold.
		bool InCallOnEE() const { return m_in_call; }
		/// EE thread. True when nothing runs the IOP's state beside the caller: no thread, a hold, or a CallOnEE
		/// function. EE code that would touch IOP state outside a hold may do it then.
		bool IopIsStopped() const { return !m_running || m_hold_depth > 0 || m_in_call; }

		/// EE thread. Changes the lag bound L. The IOP thread never reads it.
		void SetLag(s32 lag)
		{
			m_lag = lag;
		}
		s32 Lag() const { return static_cast<s32>(m_lag); }

		/// EE thread. Counts a SIF pump the EE started (it holds the IOP for the pump).
		void CountSifPump() { m_sif_ee_pumps.Add(); }

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
		void AcquireHold(HoldKind kind);
		void ReleaseHold();
		void ServeCall();
		void WakeAfterPublish(u64 cycle);
		void WakeIfParked(WakeSite site);
		/// The EE's half of the sleep handshake's barrier: a compiler barrier with membarrier, a full fence without.
		__fi void EeStoreLoadBarrier()
		{
			if (m_ee_fence) [[unlikely]]
				std::atomic_thread_fence(std::memory_order_seq_cst);
			else
				std::atomic_signal_fence(std::memory_order_seq_cst);
		}
		// IOP thread: the idle wait of the pacing loop, and the IOP's half of the barrier.
		void IdleWait();
		bool IopStoreLoadBarrier();
		bool ShouldStop();
		u64 LockstepCall(u32 kind, u64 arg);

		// Read by both threads, written only while the thread is stopped (m_lag: by the EE, rarely).
		Params m_params;
		IopThreadMode m_mode = IopThreadMode::Off;
		s64 m_lag = 0;
		s64 m_min_slice = 0;
		u64 m_idle_spin_ns = 0;
		bool m_use_membarrier = false; // the IOP's half of the sleep handshake

		Threading::Thread m_thread;
		bool m_running = false; // EE thread
		u32 m_start_count = 0; // EE thread
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
			std::atomic<bool> idle{false}; // no work: watching or sleeping (statistics and tests)
			std::atomic<u32> held_gen{0};
			std::atomic<u64> done_seq{0}; // Lockstep: the last request finished
		} m_iop;

		// The IOP's announcement that it is going to sleep (`sleeping`). A line of
		// its own, written only when the IOP sleeps and wakes, so the EE's load of it on every event test
		// stays in the EE's cache.
		struct alignas(kChannelLineSize) SleepWord
		{
			std::atomic<bool> sleeping{false};
		} m_sleep;

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

		// EE-thread private, with the EE's counters: lines the IOP never writes, so the event test's loads
		// and stores of them stay in the EE's cache.
		alignas(kChannelLineSize) u64 m_ee_wm_local = 0;
		bool m_ee_fence = false; // the EE's half of the sleep handshake is a fence (no membarrier)
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

		detail::Counter m_lag_spins, m_lag_spin_iterations, m_hold_requests, m_hold_markers, m_hold_spin_iterations, m_hold_sleeps;
		detail::Counter m_lag_stall_ns, m_lag_spins_parked, m_lag_parked_stall_ns, m_emu_hold_requests, m_emu_hold_stall_ns, m_sif_ee_pumps;
		detail::Counter m_e2i_full_spins, m_e2i_full_iterations, m_e2i_full_failfast, m_i2e_drained, m_calls_served;
		detail::Counter m_wakes_sent, m_wakes_dropped, m_stop_checks, m_handoffs, m_lag_spins_idle, m_lag_idle_stall_ns;

		// IOP-thread private, with the IOP's counters, on lines of their own. The EE writes m_iop_pos only
		// under a hold or while stopped.
		alignas(kChannelLineSize) u64 m_iop_pos = 0;
		detail::Counter m_slices, m_items_applied, m_parks, m_holds_entered, m_calls_posted, m_calls_cancelled;
		detail::Counter m_i2e_full_waits, m_kicks_applied, m_idle_waits, m_idle_ns;
	};

	/// A full stop of the IOP, for the EE thread to touch IOP state safely. Re-entrant.
	///
	/// The outermost hold publishes the EE's cycle, asks the IOP to run up to it, waits until it
	/// has, and the IOP then stays still until the hold ends. Holds taken while one is live, or
	/// inside a CallOnEE function, or in Lockstep, or with no thread running, only count: the IOP is
	/// already stopped there. A hold always ends with the IOP stopped: it does not give up when the VM is
	/// stopping (Params::should_stop), because what its caller does next needs the IOP stopped, and the IOP
	/// gets there within a slice.
	///
	/// When a hold is acquired the I2E queue has been emptied once more after the IOP stopped, so
	/// nothing the IOP did before the hold is still waiting for the EE.
	class ScopedHold
	{
	public:
		explicit ScopedHold(Thread& thread, HoldKind kind = HoldKind::Lifecycle)
			: m_thread(thread)
		{
			thread.AcquireHold(kind);
		}

		~ScopedHold() { m_thread.ReleaseHold(); }

		ScopedHold(const ScopedHold&) = delete;
		ScopedHold& operator=(const ScopedHold&) = delete;

	private:
		Thread& m_thread;
	};
} // namespace IopThread
