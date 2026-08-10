// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// EE FPU producer/consumer pairs against the console.
//
// Every other FPU test in this directory measures one instruction. That is
// structurally unable to answer what the arm64 fast path's result clamp
// (`fpuClampResult`, iFPU-arm64.cpp) is for: the clamp exists to keep a host
// Inf/NaN out of the register file so the host's Inf/NaN algebra cannot
// diverge from the EE's finite algebra on a later instruction. One instruction
// per case never reaches the later instruction.
//
// Measured on SCPH-90000, 2026-07-31, 169 pairs, two runs byte-identical
// (loaded into fpm2.sqlite behind the casc_v view). The result:
//
//   The EE has no absorbing value.
//
// An exponent-255 word is an ordinary finite number to every consumer. The
// console computes 0x7FC00000 / 0x7FC00000 = 1.0, and EEMAX * 0.0 = 0.0. There
// is no NaN on this machine, so every absorbing value that shows up in an
// emulator's register file is the host's.
//
// Scores on the full 169 (result axis, final value):
//
//   armsx2-interp                     169/169
//   arm64 JIT, fpuFullMode=true       169/169
//   arm64 JIT, fast path (shipping)    89/169
//   arm64 JIT, fast path, no clamp    109/169
//
// The 20 rows transcribed below pin the two engines that are right. The fast
// path computes in host single precision, which cannot represent the EE's
// domain (exponent 255 ordinary, saturation at 0x7FFFFFFF, no Inf, no NaN), so
// which rows it misses is not a contract: it cannot carry a legitimate EEMAX
// without carrying a word the host absorbs on, and no choice of clamp changes
// that. The one contract is that it must not produce an absorbing NaN, which
// is why the clamp is still there. See ee-has-no-absorption in the memory dir.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kA = 20, kB = 21, kMid = 22, kOther = 23, kOut = 24;

// Producer: writes kMid. Consumer: reads kMid, writes kOut.
struct Pair
{
	u32 prod;      // producer instruction
	u32 cons;      // consumer instruction
	u32 a, b;      // producer operands (kA, kB)
	u32 other;     // consumer's second operand (kOther)
	u32 want_mid;  // console: producer's result
	u32 want_fin;  // console: consumer's result
	const char* what;
};

constexpr u32 kP0 = 0x00000000u, kOne = 0x3F800000u;
constexpr u32 kFMax = 0x7F7FFFFFu, kNFMax = 0xFF7FFFFFu;
constexpr u32 kE128 = 0x7F800000u;   // 2^128 -- IEEE +Inf, ordinary on the EE
constexpr u32 kQNan = 0x7FC00000u;   // exp255 mant 0x400000 -- ordinary too
constexpr u32 kEEMax = 0x7FFFFFFFu;  // the largest number the EE has

