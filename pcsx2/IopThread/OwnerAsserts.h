// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The ownership asserts of the IOP thread design (section 6.3). Each side of the machine has entry
// points that the other side's thread must not call unless it has stopped the owner first:
//
//   IOP_SIDE_ASSERT()  at an entry into IOP-owned state (the IOP's registers, RAM, hardware registers,
//                      SPU2, CDVD, SIO2, DMA channels, the IOP half of SIF). It passes when
//                        - the IOP thread mode is Off, or no IOP thread is running, or
//                        - the caller is the IOP thread, or
//                        - the caller is the EE thread with a ScopedHold live (the IOP is stopped).
//   BLOCKING_HOST_CALL_ASSERT(block)
//                      at the top of each frontend's Host::RunOnCPUThread (design 8.12). A blocking call
//                      from the IOP thread waits for the CPU thread, which in Lockstep is blocked in the
//                      handoff and in Threaded may be spinning on the IOP: it never answers. It passes
//                      unless `block` is set and the caller is the IOP thread.
//   EE_SIDE_ASSERT()   at an entry into EE-owned state (the interrupt controllers, the EE event
//                      queue, the EE half of SIF). It passes when the mode is not Threaded, or the
//                      caller is not the IOP thread. In Lockstep the IOP thread is allowed to call
//                      into the EE directly, because the EE is blocked in the handoff.
//
// In Lockstep the two threads never run at once, so an assert cannot catch a data race there. What it
// catches is an EE-thread call into IOP state that nothing queued and nothing wrapped in a hold, which
// is a crossing that Threaded would turn into a race. Every violation is a missed row of design
// section 7.
//
// Debug and Devel builds only. Release builds compile both to nothing.
//
// The cost in Off is one load of IopThread::s_mode and a branch that is never taken (IOP_SIDE_ASSERT)
// or a load and a compare (EE_SIDE_ASSERT). That is acceptable at an entry point that runs at most a
// few thousand times a frame. It is not acceptable at one that runs per IOP memory access or per EE
// event, which is why those carry no assert (see the notes in the step 9 record).

#pragma once

#include "IopThread/IopThreadMode.h"

#include "common/Pcsx2Defs.h"

namespace IopThread
{
	/// Which side's entry point was entered from the wrong side.
	enum class Side : u8
	{
		Iop, // IOP_SIDE_ASSERT: an IOP entry called from the EE thread with no hold
		Ee, // EE_SIDE_ASSERT: an EE entry called from the IOP thread in Threaded mode
		BlockingHostCall, // BLOCKING_HOST_CALL_ASSERT: Host::RunOnCPUThread(fn, true) from the IOP thread
	};

	/// Called for every violation. The default logs it and fails an assert (pxFailRel); eerunner can
	/// swap in a handler that only counts, so one run lists every site.
	using ViolationHandler = void (*)(Side side, const char* where);
	ViolationHandler SetViolationHandler(ViolationHandler handler);

	/// The handler that logs the violation and stops with an assertion failure.
	void FailOnViolation(Side side, const char* where);
	/// A handler that logs the first violation at each site and goes on, for runs that want the full list.
	void LogViolation(Side side, const char* where);

	/// How many violations the checks have found since the last reset, whatever the handler did about them.
	u64 ViolationCount();
	void ResetViolationCount();

	// The predicates behind the macros, public so the tests can ask them in any state.

	/// True when an IOP-side entry may be called by this thread now (see the top of this file).
	bool IopSideAllowed();
	/// True when an EE-side entry may be called by this thread now.
	bool EeSideAllowed();
	/// True when this thread may make a blocking call to the CPU thread: any thread but the IOP thread.
	bool BlockingHostCallAllowed();

	/// The macros' out-of-line halves. They check, and report to the handler when the check fails.
	void CheckIopSide(const char* where);
	void CheckEeSide(const char* where);
	void CheckBlockingHostCall(const char* where);
} // namespace IopThread

#if defined(PCSX2_DEBUG) || defined(PCSX2_DEVBUILD)

#define IOP_SIDE_ASSERT() \
	do \
	{ \
		if (IopThread::s_mode != IopThreadMode::Off) [[unlikely]] \
			IopThread::CheckIopSide(__func__); \
	} while (0)

#define EE_SIDE_ASSERT() \
	do \
	{ \
		if (IopThread::s_mode == IopThreadMode::Threaded) [[unlikely]] \
			IopThread::CheckEeSide(__func__); \
	} while (0)

// Not in Off, where there is no IOP thread to be on: the same load of the mode as the other two. The call it
// guards waits for another thread, so its cost is nothing next to the call.
#define BLOCKING_HOST_CALL_ASSERT(block) \
	do \
	{ \
		if ((block) && IopThread::s_mode != IopThreadMode::Off) [[unlikely]] \
			IopThread::CheckBlockingHostCall(__func__); \
	} while (0)

#else

#define IOP_SIDE_ASSERT() ((void)0)
#define EE_SIDE_ASSERT() ((void)0)
#define BLOCKING_HOST_CALL_ASSERT(block) ((void)0)

#endif
