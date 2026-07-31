// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The EE's top binade, on the interpreter, against the console.
//
// Exponent 255 is an ordinary exponent on this FPU. There is no Inf and no NaN:
// 0x7F800000 is 2^128, 0x7FC00000 is 1.5 * 2^128, and 0x7FFFFFFF is the largest
// number the machine has. That is one binade above what an IEEE single can
// represent, so every one of those words is a host infinity or a host NaN, and
// arithmetic done in host singles cannot carry them.
//
// The interpreter used to do exactly that: fpuDouble() folded an exponent-255
// operand down to +-0x7F7FFFFF on the way in and clampToEeRange() folded a host
// infinity back to the same word on the way out. Over the 1147-case SCPH-90000
// capture, 368 cases touch the top binade and the interpreter got 113 of them;
// that was 79% of the engine's deficit. It now reads operands through
// eeToDouble() and rounds once through eeRoundToSingle().
//
// The fast path has not moved and must not: it computes in host singles, so it
// cannot hold these values at all, and its +-FLT_MAX is what shipping games are
// tuned against. Where the tiers sit:
//
//     interpreter   exact          (this file)
//     fpuFullMode   exact          (ee_rec_fpu_full_mode_tests.cpp)
//     fast path     +-FLT_MAX      (EeFpuOverflowConsole, deliberately)
//
// Every expected value below is from the capture, with its case ordinal.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include <gtest/gtest.h>

#include <cstdio>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFd = 4, kFs = 5, kFt = 6;

enum Form { F_ADD, F_SUB, F_MUL, F_ADDA, F_SUBA, F_MULA,
            F_MADD, F_MSUB, F_MADDA, F_MSUBA };

bool WritesAcc(Form f)
{
	return f == F_ADDA || f == F_SUBA || f == F_MULA || f == F_MADDA || f == F_MSUBA;
}

u32 Encode(Form f)
{
	switch (f)
	{
		case F_ADD:   return ADD_S(kFd, kFs, kFt);
		case F_SUB:   return SUB_S(kFd, kFs, kFt);
		case F_MUL:   return MUL_S(kFd, kFs, kFt);
		case F_ADDA:  return ADDA_S(kFs, kFt);
		case F_SUBA:  return SUBA_S(kFs, kFt);
		case F_MULA:  return MULA_S(kFs, kFt);
		case F_MADD:  return MADD_S(kFd, kFs, kFt);
		case F_MSUB:  return MSUB_S(kFd, kFs, kFt);
		case F_MADDA: return MADDA_S(kFs, kFt);
		default:      return MSUBA_S(kFs, kFt);
	}
}

const char* Name(Form f)
{
	switch (f)
	{
		case F_ADD:   return "add.s";
		case F_SUB:   return "sub.s";
		case F_MUL:   return "mul.s";
		case F_ADDA:  return "adda.s";
		case F_SUBA:  return "suba.s";
		case F_MULA:  return "mula.s";
		case F_MADD:  return "madd.s";
		case F_MSUB:  return "msub.s";
		case F_MADDA: return "madda.s";
		default:      return "msuba.s";
	}
}

enum Tier { TIER_INTERP, TIER_FAST, TIER_FULL };

u32 RunOne(Form f, u32 fs, u32 ft, u32 acc, Tier tier)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (tier == TIER_FULL)
		h.EnableFpuFullMode();
	h.SetAccBits(acc);
	h.SetFprBits(kFs, fs);
	h.SetFprBits(kFt, ft);
	h.LoadProgram({Encode(f)});
	if (tier == TIER_INTERP)
		h.RunInterpOnly();
	else
		h.RunJitNoDiff();

	const bool jit = tier != TIER_INTERP;
	if (WritesAcc(f))
		return jit ? h.GetAccBitsJit() : h.GetAccBitsInterp();
	return jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd);
}

struct ConsoleCase
{
	int ordinal;
	Form form;
	u32 fs, ft, acc;
	u32 want;   // the console
	u32 fast;   // what the fast path produces -- measured, see DISABLED_DumpTiers
	const char* what;
};

