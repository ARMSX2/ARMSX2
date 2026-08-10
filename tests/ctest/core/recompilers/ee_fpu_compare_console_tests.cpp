// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The EE's FPU compares, against the console. All 86 compare rows of the
// 1147-case SCPH-90000 capture, transcribed.
//
// A compare on this FPU compares two real numbers, and every EE word is one:
// there is no Inf, no NaN, no denormal. 0x7F800000 is 2^128, 0x7FC00000 is
// 1.5 * 2^128 (the corpus still calls it QNAN, which is what the same bits mean
// to a host), and 0x7FFFFFFF is the largest number the machine has. Anything
// with exponent 0 is signed zero.
//
// The interpreter used to run these through fpuDouble()'s clamp, which is why
// four rows moved: c.eq.s of 0x7F7FFFFF or 0x7F800000 against 0x7FFFFFFF came
// back true where silicon says false, and the matching c.lt.s came back false
// where silicon says true. What the clamp did is at C_cond_S in FPU.cpp, which
// reads eeToDouble() now.
//
// The three tiers over these 86 rows:
//
//     interpreter   86/86   (this file, InterpMatchesConsoleOnEveryRow)
//     fpuFullMode   86/86   (DOUBLE::recC_*_xmm, shares no code with FPU.cpp)
//     fast path     82/86   (clamps, deliberately; FastPathIsWrongInTheseExactWays)
//
// The fast path is the one tier that cannot simply drop the clamp: it compares
// in host singles, and 0x7FFFFFFF read as a host single is a NaN, unordered
// against everything. Making it exact means an integer sign-magnitude compare
// or the double path fpuFullMode already charges for, a change with its own
// cost argument.
//
// Expected FCR31 is the console's, and `got` is masked as CFC1 masks it (see
// kCfc1Mask), so every row is a flag test as well as a condition test: rows
// 736/737/738 come in with the sticky field pre-seeded and pin that a compare
// touches C and nothing else. 738 comes in with C already set and a false
// condition, so it is the row that pins the clear leg.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFs = 5, kFt = 6;

enum Cond { C_F, C_EQ, C_LT, C_LE };

u32 Encode(Cond c)
{
	switch (c)
	{
		case C_F:  return C_F_S(kFs, kFt);
		case C_EQ: return C_EQ_S(kFs, kFt);
		case C_LT: return C_LT_S(kFs, kFt);
		default:   return C_LE_S(kFs, kFt);
	}
}

const char* Name(Cond c)
{
	switch (c)
	{
		case C_F:  return "c.f.s";
		case C_EQ: return "c.eq.s";
		case C_LT: return "c.lt.s";
		default:   return "c.le.s";
	}
}

struct ConsoleCase
{
	int ordinal;
	Cond cond;
	u32 fs, ft;
	u32 fcr_pre;    // seeded before the op, as the corpus vehicle seeds it
	u32 want_fcr31; // the console's, CFC1-masked
	bool fast_diverges;
	const char* what;
};

