// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the EE charges for leaving an exception (ERET) and for taking one.
//
// Each test runs blocks whose charge is a whole number of cycles (padded with
// SYNCs), so the interpreter's carried sub-cycle remainder cannot move the
// total and both engines can be held to one number.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EECycleRate.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"

#include <gtest/gtest.h>

#include <vector>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kCp0Status = 12;
constexpr u32 kCp0Epc = 14;
constexpr u32 kStatusCu0 = 1u << 28; // kernel mode, EXL and ERL clear
constexpr u32 kCp0Config = 16;
constexpr u32 kConfigDie = 1u << 18; // dual issue on, as the BIOS sets it

u32 Eighths(u32 code) { return R5900::GetInstruction(code).cycles; }

// ERET's charge, measured on an SCPH-30001: ERET in place of a JR and its
// delay slot costs 3 cycles more, and 5 more than a taken branch.
constexpr u32 kEretEighths = 6 * 8;

// The measured charges this file pins, the table's for everything else.
u32 Measured(u32 code) { return code == ERET ? kEretEighths : Eighths(code); }

// Prepends SYNCs (no-ops in both engines) until the block's charge, plus
// `extra_eighths` for what the harness appends, is a whole number of cycles.
// Adds that charge, in cycles, to *cycles.
std::vector<u32> Whole(std::vector<u32> words, u32 extra_eighths, u64* cycles, u32 (*eighths)(u32) = Eighths)
{
	u32 raw = extra_eighths;
	for (u32 w : words)
		raw += eighths(w);
	u32 syncs = 0;
	while ((raw + syncs * Eighths(ee::SYNC)) % 8 != 0)
		syncs++;
	words.insert(words.begin(), syncs, ee::SYNC);
	*cycles += (raw + syncs * Eighths(ee::SYNC)) / 8;
	return words;
}

// COP0 Count into t0 at the start, into t1 at the start of a later block. The
// interpreter adds a block's charge to the cycle count at the block's closing
// branch, but not at ERET's, so the blocks under test are followed by one more
// (Settle) before the second read.
constexpr u32 kCp0Count = 9;
constexpr u32 kReadCount0 = MFC0(reg::t0, kCp0Count);
constexpr u32 kReadCount1 = MFC0(reg::t1, kCp0Count);

std::vector<u32> Settle(u64* cycles)
{
	std::vector<u32> b = Whole({BEQ(reg::zero, reg::zero, 1), NOP}, 0, cycles);
	b.push_back(kReadCount1);
	return b;
}

void ExpectCharged(const EeRecTestHarness& h, u64 cycles)
{
	EXPECT_EQ(h.GetGprJit(reg::t1) - h.GetGprJit(reg::t0), cycles) << "jit";
	EXPECT_EQ(h.GetGprInterp(reg::t1) - h.GetGprInterp(reg::t0), cycles) << "interp";
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

} // namespace

// ERET with EXL clear returns to EPC, as in a user-mode return: set EPC to the
// next block and take it. COP0 Count is read before and after, so the
// difference is what the blocks between were charged: the table's charge, in
// both engines. (The harness's own cycle delta of a JIT run that crosses blocks
// includes the parking lot's wait for the next event test.)
std::vector<u32> EretProgram(u64* charged, u32* ret_pc, u32 (*eighths)(u32) = Eighths)
{
	std::vector<u32> prog = Whole({LUI(reg::k0, 0), ORI(reg::k0, reg::k0, 0), MTC0(reg::k0, kCp0Epc), ERET}, Eighths(kReadCount0), charged, eighths);
	prog.insert(prog.begin(), kReadCount0);
	*ret_pc = kProgPc + 4 * static_cast<u32>(prog.size());
	prog[prog.size() - 4] = LUI(reg::k0, static_cast<u16>(*ret_pc >> 16));
	prog[prog.size() - 3] = ORI(reg::k0, reg::k0, static_cast<u16>(*ret_pc & 0xFFFF));
	const std::vector<u32> settle = Settle(charged);
	prog.insert(prog.end(), settle.begin(), settle.end());
	return prog;
}

TEST(EeRecExceptionCharge, BlockEndingInEretChargesTheTableInBothEngines)
{
	const ScopedClean clean;
	for (int pass = 0; pass < 2; pass++)
	{
		u64 charged = 0;
		u32 ret_pc = 0;
		const std::vector<u32> prog = EretProgram(&charged, &ret_pc);
		EeRecTestHarness h;
		h.SetCp0(kCp0Status, kStatusCu0);
		h.SetCp0(kCp0Config, kConfigDie);
		h.LoadProgram(prog);
		h.Run();
		EXPECT_EQ(h.GetCp0Jit(kCp0Epc), ret_pc);
		if (pass == 1)
			ExpectCharged(h, charged);
	}
}

// Both engines charge ERET its measured 6 cycles.
TEST(EeRecExceptionCharge, EretChargesSixCycles)
{
	const ScopedClean clean;
	EXPECT_EQ(static_cast<u32>(R5900::GetInstruction(ERET).cycles), kEretEighths);
	for (int pass = 0; pass < 2; pass++)
	{
		u64 charged = 0;
		u32 ret_pc = 0;
		const std::vector<u32> prog = EretProgram(&charged, &ret_pc, Measured);
		EeRecTestHarness h;
		h.SetCp0(kCp0Status, kStatusCu0);
		h.SetCp0(kCp0Config, kConfigDie);
		h.LoadProgram(prog);
		h.Run();
		if (pass == 1)
			ExpectCharged(h, charged);
	}
}
