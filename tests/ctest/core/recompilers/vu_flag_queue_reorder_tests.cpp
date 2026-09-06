// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// What the block-link MAC flag queue reorder emits, which its result cannot
// show: an identity reorder writes back the four instances it read, so the
// program that provokes one runs the same whether the emitter skips it or
// loads and stores them again, and every reorder that does move lanes lands
// the same four words whichever shuffle opened the sequence. The emitter
// counts what it did and these read the counts. Values are still checked —
// VuTestHarness::Run diffs the whole architectural state against the
// interpreter — so a reorder that emits the wrong lanes fails here too.
//
// The lane map a link asks for is set by where the block's MAC writes fall.
// The queue holds four instances and a write lands in the next ring slot, so
// four writes filling the last four cycles of a block leave slot k holding the
// instance the successor wants k-deep: the identity. Move the writes one cycle
// earlier and every instance shifts a slot, which is a rotate; give the block
// fewer than four and the older slots hold nothing the successor can use and
// the map collapses onto one instance. Both of the loops below put a
// MAC-writing upper in the branch pair and in its delay slot, so the tail
// reaches the block end, and differ only in how long that tail is.
//
// See vu0_flag_link_reorder_tests.cpp for the correctness of the reordering
// itself, which is what those loops were built for.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <vector>

extern u32 g_mvuFlagQueueIdentities;
extern u32 g_mvuFlagQueueReorders;
extern u32 g_mvuFlagQueueWorst;

namespace recompiler_tests
{

using namespace vu;

namespace
{
inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp UpperOnly(u32 upper) { return VuOp{0, upper}; }
inline VuOp LoadViImm(u32 dst, u32 imm) { return LowerOnly(VIADDIU_L(dst, vi::vi0, imm)); }
inline u32 MacWrite(u32 src) { return VADD_U(mask::xyzw, vf::vf1, src, vf::vf0); }

// A self-loop whose top reads the MAC flag — which is what makes the back edge
// an exact-match link, the only kind that reorders anything — and whose last
// `tail` pairs each write MAC, the last two being the branch and its delay
// slot.
std::vector<VuOp> MacTailLoop(int tail)
{
	std::vector<VuOp> p;
	p.push_back(LoadViImm(vi::vi2, 3));                     // 0: counter
	p.push_back(LoadViImm(vi::vi3, 0xFFF));                 // 1: mask
	p.push_back(LowerOnly(VFMAND_L(vi::vi1, vi::vi3)));     // 2: reads MAC [target]
	p.push_back(LowerOnly(VISUBIU_L(vi::vi2, vi::vi2, 1))); // 3: counter -= 1
	p.push_back(LowerOnly(0));                              // 4: VI hazard pad
	p.push_back(LowerOnly(0));                              // 5: VI hazard pad
	for (int i = 0; i < tail - 2; i++)
		p.push_back(UpperOnly(MacWrite(vf::vf2 + (i & 3))));
	const int branch = static_cast<int>(p.size());
	p.push_back(VuOp{VIBNE_L(vi::vi2, vi::vi0, 2 - branch - 1), MacWrite(vf::vf3)});
	p.push_back(VuOp{0, MacWrite(vf::vf4)});                // delay slot
	p.push_back(EBitNopPair());
	return p;
}

struct Counts { u32 identities, reorders, worst; };

Counts RunLoop(int tail)
{
	g_mvuFlagQueueIdentities = 0;
	g_mvuFlagQueueReorders = 0;
	g_mvuFlagQueueWorst = 0;

	VuTestHarness h(0);
	h.SetVf(vf::vf2, 0.0f, 0.0f, 0.0f, 1.0f);
	h.SetVf(vf::vf3, -1.0f, -1.0f, -1.0f, -1.0f);
	h.SetVf(vf::vf4, 5.0f, 5.0f, 5.0f, 5.0f);
	h.SetVf(vf::vf5, 0.0f, -2.0f, 0.0f, 3.0f);
	h.LoadProgram(MacTailLoop(tail));
	h.Run();
	return {g_mvuFlagQueueIdentities, g_mvuFlagQueueReorders, g_mvuFlagQueueWorst};
}
} // namespace

// Four MAC writes ending in the delay slot: every slot already holds what the
// successor reads from it.
TEST(VuFlagQueueReorder, AnIdentityReorderEmitsNothing)
{
	const Counts c = RunLoop(4);
	EXPECT_GT(c.identities, 0u);
	EXPECT_EQ(c.reorders, 0u);
}

// The control for the test above: one write fewer and the same loop asks for a
// reorder, so the identity count there is the emitter declining to emit and not
// the link failing to happen.
TEST(VuFlagQueueReorder, ThreeWritesInTheTailStillReorder)
{
	const Counts c = RunLoop(3);
	EXPECT_GT(c.reorders, 0u);
	EXPECT_EQ(c.identities, 0u);
}

// Three writes leave the queue rotated by one, two collapse it onto two
// instances, and each loop also links a map one lane off the shuffle that would
// match it. Opening with a copy the rotate is five instructions and the map
// beside it four; opening with the table nothing here is more than two.
TEST(VuFlagQueueReorder, NoReorderCostsMoreThanTwoInstructions)
{
	EXPECT_EQ(RunLoop(3).worst, 2u);
	EXPECT_EQ(RunLoop(2).worst, 2u);
}
} // namespace recompiler_tests
