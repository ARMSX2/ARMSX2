// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// EE cycle-rate selector: what the seven settings actually charge, and which
// of the two selectors (configured vs effective) the emitted code is built from.
//
// The charge formula is written out twice by hand — once in the EE interpreter
// (intUpdateCPUCycles) and once in the AArch64 EE recompiler
// (scaleblockcycles_calculation / _clear) — and the recompiler bakes its answer
// into the block tail as an immediate. Nothing pinned either copy before this
// file, so a transcription slip in one of them was a silent timing change.
//
// The table below is a characterization: it records what the two engines do
// today, including the parts that look arbitrary (the manually tuned 80/168
// window at rate -1, the 40-cycle floor under which no scaling happens at all,
// the wider remainder mask at +2/+3). Changing a number here means changing
// guest timing for every game, so a diff in this file should never be waved
// through as "just updating the test".
//
// The second half is the point of the configured/effective split: a block
// compiled while the governor holds the effective selector at -1 must be
// byte-identical to one compiled with -1 configured outright. If it is not,
// some site still reads EmuConfig instead of EECycleRate::GetEffective().

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EECycleRate.h"
#include "R5900.h"
#include "VUmicro.h"

#include <gtest/gtest.h>

#include <vector>

// Test-only scaler drivers (iR5900-arm64.cpp / Interpreter.cpp). Each sets the
// engine's own raw block-cycle accumulator, runs the production scaler, and
// returns the charge plus the remainder carried to the next block.
extern u32 recEeScaleBlockCyclesForTest(u32 raw_block_cycles, u32* out_remainder);
extern u32 intScaleBlockCyclesForTest(u32 raw_block_cycles, u32* out_remainder);
extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);

// Emit-time census of the EE recompiler's cycle-charge sites (iR5900-arm64.cpp).
// All four clear on a full recompiler reset.
extern u32 recEeGetChargeSiteCount();
extern u32 recEeGetFormChangeSiteCount();
extern u32 recEeGetFormChangeBlockCount();

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kProgramPc = RecompilerTestEnvironment::kProgramPc;

// Sets the configured selector (and, with it, the effective one) for the
// duration of a test, restoring both on the way out.
struct ScopedCycleRate
{
	s8 prev_configured;

	explicit ScopedCycleRate(int configured)
		: prev_configured(EmuConfig.Speedhacks.EECycleRate)
	{
		EmuConfig.Speedhacks.EECycleRate = static_cast<s8>(configured);
		EECycleRate::SyncToConfigured();
	}

	// Restores the configured selector and re-syncs the effective one, which is
	// what a settings reapply does — so a test that moved the effective value
	// off the baseline cannot leak it into the next test.
	~ScopedCycleRate()
	{
		EmuConfig.Speedhacks.EECycleRate = prev_configured;
		EECycleRate::SyncToConfigured();
	}
};

struct Expect
{
	u32 charge;
	u32 remainder;
};

struct CycleRateCase
{
	u32    raw;
	Expect by_selector[7]; // index 0 == selector -3 ... index 6 == selector +3
};

