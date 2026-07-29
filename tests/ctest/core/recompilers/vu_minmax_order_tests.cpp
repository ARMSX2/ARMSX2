// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU MAX / MINI order their operands as sign-magnitude integers, not floats.
//
// The PS2 VU has no infinity and no NaN. An exponent-0xFF word is an ordinary
// very large number and MAX has to order it like one; a denormal is an
// ordinary very small number and MINI has to order it like one. An IEEE
// float compare gets both of those wrong — it would treat 0xFF800000 as -inf
// and 0x7FC00000 as unordered — so neither engine uses one:
//
//   * the interpreter branches on "are both operands negative" and then picks
//     a signed integer min or max (VUops.cpp fp_max / fp_min);
//   * microVU flips the low 31 bits of every negative lane, which turns the
//     sign-magnitude order into a plain signed order, and then does a single
//     integer compare and select.
//
// Those are two different derivations of the same total order, which is what
// makes diffing them worth doing: an error in either is very unlikely to be
// mirrored in the other. Every case below therefore also carries the exact
// expected bit pattern, so the suite states the architectural answer rather
// than just asserting the two engines agree with each other.
//
// The value table deliberately includes both zeros. -0.0 and +0.0 compare
// equal as floats but are ordered as distinct values here, and MAX/MINI must
// return the correct one bit-for-bit.
//
// Coverage note: the whole broadcast MAX/MINI family and both scalar
// (single-destination-lane) helpers had zero coverage before this suite.

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"

#include <gtest/gtest.h>

#include <string>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }

constexpr u32 kFs = vf::vf1;
constexpr u32 kFt = vf::vf2;
constexpr u32 kFd = vf::vf3;

// Ordered ascending under the PS2's sign-magnitude rule. Names describe the
// bit pattern, not an IEEE interpretation — on the VU none of these are
// special values.
constexpr u32 kNegHuge  = 0xFF7FFFFFu; // -max finite
constexpr u32 kNegExpFF = 0xFF800000u; // exponent 0xFF, negative — a huge number here
constexpr u32 kNegOne   = 0xBF800000u; // -1.0
constexpr u32 kNegDenorm = 0x80000001u; // smallest-magnitude negative denormal
constexpr u32 kNegZero  = 0x80000000u; // -0.0
constexpr u32 kPosZero  = 0x00000000u; // +0.0
constexpr u32 kPosDenorm = 0x00000001u; // smallest-magnitude positive denormal
constexpr u32 kOne      = 0x3F800000u; // 1.0
constexpr u32 kPosExpFF = 0x7F800000u; // exponent 0xFF, positive
constexpr u32 kPosNaNish = 0x7FC00000u; // exponent 0xFF with mantissa set
constexpr u32 kPosHuge  = 0x7F7FFFFFu; // +max finite

// The reference order, used only to document the intent of each case; the
// expectations below are written out literally.
//   kNegExpFF < kNegHuge < kNegOne < kNegDenorm < kNegZero
//     < kPosZero < kPosDenorm < kOne < kPosHuge < kPosExpFF < kPosNaNish

struct MinMaxCase
{
	const char* name;
	bool is_max;
	u32 fs[4];
	u32 ft[4];
	u32 expect[4];
};

