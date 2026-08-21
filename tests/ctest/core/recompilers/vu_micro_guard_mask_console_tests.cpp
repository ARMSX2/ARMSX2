// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU adder's guard bit, scored on microVU.
//
// autocases_vgmask.h is the console measurement -- 579 rows that pick how many
// guard bits the adder keeps below its 24-bit significand out of
// {0, 1, 2, 3, no mask}, and read one. vu0_macro_guard_mask_console_tests.cpp
// scores the COP2 macro emitter on them; this file scores the other emitter of
// the same adder on the same rows. VU0 driven through microVU rather than
// through COP2 macro is what a VU0 microprogram runs on, and it is the only
// thing VU1 has.
//
// The rows were measured on VU0. VU1 is scored against VU0's own words rather
// than against the console: nothing has measured VU1's adder, and both engines
// model one FMAC for the two.
//
// The masked bits sit below half an ULP of the sum, so every row is an
// unlike-signed ADD or a like-signed SUB -- the adds that cancel, where
// clearing the bits moves the exact sum across an ULP boundary and the chop
// reports a different word. They reach neither flag word, so the whole of what
// the mask moves is the value column.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>

#include "autocases_vgmask.h"

using namespace console_vgmask;

namespace recompiler_tests
{
using namespace vu;

namespace {

// What the recompiler's column comes to below vuClampMode 4, where no mask is
// emitted. The COP2 macro emitter is blind to exactly the same rows, and its
// own kVgMaskUnmaskedBadJit is this number.
constexpr int kUnmaskedBadJit = 455;

// The console probe's register assignment, kept so the op scored is the one
// that was measured.
constexpr u32 kFs = 1, kFt = 2, kFd = 4;

u32 Encode(const VgMaskCase& c)
{
	switch (c.op)
	{
		case VG_ADD: return VADD_U(mask::xyzw, kFd, kFs, kFt);
		case VG_SUB: return VSUB_U(mask::xyzw, kFd, kFs, kFt);
		default:     return 0;
	}
}

struct Observed
{
	u32 out[4];
	u32 mac;
	u32 stat;
};

struct Pair
{
	Observed jit, interp;
};

Pair RunCase(int vu, const VgMaskCase& c, u32 word, int clamp_mode)
{
	VuTestHarness h(vu);
	h.SetVuClampMode(clamp_mode);
	h.SetVfBits(kFs, c.fs, c.fs, c.fs, c.fs);
	h.SetVfBits(kFt, c.ft, c.ft, c.ft, c.ft);
	h.SetVfBits(kFd, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu);
	h.LoadProgram({
		VuOp{0u, word},
		VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()},
		EBitNopPair(),
	});
	// Each engine is scored against the console on its own: below clamp mode 4
	// the recompiler emits a bare four-lane add, so the JIT-vs-interp auto-diff
	// would fail on every row this file exists to record.
	h.RunNoDiff();

	Pair p{};
	for (int lane = 0; lane < 4; ++lane)
	{
		p.jit.out[lane] = h.JitSnapshot().regs.VF[kFd].UL[lane];
		p.interp.out[lane] = h.InterpSnapshot().regs.VF[kFd].UL[lane];
	}
	p.jit.mac = h.GetViJit(REG_MAC_FLAG) & 0xFFFFu;
	p.interp.mac = h.GetViInterp(REG_MAC_FLAG) & 0xFFFFu;
	p.jit.stat = h.GetViJit(REG_STATUS_FLAG) & 0xFFFFu;
	p.interp.stat = h.GetViInterp(REG_STATUS_FLAG) & 0xFFFFu;
	return p;
}

// One bit per column, set where the engine disagrees with the console. The
// probe seeded fs and ft into all four lanes and read one word back, so a lane
// that does not carry the console's word is a miss whichever lane it is.
u8 Misses(const VgMaskCase& c, const Observed& o)
{
	u8 m = 0;
	for (int lane = 0; lane < 4; ++lane)
	{
		if (o.out[lane] != c.out)
			m |= VGB_VALUE;
	}
	if (o.mac != c.mac) m |= VGB_MAC;
	if (o.stat != c.stat) m |= VGB_STAT;
	return m;
}

const char* kColName[] = {"value", "mac", "stat"};
constexpr u8 kColBit[] = {VGB_VALUE, VGB_MAC, VGB_STAT};

int PopCount(u8 v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1); }

} // namespace

