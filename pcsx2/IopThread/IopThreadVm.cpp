// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopThreadVm.h"

#include "CDVD/CDVD.h"
#include "Common.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/Breakpoints.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "Hw.h"
#include "IopCounters.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "IopThread/IopPlacement.h"
#include "IopThread/OwnerAsserts.h"
#include "IopThread/SharedWord.h"
#include "Patch.h"
#include "PerformanceMetrics.h"
#include "R3000A.h"
#include "R5900.h"
#include "SIO/Sio.h"
#include "SPU2/defs.h"
#include "Sif.h"
#include "VMManager.h"

#include "common/ARCADE.h"
#include "common/Assertions.h"
#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace IopThread
{
	Thread g_thread;

	bool g_start_hold_pending = false;

	std::atomic<bool> g_pause_requested{false};

	namespace
	{
		// Minimum Threaded slice size (testing knob); 0 runs a slice whenever there is budget.
		s32 s_min_slice = 0;

		// The testing knob for Lockstep's skew (Params::lockstep_skew).
		s64 s_lockstep_skew = 0;

		// The testing knob that turns a skewed Lockstep's deferred SIF pumps into held ones (SetLockstepSkewHolds): bit 0 SIF0, bit 1 SIF1.
		u32 s_lockstep_skew_holds = 0;

		// The testing knob that keeps a record of every frame's late replies (SetReplyTrace).
		bool s_reply_trace = false;

		// The testing knob for the idle-watch window (Params::idle_spin_ns).
		u32 s_idle_spin_ns = kDefaultIdleSpinNs;

		// Held for as long as the VM stays paused.
		std::optional<ScopedHold> s_pause_hold;

		// The hold that keeps a Threaded thread started inside an event test stopped until the next one (g_start_hold_pending).
		std::optional<ScopedHold> s_start_hold;

		// The table the IOP thread applies E2I items with. Null is the real one (tests swap it).
		const E2IActions* s_e2i_actions = nullptr;

		// The IOP's position in EE cycles: where the EE is, less the IOP time still owed.
		// EEsCycle is negative when the IOP has run past the EE.
		u64 IopPosition()
		{
			return cpuRegs.cycle - static_cast<s64>(EEsCycle);
		}

		// Where the IOP is inside the slice it is running, for stamping what it queues for the EE (Params::iop_now). The slice
		// functions below write it around ExecuteBlock; the IOP thread reads it from inside, so it has one thread.
		// `ExecuteBlock` leaves psxRegs.iopCycleEE at what is still owed, so the IOP has run `budget - iopCycleEE` EE cycles
		// since `from`, to the precision of a block (the recompiler updates it at block ends).
		struct SliceClock
		{
			u64 from = 0;
			s32 budget = 0;
			u64 end = 0; // where the slice ended, for what the IOP's event test does after ExecuteBlock
			bool live = false; // inside ExecuteBlock
		};
		SliceClock s_slice;

		u64 IopTimeNow()
		{
			if (!s_slice.live)
				return s_slice.end;
			const s64 ran = static_cast<s64>(s_slice.budget) - static_cast<s64>(psxRegs.iopCycleEE);
			return s_slice.from + static_cast<u64>(std::max<s64>(ran, 0));
		}

		// The Lockstep slice: the same code Off runs on the EE thread, iopRunSliceOnIopThread(). This only keeps
		// the thread object's view of the IOP's position in step with the books; a reset or a state load moves
		// the books itself and calls ResyncClocks().
		u64 RunIopSlice(u64 from, u64 /*target*/)
		{
			// iopRunSliceOnIopThread runs ExecuteBlock(EEsCycle) when the IOP has an event, and nothing else that moves the clock. The
			// books are what it runs from, so the origin is where they say the IOP is.
			const u64 origin = IopPosition();
			s_slice = {origin, EEsCycle, origin, iopEventAction};
			iopRunSliceOnIopThread();
			s_slice.live = false;
			const u64 reached = std::max(from, IopPosition());
			s_slice.end = reached;
			return reached;
		}

		// The Threaded slice. The budget comes from the thread's watermark instead of EEsCycle, and the EE's
		// books (EEsCycle, EEoCycle) are not touched. A slice gets at most half the s32 range, which a hold's
		// catch-up after a very long stop could otherwise exceed.
		u64 RunIopSliceThreaded(u64 from, u64 target)
		{
			IOP_SIDE_ASSERT();

			const s64 owed = static_cast<s64>(target - from);
			const s32 budget = static_cast<s32>(std::clamp<s64>(owed, INT32_MIN / 2, INT32_MAX / 2));
			s32 remaining = budget;
			s_slice = {from, budget, from, false};

			if (budget > 0)
				iopEventAction = true;
			if (iopEventAction)
			{
				s_slice.live = true;
				remaining = psxCpu->ExecuteBlock(budget);
				s_slice.live = false;
				s_slice.end = from + static_cast<u64>(std::max<s64>(static_cast<s64>(budget) - static_cast<s64>(remaining), 0));
				iopEventAction = false;
			}
			iopEventTest();

			// The IOP ran `budget - remaining` cycles: past the target by the block it overshot with when
			// `remaining` is negative. It never moves back.
			const s64 ran = static_cast<s64>(budget) - static_cast<s64>(remaining);
			return from + static_cast<u64>(std::max<s64>(ran, 0));
		}

		// What the EE does with an item the IOP queued. A CallOnEE is run by the thread object.
		void HandleI2EItem(const I2EItem& item)
		{
			switch (item.kind)
			{
				case I2EKind::SbusIrq:
					// INTC_STAT is written on the EE thread only: the EE JIT loads it inline when the IntcStat
					// speedhack is off.
					hwIntcIrq(INTC_SBUS);
					break;
				case I2EKind::PauseRequest:
					// SetPaused takes a hold, which must not happen inside a hold's own spin, so the event test
					// makes the pause (DrainEeQueue).
					g_pause_requested.store(true, std::memory_order_release);
					break;
				default:
					pxFailRel("unknown IOP-to-EE item kind");
					break;
			}
		}

		// The EE-side books from where the IOP is: it is owed what the EE has run beyond its published position.
		// Valid only while the IOP is held or stopped.
		void BooksFromThread()
		{
			EEsCycle = static_cast<s32>(cpuRegs.cycle - g_thread.PublishedPos());
			EEoCycle = cpuRegs.cycle;
		}

		// LagRule::Window in the built-in set: how long the bound stays tight after the EE starts a SIF transfer, in EE cycles.
		constexpr u32 kRequestWindow = 196608;

		// The rules that tighten a large lag bound (IopLagRules.h): the built-in set.
		LagRulesConfig DefaultRules()
		{
			// Three rules are on. IdleOutstanding: while the EE is idle with a request out to the IOP, it may not run ahead of the IOP by more
			// than the tight bound (Burnout 3). Stream: while SPU2 auto-DMA is running, the same, because the EE refills the audio ring from
			// an IOP callback and a late callback makes the SPU2 replay the ring (the Devil May Cry and Metal Gear Solid 3 intros).
			// Window: for kRequestWindow cycles after every SIF transfer the EE starts, the same. God of War II's movie player sends a
			// request and spins on its completion with a system call in the loop, so the EE is never idle and IdleOutstanding does not hold;
			// at a one-frame bound each request then took a frame, the movie drew 6 to 16 fps and its sound buffer was played twice. The
			// window is not closed by a reply: closed, it was tight for 3 percent of that movie's cycles and changed nothing. Open for
			// 196,608 cycles it restores the movie (98,304 does not) and is tight for 2.4 percent of God of War II's gameplay.
			// The other rules in IopLagRules.h are off: idling alone (IdleAny) held the bound tight for 38 to 60 percent of God of War II,
			// and a CDVD read counted as a stream for 59 percent of it.
			LagRulesConfig cfg;
			cfg.mask = LagRule::Bit(LagRule::IdleOutstanding) | LagRule::Bit(LagRule::Stream) | LagRule::Bit(LagRule::Window);
			cfg.window = kRequestWindow;
			cfg.window_close_on_reply = false;
			return cfg;
		}

		// What the IOP's devices are doing, sampled by the IOP thread at the end of each slice (LagRule::Stream). The SPU2 auto-DMA
		// control word has the core's bit set while the game streams audio into that core, and the drive is busy while a read or a
		// seek is running; both are IOP-owned state the IOP thread reads without a lock.
		u32 SampleDeviceStreams()
		{
			u32 bits = 0;
			if (Cores[0].AutoDMACtrl & 1)
				bits |= StreamBit::SpuAdmaCore0;
			if (Cores[1].AutoDMACtrl & 2)
				bits |= StreamBit::SpuAdmaCore1;
			if (cdvd.Reading || cdvd.Action != 0) // 0 is cdvdAction_None (CDVD_internal.h, which defines a variable and cannot be included twice)
				bits |= StreamBit::CdvdBusy;
			return bits;
		}

		Params MakeParams()
		{
			Params params;
			params.mode = s_mode;
			params.lag = ClampLag(EmuConfig.Speedhacks.iopThreadLag);
			params.rules = DefaultRules();
			params.start_frame = g_FrameCount;
			if (params.rules.mask & LagRule::Bit(LagRule::Stream))
				params.sample_streams = SampleDeviceStreams;
			params.min_slice = s_mode == IopThreadMode::Threaded ? s_min_slice : 0;
			params.idle_spin_ns = s_idle_spin_ns;
			params.start_ee_cycle = cpuRegs.cycle;
			params.start_iop_pos = IopPosition();
			params.iop_now = IopTimeNow;
			params.ee_in_event_test = [] { return eeEventTestIsActive; };
			params.fpcr = EmuConfig.Cpu.FPUFPCR;
			params.stack_size = VMManager::EMU_THREAD_STACK_SIZE;
			if (s_mode == IopThreadMode::Threaded)
			{
				params.run_slice = RunIopSliceThreaded;
				params.has_event = [] { return iopEventAction; };
				params.handle_i2e = HandleI2EItem;
				params.on_hold_acquired = BooksFromThread;
			}
			else
			{
				params.run_slice = RunIopSlice;
				if (s_lockstep_skew > 0)
				{
					// The skewed handoff runs to a target, not to what the EE's books say it owes, so it uses the
					// Threaded slice (IOP-side only, EEsCycle untouched). RunLockstepSlice writes the books from the
					// position the handoff returns.
					params.lockstep_skew = s_lockstep_skew;
					params.run_slice = RunIopSliceThreaded;
					params.has_event = [] { return iopEventAction; };
				}
			}
			params.apply_item = [](const E2IItem& item) {
				if (item.kind == E2IKind::SifPump)
					g_thread.CountSifDeferredApplied();
				ApplyE2IItem(item, s_e2i_actions ? *s_e2i_actions : kRealE2IActions);
			};
			params.ee_cycle = [] { return static_cast<u64>(cpuRegs.cycle); };
			params.should_stop = [] {
				const VMState state = VMManager::GetState();
				return state == VMState::Stopping || state == VMState::Shutdown;
			};
			return params;
		}

		void SetMode(IopThreadMode mode)
		{
			s_mode = mode;
			g_intEventTestShared = cpuEventTestFor(mode);
		}

		// Stops the thread. A Threaded thread leaves the IOP where it was, which the EE-side books (a savestate's
		// and Off's) must say: the caller holds the IOP, so the position cannot move between the two lines.
		void StopRunningThread()
		{
			if (g_thread.Mode() == IopThreadMode::Threaded)
				BooksFromThread();
			// The metrics stop reading the handle before the thread is joined, since a joined thread has none.
			PerformanceMetrics::SetIOPThread({});
			g_thread.Stop();
		}
	} // namespace

	HostCpuInfo CurrentHostInfo()
	{
#if defined(ARCH_ARM64)
		constexpr bool is_arm64 = true;
#else
		constexpr bool is_arm64 = false;
#endif
		return MakeHostInfo(is_arm64, std::thread::hardware_concurrency(), VMManager::Internal::GetPerformanceCoreMask());
	}

	VmConditions CurrentVmConditions()
	{
		VmConditions vm;
		// PS1 mode is only meaningful once the VM has reset the IOP's registers: before that the
		// register page can still hold the last session's value.
		vm.ps1_mode = VMManager::HasValidVM() && (SharedWord::LoadRelaxed(psxHu32(HW_ICFG)) & (1 << 3)) != 0;
		vm.arcade = Arcade::IsActive();
		vm.gs_dump_replay = GSDumpReplayer::IsReplayingDump();
		vm.iop_breakpoints = !CBreakPoints::GetBreakpoints(BREAKPOINT_IOP, true).empty() ||
							 !CBreakPoints::GetMemChecks(BREAKPOINT_IOP).empty();
		vm.recompiler_test_harness = IsRecompilerTestHarness();
		return vm;
	}

	void ChooseMode()
	{
		ChooseModeFor(CurrentHostInfo(), CurrentVmConditions());
	}

	void ChooseModeFor(const HostCpuInfo& host, const VmConditions& vm)
	{
		SetMode(ResolveMode(EmuConfig, host, vm));
	}

	void ChooseModeForReset()
	{
		// The reset puts the IOP back in PS2 mode. The PS1 flag it is going to clear is still set at
		// this point, and would hold the mode at Off for as long as the VM lives.
		VmConditions vm = CurrentVmConditions();
		vm.ps1_mode = false;
		ChooseModeFor(CurrentHostInfo(), vm);
	}

	void DemoteForPs1Switch()
	{
		if (s_mode == IopThreadMode::Off)
			return;

		SetMode(IopThreadMode::Off);
		if (g_thread.IsRunning())
		{
			StopRunningThread();
			Console.WriteLn(Color_StrongGreen, "IOP thread: stopped for the PS1 mode switch.");
		}

		// The EE JIT's dispatcher calls the event test instance of the mode it was generated for. Asking
		// for a reset regenerates it for Off. The EE is inside a memory handler here, so the JIT only
		// notes the request and leaves its loop at the next event test (recResetEE).
		if (Cpu)
			Cpu->Reset();
	}

	bool PlacesThread()
	{
		return g_thread.IsRunning() && g_thread.Mode() == IopThreadMode::Threaded;
	}

	u32 PlacementToken()
	{
		return g_thread.IsRunning() ? g_thread.StartCount() : 0;
	}

	void SyncThread()
	{
		if (g_thread.IsRunning())
		{
			if (g_thread.Mode() == s_mode)
			{
				// The lag bound is the EE's to change; nothing else of Start()'s parameters is meant to change under a
				// running thread.
				if (s_mode == IopThreadMode::Threaded)
					g_thread.SetLag(ClampLag(EmuConfig.Speedhacks.iopThreadLag));
				return;
			}
			StopRunningThread();
			Console.WriteLn(Color_StrongGreen, "IOP thread: stopped.");
		}
		if (s_mode != IopThreadMode::Off)
			g_thread.SetReplyTrace(s_reply_trace);
		if (s_mode != IopThreadMode::Off && g_thread.Start(MakeParams()))
		{
			eeFastForwardKind = 0; // a skip made while the mode was Off has no event test to belong to
			PerformanceMetrics::SetIOPThread(g_thread.GetThreadHandle());
			const char* const names[] = {"Off", "Lockstep", "Threaded"};
			Console.WriteLn(Color_StrongGreen, "IOP thread: started in %s mode.", names[static_cast<int>(s_mode)]);
			if (s_mode == IopThreadMode::Threaded)
			{
				Console.WriteLn(Color_StrongGreen, "IOP thread: lag bound %d EE cycles, minimum slice %d, idle watch %u us.", g_thread.Lag(),
					static_cast<int>(s_min_slice), s_idle_spin_ns / 1000u);
				if (eeEventTestIsActive && !s_start_hold)
				{
					// Started from inside an event test: stopped until the next one (see g_start_hold_pending).
					s_start_hold.emplace(g_thread, HoldKind::Emulation);
					g_start_hold_pending = true;
				}
			}
		}
	}

	void ReleaseStartHold()
	{
		g_start_hold_pending = false;
		s_start_hold.reset();
	}

	void StopThread()
	{
		// The pause hold goes first, while the thread it was taken on still exists.
		s_pause_hold.reset();
		ReleaseStartHold();
		SetMode(IopThreadMode::Off);
		if (g_thread.IsRunning())
		{
			StopRunningThread();
			Console.WriteLn(Color_StrongGreen, "IOP thread: stopped.");
		}
		g_pause_requested.store(false, std::memory_order_relaxed);
	}

	void OnCpuConfigChanged()
	{
		ChooseMode();
		SyncThread();
		if (g_thread.IsRunning())
			g_thread.ReapplyFpcr(EmuConfig.Cpu.FPUFPCR);
	}

	void ResyncClocks()
	{
		if (g_thread.IsRunning())
			g_thread.Resync(cpuRegs.cycle, IopPosition());
	}

	void OnVmPaused(bool paused)
	{
		if (paused)
		{
			if (!s_pause_hold)
				s_pause_hold.emplace(g_thread);
		}
		else
		{
			s_pause_hold.reset();
		}
	}

	bool DeferredPausePending()
	{
		return g_pause_requested.load(std::memory_order_acquire);
	}

	void ClearDeferredPause()
	{
		g_pause_requested.store(false, std::memory_order_relaxed);
	}

	void AssertHeldOrStopped()
	{
		pxAssertMsg(!g_thread.IsRunning() || g_thread.HoldDepth() > 0,
			"the recompilers are reset or state is loaded with the IOP thread running and no hold taken");
	}

	Stats GetStats()
	{
		return g_thread.GetStats();
	}

	namespace
	{
		ViolationHandler s_violation_handler = &FailOnViolation;
		std::atomic<u64> s_violations{0};
	} // namespace

	ViolationHandler SetViolationHandler(ViolationHandler handler)
	{
		return std::exchange(s_violation_handler, handler ? handler : &FailOnViolation);
	}

	u64 ViolationCount()
	{
		return s_violations.load(std::memory_order_relaxed);
	}

	void ResetViolationCount()
	{
		s_violations.store(0, std::memory_order_relaxed);
	}

	namespace
	{
		const char* SideName(Side side)
		{
			switch (side)
			{
				case Side::Iop:
					return "IOP";
				case Side::Ee:
					return "EE";
				case Side::BlockingHostCall:
					return "blocking host call";
			}
			return "?";
		}
	} // namespace

	void FailOnViolation(Side side, const char* where)
	{
		Console.Error("IOP thread ownership: %s", where);
		switch (side)
		{
			case Side::Iop:
				pxFailRel("an IOP-side entry point was called from the EE thread with no hold and no queued item");
				break;
			case Side::Ee:
				pxFailRel("an EE-side entry point was called from the IOP thread in Threaded mode");
				break;
			case Side::BlockingHostCall:
				pxFailRel("Host::RunOnCPUThread(fn, true) was called from the IOP thread, which would wait for a CPU thread that waits for it");
				break;
		}
	}

	void LogViolation(Side side, const char* where)
	{
		// One line per site: a run that trips a site thousands of times says so once.
		static std::mutex mutex;
		static std::set<std::string> seen;
		const std::string key = std::string(SideName(side)) + ":" + where;
		{
			std::lock_guard lock(mutex);
			if (!seen.insert(key).second)
				return;
		}
		Console.Error("IOP thread ownership: %s (%s), called from %s", where, SideName(side),
			g_thread.OnIopThread() ? "the IOP thread" : "a thread that is not the IOP thread");
	}

	bool IopSideAllowed()
	{
		if (s_mode == IopThreadMode::Off || g_thread.OnIopThread())
			return true;
		// With no thread running nothing runs beside the caller, whatever the mode says. A hold stops the IOP,
		// and so does a CallOnEE function: the IOP is blocked in it, at a known point (the SIF pump runs there).
		return g_thread.IopIsStopped();
	}

	bool EeSideAllowed()
	{
		// Lockstep lets the IOP thread call into the EE directly: the EE is blocked in the handoff.
		return s_mode != IopThreadMode::Threaded || !g_thread.OnIopThread();
	}

	bool BlockingHostCallAllowed()
	{
		return !g_thread.OnIopThread();
	}

	void CheckBlockingHostCall(const char* where)
	{
		if (BlockingHostCallAllowed())
			return;
		s_violations.fetch_add(1, std::memory_order_relaxed);
		s_violation_handler(Side::BlockingHostCall, where);
	}

	void CheckIopSide(const char* where)
	{
		if (IopSideAllowed())
			return;
		// Counted here, so that every handler is counted, including one that returns to the caller.
		s_violations.fetch_add(1, std::memory_order_relaxed);
		s_violation_handler(Side::Iop, where);
	}

	void CheckEeSide(const char* where)
	{
		if (EeSideAllowed())
			return;
		s_violations.fetch_add(1, std::memory_order_relaxed);
		s_violation_handler(Side::Ee, where);
	}

	const E2IActions kRealE2IActions = {
		[] { psxHBlankStart(); },
		[] { psxHBlankEnd(); },
		[] { psxVBlankStart(); },
		[] { psxVBlankEnd(); },
		[](u32 line) { iopIntcIrq(line); },
		[](u32 job) {
			switch (job)
			{
				case E2IJob::AutoEjectTick:
					AutoEject::CountDownTicks();
					break;
				case E2IJob::MemcardBusyTick:
					MemcardBusy::Decrement();
					break;
				case E2IJob::VsyncPatchesIop:
					Patch::ApplyVsyncPatchesIOP();
					break;
				default:
					pxFailRel("unknown E2I job");
					break;
			}
		},
		[](u32 channel) { RunSifPumpOnEe(channel == 0 ? &SIF0DmaFromEe : &SIF1DmaFromEe); },
	};

	void ApplyE2IItem(const E2IItem& item, const E2IActions& actions)
	{
		switch (item.kind)
		{
			case E2IKind::HBlankStart:
				actions.hblank_start();
				break;
			case E2IKind::HBlankEnd:
				actions.hblank_end();
				break;
			case E2IKind::VBlankStart:
				actions.vblank_start();
				break;
			case E2IKind::VBlankEnd:
				actions.vblank_end();
				break;
			case E2IKind::IopIrq:
				actions.iop_irq(item.arg);
				break;
			case E2IKind::Call:
				actions.job(item.arg);
				break;
			case E2IKind::SifPump:
				pxAssertRel(actions.sif_pump, "an E2I table without a SIF pump action was given a SifPump item");
				actions.sif_pump(item.arg);
				break;
			default:
				pxFailRel("unknown E2I item kind");
				break;
		}
	}

	const E2IActions* SetE2IActionsForTests(const E2IActions* actions)
	{
		return std::exchange(s_e2i_actions, actions);
	}

	void ApplyOnIop(u16 kind, u32 arg)
	{
		// The stamp is the EE's cycle at the moment Off would have made the direct call.
		const u64 stamp = cpuRegs.cycle;
		if (g_thread.IsRunning())
		{
			if (g_thread.Mode() == IopThreadMode::Lockstep)
			{
				if (g_thread.PostE2I(stamp, kind, arg))
				{
					// With a skew the item waits in the queue for the IOP's clock to reach the stamp (the handoff
					// applies it); PostE2I does not fail under a skew, because what the ring cannot hold goes to a
					// spill queue. Without one the EE waits here until the IOP thread has applied the item, so it
					// takes effect at this point of the EE's execution and not later.
					if (g_thread.LockstepSkew() == 0)
						g_thread.LockstepApplyUpTo(stamp);
					return;
				}
				pxAssertMsg(g_thread.LockstepSkew() == 0, "an EE-to-IOP item could not be queued under a Lockstep skew");
			}
			else
			{
				// Threaded: the IOP applies it when its own clock reaches the stamp, and the EE goes on.
				if (g_thread.PostE2I(stamp, kind, arg))
					return;
				// The queue can fail only while the IOP is held or blocked in a CallOnEE function (the lag bound
				// keeps the ring nearly empty otherwise), or when the VM is going away. If the IOP is stopped,
				// apply the item here; if the VM is going away, drop it.
				pxAssertMsg(g_thread.IopIsStopped() || VMManager::GetState() == VMState::Stopping || VMManager::GetState() == VMState::Shutdown,
					"an EE-to-IOP item could not be queued");
				if (!g_thread.IopIsStopped())
					return;
			}
		}

		// No IOP thread (Off, or a mode switch or a shutdown in progress), or it is stopped: the call Off makes.
		ApplyE2IItem(E2IItem{stamp, kind, 0, arg}, s_e2i_actions ? *s_e2i_actions : kRealE2IActions);
	}

	bool DeferSifPump(u32 channel)
	{
		pxAssertMsg(channel <= 1, "only SIF0 and SIF1 pumps are deferred");
		if (s_mode == IopThreadMode::Off)
			return false;
		// No thread, or the IOP is already stopped (a hold, a CallOnEE function): nothing to wait for, so the
		// caller's pump runs in place, exactly as with the setting off.
		if (!g_thread.IsRunning() || g_thread.IopIsStopped())
			return false;
		// The holds variant of a Lockstep skew (testing): bring the IOP up to the EE, and let the caller pump in place.
		if (g_thread.LockstepSkew() > 0 && ((s_lockstep_skew_holds >> channel) & 1))
		{
			const u64 reached = g_thread.LockstepCatchUp(cpuRegs.cycle);
			// RunLockstepSlice's books for a skew: the IOP is owed what the EE has run beyond where it is (now about nothing).
			EEsCycle = static_cast<s32>(cpuRegs.cycle - reached);
			EEoCycle = cpuRegs.cycle;
			return false;
		}
		// A Lockstep skew always defers: pumping at once would hand the IOP the data D cycles early.
		if (!EmuConfig.Speedhacks.iopThreadDeferSif && g_thread.LockstepSkew() == 0)
			return false;
		// Stamped with the EE's cycle at the moment Off would have run the pump. Lockstep applies it before
		// this returns (ApplyOnIop waits for the IOP thread), which is Off's order.
		ApplyOnIop(E2IKind::SifPump, channel);
		g_thread.CountSifDeferred();
		return true;
	}

	void EeNoteSifStart(u32 channel)
	{
		if (s_mode == IopThreadMode::Off || !g_thread.IsRunning())
			return;
		g_thread.NoteEeSifStart(cpuRegs.cycle, channel);
	}

	void EeNoteSifPoll()
	{
		if (s_mode == IopThreadMode::Off || !g_thread.IsRunning())
			return;
		g_thread.NoteEePoll(cpuRegs.cycle);
	}

	void Sif0PumpFromIop()
	{
		g_thread.NoteIopSif0Reply(cpuRegs.cycle);
		SIF0Dma();
	}

	bool SyncIopForSifChannelStop(u32 channel)
	{
		pxAssertMsg(channel <= 1, "only the SIF0 and SIF1 channels are synchronised at a stop");
		if (s_mode == IopThreadMode::Off)
			return false;
		if (!g_thread.IsRunning() || g_thread.IopIsStopped())
			return false;
		if (s_mode == IopThreadMode::Threaded)
		{
			// The IOP runs up to the EE's cycle and stops; the hold ends with the scope. What it did on the way, the IOP-started pump
			// included, ran on this thread (the hold's spin serves it) before the scope ended.
			ScopedHold hold(g_thread, HoldKind::Emulation);
			return true;
		}
		// Lockstep: the IOP is current already unless a skew keeps it behind, and then only the holds variant catches it up at an EE event
		// (the deferring variant would hand the IOP the stop D cycles early).
		if (g_thread.LockstepSkew() > 0 && ((s_lockstep_skew_holds >> channel) & 1))
		{
			const u64 reached = g_thread.LockstepCatchUp(cpuRegs.cycle);
			// RunLockstepSlice's books for a skew, as at a held pump.
			EEsCycle = static_cast<s32>(cpuRegs.cycle - reached);
			EEoCycle = cpuRegs.cycle;
			return true;
		}
		return false;
	}

	bool TryPostToIop(HostCommandQueue::Command command)
	{
		return g_thread.TryPostHostCommand(std::move(command)) != 0;
	}

	void PostToCpuThread(std::function<void()> fn)
	{
		Host::RunOnCPUThread(std::move(fn), false);
	}

	bool LockstepActive()
	{
		return g_thread.IsRunning() && g_thread.Mode() == IopThreadMode::Lockstep;
	}

	void RunLockstepSlice()
	{
		const u64 reached = g_thread.LockstepRunSlice(cpuRegs.cycle);
		// With a skew the slice did not touch the books (it ran to a target), so they are written from where the IOP
		// is: what it is owed is what the EE has run beyond its position, about the skew. Without one the slice has
		// left them where Off does.
		if (g_thread.LockstepSkew() > 0)
			EEsCycle = static_cast<s32>(cpuRegs.cycle - reached);

		// A pause the IOP asked for from inside the slice: the CPU thread makes it, and leaves its
		// execution loop, exactly where Off would have made it.
		if (g_pause_requested.load(std::memory_order_acquire)) [[unlikely]]
		{
			g_pause_requested.store(false, std::memory_order_relaxed);
			VMManager::SetPaused(true);
			Cpu->ExitExecution();
		}
	}

	void RequestPause()
	{
		if (g_thread.OnIopThread())
		{
			if (s_mode == IopThreadMode::Threaded)
			{
				// The EE makes the pause at its next event test. A full queue is emptied by the EE; if the
				// object is stopping the item is not needed.
				g_thread.PostI2E(I2EKind::PauseRequest);
				return;
			}
			g_pause_requested.store(true, std::memory_order_release);
			return;
		}

		VMManager::SetPaused(true);
		Cpu->ExitExecution();
	}

	namespace
	{
		void PauseTheVm()
		{
			VMManager::SetPaused(true);
			Cpu->ExitExecution();
		}

		PauseAction s_pause_action = &PauseTheVm;
	} // namespace

	PauseAction SetPauseActionForTests(PauseAction action)
	{
		return std::exchange(s_pause_action, action ? action : &PauseTheVm);
	}

	void DrainEeQueue()
	{
		g_thread.DrainI2E();

		// A pause the IOP asked for. It is made here, in the event test, where Off makes it, and not in the item
		// handler, which also runs inside the EE's spins.
		if (g_pause_requested.load(std::memory_order_acquire))
		{
			g_pause_requested.store(false, std::memory_order_relaxed);
			s_pause_action();
		}
	}

	void RunSifPumpOnEe(void (*pump)())
	{
		if (!g_thread.OnIopThread())
		{
			pump();
			return;
		}
		// The IOP blocks until the EE has run the pump; false means the object was stopped first, and the pump is
		// not run (the VM is going away).
		g_thread.CallOnEE([pump] { pump(); });
	}

	void SetThreadedMinSlice(s32 min_slice)
	{
		s_min_slice = std::max<s32>(min_slice, 0);
	}

	s32 ThreadedMinSlice()
	{
		return s_min_slice;
	}

	void SetLockstepSkew(s64 cycles)
	{
		s_lockstep_skew = std::clamp<s64>(cycles, 0, kMaxLag);
	}

	s64 LockstepSkewSetting()
	{
		return s_lockstep_skew;
	}

	void SetLockstepSkewHolds(bool holds)
	{
		s_lockstep_skew_holds = holds ? 3u : 0u;
	}

	void SetLockstepSkewHoldChannels(u32 mask)
	{
		s_lockstep_skew_holds = mask & 3u;
	}

	bool LockstepSkewHolds()
	{
		return s_lockstep_skew_holds != 0;
	}

	void SetReplyTrace(bool on)
	{
		s_reply_trace = on;
	}

	bool ReplyTrace()
	{
		return s_reply_trace;
	}

	void NoteDirectReply(u32 kind)
	{
		if (s_mode != IopThreadMode::Lockstep || !g_thread.IsRunning() || !g_thread.OnIopThread())
			return;
		g_thread.NoteReply(kind, static_cast<u32>(IopTimeNow()));
		if (kind == ReplyKind::SbusIrq || kind == ReplyKind::Sif0Pump)
			g_thread.NoteIopSif0Reply(cpuRegs.cycle);
	}

	void SetThreadedIdleSpinNs(u32 ns)
	{
		s_idle_spin_ns = ns;
	}

	u32 ThreadedIdleSpinNs()
	{
		return s_idle_spin_ns;
	}

	std::string FormatStats(const Stats& t, double frames)
	{
		const double f = std::max(frames, 1.0);
		return fmt::format("W1 {:.2f}/f {:.1f} us/f (found IOP asleep {:.2f}/f {:.1f} us/f, idle {:.2f}/f {:.1f} us/f) | W2 {:.2f}/f {:.1f} us/f | I2E {:.1f}/f | SIF pumps ee {:.2f}/f deferred {:.2f}/f (applied {:.2f}/f) iop {:.2f}/f | SBUS near-bound waits {:.2f}/f {:.1f} us/f | IOP sleeps {:.1f}/f wakes {:.1f}/f idle {:.0f} us/f | slices {:.0f}/f | replies {:.2f}/f late {:.2f}/f idle overshoots {:.2f}/f (max {} cycles, unbroken {:.2f}/f, 90% {:.2f}/f; late but EE busy {:.2f}/f, idle light {:.2f}/f)",
			t.lag_spins / f, t.lag_stall_ns / 1000.0 / f, t.lag_spins_parked / f, t.lag_parked_stall_ns / 1000.0 / f,
			t.lag_spins_idle / f, t.lag_idle_stall_ns / 1000.0 / f,
			t.emu_hold_requests / f, t.emu_hold_stall_ns / 1000.0 / f, t.i2e_drained / f,
			t.sif_ee_pumps / f, t.sif_deferred / f, t.sif_deferred_applied / f,
			// CallOnEE requests are the IOP-started pumps and the deferred ones; the deferred ones have their own figure.
			(t.calls_posted > t.sif_deferred_applied ? t.calls_posted - t.sif_deferred_applied : 0) / f,
			t.near_waits / f, t.near_wait_ns / 1000.0 / f,
			t.parks / f, t.wakes_sent / f, t.idle_ns / 1000.0 / f, t.slices / f,
			t.replies / f, t.late_replies / f, t.idle_overshoots / f, t.idle_overshoot_max, t.idle_overshoots_unbroken / f, t.idle_overshoots_90 / f,
			t.late_ee_busy / f, t.late_idle_light / f);
	}

	namespace
	{
		std::string HistLine(const char* what, const char* kind, const LateHist& h)
		{
			std::string line = fmt::format("IOPLATE: {} {} n={} mean={:.0f} max={} |", what, kind, h.count, h.Mean(), h.max);
			for (u32 i = 0; i < kLateBuckets; i++)
				line += fmt::format(" {}", h.bucket[i]);
			return line;
		}
	} // namespace

	std::vector<std::string> FormatReplyStats(const ReplyStats& r)
	{
		std::vector<std::string> lines;
		std::string edges;
		for (u32 i = 0; i < kLateEdgeCount; i++)
			edges += fmt::format(" <{}", kLateEdges[i]);
		edges += fmt::format(" >={}", kLateEdges[kLateEdgeCount - 1]);
		lines.push_back(fmt::format("IOPLATE: buckets (EE cycles late):{}", edges));
		lines.push_back(fmt::format(
			"IOPLATE: replies={} late={} (>= {} cycles) idle_overshoots={} (idle share >= {}%) overshoot_cycles={} overshoot_max={} unbroken={} share90={} late_ee_busy={} (cycles {}) "
			"late_idle_light={} (cycles {}) late_idle_unknown={}",
			r.replies, r.late, kOvershootMin, r.idle_overshoots, kIdleShareMin, r.idle_overshoot_cycles, r.idle_overshoot_max, r.idle_overshoots_unbroken, r.idle_overshoots_90,
			r.late_ee_busy, r.late_ee_busy_cycles, r.late_idle_light, r.late_idle_light_cycles, r.late_idle_unknown));
		LateHist all_kinds, over_kinds;
		for (u32 k = 0; k < ReplyKind::kCount; k++)
		{
			for (const LateHist* src : {&r.all[k], &r.overshoot[k]})
			{
				LateHist& dst = (src == &r.all[k]) ? all_kinds : over_kinds;
				dst.count += src->count;
				dst.sum += src->sum;
				dst.max = std::max(dst.max, src->max);
				for (u32 i = 0; i < kLateBuckets; i++)
					dst.bucket[i] += src->bucket[i];
			}
		}
		lines.push_back(HistLine("all", "total", all_kinds));
		for (u32 k = 0; k < ReplyKind::kCount; k++)
			if (r.all[k].count)
				lines.push_back(HistLine("all", ReplyKind::Name(k), r.all[k]));
		lines.push_back(HistLine("idle_overshoot", "total", over_kinds));
		for (u32 k = 0; k < ReplyKind::kCount; k++)
			if (r.overshoot[k].count)
				lines.push_back(HistLine("idle_overshoot", ReplyKind::Name(k), r.overshoot[k]));
		std::string dec;
		for (u32 i = 0; i <= 10; i++)
			dec += fmt::format(" {}", r.coverage_decile[i]);
		lines.push_back(fmt::format("IOPLATE: late replies by idle share of [production, taken] in tenths (0..10):{}", dec));
		std::string wake;
		for (u32 i = 0; i <= 10; i++)
			wake += fmt::format(" {}", r.wake_decile[i]);
		lines.push_back(fmt::format("IOPLATE: idle overshoots by the share of the next {} cycles the EE then ran, in tenths (0..10):{}", kWakeWindow, wake));
		lines.push_back(fmt::format("IOPLATE: frames={} with_overshoot={} by overshoots in the frame (0 1 2 3-4 5-8 9+): {} {} {} {} {} {}", r.frames, r.frames_with_overshoot,
			r.frame_bins[0], r.frame_bins[1], r.frame_bins[2], r.frame_bins[3], r.frame_bins[4], r.frame_bins[5]));
		lines.push_back(fmt::format("IOPLATE: event_tests={} ff_event_tests={} ({:.1f}%) ff_cycles={} ee_cycles={} ({:.1f}% idle)", r.event_tests, r.ff_event_tests,
			r.event_tests ? 100.0 * r.ff_event_tests / r.event_tests : 0.0, r.ff_cycles, r.ee_cycles, r.ee_cycles ? 100.0 * r.ff_cycles / r.ee_cycles : 0.0));
		return lines;
	}

	std::vector<std::string> FormatReplyTrace(const std::vector<FrameRecord>& trace)
	{
		std::vector<std::string> lines;
		lines.reserve(trace.size());
		for (const FrameRecord& r : trace)
			lines.push_back(fmt::format("IOPLATE-FRAME: frame={} replies={} late={} idle_overshoots={} unbroken={} share90={} max_late={} ff_cycles={} ee_cycles={}", r.frame, r.replies,
				r.late, r.idle_overshoots, r.unbroken, r.share90, r.max_late, r.ff_cycles, r.ee_cycles));
		return lines;
	}

	std::string OsdStatsLine()
	{
		if (s_mode != IopThreadMode::Threaded || !g_thread.IsRunning())
			return std::string();

		// Per frame over the time since the last call. The counters have one writer each and are read here
		// while they move, so the numbers are approximate.
		static Stats s_last = {};
		static u32 s_last_frame = 0;
		const Stats now = g_thread.GetStats();
		const u32 frame = g_FrameCount;
		const u32 frames = frame - s_last_frame;
		Stats delta = now;
		const u64* a = reinterpret_cast<const u64*>(&now);
		const u64* b = reinterpret_cast<const u64*>(&s_last);
		u64* d = reinterpret_cast<u64*>(&delta);
		static_assert(sizeof(Stats) % sizeof(u64) == 0);
		for (size_t i = 0; i < sizeof(Stats) / sizeof(u64); i++)
			d[i] = a[i] >= b[i] ? a[i] - b[i] : a[i];
		s_last = now;
		s_last_frame = frame;
		return frames ? "IOP " + FormatStats(delta, frames) : std::string();
	}

} // namespace IopThread