constexpr ConsoleCase kConsole[] = {
	// --- c.f.s -- the condition is false unconditionally, so this block is a
	// test that the operands are never looked at ---
	{ 399, C_F  , 0x00000000u, 0x00000000u, 0x00000000u, 0x01000001u, false, "cf P0, P0"},
	{ 400, C_F  , 0x00000000u, 0x80000000u, 0x00000000u, 0x01000001u, false, "cf P0, N0"},
	{ 401, C_F  , 0x80000000u, 0x00000000u, 0x00000000u, 0x01000001u, false, "cf N0, P0"},
	{ 402, C_F  , 0x80000000u, 0x80000000u, 0x00000000u, 0x01000001u, false, "cf N0, N0"},
	{ 403, C_F  , 0x3F800000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "cf ONE, ONE"},
	{ 404, C_F  , 0x3F800000u, 0x40000000u, 0x00000000u, 0x01000001u, false, "cf ONE, TWO"},
	{ 405, C_F  , 0x40000000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "cf TWO, ONE"},
	{ 406, C_F  , 0xBF800000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "cf NONE, ONE"},
	{ 407, C_F  , 0x3F800000u, 0xBF800000u, 0x00000000u, 0x01000001u, false, "cf ONE, NONE"},
	{ 408, C_F  , 0x00000001u, 0x00000000u, 0x00000000u, 0x01000001u, false, "cf MIN_DEN, P0"},
	{ 409, C_F  , 0x00000000u, 0x00000001u, 0x00000000u, 0x01000001u, false, "cf P0, MIN_DEN"},
	{ 410, C_F  , 0x00000001u, 0x80000001u, 0x00000000u, 0x01000001u, false, "cf MIN_DEN, NMIN_DEN"},
	{ 411, C_F  , 0x00800000u, 0x007FFFFFu, 0x00000000u, 0x01000001u, false, "cf MIN_NORM, MAX_DEN"},
	{ 412, C_F  , 0x7F7FFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x01000001u, false, "cf FMAX, EEMAX"},
	{ 413, C_F  , 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x01000001u, false, "cf EEMAX, EEMAX"},
	{ 414, C_F  , 0x7F800000u, 0x7FFFFFFFu, 0x00000000u, 0x01000001u, false, "cf E128, EEMAX"},
	{ 415, C_F  , 0x7FC00000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "cf QNAN, ONE"},
	{ 416, C_F  , 0x3F800000u, 0x7FC00000u, 0x00000000u, 0x01000001u, false, "cf ONE, QNAN"},
	{ 417, C_F  , 0x7FC00000u, 0x7FC00000u, 0x00000000u, 0x01000001u, false, "cf QNAN, QNAN"},
	{ 418, C_F  , 0xDEADBEEFu, 0xDEADBEEFu, 0x00000000u, 0x01000001u, false, "cf GARB2, GARB2"},

	// --- c.eq.s ---
	{ 419, C_EQ , 0x00000000u, 0x00000000u, 0x00000000u, 0x01800001u, false, "ceq P0, P0"},
	{ 420, C_EQ , 0x00000000u, 0x80000000u, 0x00000000u, 0x01800001u, false, "ceq P0, N0"},
	{ 421, C_EQ , 0x80000000u, 0x00000000u, 0x00000000u, 0x01800001u, false, "ceq N0, P0"},
	{ 422, C_EQ , 0x80000000u, 0x80000000u, 0x00000000u, 0x01800001u, false, "ceq N0, N0"},
	{ 423, C_EQ , 0x3F800000u, 0x3F800000u, 0x00000000u, 0x01800001u, false, "ceq ONE, ONE"},
	{ 424, C_EQ , 0x3F800000u, 0x40000000u, 0x00000000u, 0x01000001u, false, "ceq ONE, TWO"},
	{ 425, C_EQ , 0x40000000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "ceq TWO, ONE"},
	{ 426, C_EQ , 0xBF800000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "ceq NONE, ONE"},
	{ 427, C_EQ , 0x3F800000u, 0xBF800000u, 0x00000000u, 0x01000001u, false, "ceq ONE, NONE"},
	{ 428, C_EQ , 0x00000001u, 0x00000000u, 0x00000000u, 0x01800001u, false, "ceq MIN_DEN, P0"},
	{ 429, C_EQ , 0x00000000u, 0x00000001u, 0x00000000u, 0x01800001u, false, "ceq P0, MIN_DEN"},
	{ 430, C_EQ , 0x00000001u, 0x80000001u, 0x00000000u, 0x01800001u, false, "ceq MIN_DEN, NMIN_DEN"},
	{ 431, C_EQ , 0x00800000u, 0x007FFFFFu, 0x00000000u, 0x01000001u, false, "ceq MIN_NORM, MAX_DEN"},
	{ 432, C_EQ , 0x7F7FFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x01000001u, true , "ceq FMAX, EEMAX"},
	{ 433, C_EQ , 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x01800001u, false, "ceq EEMAX, EEMAX"},
	{ 434, C_EQ , 0x7F800000u, 0x7FFFFFFFu, 0x00000000u, 0x01000001u, true , "ceq E128, EEMAX"},
	{ 435, C_EQ , 0x7FC00000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "ceq QNAN, ONE"},
	{ 436, C_EQ , 0x3F800000u, 0x7FC00000u, 0x00000000u, 0x01000001u, false, "ceq ONE, QNAN"},
	{ 437, C_EQ , 0x7FC00000u, 0x7FC00000u, 0x00000000u, 0x01800001u, false, "ceq QNAN, QNAN"},
	{ 438, C_EQ , 0xDEADBEEFu, 0xDEADBEEFu, 0x00000000u, 0x01800001u, false, "ceq GARB2, GARB2"},
	{ 715, C_EQ , 0x3F800000u, 0x3F800000u, 0x00000000u, 0x01800001u, false, "ceq ONE, ONE"},
	{ 736, C_EQ , 0x3F800000u, 0x3F800000u, 0x0083C078u, 0x0183C079u, false, "ceq ONE, ONE"},

	// --- c.lt.s ---
	{ 439, C_LT , 0x00000000u, 0x00000000u, 0x00000000u, 0x01000001u, false, "clt P0, P0"},
	{ 440, C_LT , 0x00000000u, 0x80000000u, 0x00000000u, 0x01000001u, false, "clt P0, N0"},
	{ 441, C_LT , 0x80000000u, 0x00000000u, 0x00000000u, 0x01000001u, false, "clt N0, P0"},
	{ 442, C_LT , 0x80000000u, 0x80000000u, 0x00000000u, 0x01000001u, false, "clt N0, N0"},
	{ 443, C_LT , 0x3F800000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "clt ONE, ONE"},
	{ 444, C_LT , 0x3F800000u, 0x40000000u, 0x00000000u, 0x01800001u, false, "clt ONE, TWO"},
	{ 445, C_LT , 0x40000000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "clt TWO, ONE"},
	{ 446, C_LT , 0xBF800000u, 0x3F800000u, 0x00000000u, 0x01800001u, false, "clt NONE, ONE"},
	{ 447, C_LT , 0x3F800000u, 0xBF800000u, 0x00000000u, 0x01000001u, false, "clt ONE, NONE"},
	{ 448, C_LT , 0x00000001u, 0x00000000u, 0x00000000u, 0x01000001u, false, "clt MIN_DEN, P0"},
	{ 449, C_LT , 0x00000000u, 0x00000001u, 0x00000000u, 0x01000001u, false, "clt P0, MIN_DEN"},
	{ 450, C_LT , 0x00000001u, 0x80000001u, 0x00000000u, 0x01000001u, false, "clt MIN_DEN, NMIN_DEN"},
	{ 451, C_LT , 0x00800000u, 0x007FFFFFu, 0x00000000u, 0x01000001u, false, "clt MIN_NORM, MAX_DEN"},
	{ 452, C_LT , 0x7F7FFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x01800001u, true , "clt FMAX, EEMAX"},
	{ 453, C_LT , 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x01000001u, false, "clt EEMAX, EEMAX"},
	{ 454, C_LT , 0x7F800000u, 0x7FFFFFFFu, 0x00000000u, 0x01800001u, true , "clt E128, EEMAX"},
	{ 455, C_LT , 0x7FC00000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "clt QNAN, ONE"},
	{ 456, C_LT , 0x3F800000u, 0x7FC00000u, 0x00000000u, 0x01800001u, false, "clt ONE, QNAN"},
	{ 457, C_LT , 0x7FC00000u, 0x7FC00000u, 0x00000000u, 0x01000001u, false, "clt QNAN, QNAN"},
	{ 458, C_LT , 0xDEADBEEFu, 0xDEADBEEFu, 0x00000000u, 0x01000001u, false, "clt GARB2, GARB2"},
	{ 716, C_LT , 0x7FC00000u, 0x7FC00000u, 0x00000000u, 0x01000001u, false, "clt QNAN, QNAN"},
	{ 737, C_LT , 0x3F800000u, 0x40000000u, 0x0083C078u, 0x0183C079u, false, "clt ONE, TWO"},

	// --- c.le.s ---
	{ 459, C_LE , 0x00000000u, 0x00000000u, 0x00000000u, 0x01800001u, false, "cle P0, P0"},
	{ 460, C_LE , 0x00000000u, 0x80000000u, 0x00000000u, 0x01800001u, false, "cle P0, N0"},
	{ 461, C_LE , 0x80000000u, 0x00000000u, 0x00000000u, 0x01800001u, false, "cle N0, P0"},
	{ 462, C_LE , 0x80000000u, 0x80000000u, 0x00000000u, 0x01800001u, false, "cle N0, N0"},
	{ 463, C_LE , 0x3F800000u, 0x3F800000u, 0x00000000u, 0x01800001u, false, "cle ONE, ONE"},
	{ 464, C_LE , 0x3F800000u, 0x40000000u, 0x00000000u, 0x01800001u, false, "cle ONE, TWO"},
	{ 465, C_LE , 0x40000000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "cle TWO, ONE"},
	{ 466, C_LE , 0xBF800000u, 0x3F800000u, 0x00000000u, 0x01800001u, false, "cle NONE, ONE"},
	{ 467, C_LE , 0x3F800000u, 0xBF800000u, 0x00000000u, 0x01000001u, false, "cle ONE, NONE"},
	{ 468, C_LE , 0x00000001u, 0x00000000u, 0x00000000u, 0x01800001u, false, "cle MIN_DEN, P0"},
	{ 469, C_LE , 0x00000000u, 0x00000001u, 0x00000000u, 0x01800001u, false, "cle P0, MIN_DEN"},
	{ 470, C_LE , 0x00000001u, 0x80000001u, 0x00000000u, 0x01800001u, false, "cle MIN_DEN, NMIN_DEN"},
	{ 471, C_LE , 0x00800000u, 0x007FFFFFu, 0x00000000u, 0x01000001u, false, "cle MIN_NORM, MAX_DEN"},
	{ 472, C_LE , 0x7F7FFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x01800001u, false, "cle FMAX, EEMAX"},
	{ 473, C_LE , 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x01800001u, false, "cle EEMAX, EEMAX"},
	{ 474, C_LE , 0x7F800000u, 0x7FFFFFFFu, 0x00000000u, 0x01800001u, false, "cle E128, EEMAX"},
	{ 475, C_LE , 0x7FC00000u, 0x3F800000u, 0x00000000u, 0x01000001u, false, "cle QNAN, ONE"},
	{ 476, C_LE , 0x3F800000u, 0x7FC00000u, 0x00000000u, 0x01800001u, false, "cle ONE, QNAN"},
	{ 477, C_LE , 0x7FC00000u, 0x7FC00000u, 0x00000000u, 0x01800001u, false, "cle QNAN, QNAN"},
	{ 478, C_LE , 0xDEADBEEFu, 0xDEADBEEFu, 0x00000000u, 0x01800001u, false, "cle GARB2, GARB2"},
	{ 717, C_LE , 0x80000000u, 0x80000000u, 0x00000000u, 0x01800001u, false, "cle N0, N0"},
	{ 738, C_LE , 0x7FC00000u, 0x3F800000u, 0x0083C078u, 0x0103C079u, false, "cle QNAN, ONE"},
};
constexpr int kConsoleCount = static_cast<int>(sizeof(kConsole) / sizeof(kConsole[0]));

