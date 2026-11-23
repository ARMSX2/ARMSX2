// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The words the flag pass reads are in the program's compared ranges.
//
// At a branch, mVUsetFlagInfo scans the first instructions of each successor
// for flag reads, and the block elides the MAC and status writes no successor
// reads. mVUsetupRange records only what was compiled, so a scanned word that
// no compile covers would neither invalidate the program in mVUclear nor fail
// mVUcmpProg, and a rewrite there could pair a block's flag elision with
// successor code it was not decided for.
//
// No such word survives a compile today. The scan stops where a successor's
// block stops (a branch, the E bit, the T bit), it recurses only into the
// targets of branches it meets, and every branch compiles its successors with
// it: normBranchCompile for B and BAL and the not-taken side, mVUblockFetch
// for the taken side of IBcc, both at compile time. So the side of a
// conditional branch that has not run yet is already compiled and in range,
// which this test pins.
//
// The branches that compile no successor end the program. With the E bit the
// program ends after the delay slot and the target is the next program's
// entry: its words only change how many of the block's tail flag writes are
// emitted, and the end stores one instance of each flag (needFlagFinalize and
// getLastFlagInst), which the next entry reloads into every ring slot. An M bit
// on a branch exits with TPC at the target, and the target is compiled into
// the program on the resume, before any later run of the block.

#include "harness/VuTestHarness.h"

#include "VU.h"
#include "VUmicro.h"

#include <gtest/gtest.h>

#include <cstring>

// Test hook exported by pcsx2/arm64/microVU-arm64.cpp under
// PCSX2_RECOMPILER_TESTS.
namespace mvu_test_hooks
{
	bool QuickSlotOccupied(int vu_index, u32 start_pc_bytes);
} // namespace mvu_test_hooks

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
// No I bit, so the lower word is an instruction and can be rewritten into one.
inline VuOp BareNopPair() { return VuOp{0, VNOP_U()}; }

constexpr u32 kNegOne = 0xBF800000u; // -1.0f

// Pair index of the branch's taken side.
constexpr u32 kTaken = 18;

// vf3 = vf1 + vf2 sets the MAC sign bits; ILW picks the branch side from data
// memory, so the side can change between runs without touching micro memory.
// Each side writes flags of its own (vf4 = vf0 + vf0) with four more pairs
// after it before its E bit, so the flag pass stops short of the E bit (which
// would force the ADD's flag write) and, finding no flag read on either side,
// elides the ADD's MAC write. Pairs 15-17 separate the taken side from the
// fall-through side's range.
void LoadProgram(VuTestHarness& h)
{
	const VuOp sideFmac = VuOp{0, VADD_U(mask::xyzw, vf::vf4, vf::vf0, vf::vf0)};
	std::vector<VuOp> p;
	p.push_back(VuOp{VILW_L(mask::x, vi::vi1, vi::vi0, 0), VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)}); // 0
	for (int i = 0; i < 4; i++)
		p.push_back(BareNopPair());                                     // 1-4
	p.push_back(LowerOnly(VIBNE_L(vi::vi1, vi::vi0, kTaken - 6)));     // 5: taken -> kTaken
	p.push_back(BareNopPair());                                         // 6: delay slot
	p.push_back(sideFmac);                                              // 7: fall-through side
	for (int i = 0; i < 5; i++)
		p.push_back(BareNopPair());                                     // 8-12
	p.push_back(EBit(BareNopPair()));                                   // 13
	p.push_back(BareNopPair());                                         // 14: E-bit delay slot
	for (int i = 0; i < 3; i++)
		p.push_back(BareNopPair());                                     // 15-17
	p.push_back(BareNopPair());                                         // 18: taken side; FMAND goes here
	p.push_back(sideFmac);                                              // 19
	for (int i = 0; i < 5; i++)
		p.push_back(BareNopPair());                                     // 20-24
	p.push_back(EBit(BareNopPair()));                                   // 25
	h.LoadProgram(std::move(p));
}

// Data memory word 0 is the ILW source. Written directly, not through the
// harness, so a run's pre-state restore does not put it back.
void SelectSide(int vu_index, u32 taken)
{
	std::memcpy(vuRegs[vu_index].Mem, &taken, sizeof(taken));
}

} // namespace

TEST(MvuFlagLookaheadRange, NotYetRunBranchSideIsInTheProgramRanges)
{
	VuTestHarness h(1);
	h.SetVfBits(vf::vf1, kNegOne, kNegOne, kNegOne, kNegOne);
	h.SetVfBits(vf::vf2, 0, 0, 0, 0);
	h.SetVi(vi::vi3, 0xFFFF);
	LoadProgram(h);
	SelectSide(h.VuIndex(), 0);

	// Run the program on the fall-through side only and keep it cached.
	h.Run();
	h.RunJitPreserveBlockCache();
	ASSERT_EQ(h.GetViJit(vi::vi2), 0u);
	ASSERT_TRUE(mvu_test_hooks::QuickSlotOccupied(1, 0));

	// Put a MAC read at the head of the taken side, which has never run.
	const u32 fmand = VFMAND_L(vi::vi2, vi::vi3);
	std::memcpy(vuRegs[1].Micro + kTaken * 8, &fmand, sizeof(fmand));
	CpuMicroVU1.Clear(kTaken * 8, sizeof(fmand));
	EXPECT_FALSE(mvu_test_hooks::QuickSlotOccupied(1, 0))
		<< "the taken side was compiled with the branch, so the write overlaps the program";

	SelectSide(h.VuIndex(), 1);
	h.RunJitPreserveBlockCache();
	const u32 cached = h.GetViJit(vi::vi2);

	// The same image from an empty cache, on both engines, entered with MAC
	// clear so only the ADD can put the sign bits there.
	h.SetVi(REG_MAC_FLAG, 0);
	h.SetVi(vi::vi2, 0);
	h.Run();
	ASSERT_EQ(h.GetViInterp(vi::vi2), 0xF0u) << "FMAND reads the ADD's four sign bits";
	ASSERT_EQ(h.GetViJit(vi::vi2), 0xF0u);

	EXPECT_EQ(cached, 0xF0u);
}

} // namespace recompiler_tests
