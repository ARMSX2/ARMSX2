// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Both engines charge the EE what the issue model (R5900IssueModel.h) gives
// each path: the recompiler at compile time over its blocks, the interpreter as
// it executes. They must agree with each other and with the model for every way
// a path can end, including the places where the recompiler ends a block
// without a branch.
//
// COP0 Count is read at the start of the program and at the start of a block
// after it, so the difference is what the paths between were charged. (Both
// reads are the first instruction of a block: the recompiler counts the read's
// own cycle into each, the interpreter into neither.)

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EECycleRate.h"
#include "R5900.h"
#include "R5900IssueModel.h"

#include <gtest/gtest.h>

#include <vector>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;
using R5900::Issue::Exit;

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;
constexpr u32 kCp0Status = 12;
constexpr u32 kCp0Epc = 14;
constexpr u32 kCp0Config = 16;
constexpr u32 kStatusCu0 = 1u << 28;
constexpr u32 kConfigDie = 1u << 18;
constexpr u32 kCount = 9;
constexpr u32 kRead0 = MFC0(reg::t0, kCount);
constexpr u32 kRead1 = MFC0(reg::t1, kCount);
constexpr u32 kFiller = ADDIU(reg::s7, reg::zero, 0x77); // never runs
constexpr u32 kVector = 0x80000180;

u32 At(u32 index) { return kProgPc + 4 * index; }

// A branch at `from` to `to` (program indices).
s16 Off(u32 from, u32 to) { return static_cast<s16>(static_cast<s32>(to) - static_cast<s32>(from) - 1); }

// One path of the program: the indices it runs, in order, and how it leaves.
struct PathSpec
{
	std::vector<u32> indices;
	Exit exit;
};

// What the model charges the paths, each costed from its own start.
u64 ModelCost(const std::vector<u32>& prog, const std::vector<PathSpec>& paths, u32 base_pc = kProgPc)
{
	u64 c = 0;
	for (const PathSpec& p : paths)
	{
		R5900::Issue::State s;
		R5900::Issue::Reset(s);
		for (u32 i : p.indices)
			c += R5900::Issue::Step(s, base_pc + 4 * i, prog[i]);
		c += R5900::Issue::EndCost(s, p.exit);
	}
	return c;
}

std::vector<u32> Range(u32 first, u32 last)
{
	std::vector<u32> v;
	for (u32 i = first; i <= last; i++)
		v.push_back(i);
	return v;
}

std::vector<u32> Cat(std::vector<u32> a, const std::vector<u32>& b)
{
	a.insert(a.end(), b.begin(), b.end());
	return a;
}

struct ScopedClean
{
	bool wait_loop;
	s8 rate;
	ScopedClean()
		: wait_loop(EmuConfig.Speedhacks.WaitLoop)
		, rate(EmuConfig.Speedhacks.EECycleRate)
	{
		EmuConfig.Speedhacks.WaitLoop = false;
		EmuConfig.Speedhacks.EECycleRate = 0;
		EECycleRate::SyncToConfigured();
	}
	~ScopedClean()
	{
		EmuConfig.Speedhacks.WaitLoop = wait_loop;
		EmuConfig.Speedhacks.EECycleRate = rate;
		EECycleRate::SyncToConfigured();
	}
};

void ExpectCharged(const EeRecTestHarness& h, u64 cycles)
{
	EXPECT_EQ(h.GetGprJit(reg::t1) - h.GetGprJit(reg::t0), cycles) << "jit";
	EXPECT_EQ(h.GetGprInterp(reg::t1) - h.GetGprInterp(reg::t0), cycles) << "interp";
}

void RunProgram(EeRecTestHarness& h, const std::vector<u32>& prog)
{
	h.SetCp0(kCp0Status, kStatusCu0);
	h.SetCp0(kCp0Config, kConfigDie);
	h.LoadProgram(prog);
	h.Run();
}

// The handler at the common vector steps EPC past the faulting instruction and
// returns; the environment's `jr ra; nop` stub is put back afterwards.
class KernelHandler
{
public:
	explicit KernelHandler(EeRecTestHarness& h, const std::vector<u32>& words)
		: h_(h)
		, size_(words.size())
	{
		for (size_t i = 0; i < words.size(); i++)
			h_.WriteU32(kVector + static_cast<u32>(i * 4), words[i]);
	}
	~KernelHandler()
	{
		h_.WriteU32(kVector + 0, JR(reg::ra));
		h_.WriteU32(kVector + 4, NOP);
		for (size_t i = 2; i < size_; i++)
			h_.WriteU32(kVector + static_cast<u32>(i * 4), NOP);
	}

private:
	EeRecTestHarness& h_;
	size_t size_;
};

} // namespace