// What CFC1 hands a program: the always-zero bits dropped, the always-one bits
// set. FPU.cpp CFC1 and both recompilers' recCFC1 apply exactly this, and the
// capture's FCR31 column came through it, so the raw fprc[31] has to be put
// through it too before the two can be compared.
constexpr u32 kCfc1Mask = 0x0083C078u, kCfc1Ones = 0x01000001u;

enum Tier { TIER_INTERP, TIER_FAST, TIER_FULL };

u32 RunOne(const ConsoleCase& c, Tier tier)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (tier == TIER_FULL)
		h.EnableFpuFullMode();
	h.SetFcr31(c.fcr_pre);
	h.SetFprBits(kFs, c.fs);
	h.SetFprBits(kFt, c.ft);
	h.LoadProgram({Encode(c.cond)});
	if (tier == TIER_INTERP)
		h.RunInterpOnly();
	else
		h.RunJitNoDiff();

	const u32 raw = (tier == TIER_INTERP ? h.InterpSnapshot() : h.JitSnapshot()).fprs.fprc[31];
	return (raw & kCfc1Mask) | kCfc1Ones;
}

} // namespace

// ---------------------------------------------------------------------------
// The interpreter, which is what moved. Rows 432/434/452/454 failed before the
// operand clamp came off the compares; the other 82 must not have moved.
// ---------------------------------------------------------------------------
TEST(EeFpuCompareConsole, InterpMatchesConsoleOnEveryRow)
{
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(::testing::Message() << "[fpm] case " << c.ordinal << " "
			<< Name(c.cond) << " -- " << c.what);
		EXPECT_EQ(RunOne(c, TIER_INTERP), c.want_fcr31);
	}
	EXPECT_EQ(kConsoleCount, 86) << "anti-vacuity: the console table emptied";
}

