// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP recompiler's clock bookkeeping, pinned down by the values it must leave behind. The
// harness's own JIT-versus-interpreter diff ignores the clocks (the interpreter keeps them
// differently), so nothing else in the suite notices a block end that adds the wrong number of
// cycles, a timeslice that ends one block early, an event test that runs twice or not at all, or a
// device that is handed a stale clock.
//
// What the recompiler does at the end of every block, in order (iPsxBranchTest):
//
//     psxRegs.cycle += the block's cycles          (one per instruction, delay slot included)
//     psxRegs.iopCycleEE -= 8 * the block's cycles
//     if iopCycleEE <= 0: leave the slice, without the event test
//     if psxRegs.cycle >= psxRegs.iopNextEventCycle: iopEventTest()
//
// and psxRegs.cycle is only stored at a block end: a device read in the middle of a block sees the
// value the block started with. The expected numbers below were derived from that and are the
// values the recompiler produced before its clocks were kept in host registers.

#include "harness/JitTestHarness.h"

#include "IopCounters.h"
#include "IopMem.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kProg = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kPark = RecompilerTestEnvironment::kParkingPc;

struct Result
{
	s32 ret;
	u64 cycle;
	s32 iopCycleEE;
	s32 iopBreak;
	u64 next;
	u32 pc;
	u32 v0, v1, t5;
};

// Run `prog` (at kProgramPc, ending in whatever the caller wrote) from the given clocks with the given
// slice budget, the recompiler directly.
Result RunSlice(const std::vector<u32>& prog, u64 cycle, u64 next, s32 budget, u32 start_pc = kProg, bool counter3 = false,
	std::initializer_list<std::pair<u32, u32>> gpr = {})
{
	JitTestHarness h(JitTestHarness::Mode::JitOnly);
	h.LoadProgramAt(kProg, prog.data(), prog.size(), false);
	psxRec.Clear(kProg, static_cast<u32>(prog.size())); // the block cache must not hold a previous test's code
	psxRec.Clear(kPark, 2);

	psxCounter saved[NUM_COUNTERS];
	std::memcpy(saved, psxCounters, sizeof(saved));
	const s32 saved_delta = psxNextDeltaCounter;
	const u64 saved_start = psxNextStartCounter;
	if (counter3)
	{
		// counter 3 counts one per IOP cycle from zero at cycle zero: reading it returns the clock the
		// device was shown
		psxCounters[3].count = 0;
		psxCounters[3].rate = 1;
		psxCounters[3].startCycle = 0;
		psxCounters[3].mode.modeval = 0;
	}
	// no counter comes due inside any slice below
	psxNextStartCounter = 0;
	psxNextDeltaCounter = 0x40000000;

	psxRegs.cycle = cycle;
	psxRegs.iopNextEventCycle = next;
	psxRegs.pc = start_pc;
	psxRegs.GPR.n.ra = kPark;
	for (const auto& g : gpr)
		psxRegs.GPR.r[g.first] = g.second;

	Result r{};
	r.ret = psxRec.ExecuteBlock(budget);
	r.cycle = psxRegs.cycle;
	r.iopCycleEE = psxRegs.iopCycleEE;
	r.iopBreak = psxRegs.iopBreak;
	r.next = psxRegs.iopNextEventCycle;
	r.pc = psxRegs.pc;
	r.v0 = psxRegs.GPR.n.v0;
	r.v1 = psxRegs.GPR.n.v1;
	r.t5 = psxRegs.GPR.r[13];

	std::memcpy(psxCounters, saved, sizeof(saved));
	psxNextDeltaCounter = saved_delta;
	psxNextStartCounter = saved_start;
	return r;
}

#define EXPECT_RESULT(r, ret_, cycle_, ee_, next_, pc_) \
	do \
	{ \
		EXPECT_EQ((r).ret, (ret_)); \
		EXPECT_EQ((r).cycle, static_cast<u64>(cycle_)); \
		EXPECT_EQ((r).iopCycleEE, (ee_)); \
		EXPECT_EQ((r).next, static_cast<u64>(next_)); \
		EXPECT_EQ((r).pc, static_cast<u32>(pc_)); \
	} while (0)

} // namespace

// One block of six instructions ending in `jr ra`; the slice then lands on the idle loop at the parking
// address, which fast-forwards to the next event and then spends what is left of the budget.
TEST(IopCycleAccounting, ABlockChargesOneCyclePerInstructionAndEightEeCyclesPerCycle)
{
	// A budget of 40 EE cycles is five IOP cycles: the six-cycle block overruns it by one, so the
	// slice ends at the block end, before the event test, with the clocks advanced by that block.
	const std::vector<u32> prog = {ADDIU(reg::v0, reg::zero, 1), ADDIU(reg::v1, reg::zero, 2), ADDIU(reg::t0, reg::zero, 3),
		ADDIU(reg::t1, reg::zero, 4), JR(reg::ra), NOP};
	const Result r = RunSlice(prog, 1000, 1000000, 40);
	EXPECT_EQ(r.v0, 1u);
	EXPECT_EQ(r.v1, 2u);
	EXPECT_RESULT(r, 40 - 6 * 8, 1006, 40 - 6 * 8, 1000000, kPark);
}