// Every row is a console reading from the cascade capture. Nothing here is
// derived from any engine.
const Pair kConsole[] = {
	// --- the absorption question, asked four ways -------------------------
	{MUL_S(kMid, kA, kB), MUL_S(kOut, kMid, kOther), kEEMax, kEEMax, kP0,
		kEEMax, kP0, "mul EEMAX,EEMAX then x*0 = 0 (no absorption)"},
	{MOV_S(kMid, kA), MUL_S(kOut, kMid, kOther), kEEMax, kP0, kP0,
		kEEMax, kP0, "mov EEMAX then x*0 = 0"},
	{MOV_S(kMid, kA), MUL_S(kOut, kMid, kOther), kE128, kP0, kP0,
		kE128, kP0, "mov 2^128 then x*0 = 0"},
	{MOV_S(kMid, kA), MUL_S(kOut, kMid, kOther), kQNan, kP0, kP0,
		kQNan, kP0, "mov 0x7FC00000 then x*0 = 0"},

	// --- self-subtract: the EE cancels, the host would NaN ----------------
	{MOV_S(kMid, kA), SUB_S(kOut, kMid, kMid), kEEMax, kP0, kP0,
		kEEMax, kP0, "mov EEMAX then x-x = 0"},
	{MOV_S(kMid, kA), SUB_S(kOut, kMid, kMid), kE128, kP0, kP0,
		kE128, kP0, "mov 2^128 then x-x = 0"},
	{MOV_S(kMid, kA), SUB_S(kOut, kMid, kMid), kQNan, kP0, kP0,
		kQNan, kP0, "mov 0x7FC00000 then x-x = 0"},

	// --- self-divide: 1.0, even for the host's NaN pattern ----------------
	{MOV_S(kMid, kA), DIV_S(kOut, kMid, kMid), kEEMax, kP0, kP0,
		kEEMax, kOne, "mov EEMAX then x/x = 1.0"},
	{MOV_S(kMid, kA), DIV_S(kOut, kMid, kMid), kE128, kP0, kP0,
		kE128, kOne, "mov 2^128 then x/x = 1.0"},
	{MOV_S(kMid, kA), DIV_S(kOut, kMid, kMid), kQNan, kP0, kP0,
		kQNan, kOne, "mov 0x7FC00000 then x/x = 1.0"},
	{MUL_S(kMid, kA, kB), DIV_S(kOut, kMid, kMid), kFMax, kFMax, kP0,
		kEEMax, kOne, "mul FMAX,FMAX saturates to EEMAX, then x/x = 1.0"},

	// --- arithmetic overflow saturates to EEMAX, and stays finite ---------
	{MUL_S(kMid, kA, kB), MUL_S(kOut, kMid, kOther), kFMax, kFMax, kP0,
		kEEMax, kP0, "mul FMAX,FMAX then x*0 = 0"},
	{ADD_S(kMid, kA, kB), MUL_S(kOut, kMid, kOther), kFMax, kFMax, kP0,
		kEEMax, kP0, "add FMAX,FMAX then x*0 = 0"},
	{SUB_S(kMid, kA, kB), MUL_S(kOut, kMid, kOther), kNFMax, kFMax, kP0,
		0xFFFFFFFFu, 0x80000000u, "sub -FMAX,FMAX then x*0 = -0"},

	// --- the top binade survives an identity op ---------------------------
	{MOV_S(kMid, kA), MUL_S(kOut, kMid, kOther), kEEMax, kP0, kOne,
		kEEMax, kEEMax, "mov EEMAX then x*1 = EEMAX"},
	{MOV_S(kMid, kA), ADD_S(kOut, kMid, kOther), kEEMax, kP0, kP0,
		kEEMax, kEEMax, "mov EEMAX then x+0 = EEMAX"},
	{ADD_S(kMid, kA, kB), ADD_S(kOut, kMid, kOther), kE128, kP0, kP0,
		kE128, kE128, "add 2^128,0 then x+0 = 2^128"},

	// --- controls: ordinary values, every engine must agree ---------------
	{MUL_S(kMid, kA, kB), MUL_S(kOut, kMid, kOther), kOne, 0x40000000u, kP0,
		0x40000000u, kP0, "CONTROL mul 1,2 then x*0 = 0"},
	{MOV_S(kMid, kA), MUL_S(kOut, kMid, kOther), kFMax, kP0, kP0,
		kFMax, kP0, "CONTROL mov FMAX then x*0 = 0"},
	{MOV_S(kMid, kA), DIV_S(kOut, kMid, kMid), kFMax, kP0, kP0,
		kFMax, kOne, "CONTROL mov FMAX then x/x = 1.0"},
};
constexpr int kConsoleCount = static_cast<int>(sizeof(kConsole) / sizeof(kConsole[0]));

enum Leg { LEG_INTERP, LEG_JIT_FAST, LEG_JIT_FULL, LEG_COUNT };

const char* LegName(Leg l)
{
	switch (l)
	{
		case LEG_INTERP:   return "interp";
		case LEG_JIT_FAST: return "jit-fast";
		case LEG_JIT_FULL: return "jit-full";
		default:           return "?";
	}
}

struct Observed { u32 mid, fin; };

