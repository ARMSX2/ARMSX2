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