const MinMaxCase kCases[] = {
	// An exponent-0xFF word is the largest thing in the table, not an
	// infinity to be swallowed or propagated. A float MAX/MIN would still
	// get these two right; they are here as the baseline the NaN cases
	// below are contrasted against.
	{"MaxPicksExpFF", true,
		{kOne, kPosHuge, kNegOne, kNegHuge},
		{kPosExpFF, kPosExpFF, kPosExpFF, kPosExpFF},
		{kPosExpFF, kPosExpFF, kPosExpFF, kPosExpFF}},
	{"MiniPicksNegExpFF", false,
		{kNegOne, kNegHuge, kOne, kPosHuge},
		{kNegExpFF, kNegExpFF, kNegExpFF, kNegExpFF},
		{kNegExpFF, kNegExpFF, kNegExpFF, kNegExpFF}},

	// A set mantissa on top of exponent 0xFF is what IEEE calls a quiet NaN.
	// Here it is simply larger than the mantissa-zero pattern, and MAX must
	// return it rather than treating the compare as unordered.
	{"MaxOrdersNaNPatternAboveExpFF", true,
		{kPosNaNish, kPosExpFF, kPosNaNish, kPosExpFF},
		{kPosExpFF, kPosNaNish, kPosExpFF, kPosNaNish},
		{kPosNaNish, kPosNaNish, kPosNaNish, kPosNaNish}},
	{"MiniOrdersExpFFBelowNaNPattern", false,
		{kPosNaNish, kPosExpFF, kPosNaNish, kPosExpFF},
		{kPosExpFF, kPosNaNish, kPosExpFF, kPosNaNish},
		{kPosExpFF, kPosExpFF, kPosExpFF, kPosExpFF}},

	// Both operands negative — the case the interpreter special-cases and
	// the JIT handles by flipping the low 31 bits. Getting this wrong
	// reverses the comparison for every negative pair.
	{"MaxBothNegative", true,
		{kNegOne, kNegHuge, kNegZero, kNegDenorm},
		{kNegHuge, kNegOne, kNegDenorm, kNegZero},
		{kNegOne, kNegOne, kNegZero, kNegZero}},
	{"MiniBothNegative", false,
		{kNegOne, kNegHuge, kNegZero, kNegDenorm},
		{kNegHuge, kNegOne, kNegDenorm, kNegZero},
		{kNegHuge, kNegHuge, kNegDenorm, kNegDenorm}},

	// -0.0 vs +0.0 are equal as floats and distinct here. MAX must return
	// +0.0 and MINI -0.0, bit-for-bit, whichever side they arrive on.
	{"MaxSignedZeroes", true,
		{kNegZero, kPosZero, kNegZero, kPosZero},
		{kPosZero, kNegZero, kNegZero, kPosZero},
		{kPosZero, kPosZero, kNegZero, kPosZero}},
	{"MiniSignedZeroes", false,
		{kNegZero, kPosZero, kNegZero, kPosZero},
		{kPosZero, kNegZero, kNegZero, kPosZero},
		{kNegZero, kNegZero, kNegZero, kPosZero}},

	// Denormals are not flushed by the compare — a denormal is strictly
	// between zero and the smallest normal on both sides of zero.
	{"MaxDenormalsAgainstZero", true,
		{kPosDenorm, kNegDenorm, kPosDenorm, kNegDenorm},
		{kPosZero, kPosZero, kNegZero, kNegZero},
		{kPosDenorm, kPosZero, kPosDenorm, kNegZero}},
	{"MiniDenormalsAgainstZero", false,
		{kPosDenorm, kNegDenorm, kPosDenorm, kNegDenorm},
		{kPosZero, kPosZero, kNegZero, kNegZero},
		{kPosZero, kNegDenorm, kNegZero, kNegDenorm}},

	// Mixed signs across the four lanes at once, so a lane-wise mistake
	// cannot hide behind a uniform operand vector.
	{"MaxMixedSigns", true,
		{kNegHuge, kOne, kNegZero, kPosHuge},
		{kPosDenorm, kNegOne, kPosZero, kNegExpFF},
		{kPosDenorm, kOne, kPosZero, kPosHuge}},
	{"MiniMixedSigns", false,
		{kNegHuge, kOne, kNegZero, kPosHuge},
		{kPosDenorm, kNegOne, kPosZero, kNegExpFF},
		{kNegHuge, kNegOne, kNegZero, kNegExpFF}},

	// Equal operands must come back unchanged, including for the patterns an
	// IEEE compare would call unordered.
	{"MaxEqualOperands", true,
		{kPosNaNish, kNegZero, kNegExpFF, kPosDenorm},
		{kPosNaNish, kNegZero, kNegExpFF, kPosDenorm},
		{kPosNaNish, kNegZero, kNegExpFF, kPosDenorm}},
	{"MiniEqualOperands", false,
		{kPosNaNish, kNegZero, kNegExpFF, kPosDenorm},
		{kPosNaNish, kNegZero, kNegExpFF, kPosDenorm},
		{kPosNaNish, kNegZero, kNegExpFF, kPosDenorm}},
};

void ExpectFdBits(VuTestHarness& h, const u32 expect[4])
{
	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	for (int i = 0; i < 4; i++)
	{
		EXPECT_EQ(h.GetVfBitsJit(kFd, kLane[i]), expect[i])
			<< "lane " << kLane[i] << " (jit)";
	}
}

} // namespace

class VuMinMaxOrder : public ::testing::TestWithParam<MinMaxCase>
{
};