Observed RunPair(const Pair& p, Leg leg)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (leg == LEG_JIT_FULL)
		h.EnableFpuFullMode();
	h.SetFprBits(kA, p.a);
	h.SetFprBits(kB, p.b);
	h.SetFprBits(kOther, p.other);
	// Park a recognisable word in both destinations so an op that writes
	// nothing reads as such rather than as a stale success.
	h.SetFprBits(kMid, 0xDEADBEEFu);
	h.SetFprBits(kOut, 0xDEADBEEFu);
	h.LoadProgram({p.prod, p.cons});
	if (leg == LEG_INTERP)
		h.RunInterpOnly();
	else
		h.RunJitNoDiff();
	// RunJitNoDiff never runs the interpreter, so GetFprBitsInterp would hand
	// back the JIT's own register file -- read the side that ran.
	if (leg == LEG_INTERP)
		return {h.GetFprBitsInterp(kMid), h.GetFprBitsInterp(kOut)};
	return {h.GetFprBitsJit(kMid), h.GetFprBitsJit(kOut)};
}

bool IsHostNan(u32 w)
{
	return (w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu) != 0;
}

} // namespace

// ---------------------------------------------------------------------------
// The interpreter matched the console on all 169 captured pairs. The 20
// transcribed here are the must-not-regress control.
// ---------------------------------------------------------------------------
TEST(EeFpuCascadeConsole, InterpMatchesConsoleOnPairs)
{
	int checked = 0;
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const Pair& p = kConsole[i];
		SCOPED_TRACE(p.what);
		const Observed o = RunPair(p, LEG_INTERP);
		EXPECT_EQ(o.mid, p.want_mid) << "producer";
		EXPECT_EQ(o.fin, p.want_fin) << "consumer";
		++checked;
	}
	EXPECT_EQ(checked, kConsoleCount) << "anti-vacuity";
}

// ---------------------------------------------------------------------------
// Full clamp mode (the DOUBLE tier) is the other 169/169 engine: it computes
// in a domain that can hold the EE's range.
// ---------------------------------------------------------------------------
TEST(EeFpuCascadeConsole, FullModeMatchesConsoleOnPairs)
{
	int checked = 0;
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const Pair& p = kConsole[i];
		SCOPED_TRACE(p.what);
		const Observed o = RunPair(p, LEG_JIT_FULL);
		EXPECT_EQ(o.mid, p.want_mid) << "producer";
		EXPECT_EQ(o.fin, p.want_fin) << "consumer";
		++checked;
	}
	EXPECT_EQ(checked, kConsoleCount) << "anti-vacuity";
}

// ---------------------------------------------------------------------------
// The fast path is wrong here by construction. What this asserts is the one
// property the result clamp buys, and the reason it is still emitted
// unconditionally: the fast path must never hand back an absorbing host NaN.
//
// Deleting fpuClampResult wins 20 of the 169 captured pairs and lands a host-NaN
// bit pattern where the console produced an ordinary word on 22 of them. A NaN
// in a vertex position propagates through every later transform; a
// wrong-but-finite 0x7F7FFFFF costs one vertex. That trade is why the single-op
// corpus's "70 cases onto silicon, 0 away" is not the whole story.
//
// IsHostNan is "exponent 255 with a non-zero mantissa", not "== 0x7FC00000":
// 0x7FFFFFFF is EEMAX to the console and a signalling NaN to the host, so it
// absorbs identically, and 12 of the 22 are that pattern. Counting only the
// canonical qNaN reported 11 and hid half the class.
// ---------------------------------------------------------------------------
TEST(EeFpuCascadeConsole, FastPathNeverInventsAnAbsorbingNan)
{
	int checked = 0, at_risk = 0;
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const Pair& p = kConsole[i];
		SCOPED_TRACE(p.what);
		const Observed o = RunPair(p, LEG_JIT_FAST);
		// A NaN-shaped word is an ordinary EE number and the console hands one
		// back in some of these rows (mov EEMAX then x*1 = 0x7FFFFFFF), so only
		// rows with a non-NaN console answer can be checked.
		if (!IsHostNan(p.want_fin))
		{
			EXPECT_FALSE(IsHostNan(o.fin))
				<< "fast path produced host NaN " << std::hex << o.fin
				<< " where the console said " << p.want_fin
				<< " -- the console has no absorbing value, and preventing this "
				   "is the one thing fpuClampResult still buys";
			++at_risk;
		}
		++checked;
	}
	EXPECT_EQ(checked, kConsoleCount) << "anti-vacuity";
	EXPECT_GT(at_risk, 12)
		<< "anti-vacuity: most rows must have a non-NaN console answer, or this "
		   "test asserts nothing";
}

