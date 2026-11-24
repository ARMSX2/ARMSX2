// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the EE charges for leaving an exception (ERET) and for taking one.
//
// Every charge is a whole number of cycles (the issue model's), so both
// engines can be held to one number: the model's cost of the paths run, plus
// the entry for each exception taken.

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

namespace {

using R5900::Issue::Exit;

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kCp0Status = 12;
constexpr u32 kCp0Epc = 14;
constexpr u32 kStatusCu0 = 1u << 28; // kernel mode, EXL and ERL clear
constexpr u32 kCp0Config = 16;
constexpr u32 kConfigDie = 1u << 18; // dual issue on, as the BIOS sets it

// ERET's charge, measured on an SCPH-30001: ERET in place of a JR and its
// delay slot costs 3 cycles more, and 5 more than a taken branch.
constexpr u32 kEretCycles = 6;

// What taking an exception costs beyond the instructions it runs, measured on
// an SCPH-30001: a BREAK into a five-instruction handler that returns with
// ERET costs 33 cycles there, 9 more than the table charged for the BREAK, the
// kernel's ten-instruction vector and the handler.
constexpr u64 kEntryCycles = 9;

// The model's cost of `words` run as one path from `pc`, leaving by `exit`.
u64 PathCost(u32 pc, const std::vector<u32>& words, Exit exit)
{
	return R5900::Issue::PathCycles(pc, words.data(), static_cast<u32>(words.size()), exit);
}

// COP0 Count into t0 at the start, into t1 at the start of a later block. The
// interpreter adds a block's charge to the cycle count at the block's closing
// branch, but not at ERET's, so the blocks under test are followed by one more
// (Settle) before the second read.
constexpr u32 kCp0Count = 9;
constexpr u32 kReadCount0 = MFC0(reg::t0, kCp0Count);
constexpr u32 kReadCount1 = MFC0(reg::t1, kCp0Count);

// The settling block's path (its branch is taken) and the read after it.
const std::vector<u32> kSettle = {BEQ(reg::zero, reg::zero, 1), NOP};

void AppendSettle(std::vector<u32>& prog, u64* cycles)
{
	*cycles += PathCost(kProgPc + 4 * static_cast<u32>(prog.size()), kSettle, Exit::Taken);
	prog.insert(prog.end(), kSettle.begin(), kSettle.end());
	prog.push_back(kReadCount1);
}

void ExpectCharged(const EeRecTestHarness& h, u64 cycles)
{
	EXPECT_EQ(h.GetGprJit(reg::t1) - h.GetGprJit(reg::t0), cycles) << "jit";
	EXPECT_EQ(h.GetGprInterp(reg::t1) - h.GetGprInterp(reg::t0), cycles) << "interp";
}

constexpr u32 kVector = 0x80000180;

// Installs a guest kernel handler at the common exception vector and puts the
// environment's `jr ra; nop` stub back afterwards.
class KernelHandler
{
public:
	KernelHandler(EeRecTestHarness& h, const std::vector<u32>& words)
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
// difference is what the blocks between were charged. (The harness's own cycle
// delta of a JIT run that crosses blocks includes the parking lot's wait for
// the next event test.) ERET ends its path.
std::vector<u32> EretProgram(u64* charged, u32* ret_pc)
{
	std::vector<u32> prog = {kReadCount0, LUI(reg::k0, 0), ORI(reg::k0, reg::k0, 0), MTC0(reg::k0, kCp0Epc), ERET};
	*ret_pc = kProgPc + 4 * static_cast<u32>(prog.size());
	prog[1] = LUI(reg::k0, static_cast<u16>(*ret_pc >> 16));
	prog[2] = ORI(reg::k0, reg::k0, static_cast<u16>(*ret_pc & 0xFFFF));
	*charged += PathCost(kProgPc, prog, Exit::None);
	AppendSettle(prog, charged);
	return prog;
}

TEST(EeRecExceptionCharge, BlockEndingInEretChargesTheModelInBothEngines)
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
		ExpectCharged(h, charged);
	}
}

// Both engines charge ERET its measured 6 cycles.
TEST(EeRecExceptionCharge, EretChargesSixCycles)
{
	const ScopedClean clean;
	EXPECT_EQ(PathCost(kProgPc, {ERET}, Exit::None), kEretCycles);
	// ERET in place of an ADDIU in the same program: 5 more.
	u64 with_eret = 0, with_addiu = 0;
	u32 ret_pc = 0;
	std::vector<u32> prog = EretProgram(&with_eret, &ret_pc);
	std::vector<u32> alt = {prog[0], prog[1], prog[2], prog[3], ADDIU(reg::v1, reg::zero, 1)};
	with_addiu = PathCost(kProgPc, alt, Exit::None);
	EXPECT_EQ(PathCost(kProgPc, {prog[0], prog[1], prog[2], prog[3], ERET}, Exit::None), with_addiu + kEretCycles - 1);
	EeRecTestHarness h;
	h.SetCp0(kCp0Status, kStatusCu0);
	h.SetCp0(kCp0Config, kConfigDie);
	h.LoadProgram(prog);
	h.Run();
	ExpectCharged(h, with_eret);
}

// SYSCALL or BREAK in a block of its own, a handler at the common vector that
// steps EPC past it, as the BIOS does, and returns with ERET, then a settling
// block. Both engines charge the paths plus the entry.
void ExpectRoundTrip(u32 op)
{
	const ScopedClean clean;
	for (int pass = 0; pass < 2; pass++)
	{
		u64 charged = kEntryCycles;
		std::vector<u32> prog = {kReadCount0, ADDIU(reg::v1, reg::zero, 0x2F), op};
		charged += PathCost(kProgPc, prog, Exit::None);
		const u32 ret_pc = kProgPc + 4 * static_cast<u32>(prog.size());
		AppendSettle(prog, &charged);
		const std::vector<u32> handler = {
			MFC0(reg::k0, kCp0Epc),
			ADDIU(reg::k0, reg::k0, 4),
			MTC0(reg::k0, kCp0Epc),
			ERET,
		};
		charged += PathCost(kVector, handler, Exit::None);

		EeRecTestHarness h;
		h.SetCp0(kCp0Status, kStatusCu0);
		h.SetCp0(kCp0Config, kConfigDie);
		KernelHandler kernel(h, handler);
		h.LoadProgram(prog);
		h.Run();
		h.ExpectGpr64(reg::k0, ret_pc); // the handler ran and returned past the op
		ExpectCharged(h, charged);
	}
}

TEST(EeRecExceptionCharge, SyscallRoundTripChargesTheEntry)
{
	ExpectRoundTrip(SYSCALL_());
}

TEST(EeRecExceptionCharge, BreakRoundTripChargesTheEntry)
{
	ExpectRoundTrip(BREAK);
}
