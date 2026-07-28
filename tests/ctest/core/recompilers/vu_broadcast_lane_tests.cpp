// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Broadcast-lane sweep for the upper-pipe FMACs — every op, every lane.
//
// Two thirds of the PS2 VU's upper pipe is broadcast forms: ADDx/y/z/w,
// SUBbc, MULbc, MADDbc, MSUBbc, MAXbc, MINIbc and their accumulator-target
// twins. Each takes ONE lane of Ft and applies it to all four lanes of Fs.
// The lane is encoded in the opcode itself, not in an operand field, so the
// only thing separating VMULy from VMULz in the emitter is a table index —
// and a transposed index produces a numerically plausible result that no
// crash and no assert will ever catch. It shows up as geometry that is
// subtly wrong, in one game, on one model.
//
// This suite makes a wrong lane impossible to miss. Ft holds four pairwise
// distinct values, so each broadcast lane yields a distinct result vector,
// and every case carries a hand-computed expectation — the test knows the
// right answer independently of both engines, and `Run()` additionally
// diffs the JIT against the interpreter.
//
// The hand-computed expectation is not decoration. A diff-only test passes
// vacuously if a mis-encoded instruction decodes to something inert in BOTH
// engines; pinning the absolute result is what rules that out.
//
// Coverage note: before this suite, MAXx/y/z/w, MINIx/y/z/w, MADDx/y,
// MSUBy/z/w, MULw, SUBy/z, MAXi and MINIi had never been emitted by any
// test. MULbc in particular routes through the AX-14 by-element FMUL fold
// (setupFtReg's canLaneFold path), where the lane index becomes a vixl
// by-element operand — a place this codebase has a documented history of
// getting silently wrong (a V4S vm selects the fp16 opcode once Devel
// strips the assert).

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"

#include <gtest/gtest.h>

#include <string>

