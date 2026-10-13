// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopThreadVm.h"

#include "Common.h"
#include "Config.h"
#include "DebugTools/Breakpoints.h"
#include "GSDumpReplayer.h"
#include "IopHw.h"
#include "IopMem.h"
#include "IopThread/SharedWord.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/ARCADE.h"
#include "common/Assertions.h"
#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace IopThread
{
	Thread g_thread;

	namespace
	{
		// The pause hold (design 9.3), kept for as long as the VM stays paused.
		std::optional<ScopedHold> s_pause_hold;

		// A pause the IOP thread asked for, waiting for the EE to make it.
		std::atomic<bool> s_pause_requested{false};

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

		Params MakeParams()
		{
			Params params;
			params.mode = s_mode;
			params.lag = ClampLag(EmuConfig.Speedhacks.iopThreadLag);
			params.min_slice = 0;
			params.start_ee_cycle = cpuRegs.cycle;
			params.start_iop_pos = IopPosition();
			params.fpcr = EmuConfig.Cpu.FPUFPCR;
			params.stack_size = VMManager::EMU_THREAD_STACK_SIZE;
			params.run_slice = RunIopSlice;
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
	} // namespace

	// ================================================================ choosing and starting

	HostCpuInfo CurrentHostInfo()
	{
		HostCpuInfo host;
#if defined(ARCH_ARM64)
		host.is_arm64 = true;
#endif
		// The core-count rule only matters to Threaded, which does not exist yet; the real
		// performance-core count comes with the thread placement work.
		host.total_cores = std::thread::hardware_concurrency();
		return host;
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
		IopThreadMode mode = ResolveMode(EmuConfig, host, vm);
		// Threaded is not built yet (design step 11): the setting resolves to Off until it is.
		if (mode == IopThreadMode::Threaded)
			mode = IopThreadMode::Off;
		SetMode(mode);
	}

	void SyncThread()
	{
		if (g_thread.IsRunning())
		{
			if (g_thread.Mode() == s_mode)
				return;
			g_thread.Stop();
			Console.WriteLn(Color_StrongGreen, "IOP thread: stopped.");
		}
		if (s_mode != IopThreadMode::Off && g_thread.Start(MakeParams()))
			Console.WriteLn(Color_StrongGreen, "IOP thread: started in %s mode.", s_mode == IopThreadMode::Lockstep ? "Lockstep" : "Threaded");
	}

	void StopThread()
	{
		// The pause hold goes first, while the thread it was taken on still exists.
		s_pause_hold.reset();
		SetMode(IopThreadMode::Off);
		if (g_thread.IsRunning())
		{
			g_thread.Stop();
			Console.WriteLn(Color_StrongGreen, "IOP thread: stopped.");
		}
		s_pause_requested.store(false, std::memory_order_relaxed);
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
		return s_pause_requested.load(std::memory_order_acquire);
	}

	void ClearDeferredPause()
	{
		s_pause_requested.store(false, std::memory_order_relaxed);
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
		if (s_pause_requested.load(std::memory_order_acquire)) [[unlikely]]
		{
			s_pause_requested.store(false, std::memory_order_relaxed);
			VMManager::SetPaused(true);
			Cpu->ExitExecution();
		}
	}

	void RequestPause()
	{
		if (g_thread.OnIopThread())
		{
			s_pause_requested.store(true, std::memory_order_release);
			return;
		}

		VMManager::SetPaused(true);
		Cpu->ExitExecution();
	}
} // namespace IopThread
