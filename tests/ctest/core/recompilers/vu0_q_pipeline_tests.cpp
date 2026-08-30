// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU0 Q-pipeline DiffJitVsInterp suite. Covers VDIV / VSQRT /
// VRSQRT (the three Q-producers) plus VWAITQ at varying cycle distances,
// plus the broadcast-from-Q upper ops VADDq / VMULq / VMSUBq. End-state
// is captured at E-bit, so the JIT's mVUendProgram() spill of pending_q
// → q is what we diff against the interpreter's per-instruction `VU.q`
// updates. STATUS-flag bits 0x10 (invalid op) and 0x20 (div-by-zero) are
// exercised — both the architectural REG_STATUS_FLAG and the saturated q
// payload the VU emits on /0 with sign.
//
// FDIV flag → STATUS transfer (micro mode): the I (invalid, 0x10) and D
// (divide-by-zero, 0x20) bits are produced by VDIV/VSQRT/VRSQRT into
// mVU.divFlag, then folded into the architectural STATUS flag by mVUdivSet()
// on the instruction 7 cycles downstream (the FDIV flag latency). For a long
// time arm64 micro-mode dropped this entirely — doUpperOp() never called
// mVUdivSet(), so the bits reached STATUS only in COP2 macro mode. That
// surfaced as Soul Calibur 3 character-model jitter and was misfiled as a
// "short standalone program" / test-side limitation (the older tests below
// opt REG_STATUS_FLAG out of the diff and assert via the interp side). The
// fix wires mVUdivSet() into doUpperOp(), matching x86; the
// *InJitStatus regression tests assert the bit on the JIT side directly and
// are RED without it. A genuinely-too-short program (FDIV that E-bits within
// the 7-cycle latency) still can't observe the flag — that's shared with x86,
// not an arm64 bug — so pad past the latency when asserting the JIT side.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }
inline VuOp Pair(u32 lower, u32 upper) { return VuOp{lower, upper}; }

// VWAITQ + upper-NOP — the canonical "drain Q-pipe" pair.
inline VuOp WaitQPair() { return VuOp{VWAITQ_L(), VNOP_U()}; }

// The console's saturated quotient, which every engine here returns. A
// quotient reached by dividing rather than by taking the zero-divisor branch
// still saturates a binade lower; VuDivUnitConsole scores that in full.
constexpr u32 kQPlusMax  = 0x7FFFFFFFu;
constexpr u32 kQMinusMax = 0xFFFFFFFFu;

} // namespace

// -------- VDIV — basic positive --------

TEST(Vu0Qpipe, VdivBasicPositiveProducesQuotient)
{
	VuTestHarness h(0);
	h.SetVf(1, 6.0f, 99.0f, 99.0f, 99.0f);  // fs.x = numerator
	h.SetVf(2, 99.0f, 99.0f, 2.0f, 99.0f);  // ft.z = denominator
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, /*fsf=*/0, vf::vf2, /*ftf=*/2)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	const u32 q_jit = h.GetViJit(REG_Q);
	const u32 q_int = h.GetViInterp(REG_Q);
	EXPECT_EQ(q_jit, q_int);
	EXPECT_FLOAT_EQ(std::bit_cast<float>(q_jit), 3.0f);
}

TEST(Vu0Qpipe, VdivNegativeOverPositiveSignsResult)
{
	VuTestHarness h(0);
	h.SetVf(1, -10.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 4.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), -2.5f);
}

// -------- VDIV — div-by-zero status flags + magic Q values --------

TEST(Vu0Qpipe, VdivByZeroSetsBit20AndMaxFloatQ)
{
	// Per _vuDIV: ft==0 && fs!=0 → statusflag |= 0x20, q saturates with the
	// xor of the two signs.
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG); // see file header — divFlag→STATUS lost on short programs
	h.SetVf(1, 1.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 0.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_EQ(h.GetViJit(REG_Q), kQPlusMax);
	EXPECT_EQ(h.GetViInterp(REG_Q), kQPlusMax);
	// Status bit 0x20 = D-flag (div-by-zero). Asserted via interp (the JIT
	// loses the divFlag on short programs — see file header).
	EXPECT_NE(h.GetViInterp(REG_STATUS_FLAG) & 0x20, 0u) << "div-by-zero D bit missing in interp status";
}