// Chosen to cover the three things the old model got wrong: an exponent-255
// operand that must survive, an exact result in the top binade that must be
// reachable, and a result past the maximum that must saturate at 0x7FFFFFFF
// rather than at FLT_MAX.
constexpr ConsoleCase kConsole[] = {
	// --- operand in the top binade, result is the operand unchanged ---
	{  16, F_ADD,  0x7F800000u, 0x00000000u, 0u, 0x7F800000u, 0x7F7FFFFFu, "2^128 + 0 is 2^128"},
	{  95, F_ADD,  0x7F800000u, 0x40000000u, 0u, 0x7F800000u, 0x7F7FFFFFu, "2^128 + 2.0 is 2^128"},
	{  98, F_ADD,  0x7FC00000u, 0x3F800000u, 0u, 0x7FC00000u, 0x7F7FFFFFu, "1.5*2^128 + 1.0 unchanged"},
	{  92, F_ADD,  0x7FFFFFFFu, 0x3F800000u, 0u, 0x7FFFFFFFu, 0x7F7FFFFFu, "EEMAX + 1.0 is EEMAX"},
	{ 132, F_SUB,  0x7F800000u, 0x40000000u, 0u, 0x7F800000u, 0x7F7FFFFFu, "2^128 - 2.0 is 2^128"},
	{ 487, F_ADDA, 0x7F800000u, 0x40000000u, 0u, 0x7F800000u, 0x7F7FFFFFu, "the A-form too"},

	// --- exponent-255 operand that must reach the multiplier intact ---
	{ 609, F_MADD, 0x7FC00000u, 0x3EAAAAABu, 0x3F800000u, 0x7F000000u, 0x7F7FFFFFu,
	  "1.5*2^128 * 1/3 is 2^127 -- the old clamp landed a binade low"},
	{ 607, F_MADD, 0x7FFFFFFFu, 0x00800000u, 0x3F800000u, 0x410FFFFFu, 0x7F7FFFFFu,
	  "EEMAX * 2^-126 + 1.0 -- the clamp cost three whole bits of the result"},

	// --- cancellation that only works with the real operands ---
	{  17, F_SUB,  0x7F800000u, 0x7F800000u, 0u, 0x00000000u, 0x7F7FFFFFu, "2^128 - 2^128 is 0"},
	{ 136, F_SUB,  0x7FA00000u, 0x7FC00000u, 0u, 0xFE800000u, 0x7F7FFFFFu,
	  "1.25*2^128 - 1.5*2^128 is -2^126, not the 0 two clamped operands give"},

	// --- exact results IN the top binade ---
	{  18, F_ADD,  0x7F7FFFFFu, 0x7F7FFFFFu, 0u, 0x7FFFFFFFu, 0x7F7FFFFFu,
	  "FLT_MAX + FLT_MAX is EEMAX EXACTLY -- not an overflow at all"},
	{   9, F_ADD,  0xFF7FFFFFu, 0xFF7FFFFFu, 0u, 0xFFFFFFFFu, 0xFF7FFFFFu, "and negated"},
	{  19, F_SUB,  0x7F7FFFFFu, 0xFF7FFFFFu, 0u, 0x7FFFFFFFu, 0x7F7FFFFFu, "by subtraction"},
	{  20, F_ADDA, 0x7F7FFFFFu, 0x7F7FFFFFu, 0u, 0x7FFFFFFFu, 0x7F7FFFFFu, "into the accumulator"},

	// --- past the maximum: saturate at EEMAX, not FLT_MAX ---
	{  94, F_ADD,  0x7F800000u, 0x7F800000u, 0u, 0x7FFFFFFFu, 0x7F7FFFFFu, "2^129 saturates"},
	{ 100, F_ADD,  0x7FC00000u, 0x7FC00000u, 0u, 0x7FFFFFFFu, 0x7F7FFFFFu, "3*2^128 saturates"},
	{  15, F_ADD,  0x7FFFFFFFu, 0x7FFFFFFFu, 0u, 0x7FFFFFFFu, 0x7F7FFFFFu, "EEMAX doubled"},

	// --- an overflowing product ends the instruction ---
	{ 521, F_MADD, 0x7F7FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7FFFFFFFu, 0x00000000u,
	  "the accumulate does not get to bring an overflowed product back"},
	{ 535, F_MSUB, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0xFFFFFFFFu, 0x00000000u, "and negated"},
	{ 549, F_MADDA, 0x7F7FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7FFFFFFFu, 0x00000000u, "A-form"},
	{ 563, F_MSUBA, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0xFFFFFFFFu, 0x00000000u, "A-form"},
};
constexpr int kConsoleCount = static_cast<int>(sizeof(kConsole) / sizeof(kConsole[0]));

} // namespace

// ---------------------------------------------------------------------------
// The interpreter, which is what moved. Every one of these failed before the
// operand clamp came out of the arithmetic path.
// ---------------------------------------------------------------------------
TEST(EeFpuTopBinadeConsole, InterpMatchesConsoleOnEveryRow)
{
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(::testing::Message() << "[fpm] case " << c.ordinal << " "
			<< Name(c.form) << " -- " << c.what);
		EXPECT_EQ(RunOne(c.form, c.fs, c.ft, c.acc, TIER_INTERP), c.want);
	}
	EXPECT_EQ(kConsoleCount, 21) << "anti-vacuity: the console table emptied";
}