// charge / retained-remainder for each raw 3-bit-fixed-point block cost.
//
//   raw <= 40      no scaling at any selector: charge = raw >> 3.
//   selector -1    5/32 of raw, except in the manually tuned (80, 168] window
//                  where it is 7/32 — the 80->81 and 168->169 rows are that
//                  boundary.
//   selector -2/-3 7/32 and 9/32 of raw.
//   selector +1    (raw >> 3) / 1.3, truncated.
//   selector +2/+3 raw >> 4 and raw >> 5.
//   every selector charges at least 1.
//   the remainder is raw & 7, except at +2 (raw & 15) and +3 (raw & 31) for a
//   block that was eligible for scaling at all (raw > 40).
const CycleRateCase kCases[] = {
	{    0, {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}}},
	{    1, {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
	{    7, {{1, 7}, {1, 7}, {1, 7}, {1, 7}, {1, 7}, {1, 7}, {1, 7}}},
	{    8, {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}}},
	{   40, {{5, 0}, {5, 0}, {5, 0}, {5, 0}, {5, 0}, {5, 0}, {5, 0}}},
	{   41, {{11, 1}, {8, 1}, {6, 1}, {5, 1}, {3, 1}, {2, 9}, {1, 9}}},
	{   79, {{22, 7}, {17, 7}, {12, 7}, {9, 7}, {6, 7}, {4, 15}, {2, 15}}},
	{   80, {{22, 0}, {17, 0}, {12, 0}, {10, 0}, {7, 0}, {5, 0}, {2, 16}}},
	{   81, {{22, 1}, {17, 1}, {17, 1}, {10, 1}, {7, 1}, {5, 1}, {2, 17}}},
	{  168, {{47, 0}, {36, 0}, {36, 0}, {21, 0}, {16, 0}, {10, 8}, {5, 8}}},
	{  169, {{47, 1}, {36, 1}, {26, 1}, {21, 1}, {16, 1}, {10, 9}, {5, 9}}},
	{  320, {{90, 0}, {70, 0}, {50, 0}, {40, 0}, {30, 0}, {20, 0}, {10, 0}}},
	{ 1023, {{287, 7}, {223, 7}, {159, 7}, {127, 7}, {97, 7}, {63, 15}, {31, 31}}},
	{ 4096, {{1152, 0}, {896, 0}, {640, 0}, {512, 0}, {393, 0}, {256, 0}, {128, 0}}},
	{65535, {{18431, 7}, {14335, 7}, {10239, 7}, {8191, 7}, {6300, 7}, {4095, 15}, {2047, 31}}},
};

// A block long enough to clear the 40-cycle no-scaling floor, so its charge
// really does depend on the selector.
std::vector<u32> LongBlock()
{
	std::vector<u32> prog;
	for (int i = 0; i < 32; i++)
		prog.push_back(ADDIU(reg::a1, reg::a1, 1));
	return prog;
}

// A block deep enough that its charge outgrows ADD's 12-bit immediate two
// steps below the configured rate.
//
// The arithmetic: underclocking multiplies the charge by up to 2.25x (rate 0
// charges raw/8, rate -3 charges 9*raw/32), so a site well inside imm12 at the
// configured rate can leave it inside the governor's window. At configured 0
// the window bottoms out at -2, which charges 7*raw/32; that first stops being
// encodable at raw 18730 (see the boundary asserted in the test below). PDIVW
// costs 176 raw units (Cycles::MMI_Div in R5900OpcodeTables.cpp), doubled again
// when CP0 Config bit 18 is clear, so 128 of them clear the boundary either
// way — 22528 raw at worst.
std::vector<u32> DeepDivideBlock()
{
	std::vector<u32> prog;
	for (int i = 0; i < 128; i++)
		prog.push_back(ee::PDIVW(reg::a1, reg::a2));
	return prog;
}

// Compile LongBlock() from an empty code cache and return the emitted host
// bytes of the block at kProgramPc. The full recompiler reset is what makes the
// bytes comparable across calls: it rewinds the emit pointer, so the same
// program lands at the same host address and any difference is real.
std::vector<u8> CaptureBlockBytes(int configured, int effective)
{
	EmuConfig.Speedhacks.EECycleRate = static_cast<s8>(configured);
	EECycleRate::SyncToConfigured();
	if (effective != configured)
		EXPECT_TRUE(EECycleRate::ApplyEffective(static_cast<s8>(effective), "ee_rec_cycle_rate_tests"));

	recCpu.Reset();

	EeRecTestHarness h;
	h.SetGpr64(reg::a1, 0);
	h.LoadProgram(LongBlock());
	h.Run();

	uptr fnptr = 0, lut = 0;
	u32  host_size = 0;
	EXPECT_TRUE(recEeBlockHostInfo(kProgramPc, &fnptr, &host_size, &lut));
	if (!fnptr || !host_size)
		return {};

	const u8* p = reinterpret_cast<const u8*>(fnptr);
	return std::vector<u8>(p, p + host_size);
}

} // namespace

// The interpreter and the recompiler each hold their own copy of the formula.
// The charge itself always agrees, or a game would run at one speed under the
// JIT and another under the interpreter.
TEST(EeRecCycleRate, InterpreterAndRecompilerChargeTheSame)
{
	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		for (const CycleRateCase& c : kCases)
		{
			u32 rec_rem = 0, int_rem = 0;
			const u32 rec_charge = recEeScaleBlockCyclesForTest(c.raw, &rec_rem);
			const u32 int_charge = intScaleBlockCyclesForTest(c.raw, &int_rem);
			EXPECT_EQ(rec_charge, int_charge)
				<< "selector " << selector << ", raw " << c.raw << ": charge";
			(void)rec_rem;
			(void)int_rem;
		}
	}
}

