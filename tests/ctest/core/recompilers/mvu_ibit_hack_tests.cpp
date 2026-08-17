// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The I immediate under the IbitHack gamefix.
//
// An upper instruction with the I bit set turns its pair's lower word into a
// 32-bit float that lands in VI[REG_I]. Games that rewrite that word in place
// between runs of an otherwise-identical microprogram would make microVU
// recompile on every object, so IbitHack takes the word out of the range
// mVUsetupRange hands the program compare — the block is reused across the
// rewrite. A compile-time fold of the immediate therefore has to become a
// run-time load of it, or every reuse runs on the constant the first compile
// happened to see.
//
// Scarface (SLUS-21111, the game the gamefix is listed for) writes a per-object
// transform into VU1 micro memory this way: consecutive dispatches of the same
// start_pc differ in ~60 I immediates and in no instruction word.

#include "harness/VuTestHarness.h"

#include "VU.h"
#include "VUmicro.h"
#include "Config.h"
#include "arm64/microVU_Persist-arm64.h"

#include <gtest/gtest.h>

#include <cstring>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }

// Two immediates a run apart, both exactly representable so the products are
// exact and the comparison is on bits.
constexpr u32 kImmA = 0x40000000u; // 2.0f
constexpr u32 kImmB = 0x41200000u; // 10.0f
constexpr u32 kSeed = 0x3f800000u; // 1.0f

// Byte offset of pair 0's lower word — the I immediate the test rewrites.
constexpr u32 kImmAddr = VuTestHarness::kProgramPc;

struct ScopedIbitHack
{
	bool prev;
	explicit ScopedIbitHack(bool on) : prev(EmuConfig.Gamefixes.IbitHack) { EmuConfig.Gamefixes.IbitHack = on; }
	~ScopedIbitHack() { EmuConfig.Gamefixes.IbitHack = prev; }
};

// vf2 = vf1 * I, with I loaded by the pair before it (both microVU and the
// interpreter write VI[REG_I] after the pair's own upper op, so the multiply
// reads the previous pair's immediate).
void LoadIThenMultiply(VuTestHarness& h)
{
	h.SetVfBits(vf::vf1, kSeed, kSeed, kSeed, kSeed);
	h.LoadProgram({
		IBit(VuOp{VLitI(kImmA), VNOP_U()}),                       // VI[REG_I] <- kImmA
		UpperOnly(VMULi_U(mask::xyzw, vf::vf2, vf::vf1)),         // vf2 = vf1 * I
		EBitNopPair(),
	});
}

// Rewrite the immediate the way a game does: store, then run the invalidation
// the vtlb write path would have run. mVUclear is range-aware, so whether this
// invalidates anything is exactly the question the gamefix decides.
void RewriteImmediate(int vu_index, u32 bits)
{
	std::memcpy(vuRegs[vu_index].Micro + kImmAddr, &bits, sizeof(bits));
	CpuMicroVU1.Clear(kImmAddr, sizeof(bits));
}

} // namespace

// Hack ON: the rewritten immediate must reach the reused block.
TEST(MvuIbitHack, RewrittenImmediateReachesTheReusedBlock)
{
	ScopedIbitHack hack(true);

	VuTestHarness h(1);
	LoadIThenMultiply(h);
	h.Run();
	ASSERT_EQ(h.GetVfBitsJit(vf::vf2, 'x'), 0x40000000u) << "1.0 * 2.0";
	ASSERT_EQ(h.GetVfBitsInterp(vf::vf2, 'x'), 0x40000000u);

	// The first re-entry compiles a second block variant: it enters on the
	// pipeline state the previous run left behind, not the post-Reset one.
	// From the second re-entry on that variant is warm, which is the state
	// this test needs before it can attribute anything to the rewrite.
	h.RunJitPreserveBlockCache();
	const u64 warm = mVUPersist::GetBlockCompileCount(1);
	h.RunJitPreserveBlockCache();
	ASSERT_EQ(mVUPersist::GetBlockCompileCount(1), warm) << "block never went warm";
	ASSERT_EQ(h.GetVfBitsJit(vf::vf2, 'x'), 0x40000000u);

	// Only the immediate changes; every instruction word stays put, which is
	// what keeps the block out of a recompile.
	RewriteImmediate(h.VuIndex(), kImmB);
	h.RunJitPreserveBlockCache();

	ASSERT_EQ(mVUPersist::GetBlockCompileCount(1), warm)
		<< "the rewritten word is supposed to be outside the compared range; "
		   "a recompile here makes the assertion below vacuous";
	EXPECT_EQ(h.GetVfBitsJit(vf::vf2, 'x'), 0x41200000u) << "1.0 * 10.0";
}

// Hack OFF (control): the immediate is inside the compared range, so the
// rewrite forces a recompile and the same value has to come out that way.
TEST(MvuIbitHack, RewrittenImmediateForcesRecompileWithoutTheHack)
{
	ScopedIbitHack hack(false);

	VuTestHarness h(1);
	LoadIThenMultiply(h);
	h.Run();
	ASSERT_EQ(h.GetVfBitsJit(vf::vf2, 'x'), 0x40000000u);

	h.RunJitPreserveBlockCache();
	const u64 warm = mVUPersist::GetBlockCompileCount(1);
	h.RunJitPreserveBlockCache();
	ASSERT_EQ(mVUPersist::GetBlockCompileCount(1), warm) << "block never went warm";

	RewriteImmediate(h.VuIndex(), kImmB);
	h.RunJitPreserveBlockCache();

	EXPECT_GT(mVUPersist::GetBlockCompileCount(1), warm)
		<< "without the hack the word is compared, so it must force a recompile";
	EXPECT_EQ(h.GetVfBitsJit(vf::vf2, 'x'), 0x41200000u);
}

} // namespace recompiler_tests
