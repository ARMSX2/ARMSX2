// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// A TLB miss on an inline load or store under the EE recompiler.
//
// Every test here is DISABLED: the rec does not divert to the exception
// vector, and the work to make it is staged. Force-enable with
// --gtest_also_run_disabled_tests; each one that starts passing drops its
// prefix.
//
// vtlb_Miss (vtlb.cpp) reports the miss and returns without raising, so the
// guest's handler never runs, the load reads zero, the store is dropped and
// the block continues. EeRecTraps.LoadTlbMissDoesNotRaiseOnTheRecompiler pins
// that; EeRecTraps.LoadTlbMissInDelaySlotSetsCauseBdAndBranchEpcOnTheInterpreter
// is the interpreter behaviour the tests below ask the rec to match.
//
// Raising and then walking past the raise was worse: cpuException leaves EPC
// alone whenever Status.EXL is already set, so every exception after the first
// swallowed miss keeps its predecessor's EPC, and the next syscall's kernel
// epilogue erets to an address belonging to the fault.
// DISABLED_MissLeavesExlLatchedSoTheNextSyscallLosesItsEpc is that chain in
// four instructions; it fails here because nothing raises at all.
//
// Softmem, not fastmem: the test binary installs no host SIGSEGV handler, so a
// fastmem probe of the unmapped page kills the process instead of backpatching
// (same constraint as CallerSavedPinsSurviveVtlbSlowPath). Nothing here reaches
// the fastmem backpatch thunk, which is generated at fault time and cannot name
// the live guest values of the block around it, so it has nothing to divert
// with.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "R5900.h"

#include <gtest/gtest.h>

#include <iterator>
#include <vector>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kUnmapped = 0x40000000; // useg, no TLB entry
constexpr u32 kCauseTlbL = 0x08;      // ExcCode=2  (TLBL), << 2
constexpr u32 kCauseTlbS = 0x0C;      // ExcCode=3  (TLBS), << 2
constexpr u32 kCauseSys  = 0x20;      // ExcCode=8  (Sys),  << 2

// Restores EnableFastmem whatever the assertions do.
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

} // namespace

// The faulting load is the last instruction that executes. Everything after it
// belongs to the exception handler, which the harness stubs at the TLB-refill
// vector with `jr ra; nop` back to the parking lot.
TEST(EeRecTlbDivert, DISABLED_LoadMissDivertsToTheVector)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.LoadProgram({
		LUI(reg::a0, kUnmapped >> 16),   // +0x0
		LW(reg::v1, 0, reg::a0),         // +0x4  TLB refill miss
		ADDIU(reg::v0, reg::zero, 99),   // +0x8  must not execute
	});
	h.Run();

	h.ExpectGpr64(reg::v0, 0ull);        // the block must not run on past the miss
	h.ExpectGpr64(reg::v1, 0ull);        // the faulting load must not write rt

	EXPECT_EQ(h.GetCp0Interp(13) & 0xFFu, kCauseTlbL);
	EXPECT_EQ(h.GetCp0Interp(13) & 0x80000000u, 0u) << "interp CAUSE.BD clear";
	EXPECT_EQ(h.GetCp0Interp(14), RecompilerTestEnvironment::kProgramPc + 4)
		<< "interp EPC = the faulting load";
	EXPECT_EQ(h.GetCp0Interp(8), kUnmapped);

	EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, kCauseTlbL);
	EXPECT_EQ(h.GetCp0Jit(13) & 0x80000000u, 0u) << "JIT CAUSE.BD clear";
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc + 4)
		<< "JIT EPC = the faulting load";
	EXPECT_EQ(h.GetCp0Jit(8), kUnmapped);
}

// The store side of the class. vtlbSoftmemWrite's slow path and the const-paddr
// write shortcut are separate emitters from their read twins, so each needs its
// own poll — TLBS instead of TLBL, everything else identical.
TEST(EeRecTlbDivert, DISABLED_StoreMissDivertsToTheVector)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.SetGpr64(reg::a1, 0x1234);
	h.LoadProgram({
		LUI(reg::a0, kUnmapped >> 16),   // +0x0
		SW(reg::a1, 0, reg::a0),         // +0x4  TLB refill miss on a store
		ADDIU(reg::v0, reg::zero, 99),   // +0x8  must not execute
	});
	h.Run();

	h.ExpectGpr64(reg::v0, 0ull);

	EXPECT_EQ(h.GetCp0Interp(13) & 0xFFu, kCauseTlbS);
	EXPECT_EQ(h.GetCp0Interp(14), RecompilerTestEnvironment::kProgramPc + 4);
	EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, kCauseTlbS);
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc + 4)
		<< "JIT EPC = the faulting store";
}