// The retained remainder does NOT always agree, and this pins where it doesn't.
//
// Both recompilers (arm64 scaleblockcycles_clear, and x86's in
// ix86-32/iR5900.cpp) mask the carried remainder with the wide +2/+3 mask only
// for a block that was eligible for scaling at all:
//
//     if (!lowcycles && cyclerate > 1)   s_nBlockCycles &= (1 << (cyclerate+2)) - 1;
//     else                               s_nBlockCycles &= 0x7;
//
// The EE interpreter (intUpdateCPUCycles, shared upstream code) has no
// `!lowcycles` term, so for a block of 40 raw cycles or fewer at selector +2 or
// +3 it carries 4 or 5 bits where the recompilers carry 3. The two engines then
// drift apart by a fraction of a cycle per short block.
//
// This is an upstream divergence, not an ARMSX2 transcription slip, and it is
// confined to the two overclock selectors. It is recorded rather than fixed:
// changing either side changes guest timing, which is a separate decision from
// the configured/effective split this file exists for. If it is fixed, this
// test is what will say so.
TEST(EeRecCycleRate, InterpreterCarriesAWiderRemainderOnShortOverclockedBlocks)
{
	const u32 raw = 25; // <= 40, so no scaling; low bits set past bit 2

	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		u32 rec_rem = 0, int_rem = 0;
		recEeScaleBlockCyclesForTest(raw, &rec_rem);
		intScaleBlockCyclesForTest(raw, &int_rem);

		EXPECT_EQ(rec_rem, raw & 0x7u) << "selector " << selector;
		if (selector > 1)
		{
			EXPECT_EQ(int_rem, raw & ((1u << (selector + 2)) - 1u))
				<< "selector " << selector << ": interpreter no longer carries the wide remainder";
			EXPECT_NE(int_rem, rec_rem) << "selector " << selector;
		}
		else
		{
			EXPECT_EQ(int_rem, rec_rem) << "selector " << selector;
		}
	}
}

// The table itself. A change here is a change to every game's timing.
TEST(EeRecCycleRate, CharacterizesTheSevenSelectors)
{
	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		const int idx = selector + 3;
		for (const CycleRateCase& c : kCases)
		{
			u32 rem = 0;
			const u32 charge = recEeScaleBlockCyclesForTest(c.raw, &rem);
			EXPECT_EQ(charge, c.by_selector[idx].charge)
				<< "selector " << selector << ", raw " << c.raw << ": charge";
			EXPECT_EQ(rem, c.by_selector[idx].remainder)
				<< "selector " << selector << ", raw " << c.raw << ": remainder";
			EXPECT_GE(charge, 1u) << "selector " << selector << ", raw " << c.raw;
		}
	}
}

// The remainder is what carries the fractional part of a block's cost into the
// next block, so its width is part of the timing model, not an implementation
// detail: 3 bits everywhere except +2 (4 bits) and +3 (5 bits), and 3 bits for
// any block short enough to skip scaling entirely.
TEST(EeRecCycleRate, RemainderMaskWidthPerSelector)
{
	const u32 all_ones = 0x3F; // 6 bits set, > 40 so scaling is eligible
	const u32 expected_mask[7] = {0x7, 0x7, 0x7, 0x7, 0x7, 0xF, 0x1F};

	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		u32 rem = 0;
		recEeScaleBlockCyclesForTest(all_ones, &rem);
		EXPECT_EQ(rem, all_ones & expected_mask[selector + 3]) << "selector " << selector;

		// A block at or under the 40-cycle floor keeps the narrow mask whatever
		// the selector says.
		u32 low_rem = 0;
		recEeScaleBlockCyclesForTest(39, &low_rem);
		EXPECT_EQ(low_rem, 39u & 0x7u) << "selector " << selector << " (low-cycle block)";
	}
}

