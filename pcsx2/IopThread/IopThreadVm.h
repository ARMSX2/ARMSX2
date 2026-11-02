// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the VM does with the IOP thread: the one thread object, the mode in effect, and the calls the
// VM's lifecycle makes.
//
// In Lockstep the IOP thread runs the IOP's slice of every EE event test while the EE waits. Everything
// else still runs on the EE thread, which is legal because the IOP thread is parked whenever the EE runs.
// A hold (VmHold) is therefore a marker in Lockstep, but the lifecycle takes one at every site where
// Threaded needs a real one.
//
// In Threaded the IOP thread paces itself from the EE's watermark: the EE publishes its cycle at every
// event test, empties the IOP-to-EE queue and waits only if it is more than L ahead (W1). EE-to-IOP
// effects (blanking, an interrupt, once-a-frame jobs) are queued with a cycle stamp and applied by the IOP
// when its clock reaches it. IOP-to-EE effects (the SBUS interrupt, a pause) are queued for the EE to
// apply at its next event test. A SIF pump runs on the EE thread with the IOP stopped: the EE holds the
// IOP for the pumps it starts, and the IOP blocks in CallOnEE for the pumps it starts. While the thread
// runs the EE-side books (EEsCycle, EEoCycle) are not maintained: the IOP's position is the thread's, and
// the books are rewritten from it whenever the IOP is held or stopped, which is where the savestate and a
// mode change read them.
//
// Everything here is called on the CPU thread unless it says otherwise.

#pragma once

