// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the VM does with the IOP thread: the one thread object, the mode in effect, and the calls the
// VM's lifecycle makes (design sections 9.2 to 9.5). Only Lockstep is wired up; Threaded resolves to
// Off until it exists.
//
// In Lockstep the IOP thread runs the IOP's slice of every EE event test while the EE waits (design
// section 6.2). Everything else still runs on the EE thread, which is legal because the IOP thread is
// parked whenever the EE runs. A hold (VmHold) is therefore a marker in Lockstep, but the lifecycle
// takes one at every site where Threaded will need a real one, so the sites are in place and checked.
//
// Everything here is called on the CPU thread unless it says otherwise.

#pragma once

#include "IopThread/IopThread.h"
#include "IopThread/IopThreadMode.h"

#include <optional>

namespace IopThread
{
	/// The one IOP thread object. It exists for the life of the process and runs only while the mode
	/// in effect is not Off.
	extern Thread g_thread;

	// ---- Choosing and starting ---------------------------------------------------------------

	/// What this host looks like to ResolveMode. Only is_arm64 matters until Threaded exists: its
	/// core-count rule is wired to the real topology with the placement work (design step 14).
	HostCpuInfo CurrentHostInfo();

	/// The conditions about the running VM that rule the IOP thread out: PS1 mode, an arcade title,
	/// a GS dump replay, an IOP breakpoint or memcheck, and the recompiler test harness flag.
	VmConditions CurrentVmConditions();

	/// Resolves the mode from EmuConfig and the VM, and makes it the mode in effect: s_mode, and the
	/// interpreter's event test instance. It does not touch the thread. The EE JIT's dispatcher is
	/// generated for the mode in effect, so this runs before the recompilers are reset, and every
	/// caller resets them right after (VMManager::Initialize, and a settings change in
	/// CheckForCPUConfigChanges). Threaded is not built yet, so it resolves to Off here.
	void ChooseMode();
	/// The same with the host and the VM conditions given, for the tests: ResolveMode would give Threaded
	/// on a host with enough cores, and that is where the demotion to Off applies.
	void ChooseModeFor(const HostCpuInfo& host, const VmConditions& vm);

	/// Makes the thread match the mode in effect: starts it when the mode is Lockstep and it is not
	/// running, stops it when the mode is Off and it is. The IOP's clocks are taken from the EE's, so
	/// call it with the CPU state in place (after cpuReset). With a hold live the thread starts held.
	void SyncThread();

	/// The VM is going away: stops the thread, releases the pause hold and turns the mode Off.
	void StopThread();

	/// The CPU settings changed (VMManager::CheckForCPUConfigChanges): choose again, start or stop the
	/// thread to match, and give a running thread the new FPCR. Call it before the recompilers are
	/// reset.
	void OnCpuConfigChanged();

	// ---- Clocks and pausing ------------------------------------------------------------------

	/// The EE's cycle counter and the IOP's slice books were reset or loaded (cpuReset, a savestate
	/// load): move the thread's view of the IOP's position to match. Needs the IOP held or stopped.
	void ResyncClocks();

	/// The VM paused or resumed (VMManager::SetState). A paused VM keeps a hold for as long as it
	/// stays paused, so the debugger and the settings see a still IOP (design 9.3).
	void OnVmPaused(bool paused);

	/// A request from the IOP thread to pause the VM, waiting for the EE. RunLockstepSlice() makes it
	/// as soon as the handoff returns; the two functions are for the tests and for that.
	bool DeferredPausePending();
	void ClearDeferredPause();

	// ---- Holds -------------------------------------------------------------------------------

	/// A full stop of the IOP for the length of a scope: the EE thread may touch IOP state inside it.
	/// A marker in Lockstep and with no thread. See ScopedHold for the rules.
	class VmHold
	{
	public:
		explicit VmHold(HoldKind kind = HoldKind::Lifecycle)
			: m_hold(g_thread, kind)
		{
		}

		bool Acquired() const { return m_hold.Acquired(); }

	private:
		ScopedHold m_hold;
	};

	/// Devel assert for the sites that reset the recompilers or load state: the IOP must be held or
	/// not running. A marker hold counts.
	void AssertHeldOrStopped();

	/// The thread object's counters (handoffs and so on), for the run reports.
	Stats GetStats();
} // namespace IopThread
