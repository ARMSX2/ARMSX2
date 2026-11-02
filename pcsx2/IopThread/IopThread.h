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
//  - A deferred SIF pump (a SifPump item, IopThreadVm.h) is one more CallOnEE, made from ApplyDueItems on
//    the IOP's own turn: from the pacing loop, or from the catch-up of a hold, which the EE spends in the
//    hold's spin, draining. The EE posts the item and goes on; it never waits for the IOP to apply it. A
//    hold that the EE takes while such an item is queued has its catch-up apply the item first (the stamp
//    is at or before the hold's cycle), so every item is gone by the time the hold is granted.
//
// A function given to CallOnEE therefore must not wait for the IOP. That is a rule on callers.

#pragma once

#include "IopThread/IopChannels.h"
#include "IopThread/IopLagControl.h"
#include "IopThread/IopLagRules.h"
#include "IopThread/IopThreadMode.h"

#include "common/FPControl.h"
#include "common/Pcsx2Defs.h"
#include "common/Threading.h"

#include <algorithm>
#include <atomic>
#include <deque>
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

		/// The rules that tighten the bound while the EE waits for the IOP (IopLagRules.h). With mask 0, or a lag at or below
		/// the tight bound, the bound is the lag, as it was before the rules existed.
		LagRulesConfig rules;

		/// S_min: the IOP waits until its budget reaches this many cycles before it runs a slice,
		/// unless an item is due at the end of the slice, an IOP event is pending or the EE is
		/// spinning. 0, the default, runs whenever there is budget.
		s32 min_slice = 0;

		/// Lockstep only, and for tests: the IOP is kept this many EE cycles behind the EE at every handoff instead of
		/// being brought up to it, and E2I items are applied when the IOP's own clock reaches their stamp instead of at
		/// the moment they are posted. 0, the default, is Lockstep as it has always been. The caller clamps it to at most
		/// kMaxLag. Items the E2I ring cannot hold go to a spill queue (m_e2i_spill). Ignored in Threaded.
		///
		/// A handoff then runs `run_slice` towards min(EE cycle - skew, next item's stamp), item by item, so the budget
		/// has to come from the target and not from the EE's books (the Threaded slice, not the Lockstep one); the
		/// caller writes the books from the position the handoff returns.
		s64 lockstep_skew = 0;

		/// Where the clocks start: the EE's cycle (the first watermark) and the IOP's position.
		u64 start_ee_cycle = 0;
		u64 start_iop_pos = 0;
		/// The frame count at Start(): LagRule::Boot counts its frames from here.
		u32 start_frame = 0;

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

		/// Optional. The IOP's position in EE cycles right now, from inside a slice: how far `run_slice` has got, to the
		/// precision of a block. PostI2E stamps its items with it (I2EItem::stamp). Empty takes the position at the start
		/// of the slice, which is as exact as an IOP with no clock inside its slice can be.
		std::function<u64()> iop_now;

		/// Optional. Receives the E2IKick bits raised since the last call.
		std::function<void(u32 kicks)> apply_kicks;

		/// Optional. The IOP's devices as StreamBit bits (SPU2 auto-DMA, a CDVD read), sampled at the end of every slice and
		/// published to the EE for LagRule::Stream. Empty means no stream is ever running.
		std::function<u32()> sample_streams;

		// ---- run on the EE thread

		/// Optional. Handles every I2E item except CallOnEE. A handler that waits (a hold, a full
		/// ring) lets later items run inside it; the object does not keep drains from nesting,
		/// because refusing to drain there could deadlock against a full I2E queue.
		std::function<void(const I2EItem&)> handle_i2e;

		/// The EE's current cycle (cpuRegs.cycle). ScopedHold takes its target from it. If empty,
		/// the last published watermark is used.
		std::function<u64()> ee_cycle;

		/// Optional. EE thread. True while the EE is inside an event test: a reply taken then can be an overshoot in idle
		/// (ReplyMonitor::OnReply). Empty counts every reply as taken inside one.
		std::function<bool()> ee_in_event_test;

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
		u64 sif_deferred; // SIF pumps the EE started and did not hold for: posted as a SifPump item (iopThreadDeferSif, a Lockstep skew)
		u64 near_waits; // SBUS mailbox accesses that had to wait for the near bound (lag above the default)
		u64 near_wait_iterations;
		u64 near_wait_ns; // wall time spent in those waits
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
		u64 ee_event_tests; // event tests the EE made with this thread running (NoteEventTest)
		u64 ee_ff_wait_loop; // ... of which were reached through a recompiler wait loop fast-forward
		u64 ee_ff_timeout_loop; // ... through a recompiler timeout loop skip
		u64 ee_ff_interpreter; // ... through the interpreter's wait loop skip
		u64 replies; // what the IOP produced for the EE and the EE took (the SBUS interrupt, the SIF pumps the IOP started)
		u64 late_replies; // ... late by at least kOvershootMin EE cycles
		u64 idle_overshoots; // ... of which the EE took in an idle event test, idle since the reply was produced (ReplyMonitor)
		u64 idle_overshoot_cycles; // their lateness, summed
		u64 idle_overshoot_max; // and the largest
		u64 late_ee_busy; // late replies taken while the EE was not idle
		u64 late_idle_light; // late replies taken in an idle event test with less than kIdleShareMin percent of the lateness idle
		u64 idle_overshoots_unbroken; // idle overshoots read strictly: no EE run between the production and now
		u64 idle_overshoots_90; // ... and read with a 90 percent idle share
		// IOP thread
		u64 slices;
		u64 items_applied;
		u64 parks; // times the IOP went to sleep
		u64 holds_entered;
		u64 calls_posted; // CallOnEE requests: the IOP-started SIF pumps (the IOP blocks until the EE has run each)
		u64 calls_cancelled;
		u64 i2e_full_waits; // PostI2E calls that found the queue full
		u64 kicks_applied;
		u64 sif_deferred_applied; // SifPump items the IOP applied (each is one CallOnEE: the IOP waits for the EE to run the pump)
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

		/// EE thread, first thing in every event test of a Lockstep or Threaded EE (after the stopping check). `ff_kind` is the
		/// FastForward bits (IopThreadMode.h) the skip that ended in this event test left, 0 when the EE got here by running. An event test
		/// reached by a fast-forward is one the EE spent idle: the guest was waiting and its clock was jumped to the next event.
		__fi void NoteEventTest(u64 cycle, u8 ff_kind, u32 frame)
		{
			m_ee_event_tests.Add();
			if (ff_kind != 0) [[unlikely]]
			{
				if (ff_kind & FastForward::WaitLoop)
					m_ee_ff_wait_loop.Add();
				if (ff_kind & FastForward::TimeoutLoop)
					m_ee_ff_timeout_loop.Add();
				if (ff_kind & FastForward::Interpreter)
					m_ee_ff_interpreter.Add();
			}
			m_replies.OnEventTest(cycle, ff_kind != 0, frame);
			m_ev_idle = ff_kind != 0;
			m_ev_frame = frame;
			// Lockstep with a skew: the IOP is kept `skew_cur` behind at this event test's handoff. Threaded asks the rules at the
			// lag check instead, after the event test has drained what the IOP queued.
			if (m_lockstep_skew > 0) [[unlikely]]
			{
				m_lockstep_skew_cur = m_lockstep_skew;
				if (m_rules.Live(m_lockstep_skew))
					m_lockstep_skew_cur = std::min<s64>(m_lockstep_skew, m_rules.Bound(cycle, m_ev_idle, m_streams.load(std::memory_order_relaxed), frame, m_lockstep_skew));
			}
		}

		/// EE thread, or the IOP thread in Lockstep with the EE blocked in the handoff. The EE takes something the IOP produced
		/// (ReplyKind) at the IOP time whose low 32 bits are `stamp_lo`, at the EE's cycle now (Params::ee_cycle). Threaded counts
		/// its I2E items and CallOnEE requests itself; the VM calls this for the effects a Lockstep IOP has on the EE directly.
		ReplyMonitor::Verdict NoteReply(u32 kind, u32 stamp_lo)
		{
			const u64 now = m_params.ee_cycle ? m_params.ee_cycle() : m_ee_wm_local;
			const bool in_event_test = m_params.ee_in_event_test ? m_params.ee_in_event_test() : true;
			return m_replies.OnReply(kind, stamp_lo, now, in_event_test);
		}

		/// The late-reply books. Read them when the EE (and in Lockstep the IOP) is not running, or accept a snapshot a moment old.
		const ReplyMonitor& Replies() const { return m_replies; }
		/// Keep a record of every frame (before Start(), which resets the books and keeps this).
		void SetReplyTrace(bool on) { m_replies.EnableTrace(on); }

		/// Returns once cycle - iop_pos <= lag, so the EE is never more than L ahead after an
		/// event test. Spins (see the top of this file) while it waits. False if the stop predicate
		/// ended the wait; the lag is then unmet and the caller must not carry on as if it held.
		/// Returns true at once when the IOP is held or stopped, since it cannot catch up.
		__fi bool CheckLag(u64 cycle) { return CheckLagTo(cycle, LagBound(cycle), false); }

		/// The bound in force at an event test at `cycle`: L, or the tight bound while a rule holds (IopLagRules.h).
		__fi s64 LagBound(u64 cycle)
		{
			if (!m_rules.Live(m_lag)) [[likely]]
				return m_lag;
			return m_rules.Bound(cycle, m_ev_idle, m_streams.load(std::memory_order_relaxed), m_ev_frame, m_lag);
		}

		/// CheckLag with the bound given. The fast path is the same: one compare against the last position seen.
		/// `near_bound` says the wait is not the lag bound (the SBUS near bound, CheckNearLag), which keeps its
		/// time out of the lag statistics.
		__fi bool CheckLagTo(u64 cycle, s64 bound, bool near_bound)
		{
			// The position only moves forward (Resync moves it only under a hold, and updates the copy), so
			// a bound that holds against the last position seen holds against the current one, and the EE
			// does not have to read the IOP's line.
			if (static_cast<s64>(cycle - m_last_seen_iop_pos) <= bound) [[likely]]
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
			if (static_cast<s64>(cycle - pos) <= bound) [[likely]]
				return true;
			return LagSpin(cycle, bound, near_bound);
		}

		/// The near bound for the EE's accesses to the SBUS mailbox words (F200..F260). Those words are live
		/// shared memory, not queued items, so with a lag bound far above the default the IOP could read an EE
		/// write up to L cycles early in its own time, and the EE could read an IOP write that late. When L is above
		/// kDefaultLag this waits until the IOP is within kDefaultLag of `cycle`, which is the skew the mailbox
		/// handshakes of an IOP reboot were tested at. At or below kDefaultLag the event test's bound already
		/// holds and this returns at once. False if the stop predicate ended the wait.
		__fi bool CheckNearLag(u64 cycle)
		{
			if (m_lag <= kDefaultLag) [[likely]]
				return true;
			return CheckLagTo(cycle, kDefaultLag, true);
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

		/// The skew this Lockstep thread was started with (Params::lockstep_skew), 0 in Threaded and with no thread. With
		/// a skew above 0, LockstepRunSlice(target) runs the IOP to target - skew, applying the items that fall due on the
		/// way, and LockstepApplyUpTo() must not be used. EE thread.
		s64 LockstepSkew() const { return m_lockstep_skew; }
		/// The skew in force at the current event test: LockstepSkew(), or less while a lag rule holds the IOP closer. What the EE's
		/// books and the handoff use.
		s64 LockstepSkewCurrent() const { return m_lockstep_skew_cur; }

		/// Lockstep skew only, testing. A hold: runs the IOP up to `ee_cycle` itself, whatever the skew (applying the items due on the
		/// way, as the skewed slice does), and returns the cycle it reached, at or a little past `ee_cycle`. The next handoff's goal is
		/// `ee_cycle - skew` again, so the IOP then waits for the EE to run a skew ahead of it: the skew re-grows as the EE runs on.
		/// EE thread, Lockstep thread running.
		u64 LockstepCatchUp(u64 ee_cycle);

		/// How many catch-ups were made, and the EE cycles the IOP ran in them (what the skew had grown to, summed).
		u64 LockstepCatchUps() const { return m_catchups; }
		u64 LockstepCatchUpCycles() const { return m_catchup_cycles; }

		/// Lockstep skew only: how many items went to the spill queue instead of the ring this run, and its deepest point.
		u64 E2ISpillTotal() const { return m_e2i_spill_total; }
		u64 E2ISpillMax() const { return m_e2i_spill_max; }

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
		/// IOP thread. The IOP's time in EE cycles at this moment (Params::iop_now, else the position at the start of the slice).
		/// What an item it queues is stamped with.
		u64 IopNow() const { return m_params.iop_now ? m_params.iop_now() : m_iop_pos; }
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

		// ---- The lag rules (IopLagRules.h). EE thread, except as noted.
		LagRules& Rules() { return m_rules; }
		const LagRules& Rules() const { return m_rules; }
		/// The EE started a SIF transfer on `channel` (0 or 1).
		void NoteEeSifStart(u64 cycle, u32 channel) { m_rules.OnEeSifStart(cycle, channel); }
		/// A SIF0 transfer from the IOP reached the EE (on the EE thread; in Lockstep on the IOP thread with the EE blocked).
		void NoteIopSif0Reply(u64 cycle) { m_rules.OnIopReply(cycle); }
		/// The EE touched an SBUS mailbox word.
		void NoteEeMailboxAccess(u64 cycle) { m_rules.OnMailboxAccess(cycle); }
		/// The EE read the status of a SIF DMA channel that is running.
		void NoteEePoll(u64 cycle) { m_rules.OnEePoll(cycle); }
		/// What the IOP's devices are doing now (StreamBit bits); written by the IOP thread, read by the EE for LagRule::Stream.
		u32 Streams() const { return m_streams.load(std::memory_order_relaxed); }
		s32 Lag() const { return static_cast<s32>(m_lag); }

		/// EE thread. Counts a SIF pump the EE started (it holds the IOP for the pump).
		void CountSifPump() { m_sif_ee_pumps.Add(); }
		/// EE thread. Counts a SIF pump the EE started and posted as an item instead of holding the IOP for it.
		void CountSifDeferred() { m_sif_deferred.Add(); }
		/// IOP thread. Counts a deferred pump item the IOP applied.
		void CountSifDeferredApplied() { m_sif_deferred_applied.Add(); }

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
			kLsCatchUp, // Lockstep skew, testing: run the IOP to the EE's cycle itself, as a Threaded hold does
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
		u64 LockstepSkewedSlice(u64 ee_cycle, s64 skew);
		void SampleStreams();
		bool PopDueE2I(u64 now, E2IItem& out);
		u64 NextE2IStamp();
		bool WaitForI2ESpace(u16 kind, u32 arg, u32 stamp);
		bool CallOnEEImpl(void (*fn)(void*), void* arg);
		template <class Ready>
		void ParkUntil(Ready&& ready);

		// ---- EE thread: the waits
		bool LagSpin(u64 cycle, s64 bound, bool near_bound);
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
		s64 m_lockstep_skew = 0; // Lockstep only; written by Start() and Stop()
		s64 m_lockstep_skew_cur = 0; // Lockstep only; the skew in force, set by the EE at each event test (the handoff reads it)
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
			u32 stamp = 0; // the IOP's time of the request (the low 32 bits of IopNow()), written with fn and arg
			std::atomic<u32> state{kCallIdle};
		} m_call;

		ReplyMonitor m_replies; // EE thread; in Lockstep also the IOP thread, with the EE blocked in the handoff
		LagRules m_rules; // EE thread; in Lockstep also the IOP thread (a SIF0 reply), with the EE blocked in the handoff
		bool m_ev_idle = false; // EE thread: the last event test was reached by a fast-forward
		u32 m_ev_frame = 0; // EE thread: its frame count
		std::atomic<u32> m_streams{0}; // written by the IOP thread at the end of a slice (StreamBit bits), read by the EE

		StampedRing<kE2IRingCapacity> m_e2i;
		// Lockstep skew only (a testing mode): the EE-to-IOP items that did not fit the ring, oldest first. A skewed IOP does not
		// run until the EE is a skew ahead, so a burst of posts (a state whose counters have to catch up, a guest loop that starts
		// a SIF DMA every few hundred cycles while the deferred pump is pending) can exceed any fixed capacity. Items go here once
		// the ring is full and until this is empty again, so every item in it is newer than every item in the ring and the order
		// holds. The EE appends while the IOP is parked in the handoff wait and the IOP consumes while the EE is blocked in the
		// handoff, so the two never touch it at once. Never used with a skew of 0 or in Threaded.
		std::deque<E2IItem> m_e2i_spill;
		u64 m_catchups = 0; // LockstepCatchUp calls
		u64 m_catchup_cycles = 0; // EE cycles the IOP ran in them
		u64 m_e2i_spill_total = 0; // items that went to the spill
		u64 m_e2i_spill_max = 0; // its deepest point
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
		detail::Counter m_sif_deferred, m_near_waits, m_near_wait_iterations, m_near_wait_ns;
		detail::Counter m_ee_event_tests, m_ee_ff_wait_loop, m_ee_ff_timeout_loop, m_ee_ff_interpreter;

		// IOP-thread private, with the IOP's counters, on lines of their own. The EE writes m_iop_pos only
		// under a hold or while stopped.
		alignas(kChannelLineSize) u64 m_iop_pos = 0;
		detail::Counter m_slices, m_items_applied, m_parks, m_holds_entered, m_calls_posted, m_calls_cancelled;
		detail::Counter m_i2e_full_waits, m_kicks_applied, m_idle_waits, m_idle_ns, m_sif_deferred_applied;
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
