// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the EE charges for its divide instructions, in both engines.
//
// Every guest instruction is charged what it adds to its path under the issue
// model (R5900IssueModel.h). A divide occupies its unit for the time the
// console takes, and a path that ends with the unit still busy is charged until
// it is free. The interpreter adds the charge per instruction and the
// recompiler sums it per block and bakes the scaled sum into the block tail,
// so a divide's charge reaches cpuRegs.cycle through two different paths. This
// file pins the charge per divide opcode as both engines apply it: at every EE
// cycle-rate selector, with dual issue on and off, and for the FPU divides at
// every FPU clamp mode (the recompiler has two FPU emitters, iFPU and iFPUd,
// and the charge must not depend on which one runs).
//
// A change to one of these numbers is a timing change for every game that
// divides, not a test update.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EECycleRate.h"
#include "R5900.h"
#include "R5900IssueModel.h"
#include "R5900OpcodeTables.h"

#include <gtest/gtest.h>

#include <string>

// Test-only scaler driver (iR5900-arm64.cpp): the recompiler's charge for a
// block of raw_block_cycles at the effective selector.
extern u32 recEeScaleBlockCyclesForTest(u32 raw_block_cycles, u32* out_remainder);

using namespace recompiler_tests;
using namespace mips;

namespace {

// CP0 Config bit 18 (DIE, dual issue enable). The BIOS sets it; with it clear
// both engines charge every instruction twice. The harness zeroes Config.
constexpr u32 kConfigDie = 1u << 18;

struct DivideOp
{
	const char* name;
	u32 code;
	u32 cycles; // how long the divide holds its unit
	bool fpu;
};

// The time per divide opcode, in cycles.
const DivideOp kOps[] = {
	{"DIV",     DIV(reg::a0, reg::a1),         37, false},
	{"DIVU",    DIVU(reg::a0, reg::a1),        37, false},
	{"DIV1",    ee::DIV1(reg::a0, reg::a1),    37, false},
	{"DIVU1",   ee::DIVU1(reg::a0, reg::a1),   37, false},
	{"DIV_S",   ee::DIV_S(3, 1, 2),            7,  true},
	{"SQRT_S",  ee::SQRT_S(3, 2),              7,  true},
	{"RSQRT_S", ee::RSQRT_S(3, 1, 2),          13, true},
	{"PDIVW",   ee::PDIVW(reg::a0, reg::a1),   37, false},
	{"PDIVUW",  ee::PDIVUW(reg::a0, reg::a1),  37, false},
	{"PDIVBW",  ee::PDIVBW(reg::a0, reg::a1),  37, false},
};

// The model's cost, in eighths with dual issue on, of `words` plus the JR $ra
// and NOP the harness appends to every program: one path that leaves by the JR.
u32 ModelEighths(const std::vector<u32>& words)
{
	std::vector<u32> w = words;
	w.push_back(JR(reg::ra));
	w.push_back(NOP);
	return 8 * R5900::Issue::PathCycles(RecompilerTestEnvironment::kProgramPc, w.data(), static_cast<u32>(w.size()),
		R5900::Issue::Exit::RegisterJump);
}

// Sets the configured selector (and with it the effective one) for a scope.
struct ScopedCycleRate
{
	s8 prev;
	explicit ScopedCycleRate(int selector)
		: prev(EmuConfig.Speedhacks.EECycleRate)
	{
		EmuConfig.Speedhacks.EECycleRate = static_cast<s8>(selector);
		EECycleRate::SyncToConfigured();
	}
	~ScopedCycleRate()
	{
		EmuConfig.Speedhacks.EECycleRate = prev;
		EECycleRate::SyncToConfigured();
	}
};

// The test block: SYNCs, then `copies` of the divide, so the block's raw
// charge (with the harness's JR + NOP tail) is a multiple of 32. That makes the
// interpreter's charge independent of the sub-cycle remainder it carries over
// from earlier runs at every selector from 0 up (the scaler drops at most 5
// low bits there), so both engines can be held to the same exact number. A
// SYNC issues alone and adds one cycle in front of the divides.
struct Block
{
	std::vector<u32> words;
	u32 raw; // with dual issue on
};

Block MakeBlock(const DivideOp& op, int copies)
{
	for (u32 syncs = 0; syncs < 8; syncs++)
	{
		Block b;
		b.words.assign(syncs, ee::SYNC);
		for (int i = 0; i < copies; i++)
			b.words.push_back(op.code);
		b.raw = ModelEighths(b.words);
		if (b.raw % 32 == 0)
			return b;
	}
	ADD_FAILURE() << op.name << ": no count of SYNCs makes the block a multiple of 32";
	return {};
}

// The FPU clamp modes, numbered as the GameDB's eeClampMode.
enum class Fpu
{
	NoOverflow = 0,
	Default = 1,
	ExtraOverflow = 2,
	Full = 3,  // the iFPUd emitters
	Exact = 4, // the iFPUd emitters
};

struct Charged
{
	u64 jit;
	u64 interp;
};

// The interpreter's wait-loop speedhack charges a branch whose target repeats
// the previous branch's target twice: once to decide whether the loop is idle,
// then again in doBranch, where the empty remainder still costs the scaler's
// one-cycle minimum. Running the same program twice is exactly that case, so
// the speedhack is off here; it has nothing to do with the divides.
struct ScopedWaitLoopOff
{
	bool prev;
	ScopedWaitLoopOff() : prev(EmuConfig.Speedhacks.WaitLoop) { EmuConfig.Speedhacks.WaitLoop = false; }
	~ScopedWaitLoopOff() { EmuConfig.Speedhacks.WaitLoop = prev; }
};

// Runs the block twice under identical settings and returns the second run's
// charges. The first run leaves the interpreter's carried remainder below the
// selector's mask, so the second is exact (see MakeBlock).
Charged RunBlock(const Block& b, bool die, Fpu fpu = Fpu::Default)
{
	const ScopedWaitLoopOff no_wait_loop;
	Charged c = {};
	for (int pass = 0; pass < 2; pass++)
	{
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetCp0(16, die ? kConfigDie : 0u);
		h.SetGpr64(reg::a0, 1000);
		h.SetGpr64(reg::a1, 7);
		// Exact quotients and roots, so every clamp mode and both engines
		// agree on the value and the run is only about the charge.
		h.SetFprBits(1, 0x40800000u); // 4.0
		h.SetFprBits(2, 0x40800000u); // 4.0
		switch (fpu)
		{
			case Fpu::Default: break;
			case Fpu::NoOverflow: h.DisableFpuOverflow(); break;
			case Fpu::ExtraOverflow: h.EnableFpuExtraOverflow(); break;
			case Fpu::Full: h.EnableFpuFullMode(); break;
			case Fpu::Exact: h.EnableFpuExactMode(); break;
		}
		h.LoadProgram(b.words);
		// Modes 3 and 4 have no interpreter counterpart (it has no double
		// path), so they run the recompiler alone.
		if (fpu == Fpu::Full || fpu == Fpu::Exact)
			h.RunJitNoDiff();
		else
			h.Run();
		c.jit = h.GetCycleDeltaJit();
		c.interp = (fpu == Fpu::Full || fpu == Fpu::Exact) ? c.jit : h.GetCycleDeltaInterp();
	}
	return c;
}

} // namespace