namespace recompiler_tests {

using namespace vu;

namespace {

// I-bit set so the zero lower word is suppressed (becomes the VI[REG_I]
// immediate) rather than decoding as LQ vf0 — the canonical suppression idiom.
inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }

constexpr u32 kFs = vf::vf1;    // (1.0, 2.5, 4.0, 6.0)
constexpr u32 kFt = vf::vf2;    // (2.0, 3.0, 5.0, 7.0) — four distinct lanes
constexpr u32 kFd = vf::vf3;    // pre-seeded with a sentinel
constexpr u32 kAccSrc = vf::vf4;  // (10, 20, 30, 40)
constexpr u32 kOnes = vf::vf5;    // (1, 1, 1, 1)

// Where the op deposits its result. The two are separate emitter paths:
// FMACa/FMACc/FMACd write a VF, FMACa(isACC)/FMACb write the accumulator.
enum class Dest
{
	Vf,
	Acc,
};

struct BcCase
{
	const char* name;
	u32 upper;
	Dest dest;
	float expect[4];
};

// Fs = (1.0, 2.5, 4.0, 6.0), Ft = (2.0, 3.0, 5.0, 7.0), ACC = (10, 20, 30, 40).
// Every expectation below is a multiple of 0.5, so binary32 holds it exactly
// and the float compares are equality, not tolerance.
const BcCase kCases[] = {
	// ---- Fd ← Fs + Ft.bc ----
	{"AddX",  VADDx_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {3.0f, 4.5f, 6.0f, 8.0f}},
	{"AddY",  VADDy_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {4.0f, 5.5f, 7.0f, 9.0f}},
	{"AddZ",  VADDz_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {6.0f, 7.5f, 9.0f, 11.0f}},
	{"AddW",  VADDw_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {8.0f, 9.5f, 11.0f, 13.0f}},

	// ---- Fd ← Fs - Ft.bc ----
	{"SubX",  VSUBx_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {-1.0f, 0.5f, 2.0f, 4.0f}},
	{"SubY",  VSUBy_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {-2.0f, -0.5f, 1.0f, 3.0f}},
	{"SubZ",  VSUBz_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {-4.0f, -2.5f, -1.0f, 1.0f}},
	{"SubW",  VSUBw_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {-6.0f, -4.5f, -3.0f, -1.0f}},

	// ---- Fd ← Fs * Ft.bc — the AX-14 by-element FMUL fold ----
	{"MulX",  VMULx_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {2.0f, 5.0f, 8.0f, 12.0f}},
	{"MulY",  VMULy_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {3.0f, 7.5f, 12.0f, 18.0f}},
	{"MulZ",  VMULz_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {5.0f, 12.5f, 20.0f, 30.0f}},
	{"MulW",  VMULw_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {7.0f, 17.5f, 28.0f, 42.0f}},

	// ---- Fd ← ACC + Fs * Ft.bc ----
	{"MaddX", VMADDx_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {12.0f, 25.0f, 38.0f, 52.0f}},
	{"MaddY", VMADDy_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {13.0f, 27.5f, 42.0f, 58.0f}},
	{"MaddZ", VMADDz_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {15.0f, 32.5f, 50.0f, 70.0f}},
	{"MaddW", VMADDw_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {17.0f, 37.5f, 58.0f, 82.0f}},

	// ---- Fd ← ACC - Fs * Ft.bc ----
	{"MsubX", VMSUBx_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {8.0f, 15.0f, 22.0f, 28.0f}},
	{"MsubY", VMSUBy_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {7.0f, 12.5f, 18.0f, 22.0f}},
	{"MsubZ", VMSUBz_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {5.0f, 7.5f, 10.0f, 10.0f}},
	{"MsubW", VMSUBw_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {3.0f, 2.5f, 2.0f, -2.0f}},

	// ---- Fd ← max(Fs, Ft.bc) — all operands positive here, so the
	//      sign-magnitude ordering degenerates to a plain max. The
	//      negative/exp-FF corners live in vu_minmax_order_tests.cpp.
	{"MaxX",  VMAXx_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {2.0f, 2.5f, 4.0f, 6.0f}},
	{"MaxY",  VMAXy_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {3.0f, 3.0f, 4.0f, 6.0f}},
	{"MaxZ",  VMAXz_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {5.0f, 5.0f, 5.0f, 6.0f}},
	{"MaxW",  VMAXw_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {7.0f, 7.0f, 7.0f, 7.0f}},

	// ---- Fd ← min(Fs, Ft.bc) ----
	{"MiniX", VMINIx_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {1.0f, 2.0f, 2.0f, 2.0f}},
	{"MiniY", VMINIy_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {1.0f, 2.5f, 3.0f, 3.0f}},
	{"MiniZ", VMINIz_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {1.0f, 2.5f, 4.0f, 5.0f}},
	{"MiniW", VMINIw_U(mask::xyzw, kFd, kFs, kFt), Dest::Vf, {1.0f, 2.5f, 4.0f, 6.0f}},

	// ---- ACC ← Fs + Ft.bc (overwrites the accumulator) ----
	{"AddaX", VADDAx_U(mask::xyzw, kFs, kFt), Dest::Acc, {3.0f, 4.5f, 6.0f, 8.0f}},
	{"AddaY", VADDAy_U(mask::xyzw, kFs, kFt), Dest::Acc, {4.0f, 5.5f, 7.0f, 9.0f}},
	{"AddaZ", VADDAz_U(mask::xyzw, kFs, kFt), Dest::Acc, {6.0f, 7.5f, 9.0f, 11.0f}},
	{"AddaW", VADDAw_U(mask::xyzw, kFs, kFt), Dest::Acc, {8.0f, 9.5f, 11.0f, 13.0f}},

	// ---- ACC ← Fs - Ft.bc ----
	{"SubaX", VSUBAx_U(mask::xyzw, kFs, kFt), Dest::Acc, {-1.0f, 0.5f, 2.0f, 4.0f}},
	{"SubaY", VSUBAy_U(mask::xyzw, kFs, kFt), Dest::Acc, {-2.0f, -0.5f, 1.0f, 3.0f}},
	{"SubaZ", VSUBAz_U(mask::xyzw, kFs, kFt), Dest::Acc, {-4.0f, -2.5f, -1.0f, 1.0f}},
	{"SubaW", VSUBAw_U(mask::xyzw, kFs, kFt), Dest::Acc, {-6.0f, -4.5f, -3.0f, -1.0f}},

	// ---- ACC ← Fs * Ft.bc ----
	{"MulaX", VMULAx_U(mask::xyzw, kFs, kFt), Dest::Acc, {2.0f, 5.0f, 8.0f, 12.0f}},
	{"MulaY", VMULAy_U(mask::xyzw, kFs, kFt), Dest::Acc, {3.0f, 7.5f, 12.0f, 18.0f}},
	{"MulaZ", VMULAz_U(mask::xyzw, kFs, kFt), Dest::Acc, {5.0f, 12.5f, 20.0f, 30.0f}},
	{"MulaW", VMULAw_U(mask::xyzw, kFs, kFt), Dest::Acc, {7.0f, 17.5f, 28.0f, 42.0f}},

	// ---- ACC ← ACC + Fs * Ft.bc (accumulator is both source and dest) ----
	{"MaddaX", VMADDAx_U(mask::xyzw, kFs, kFt), Dest::Acc, {12.0f, 25.0f, 38.0f, 52.0f}},
	{"MaddaY", VMADDAy_U(mask::xyzw, kFs, kFt), Dest::Acc, {13.0f, 27.5f, 42.0f, 58.0f}},
	{"MaddaZ", VMADDAz_U(mask::xyzw, kFs, kFt), Dest::Acc, {15.0f, 32.5f, 50.0f, 70.0f}},
	{"MaddaW", VMADDAw_U(mask::xyzw, kFs, kFt), Dest::Acc, {17.0f, 37.5f, 58.0f, 82.0f}},

	// ---- ACC ← ACC - Fs * Ft.bc ----
	{"MsubaX", VMSUBAx_U(mask::xyzw, kFs, kFt), Dest::Acc, {8.0f, 15.0f, 22.0f, 28.0f}},
	{"MsubaY", VMSUBAy_U(mask::xyzw, kFs, kFt), Dest::Acc, {7.0f, 12.5f, 18.0f, 22.0f}},
	{"MsubaZ", VMSUBAz_U(mask::xyzw, kFs, kFt), Dest::Acc, {5.0f, 7.5f, 10.0f, 10.0f}},
	{"MsubaW", VMSUBAw_U(mask::xyzw, kFs, kFt), Dest::Acc, {3.0f, 2.5f, 2.0f, -2.0f}},
};

} // namespace

