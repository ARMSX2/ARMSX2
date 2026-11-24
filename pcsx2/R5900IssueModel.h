// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// What the EE is charged for the code it runs: an in-order, two-issue model of
// the R5900 pipeline, with the pairing rules and result latencies the
// SCPH-30001 shows.
//
// The unit of charge is a path: the instructions from where control arrives
// (a branch target, the instruction after a delay slot, an exception vector,
// the return of an ERET, the first word of a 4 KB page) up to and including
// the next branch's delay slot (SYSCALL, BREAK and ERET also end a path). The
// model keeps a ready time for each register, the class of the instructions
// that issued in the current cycle, and when each unpipelined unit is free.
// A path costs the cycles it took to issue, plus a constant for how it left:
//
//   conditional branch taken, J, JAL       1
//   conditional branch not taken           0
//   branch-likely not taken                3 (its delay slot does not run)
//   JR, JALR                               2, or 5 right after a load or an
//                                            MFC0 of its register
//
// plus kLoadCharge per load. The charge is in whole cycles. Both engines call
// Step() for each instruction as they reach it and EndCost() where the path
// leaves, so they charge the same for the same path: the JIT at compile time,
// over a block, and the interpreter as it executes.
namespace R5900::Issue
{
	// Cycles per load beyond what the issue model gives, in hundredths. It
	// stands for the D-cache misses a static model cannot see. The k-th load of
	// a path adds round(k * L) - round((k - 1) * L) whole cycles.
	static constexpr u32 kLoadChargeHundredths = 84;

	enum class Exit : u8
	{
		None,           // the path ends without a branch: SYSCALL, BREAK, ERET, an exception, a page edge
		NotTaken,       // a conditional branch not taken; its delay slot ran
		Taken,          // a conditional branch taken, J or JAL
		LikelyNotTaken, // a branch-likely not taken; its delay slot did not run
		RegisterJump,   // JR or JALR
	};

	// Number of register slots the model tracks: GPR 0-31, FPR 32-63, VF 64-95,
	// HI/LO of both pipelines, the FPU accumulator and condition, the VU0
	// accumulator, SA, VI 0-15.
	static constexpr u32 kRegs = 120;
	static constexpr u32 kUnits = 5;

	struct State
	{
		u32 ready[kRegs];
		u32 unit_free[kUnits];
		u8 unit_order[kUnits]; // units in the order the path first used them
		u8 n_units;
		u32 t;          // the cycle being filled
		u8 n_in;        // instructions issued in cycle t (0, 1 or 2)
		u8 cls_in;      // class of the first of them
		u8 n_prev_writes;
		u8 prev_writes[6];
		u16 steps;      // instructions stepped on this path
		u16 loads;
		s16 prev_load_dst;   // register a load or MFC0 just wrote, or -1
		bool jr_after_load;  // the path's JR/JALR read the register written right before it
		bool branch_pending; // a branch was stepped and its delay slot was not
		bool complete;       // the path ended at a delay slot; the next Step starts a new one
		u32 branch_pc;       // the path's branch, once stepped
		u32 branch_code;
	};

	void Reset(State& s);

	// Whole cycles one instruction adds to the path, including its load
	// charge. Starts a new path first (charging the old one's tail) when the
	// old one ended at a not-taken branch's delay slot, or when `pc` is the
	// first word of a page and is not a delay slot.
	u32 Step(State& s, u32 pc, u32 code, u32 load_hundredths = kLoadChargeHundredths);

	// Whole cycles to charge where the path leaves: what an unpipelined unit
	// still owes (a divide issued near the end) and the exit's constant. Does
	// not change `s`; call Reset() to start the next path.
	u32 EndCost(const State& s, Exit exit);

	// The exit a conditional branch, jump or register jump takes to `target`,
	// from the branch Step() recorded. A non-likely branch whose target is its
	// own fall-through counts as taken whichever way it goes. Exit::None when
	// the path has no branch.
	Exit ExitFor(const State& s, u32 target);

	// The exit kind a branch-taking flow uses, independent of the target:
	// Taken for a conditional branch or J/JAL, RegisterJump for JR/JALR.
	Exit TakenExit(const State& s);

	// True when the instruction ends a path by itself: SYSCALL, BREAK, ERET.
	bool EndsPath(u32 code);

	// True when `code` has a delay slot (branches and jumps).
	bool IsBranch(u32 code);

	// The cost of a whole path, for tests and tools: Step() over `words`
	// (consecutive words from `pc`), then EndCost(exit).
	u32 PathCycles(u32 pc, const u32* words, u32 count, Exit exit,
		u32 load_hundredths = kLoadChargeHundredths);

	// The cycles of `words` alone: what Step() returns over them, plus the
	// tail, without an exit constant, and the number of loads. For tests.
	struct IssueResult
	{
		u32 cycles;
		u32 loads;
		bool jr_after_load;
	};
	IssueResult Issue(u32 pc, const u32* words, u32 count);

#ifdef PCSX2_RECOMPILER_TESTS
	// Test hook: every EE charge scaled by (1024 + this) / 1024, carried
	// exactly by both engines. 0 in every normal run.
	extern u32 g_testExtraPer1024;
#endif
} // namespace R5900::Issue
