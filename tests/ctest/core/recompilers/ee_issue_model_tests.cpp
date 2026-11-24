// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The EE issue model on its own: the console microbenchmark corpus against the
// reference implementation, and the rules one by one.

#include "harness/MipsEncode.h"

#include "R5900IssueModel.h"

#include "ee_issue_model_corpus.h"

#include <gtest/gtest.h>

#include <vector>

using namespace mips;
using namespace mips::ee;
using namespace R5900::Issue;

namespace {

constexpr u32 kPc = 0x00100000;

u32 Cost(std::initializer_list<u32> words, Exit exit, u32 load_hundredths = 0)
{
	const std::vector<u32> w(words);
	return PathCycles(kPc, w.data(), static_cast<u32>(w.size()), exit, load_hundredths);
}

constexpr u32 kSyncP = SYNC | (0x10u << 6);

} // namespace

// Every distinct path of the corpus costs what the reference gives it, at a
// load charge of 0 and at the shipped one.
TEST(EeIssueModel, ConsoleKernelCorpusMatchesTheReference)
{
	using namespace issue_corpus;
	const u32 n_stream = sizeof(kIssueCorpusStream) / sizeof(kIssueCorpusStream[0]);
	u32 at = 0;
	u32 paths = 0;
	u32 loads_seen = 0;
	while (at < n_stream)
	{
		const Exit exit = static_cast<Exit>(kIssueCorpusStream[at++]);
		const u32 want = kIssueCorpusStream[at++];
		const u32 loads = kIssueCorpusStream[at++];
		const bool jal = kIssueCorpusStream[at++] != 0;
		const u32 nseg = kIssueCorpusStream[at++];
		std::vector<u32> words;
		for (u32 s = 0; s < nseg; s++)
		{
			const u32 len = kIssueCorpusStream[at++];
			const u32 reps = kIssueCorpusStream[at++];
			for (u32 r = 0; r < reps; r++)
				for (u32 k = 0; k < len; k++)
					words.push_back(kIssueCorpusWords[kIssueCorpusStream[at + k]]);
			at += len;
		}
		const u32 n = static_cast<u32>(words.size());
		const u32 got = PathCycles(kPc, words.data(), n, exit, 0);
		const IssueResult r = Issue(kPc, words.data(), n);
		EXPECT_EQ(got, want) << "path " << paths << " first word " << std::hex << words[0];
		EXPECT_EQ(r.loads, loads) << "path " << paths;
		EXPECT_EQ(r.jr_after_load, jal) << "path " << paths;
		EXPECT_EQ(PathCycles(kPc, words.data(), n, exit), want + (loads * kLoadChargeHundredths + 50) / 100)
			<< "path " << paths;
		loads_seen += loads;
		paths++;
	}
	EXPECT_EQ(paths, kPaths);
	EXPECT_GT(loads_seen, 0u);
}

// Two independent integer ops issue in one cycle; a dependent pair takes two.
TEST(EeIssueModel, IndependentIntegerOpsPair)
{
	EXPECT_EQ(Cost({ADDIU(reg::t0, reg::zero, 1), ADDIU(reg::t1, reg::zero, 2)}, Exit::None), 1u);
	EXPECT_EQ(Cost({ADDIU(reg::t0, reg::zero, 1), ADDIU(reg::t1, reg::t0, 2)}, Exit::None), 2u);
	// Two writes of one register do not issue together.
	EXPECT_EQ(Cost({ADDIU(reg::t0, reg::zero, 1), ADDIU(reg::t0, reg::zero, 2)}, Exit::None), 2u);
	// An MMI op takes both integer pipelines.
	EXPECT_EQ(Cost({ADDIU(reg::t0, reg::zero, 1), PEXTLW(reg::t1, reg::t2, reg::t3)}, Exit::None), 2u);
}

// A load's result is ready two cycles after it issues; a load pairs with
// integer work but not with another load.
TEST(EeIssueModel, LoadLatencyAndPairing)
{
	EXPECT_EQ(Cost({LW(reg::t0, 0, reg::a0), ADDIU(reg::t1, reg::zero, 1)}, Exit::None), 1u);
	EXPECT_EQ(Cost({LW(reg::t0, 0, reg::a0), LW(reg::t1, 4, reg::a0)}, Exit::None), 2u);
	EXPECT_EQ(Cost({LW(reg::t0, 0, reg::a0), ADDIU(reg::t1, reg::t0, 1)}, Exit::None), 3u);
	// L per load, in whole cycles, rounded per path: 0.84, 1.68, 2.52.
	EXPECT_EQ(Cost({LW(reg::t0, 0, reg::a0)}, Exit::None, 84), 2u);
	EXPECT_EQ(Cost({LW(reg::t0, 0, reg::a0), LW(reg::t1, 4, reg::a0)}, Exit::None, 84), 4u);
	EXPECT_EQ(Cost({LW(reg::t0, 0, reg::a0), LW(reg::t1, 4, reg::a0), LW(reg::t2, 8, reg::a0)}, Exit::None, 84), 6u);
}