// The 3D Pinball chain. cpuException leaves EPC alone whenever EXL is already
// set (R5900.cpp, architectural MIPS), so a swallowed miss does not merely lose
// one instruction — it silently disarms EPC for every exception that follows.
// The SYSCALL here stands in for the game's `jal GetThreadId`, whose kernel
// epilogue then computes its return address from an EPC belonging to the
// faulting load and erets into the middle of strlen.
//
// Required: the block leaves at +0x4, so neither +0x8 nor +0xC runs, Cause
// still reads TLBL and EPC still points at the load. Today it runs on into the
// SYSCALL, which overwrites Cause with Sys and — EXL being latched — does not
// update EPC. Cause and EPC describing two different instructions is the
// corruption itself, and it is what the game's kernel epilogue then erets on.
TEST(EeRecTlbDivert, DISABLED_MissLeavesExlLatchedSoTheNextSyscallLosesItsEpc)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.LoadProgram({
		LUI(reg::a0, kUnmapped >> 16),   // +0x0
		LW(reg::v1, 0, reg::a0),         // +0x4  TLB refill miss
		ADDIU(reg::v0, reg::zero, 99),   // +0x8  must not execute
		SYSCALL_(),                      // +0xC  must not execute
	});
	h.Run();

	h.ExpectGpr64(reg::v0, 0ull);

	EXPECT_EQ(h.GetCp0Interp(13) & 0xFFu, kCauseTlbL);
	EXPECT_EQ(h.GetCp0Interp(14), RecompilerTestEnvironment::kProgramPc + 4);

	EXPECT_NE(h.GetCp0Jit(13) & 0xFFu, kCauseSys)
		<< "the SYSCALL after the swallowed miss must never have executed";
	EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, kCauseTlbL);
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc + 4)
		<< "EPC must still name the load, not a later exception's instruction";
}

// A const base (LUI, above) resolves its page at compile time and takes the
// const-paddr MMIO shortcut — a direct BL to the unmapped handler. A base the
// block cannot fold takes the generic path instead: the inline vmap lookup in
// vtlbSoftmemRead / vtlbSoftmemWrite and its slow-path call to vtlb_memRead /
// vtlb_memWrite. Both reach vtlb_Miss, and each is its own emitter.
TEST(EeRecTlbDivert, DISABLED_DynamicBaseLoadMissDivertsToTheVector)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, kUnmapped);  // seeded, so not compile-time const
	h.LoadProgram({
		LW(reg::v1, 0, reg::a0),         // +0x0  TLB refill miss
		ADDIU(reg::v0, reg::zero, 99),   // +0x4  must not execute
	});
	h.Run();

	h.ExpectGpr64(reg::v0, 0ull);
	h.ExpectGpr64(reg::v1, 0ull);

	EXPECT_EQ(h.GetCp0Interp(13) & 0xFFu, kCauseTlbL);
	EXPECT_EQ(h.GetCp0Interp(14), RecompilerTestEnvironment::kProgramPc);
	EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, kCauseTlbL);
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc)
		<< "JIT EPC = the faulting load";
	EXPECT_EQ(h.GetCp0Jit(8), kUnmapped);
}

TEST(EeRecTlbDivert, DISABLED_DynamicBaseStoreMissDivertsToTheVector)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, kUnmapped);
	h.SetGpr64(reg::a1, 0x1234);
	h.LoadProgram({
		SW(reg::a1, 0, reg::a0),         // +0x0  TLB refill miss on a store
		ADDIU(reg::v0, reg::zero, 99),   // +0x4  must not execute
	});
	h.Run();

	h.ExpectGpr64(reg::v0, 0ull);

	EXPECT_EQ(h.GetCp0Interp(13) & 0xFFu, kCauseTlbS);
	EXPECT_EQ(h.GetCp0Interp(14), RecompilerTestEnvironment::kProgramPc);
	EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, kCauseTlbS);
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc)
		<< "JIT EPC = the faulting store";
}

// The generic path with computed rather than folded values, so nothing here
// comes out of the const table. Not enough pressure to reach x28 — see the
// register-pressure case below for the one that actually needs the flush.
TEST(EeRecTlbDivert, DISABLED_DynamicWritesBeforeTheMissSurviveTheDivert)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, kUnmapped);
	h.SetGpr64(reg::a1, 7);
	h.LoadProgram({
		ADDIU(reg::t0, reg::a1, 4),      // +0x0  } computed from a seeded reg;
		ADDU(reg::t1, reg::t0, reg::a1), // +0x4  } no const folding, so these
		ADDIU(reg::t2, reg::t1, 1),      // +0x8  } live in host registers
		LW(reg::v1, 0, reg::a0),         // +0xC  TLB refill miss
		ADDIU(reg::v0, reg::zero, 99),   // +0x10 must not execute
	});
	h.Run();

	h.ExpectGpr64(reg::t0, 11ull);
	h.ExpectGpr64(reg::t1, 18ull);
	h.ExpectGpr64(reg::t2, 19ull);
	h.ExpectGpr64(reg::v0, 0ull);
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc + 0xC)
		<< "JIT EPC = the faulting load";
}