// Independent integer ops pair, a load's result waits two cycles, the taken
// branch adds one.
TEST(EeRecIssueCharge, StraightLinePathAndATakenBranch)
{
	const ScopedClean clean;
	const std::vector<u32> prog = {
		kRead0,                          // 0
		ADDIU(reg::t2, reg::zero, 1),    // 1
		ADDIU(reg::t3, reg::zero, 2),    // 2
		LUI(reg::a0, kScratch >> 16),    // 3
		LW(reg::t4, 0, reg::a0),         // 4
		ADDU(reg::t5, reg::t4, reg::t2), // 5
		BEQ(reg::zero, reg::zero, Off(6, 9)),
		NOP,                             // 7
		kFiller,                         // 8
		kRead1,                          // 9
	};
	EeRecTestHarness h;
	RunProgram(h, prog);
	ExpectCharged(h, ModelCost(prog, {{Range(0, 7), Exit::Taken}}));
}

// A not-taken branch ends a path after its delay slot; the code after it starts
// a new one (the recompiler compiles it into the same block).
TEST(EeRecIssueCharge, NotTakenBranchThenTakenBranch)
{
	const ScopedClean clean;
	const std::vector<u32> prog = {
		kRead0,                               // 0
		ADDIU(reg::t2, reg::zero, 1),         // 1
		BEQ(reg::t2, reg::zero, Off(2, 8)),   // 2: not taken
		ADDIU(reg::t3, reg::t2, 1),           // 3
		ADDIU(reg::t4, reg::zero, 3),         // 4
		ADDIU(reg::t5, reg::t4, 4),           // 5
		BEQ(reg::zero, reg::zero, Off(6, 9)), // 6
		NOP,                                  // 7
		kFiller,                              // 8
		kRead1,                               // 9
	};
	EeRecTestHarness h;
	RunProgram(h, prog);
	ExpectCharged(h, ModelCost(prog, {{Range(0, 3), Exit::NotTaken}, {Range(4, 7), Exit::Taken}}));
}

// A branch-likely not taken skips its delay slot and adds three.
TEST(EeRecIssueCharge, BranchLikelyNotTaken)
{
	const ScopedClean clean;
	const std::vector<u32> prog = {
		kRead0,                               // 0
		ADDIU(reg::t2, reg::zero, 1),         // 1
		BEQL(reg::t2, reg::zero, Off(2, 8)),  // 2: not taken
		ADDIU(reg::t3, reg::zero, 9),         // 3: nullified
		ADDIU(reg::t4, reg::zero, 3),         // 4
		BEQ(reg::zero, reg::zero, Off(5, 8)), // 5
		NOP,                                  // 6
		kFiller,                              // 7
		kRead1,                               // 8
	};
	EeRecTestHarness h;
	RunProgram(h, prog);
	ExpectCharged(h, ModelCost(prog, {{Range(0, 2), Exit::LikelyNotTaken}, {Range(4, 6), Exit::Taken}}));
}

// J and JAL add one; JR and JALR add two, and three more right after a load of
// their register.
TEST(EeRecIssueCharge, JumpsAndRegisterJumps)
{
	const ScopedClean clean;
	const std::vector<u32> prog = {
		kRead0,                                     // 0
		J(At(4)),                                   // 1
		NOP,                                        // 2
		kFiller,                                    // 3
		LUI(reg::t9, At(9) >> 16),                  // 4
		ORI(reg::t9, reg::t9, At(9) & 0xFFFF),      // 5
		JR(reg::t9),                                // 6
		NOP,                                        // 7
		kFiller,                                    // 8
		LUI(reg::a0, kScratch >> 16),               // 9
		LW(reg::t9, 0, reg::a0),                    // 10
		JALR(reg::t8, reg::t9),                     // 11: right after the load of t9
		NOP,                                        // 12
		kFiller,                                    // 13
		JAL(At(17)),                                // 14
		NOP,                                        // 15
		kFiller,                                    // 16
		LUI(reg::ra, RecompilerTestEnvironment::kParkingPc >> 16), // 17
		BEQ(reg::zero, reg::zero, Off(18, 21)),     // 18
		NOP,                                        // 19
		kFiller,                                    // 20
		kRead1,                                     // 21
	};
	EeRecTestHarness h;
	h.WriteU32(kScratch, At(14));
	RunProgram(h, prog);
	ExpectCharged(h, ModelCost(prog, {
		{Range(0, 2), Exit::Taken},
		{Range(4, 7), Exit::RegisterJump},
		{Range(9, 12), Exit::RegisterJump},
		{Range(14, 15), Exit::Taken},
		{Range(17, 19), Exit::Taken},
	}));
}

