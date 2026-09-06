// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Block-start VF preloads folded into Ldp.
//
// mvuPreloadRegisters queues the VF registers the next few instructions
// read and emits the queue at the end of the walk. Two entries one register
// apart sit 16 bytes apart in VURegs::VF, so they load as one Ldp — and
// Ldp's first destination takes the LOWER address, which means a descending
// queue has to hand the two slots to the instruction the other way round.
// Get that backwards and each register of the pair holds the other's value:
// arithmetic that is entirely plausible, on operands that are not the ones
// the program named.
//
// Every op here is a SUB. Under ADD a swapped pair computes the same sum,
// so a suite built on it passes with the swap deleted — which is what the
// first cut of this file did. Subtraction turns the swap into a sign, and
// the seeding gives each lane a different multiple of the difference so a
// rotated lane cannot land on the value it displaced either. Seeds are small
// integers and every expectation is a difference of two, so binary32 is
// exact and the compares are equality.
//
// The fold count comes from the emitter itself. Without it a test passes
// whether or not the fold fired, and the digest in mvu_abi_digest_tests only
// says the shape CHANGED, not that it changed the intended way.
//
// VU0 carries these tests; mvuPreloadRegisters is not per-VU, and the
// register file it reads is at the same offset in both.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <vector>

// PCSX2_RECOMPILER_TESTS hook, global scope — see microVU_Compile-arm64.inl.
extern u32 g_mvuPreloadPairCount;

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }

// VF[n] = (n, 2n, 4n, 8n). A per-lane weight is what keeps the lanes of a
// difference distinct: with VF[n] = (4n, 4n+1, 4n+2, 4n+3) the +i cancels and
// every lane of VF[a]-VF[b] carries the same number.
void SeedRegisterFile(VuTestHarness& h)
{
	for (u32 n = 1; n <= 16; n++)
	{
		const float v = static_cast<float>(n);
		h.SetVf(n, v, 2.0f * v, 4.0f * v, 8.0f * v);
	}
}

struct Diff
{
	u32 dest;
	u32 a;
	u32 b;
};

// Runs the program and checks each destination against VF[a] - VF[b], read
// out of the JIT's own snapshot. Returns how many pairs the emitter folded.
u32 RunAndCheck(const std::vector<VuOp>& body, const std::vector<Diff>& diffs)
{
	VuTestHarness h(0);
	SeedRegisterFile(h);

	std::vector<VuOp> prog = body;
	prog.push_back(EBitNopPair());

	h.LoadProgram(prog);
	g_mvuPreloadPairCount = 0;
	h.Run(); // diffs JIT against interp across the whole architectural surface
	const u32 folds = g_mvuPreloadPairCount;

	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	static const float kWeight[4] = {1.0f, 2.0f, 4.0f, 8.0f};
	for (const Diff& d : diffs)
	{
		for (int i = 0; i < 4; i++)
		{
			const float want = kWeight[i] * (static_cast<float>(d.a) - static_cast<float>(d.b));
			EXPECT_FLOAT_EQ(h.GetVfJit(d.dest, kLane[i]), want)
				<< "vf" << d.dest << " lane " << kLane[i]
				<< " = vf" << d.a << " - vf" << d.b;
		}
	}
	return folds;
}

} // namespace

// Fs is the lower register of each pair, so the queue ascends and each pair's
// lower-numbered register takes Ldp's first destination unswapped.
TEST(VuPreloadPair, AscendingQueueLoadsEachRegisterIntoItsOwnSlot)
{
	const u32 folds = RunAndCheck(
		{
			UpperOnly(VSUB_U(mask::xyzw, vf::vf20, vf::vf1, vf::vf2)),
			UpperOnly(VSUB_U(mask::xyzw, vf::vf21, vf::vf3, vf::vf4)),
			UpperOnly(VSUB_U(mask::xyzw, vf::vf22, vf::vf5, vf::vf6)),
		},
		{{vf::vf20, 1, 2}, {vf::vf21, 3, 4}, {vf::vf22, 5, 6}});
	EXPECT_EQ(folds, 3u) << "three adjacent pairs were queued and none folded";
}

// The same six registers with the operands the other way round, which is the
// order the queue takes them in: Fs first. Ldp still addresses the lower slot
// first, so this is the branch that hands it the two destinations swapped.
TEST(VuPreloadPair, DescendingQueueLoadsEachRegisterIntoItsOwnSlot)
{
	const u32 folds = RunAndCheck(
		{
			UpperOnly(VSUB_U(mask::xyzw, vf::vf20, vf::vf2, vf::vf1)),
			UpperOnly(VSUB_U(mask::xyzw, vf::vf21, vf::vf4, vf::vf3)),
			UpperOnly(VSUB_U(mask::xyzw, vf::vf22, vf::vf6, vf::vf5)),
		},
		{{vf::vf20, 2, 1}, {vf::vf21, 4, 3}, {vf::vf22, 6, 5}});
	EXPECT_EQ(folds, 3u) << "three adjacent pairs were queued and none folded";
}

// Registers three apart throughout, so no two queue entries are adjacent and
// the emitter has nothing to fold. The same arithmetic has to come out, which
// is what says the two tests above pin the fold and not the seeding.
TEST(VuPreloadPair, NonAdjacentQueueLoadsSeparatelyAndAgrees)
{
	const u32 folds = RunAndCheck(
		{
			UpperOnly(VSUB_U(mask::xyzw, vf::vf20, vf::vf1, vf::vf4)),
			UpperOnly(VSUB_U(mask::xyzw, vf::vf21, vf::vf7, vf::vf10)),
			UpperOnly(VSUB_U(mask::xyzw, vf::vf22, vf::vf13, vf::vf16)),
		},
		{{vf::vf20, 1, 4}, {vf::vf21, 7, 10}, {vf::vf22, 13, 16}});
	EXPECT_EQ(folds, 0u) << "no queued pair is adjacent, so nothing may fold";
}

} // namespace recompiler_tests