TEST(VuMicroGuardMaskConsole, GuardMaskMatchesConsole)
{
	int checked = 0;
	for (int i = 0; i < kVgMaskCaseCount; ++i)
	{
		const VgMaskCase& c = kVgMaskCases[i];
		const u32 word = Encode(c);
		ASSERT_NE(word, 0u) << "case " << i << ": no encoder for op " << int(c.op);

		const Pair p = RunCase(0, c, word, 4);
		const u8 got[2] = {Misses(c, p.interp), Misses(c, p.jit)};
		for (int jit = 0; jit < 2; ++jit)
		{
			for (int col = 0; col < 3; ++col)
			{
				SCOPED_TRACE(::testing::Message()
					<< "case " << i << " [" << (jit ? "jit" : "interp") << "] "
					<< kColName[col] << ": " << c.what);
				EXPECT_FALSE(got[jit] & kColBit[col]) << "divergence from silicon";
			}
		}
		++checked;
	}
	EXPECT_EQ(checked, kVgMaskCaseCount);
}

// The other side of the gate, and where it sits. Every row here was collected
// because the mask moves it, so at each mode below 4 the recompiler has to miss
// all of them -- and miss them on the value alone, since the mask reaches
// neither flag word. Scoring 1 to 3 rather than only 3 is what says the gate is
// on the topmost mode rather than somewhere below it.
TEST(VuMicroGuardMaskConsole, UnmaskedRecompilerMissesTheseRows)
{
	for (int mode = 1; mode <= 3; ++mode)
	{
		SCOPED_TRACE(::testing::Message() << "vuClampMode " << mode);
		int bad = 0, value = 0, rows_hit = 0;
		for (int i = 0; i < kVgMaskCaseCount; ++i)
		{
			const VgMaskCase& c = kVgMaskCases[i];
			const u8 m = Misses(c, RunCase(0, c, Encode(c), mode).jit);
			bad += PopCount(m);
			value += (m & VGB_VALUE) != 0;
			rows_hit += m != 0;
		}
		std::printf("vuClampMode %d: %d column misses over %d rows (%d value, %d rows)\n",
			mode, bad, kVgMaskCaseCount, value, rows_hit);
		EXPECT_EQ(bad, kUnmaskedBadJit);
		EXPECT_EQ(value, bad);
	}
}

// One FMAC for the two VUs. The gate reads CHECK_VU_EXACT(mVU.index), so a VU1
// program at mode 4 gets the mask and a VU0 one at mode 1 does not -- this
// asserts the two microprograms land on the same word at each mode, which is
// what says the index is being read rather than a VU0 constant.
TEST(VuMicroGuardMaskConsole, BothVusEmitTheSameAdder)
{
	for (int mode : {1, 4})
	{
		for (int i = 0; i < kVgMaskCaseCount; ++i)
		{
			const VgMaskCase& c = kVgMaskCases[i];
			const u32 word = Encode(c);
			const Pair vu0 = RunCase(0, c, word, mode);
			const Pair vu1 = RunCase(1, c, word, mode);
			SCOPED_TRACE(::testing::Message()
				<< "vuClampMode " << mode << " case " << i << ": " << c.what);
			for (int lane = 0; lane < 4; ++lane)
				EXPECT_EQ(vu1.jit.out[lane], vu0.jit.out[lane]) << "lane " << lane;
			EXPECT_EQ(vu1.jit.mac, vu0.jit.mac);
		}
	}
}

// Regenerates the counts above.
TEST(VuMicroGuardMaskConsole, DISABLED_DumpConsoleComparison)
{
	for (int mode = 1; mode <= 4; ++mode)
	{
		int bi = 0, bj = 0, vj = 0, mj = 0, sj = 0, rows_j = 0, vu1_differs = 0;
		for (int i = 0; i < kVgMaskCaseCount; ++i)
		{
			const VgMaskCase& c = kVgMaskCases[i];
			const u32 word = Encode(c);
			const Pair p = RunCase(0, c, word, mode);
			const u8 mi = Misses(c, p.interp), m = Misses(c, p.jit);
			bi += PopCount(mi);
			bj += PopCount(m);
			vj += (m & VGB_VALUE) != 0;
			mj += (m & VGB_MAC) != 0;
			sj += (m & VGB_STAT) != 0;
			rows_j += m != 0;

			const Pair p1 = RunCase(1, c, word, mode);
			for (int lane = 0; lane < 4; ++lane)
				vu1_differs += p1.jit.out[lane] != p.jit.out[lane];
		}
		std::printf("mode %d: interp %d columns, jit %d columns over %d rows "
					"(value %d, mac %d, stat %d; %d rows), VU1 lane diffs %d\n",
			mode, bi, bj, kVgMaskCaseCount, vj, mj, sj, rows_j, vu1_differs);
	}
}

} // namespace recompiler_tests