// The first word of a page starts a new path in both engines; the recompiler
// ends its block there.
TEST(EeRecIssueCharge, PageEdgeEndsAPath)
{
	const ScopedClean clean;
	std::vector<u32> prog = {kRead0};
	while (prog.size() < 1024)
		prog.push_back(ADDIU(prog.size() & 1 ? reg::t2 : reg::t3, reg::zero, 1));
	prog.push_back(LW(reg::t4, 0, reg::zero));      // 1024: the next page
	prog.push_back(ADDU(reg::t5, reg::t4, reg::t2)); // 1025
	prog.push_back(BEQ(reg::zero, reg::zero, Off(1026, 1029)));
	prog.push_back(NOP);                             // 1027
	prog.push_back(kFiller);                         // 1028
	prog.push_back(kRead1);                          // 1029
	EeRecTestHarness h;
	RunProgram(h, prog);
	ExpectCharged(h, ModelCost(prog, {{Range(0, 1023), Exit::None}, {Range(1024, 1027), Exit::Taken}}));
}

// A trap that does not fire, and EI, end a path: the recompiler ends its block
// at each.
TEST(EeRecIssueCharge, TrapAndEiEndAPath)
{
	const ScopedClean clean;
	for (const u32 op : {TEQ(reg::t2, reg::zero), EI})
	{
		const std::vector<u32> prog = {
			kRead0,                               // 0
			ADDIU(reg::t2, reg::zero, 1),         // 1
			op,                                   // 2
			ADDIU(reg::t3, reg::t2, 1),           // 3
			BEQ(reg::zero, reg::zero, Off(4, 7)), // 4
			NOP,                                  // 5
			kFiller,                              // 6
			kRead1,                               // 7
		};
		EeRecTestHarness h;
		RunProgram(h, prog);
		ExpectCharged(h, ModelCost(prog, {{Range(0, 2), Exit::None}, {Range(3, 5), Exit::Taken}}));
	}
}

// A trap that fires in the middle of a path: the path ends there, taking the
// exception adds its entry, the handler is a path of its own and so is the
// code ERET returns to.
TEST(EeRecIssueCharge, ExceptionInTheMiddleOfAPath)
{
	const ScopedClean clean;
	const std::vector<u32> prog = {
		kRead0,                               // 0
		ADDIU(reg::t2, reg::zero, 1),         // 1
		ADDIU(reg::t3, reg::zero, 2),         // 2
		TEQ(reg::zero, reg::zero),            // 3: fires
		ADDIU(reg::t4, reg::t3, 1),           // 4
		BEQ(reg::zero, reg::zero, Off(5, 8)), // 5
		NOP,                                  // 6
		kFiller,                              // 7
		kRead1,                               // 8
	};
	const std::vector<u32> handler = {
		MFC0(reg::k0, kCp0Epc),
		ADDIU(reg::k0, reg::k0, 4),
		MTC0(reg::k0, kCp0Epc),
		ERET,
	};
	EeRecTestHarness h;
	KernelHandler kernel(h, handler);
	RunProgram(h, prog);
	const u64 entry = 9;
	ExpectCharged(h, ModelCost(prog, {{Range(0, 3), Exit::None}, {Range(4, 6), Exit::Taken}}) + entry +
		ModelCost(handler, {{Range(0, 3), Exit::None}}, kVector));
}

