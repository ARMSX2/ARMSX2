// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// mVUclear must drop exactly the quick slots a full scan of quick[] would.
//
// After a Clear of [addr, addr+size), slot S is empty iff it was empty before
// or its program's compiled ranges overlap the write (QuickSlotWouldDrop).
// Nothing else may move.
//
// mVUclear visits only the occupied slots, tracked by prog.quickOcc. An index
// that misses a slot lets it survive a write that should have invalidated it,
// and the next dispatch runs stale code with no diagnostic. So besides the
// slot-set comparison, the test rewrites program bytes and checks the new code
// runs after the Clear, and checks that quickOcc agrees bit for bit with a full
// scan of quick[] after every dispatch, Clear and reset.
//
// Setup: eight tiny E-bit programs at evenly spaced PCs, each leaving a
// distinct value in vi3, dispatched through the production path so each owns
// a quick slot. Then randomized Clear windows, some aligned to program
// boundaries, some rewriting a program's first pair.

#include "harness/VuTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "VU.h"
#include "VUmicro.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <random>

// Test hooks exported by pcsx2/arm64/microVU-arm64.cpp under
// PCSX2_RECOMPILER_TESTS (cross-TU pattern; see mvu_lpstate_invariant_tests).
namespace mvu_test_hooks
{
	bool QuickSlotOccupied(int vu_index, u32 start_pc_bytes);
	bool QuickSlotWouldDrop(int vu_index, u32 start_pc_bytes, u32 addr, u32 size);
	u32 QuickSlotCount(int vu_index);
	bool QuickOccupancyMatchesScan(int vu_index);
} // namespace mvu_test_hooks

namespace recompiler_tests {

using namespace vu;

namespace {

constexpr u32 kBigBudget = 4096;
constexpr u32 kPrograms = 8;
constexpr u32 kProgramBytes = 0x18; // three pairs

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

struct VuUnderTest
{
	int idx;

	BaseVUmicroCPU* Cpu() const
	{
		return idx ? static_cast<BaseVUmicroCPU*>(&CpuMicroVU1)
		           : static_cast<BaseVUmicroCPU*>(&CpuMicroVU0);
	}
	u32 MicroBytes() const { return idx ? 0x4000u : 0x1000u; }
	u32 RunningBit() const { return idx ? 0x100u : 0x1u; }
	u32 ProgramPc(u32 i) const { return i * (MicroBytes() / kPrograms); }
};

void WritePair(const VuUnderTest& v, u32 byte_addr, const VuOp& op)
{
	std::memcpy(vuRegs[v.idx].Micro + byte_addr + 0, &op.lower, 4);
	std::memcpy(vuRegs[v.idx].Micro + byte_addr + 4, &op.upper, 4);
}

// vi3 = value; vi3 += 1 at the E-bit; the third pair is the E-bit delay slot.
// The program therefore leaves value + 1 in vi3.
void WriteProgram(const VuUnderTest& v, u32 pc, u32 value)
{
	WritePair(v, pc + 0x00, LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, value)));
	WritePair(v, pc + 0x08, EBit(LowerOnly(VIADDIU_L(vi::vi3, vi::vi3, 1))));
	WritePair(v, pc + 0x10, LowerOnly(VIADDIU_L(vi::vi4, vi::vi0, 0)));
}

bool Busy(const VuUnderTest& v)
{
	return (vuRegs[0].VI[REG_VPU_STAT].UL & v.RunningBit()) != 0;
}

// Production-shaped dispatch at byte address pc, then run to the E-bit end.
void Dispatch(const VuUnderTest& v, u32 pc)
{
	vuRegs[v.idx].VI[REG_TPC].UL = pc / 8u;
	v.Cpu()->SetStartPC(pc);
	vuRegs[0].VI[REG_VPU_STAT].UL |= v.RunningBit();
	vuRegs[v.idx].VI[REG_VPU_STAT].UL = vuRegs[0].VI[REG_VPU_STAT].UL;
	v.Cpu()->Execute(kBigBudget);
}