// ---------------------------------------------------------------------------
// The full double path was already exact on all 86 rows and shares no code with
// FPU.cpp. Pinned so the two exact tiers cannot drift apart silently.
// ---------------------------------------------------------------------------
TEST(EeFpuCompareConsole, FullModeMatchesConsoleOnEveryRow)
{
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(::testing::Message() << "[fpm] case " << c.ordinal << " "
			<< Name(c.cond) << " -- " << c.what);
		const u32 full = RunOne(c, TIER_FULL);
		EXPECT_EQ(full, c.want_fcr31) << "the FULL path stopped matching the console";
		EXPECT_EQ(full, RunOne(c, TIER_INTERP)) << "the two exact tiers disagree";
	}
}

// ---------------------------------------------------------------------------
// The fast path is expected to be wrong on four rows and right on the other 82,
// and both directions are asserted. It is not a tripwire to be graduated by
// editing the flags below -- the header says why it clamps.
// ---------------------------------------------------------------------------
TEST(EeFpuCompareConsole, FastPathIsWrongInTheseExactWays)
{
	int diverged = 0;
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(::testing::Message() << "[fpm] case " << c.ordinal << " "
			<< Name(c.cond) << " -- " << c.what);
		const u32 fast = RunOne(c, TIER_FAST);
		if (c.fast_diverges)
		{
			EXPECT_NE(fast, c.want_fcr31)
				<< "the fast path now agrees with the console here; if its "
				   "compare was deliberately made exact, move this row's flag "
				   "and say so in the commit";
			// Wrong in the way the clamp produces: both operands folded to
			// the same word, so C says "equal".
			EXPECT_EQ(fast, (c.cond == C_EQ) ? 0x01800001u : 0x01000001u);
			++diverged;
		}
		else
		{
			EXPECT_EQ(fast, c.want_fcr31) << "the fast path regressed on a row "
				"it used to get right";
		}
	}
	EXPECT_EQ(diverged, 4);
}