// The shape that decides how the divert has to be built. Reaching the vector
// with guest state complete is most of the work, and the reason is narrow:
// iFlushCall(FLUSH_VTLB), which every inline vtlb path already runs before the
// access, frees the caller-saved host registers but not x28 — the allocator's
// one callee-saved host. A guest register that landed there is live and dirty
// when the miss fires, and a bare jump to DispatcherReg drops it. A boot of the
// RA ISO with fastmem off reaches that state at 489 sites and nothing else was
// ever dirty there.
//
// It takes 16 live guest values at once to make the allocator reach x28, which
// is why the smaller cases in this file do not exercise the writeback at all.
TEST(EeRecTlbDivert, DISABLED_RegisterPressureWritesSurviveTheDivert)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.SetGpr64(reg::a2, kUnmapped); // base, and the source of every value below

	std::vector<u32> program;
	// r7 (a3), r8-r15 (t0-t7), r17-r23 (s1-s7): 16 unpinned registers, all
	// computed rather than folded, all still live across the load.
	const u32 dirty[] = {7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23};
	for (u32 i = 0; i < std::size(dirty); i++)
		program.push_back(ADDIU(dirty[i], reg::a2, static_cast<s16>(i + 1)));
	const u32 miss_off = static_cast<u32>(program.size()) * 4;
	program.push_back(LW(reg::v1, 0, reg::a2));       // TLB refill miss
	program.push_back(ADDIU(reg::v0, reg::zero, 99)); // must not execute
	h.LoadProgram(program);
	h.Run();

	for (u32 i = 0; i < std::size(dirty); i++)
		h.ExpectGpr64(dirty[i], static_cast<u64>(kUnmapped + i + 1));
	h.ExpectGpr64(reg::v0, 0ull);
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc + miss_off)
		<< "JIT EPC = the faulting load";
}

// The const-folded twin of the pressure case above. Both the writes and the
// address fold at compile time, so nothing is allocator-resident at the miss
// and the writeback is not what carries them. It is here for the const path's
// own coverage of "the block stops at the faulting load".
TEST(EeRecTlbDivert, DISABLED_WritesBeforeTheMissSurviveTheDivert)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.LoadProgram({
		LUI(reg::a0, kUnmapped >> 16),   // +0x0
		ADDIU(reg::t0, reg::zero, 11),   // +0x4  } retired before the miss;
		ADDIU(reg::t1, reg::zero, 22),   // +0x8  } must be architecturally
		ADDIU(reg::t2, reg::zero, 33),   // +0xC  } visible to the handler
		LW(reg::v1, 0, reg::a0),         // +0x10 TLB refill miss
		ADDIU(reg::v0, reg::zero, 99),   // +0x14 must not execute
	});
	h.Run();

	h.ExpectGpr64(reg::t0, 11ull);
	h.ExpectGpr64(reg::t1, 22ull);
	h.ExpectGpr64(reg::t2, 33ull);
	h.ExpectGpr64(reg::v0, 0ull);
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc + 0x10)
		<< "JIT EPC = the faulting load";
}

// The delay-slot case is its own emitter seam: the divert rides the
// cpuRegs.branch bracket epilogue rather than the load-store path, and EPC and
// Cause.BD name the branch instead of the slot.
TEST(EeRecTlbDivert, DISABLED_DelaySlotMissDivertsToTheBranchVector)
{
	SoftmemScope softmem;
	EeRecTestHarness h;
	h.LoadProgram({
		LUI(reg::a0, kUnmapped >> 16),   // +0x0
		BEQ(reg::zero, reg::zero, 2),    // +0x4  taken, to +0x10
		LW(reg::v1, 0, reg::a0),         // +0x8  delay slot — TLB refill miss
		ADDIU(reg::v0, reg::zero, 99),   // +0xC  skipped by the branch
		ADDIU(reg::a1, reg::zero, 77),   // +0x10 target — the vector preempts it
	});
	h.Run();

	h.ExpectGpr64(reg::v0, 0ull);
	h.ExpectGpr64(reg::v1, 0ull);
	h.ExpectGpr64(reg::a1, 0ull);

	EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, kCauseTlbL);
	EXPECT_NE(h.GetCp0Jit(13) & 0x80000000u, 0u) << "JIT CAUSE.BD";
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc + 4)
		<< "JIT EPC = the branch, not the delay slot";
	EXPECT_EQ(h.GetCp0Jit(8), kUnmapped);
}
