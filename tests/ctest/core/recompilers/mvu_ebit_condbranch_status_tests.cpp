// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The final STATUS of a VU1 program that ends with the E bit on a
// conditional branch.
//
// The program stops after the branch's delay slot, so the branch target never
// runs. microVU still decides which status instance the block leaves behind
// from a flag lookahead that walks into that target: when the target's first
// pairs read STATUS (FSAND), the block ends with an older instance than the
// one it would leave if the target did not read it. The final STATUS then
// depends on code that never executes.
//
// This runs the same program twice, with and without a status reader in the
// unexecuted target, and expects the same final STATUS from both, matching
// the interpreter. It is red on microVU today; microVU is not fixed here.
//
// No title in the VU capture corpus reaches this: every E-bit conditional
// branch in it is followed by code that does not read STATUS.

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"
#include "harness/VuEncode.h"

#include <gtest/gtest.h>

#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp U(u32 upper) { return VuOp{0x8000033c, upper}; }
inline VuOp L(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp P(u32 lower, u32 upper) { return VuOp{lower, upper}; }

struct Status
{
	u32 jit;
	u32 interp;
};

// gap = FMACs between the second status producer and the branch pair.
Status RunEbitBranch(bool targetReadsStatus, int gap)
{
	VuTestHarness h(1);
	h.SetVf(1, 1.0f, 2.0f, 3.0f, 4.0f);
	h.SetVf(2, -5.0f, 2.0f, 3.0f, 4.0f);
	std::vector<VuOp> p;
	p.push_back(U(VSUB_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)));
	p.push_back(U(VADD_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf1)));
	for (int g = 0; g < gap; g++)
		p.push_back(U(VMUL_U(mask::xyzw, vf::vf5, vf::vf1, vf::vf2)));
	// E bit on a taken conditional branch: the program ends after the delay slot.
	p.push_back(EBit(P(VIBEQ_L(vi::vi0, vi::vi0, 2), VSUB_U(mask::xyzw, vf::vf6, vf::vf2, vf::vf1))));
	p.push_back(U(VADD_U(mask::x, vf::vf7, vf::vf1, vf::vf1))); // delay slot
	// Branch target: never executed.
	p.push_back(targetReadsStatus ? L(VFSAND_L(vi::vi5, 0x3)) : U(VNOP_U()));
	p.push_back(U(VNOP_U()));
	p.push_back(EBit(targetReadsStatus ? L(VFSAND_L(vi::vi6, 0x3)) : U(VNOP_U())));
	h.LoadProgram(p);
	h.RunNoDiff();
	return {h.GetViJit(REG_STATUS_FLAG), h.GetViInterp(REG_STATUS_FLAG)};
}

} // namespace

TEST(MvuEbitCondBranchStatus, FinalStatusIgnoresUnexecutedTarget)
{
	for (int gap = 0; gap < 4; gap++)
	{
		const Status plain = RunEbitBranch(false, gap);
		const Status read = RunEbitBranch(true, gap);
		// The control: with no reader in the target, microVU agrees with the interpreter.
		EXPECT_EQ(plain.jit, plain.interp) << "gap=" << gap;
		EXPECT_EQ(read.interp, plain.interp) << "gap=" << gap;
		EXPECT_EQ(read.jit, plain.jit) << "gap=" << gap << ": a status reader in the unexecuted branch target changed the final STATUS";
	}
}

} // namespace recompiler_tests
