// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Which way the IOP runs: on the EE's thread as it always has (Off), on its own thread in strict
// turns with the EE (Lockstep), or on its own thread concurrently with the EE (Threaded).
//
// This header holds the three-way choice, the pure function that makes it (ResolveMode), and the
// few run-time pieces the EE core needs without pulling in the thread object: the mode in effect,
// the test-harness flag, and the calls the Lockstep event test makes. IopThreadVm.h has the rest of
// what the VM does with the thread.

#pragma once

#include "common/Pcsx2Types.h"

struct Pcsx2Config;

enum class IopThreadMode : u8
{
	Off, // the IOP runs inside the EE's event test, as before
	Lockstep, // the IOP has its own thread, but the two threads take strict turns
	Threaded, // the IOP has its own thread and runs up to the lag bound behind the EE
};

namespace IopThread
{
	// How many EE cycles pass between two EE event tests at most. This is the number the EE core
	// schedules its next event test with (R5900.cpp shares it), so the lag bound below can be
	// stated relative to it.
	inline constexpr s32 kEeWaitCycles = 3072;

	// The lag bound L: how many EE cycles the EE may be ahead of the IOP at an event test before
	// it waits. All three values are in EE cycles.
	//
	// The default is eight times kEeWaitCycles. A larger L lets the EE run further ahead of the
	// IOP, which lowered the EE's time per frame on a big core; the gain flattened above 12288
	// in some titles.
	//
	// The minimum has to be strictly greater than kEeWaitCycles. The IOP only runs up to the cycle
	// the EE published at its previous event test, so at the next one the EE is already about
	// kEeWaitCycles ahead of it. A bound at or below that finds the IOP behind on nearly every
	// check. The extra 1024 cycles cover the measured overshoot: an event test lands past its
	// target by 81 cycles at the 90th percentile, and the IOP ends its last block up to 57 cycles
	// past what it was given. 1024 is more than seven times the sum of those.
	//
	// The maximum, 64 times kEeWaitCycles, is a sanity bound. It is a twenty-fifth of a frame
	// (4.9 million EE cycles at 60 Hz), so the cycle-stamped EE-to-IOP queue (1024 entries, about
	// 527 posts per frame) cannot fill within one window, and the largest delay the EE can see an
	// IOP effect after is still below a millisecond.
	inline constexpr s32 kDefaultLag = 8 * kEeWaitCycles;
	inline constexpr s32 kMinLag = kEeWaitCycles + 1024;
	inline constexpr s32 kMaxLag = 64 * kEeWaitCycles;

	static_assert(kMinLag > kEeWaitCycles);
	static_assert(kMinLag <= kDefaultLag && kDefaultLag <= kMaxLag);

	/// The legal value nearest to `requested`. A value from a settings file, a per-game override
	/// or a command line goes through this before anything uses it as L.
	s32 ClampLag(s32 requested);

	/// What ResolveMode needs to know about the host. A test describes a device with it; the
	/// emulator fills it from the CPU topology.
	struct HostCpuInfo
	{
		bool is_arm64 = false; // both modes are ARM64-only: the x86 IOP JIT is not audited for a second thread
		u32 total_cores = 0;
		/// Cores of at least 0.75 of the fastest core's speed (GetPerformanceCoresMask() in
		/// VMManager.cpp). 0 when the cores cannot be told apart or cannot be read.
		u32 performance_cores = 0;
	};

	/// The things about the running VM, other than the settings, that rule the IOP thread out.
	struct VmConditions
	{
		bool ps1_mode = false; // the game switched the IOP to PS1 mode (HW_ICFG bit 3)
		bool arcade = false; // an arcade title (Arcade::IsActive())
		bool gs_dump_replay = false; // a GS dump is being replayed, with no real IOP
		bool iop_breakpoints = false; // any IOP breakpoint or memcheck exists
		bool recompiler_test_harness = false; // the DiffJitVsInterp harness, a single-thread interleave
	};

	/// Decides how the IOP runs, from what was asked for and what this host and VM allow.
	/// Pure: it reads only its arguments.
	///
	/// Returns Off unless the player enabled the IOP thread and none of the VM conditions holds:
	/// the VM is in PS1 mode, runs an arcade title, replays a GS dump, has an IOP breakpoint, or
	/// is under the recompiler test harness.
	///
	/// On a host that is not ARM64 it returns Off for both modes: the x86 IOP JIT is not audited
	/// for running on another thread.
	///
	/// With the lockstep flag set it then returns Lockstep. Lockstep is the correctness reference
	/// used by tests, eerunner and debugging, so the core rule below does not apply to it.
	///
	/// Without the lockstep flag it returns Threaded, unless the host has no spare big core. The
	/// threads are the EE, the GS, the IOP and, with MTVU on, the VU1 thread. It needs at least
	/// that many performance cores, and at least two more cores in total for the OS, the GS back
	/// thread and the audio callback.
	IopThreadMode ResolveMode(const Pcsx2Config& config, const HostCpuInfo& host, const VmConditions& vm);

	/// The mode in effect. A plain variable, written on the CPU thread only, by ChooseMode(). The EE
	/// core reads it only where it builds something for the mode (the JIT dispatcher at a recompiler
	/// reset, the interpreter's event test pointer). No event-frequency path reads it.
	//
	// Hidden visibility: every reader is in the one executable, and a default-visibility variable is
	// read through the GOT in position-independent code (two loads for one byte). The ownership
	// asserts read it at event frequency in Off, so they are an adrp, a byte load and a branch.
#if defined(__GNUC__) || defined(__clang__)
	extern IopThreadMode s_mode __attribute__((visibility("hidden")));
#else
	extern IopThreadMode s_mode;
#endif
	inline IopThreadMode GetMode() { return s_mode; }

	/// Says that the gtest recompiler environment (DiffJitVsInterp) is running, which makes
	/// VmConditions::recompiler_test_harness true. That harness runs the EE and the IOP in a
	/// single-thread interleave and boots no VM; the flag is how it keeps Lockstep out of its way.
	/// Only that environment sets it. It is deliberately not derived from PCSX2_RECOMPILER_TESTS:
	/// eerunner is built with that define and has to be able to run Lockstep.
	void SetRecompilerTestHarness(bool active);
	bool IsRecompilerTestHarness();

	/// Lockstep, called from the Lockstep instance of the EE event test (R5900.cpp), on the EE thread.
	/// LockstepActive() is false when the thread does not exist (a mode switch or shutdown in
	/// progress), and the event test then runs the IOP slice itself, as Off does.
	bool LockstepActive();
	/// Hands the IOP thread this event test's slice and waits for it.
	void RunLockstepSlice();

	/// The IOP asks the VM to pause: its recompiler met an opcode it cannot run, or its interpreter
	/// hit a breakpoint. SetPaused and ExitExecution belong to the CPU thread, so from the IOP thread
	/// the request is kept and made by the EE as soon as its handoff returns.
	void RequestPause();
} // namespace IopThread
