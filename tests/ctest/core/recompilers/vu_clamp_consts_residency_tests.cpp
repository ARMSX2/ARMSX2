// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Micro-mode clamp-bound residency (q25/q26).
//
// mVUclamp1 reads its two bounds out of qmmClampMax/qmmClampMin rather than
// loading them per site. What keeps those registers holding the bounds is
// mVUemitClampConsts: one Ldp at the top of every block's codegen, and one
// more behind mVUrestoreRegs, which follows every C call a block resumes
// from. A missing establishment clamps against whatever the register happened
// to hold, so these count the emissions.
//
// The counter is what has to be pinned rather than a clamped value: the C
// callees on the resume seam do not in fact write v25/v26 today, so a value
// test passes with the seam's establishment deleted. Only a callee that
// clobbers makes the difference visible, and that is not something a test can
// arrange.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

// PCSX2_RECOMPILER_TESTS hook, global scope — see microVU_Clamp-arm64.inl.
extern u32 g_mvuClampConstEstablishCount;

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }
inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

// Two clamping FMACs and nothing else: at vuClampMode 2 each carries three
// mVUclamp1 sites, and all six read the same two registers.
std::vector<VuOp> ClampingBody()
{
	return {
		UpperOnly(VMUL_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
		UpperOnly(VADD_U(mask::xyzw, vf::vf4, vf::vf3, vf::vf2)),
	};
}

u32 CompileAndCount(int clampMode, bool withXgkick)
{
	VuTestHarness h(1);
	h.SetVuClampMode(clampMode);
	h.SetDiffMode(VuDiffMode::XgkickPacketEquivalent);
	h.SetVf(vf::vf1, 1.5f, -2.25f, 3.0f, 0.0625f);
	h.SetVf(vf::vf2, 4.0f, 0.5f, -1.0f, 8.0f);
	h.SetVi(vi::vi5, 0);

	std::vector<VuOp> prog = ClampingBody();
	if (withXgkick)
		prog.push_back(LowerOnly(VXGKICK_L(vi::vi5)));
	prog.push_back(EBitNopPair());

	h.LoadProgram(prog);
	const u32 before = g_mvuClampConstEstablishCount;
	h.Run();
	return g_mvuClampConstEstablishCount - before;
}

} // namespace

// A block with no C-call seam in it establishes the bounds exactly once, at
// its entry, however many clamps it goes on to emit.
TEST(VuClampConstsResidency, BlockEntryEstablishesOnce)
{
	EXPECT_EQ(CompileAndCount(2, /*withXgkick*/ false), 1u);
}

// XGKICK's helper is a plain AAPCS call, so the block resumes from it with
// q25/q26 clobbered and mVUrestoreRegs has to lay them down again.
TEST(VuClampConstsResidency, ResumeSeamEstablishesAgain)
{
	EXPECT_EQ(CompileAndCount(2, /*withXgkick*/ true), 2u);
}

// Nothing to establish where nothing clamps.
TEST(VuClampConstsResidency, ClampModeZeroEstablishesNothing)
{
	EXPECT_EQ(CompileAndCount(0, /*withXgkick*/ false), 0u);
	EXPECT_EQ(CompileAndCount(0, /*withXgkick*/ true), 0u);
}

} // namespace recompiler_tests