TEST(IopCycleAccounting, TheSliceContinuesWhileBudgetRemains)
{
	// 200 EE cycles = 25 IOP cycles: the program block (6), then the idle loop's fast-forward, which
	// consumes the rest, 19 cycles, and ends the slice (nothing due, so no event test).
	const std::vector<u32> prog = {ADDIU(reg::v0, reg::zero, 1), ADDIU(reg::v1, reg::zero, 2), ADDIU(reg::t0, reg::zero, 3),
		ADDIU(reg::t1, reg::zero, 4), JR(reg::ra), NOP};
	const Result r = RunSlice(prog, 1000, 1000000, 200);
	// 200 - 48 = 152 left; fast-forward advances (152 + 7) >> 3 = 19 cycles and consumes 152
	EXPECT_RESULT(r, 0, 1006 + 19, 0, 1000000, kPark);
}

TEST(IopCycleAccounting, AnEventDueAtABlockEndRunsTheEventTestOnceAndReschedules)
{
	// The event is 4 cycles away; the six-cycle block ends past it. iopEventTest runs at that block end
	// and sets the next event 384 cycles after the clock it saw (iopWaitCycles), 1006 + 384. The idle loop
	// then fast-forwards the remaining budget in one step (no event comes inside it).
	const std::vector<u32> prog = {ADDIU(reg::v0, reg::zero, 1), ADDIU(reg::v1, reg::zero, 2), ADDIU(reg::t0, reg::zero, 3),
		ADDIU(reg::t1, reg::zero, 4), JR(reg::ra), NOP};
	const Result r = RunSlice(prog, 1000, 1004, 200);
	EXPECT_RESULT(r, 0, 1006 + 19, 0, 1006 + 384, kPark);
}

TEST(IopCycleAccounting, TheEventTestSeesTheClockOfTheBlockEnd)
{
	// Same program, event exactly at the block end (1006): due, because cycle >= next.
	const std::vector<u32> prog = {ADDIU(reg::v0, reg::zero, 1), ADDIU(reg::v1, reg::zero, 2), ADDIU(reg::t0, reg::zero, 3),
		ADDIU(reg::t1, reg::zero, 4), JR(reg::ra), NOP};
	const Result due = RunSlice(prog, 1000, 1006, 200);
	EXPECT_EQ(due.next, 1006u + 384u) << "event at the block-end clock is due";
	// One cycle later it is not due at the block end (1006 < 1007). The idle loop's fast-forward is clamped to the
	// event, 1007, which uses 8 of the 152 EE cycles left; the event test runs there and schedules 1007 + 384; the
	// loop goes round again and spends the remaining 144 on 18 cycles, ending the slice.
	const Result not_due = RunSlice(prog, 1000, 1007, 200);
	EXPECT_RESULT(not_due, 0, 1007 + 18, 0, 1007 + 384, kPark);
}

// A counted loop of three trips through a two-block shape (the loop body is one block of three
// instructions, charged each trip).
TEST(IopCycleAccounting, ALoopChargesEveryTrip)
{
	// addiu t5,zero,3 ; loop: addiu t5,t5,-1 ; bne t5,zero,loop ; nop ; jr ra ; nop
	const std::vector<u32> prog = {ADDIU(reg::t5, reg::zero, 3), ADDIU(reg::t5, reg::t5, -1), BNE(reg::t5, reg::zero, -2), NOP,
		JR(reg::ra), NOP};
	// block 1: addiu (1 instruction, falls into the loop block) ; loop block: 3 per trip (addiu, bne, delay slot) x3 ;
	// then jr ra block: 2. Budget big enough to finish: the program is 1 + 9 + 2 = 12 cycles.
	const Result r = RunSlice(prog, 5000, 1000000, 800);
	EXPECT_EQ(r.t5, 0u);
	// 800 EE = 100 cycles; program 12; fast-forward consumes the remaining 88 cycles worth: 704 EE
	EXPECT_RESULT(r, 0, 5000 + 12 + 88, 0, 1000000, kPark);
}