// The constant each exit adds, and the JR right after the load of its register.
TEST(EeIssueModel, ExitConstants)
{
	const u32 beq = BEQ(reg::t0, reg::t1, 4);
	EXPECT_EQ(Cost({beq, NOP}, Exit::NotTaken), 1u);
	EXPECT_EQ(Cost({beq, NOP}, Exit::Taken), 2u);
	EXPECT_EQ(Cost({BEQL(reg::t0, reg::t1, 4)}, Exit::LikelyNotTaken), 4u);
	EXPECT_EQ(Cost({J(0x00100100), NOP}, Exit::Taken), 2u);
	EXPECT_EQ(Cost({JR(reg::t0), NOP}, Exit::RegisterJump), 3u);
	EXPECT_EQ(Cost({LW(reg::t0, 0, reg::a0), JR(reg::t0), NOP}, Exit::RegisterJump), 3u + 2u + 3u);
	EXPECT_EQ(Cost({MFC0(reg::k0, 14), JR(reg::k0), NOP}, Exit::RegisterJump), 2u + 5u);
	// One instruction between: no extra.
	EXPECT_EQ(Cost({LW(reg::t0, 0, reg::a0), NOP, JR(reg::t0), NOP}, Exit::RegisterJump), 3u + 2u);
}

// What issues alone: SYNC.P 5, ERET 6, SYNC 1.
TEST(EeIssueModel, SoloCosts)
{
	EXPECT_EQ(Cost({kSyncP}, Exit::None), 5u);
	EXPECT_EQ(Cost({SYNC}, Exit::None), 1u);
	EXPECT_EQ(Cost({ERET}, Exit::None), 6u);
	EXPECT_TRUE(EndsPath(ERET));
	EXPECT_TRUE(EndsPath(SYSCALL_(0)));
	EXPECT_FALSE(EndsPath(kSyncP));
}

// A divide still running at the end of a path is charged there.
TEST(EeIssueModel, UnfinishedDivideIsChargedAtTheEnd)
{
	EXPECT_EQ(Cost({DIV(reg::t0, reg::t1)}, Exit::None), 37u);
	EXPECT_EQ(Cost({MULTU(reg::t0, reg::t1)}, Exit::None), 1u);
}

// The exit a branch takes to a target, from the branch the path stepped.
TEST(EeIssueModel, ExitForTarget)
{
	State s;
	Reset(s);
	Step(s, kPc, BNE(reg::t0, reg::t1, 4));
	Step(s, kPc + 4, NOP);
	EXPECT_EQ(ExitFor(s, kPc + 4 + 16), Exit::Taken);
	EXPECT_EQ(ExitFor(s, kPc + 8), Exit::NotTaken);

	// A branch to its own fall-through counts as taken either way.
	Reset(s);
	Step(s, kPc, BNE(reg::t0, reg::t1, 1));
	Step(s, kPc + 4, NOP);
	EXPECT_EQ(ExitFor(s, kPc + 8), Exit::Taken);

	// A likely branch that leaves before its delay slot was not taken.
	Reset(s);
	Step(s, kPc, BNEL(reg::t0, reg::t1, 4));
	EXPECT_EQ(ExitFor(s, kPc + 8), Exit::LikelyNotTaken);
	Step(s, kPc + 4, NOP);
	EXPECT_EQ(ExitFor(s, kPc + 4 + 16), Exit::Taken);

	Reset(s);
	Step(s, kPc, JR(reg::ra));
	Step(s, kPc + 4, NOP);
	EXPECT_EQ(ExitFor(s, 0), Exit::RegisterJump);
}

// A path that ends at a not-taken branch's delay slot is closed by the next
// Step, which starts a new path; so is one that reaches the first word of a
// page, unless that word is a delay slot.
TEST(EeIssueModel, NewPathAfterADelaySlotAndAtAPageEdge)
{
	State s;
	Reset(s);
	u32 c = 0;
	c += Step(s, kPc, ADDIU(reg::t0, reg::zero, 1));
	c += Step(s, kPc + 4, BNE(reg::t0, reg::t1, 4));
	c += Step(s, kPc + 8, NOP);
	EXPECT_TRUE(s.complete);
	// Would pair with the delay slot on one path; starts its own here.
	c += Step(s, kPc + 12, ADDIU(reg::t2, reg::zero, 1));
	EXPECT_FALSE(s.complete);
	EXPECT_EQ(s.steps, 1u);
	EXPECT_EQ(c, 2u + 1u);

	Reset(s);
	c = 0;
	c += Step(s, 0x00100FFC, ADDIU(reg::t0, reg::zero, 1));
	c += Step(s, 0x00101000, ADDIU(reg::t1, reg::zero, 1));
	EXPECT_EQ(c, 2u);
	EXPECT_EQ(s.steps, 1u);

	Reset(s);
	c = 0;
	c += Step(s, 0x00100FF8, ADDIU(reg::t0, reg::zero, 1));
	c += Step(s, 0x00100FFC, BNE(reg::t0, reg::t1, 4));
	c += Step(s, 0x00101000, NOP);
	EXPECT_EQ(s.steps, 3u);
}