#include "IopThread/IopThread.h"
#include "IopThread/IopThreadMode.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace IopThread
{
	/// The one IOP thread object. It exists for the life of the process and runs only while the mode
	/// in effect is not Off.
	extern Thread g_thread;

	/// What this host looks like to ResolveMode: whether it is ARM64, its core count and how many of the
	/// cores are performance cores (VMManager's GetPerformanceCoresMask; 0 where the cores cannot be told apart).
	HostCpuInfo CurrentHostInfo();

	/// The IOP thread runs concurrently with the EE, so it is one more busy thread that needs a processor of its own
	/// (VMManager::SetEmuThreadAffinities counts it in the threads to place). False in Lockstep, whose two threads take
	/// strict turns, and when there is no thread.
	bool PlacesThread();

	/// Changes whenever the thread that exists changes: 0 with no thread, else the number of the start. VMManager
	/// places the thread when this differs from the value it last placed for. CPU thread only.
	u32 PlacementToken();

	/// The conditions about the running VM that rule the IOP thread out: PS1 mode, an arcade title,
	/// a GS dump replay, an IOP breakpoint or memcheck, and the recompiler test harness flag.
	VmConditions CurrentVmConditions();

	/// Resolves the mode from EmuConfig and the VM, and makes it the mode in effect: s_mode, and the
	/// interpreter's event test instance. It does not touch the thread. The EE JIT's dispatcher is
	/// generated for the mode in effect, so this runs before the recompilers are reset, and every
	/// caller resets them right after (VMManager::Initialize, and a settings change in
	/// CheckForCPUConfigChanges).
	void ChooseMode();
	/// The same with the host and the VM conditions given, for the tests.
	void ChooseModeFor(const HostCpuInfo& host, const VmConditions& vm);

	/// Makes the thread match the mode in effect: starts it when the mode is Lockstep or Threaded and it is
	/// not running, stops it when the mode is Off and it is. The IOP's clocks are taken from the EE's, so
	/// call it with the CPU state in place (after cpuReset). With a hold live the thread starts held.
	void SyncThread();

	/// The VM is going away: stops the thread, releases the pause hold and turns the mode Off.
	void StopThread();

	/// The CPU settings changed (VMManager::CheckForCPUConfigChanges): choose again, start or stop the
	/// thread to match, and give a running thread the new FPCR. Call it before the recompilers are
	/// reset.
	void OnCpuConfigChanged();

	/// A VM reset is about to reset the recompilers (VMManager::Reset). The reset puts the IOP back in
	/// PS2 mode, so the mode is chosen as if the PS1 flag in the IOP's registers were already clear: a
	/// VM that was demoted to Off by a PS1 game gets its IOP thread back. The caller
	/// starts the thread with SyncThread() once the CPU state has been reset.
	void ChooseModeForReset();

	/// The EE wrote F240 bit 19: the IOP is about to be reset into PS1 mode. PS1 mode forces Off (the
	/// IOP runs at a different clock ratio), so the IOP thread is stopped here and the mode stays Off
	/// until the next reset or boot. The caller holds the IOP (IopBridge::Ps1Switch), and
	/// the EE core is asked to regenerate its dispatcher, which still calls the event test instance of
	/// the old mode. Needs the CPU thread.
	void DemoteForPs1Switch();

	// The EE tells the IOP things at a cycle: blanking, an interrupt, a few once-a-frame jobs. The EE posts
	// an item to the E2I queue stamped with its cycle. Lockstep then hands the IOP thread "apply everything
	// up to this stamp" and waits, so the item takes effect at exactly the point of the EE's execution where
	// Off made the direct call. Threaded posts and goes on.

	namespace E2IKind
	{
		inline constexpr u16 HBlankStart = 0; // psxHBlankStart()
		inline constexpr u16 HBlankEnd = 1; // psxHBlankEnd()
		inline constexpr u16 VBlankStart = 2; // psxVBlankStart()
		inline constexpr u16 VBlankEnd = 3; // psxVBlankEnd()
		inline constexpr u16 IopIrq = 4; // iopIntcIrq(arg)
		inline constexpr u16 Call = 5; // the job whose number is arg (E2IJob)
		inline constexpr u16 SifPump = 6; // the pump of an EE-started SIF DMA, deferred (DeferSifPump); arg is the channel, 0 for SIF0 and 1 for SIF1
	} // namespace E2IKind

	/// The once-a-frame jobs the EE runs for IOP devices.
	namespace E2IJob
	{
		inline constexpr u32 AutoEjectTick = 0; // AutoEject::CountDownTicks()
		inline constexpr u32 MemcardBusyTick = 1; // MemcardBusy::Decrement()
		inline constexpr u32 VsyncPatchesIop = 2; // Patch::ApplyVsyncPatchesIOP()
	} // namespace E2IJob

	/// What each kind of item does on the IOP. The real table calls the IOP's functions; a test swaps
	/// in a table that records which thread ran what.
	struct E2IActions
	{
		void (*hblank_start)();
		void (*hblank_end)();
		void (*vblank_start)();
		void (*vblank_end)();
		void (*iop_irq)(u32 line);
		void (*job)(u32 job);
		/// Last, so a table written before the kind existed is still a valid initializer. A SifPump item finds
		/// it null only in a table that never posts one.
		void (*sif_pump)(u32 channel);
	};
	extern const E2IActions kRealE2IActions;

	/// Applies one item with `actions`. An unknown kind or job is a bug in the caller.
	void ApplyE2IItem(const E2IItem& item, const E2IActions& actions = kRealE2IActions);

	/// Replaces the table the IOP thread applies items with (nullptr: the real one). For tests; returns
	/// the previous table. Needs the thread stopped or the EE in a state where it is not applying.
	const E2IActions* SetE2IActionsForTests(const E2IActions* actions);

	/// Lockstep: the EE posts the item, stamped with its cycle, and waits until the IOP thread has
	/// applied it. With no IOP thread (a mode switch or shutdown in progress, or Off) the item is applied
	/// here, directly. EE thread only.
	void ApplyOnIop(u16 kind, u32 arg = 0);

	/// EE thread. The EE has started a SIF DMA (SIF0 or SIF1, `channel` 0 or 1) and is about to run its pump. If
	/// the IOP thread is to defer pumps, posts a SifPump item stamped with the EE's cycle and returns true: the
	/// caller must not run the pump. The IOP applies the item when its clock reaches the stamp and runs the pump
	/// through CallOnEE (RunSifPumpOnEe), so the EE blocks for nothing and the data moves at the IOP's time of the
	/// start. False when the caller should run the pump itself, holding the IOP as before: no IOP thread, the IOP
	/// already stopped (a hold, inside a CallOnEE function), or deferral not wanted.
	///
	/// Deferral is wanted in Threaded when EmuConfig.Speedhacks.iopThreadDeferSif is set, and in Lockstep when
	/// it is set too (the item is then applied at once, which is Off's order).
	bool DeferSifPump(u32 channel);

	/// EE thread. The EE is about to stop a SIF channel that is running: a write of STR=0 to D5_CHCR or D6_CHCR (`channel` 0 for SIF0, 1 for
	/// SIF1). The BIOS's sceSifSetDma does this at every call, to make room for its next transfer, and rebuilds the channel from the registers
	/// the stop leaves. A transfer in flight may be waiting for the IOP: the IOP has not yet re-armed its DMA 9 or 10 after the transfer before.
	/// On a console the IOP did that within microseconds, so by the time of the next call the transfer is done; an IOP that runs behind the EE
	/// has not got there, and the stop cuts the transfer in the middle of a block, saves the rest as a new tag, and cancels the completion
	/// interrupt. The IOP then gets a stream whose block boundaries are not where its tags say, and the BIOS's count of finished transfers is
	/// one short for good.
	///
	/// So the IOP is brought up to the EE's cycle first, as for a pump the EE starts (a Threaded hold, a Lockstep catch-up), and a transfer it
	/// then finishes completes before the stop. True when the IOP was brought up, and the caller should deliver a completion interrupt that
	/// the transfer scheduled meanwhile instead of letting the stop cancel it. False, and nothing done, in Off, with no IOP thread, with the
	/// IOP already stopped (a hold, a CallOnEE function), and in Lockstep without a skew or with that channel's holds off (the skew test arms
	/// that defer the pump).
	bool SyncIopForSifChannelStop(u32 channel);

	/// Any thread. For a host thread that calls into IOP state directly and cannot take a hold (the
	/// physical-disc thread's new-disc callback). With an IOP thread running, queues `command` for it and
	/// returns true: it runs on the IOP thread between slices (Lockstep: in the next handoff). Without one
	/// (Off, or a mode switch in progress) returns false and does not queue it, and the caller makes the
	/// call itself. A command accepted just as the thread stops runs on the IOP thread before it exits.
	bool TryPostToIop(HostCommandQueue::Command command);

	/// IOP-side code asks for something that only the CPU thread may do and that nothing waits for (pad and
	/// USB vibration). Off and Lockstep call `fn` here: in Off the caller is the CPU
	/// thread, and in Lockstep the CPU thread is blocked in the handoff, so the call is legal and keeps
	/// the order Off has. Threaded posts it with Host::RunOnCPUThread(fn, false), which never waits for
	/// the CPU thread, because the CPU thread may be spinning on the IOP.
	///
	/// One load of the mode and a branch that is not taken in Off. Call sites run at most a few times a
	/// frame (one per pad poll).
	void PostToCpuThread(std::function<void()> fn);

	template <class F>
	__forceinline_odr void CallOnCpuThread(F&& fn)
	{
		if (GetMode() == IopThreadMode::Threaded) [[unlikely]]
			PostToCpuThread(std::function<void()>(std::forward<F>(fn)));
		else
			fn();
	}

	// The Threaded instance of the event test calls these, on the EE thread.

	/// A pause the IOP asked for (its recompiler met an opcode it cannot run, or its interpreter hit a
	/// breakpoint). In Lockstep the handoff returns and makes it; in Threaded the I2E item sets the flag and
	/// the next event test makes it, because SetPaused takes a hold and must not run inside a hold's spin.
	extern std::atomic<bool> g_pause_requested;

	/// Empties the IOP-to-EE queue (the SBUS interrupt, a pause request, a SIF pump the IOP wants run) and
	/// makes a pause the IOP asked for. Event test only.
	void DrainEeQueue();

	/// What the event test does with a pause the IOP asked for: pauses the VM and leaves the EE's execution loop. A test
	/// that has no VM replaces it (nullptr puts the real one back); returns the previous one.
	using PauseAction = void (*)();
	PauseAction SetPauseActionForTests(PauseAction action);

	/// A Threaded thread that was started from inside an EE event test (a settings change delivered by the message pump at
	/// vsync) is kept stopped until the next event test, which is the Threaded instance's. The rest of the event test it was
	/// started in belongs to the instance of the mode before: in Off that calls IOP code directly (the blanking, the memory card
	/// tick), which must not run beside the new thread. EE thread only.
	extern bool g_start_hold_pending;
	void ReleaseStartHold();
	inline bool StartHoldPendingForTests() { return g_start_hold_pending; }

	/// First thing in the Threaded event test: tell the IOP how far the EE has got (a release store, and a
	/// wake-up if the IOP is parked and has budget) and take what the IOP queued for the EE.
	__forceinline_odr void EeEventTestBegin(u64 cycle)
	{
		if (g_start_hold_pending) [[unlikely]]
			ReleaseStartHold();
		g_thread.PublishWatermark(cycle);
		if (g_thread.MayHaveI2E() || g_pause_requested.load(std::memory_order_relaxed)) [[unlikely]]
			DrainEeQueue();
	}

	/// W1, the lag bound: waits until the EE is at most L ahead of the IOP. False when no IOP thread runs
	/// (a mode switch or a shutdown in progress): the event test then runs the IOP's slice itself, as Off does.
	__forceinline_odr bool EeLagWait(u64 cycle)
	{
		if (!g_thread.IsRunning()) [[unlikely]]
			return false;
		g_thread.CheckLag(cycle);
		return true;
	}

	/// The near bound: called by the EE before it reads or writes an SBUS mailbox word (HwRead.cpp, HwWrite.cpp).
	/// In Threaded with a lag bound above kDefaultLag it waits until the IOP is within kDefaultLag of `cycle`, so
	/// the mailbox handshakes (an IOP reboot makes many; gameplay makes none) see the skew they were tested at.
	/// One load of the mode otherwise.
	__forceinline_odr void EeSbusNearBound(u64 cycle)
	{
		const IopThreadMode mode = GetMode();
		if (mode == IopThreadMode::Off) [[likely]]
			return;
		if (!g_thread.IsRunning()) [[unlikely]]
			return;
		// An IOP reboot handshakes through these words; the lag rules hold the bound tight for a while after one.
		g_thread.NoteEeMailboxAccess(cycle);
		if (mode == IopThreadMode::Threaded)
			g_thread.CheckNearLag(cycle);
	}

	/// EE thread. The EE started a SIF DMA on `channel` (0: it armed SIF0 to receive, 1: it sent on SIF1), before its pump runs. A request
	/// went out, so the lag rules hold the bound tight until the reply (LagRule::Window, IdleOutstanding). A load of the mode in Off.
	void EeNoteSifStart(u32 channel);

	/// EE thread. The EE read the status of D5 or D6 (CHCR) with STR set: it polls for the end of a SIF transfer, which may be waiting
	/// for the IOP (LagRule::Poll).
	void EeNoteSifPoll();

	/// The SIF0 pump the IOP starts: the data and the completion reach the EE. Counts as the IOP's reply for the lag rules, then pumps.
	/// Runs on the EE thread (Threaded: through CallOnEE).
	void Sif0PumpFromIop();

	/// A SIF pump that the IOP starts. The pump moves data on both sides and schedules both sides' completion
	/// events, so it runs on the EE thread with the IOP stopped: the IOP posts it with CallOnEE and blocks until the
	/// EE has run it (at its next event test, or sooner inside any EE wait). On the EE thread it just runs.
	void RunSifPumpOnEe(void (*pump)());

	// ---- Testing knobs (eerunner) ---------------------------------------------------------------
	/// The minimum Threaded slice size (S_min) for the next Start(); 0 by default. Not a setting.
	void SetThreadedMinSlice(s32 min_slice);
	s32 ThreadedMinSlice();
	/// Lockstep, for testing only (eerunner --iop-lockstep-skew; not a setting). The IOP is kept `cycles` EE cycles behind
	/// the EE at every handoff instead of being brought up to it, E2I items are applied when the IOP's clock reaches their
	/// stamp, and EE-started SIF0/SIF1 pumps are deferred as with iopThreadDeferSif. Every run of a state with the same
	/// skew is the same run. 0, the default, is Lockstep as before, bit for bit. Takes effect at the next Start() of a Lockstep
	/// thread; clamped to 0..kMaxLag.
	///
	/// What stays direct: the EE's reads and writes of IOP address space and its SIF2 pumps still touch the IOP where it
	/// is, D behind, and the SBUS words are live. The mode is for states whose play makes none of them. Do not take or load
	/// a savestate, or change settings, during a skewed run: the items still queued for the IOP are not part of a state.
	void SetLockstepSkew(s64 cycles);
	s64 LockstepSkewSetting();
	/// Lockstep skew, for testing only (eerunner --iop-lockstep-skew-holds; not a setting). With a skew above 0 the EE-started SIF0/SIF1
	/// pumps are not deferred but HELD, as a Threaded hold does them: the EE runs the IOP up to its own cycle (applying the items due on the
	/// way), runs the combined pump at once, and the skew then re-grows as the EE runs on (the IOP does not run again until the EE is a
	/// skew ahead of it). So the pump reads and writes EE memory at the EE's cycle of the start, as on a console, and the IOP side of it
	/// happens at the EE's time and not a skew late. Does nothing with a skew of 0. Takes effect at once.
	void SetLockstepSkewHolds(bool holds);
	/// The same per channel: bit 0 holds SIF0 pumps, bit 1 SIF1 pumps; the channel without its bit is deferred as before. 3 is
	/// SetLockstepSkewHolds(true). eerunner --iop-lockstep-skew-holds-sif0 / -sif1. For finding which channel's deferral matters.
	void SetLockstepSkewHoldChannels(u32 mask);
	bool LockstepSkewHolds();
	/// The skew the running Lockstep thread has, 0 otherwise. What the EE core's event test reads.
	inline s32 LockstepSkewCycles() { return static_cast<s32>(g_thread.LockstepSkewCurrent()); }

	/// Keep a record of every frame's late replies (IOPLATE-FRAME: lines in eerunner), for the next Start(). Not a setting.
	void SetReplyTrace(bool on);
	bool ReplyTrace();

	/// IOP thread, Lockstep, with the EE blocked in the handoff: the IOP has an effect on the EE directly, where Threaded queues an
	/// item (the SBUS interrupt, a SIF pump the IOP started). Counts it as a reply of `kind` (ReplyKind) at the IOP's time now. Does
	/// nothing in any other mode.
	void NoteDirectReply(u32 kind);

	/// The idle-watch window for the next Start(): how long an idle IOP watches the watermark before it sleeps
	/// (Params::idle_spin_ns; default kDefaultIdleSpinNs). Not a setting.
	void SetThreadedIdleSpinNs(u32 ns);
	u32 ThreadedIdleSpinNs();

	/// The late-reply books as text, for the run reports: a total line, a line per kind for every reply and for the overshoots in idle (count, mean,
	/// max, and the histogram of lateness), the idle share of the late replies in tenths, and the frames by overshoots in idle.
	/// Each line starts with IOPLATE:.
	std::vector<std::string> FormatReplyStats(const ReplyStats& stats);
	/// One IOPLATE-FRAME: line per completed frame, when the trace is on.
	std::vector<std::string> FormatReplyTrace(const std::vector<FrameRecord>& trace);

	/// One line of the Threaded counters, per frame over the time since the last call, for the OSD. Empty unless
	/// the mode is Threaded.
	std::string OsdStatsLine();
	/// The same line for a total over `frames` frames, for the run reports.
	std::string FormatStats(const Stats& totals, double frames);

	/// The EE's cycle counter and the IOP's slice books were reset or loaded (cpuReset, a savestate
	/// load): move the thread's view of the IOP's position to match. Needs the IOP held or stopped.
	void ResyncClocks();

	/// The VM paused or resumed (VMManager::SetState). A paused VM keeps a hold for as long as it
	/// stays paused, so the debugger and the settings see a still IOP.
	void OnVmPaused(bool paused);

	/// A request from the IOP thread to pause the VM, waiting for the EE. RunLockstepSlice() makes it
	/// as soon as the handoff returns; the two functions are for the tests and for that.
	bool DeferredPausePending();
	void ClearDeferredPause();

	/// A full stop of the IOP for the length of a scope: the EE thread may touch IOP state inside it.
	/// A marker in Lockstep and with no thread. See ScopedHold for the rules.
	class VmHold
	{
	public:
		explicit VmHold(HoldKind kind = HoldKind::Lifecycle)
			: m_hold(g_thread, kind)
		{
		}

	private:
		ScopedHold m_hold;
	};

	/// Devel assert for the sites that reset the recompilers or load state: the IOP must be held or
	/// not running. A marker hold counts.
	void AssertHeldOrStopped();

	/// The thread object's counters (handoffs and so on), for the run reports.
	Stats GetStats();
} // namespace IopThread