void RunAndCheck(const VuUnderTest& v, u32 i, u32 value, const char* what)
{
	vuRegs[v.idx].VI[vi::vi3].UL = 0;
	Dispatch(v, v.ProgramPc(i));
	ASSERT_FALSE(Busy(v)) << what << ": program " << i << " must reach its E-bit end";
	ASSERT_EQ(vuRegs[v.idx].VI[vi::vi3].UL & 0xFFFFu, value + 1)
		<< what << ": program " << i << " ran the wrong code (stale block?)";
	ASSERT_TRUE(mvu_test_hooks::QuickSlotOccupied(v.idx, v.ProgramPc(i)))
		<< what << ": program " << i << " must be quick-cached after a dispatch";
	ASSERT_TRUE(mvu_test_hooks::QuickOccupancyMatchesScan(v.idx))
		<< what << ": the occupied-slot index disagrees with quick[] after a dispatch";
}

u32 ValueFor(u32 i, u32 generation) { return 0x100u + i * 0x10u + (generation & 0xFu); }

struct ClearStats
{
	u32 dropped = 0;
	u32 kept = 0;
};

// Clear [addr, addr+size) and check the program slots end up exactly where
// the full-scan rule says. `expect` receives the per-program survivors.
void ClearAndCompare(const VuUnderTest& v, u32 addr, u32 size, const char* what,
	std::array<bool, kPrograms>& expect, ClearStats& stats)
{
	u32 expectCount = 0;
	for (u32 i = 0; i < kPrograms; i++)
	{
		const bool was = mvu_test_hooks::QuickSlotOccupied(v.idx, v.ProgramPc(i));
		expect[i] = was && !mvu_test_hooks::QuickSlotWouldDrop(v.idx, v.ProgramPc(i), addr, size);
		expectCount += expect[i];
		stats.dropped += was && !expect[i];
		stats.kept += expect[i];
	}

	v.Cpu()->Clear(addr, size);

	for (u32 i = 0; i < kPrograms; i++)
	{
		ASSERT_EQ(mvu_test_hooks::QuickSlotOccupied(v.idx, v.ProgramPc(i)), expect[i])
			<< what << ": clear [" << std::hex << addr << ", +" << size
			<< ") left the slot of program " << std::dec << i << " in the wrong state";
	}
	ASSERT_EQ(mvu_test_hooks::QuickSlotCount(v.idx), expectCount)
		<< what << ": a slot outside the programs changed";
	ASSERT_TRUE(mvu_test_hooks::QuickOccupancyMatchesScan(v.idx))
		<< what << ": the occupied-slot index disagrees with quick[] after a clear";
}

#define BAIL_ON_FATAL() \
	do { if (::testing::Test::HasFatalFailure()) return; } while (0)

