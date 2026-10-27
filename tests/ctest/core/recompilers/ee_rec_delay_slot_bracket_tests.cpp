// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The cpuRegs.branch bracket around a memory op in a branch delay slot.
//
// The bracket (store 1 to cpuRegs.branch before the slot, reload it after,
// divert to DispatcherReg if an exception cleared it, store 0) exists for
// delay-slot instructions that can raise, so that cpuException sees bd = 1.
// Under the recompiler a load or store raises nothing: vtlb_Miss and
// _ext_memUnknown report and return (EeRecTraps.LoadTlbMissDoesNotRaiseOnTheRecompiler,
// UnknownMmioAccessDoesNotRaiseOnTheRecompiler). On the fastmem path the
// bracket therefore never observes anything, and it is not emitted. A softmem
// compile keeps it: the staged TLB-miss divert in ee_rec_tlb_divert_tests.cpp
// rides its epilogue.
//
// The shape tests count 32-bit accesses to cpuRegs.branch off RSTATE and
// CBZ-to-DispatcherReg exits in the block's host code. The other tests diff
// the slot's effect against the interpreter for each memory emitter family.

#include "harness/EeRecTestHarness.h"

#include "arm64/iR5900-arm64.h"

#include "Config.h"
#include "R5900.h"

#include <gtest/gtest.h>

#include <vector>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);
extern void recEeDispatcherAddrs(const void** dispatcher_event, const void** dispatcher_reg);

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;

class SoftmemScope
{
public:
	SoftmemScope()
		: saved_(EmuConfig.Cpu.Recompiler.EnableFastmem)
	{
		EmuConfig.Cpu.Recompiler.EnableFastmem = false;
	}
	~SoftmemScope() { EmuConfig.Cpu.Recompiler.EnableFastmem = saved_; }

private:
	bool saved_;
};

struct BracketShape
{
	u32 branch_stores = 0; // STR wN, [x20, #branch]
	u32 branch_loads = 0;  // LDR wN, [x20, #branch]
	u32 cbz_to_reg = 0;    // CBZ wN, DispatcherReg
};

BracketShape ShapeOfBlock(u32 guest_pc)
{
	BracketShape s;
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
	{
		ADD_FAILURE() << "no compiled block at 0x" << std::hex << guest_pc;
		return s;
	}
	const void* dispatcher_event = nullptr;
	const void* dispatcher_reg = nullptr;
	recEeDispatcherAddrs(&dispatcher_event, &dispatcher_reg);

	const u32 branch_off = static_cast<u32>(
		reinterpret_cast<const u8*>(&cpuRegs.branch) - reinterpret_cast<const u8*>(&cpuRegs));
	const u32 rstate = RSTATE.GetCode();
	const u32* code = reinterpret_cast<const u32*>(fn);
	for (u32 i = 0; i < size / 4; i++)
	{
		const u32 w = code[i];
		const bool str32 = (w & 0xFFC00000u) == 0xB9000000u;
		const bool ldr32 = (w & 0xFFC00000u) == 0xB9400000u;
		if ((str32 || ldr32) && ((w >> 5) & 0x1F) == rstate && ((w >> 10) & 0xFFF) * 4 == branch_off)
			(str32 ? s.branch_stores : s.branch_loads)++;
		if ((w & 0xFF000000u) == 0x34000000u) // CBZ (32-bit)
		{
			const s32 imm19 = static_cast<s32>(w << 8) >> 13;
			const u8* dest = reinterpret_cast<const u8*>(&code[i]) + imm19 * 4;
			if (dest == dispatcher_reg)
				s.cbz_to_reg++;
		}
	}
	return s;
}

// A taken branch whose delay slot is `slot`; the instruction after the slot
// is skipped, the target runs.
std::vector<u32> DelaySlotProgram(u32 slot)
{
	return {
		BEQ(reg::zero, reg::zero, 2),  // +0x0: taken, to +0xC
		slot,                          // +0x4: delay slot
		ADDIU(reg::v0, reg::zero, 99), // +0x8: skipped
		ADDIU(reg::a1, reg::zero, 77), // +0xC: target
	};
}

void SeedScratch(EeRecTestHarness& h)
{
	for (u32 i = 0; i < 16; i++)
		h.WriteU32(kScratch + i * 4, 0x11223344u * (i + 1));
	h.SetGpr64(reg::a0, kScratch);
	h.SetGpr64(reg::t0, 0x8899AABBCCDDEEFFull);
}

void ExpectBranchSemantics(EeRecTestHarness& h)
{
	h.ExpectGpr64(reg::v0, 0ull);
	h.ExpectGpr64(reg::a1, 77ull);
	EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, 0u) << "no exception on this path";
}

} // namespace