// The whole point of the split: the emitted charge follows the effective
// selector, not the configured one.
TEST(EeRecCycleRate, EmittedCodeFollowsTheEffectiveSelector)
{
	const s8 saved_configured = EmuConfig.Speedhacks.EECycleRate;

	// The first compile of this program in the process is not comparable with
	// the rest: the page holding it is still write-protected, so the block gets
	// no manual-protection prologue. Loading the program a second time flips the
	// page to ProtMode_Manual and every compile from then on carries the
	// page-compare prologue. Discard the cold one and compare warmed ones.
	CaptureBlockBytes(0, 0);
	const std::vector<u8> rate0_a = CaptureBlockBytes(0, 0);
	const std::vector<u8> rate0_b = CaptureBlockBytes(0, 0);
	const std::vector<u8> ratem1  = CaptureBlockBytes(-1, -1);
	const std::vector<u8> ratem1b = CaptureBlockBytes(-1, -1);
	const std::vector<u8> effm1   = CaptureBlockBytes(0, -1);
	const std::vector<u8> rate0_c = CaptureBlockBytes(0, 0);
	EXPECT_EQ(ratem1, ratem1b);
	EXPECT_EQ(rate0_a, rate0_c);

	EmuConfig.Speedhacks.EECycleRate = saved_configured;
	EECycleRate::SyncToConfigured();

	ASSERT_FALSE(rate0_a.empty());

	// Control: recompiling the same program from an empty cache is byte-stable,
	// so a difference below means something real.
	ASSERT_EQ(rate0_a, rate0_b) << "block emission is not deterministic; the rest of this test proves nothing";

	// The selector reaches emitted code at all.
	ASSERT_NE(rate0_a, ratem1);

	// Configured 0 + effective -1 emits exactly what configured -1 emits.
	EXPECT_EQ(effm1, ratem1);
}

// Bounds. The governor may go two steps down from the user's baseline and no
// further, and may never go up past it.
TEST(EeRecCycleRate, ApplyEffectiveHonoursTheBounds)
{
	ScopedCycleRate rate(0);

	R5900cpu* saved_cpu = Cpu;
	BaseVUmicroCPU* saved_vu0 = CpuVU0;
	Cpu = &intCpu;
	CpuVU0 = &CpuIntVU0;

	EXPECT_EQ(EECycleRate::GetConfigured(), 0);
	EXPECT_EQ(EECycleRate::GetFloor(), -2);

	const u32 before = EECycleRate::GetTransitionCount();

	// Above the baseline: refused.
	EXPECT_FALSE(EECycleRate::ApplyEffective(1, "test"));
	EXPECT_EQ(EECycleRate::GetEffective(), 0);

	// Below the two-step floor: refused.
	EXPECT_FALSE(EECycleRate::ApplyEffective(-3, "test"));
	EXPECT_EQ(EECycleRate::GetEffective(), 0);

	EXPECT_EQ(EECycleRate::GetTransitionCount(), before) << "a rejected request is not a transition";

	// Inside the window: taken.
	EXPECT_TRUE(EECycleRate::ApplyEffective(-2, "test"));
	EXPECT_EQ(EECycleRate::GetEffective(), -2);
	EXPECT_EQ(EECycleRate::GetTransitionCount(), before + 1);

	// Asking for what is already set is a no-op, not a transition.
	EXPECT_TRUE(EECycleRate::ApplyEffective(-2, "test"));
	EXPECT_EQ(EECycleRate::GetTransitionCount(), before + 1);

	// Back to the baseline.
	EXPECT_TRUE(EECycleRate::ApplyEffective(0, "test"));
	EXPECT_EQ(EECycleRate::GetEffective(), 0);
	EXPECT_EQ(EECycleRate::GetTransitionCount(), before + 2);

	Cpu = saved_cpu;
	CpuVU0 = saved_vu0;
}