// ---------------------------------------------------------------------------
// The FULL double path was already exact here, and is the independent
// implementation this change was cross-checked against. Pinned so that the two
// exact tiers cannot drift apart silently -- if one of them regresses, this
// says which.
// ---------------------------------------------------------------------------
TEST(EeFpuTopBinadeConsole, FullModeMatchesTheInterpreterOnEveryRow)
{
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(::testing::Message() << "[fpm] case " << c.ordinal << " "
			<< Name(c.form) << " -- " << c.what);
		const u32 full = RunOne(c.form, c.fs, c.ft, c.acc, TIER_FULL);
		EXPECT_EQ(full, c.want) << "the FULL path stopped matching the console";
		EXPECT_EQ(full, RunOne(c.form, c.fs, c.ft, c.acc, TIER_INTERP))
			<< "the two exact tiers disagree";
	}
}

// ---------------------------------------------------------------------------
// The fast path is wrong here, and the `fast` column pins how wrong, row by
// row. It is not a tripwire to be graduated: making the fast path exact means
// giving it the double arithmetic fpuFullMode already charges for.
// ---------------------------------------------------------------------------
TEST(EeFpuTopBinadeConsole, FastPathIsWrongInTheseExactWays)
{
	int wrong = 0;
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(::testing::Message() << "[fpm] case " << c.ordinal << " "
			<< Name(c.form) << " -- " << c.what);
		EXPECT_EQ(RunOne(c.form, c.fs, c.ft, c.acc, TIER_FAST), c.fast)
			<< "the fast path's value moved. If it moved TOWARD the console, "
			   "that is game-visible behaviour and wants its own change and its "
			   "own justification -- not a quiet update here.";
		wrong += c.fast != c.want;
	}
	EXPECT_EQ(wrong, 21) << "the fast path now agrees with the console on more "
						    "(or fewer) of these rows than when the table was "
						    "measured; re-measure with DISABLED_DumpTiers "
						    "before changing this number";
}

// ---------------------------------------------------------------------------
// Prints all three tiers per row and asserts nothing; this is where the `fast`
// column above comes from. Run with
// --gtest_also_run_disabled_tests --gtest_filter=*DumpTiers*
// ---------------------------------------------------------------------------
TEST(EeFpuTopBinadeConsole, DISABLED_DumpTiers)
{
	std::printf("\n%-6s %-8s %-10s %-10s %-10s %-10s  %s\n",
		"case", "op", "console", "interp", "fast", "full", "note");
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		std::printf("%-6d %-8s 0x%08X 0x%08X 0x%08X 0x%08X  %s\n",
			c.ordinal, Name(c.form), c.want,
			RunOne(c.form, c.fs, c.ft, c.acc, TIER_INTERP),
			RunOne(c.form, c.fs, c.ft, c.acc, TIER_FAST),
			RunOne(c.form, c.fs, c.ft, c.acc, TIER_FULL), c.what);
	}
}

// ---------------------------------------------------------------------------
// The property behind the whole class, asserted directly rather than through
// the table: on the interpreter, exponent 255 is an ordinary exponent. Adding
// zero to any word in the top binade must return that word bit-for-bit, and
// multiplying it by 1.0 must too. Both are identities on the EE and neither
// survived the old operand clamp.
//
// The operands include patterns the capture does not contain: 0x7F800001 and
// 0x7FA00000 are host signalling NaNs, which a model leaning on host float
// semantics anywhere will mangle.
// ---------------------------------------------------------------------------
TEST(EeFpuTopBinadeConsole, AddingZeroAndMultiplyingByOneAreIdentitiesUpThere)
{
	constexpr u32 kOperands[] = {
		0x7F800000u, // 2^128
		0x7F800001u, // host sNaN
		0x7FA00000u, // host sNaN
		0x7FC00000u, // host qNaN
		0x7FFFFFFFu, // EEMAX
		0xFF800000u, 0xFF800001u, 0xFFC00000u, 0xFFFFFFFFu, // and negated
	};
	constexpr int kN = static_cast<int>(sizeof(kOperands) / sizeof(kOperands[0]));

	for (int i = 0; i < kN; ++i)
	{
		const u32 in = kOperands[i];
		SCOPED_TRACE(::testing::Message() << std::hex << "operand " << in);
		// x + 0 == x. The zero is +0 so it cannot supply the sign itself.
		EXPECT_EQ(RunOne(F_ADD, in, 0x00000000u, 0u, TIER_INTERP), in)
			<< "adding zero changed a top-binade word";
		// x * 1 == x.
		EXPECT_EQ(RunOne(F_MUL, in, 0x3F800000u, 0u, TIER_INTERP), in)
			<< "multiplying by one changed a top-binade word";
		// x - x == +0, by cancellation rather than by both operands clamping to
		// the same wrong number, which is how the old model reached it.
		EXPECT_EQ(RunOne(F_SUB, in, in, 0u, TIER_INTERP), 0x00000000u)
			<< "a top-binade word did not cancel with itself";
	}
	EXPECT_EQ(kN, 9) << "anti-vacuity";
}