// A divide alone costs its unit's time.
TEST(EeRecDivideCharge, ModelChargesEachDivideAsMeasured)
{
	for (const DivideOp& op : kOps)
	{
		EXPECT_STREQ(R5900::GetInstruction(op.code).Name, op.name);
		EXPECT_EQ(R5900::Issue::PathCycles(0, &op.code, 1, R5900::Issue::Exit::None), op.cycles) << op.name;
	}
}

// Both engines charge exactly the model for one and for two of each divide,
// at the default selector with dual issue on (what a game runs).
TEST(EeRecDivideCharge, BothEnginesChargeTheModel)
{
	ScopedCycleRate rate(0);
	for (const DivideOp& op : kOps)
	{
		for (int copies = 1; copies <= 2; copies++)
		{
			const Block b = MakeBlock(op, copies);
			const Charged c = RunBlock(b, true);
			EXPECT_EQ(c.jit, b.raw / 8) << op.name << " x" << copies << " (jit)";
			EXPECT_EQ(c.interp, b.raw / 8) << op.name << " x" << copies << " (interp)";
		}
	}
}

// Two divides cost exactly one divide more than one divide: the second waits
// for the unit, with nothing shared between back-to-back divides.
TEST(EeRecDivideCharge, EachExtraDivideAddsItsWholeCharge)
{
	ScopedCycleRate rate(0);
	for (const DivideOp& op : kOps)
	{
		// Equal padding on both: a NOP count that makes the 2-copy block a
		// multiple of 32 also leaves the 1-copy block a multiple of 8, since
		// every divide charge is a whole number of cycles.
		const Block b2 = MakeBlock(op, 2);
		Block b1 = b2;
		b1.words.pop_back();
		b1.raw = ModelEighths(b1.words);
		ASSERT_EQ(b2.raw - b1.raw, 8 * op.cycles) << op.name;
		const Charged c1 = RunBlock(b1, true);
		const Charged c2 = RunBlock(b2, true);
		EXPECT_EQ(c2.jit - c1.jit, op.cycles) << op.name << " (jit)";
		EXPECT_EQ(c2.interp - c1.interp, op.cycles) << op.name << " (interp)";
	}
}

