// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The single-lane result clamp must leave lanes 1-3 of its register alone.
//
// A single-dest-lane FMAC rotates the live lane to lane 0 (shuffleSSto0),
// clamps and operates there, then rotates the siblings back (shuffleSSfrom0)
// into the register the allocator still caches. Those siblings sit in lanes
// 1-3 across the clamp. An aarch64 32-bit scalar FP op zeroes the dest V
// register's upper lanes, unlike x86 MIN.SS / MAX.SS, so a clamp written that
// way leaves the cached register carrying zeros where the other lanes were and
// the next op to read it from cache reads them. That is SoulCalibur III's
// vuClampMode 2 SPS.
//
// MADD at vuClampMode 2 reaches it: mVUclamp3 clamps ACC as NEON_ADDSS's
// `from` operand, which for a Y/Z/W dest lands between the two rotations and
// for an X dest writes the cached ACC with no rotation in the way at all. So
// each program here seeds ACC, runs a single-lane MADD over it, then reads ACC
// back through a full-width MADD. The read-back has to come from the register
// the middle op left behind: MADD does not write ACC, so its slot stays clean,
// is never stored, and memory still holds the seed.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace vu;

namespace {

// I-bit set so the zero lower word is suppressed rather than decoding as LQ.
inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }

// ACC ← Fs * Ft over every lane.
inline VuOp Mula(u32 fs, u32 ft) { return UpperOnly(VMULA_U(mask::xyzw, fs, ft)); }
// Fd.<mask> ← ACC.<mask> + Fs.<mask> * Ft.<mask>.
inline VuOp Madd(u32 m, u32 fd, u32 fs, u32 ft) { return UpperOnly(VMADD_U(m, fd, fs, ft)); }

constexpr u32 k1 = 0x3F800000u; // 1.0
constexpr u32 k2 = 0x40000000u; // 2.0
constexpr u32 k3 = 0x40400000u; // 3.0
constexpr u32 k4 = 0x40800000u; // 4.0
constexpr u32 k5 = 0x40A00000u; // 5.0
constexpr u32 k6 = 0x40C00000u; // 6.0
constexpr u32 k7 = 0x40E00000u; // 7.0

// Seed ACC = [2,3,4,5], run a single-lane MADD at `m`, then read ACC into vf6
// with a full-width MADD whose product is 1. vf6 must come back as seed + 1.
void RunLaneProgram(VuTestHarness& h, u32 m)
{
	h.SetVuClampMode(2);
	h.SetVfBits(vf::vf1, k2, k3, k4, k5); // ACC seed
	h.SetVfBits(vf::vf2, k1, k1, k1, k1);
	h.SetVfBits(vf::vf3, k7, k7, k7, k7); // single-lane MADD operands
	h.SetVfBits(vf::vf4, k1, k1, k1, k1);
	h.SetVfBits(vf::vf7, k1, k1, k1, k1); // the read-back's +1 product
	h.LoadProgram({
		Mula(vf::vf1, vf::vf2),
		Madd(m, vf::vf5, vf::vf3, vf::vf4),
		Madd(mask::xyzw, vf::vf6, vf::vf7, vf::vf7),
		EBitNopPair(),
	});
	h.Run();
}

// ACC as the read-back saw it, one added to every lane.
void ExpectAccSurvived(VuTestHarness& h, u32 x, u32 y, u32 z, u32 w)
{
	const VURegs& j = h.JitSnapshot().regs;
	const VURegs& i = h.InterpSnapshot().regs;
	EXPECT_EQ(i.VF[vf::vf6].UL[0], x) << "interp oracle vf6.x";
	EXPECT_EQ(i.VF[vf::vf6].UL[1], y) << "interp oracle vf6.y";
	EXPECT_EQ(i.VF[vf::vf6].UL[2], z) << "interp oracle vf6.z";
	EXPECT_EQ(i.VF[vf::vf6].UL[3], w) << "interp oracle vf6.w";
	EXPECT_EQ(j.VF[vf::vf6].UL[0], x) << "jit vf6.x";
	EXPECT_EQ(j.VF[vf::vf6].UL[1], y) << "jit vf6.y";
	EXPECT_EQ(j.VF[vf::vf6].UL[2], z) << "jit vf6.z";
	EXPECT_EQ(j.VF[vf::vf6].UL[3], w) << "jit vf6.w";
}

} // namespace

// A Y-lane MADD rotates ACC by one; x/z/w ride in lanes 1-3 across the clamp.
TEST(VuSsClampLane, MaddYLeavesTheOtherAccLanes)
{
	VuTestHarness h(0);
	RunLaneProgram(h, mask::y);
	ExpectAccSurvived(h, k3, k4, k5, k6);
}

TEST(VuSsClampLane, MaddZLeavesTheOtherAccLanes)
{
	VuTestHarness h(0);
	RunLaneProgram(h, mask::z);
	ExpectAccSurvived(h, k3, k4, k5, k6);
}

TEST(VuSsClampLane, MaddWLeavesTheOtherAccLanes)
{
	VuTestHarness h(0);
	RunLaneProgram(h, mask::w);
	ExpectAccSurvived(h, k3, k4, k5, k6);
}

// X needs no rotation at all, so the clamp writes the cached ACC in place --
// the two shuffles that carry the damage in the other three are not even in
// the way here.
TEST(VuSsClampLane, MaddXLeavesTheOtherAccLanes)
{
	VuTestHarness h(0);
	RunLaneProgram(h, mask::x);
	ExpectAccSurvived(h, k3, k4, k5, k6);
}

} // namespace recompiler_tests