// The recompiler ends a block where another one already starts. Entered by
// falling through, the path goes on in the interpreter's view: the first block
// charges the difference.
TEST(EeRecIssueCharge, BlockCutAtAnExistingBlockFallsThroughAsOnePath)
{
	const ScopedClean clean;
	const std::vector<u32> prog = {
		kRead0,                               // 0
		ADDIU(reg::t6, reg::zero, 2),         // 1
		J(At(7)),                             // 2: compiles the block at 7 first
		NOP,                                  // 3
		kFiller,                              // 4
		// 5: MULT t2, a0, a1 (the R5900's three-operand form); its block ends at 7
		(reg::a0 << 21) | (reg::a1 << 16) | (reg::t2 << 11) | 0x18,
		ADDIU(reg::t3, reg::t3, 1),           // 6
		ADDIU(reg::t6, reg::t6, -1),          // 7
		ADDU(reg::t5, reg::t2, reg::t6),      // 8: waits for the product when entered from 6
		BNE(reg::t6, reg::zero, Off(9, 5)),   // 9: taken once, back to 5
		NOP,                                  // 10
		BEQ(reg::zero, reg::zero, Off(11, 14)), // 11: the interpreter adds its charge at a taken branch
		NOP,                                  // 12
		kFiller,                              // 13
		kRead1,                               // 14
	};
	EeRecTestHarness h;
	RunProgram(h, prog);
	ExpectCharged(h, ModelCost(prog, {
		{Range(0, 3), Exit::Taken},
		{Range(7, 10), Exit::Taken},
		{Range(5, 10), Exit::NotTaken},
		{Range(11, 12), Exit::Taken},
	}));
}

// A backward branch into the block being compiled splits it at the loop head;
// the code before the head falls into the loop as one path.
TEST(EeRecIssueCharge, LoopHeadSplitFallsThroughAsOnePath)
{
	const ScopedClean clean;
	const std::vector<u32> prog = {
		kRead0,                               // 0
		ADDIU(reg::t6, reg::zero, 3),         // 1
		LW(reg::t2, 0, reg::zero),            // 2
		ADDU(reg::t3, reg::t2, reg::t2),      // 3: the loop head; waits for the load on the first pass
		ADDIU(reg::t6, reg::t6, -1),          // 4
		BNE(reg::t6, reg::zero, Off(5, 3)),   // 5
		NOP,                                  // 6
		BEQ(reg::zero, reg::zero, Off(7, 10)), // 7
		NOP,                                  // 8
		kFiller,                              // 9
		kRead1,                               // 10
	};
	EeRecTestHarness h;
	RunProgram(h, prog);
	ExpectCharged(h, ModelCost(prog, {
		{Range(0, 6), Exit::Taken},
		{Range(3, 6), Exit::Taken},
		{Range(3, 6), Exit::NotTaken},
		{Range(7, 8), Exit::Taken},
	}));
}

// The test hook that scales every charge (used to ask whether a game still
// runs right if the EE were charged a few percent more) scales both engines
// alike, carrying the fraction from charge to charge.
TEST(EeRecIssueCharge, ChargeScalingHookScalesBothEngines)
{
	const ScopedClean clean;
	constexpr u32 kIterations = 200;
	const std::vector<u32> prog = {
		kRead0,                                    // 0
		ADDIU(reg::t6, reg::zero, kIterations),    // 1
		ADDIU(reg::t6, reg::t6, -1),               // 2: the loop
		ADDU(reg::t5, reg::t6, reg::t6),           // 3
		BNE(reg::t6, reg::zero, Off(4, 2)),        // 4
		NOP,                                       // 5
		BEQ(reg::zero, reg::zero, Off(6, 9)),      // 6
		NOP,                                       // 7
		kFiller,                                   // 8
		kRead1,                                    // 9
	};
	std::vector<PathSpec> paths = {{Range(0, 5), Exit::Taken}};
	for (u32 k = 1; k < kIterations - 1; k++)
		paths.push_back({Range(2, 5), Exit::Taken});
	paths.push_back({Range(2, 5), Exit::NotTaken});
	paths.push_back({Range(6, 7), Exit::Taken});
	const u64 base = ModelCost(prog, paths);

	R5900::Issue::g_testExtraPer1024 = 31;
	EeRecTestHarness h;
	RunProgram(h, prog);
	R5900::Issue::g_testExtraPer1024 = 0;
	const u64 jit = h.GetGprJit(reg::t1) - h.GetGprJit(reg::t0);
	const u64 interp = h.GetGprInterp(reg::t1) - h.GetGprInterp(reg::t0);
	const u64 want = base * (1024 + 31) / 1024;
	EXPECT_NEAR(static_cast<double>(jit), static_cast<double>(want), 2.0) << "base " << base;
	EXPECT_NEAR(static_cast<double>(interp), static_cast<double>(want), 2.0) << "base " << base;
}