// With dual issue off, every charge doubles, the divides included.
TEST(EeRecDivideCharge, SingleIssueDoublesTheCharge)
{
	ScopedCycleRate rate(0);
	for (const DivideOp& op : kOps)
	{
		const Block b = MakeBlock(op, 1);
		const Charged c = RunBlock(b, false);
		EXPECT_EQ(c.jit, 2 * b.raw / 8) << op.name << " (jit)";
		EXPECT_EQ(c.interp, 2 * b.raw / 8) << op.name << " (interp)";
	}
}

// The cycle-rate selectors scale a block that holds a divide like any other
// block: the recompiler's charge is the production scaler applied to the raw
// sum, and the interpreter agrees. ee_rec_cycle_rate_tests.cpp pins the scaler
// itself; this pins that the divide's model charge is what it scales.
//
// Below selector 0 the interpreter's charge also depends on the sub-cycle
// remainder it carries from earlier blocks (up to 7 eighths, which the scaler
// multiplies by less than one), so there it is held to the range that
// remainder allows; the recompiler, which starts every block at zero, is
// exact everywhere.
TEST(EeRecDivideCharge, EveryCycleRateSelectorScalesTheDivideCharge)
{
	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		for (const DivideOp& op : kOps)
		{
			const Block b = MakeBlock(op, 1);
			const u32 expect = recEeScaleBlockCyclesForTest(b.raw, nullptr);
			const Charged c = RunBlock(b, true);
			EXPECT_EQ(c.jit, expect) << op.name << " selector " << selector << " (jit)";
			if (selector >= 0)
			{
				EXPECT_EQ(c.interp, expect) << op.name << " selector " << selector << " (interp)";
			}
			else
			{
				EXPECT_GE(c.interp, expect) << op.name << " selector " << selector << " (interp)";
				EXPECT_LE(c.interp, recEeScaleBlockCyclesForTest(b.raw + 7, nullptr))
					<< op.name << " selector " << selector << " (interp)";
			}
		}
	}
}

// The FPU divides are charged the same at every FPU clamp mode. The charge is
// taken from the model before the per-opcode emitter runs, so neither the
// clamp mode nor the choice between iFPU and iFPUd (modes 3 and 4) can move it.
TEST(EeRecDivideCharge, FpuDivideChargeIsTheSameAtEveryClampMode)
{
	ScopedCycleRate rate(0);
	const Fpu modes[] = {Fpu::NoOverflow, Fpu::Default, Fpu::ExtraOverflow, Fpu::Full, Fpu::Exact};
	for (const DivideOp& op : kOps)
	{
		if (!op.fpu)
			continue;
		const Block b = MakeBlock(op, 2);
		for (const Fpu mode : modes)
		{
			const Charged c = RunBlock(b, true, mode);
			EXPECT_EQ(c.jit, b.raw / 8) << op.name << " clamp mode " << static_cast<int>(mode) << " (jit)";
			EXPECT_EQ(c.interp, b.raw / 8) << op.name << " clamp mode " << static_cast<int>(mode) << " (interp)";
		}
	}
}
