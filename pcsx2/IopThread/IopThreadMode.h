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
	// it waits. All the values below are in EE cycles.
	//
	// kDefaultLag is eight times kEeWaitCycles. A larger L lets the EE run further ahead of the
	// IOP, which lowered the EE's time per frame on a big core; the gain flattened above 12288
	// in some titles. It is the tight bound: the lag the IOP thread object starts with when a
	// caller names none, the bound the lag rules (IopLagRules.h) fall back to while the EE waits on
	// the IOP, and the bound of the SBUS mailbox accesses. It is not the default of the iopThreadLag
	// setting; that is kDefaultSettingLag.
	//
	// The minimum has to be strictly greater than kEeWaitCycles. The IOP only runs up to the cycle
	// the EE published at its previous event test, so at the next one the EE is already about
	// kEeWaitCycles ahead of it. A bound at or below that finds the IOP behind on nearly every
	// check. The extra 1024 cycles cover the measured overshoot: an event test lands past its
	// target by 81 cycles at the 90th percentile, and the IOP ends its last block up to 57 cycles
	// past what it was given. 1024 is more than seven times the sum of those.
	//
	// kOneFrameLag is one NTSC frame (294,912,000 EE cycles a second at 60 Hz is 4,915,200 cycles).
	// kDefaultSettingLag, what the iopThreadLag setting starts at, is that: a whole frame of slack
	// lets the IOP thread emulate the idle time the EE skips almost instantly while the EE does its
	// next busy stretch, and the lag rules pull the bound back to kDefaultLag at the moments when
	// the EE is waiting on the IOP, which is where a bound that large costs.
	//
	// The maximum, 3200 times kEeWaitCycles, is two NTSC frames. The cycle-stamped EE-to-IOP queue
	// holds kE2IRingCapacity entries, which is about three times the posts of a maximum window (about
	// 527 posts per frame, 1052 on OutRun, plus the SIF items), so it cannot fill within one window. The
	// largest delay the EE can see an IOP effect after is the lag plus one event-test spacing: about
	// 33 ms at the maximum.
	inline constexpr s32 kDefaultLag = 8 * kEeWaitCycles;
	inline constexpr s32 kMinLag = kEeWaitCycles + 1024;
	inline constexpr s32 kOneFrameLag = 1600 * kEeWaitCycles;
	inline constexpr s32 kDefaultSettingLag = kOneFrameLag;
	inline constexpr s32 kMaxLag = 3200 * kEeWaitCycles;

	static_assert(kMinLag > kEeWaitCycles);
	static_assert(kMinLag <= kDefaultLag && kDefaultLag <= kMaxLag);
	static_assert(kDefaultLag <= kDefaultSettingLag && kDefaultSettingLag <= kMaxLag, "the setting starts no tighter than the tight bound");
	static_assert(kOneFrameLag == 4915200, "one NTSC frame of EE cycles");
	static_assert(kMaxLag % kEeWaitCycles == 0, "the maximum is a whole number of event-test spacings");
	// The lag is kept as an s32 (Params::lag, the EE-side books). Leave room for the cycle differences added to it.
	static_assert(kMaxLag > 0 && kMaxLag < INT32_MAX / 64, "the lag bound must leave s32 headroom");

	/// What jumped the EE's clock to its next event because the guest was idle: the bits of eeFastForwardKind (R5900.h).
	namespace FastForward
	{
		inline constexpr u8 WaitLoop = 1 << 0; // recompiler: a block that spins on itself with nothing to change
		inline constexpr u8 TimeoutLoop = 1 << 1; // recompiler: a counted delay loop, cut short by the event
		inline constexpr u8 Interpreter = 1 << 2; // interpreter: a spin on one branch target
	} // namespace FastForward

	/// The legal value nearest to `requested`. A value from a settings file, a per-game override
	/// or a command line goes through this before anything uses it as L.
	s32 ClampLag(s32 requested);

	/// The fewest performance-tier cores a host needs for ResolveMode to give the Threaded mode. The EE and GS
	/// threads are the two that are busy all the time, and they are the ones that should sit on fast cores.
	inline constexpr u32 kMinPerformanceCores = 2;

	/// The most performance-tier cores a host can have for the IOP thread to be requested by default. With two or three
	/// fast cores EE, GS and VU1 (each 85 to 95% busy) already compete for them, and the IOP thread's work moves to an
	/// otherwise idle slow core: measured on the MQ65 with two A73 online, +3 to +6% speed with MTVU on and +7 to +15% with
	/// it off in God of War II, Shadow of the Colossus and Burnout 3. With four fast cores those games are GS-bound and the
	/// thread gained nothing (measured while it still ran on a fast core), so there it stays a choice (and the game database's, for titles that
	/// are EE-bound). Three fast cores was not measured; it is included by decision.
	inline constexpr u32 kMaxDefaultOnPerformanceCores = 3;

	/// What ResolveMode needs to know about the host. A test describes a device with it; the
	/// emulator fills it from the CPU topology.
	struct HostCpuInfo
	{
		bool is_arm64 = false; // both modes are ARM64-only: the x86 IOP JIT is not audited for a second thread
		u32 total_cores = 0;
		/// Cores of at least 0.75 of the fastest core's speed (GetPerformanceCoresMask() in
		/// VMManager.cpp). On a part whose cores are all the same speed that is every core. 0 only when no
		/// core's speed can be read, and on hosts with no capacity source (everything but Linux and Android).
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
	/// Without the lockstep flag it returns Threaded, unless the host has too few cores or too few fast ones.
	/// The threads are the EE, the GS, the IOP and, with MTVU on, the VU1 thread. It needs at least that
	/// many cores in total plus two more for the OS, the GS back thread and the audio callback, and a
	/// performance tier of at least kMinPerformanceCores cores (a tier that cannot be read counts as none).
	/// The tier may be narrower than the threads: the IOP thread runs outside it, on the
	/// slower cores, in any case (SlowCoresMask in IopPlacement.h).
	IopThreadMode ResolveMode(const Pcsx2Config& config, const HostCpuInfo& host, const VmConditions& vm);

	/// Whether the iopThread setting should start on for this host: its performance tier holds between
	/// kMinPerformanceCores and kMaxDefaultOnPerformanceCores cores, and ResolveMode would give Threaded with the setting
	/// on (`mtvu` is the host's MTVU setting, which counts one more thread). False on a host ResolveMode can never run
	/// the thread on, and on four or more fast cores. This is the one place the rule is written: the settings defaults
	/// (SetHardwareDependentDefaultSettings, the Android app through NativeApp) and the one-time move of an existing
	/// install ask it, and the tests describe devices to it.
	bool DefaultOnForHost(const HostCpuInfo& host, bool mtvu);

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