TEST(EeRecDelaySlotBracket, FastmemIsOnInTheTestBinary)
{
	// Every fastmem test below is vacuous without it.
	ASSERT_TRUE(CHECK_FASTMEM);
}

TEST(EeRecDelaySlotBracket, FastmemLoadInDelaySlotEmitsNoBracket)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	EeRecTestHarness h;
	SeedScratch(h);
	h.LoadProgram(DelaySlotProgram(LW(reg::v1, 4, reg::a0)));
	h.Run();
	ExpectBranchSemantics(h);
	h.ExpectGpr64(reg::v1, static_cast<u64>(static_cast<s64>(static_cast<s32>(0x11223344u * 2))));
	const BracketShape s = ShapeOfBlock(kProgPc);
	EXPECT_EQ(s.branch_stores, 0u);
	EXPECT_EQ(s.branch_loads, 0u);
	EXPECT_EQ(s.cbz_to_reg, 0u);
}

TEST(EeRecDelaySlotBracket, FastmemStoreInDelaySlotEmitsNoBracket)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	EeRecTestHarness h;
	SeedScratch(h);
	h.LoadProgram(DelaySlotProgram(SW(reg::t0, 8, reg::a0)));
	h.Run();
	ExpectBranchSemantics(h);
	EXPECT_EQ(h.ReadU32(kScratch + 8), 0xCCDDEEFFu);
	const BracketShape s = ShapeOfBlock(kProgPc);
	EXPECT_EQ(s.branch_stores, 0u);
	EXPECT_EQ(s.branch_loads, 0u);
	EXPECT_EQ(s.cbz_to_reg, 0u);
}

// Each emitter family the bracket used to wrap: scalar, 128-bit, unaligned,
// FPU and COP2 accesses. The run diffs against the interpreter.
TEST(EeRecDelaySlotBracket, EveryFastmemMemoryFamilyInDelaySlotRunsWithoutBracket)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	const u32 slots[] = {
		LB(reg::v1, 3, reg::a0),
		LD(reg::v1, 8, reg::a0),
		SB(reg::t0, 5, reg::a0),
		SD(reg::t0, 16, reg::a0),
		LQ(reg::v1, 16, reg::a0),
		SQ(reg::t0, 32, reg::a0),
		LDL(reg::v1, 11, reg::a0),
		SDR(reg::t0, 21, reg::a0),
		SDL(reg::t0, 21, reg::a0),
		LWL(reg::v1, 6, reg::a0),
		SWR(reg::t0, 7, reg::a0),
		LWC1(2, 12, reg::a0),
		LQC2(3, reg::a0, 32),
	};
	for (const u32 slot : slots)
	{
		SCOPED_TRACE(::testing::Message() << "slot 0x" << std::hex << slot);
		EeRecTestHarness h;
		SeedScratch(h);
		h.LoadProgram(DelaySlotProgram(slot));
		h.Run();
		ExpectBranchSemantics(h);
		const BracketShape s = ShapeOfBlock(kProgPc);
		EXPECT_EQ(s.branch_stores, 0u);
		EXPECT_EQ(s.cbz_to_reg, 0u);
	}
}

// The control: a softmem compile keeps the bracket (store 1, reload, divert,
// store 0), so the shape counter above is not blind.
TEST(EeRecDelaySlotBracket, SoftmemLoadInDelaySlotKeepsTheBracket)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	SeedScratch(h);
	h.LoadProgram(DelaySlotProgram(LW(reg::v1, 4, reg::a0)));
	h.Run();
	ExpectBranchSemantics(h);
	const BracketShape s = ShapeOfBlock(kProgPc);
	EXPECT_EQ(s.branch_stores, 2u) << "the set and the clear";
	EXPECT_EQ(s.branch_loads, 1u);
	EXPECT_EQ(s.cbz_to_reg, 1u);
}

// The raisers keep it whatever the memory path: SYSCALL in a delay slot with
// fastmem on still brackets (EeRecTraps.SyscallInDelaySlotSetsCauseBdAndBranchEpc
// checks the result; this checks the shape).
TEST(EeRecDelaySlotBracket, SyscallInDelaySlotKeepsTheBracketUnderFastmem)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	EeRecTestHarness h;
	h.LoadProgram({
		BEQ(reg::zero, reg::zero, 2),
		SYSCALL_(),
		ADDIU(reg::v0, reg::zero, 99),
		ADDIU(reg::v1, reg::zero, 77),
	});
	h.Run();
	const BracketShape s = ShapeOfBlock(kProgPc);
	EXPECT_GE(s.branch_stores, 1u);
	EXPECT_EQ(s.cbz_to_reg, 1u);
}