// ---------------------------------------------------------------------------
// The property behind every row above, asserted directly rather than only
// through the transcribed table: on the console x/x is 1.0 for every operand
// with a non-zero exponent field, including the two exponent-255 patterns a
// host reads as Inf and NaN. Both 169/169 engines, so this pins their
// agreement as well.
//
// Exponent field, not value: denormals are zero on the EE (DAZ is not optional
// on this silicon), so a denormal self-divide is the 0/0 zero-divisor case and
// answers 0x7FFFFFFF instead. That is the test below, and why the pool here
// holds no denormals.
// ---------------------------------------------------------------------------
TEST(EeFpuCascadeConsole, SelfDivideIsOneForEveryNonZeroExponentIncludingExp255)
{
	static constexpr u32 kPool[] = {
		kOne, 0x40000000u, 0xBF800000u, kFMax, kNFMax,
		kE128, 0xFF800000u, kQNan, 0xFFC00000u, kEEMax, 0xFFFFFFFFu,
		0x00800000u, 0x00800001u,
	};
	constexpr int kPoolCount = static_cast<int>(sizeof(kPool) / sizeof(kPool[0]));

	int checked = 0, exp255 = 0;
	for (int leg = 0; leg < 2; ++leg)
	{
		const Leg l = (leg == 0) ? LEG_INTERP : LEG_JIT_FULL;
		for (int i = 0; i < kPoolCount; ++i)
		{
			const u32 v = kPool[i];
			ASSERT_NE(v & 0x7F800000u, 0u) << "pool must hold no denormals";
			SCOPED_TRACE(testing::Message()
				<< "x=" << std::hex << v << " [" << LegName(l) << "]");
			Pair p{MOV_S(kMid, kA), DIV_S(kOut, kMid, kMid), v, kP0, kP0,
				v, kOne, "self-divide"};
			const Observed o = RunPair(p, l);
			EXPECT_EQ(o.mid, v) << "mov.s must pass the word through";
			EXPECT_EQ(o.fin, kOne) << "x/x is 1.0 for any x with a live exponent";
			++checked;
			if (leg == 0 && (v & 0x7F800000u) == 0x7F800000u)
				++exp255;
		}
	}
	EXPECT_EQ(checked, kPoolCount * 2) << "anti-vacuity";
	EXPECT_GT(exp255, 5) << "anti-vacuity: the pool must keep exponent-255 "
							"operands -- they are the whole point";
}

// ---------------------------------------------------------------------------
// The other side of that boundary, from the capture: a denormal is zero, so
// multiplying two of them gives zero, and dividing that zero by itself takes
// the zero-divisor path to 0x7FFFFFFF. Capture row: producer
// `mul MIN_DEN,MIN_DEN`, consumer `x / x`.
// ---------------------------------------------------------------------------
TEST(EeFpuCascadeConsole, DenormalsAreZeroSoSelfDivideTakesTheZeroDivisorPath)
{
	const Pair p{MUL_S(kMid, kA, kB), DIV_S(kOut, kMid, kMid),
		0x00000001u, 0x00000001u, kP0, kP0, kEEMax,
		"mul MIN_DEN,MIN_DEN = 0, then 0/0 = EEMAX"};
	for (int leg = 0; leg < 2; ++leg)
	{
		const Leg l = (leg == 0) ? LEG_INTERP : LEG_JIT_FULL;
		SCOPED_TRACE(LegName(l));
		const Observed o = RunPair(p, l);
		EXPECT_EQ(o.mid, kP0) << "denormal * denormal is +0 on the EE";
		EXPECT_EQ(o.fin, kEEMax) << "0/0 takes the zero-divisor path";
	}
}