TEST_P(VuMinMaxOrder, OrdersOperandsBySignMagnitude)
{
	const MinMaxCase& c = GetParam();

	VuTestHarness h(0);
	h.SetVfBits(kFs, c.fs[0], c.fs[1], c.fs[2], c.fs[3]);
	h.SetVfBits(kFt, c.ft[0], c.ft[1], c.ft[2], c.ft[3]);
	h.SetVfBits(kFd, 0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u);
	h.LoadProgram({
		UpperOnly(c.is_max ? VMAX_U(mask::xyzw, kFd, kFs, kFt)
		                   : VMINI_U(mask::xyzw, kFd, kFs, kFt)),
		EBitNopPair(),
	});
	h.Run();
	ExpectFdBits(h, c.expect);
}

INSTANTIATE_TEST_SUITE_P(
	Packed, VuMinMaxOrder, ::testing::ValuesIn(kCases),
	[](const ::testing::TestParamInfo<MinMaxCase>& info) { return std::string(info.param.name); });

// The same ordering has to hold when the operand arrives by broadcast rather
// than lane-wise. Ft.z is the exponent-0xFF pattern; every Fs lane is
// compared against it.
TEST(VuMinMaxOrder, MaxBroadcastZAgainstExpFF)
{
	VuTestHarness h(0);
	h.SetVfBits(kFs, kNegHuge, kOne, kPosNaNish, kNegZero);
	h.SetVfBits(kFt, kPosZero, kPosZero, kPosExpFF, kPosZero);
	h.SetVfBits(kFd, 0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u);
	h.LoadProgram({UpperOnly(VMAXz_U(mask::xyzw, kFd, kFs, kFt)), EBitNopPair()});
	h.Run();
	const u32 expect[4] = {kPosExpFF, kPosExpFF, kPosNaNish, kPosExpFF};
	ExpectFdBits(h, expect);
}

TEST(VuMinMaxOrder, MiniBroadcastWAgainstNegZero)
{
	VuTestHarness h(0);
	h.SetVfBits(kFs, kNegDenorm, kPosZero, kPosDenorm, kNegOne);
	h.SetVfBits(kFt, kPosZero, kPosZero, kPosZero, kNegZero);
	h.SetVfBits(kFd, 0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u);
	h.LoadProgram({UpperOnly(VMINIw_U(mask::xyzw, kFd, kFs, kFt)), EBitNopPair()});
	h.Run();
	const u32 expect[4] = {kNegDenorm, kNegZero, kNegZero, kNegOne};
	ExpectFdBits(h, expect);
}

// The I register is a third source shape for the same compare — a 32-bit
// immediate carried in the instruction stream rather than a VF lane. The
// immediate itself is max-finite: an exponent-0xFF immediate is a separate
// story, told by IbitImmediateIsClampedByTheCompilerNotTheInterpreter below.
TEST(VuMinMaxOrder, MaxiOrdersExpFFOperandsAboveMaxFiniteImmediate)
{
	VuTestHarness h(0);
	h.SetVfBits(kFs, kPosExpFF, kNegExpFF, kOne, kPosNaNish);
	h.SetVfBits(kFd, 0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u);
	h.LoadProgram({
		IBit(VuOp{VLitI(kPosHuge), VNOP_U()}), // I = +max finite
		VuOp{0u, VMAXi_U(mask::xyzw, kFd, kFs)},
		EBitNopPair(),
	});
	h.Run();
	const u32 expect[4] = {kPosExpFF, kPosHuge, kPosHuge, kPosNaNish};
	ExpectFdBits(h, expect);
}

TEST(VuMinMaxOrder, MiniiAgainstNegativeImmediate)
{
	VuTestHarness h(0);
	h.SetVfBits(kFs, kNegHuge, kNegDenorm, kPosZero, kNegExpFF);
	h.SetVfBits(kFd, 0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u);
	h.LoadProgram({
		IBit(VuOp{VLitI(kNegOne), VNOP_U()}), // I = -1.0
		VuOp{0u, VMINIi_U(mask::xyzw, kFd, kFs)},
		EBitNopPair(),
	});
	h.Run();
	const u32 expect[4] = {kNegHuge, kNegOne, kNegOne, kNegExpFF};
	ExpectFdBits(h, expect);
}

