// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The mark a fast-forward leaves for the event test it ends in (eeFastForwardKind, R5900.h).
//
// A WaitLoop block that spins on itself, and a timeout loop cut short by an event, jump the EE's clock to its next event and
// enter DispatcherEvent. The IOP thread's late-reply statistics need to know the event test was reached that way, because it
// means the guest was idle. The skip sets a bit and the event test takes it. The harness has no VM, so its event test returns
// before it takes the mark; these tests read it from the harness's event-test hook and clear it there, as the event test does.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "R5900.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

extern void (*g_eeHarnessEventTestHook)();

namespace {

constexpr u32 kPark = RecompilerTestEnvironment::kParkingPc;

struct ScopedWaitLoop
{
	bool prev;
	explicit ScopedWaitLoop(bool on) : prev(EmuConfig.Speedhacks.WaitLoop) { EmuConfig.Speedhacks.WaitLoop = on; }
	~ScopedWaitLoop() { EmuConfig.Speedhacks.WaitLoop = prev; }
};

u8 s_marks_seen;
u32 s_event_tests;

void TakeTheMark()
{
	// The harness's parking lot (`j self; nop`) is a wait loop of its own: whatever it leaves is not what the program under
	// test did.
	if (cpuRegs.pc != kPark)
	{
		s_marks_seen |= eeFastForwardKind;
		s_event_tests++;
	}
	eeFastForwardKind = 0;
}

// Zeroes the observations, installs the hook, and removes it again.
class MarkWatcher
{
public:
	MarkWatcher()
	{
		s_marks_seen = 0;
		s_event_tests = 0;
		eeFastForwardKind = 0;
		g_eeHarnessEventTestHook = TakeTheMark;
	}
	~MarkWatcher()
	{
		g_eeHarnessEventTestHook = nullptr;
		eeFastForwardKind = 0;
	}
};

} // namespace

// The self-loop with nothing in it: s_nBlockFF, so the loop's branch is the fast-forward.
TEST(EeRecFastForwardMark, AWaitLoopSkipMarksTheEventTestItEndsIn)
{
	ScopedWaitLoop wl(true);
	MarkWatcher watcher;
	EeRecTestHarness h;
	h.LoadProgramNoTerm({
		BEQ(reg::zero, reg::zero, -1), // +0x00 TOP -> TOP
		NOP,                           // +0x04 delay slot
	});
	h.RunJitNoDiff(); // spins to the cycle budget
	EXPECT_GT(s_event_tests, 0u);
	EXPECT_EQ(s_marks_seen, IopThread::FastForward::WaitLoop);
}

// The same program with the speedhack off is an ordinary self-loop: its event tests are reached by running the loop.
TEST(EeRecFastForwardMark, WithTheSpeedhackOffTheSameLoopLeavesNoMark)
{
	ScopedWaitLoop wl(false);
	MarkWatcher watcher;
	EeRecTestHarness h;
	h.LoadProgramNoTerm({
		BEQ(reg::zero, reg::zero, -1),
		NOP,
	});
	h.RunJitNoDiff();
	EXPECT_GT(s_event_tests, 0u);
	EXPECT_EQ(s_marks_seen, 0u);
}

// A loop that computes (a write after a read) is not a wait loop, so running into its event tests leaves no mark.
TEST(EeRecFastForwardMark, ACountingLoopLeavesNoMark)
{
	ScopedWaitLoop wl(true);
	MarkWatcher watcher;
	EeRecTestHarness h;
	h.LoadProgramNoTerm({
		ADDIU(reg::t0, reg::t0, 1),    // +0x00 TOP
		BEQ(reg::zero, reg::zero, -2), // +0x04 -> TOP
		NOP,                           // +0x08
	});
	h.RunJitNoDiff();
	EXPECT_GT(s_event_tests, 0u);
	EXPECT_EQ(s_marks_seen, 0u);
}

// A timeout loop whose counter outlasts the distance to the next event: the skip ends in the event, which is a fast-forward.
TEST(EeRecFastForwardMark, ATimeoutLoopCutShortByAnEventMarksTheEventTest)
{
	ScopedWaitLoop wl(true);
	MarkWatcher watcher;
	EeRecTestHarness h;
	h.SetGpr64(reg::t0, 0x10000000); // 2^28 iterations of 8 cycles: far past the next event
	h.LoadProgramNoTerm({
		ADDIU(reg::t0, reg::t0, -1),   // +0x00 TOP
		BNE(reg::t0, reg::zero, -2),   // +0x04 -> TOP
		NOP,                           // +0x08 delay slot
		J(kPark),                      // +0x0C
		NOP,                           // +0x10
	});
	h.RunJitNoDiff();
	EXPECT_GT(s_event_tests, 0u);
	EXPECT_EQ(s_marks_seen, IopThread::FastForward::TimeoutLoop);
}

// A timeout loop that runs out before any event is a loop the guest ran, not a wait an event ended: no mark.
TEST(EeRecFastForwardMark, ATimeoutLoopThatRunsOutLeavesNoMark)
{
	ScopedWaitLoop wl(true);
	MarkWatcher watcher;
	EeRecTestHarness h;
	h.SetGpr64(reg::t0, 3);
	h.LoadProgramNoTerm({
		ADDIU(reg::t0, reg::t0, -1),
		BNE(reg::t0, reg::zero, -2),
		NOP,
		J(kPark),
		NOP,
	});
	h.RunJitNoDiff();
	EXPECT_EQ(s_marks_seen, 0u);
	EXPECT_EQ(h.GetGpr64Jit(reg::t0), 0ull);
}
