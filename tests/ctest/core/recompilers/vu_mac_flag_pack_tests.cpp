// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// MAC-flag packing in mVUupdateFlags (arm64).
//
// x86 extracts the sign and zero lanes with two independent MOVMSKPS chains and
// glues them together in the GPR: AND with the dest-field mask, SHL 4, AND
// again, OR, then SHL by the single-scalar rotate. arm64 has no MOVMSKPS, so
// the port paid two AND/ADDV/UMOV sequences plus a per-site literal-pool weight
// vector — 16 instructions of flag packing around 2 instructions of FMAC.
//
// It now packs both predicates in ONE horizontal add: SLI merges CMLT's and
// FCMEQ's all-ones lanes into a single register (bits [31:4] = sign, [3:0] =
// zero) and one AND against a combined per-lane weight vector selects lane i's
// sign into bit (i+4) and its zero into bit i. The weight vector is picked at
// emit time from mVUglob.macWeights, so the dest-field mask AND *and* the
// single-scalar rotate stop existing as instructions — they are baked into the
// weights.
//
// That makes the weight table load-bearing for flag correctness in a way no
// individual emitted instruction is: get one lane's weight wrong and the MAC
// flag silently reports the wrong field. These tests pin the packed value for
// the three shapes that select different table rows — full mask, partial mask,
// single scalar (which is the only shape with a non-zero rotate to fold) —
// against the interpreter, which does the packing in scalar C.
//
// PS2 MAC flag layout: bits [3:0] = Z per field, bits [7:4] = S per field, with
// bit0 = W and bit3 = X (the reverse of NEON lane order, which is what x86 pays
// a PSHUF.D 0x1B for and we get free from the weight vector's lane→bit map).

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return VuOp{0, upper}; }
inline VuOp BareNopPair() { return VuOp{0, VNOP_U()}; }

constexpr u32 kNegOne  = 0xBF800000u; // -1.0
constexpr u32 kZero    = 0x00000000u; // +0.0
constexpr u32 kTwo     = 0x40000000u; // +2.0
constexpr u32 kNegFive = 0xC0A00000u; // -5.0

// MAC bit for a field. X is bit 3, W is bit 0.
constexpr u32 kZ_X = 1u << 3, kZ_Y = 1u << 2, kZ_Z = 1u << 1, kZ_W = 1u << 0;
constexpr u32 kS_X = 1u << 7, kS_Y = 1u << 6, kS_Z = 1u << 5, kS_W = 1u << 4;

// vf3 = vf1 + vf2 over `dest`, with vf2 seeded to +0.0 so the result lanes are
// exactly vf1's (no -0.0 anywhere, so the +0.0 addend never changes a sign).
// Trailing NOPs keep the E-bit off the FMAC itself.
u32 RunAddAndReadMac(VuTestHarness& h, u32 dest, u32 x, u32 y, u32 z, u32 w)
{
	h.SetVfBits(vf::vf1, x, y, z, w);
	h.SetVfBits(vf::vf2, kZero, kZero, kZero, kZero);
	h.LoadProgram({
		UpperOnly(VADD_U(dest, vf::vf3, vf::vf1, vf::vf2)),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_EQ(h.GetViJit(REG_MAC_FLAG), h.GetViInterp(REG_MAC_FLAG))
		<< "JIT MAC flag diverged from the interpreter oracle";
	return h.GetViJit(REG_MAC_FLAG) & 0xFFu;
}

} // namespace

// Full dest mask: every lane weighs in, forward-mask row of the table, and the
// lane→bit reversal has to land X in bit 3 rather than bit 0.
TEST(VuMacFlagPack, FullMaskPacksSignAndZeroPerField)
{
	VuTestHarness h(0);
	// (-1.0, +0.0, +2.0, -5.0) → X negative, Y zero, Z plain, W negative.
	const u32 mac = RunAddAndReadMac(h, mask::xyzw, kNegOne, kZero, kTwo, kNegFive);
	EXPECT_EQ(mac, kS_X | kZ_Y | kS_W);
}

// Complementary fields, so between the two full-mask cases every lane→bit pair
// of the forward row is exercised in both nibbles.
TEST(VuMacFlagPack, FullMaskPacksComplementaryFields)
{
	VuTestHarness h(0);
	// (+0.0, +2.0, -1.0, +0.0) → X zero, Y plain, Z negative, W zero.
	const u32 mac = RunAddAndReadMac(h, mask::xyzw, kZero, kTwo, kNegOne, kZero);
	EXPECT_EQ(mac, kZ_X | kS_Z | kZ_W);
}

// Partial dest mask: the fields outside the mask must contribute nothing. Y is
// zero and W is negative here, and both are masked out — with the mask folded
// into the weight vector, a wrong lane weight shows up as a stray flag bit
// rather than as a missing AND.
TEST(VuMacFlagPack, PartialMaskZeroesUnwrittenFields)
{
	VuTestHarness h(0);
	const u32 mac = RunAddAndReadMac(h, mask::x | mask::z, kNegOne, kZero, kTwo, kNegFive);
	EXPECT_EQ(mac, kS_X);
}

// Single scalar: the only shape where AND_XYZW is forced to 1 and the result
// lives in lane 0, so the field's real MAC bit comes from SHIFT_XYZW's rotate —
// which is now folded into the weight vector (mVUglob.macWeights.bySSShift).
// Y selects a rotate of 2, so a negative result must land in S_Y, not S_W.
TEST(VuMacFlagPack, SingleScalarFoldsFieldRotate)
{
	VuTestHarness h(0);
	const u32 mac = RunAddAndReadMac(h, mask::y, kTwo, kNegFive, kTwo, kTwo);
	EXPECT_EQ(mac, kS_Y);
}

// Same shape, zero result: exercises the low (zero) nibble of the rotated
// weight, which SLI's 4-bit insert window constrains.
TEST(VuMacFlagPack, SingleScalarZeroFoldsFieldRotate)
{
	VuTestHarness h(0);
	const u32 mac = RunAddAndReadMac(h, mask::z, kTwo, kTwo, kZero, kTwo);
	EXPECT_EQ(mac, kZ_Z);
}

} // namespace recompiler_tests