// =========================================================================
//  The one place the two engines legitimately disagree about MAX/MINI.
//
//  The I-bit immediate does not reach the compare the same way in both
//  engines. The interpreter copies the instruction word into VI[REG_I]
//  verbatim. microVU folds it in as a compile-time constant, and while doing
//  so clamps an exponent-0xFF immediate down to max-finite, keeping its sign
//  (microVU_Compile doIbit, gated on the VU overflow clamp mode). x86 mVU has
//  the identical clamp — it even logs "Clamping I Reg" — so this is upstream
//  behaviour we share, not an ARM64 defect.
//
//  Consequence worth knowing during a divergence hunt: for an exponent-0xFF
//  immediate, MAXi/MINIi/ADDi/MULi results differ between JIT and interpreter
//  by design, and the interpreter is not the oracle. The test therefore scores
//  the two engines separately instead of diffing them.
//
//  The sign term in the clamp is the fragile part. Dropping it would turn a
//  huge negative immediate into a huge positive one, which is why both signs
//  are pinned here.
// =========================================================================

namespace {
struct ScopedVuOverflowClamp
{
	bool prev = EmuConfig.Cpu.Recompiler.vu0Overflow;
	explicit ScopedVuOverflowClamp(bool on) { EmuConfig.Cpu.Recompiler.vu0Overflow = on; }
	~ScopedVuOverflowClamp() { EmuConfig.Cpu.Recompiler.vu0Overflow = prev; }
};
} // namespace

TEST(VuMinMaxOrder, IbitImmediateIsClampedByTheCompilerNotTheInterpreter)
{
	ScopedVuOverflowClamp clamp(true);

	VuTestHarness h(0);
	h.SetVfBits(kFs, kOne, kOne, kOne, kOne);
	h.LoadProgram({
		IBit(VuOp{VLitI(kPosExpFF), VNOP_U()}),          // I = +exponent-0xFF
		VuOp{0u, VMAXi_U(mask::xyzw, kFd, kFs)},          // vf3 = max(1.0, I)
		IBit(VuOp{VLitI(kNegExpFF), VNOP_U()}),          // I = -exponent-0xFF
		VuOp{0u, VMINIi_U(mask::xyzw, vf::vf6, kFs)},     // vf6 = min(1.0, I)
		EBitNopPair(),
	});
	h.RunNoDiff();

	// microVU clamped the immediate on the way in, so MAX/MINI never see the
	// exponent-0xFF word at all.
	EXPECT_EQ(h.GetVfBitsJit(kFd, 'x'), kPosHuge);
	EXPECT_EQ(h.GetVfBitsJit(vf::vf6, 'x'), kNegHuge) << "clamp must preserve the sign";

	// The interpreter compared against the raw immediate.
	EXPECT_EQ(h.GetVfBitsInterp(kFd, 'x'), kPosExpFF);
	EXPECT_EQ(h.GetVfBitsInterp(vf::vf6, 'x'), kNegExpFF);
}

// With the overflow clamp off, the immediate reaches the compare untouched
// and the two engines agree again — which is what identifies the clamp, and
// not something about the I-register path itself, as the cause.
TEST(VuMinMaxOrder, IbitImmediateSurvivesWhenTheOverflowClampIsOff)
{
	ScopedVuOverflowClamp clamp(false);

	VuTestHarness h(0);
	h.SetVfBits(kFs, kOne, kOne, kOne, kOne);
	h.LoadProgram({
		IBit(VuOp{VLitI(kPosExpFF), VNOP_U()}),
		VuOp{0u, VMAXi_U(mask::xyzw, kFd, kFs)},
		EBitNopPair(),
	});
	h.Run(); // agreement is now expected, so diff them
	EXPECT_EQ(h.GetVfBitsJit(kFd, 'x'), kPosExpFF);
}

// =========================================================================
//  Single-destination-lane MAX / MINI.
//
//  A one-lane mask routes through a different pair of helpers than the
//  packed form, and for y/z/w the operand is first rotated into lane 0 and
//  the result rotated back. Those two helpers had no coverage at all, and
//  they are the ones that would be quietly replaced by an IEEE FMAX/FMIN by
//  anyone "simplifying" the emitter — which is precisely what the
//  exponent-0xFF operands below would catch.
// =========================================================================

namespace {

void RunSingleLaneMinMax(bool is_max, u32 mask, char lane, u32 fs_bits, u32 ft_bits, u32 expect)
{
	VuTestHarness h(0);
	h.SetVfBits(kFs, fs_bits, fs_bits, fs_bits, fs_bits);
	h.SetVfBits(kFt, ft_bits, ft_bits, ft_bits, ft_bits);
	h.SetVfBits(kFd, 0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u);
	h.LoadProgram({
		UpperOnly(is_max ? VMAX_U(mask, kFd, kFs, kFt) : VMINI_U(mask, kFd, kFs, kFt)),
		EBitNopPair(),
	});
	h.Run();

	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	static const u32 kSentinel[4] = {0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u};
	for (int i = 0; i < 4; i++)
	{
		const u32 want = (kLane[i] == lane) ? expect : kSentinel[i];
		EXPECT_EQ(h.GetVfBitsJit(kFd, kLane[i]), want) << "lane " << kLane[i];
	}
}

} // namespace

