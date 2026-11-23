// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// A branch in another branch's delay slot at the top of VU1 micro memory.
//
// The first branch's target runs as the delay slot of the second, and when
// the second is not taken the program continues one pair after that target.
// microVU computes that continuation at run time from the target it stored
// (condEvilBranch), and BAL and JALR in the delay slot link it. Fetches wrap
// at 16 KiB, so a target in the last pair continues at pair 0; the link does
// not wrap.

#include "harness/VuTestHarness.h"

#include "VU.h"
#include "VUmicro.h"

#include <gtest/gtest.h>

#include <cstring>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

constexpr u32 kLastPair = 0x7FF;

void WritePair(u32 pair, const VuOp& op)
{
	std::memcpy(vuRegs[1].Micro + pair * 8 + 0, &op.lower, 4);
	std::memcpy(vuRegs[1].Micro + pair * 8 + 4, &op.upper, 4);
}

// Pairs 0-2: the code after the wrap. Pairs 0x7FC-0x7FF: B to the last pair
// with `delaySlot` in its delay slot; the last pair sets vi2.
void LoadWrapProgram(VuTestHarness& h, const VuOp& delaySlot)
{
	h.LoadProgram({
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 0x33)), // pair 0: reached through the wrap
		EBitNopPair(),                                // pair 1
	});
	WritePair(kLastPair - 3, LowerOnly(VB_L(+2)));                         // B to the last pair
	WritePair(kLastPair - 2, delaySlot);                                   // its delay slot
	WritePair(kLastPair - 1, LowerOnly(VIADDIU_L(vi::vi4, vi::vi0, 0x44))); // skipped
	WritePair(kLastPair, LowerOnly(VIADDIU_L(vi::vi2, vi::vi0, 0x22)));     // the target
	h.SetStartPc((kLastPair - 3) * 8);
}

} // namespace

// IBNE not taken in B's delay slot: the target runs, then pair 0.
TEST(MvuBranchWrap, NotTakenBranchInDelaySlotContinuesAtPairZero)
{
	VuTestHarness h(1);
	h.SetVi(vi::vi1, 0);
	LoadWrapProgram(h, LowerOnly(VIBNE_L(vi::vi1, vi::vi0, +2)));
	h.Run();
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x22u);
	EXPECT_EQ(h.GetViJit(vi::vi3), 0x33u);
	EXPECT_EQ(h.GetViJit(vi::vi4), 0u);
}

// BAL and JALR in the last two pairs link their own pair + 2 without a wrap,
// as a console capture reads it: 0x800 from pair 0x7FE, 0x801 from 0x7FF. Both
// jump into pairs 0-2, and from 0x7FF the delay slot is fetched from pair 0.
TEST(MvuBranchWrap, BalAndJalrAtTheTopLinkPastIt)
{
	for (const bool jalr : {false, true})
	{
		for (const u32 pair : {kLastPair - 1, kLastPair})
		{
			SCOPED_TRACE(testing::Message() << (jalr ? "JALR" : "BAL") << " in pair 0x" << std::hex << pair);
			VuTestHarness h(1);
			const u32 target = pair - (kLastPair - 1); // pair 0 from 0x7FE, pair 1 from 0x7FF
			h.SetVi(vi::vi6, target);
			h.LoadProgram({
				LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 0x33)), // pair 0
				LowerOnly(VIADDIU_L(vi::vi4, vi::vi0, 0x44)), // pair 1
				EBitNopPair(),                                // pair 2
			});
			WritePair(pair, LowerOnly(jalr ? VJALR_L(vi::vi5, vi::vi6) : VBAL_L(vi::vi5, +1)));
			if (pair == kLastPair - 1)
				WritePair(kLastPair, LowerOnly(VIADDIU_L(vi::vi2, vi::vi0, 0x22))); // delay slot
			h.SetStartPc(pair * 8);
			h.Run();
			EXPECT_EQ(h.GetViJit(vi::vi3), 0x33u);
			EXPECT_EQ(h.GetViJit(vi::vi4), 0x44u);
			EXPECT_EQ(h.GetViInterp(vi::vi5), pair + 2);
			EXPECT_EQ(h.GetViJit(vi::vi5), pair + 2);
		}
	}
}

// The TPC an E or T stop leaves is wrapped, as a console capture reads it:
// 0 after an E bit on 0x7FE (delay slot 0x7FF) and after a T bit on 0x7FF, 1
// after an E bit on 0x7FF (delay slot fetched from pair 0).
TEST(MvuBranchWrap, StopAtTheTopLeavesTpcWrapped)
{
	struct Case
	{
		const char* name;
		u32 pair;
		VuOp op;
		u32 tpc;
	};
	const Case cases[] = {
		{"E bit on 0x7FE", kLastPair - 1, EBitNopPair(), 0},
		{"E bit on 0x7FF", kLastPair, EBitNopPair(), 1},
		{"T bit on 0x7FF", kLastPair, TBit(NopPair()), 0},
	};
	const u32 savedFbrst = vuRegs[0].VI[REG_FBRST].UL;
	for (const Case& c : cases)
	{
		SCOPED_TRACE(c.name);
		VuTestHarness h(1);
		h.LoadProgram({
			LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 0x33)), // pair 0
			EBitNopPair(),                                // pair 1
		});
		WritePair(c.pair, c.op);
		h.SetStartPc(c.pair * 8);
		vuRegs[0].VI[REG_FBRST].UL = 0x800u; // T-stop for VU1
		h.Run();
		vuRegs[0].VI[REG_FBRST].UL = savedFbrst;
		EXPECT_EQ(h.GetViInterp(REG_TPC), c.tpc);
		EXPECT_EQ(h.GetViJit(REG_TPC), c.tpc);
	}
}

// BAL or JALR in B's delay slot (both to pair 0) links the pair after B's
// target, and the link does not wrap: a console capture reads 0x800 for both.
TEST(MvuBranchWrap, BalInDelaySlotLinksPastTheTop)
{
	VuTestHarness h(1);
	LoadWrapProgram(h, LowerOnly(VBAL_L(vi::vi5, +2)));
	h.Run();
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x22u);
	EXPECT_EQ(h.GetViInterp(vi::vi5), 0x800u);
	EXPECT_EQ(h.GetViJit(vi::vi5), 0x800u);
}

TEST(MvuBranchWrap, JalrInDelaySlotLinksPastTheTop)
{
	VuTestHarness h(1);
	h.SetVi(vi::vi6, 0);
	LoadWrapProgram(h, LowerOnly(VJALR_L(vi::vi5, vi::vi6)));
	h.Run();
	EXPECT_EQ(h.GetViJit(vi::vi2), 0x22u);
	EXPECT_EQ(h.GetViJit(vi::vi3), 0x33u);
	EXPECT_EQ(h.GetViInterp(vi::vi5), 0x800u);
	EXPECT_EQ(h.GetViJit(vi::vi5), 0x800u);
}

} // namespace recompiler_tests