class VuBroadcastLane : public ::testing::TestWithParam<BcCase>
{
};

TEST_P(VuBroadcastLane, SelectsTheEncodedFtLane)
{
	const BcCase& c = GetParam();

	VuTestHarness h(0);
	h.SetVf(kFs, 1.0f, 2.5f, 4.0f, 6.0f);
	h.SetVf(kFt, 2.0f, 3.0f, 5.0f, 7.0f);
	h.SetVf(kFd, -101.0f, -102.0f, -103.0f, -104.0f); // sentinel — must be overwritten
	h.SetVf(kAccSrc, 10.0f, 20.0f, 30.0f, 40.0f);
	h.SetVf(kOnes, 1.0f, 1.0f, 1.0f, 1.0f);

	// MULA seeds ACC = (10, 20, 30, 40) for the MADD/MSUB families; the three
	// NOP pairs let the FMAC pipeline retire before the op under test reads it.
	h.LoadProgram({
		UpperOnly(VMULA_U(mask::xyzw, kAccSrc, kOnes)),
		NopPair(),
		NopPair(),
		NopPair(),
		UpperOnly(c.upper),
		EBitNopPair(),
	});
	h.Run(); // diffs JIT against interp across the whole architectural surface

	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	if (c.dest == Dest::Vf)
	{
		for (int i = 0; i < 4; i++)
			EXPECT_FLOAT_EQ(h.GetVfJit(kFd, kLane[i]), c.expect[i]) << "lane " << kLane[i];
	}
	else
	{
		const VURegs& j = h.JitSnapshot().regs;
		for (int i = 0; i < 4; i++)
			EXPECT_FLOAT_EQ(j.ACC.F[i], c.expect[i]) << "ACC lane " << kLane[i];
	}
}

INSTANTIATE_TEST_SUITE_P(
	Fmac, VuBroadcastLane, ::testing::ValuesIn(kCases),
	[](const ::testing::TestParamInfo<BcCase>& info) { return std::string(info.param.name); });

// =========================================================================
//  Single-lane destinations take a different emitter path.
//
//  A full-width mask goes through the packed helpers; a one-lane mask goes
//  through the scalar ones, and for y/z/w it first rotates that lane into
//  lane 0 (EXT) and rotates back on writeback. Combining a broadcast source
//  with a rotated destination crosses the two lane-selection mechanisms,
//  which is exactly where an off-by-one shows up. It also suppresses the
//  by-element FMUL fold, so MUL lands on the scalar helper instead.
// =========================================================================

namespace {

void RunSingleLane(u32 upper, char lane, float expect)
{
	VuTestHarness h(0);
	h.SetVf(kFs, 1.0f, 2.5f, 4.0f, 6.0f);
	h.SetVf(kFt, 2.0f, 3.0f, 5.0f, 7.0f);
	h.SetVf(kFd, -101.0f, -102.0f, -103.0f, -104.0f);
	h.LoadProgram({UpperOnly(upper), EBitNopPair()});
	h.Run();

	EXPECT_FLOAT_EQ(h.GetVfJit(kFd, lane), expect);
	// The three unwritten lanes keep their sentinel — a rotation that fails to
	// unwind would smear the result across them.
	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	static const float kSentinel[4] = {-101.0f, -102.0f, -103.0f, -104.0f};
	for (int i = 0; i < 4; i++)
	{
		if (kLane[i] == lane)
			continue;
		EXPECT_FLOAT_EQ(h.GetVfJit(kFd, kLane[i]), kSentinel[i]) << "lane " << kLane[i];
	}
}

} // namespace

