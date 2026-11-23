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

// A lower op with an immediate (here IADDIU) under the hack: the immediate is
// read from micro memory at run time, the rest of the word is compiled in.
// `word` goes in pair 0; vi2 and vi3 are the two possible sources.
void LoadImmediateOp(VuTestHarness& h, u32 word)
{
	h.SetVi(vi::vi2, 0x10);
	h.SetVi(vi::vi3, 0x20);
	h.LoadProgram({
		VuOp{word, VNOP_U()},
		EBitNopPair(),
	});
}

void RewriteWord0(u32 word)
{
	std::memcpy(vuRegs[1].Micro + VuTestHarness::kProgramPc, &word, sizeof(word));
	CpuMicroVU1.Clear(VuTestHarness::kProgramPc, sizeof(word));
}

// Run(), then JIT re-runs until the block stops compiling. Returns the
// compile count at that point.
u64 WarmUp(VuTestHarness& h)
{
	h.Run();
	h.RunJitPreserveBlockCache();
	const u64 warm = mVUPersist::GetBlockCompileCount(1);
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), warm) << "block never went warm";
	return warm;
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

// Hack ON: a rewrite of a lower op's immediate keeps the block, which reads
// the new immediate.
TEST(MvuIbitHack, RewrittenLowerImmediateReachesTheReusedBlock)
{
	ScopedIbitHack hack(true);

	VuTestHarness h(1);
	LoadImmediateOp(h, VIADDIU_L(vi::vi1, vi::vi2, 5));
	const u64 warm = WarmUp(h);
	ASSERT_EQ(h.GetViJit(vi::vi1), 0x15u);

	RewriteWord0(VIADDIU_L(vi::vi1, vi::vi2, 7));
	h.RunJitPreserveBlockCache();

	EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), warm)
		<< "only the immediate changed, so the block should have been reused";
	EXPECT_EQ(h.GetViJit(vi::vi1), 0x17u);
}

// Hack ON: the hack leaves the immediate out of the compare, not the rest of
// the word. A rewrite of the source register field must reach the program.
TEST(MvuIbitHack, RewrittenLowerRegisterFieldIsNotIgnored)
{
	ScopedIbitHack hack(true);

	VuTestHarness h(1);
	LoadImmediateOp(h, VIADDIU_L(vi::vi1, vi::vi2, 5));
	WarmUp(h);
	ASSERT_EQ(h.GetViJit(vi::vi1), 0x15u);

	RewriteWord0(VIADDIU_L(vi::vi1, vi::vi3, 5));
	h.RunJitPreserveBlockCache();

	EXPECT_EQ(h.GetViJit(vi::vi1), 0x25u)
		<< "the block compiled for vi2 ran after the word was rewritten to read vi3";
}

// Hack ON: a kept-bit word in the middle of a range. Immediate rewrites keep
// the block, a register-field rewrite after them still reaches the program,
// and so does a rewrite of an ordinary word in the same range.
TEST(MvuIbitHack, KeptWordInsideARangeComparesItsCompiledBits)
{
	ScopedIbitHack hack(true);

	const auto rewriteLower = [](u32 pair, u32 word) {
		std::memcpy(vuRegs[1].Micro + pair * 8, &word, sizeof(word));
		CpuMicroVU1.Clear(pair * 8, sizeof(word));
	};

	VuTestHarness h(1);
	h.SetVi(vi::vi2, 0x10);
	h.SetVi(vi::vi3, 0x20);
	h.LoadProgram({
		VuOp{VIOR_L(vi::vi0, vi::vi0, vi::vi0), VNOP_U()},
		VuOp{VIADDIU_L(vi::vi1, vi::vi2, 5), VNOP_U()},
		EBit(VuOp{VIOR_L(vi::vi0, vi::vi0, vi::vi0), VNOP_U()}),
	});
	const u64 warm = WarmUp(h);
	ASSERT_EQ(h.GetViJit(vi::vi1), 0x15u);

	rewriteLower(1, VIADDIU_L(vi::vi1, vi::vi2, 7));
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), warm) << "only the immediate changed";
	EXPECT_EQ(h.GetViJit(vi::vi1), 0x17u);

	rewriteLower(1, VIADDIU_L(vi::vi1, vi::vi2, 9));
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), warm) << "only the immediate changed";
	EXPECT_EQ(h.GetViJit(vi::vi1), 0x19u);

	rewriteLower(1, VIADDIU_L(vi::vi1, vi::vi3, 9));
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(h.GetViJit(vi::vi1), 0x29u)
		<< "the block compiled for vi2 ran after the word was rewritten to read vi3";

	rewriteLower(0, VIOR_L(vi::vi5, vi::vi2, vi::vi0));
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(h.GetViJit(vi::vi5), 0x10u) << "the rewrite of pair 0 did not reach the program";
}

} // namespace recompiler_tests
