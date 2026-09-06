// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The direction a partial VF write is merged in.
//
// An op with a masked destination computes into a slot of its own and leaves
// the components outside the mask holding whatever the full-width NEON op put
// there. clearNeeded reconciles that with the other cached copy of the same
// VF. Handing the other copy the components we wrote costs two element
// inserts for a three-component mask; taking the one component we did not
// write costs one, and our slot becomes the whole register.
//
// What can go wrong is which component ends up where. The lane the reversal
// reads is the complement of the write mask, so a table entry that is off by
// one leaves the destination holding its own stale component in a lane the op
// was supposed to write, and the op's result in the lane it was supposed to
// leave alone — two plausible floats in the wrong two places.
//
// Seeds are VF[n] = (n, 2n, 4n, 8n) and every expectation is a difference of
// two of them, so each lane of a result carries a different multiple and a
// displaced lane cannot land on the value it displaced. Every op is a SUB:
// under ADD the two directions agree whenever the operands are symmetric.
//
// The fold count comes from the emitter. Without it these all pass on a build
// where the reversal never fires, which is exactly what the two-and-fewer
// masks are supposed to do — so the mask control and the fold it is a control
// for share one program, and the control's liveness is that program's count.
//
// VU0 carries these tests; clearNeeded is not per-VU.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <vector>

// PCSX2_RECOMPILER_TESTS hook, global scope — see microVU_IR-arm64.h.
extern u32 g_mvuMergeFoldCount;

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }

constexpr float kWeight[4] = {1.0f, 2.0f, 4.0f, 8.0f};
constexpr char kLane[4] = {'x', 'y', 'z', 'w'};

void SeedRegisterFile(VuTestHarness& h)
{
	for (u32 n = 1; n <= 24; n++)
	{
		const float v = static_cast<float>(n);
		h.SetVf(n, v, 2.0f * v, 4.0f * v, 8.0f * v);
	}
}

// One masked difference: dest's masked lanes hold VF[a] - VF[b], the rest
// still hold dest's seed.
struct Masked
{
	u32 dest;
	u32 a;
	u32 b;
	u32 lanes; // bit i = lane i (x is bit 0), matching the check below
};

u32 RunAndCheck(const std::vector<VuOp>& body, const std::vector<Masked>& want)
{
	VuTestHarness h(0);
	SeedRegisterFile(h);

	std::vector<VuOp> prog = body;
	prog.push_back(EBitNopPair());

	h.LoadProgram(prog);
	g_mvuMergeFoldCount = 0;
	h.Run(); // diffs JIT against interp across the whole architectural surface
	const u32 folds = g_mvuMergeFoldCount;

	for (const Masked& m : want)
	{
		for (int i = 0; i < 4; i++)
		{
			const float expected = (m.lanes & (1u << i))
				? kWeight[i] * (static_cast<float>(m.a) - static_cast<float>(m.b))
				: kWeight[i] * static_cast<float>(m.dest);
			EXPECT_FLOAT_EQ(h.GetVfJit(m.dest, kLane[i]), expected)
				<< "vf" << m.dest << " lane " << kLane[i]
				<< (m.lanes & (1u << i) ? " should be the difference" : " should be untouched");
		}
	}
	return folds;
}

} // namespace

// All four three-component masks, so each entry of the complement-lane table
// is read once and the component it names is the one the op left alone.
TEST(VuMergeFold, ThreeComponentWriteKeepsTheComponentItDidNotWrite)
{
	const u32 folds = RunAndCheck(
		{
			UpperOnly(VSUB_U(mask::x | mask::y | mask::z, vf::vf20, vf::vf1, vf::vf2)),
			UpperOnly(VSUB_U(mask::x | mask::y | mask::w, vf::vf21, vf::vf3, vf::vf4)),
			UpperOnly(VSUB_U(mask::x | mask::z | mask::w, vf::vf22, vf::vf5, vf::vf6)),
			UpperOnly(VSUB_U(mask::y | mask::z | mask::w, vf::vf23, vf::vf7, vf::vf8)),
		},
		{
			{vf::vf20, 1, 2, 0b0111},
			{vf::vf21, 3, 4, 0b1011},
			{vf::vf22, 5, 6, 0b1101},
			{vf::vf23, 7, 8, 0b1110},
		});
	EXPECT_EQ(folds, 4u) << "four three-component writes, and the merge folded " << folds;
}

// The reversal leaves the register in the slot the op wrote, and drops the
// copy it took the fourth component from. A read of the same VF afterwards
// has to reach the written slot: the dropped copy still holds the value from
// before the write, in lanes that are all plausible.
TEST(VuMergeFold, TheNextReadSeesTheWrittenSlotAndNotTheDroppedCopy)
{
	VuTestHarness h(0);
	SeedRegisterFile(h);

	h.LoadProgram({
		UpperOnly(VSUB_U(mask::x | mask::y | mask::z, vf::vf20, vf::vf1, vf::vf2)),
		UpperOnly(VSUB_U(mask::xyzw, vf::vf21, vf::vf20, vf::vf9)),
		EBitNopPair(),
	});
	g_mvuMergeFoldCount = 0;
	h.Run();
	EXPECT_EQ(g_mvuMergeFoldCount, 1u) << "the three-component write did not fold";

	// vf20 after the write, read back the way the second op reads it.
	const float after[4] = {
		kWeight[0] * (1.0f - 2.0f),
		kWeight[1] * (1.0f - 2.0f),
		kWeight[2] * (1.0f - 2.0f),
		kWeight[3] * 20.0f,
	};
	for (int i = 0; i < 4; i++)
	{
		EXPECT_FLOAT_EQ(h.GetVfJit(vf::vf21, kLane[i]), after[i] - kWeight[i] * 9.0f)
			<< "vf21 lane " << kLane[i] << " read vf20 from the dropped copy";
	}
}

// Two components and one leave more than one component behind, so the
// complement is not a single insert and the merge stays as it was. The
// three-component write in the same program is what says the count is live.
TEST(VuMergeFold, FewerThanThreeComponentsAreLeftToTheOtherDirection)
{
	const u32 folds = RunAndCheck(
		{
			UpperOnly(VSUB_U(mask::x, vf::vf20, vf::vf1, vf::vf2)),
			UpperOnly(VSUB_U(mask::x | mask::y, vf::vf21, vf::vf3, vf::vf4)),
			UpperOnly(VSUB_U(mask::y | mask::w, vf::vf22, vf::vf5, vf::vf6)),
			UpperOnly(VSUB_U(mask::x | mask::y | mask::z, vf::vf23, vf::vf7, vf::vf8)),
		},
		{
			{vf::vf20, 1, 2, 0b0001},
			{vf::vf21, 3, 4, 0b0011},
			{vf::vf22, 5, 6, 0b1010},
			{vf::vf23, 7, 8, 0b0111},
		});
	EXPECT_EQ(folds, 1u) << "only the three-component write should fold, and " << folds << " did";
}

} // namespace recompiler_tests