// A transition rebuilds the EE recompiler and microVU0 and nothing else. The
// IOP recompiler, microVU1 and the VIF unpack dynarec bake no EE selector, and
// resetting them would cost a rebuild for nothing — under MTVU, resetting VU1
// would also reach across a thread.
TEST(EeRecCycleRate, TransitionResetsOnlyEeAndVu0)
{
	ScopedCycleRate rate(0);

	R5900cpu* saved_cpu = Cpu;
	BaseVUmicroCPU* saved_vu0 = CpuVU0;
	Cpu = &recCpu;
	CpuVU0 = &CpuMicroVU0;

	const EECycleRate::ResetGenerations before = EECycleRate::GetResetGenerations();
	ASSERT_TRUE(EECycleRate::ApplyEffective(-1, "test"));
	const EECycleRate::ResetGenerations after = EECycleRate::GetResetGenerations();

	EXPECT_GT(after.ee, before.ee);
	EXPECT_GT(after.vu0, before.vu0);
	EXPECT_EQ(after.iop, before.iop);
	EXPECT_EQ(after.vu1, before.vu1);
	EXPECT_EQ(after.vif, before.vif);

	EXPECT_TRUE(EECycleRate::ApplyEffective(0, "test"));

	Cpu = saved_cpu;
	CpuVU0 = saved_vu0;
}

// The census the immediate-patching design needs before it can be costed: how
// many charge sites would stop fitting the one instruction they were emitted
// in if the governor moved the selector, and how many blocks hold one.
//
// An ordinary block is nowhere near the boundary. 32 ADDIUs cost 9 raw units
// each, so the charge is double digits at every selector in the window and the
// same ADD encoding covers all of them.
TEST(EeRecCycleRate, AnOrdinaryBlockKeepsItsChargeEncodingAcrossTheWindow)
{
	ScopedCycleRate rate(0);
	ASSERT_EQ(EECycleRate::GetFloor(), -2);

	recCpu.Reset();

	EeRecTestHarness h;
	h.SetGpr64(reg::a1, 0);
	h.LoadProgram(LongBlock());
	h.Run();

	EXPECT_GT(recEeGetChargeSiteCount(), 0u) << "nothing was compiled; the rest proves nothing";
	EXPECT_EQ(recEeGetFormChangeSiteCount(), 0u);
	EXPECT_EQ(recEeGetFormChangeBlockCount(), 0u);
}

// A block of 128 parallel divides is not exotic, and it is over the line. The
// charge fits ADD's imm12 at the configured rate and does not fit it two steps
// down, so the block cannot be repaired by rewriting the immediate in place —
// a fresh compile would have emitted a longer sequence there.
TEST(EeRecCycleRate, ADeepDivideBlockOutgrowsItsChargeEncodingTwoStepsDown)
{
	ScopedCycleRate rate(0);
	ASSERT_EQ(EECycleRate::GetFloor(), -2);

	// The boundary itself, stated against the production scaler rather than
	// assumed. IsImmAddSub accepts a 12-bit immediate, or a 12-bit one shifted
	// left by 12 with the low bits clear — so 4096 is still one instruction and
	// 4097 is not. At rate -2 the charge is 7*raw/32, which reaches 4097 at raw
	// 18730; 18729 still lands on 4096.
	const auto fits_add_imm12 = [](u32 c) {
		return (c <= 0xfffu) || (((c >> 12) <= 0xfffu) && ((c & 0xfffu) == 0));
	};
	EXPECT_TRUE(fits_add_imm12(recEeScaleBlockCyclesForTest(18730, nullptr)))
		<< "at the configured rate this block is nowhere near the boundary";
	{
		ScopedCycleRate floor_rate(-2);
		EXPECT_EQ(recEeScaleBlockCyclesForTest(18729, nullptr), 4096u);
		EXPECT_TRUE(fits_add_imm12(recEeScaleBlockCyclesForTest(18729, nullptr)));
		EXPECT_EQ(recEeScaleBlockCyclesForTest(18730, nullptr), 4097u);
		EXPECT_FALSE(fits_add_imm12(recEeScaleBlockCyclesForTest(18730, nullptr)));
	}

	recCpu.Reset();

	EeRecTestHarness h;
	h.SetGpr64(reg::a1, 0x0001000200030004ull);
	h.SetGpr64(reg::a2, 0x0000000500000007ull);
	h.LoadProgram(DeepDivideBlock());
	// JIT only: this is a test about what the compiler emits, not about what
	// PDIVW computes.
	h.RunJitNoDiff();

	EXPECT_GE(recEeGetFormChangeSiteCount(), 1u);
	EXPECT_GE(recEeGetFormChangeBlockCount(), 1u);
}
