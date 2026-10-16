// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopThreadVm.h"

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

	// A pause the IOP thread asked for, waiting for the EE to make it.
	std::atomic<bool> g_pause_requested{false};

	namespace
	{
		// The testing knob for S_min (design 5.3); 0 runs a slice whenever there is budget.
		s32 s_min_slice = 0;

		// The testing knob for the idle-watch window (Params::idle_spin_ns).
		u32 s_idle_spin_ns = kDefaultIdleSpinNs;

		// The pause hold (design 9.3), kept for as long as the VM stays paused.
		std::optional<ScopedHold> s_pause_hold;

		// The hold that keeps a Threaded thread started inside an event test stopped until the next one (g_start_hold_pending).
		std::optional<ScopedHold> s_start_hold;

		// The table the IOP thread applies E2I items with. Null is the real one (tests swap it).
		const E2IActions* s_e2i_actions = nullptr;

		// The IOP's position in EE cycles: where the EE is, less the IOP time still owed (design 3.3).
		// EEsCycle is negative when the IOP has run past the EE.
		u64 IopPosition()
		{
			return cpuRegs.cycle - static_cast<s64>(EEsCycle);
		}

		// What the thread runs for each Lockstep request. The slice is the same code Off runs on the EE
		// thread, iopRunSliceOnIopThread(); this only keeps the thread object's view of the IOP's position
		// moving with the books. A reset or a state load moves the books by itself and calls ResyncClocks().
		u64 RunIopSlice(u64 from, u64 /*target*/)
		{
			iopRunSliceOnIopThread();
			return std::max(from, IopPosition());
		}

		// What the thread runs for each Threaded slice (design 5.2): today's pair, with the budget the
		// thread computed from the watermark in place of EEsCycle. A slice is not given more than half of
		// the s32 range, which a hold's catch-up after a very long stop could otherwise exceed. EEsCycle and
		// EEoCycle are the EE's books and are not touched.
		u64 RunIopSliceThreaded(u64 from, u64 target)
		{
			IOP_SIDE_ASSERT();

			const s64 owed = static_cast<s64>(target - from);
			const s32 budget = static_cast<s32>(std::clamp<s64>(owed, INT32_MIN / 2, INT32_MAX / 2));
			s32 remaining = budget;

			if (budget > 0)
				iopEventAction = true;
			if (iopEventAction)
			{
				remaining = psxCpu->ExecuteBlock(budget);
				iopEventAction = false;
			}
			iopEventTest();

			// The IOP ran `budget - remaining` cycles: past the target by the block it overshot with when
			// `remaining` is negative. It never moves back.
			const s64 ran = static_cast<s64>(budget) - static_cast<s64>(remaining);
			return from + static_cast<u64>(std::max<s64>(ran, 0));
		}

		// What the EE does with an item the IOP queued (design 3.5). A CallOnEE is run by the thread object.
		void HandleI2EItem(const I2EItem& item)
		{
			switch (item.kind)
			{
				case I2EKind::SbusIrq:
					// INTC_STAT is written on the EE thread only: the EE JIT loads it inline when the IntcStat
					// speedhack is off (design 7.1 C13).
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

		// The EE-side books from where the IOP is (design 9.1, Threaded to anything): the IOP is at `pos`, so it
		// is owed what the EE has run beyond that. Valid only while the IOP is held or stopped.
		void BooksFromThread()
		{
			EEsCycle = static_cast<s32>(cpuRegs.cycle - g_thread.PublishedPos());
			EEoCycle = cpuRegs.cycle;
		}

		Params MakeParams()
		{
			Params params;
			params.mode = s_mode;
			params.lag = ClampLag(EmuConfig.Speedhacks.iopThreadLag);
			params.min_slice = s_mode == IopThreadMode::Threaded ? s_min_slice : 0;
			params.idle_spin_ns = s_idle_spin_ns;
			params.start_ee_cycle = cpuRegs.cycle;
			params.start_iop_pos = IopPosition();
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
			}
			params.apply_item = [](const E2IItem& item) { ApplyE2IItem(item, s_e2i_actions ? *s_e2i_actions : kRealE2IActions); };
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

	// ================================================================ choosing and starting

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
		if (s_mode != IopThreadMode::Off && g_thread.Start(MakeParams()))
		{
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

	// ================================================================ clocks and pausing

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

	// ================================================================ ownership asserts

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

	// ================================================================ EE-to-IOP items

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
					// The EE waits here until the IOP thread has applied the item, so it takes effect at this
					// point of the EE's execution and not later.
					g_thread.LockstepApplyUpTo(stamp);
					return;
				}
			}
			else
			{
				// Threaded: the IOP applies it when its own clock reaches the stamp, and the EE goes on.
				if (g_thread.PostE2I(stamp, kind, arg))
					return;
				// The ring cannot make room for it while the IOP is held or blocked in a CallOnEE function
				// (the lag bound keeps it nearly empty, so this does not happen), and the stop predicate ends a
				// wait for room when the VM is going away. In the first case the IOP is stopped and the item can be
				// applied here; in the second it is not, and the item is dropped with the VM.
				pxAssertMsg(g_thread.IopIsStopped() || VMManager::GetState() == VMState::Stopping || VMManager::GetState() == VMState::Shutdown,
					"an EE-to-IOP item could not be queued");
				if (!g_thread.IopIsStopped())
					return;
			}
		}

		// No IOP thread (Off, or a mode switch or a shutdown in progress), or it is stopped: the call Off makes.
		ApplyE2IItem(E2IItem{stamp, kind, 0, arg}, s_e2i_actions ? *s_e2i_actions : kRealE2IActions);
	}

	// ================================================================ host threads and the CPU thread

	bool TryPostToIop(HostCommandQueue::Command command)
	{
		return g_thread.TryPostHostCommand(std::move(command)) != 0;
	}

	void PostToCpuThread(std::function<void()> fn)
	{
		Host::RunOnCPUThread(std::move(fn), false);
	}

	// ================================================================ the EE core's calls

	bool LockstepActive()
	{
		return g_thread.IsRunning() && g_thread.Mode() == IopThreadMode::Lockstep;
	}

	void RunLockstepSlice()
	{
		g_thread.LockstepRunSlice(cpuRegs.cycle);

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

	// ================================================================ Threaded: the EE's event test and SIF

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

	void SetThreadedIdleSpinNs(u32 ns)
	{
		s_idle_spin_ns = ns;
	}

	u32 ThreadedIdleSpinNs()
	{
		return s_idle_spin_ns;
	}

	// ================================================================ counters

	std::string FormatStats(const Stats& t, double frames)
	{
		const double f = std::max(frames, 1.0);
		return fmt::format("W1 {:.2f}/f {:.1f} us/f (found IOP asleep {:.2f}/f {:.1f} us/f, idle {:.2f}/f {:.1f} us/f) | W2 {:.2f}/f {:.1f} us/f | I2E {:.1f}/f | SIF pumps ee {:.2f}/f iop {:.2f}/f | IOP sleeps {:.1f}/f wakes {:.1f}/f idle {:.0f} us/f | slices {:.0f}/f",
			t.lag_spins / f, t.lag_stall_ns / 1000.0 / f, t.lag_spins_parked / f, t.lag_parked_stall_ns / 1000.0 / f,
			t.lag_spins_idle / f, t.lag_idle_stall_ns / 1000.0 / f,
			t.emu_hold_requests / f, t.emu_hold_stall_ns / 1000.0 / f, t.i2e_drained / f,
			t.sif_ee_pumps / f, t.calls_posted / f, t.parks / f, t.wakes_sent / f, t.idle_ns / 1000.0 / f, t.slices / f);
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