// A slice that ends mid-program leaves the clocks and the pc exactly at the end of the block that
// exhausted the budget, and a second slice carries on from there.
TEST(IopCycleAccounting, ASliceEndingMidProgramIsResumedExactly)
{
	const std::vector<u32> prog = {ADDIU(reg::t5, reg::zero, 3), ADDIU(reg::t5, reg::t5, -1), BNE(reg::t5, reg::zero, -2), NOP,
		JR(reg::ra), NOP};
	// 16 EE cycles = 2 IOP cycles: the first block (1 cycle) leaves one, the loop block (3 cycles) exhausts it.
	const Result r = RunSlice(prog, 5000, 1000000, 16);
	EXPECT_EQ(r.cycle, 5000u + 1 + 3);
	EXPECT_EQ(r.iopCycleEE, 16 - 8 * (1 + 3));
	EXPECT_EQ(r.ret, 16 - 8 * (1 + 3));
	EXPECT_EQ(r.t5, 2u);
}

// A device read in the middle of a block is shown the clock the block started with, and the next block
// the clock after the first: counter 3 counts one per cycle from zero, so the value read is the clock.
TEST(IopCycleAccounting, ADeviceReadSeesTheClockAtTheStartOfItsBlock)
{
	// block A (5 instructions): lui t1,0x1f80 ; addiu t0,zero,1 ; lw v0,0x1480(t1) ; beq zero,zero,+1 ; nop
	// block B: lw v1,0x1480(t1) ; jr ra ; nop    (reached by the branch over one instruction)
	const std::vector<u32> prog = {LUI(reg::t1, 0x1f80), ADDIU(reg::t0, reg::zero, 1), LW(reg::v0, 0x1480, reg::t1),
		BEQ(reg::zero, reg::zero, 1), NOP, NOP, LW(reg::v1, 0x1480, reg::t1), JR(reg::ra), NOP};
	const Result r = RunSlice(prog, 7000, 1000000, 400, kProg, /*counter3=*/true);
	EXPECT_EQ(r.v0, 7000u) << "the first read is in block A, which started at 7000";
	EXPECT_EQ(r.v1, 7000u + 5) << "the second read is in block B, which started after A's five cycles";
}

// The same with the device address held in a register, so the load site does not know at compile time that it
// is not RAM.
TEST(IopCycleAccounting, ADeviceReadWithARuntimeBaseSeesTheClockAtTheStartOfItsBlock)
{
	const std::vector<u32> prog = {ADDIU(reg::t0, reg::zero, 1), LW(reg::v0, 0x1480, reg::t1), BEQ(reg::zero, reg::zero, 1), NOP, NOP,
		LW(reg::v1, 0x1480, reg::t1), JR(reg::ra), NOP};
	const Result r = RunSlice(prog, 7000, 1000000, 400, kProg, true, {{reg::t1, 0x1f800000u}});
	EXPECT_EQ(r.v0, 7000u);
	EXPECT_EQ(r.v1, 7000u + 4);
}

// The same reads, with the clock past 2^32 and an event between the two blocks.
TEST(IopCycleAccounting, ADeviceReadAfterAnEventTestSeesTheReloadedClock)
{
	const std::vector<u32> prog = {LUI(reg::t1, 0x1f80), ADDIU(reg::t0, reg::zero, 1), LW(reg::v0, 0x1480, reg::t1),
		BEQ(reg::zero, reg::zero, 1), NOP, NOP, LW(reg::v1, 0x1480, reg::t1), JR(reg::ra), NOP};
	const u64 c0 = 0x100000000ull + 10;
	// the event is due at block A's end: iopEventTest runs, moves the next event to (c0 + 5) + 384, and block B's
	// read still sees the clock c0 + 5
	const Result r = RunSlice(prog, c0, c0 + 2, 400, kProg, true);
	EXPECT_EQ(r.v0, static_cast<u32>(c0));
	EXPECT_EQ(r.v1, static_cast<u32>(c0 + 5));
	EXPECT_GE(r.next, c0 + 5 + 384 - 1) << "the event test ran at block A's end";
}

// Entering the idle loop with the budget already spent, or negative (the IOP is ahead of the EE),
// still runs one fast-forward and leaves through the exit.
TEST(IopCycleAccounting, IdleLoopWithAnExhaustedBudget)
{
	const std::vector<u32> none = {NOP};
	for (const s32 budget : {1, 7, 8, 9, 0, -1, -9, -100})
	{
		SCOPED_TRACE(budget);
		const Result r = RunSlice(none, 3000, 1000000, budget, kPark);
		// advance = (budget + 7) >> 3 (an arithmetic shift, taken as an unsigned 32-bit value when added),
		// clamped to the event; the budget falls by 8 * the advance
		const u32 adv = static_cast<u32>(static_cast<s32>(static_cast<u32>(budget) + 7u) >> 3);
		u64 next_cycle = 3000ull + adv;
		if (next_cycle > 1000000)
			next_cycle = 1000000;
		const u32 consumed = static_cast<u32>(next_cycle - 3000) << 3;
		const s32 want_ee = static_cast<s32>(static_cast<u32>(budget) - consumed);
		EXPECT_EQ(r.cycle, next_cycle);
		EXPECT_EQ(r.iopCycleEE, want_ee);
	}
}