// Fs.x = 1.0, Ft.z = 5.0 → 6.0 into Fd.x only (no rotation: x is lane 0).
TEST(VuBroadcastSingleLane, AddZIntoXOnly)
{
	RunSingleLane(VADDz_U(mask::x, kFd, kFs, kFt), 'x', 6.0f);
}

// Fs.y = 2.5, Ft.w = 7.0 → 9.5 into Fd.y only (rotate lane 1 → 0 and back).
TEST(VuBroadcastSingleLane, AddWIntoYOnly)
{
	RunSingleLane(VADDw_U(mask::y, kFd, kFs, kFt), 'y', 9.5f);
}

// Fs.z = 4.0, Ft.x = 2.0 → 8.0 into Fd.z only. MUL at a single-lane mask
// bypasses the by-element fold and uses the scalar multiply helper.
TEST(VuBroadcastSingleLane, MulXIntoZOnly)
{
	RunSingleLane(VMULx_U(mask::z, kFd, kFs, kFt), 'z', 8.0f);
}

// Fs.w = 6.0, Ft.y = 3.0 → 18.0 into Fd.w only (rotate lane 3 → 0 and back).
TEST(VuBroadcastSingleLane, MulYIntoWOnly)
{
	RunSingleLane(VMULy_U(mask::w, kFd, kFs, kFt), 'w', 18.0f);
}

// Fs.w = 6.0, Ft.z = 5.0 → 1.0 into Fd.w only.
TEST(VuBroadcastSingleLane, SubZIntoWOnly)
{
	RunSingleLane(VSUBz_U(mask::w, kFd, kFs, kFt), 'w', 1.0f);
}

// =========================================================================
//  The by-element FMUL fold on the plain MULbc path.
//
//  The fold replaces "splat Ft.bc across a scratch register, then multiply"
//  with a single multiply that names the lane directly. It is suppressed
//  whenever an Ft clamp would be emitted — and MULbc at a packed mask asks
//  for an Ft clamp, so under the shipped clamp default the plain MUL never
//  reaches the fold at all. MADDbc, MSUBbc and MULAbc do, which is why the
//  sweep above covers the mechanism; this pair covers the one op where
//  reaching it takes a non-default clamp mode.
//
//  Worth having because the fold is where the lane index stops being a
//  shuffle operand and becomes part of the multiply encoding — a different
//  way to get the lane wrong than anything above tests.
// =========================================================================

namespace {
struct ScopedVuOverflowClamp
{
	bool prev = EmuConfig.Cpu.Recompiler.vu0Overflow;
	explicit ScopedVuOverflowClamp(bool on) { EmuConfig.Cpu.Recompiler.vu0Overflow = on; }
	~ScopedVuOverflowClamp() { EmuConfig.Cpu.Recompiler.vu0Overflow = prev; }
};
} // namespace

TEST(VuBroadcastLaneFold, MulSelectsTheEncodedLaneWithoutTheOperandClamp)
{
	ScopedVuOverflowClamp clamp(false); // opens the fold for MULbc

	struct
	{
		u32 upper;
		float expect[4];
	} const cases[] = {
		{VMULx_U(mask::xyzw, kFd, kFs, kFt), {2.0f, 5.0f, 8.0f, 12.0f}},
		{VMULy_U(mask::xyzw, kFd, kFs, kFt), {3.0f, 7.5f, 12.0f, 18.0f}},
		{VMULz_U(mask::xyzw, kFd, kFs, kFt), {5.0f, 12.5f, 20.0f, 30.0f}},
		{VMULw_U(mask::xyzw, kFd, kFs, kFt), {7.0f, 17.5f, 28.0f, 42.0f}},
	};

	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	for (int c = 0; c < 4; c++)
	{
		SCOPED_TRACE(::testing::Message() << "broadcast lane " << kLane[c]);
		VuTestHarness h(0);
		h.SetVf(kFs, 1.0f, 2.5f, 4.0f, 6.0f);
		h.SetVf(kFt, 2.0f, 3.0f, 5.0f, 7.0f);
		h.SetVf(kFd, -101.0f, -102.0f, -103.0f, -104.0f);
		h.LoadProgram({UpperOnly(cases[c].upper), EBitNopPair()});
		h.Run();
		for (int i = 0; i < 4; i++)
			EXPECT_FLOAT_EQ(h.GetVfJit(kFd, kLane[i]), cases[c].expect[i]) << "lane " << kLane[i];
	}
}

} // namespace recompiler_tests