void RunScenario(int vu_index)
{
	const VuUnderTest v{vu_index};
	VuTestHarness h(vu_index);
	RecompilerTestEnvironment::ResetVuBlockCache(vu_index);
	std::memset(vuRegs[vu_index].Micro, 0, v.MicroBytes());

	std::array<u32, kPrograms> value{};
	std::array<u32, kPrograms> generation{};
	std::array<bool, kPrograms> expect{};
	ClearStats stats;

	// Establish the programs one at a time. Each write lands in a region no
	// earlier program covers, so earlier slots survive it.
	for (u32 i = 0; i < kPrograms; i++)
	{
		value[i] = ValueFor(i, 0);
		WriteProgram(v, v.ProgramPc(i), value[i]);
		v.Cpu()->Clear(v.ProgramPc(i), kProgramBytes);
		RunAndCheck(v, i, value[i], "setup");
		BAIL_ON_FATAL();
	}
	ASSERT_EQ(mvu_test_hooks::QuickSlotCount(vu_index), kPrograms);

	std::mt19937 rng(0xC1EA2u + static_cast<u32>(vu_index));
	auto pick = [&](u32 n) { return static_cast<u32>(rng() % n); };

	for (u32 step = 0; step < 400; step++)
	{
		// Choose a window. Half the steps rewrite one program's first pair
		// under a window that covers it; the rest are bare clears, half of
		// them anchored near a program start so range edges get exercised.
		u32 addr, size;
		s32 rewrite = -1;
		if (pick(2) == 0)
		{
			rewrite = static_cast<s32>(pick(kPrograms));
			const u32 pc = v.ProgramPc(rewrite);
			const u32 back = std::min(pc, 8u * pick(4));
			addr = pc - back;
			size = back + 8u + 8u * pick(0x80);
		}
		else
		{
			static const u32 kSizes[] = {8, 16, 24, 0x40, 0x100, 0x400, 0x800};
			size = kSizes[pick(7)];
			if (pick(2) == 0)
			{
				const u32 pc = v.ProgramPc(pick(kPrograms));
				addr = pc - std::min(pc, 8u * pick(6));
			}
			else
			{
				addr = 8u * pick(v.MicroBytes() / 8);
			}
		}
		if (addr + size > v.MicroBytes())
			size = v.MicroBytes() - addr;

		if (rewrite >= 0)
		{
			generation[rewrite]++;
			value[rewrite] = ValueFor(rewrite, generation[rewrite]);
			WriteProgram(v, v.ProgramPc(rewrite), value[rewrite]);
		}

		ClearAndCompare(v, addr, size, "step", expect, stats);
		BAIL_ON_FATAL();

		// Re-dispatch whatever was dropped, and the rewritten program, which
		// must now run its NEW code: a slot the clear missed runs the old one.
		for (u32 i = 0; i < kPrograms; i++)
		{
			if (!expect[i] || static_cast<s32>(i) == rewrite)
			{
				RunAndCheck(v, i, value[i], "redispatch");
				BAIL_ON_FATAL();
			}
		}
		// And one program through its quick slot (a survivor, or one just
		// re-installed), which must still run its current code.
		const u32 any = pick(kPrograms);
		RunAndCheck(v, any, value[any], "quick hit");
		BAIL_ON_FATAL();
		ASSERT_EQ(mvu_test_hooks::QuickSlotCount(vu_index), kPrograms);
	}

	// The windows must have exercised both outcomes, or the comparison above
	// proves nothing.
	EXPECT_GT(stats.dropped, 100u);
	EXPECT_GT(stats.kept, 100u);

	ClearAndCompare(v, 0, v.MicroBytes(), "whole memory", expect, stats);
	BAIL_ON_FATAL();
	EXPECT_EQ(mvu_test_hooks::QuickSlotCount(vu_index), 0u);

	v.Cpu()->Clear(0x40, 8);
	EXPECT_EQ(mvu_test_hooks::QuickSlotCount(vu_index), 0u);
	for (u32 i = 0; i < kPrograms; i++)
	{
		RunAndCheck(v, i, value[i], "reinstall");
		BAIL_ON_FATAL();
	}
	ASSERT_EQ(mvu_test_hooks::QuickSlotCount(vu_index), kPrograms);

	RecompilerTestEnvironment::ResetVuBlockCache(vu_index);
	EXPECT_EQ(mvu_test_hooks::QuickSlotCount(vu_index), 0u);
	EXPECT_TRUE(mvu_test_hooks::QuickOccupancyMatchesScan(vu_index));
	for (u32 i = 0; i < kPrograms; i++)
	{
		RunAndCheck(v, i, value[i], "after reset");
		BAIL_ON_FATAL();
	}
	ASSERT_EQ(mvu_test_hooks::QuickSlotCount(vu_index), kPrograms);
	ClearAndCompare(v, v.ProgramPc(3), 8, "after reset", expect, stats);
	BAIL_ON_FATAL();
	EXPECT_FALSE(mvu_test_hooks::QuickSlotOccupied(vu_index, v.ProgramPc(3)));
}

} // namespace

TEST(MvuClearOccupied, Vu1DropsTheSameSlotsAsAFullScan)
{
	RunScenario(1);
}

TEST(MvuClearOccupied, Vu0DropsTheSameSlotsAsAFullScan)
{
	RunScenario(0);
}

} // namespace recompiler_tests