TEST(Vu0Qpipe, VdivByZeroNegativeNumeratorSignsMagicQ)
{
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(1, -1.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 0.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_EQ(h.GetViJit(REG_Q), kQMinusMax);
	EXPECT_EQ(h.GetViInterp(REG_Q), kQMinusMax);
}

TEST(Vu0Qpipe, VdivZeroOverZeroSetsBit10InvalidOp)
{
	// Per _vuDIV: ft==0 && fs==0 → statusflag |= 0x10 (invalid-op I-flag),
	// q saturates positive since the signs match (both +0).
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(1, 0.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 0.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_EQ(h.GetViJit(REG_Q), kQPlusMax);
	EXPECT_EQ(h.GetViInterp(REG_Q), kQPlusMax);
	EXPECT_NE(h.GetViInterp(REG_STATUS_FLAG) & 0x10, 0u);
}

// -------- FDIV flags reach the JIT STATUS flag (micro-mode regression) --------
//
// These assert the I/D bits on the JIT side directly (the older tests above
// route through interp because the JIT used to drop them). They pad past the
// 7-cycle FDIV flag latency so mVUdivSet() — now called from doUpperOp() — has
// a downstream instruction to fold mVU.divFlag into STATUS. RED before the
// doUpperOp()→mVUdivSet() fix; the Soul Calibur 3 VU0-micro jitter regression.

// Eight NOP pairs (inlined per call site) span the 7-cycle FDIV flag latency.

TEST(Vu0Qpipe, VdivByZeroSetsDBitInJitStatus)
{
	// 1.0 / 0.0 → D (divide-by-zero, 0x20) must reach the JIT STATUS flag.
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG); // sticky/Z/S corners differ on tiny progs; assert the D bit directly
	h.SetVf(1, 1.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 0.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		NopPair(), NopPair(), NopPair(), NopPair(),
		NopPair(), NopPair(), NopPair(), NopPair(),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_NE(h.GetViJit(REG_STATUS_FLAG) & 0x20u, 0u)
		<< "JIT dropped the FDIV divide-by-zero (D) flag — mVUdivSet missing from doUpperOp?";
	EXPECT_NE(h.GetViInterp(REG_STATUS_FLAG) & 0x20u, 0u); // oracle
}

TEST(Vu0Qpipe, VdivZeroOverZeroSetsIBitInJitStatus)
{
	// 0.0 / 0.0 → I (invalid-op, 0x10) must reach the JIT STATUS flag.
	// This is the exact shape behind the Soul Calibur 3 VU0-micro jitter.
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(1, 0.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 0.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		NopPair(), NopPair(), NopPair(), NopPair(),
		NopPair(), NopPair(), NopPair(), NopPair(),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_NE(h.GetViJit(REG_STATUS_FLAG) & 0x10u, 0u)
		<< "JIT dropped the FDIV invalid-op (I) flag (0/0)";
	EXPECT_NE(h.GetViInterp(REG_STATUS_FLAG) & 0x10u, 0u);
}

TEST(Vu0Qpipe, VsqrtNegativeSetsIBitInJitStatus)
{
	// sqrt(-25) → I (invalid-op, 0x10) must reach the JIT STATUS flag.
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(1, 0, 0, -25.0f, 0);
	h.LoadProgram({
		LowerOnly(VSQRT_L(vf::vf1, 2)),
		NopPair(), NopPair(), NopPair(), NopPair(),
		NopPair(), NopPair(), NopPair(), NopPair(),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_NE(h.GetViJit(REG_STATUS_FLAG) & 0x10u, 0u)
		<< "JIT dropped the VSQRT invalid-op (I) flag (negative operand)";
	EXPECT_NE(h.GetViInterp(REG_STATUS_FLAG) & 0x10u, 0u);
}

TEST(Vu0Qpipe, VdivByZeroSetsDBitInJitStatusOnVu1)
{
	// Same transfer on VU1's micro path.
	VuTestHarness h(1);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(1, 1.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 0.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		NopPair(), NopPair(), NopPair(), NopPair(),
		NopPair(), NopPair(), NopPair(), NopPair(),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_NE(h.GetViJit(REG_STATUS_FLAG) & 0x20u, 0u)
		<< "VU1 JIT dropped the FDIV divide-by-zero (D) flag";
}

// -------- VSQRT --------

TEST(Vu0Qpipe, VsqrtPositive)
{
	VuTestHarness h(0);
	h.SetVf(1, 0, 16.0f, 0, 0);
	h.LoadProgram({
		LowerOnly(VSQRT_L(vf::vf1, /*ftf=*/1)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 4.0f);
}

TEST(Vu0Qpipe, VsqrtNegativeSetsInvalidAndUsesAbs)
{
	// Per _vuSQRT: ft<0 → statusflag |= 0x10 (I); q = sqrt(|ft|).
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(1, 0, 0, -25.0f, 0);
	h.LoadProgram({
		LowerOnly(VSQRT_L(vf::vf1, 2)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 5.0f);
	EXPECT_NE(h.GetViInterp(REG_STATUS_FLAG) & 0x10, 0u);
}

// -------- VRSQRT --------

TEST(Vu0Qpipe, VrsqrtBasic)
{
	// q = fs / sqrt(|ft|). 8 / sqrt(4) = 4.
	VuTestHarness h(0);
	h.SetVf(1, 8.0f, 0, 0, 0);
	h.SetVf(2, 0, 4.0f, 0, 0);
	h.LoadProgram({
		LowerOnly(VRSQRT_L(vf::vf1, 0, vf::vf2, 1)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 4.0f);
}

TEST(Vu0Qpipe, VrsqrtNegativeFtSetsInvalidAndUsesAbs)
{
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(1, 8.0f, 0, 0, 0);
	h.SetVf(2, 0, -4.0f, 0, 0);
	h.LoadProgram({
		LowerOnly(VRSQRT_L(vf::vf1, 0, vf::vf2, 1)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 4.0f);
	EXPECT_NE(h.GetViInterp(REG_STATUS_FLAG) & 0x10, 0u);
}

TEST(Vu0Qpipe, VrsqrtByZeroNonZeroNumeratorMagicQ)
{
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(1, 1.0f, 0, 0, 0);
	h.SetVf(2, 0, 0.0f, 0, 0);
	h.LoadProgram({
		LowerOnly(VRSQRT_L(vf::vf1, 0, vf::vf2, 1)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_EQ(h.GetViJit(REG_Q), kQPlusMax);
	EXPECT_EQ(h.GetViInterp(REG_Q), kQPlusMax);
}

TEST(Vu0Qpipe, VrsqrtByZeroZeroNumeratorSaturatesAndRaisesInvalidOnly)
{
	// 0/0 takes the same saturated quotient every other zero divisor takes, and
	// raises I without D -- the dividend decides the division's cause and the
	// two are exclusive, as they are for VDIV. The interpreter used to return a
	// signed zero here and raise both, which put it at odds with mVU_RSQRT;
	// autocases_vurs.h holds the console rows that settled it.
	VuTestHarness h(0);
	h.SetVf(1, 0.0f, 0, 0, 0);
	h.SetVf(2, 0, 0.0f, 0, 0);
	h.LoadProgram({
		LowerOnly(VRSQRT_L(vf::vf1, 0, vf::vf2, 1)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_EQ(h.GetViInterp(REG_Q), kQPlusMax);
	EXPECT_EQ(h.GetViJit(REG_Q), kQPlusMax);
	EXPECT_EQ(h.GetViInterp(REG_STATUS_FLAG) & 0x30, 0x10u);
}

// -------- VDIV → VWAITQ → VADDq broadcast (Q observable after wait) --------

TEST(Vu0Qpipe, AddqPicksUpVdivResultAfterWaitq)
{
	// 12.0 / 4.0 = 3.0 → ADDq broadcasts 3.0 → fd.{xyzw} = fs.{xyzw} + 3.
	VuTestHarness h(0);
	h.SetVf(1, 12.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 4.0f);
	h.SetVf(3, 1.0f, 2.0f, 3.0f, 4.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		WaitQPair(),
		UpperOnly(VADDq_U(mask::xyzw, vf::vf4, vf::vf3)),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(h.GetVfJit(4, 'x'), 4.0f);
	EXPECT_FLOAT_EQ(h.GetVfJit(4, 'y'), 5.0f);
	EXPECT_FLOAT_EQ(h.GetVfJit(4, 'z'), 6.0f);
	EXPECT_FLOAT_EQ(h.GetVfJit(4, 'w'), 7.0f);
}

TEST(Vu0Qpipe, MulqBroadcastsQ)
{
	// 1.0 / 0.5 = 2.0; broadcast multiply.
	VuTestHarness h(0);
	h.SetVf(1, 1.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 0.5f);
	h.SetVf(3, 1.0f, 2.0f, 3.0f, 4.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		WaitQPair(),
		UpperOnly(VMULq_U(mask::xyzw, vf::vf4, vf::vf3)),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(h.GetVfJit(4, 'x'), 2.0f);
	EXPECT_FLOAT_EQ(h.GetVfJit(4, 'w'), 8.0f);
}

// -------- VWAITQ at varying cycle distances --------
//
// The Q-pipe latency model is fixed in microVU (7 cycles for VDIV, 7 for
// VRSQRT, 4 for VSQRT — see the per-op schedules). The interpreter doesn't
// model latency, so the *value* visible at E-bit is identical regardless
// of how many filler instructions sit between VDIV and VWAITQ. The diff
// catches any off-by-one in the JIT's drain bookkeeping.

TEST(Vu0Qpipe, VdivThenLongPipelineThenWaitq)
{
	VuTestHarness h(0);
	h.SetVf(1, 21.0f, 0, 0, 0);
	h.SetVf(2, 0, 0, 0, 7.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 3)),
		// Fill 8 cycles of NOPs to safely span Q-pipe latency.
		NopPair(), NopPair(), NopPair(), NopPair(),
		NopPair(), NopPair(), NopPair(), NopPair(),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 3.0f);
}

TEST(Vu0Qpipe, BackToBackVdivOverwritesPendingQ)
{
	// Two VDIVs without intervening VWAITQ: the second's pending_q overwrites
	// the first before either lands in Q. Final Q at E-bit must be the second
	// quotient — both JIT and interp must agree.
	VuTestHarness h(0);
	h.SetVf(1, 10.0f, 0, 0, 0);
	h.SetVf(2, 0, 2.0f, 0, 0);     // first = 5
	h.SetVf(3, 100.0f, 0, 0, 0);
	h.SetVf(4, 0, 0, 0, 4.0f);     // second = 25
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 1)),
		LowerOnly(VDIV_L(vf::vf3, 0, vf::vf4, 3)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 25.0f);
}

// -------- VU1 spot check --------

TEST(Vu0Qpipe, VdivWorksOnVu1)
{
	VuTestHarness h(1);
	h.SetVf(1, 9.0f, 0, 0, 0);
	h.SetVf(2, 0, 3.0f, 0, 0);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 1)),
		WaitQPair(),
		EBitNopPair(),
	});
	h.Run();
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 3.0f);
}

} // namespace recompiler_tests