// Lane 0 — no rotation.
TEST(VuMinMaxSingleLane, MaxIntoXTakesExpFF)
{
	RunSingleLaneMinMax(true, mask::x, 'x', kPosHuge, kPosExpFF, kPosExpFF);
}

TEST(VuMinMaxSingleLane, MiniIntoXTakesNegExpFF)
{
	RunSingleLaneMinMax(false, mask::x, 'x', kNegHuge, kNegExpFF, kNegExpFF);
}

// Lanes 1..3 — rotate into lane 0, operate, rotate back.
TEST(VuMinMaxSingleLane, MaxIntoYBothNegative)
{
	RunSingleLaneMinMax(true, mask::y, 'y', kNegHuge, kNegOne, kNegOne);
}

TEST(VuMinMaxSingleLane, MiniIntoZBothNegative)
{
	RunSingleLaneMinMax(false, mask::z, 'z', kNegDenorm, kNegHuge, kNegHuge);
}

TEST(VuMinMaxSingleLane, MaxIntoWSignedZeroes)
{
	RunSingleLaneMinMax(true, mask::w, 'w', kNegZero, kPosZero, kPosZero);
}

TEST(VuMinMaxSingleLane, MiniIntoWSignedZeroes)
{
	RunSingleLaneMinMax(false, mask::w, 'w', kPosZero, kNegZero, kNegZero);
}

// A single destination lane fed by a broadcast source crosses the rotation
// with the broadcast lane select — the two independent lane mechanisms.
TEST(VuMinMaxSingleLane, MaxBroadcastYIntoWOnly)
{
	VuTestHarness h(0);
	h.SetVfBits(kFs, kPosZero, kPosZero, kPosZero, kNegOne);
	h.SetVfBits(kFt, kNegExpFF, kPosDenorm, kPosExpFF, kNegHuge);
	h.SetVfBits(kFd, 0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u);
	h.LoadProgram({UpperOnly(VMAXy_U(mask::w, kFd, kFs, kFt)), EBitNopPair()});
	h.Run();
	// Fs.w = -1.0 against the broadcast Ft.y = smallest positive denormal.
	const u32 expect[4] = {0xDEAD0000u, 0xDEAD0001u, 0xDEAD0002u, kPosDenorm};
	ExpectFdBits(h, expect);
}

// =========================================================================
//  MAX / MINI are the two FMACs that do not touch the MAC flags. Everything
//  else in the upper pipe updates them, so an emitter that routed MAX
//  through the generic flag-updating path would be caught here rather than
//  in whatever game first depended on a stale flag.
// =========================================================================

TEST(VuMinMaxOrder, MaxLeavesMacFlagsFromThePrecedingOp)
{
	VuTestHarness h(0);
	// A SUB that produces exactly zero in every lane sets the Z bits.
	h.SetVfBits(kFs, kOne, kOne, kOne, kOne);
	h.SetVfBits(kFt, kOne, kOne, kOne, kOne);
	h.SetVfBits(vf::vf4, kNegHuge, kOne, kNegOne, kPosHuge);
	h.SetVfBits(vf::vf5, kOne, kNegHuge, kPosHuge, kNegOne);
	h.LoadProgram({
		UpperOnly(VSUB_U(mask::xyzw, kFd, kFs, kFt)), // MAC.Z ← 1111
		NopPair(),
		NopPair(),
		NopPair(),
		UpperOnly(VMAX_U(mask::xyzw, vf::vf6, vf::vf4, vf::vf5)),
		EBitNopPair(),
	});
	h.Run();
	// The Run() diff is the assertion that matters: if MAX wrote the flag
	// pipeline in one engine and not the other, MAC/STATUS would diverge.
	// Pin the result too, so the test cannot pass by MAX not executing.
	const u32 expect[4] = {kOne, kOne, kPosHuge, kPosHuge};
	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	for (int i = 0; i < 4; i++)
		EXPECT_EQ(h.GetVfBitsJit(vf::vf6, kLane[i]), expect[i]) << "lane " << kLane[i];
}

} // namespace recompiler_tests
