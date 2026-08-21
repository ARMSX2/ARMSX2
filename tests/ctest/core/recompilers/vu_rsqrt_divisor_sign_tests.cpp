// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The div unit's flag and quotient-sign rules on hand-picked witnesses, across
// `_vuRSQRT`/`_vuSQRT`, `recCOP2_VRSQRT`/`recCOP2_VSQRT` and
// `mVU_RSQRT`/`mVU_SQRT`. VuDivUnitConsole scores the same rules over a whole
// operand grid; this is the readable form, and it names which engine had each
// case wrong.
//
// Q is the console's word. microVU still saturates a binade low of it, which
// MicroQ() below states once and VuDivUnitConsole scores in full; the macro
// recompiler reaches the console's ceiling and is asserted on it directly.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "VU.h"

#include <vector>

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee;
using namespace vu;

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp WaitQPair() { return VuOp{VWAITQ_L(), VNOP_U()}; }

constexpr u32 kFs = 4, kFt = 5;
constexpr u32 kRStatus = 8, kRQ = 9;

// Cause D|I (0x30) plus the sticky pair they set (0xC00). Everything outside
// this mask is the ZSUO cause, which no div-unit op touches.
constexpr u32 kDiMask = 0xC30u;
constexpr u32 kI = 0x410u; // cause I + sticky I
constexpr u32 kD = 0x820u; // cause D + sticky D

struct Row
{
	const char* what;
	u32 fs;
	u32 ft;
	u32 di; // expected STATUS & kDiMask
	u32 q;  // the console's
};

// A saturated quotient reads back as FLT_MAX from microVU -- its ceiling is the
// clamp constant mVUglob.maxvals, not the EE range's. Nothing else moves, so the
// rows below stay the console's and this is the only place the deficit is
// spelled.
constexpr bool Saturated(u32 q) { return (q & 0x7FFFFFFFu) == 0x7FFFFFFFu; }
constexpr u32 MicroQ(u32 q) { return Saturated(q) ? ((q & 0x80000000u) | 0x7F7FFFFFu) : q; }
constexpr const char* kWhyCeiling = "micro Q: microVU's ceiling is FLT_MAX, the interpreter's is 0x7FFFFFFF";

// Every row is checked on all four engines. 0/0 goes to its own test below.
constexpr Row kRows[] = {
	// Under xor both quotients would come back with the opposite sign, so this
	// pair alone refutes it.
	{"+1 / -0",      0x3F800000u, 0x80000000u, kI | kD, 0x7FFFFFFFu},
	{"-1 / -0",      0xBF800000u, 0x80000000u, kI | kD, 0xFFFFFFFFu},

	// Positive divisor, D alone: says the pair above moved for the sign bit
	// and not because the whole zero branch changed.
	{"+1 / +0",      0x3F800000u, 0x00000000u, kD,      0x7FFFFFFFu},
	{"-1 / +0",      0xBF800000u, 0x00000000u, kD,      0xFFFFFFFFu},

	// Zero exponent, nonzero mantissa: the VU has no denormals, so this
	// reaches the zero branch too.
	{"+1 / -denorm", 0x3F800000u, 0x80000001u, kI | kD, 0x7FFFFFFFu},

	// Nonzero divisors: the sign bit still decides I, and nothing raises D.
	{"+1 / -4",      0x3F800000u, 0xC0800000u, kI,      0x3F000000u},
	{"-1 / -4",      0xBF800000u, 0xC0800000u, kI,      0xBF000000u},
	{"+1 / +4",      0x3F800000u, 0x40800000u, 0,       0x3F000000u},
};

void BuildMacro(EeRecTestHarness& h, u32 fs, u32 ft)
{
	h.EnableVu0Capture();
	h.SeedVu0VfBits(kFs, fs, fs, fs, fs);
	h.SeedVu0VfBits(kFt, ft, ft, ft, ft);
	h.LoadProgram(std::vector<u32>{
		CTC2(0, REG_STATUS_FLAG), // clear the sticky field the prologue left
		VRSQRT_C2(0, 0, kFs, kFt),
		CFC2(kRStatus, REG_STATUS_FLAG),
		CFC2(kRQ, REG_Q),
	});
}

// The FDIV flag reaches STATUS 13 cycles after an RSQRT (mVUanalyzeFDIV), so
// the program has to run past that before the JIT side can be read.
void BuildMicro(VuTestHarness& h, u32 fs, u32 ft)
{
	h.SetVfBits(kFs, fs, fs, fs, fs);
	h.SetVfBits(kFt, ft, ft, ft, ft);
	std::vector<VuOp> prog;
	prog.push_back(LowerOnly(VRSQRT_L(kFs, 0, kFt, 0)));
	for (int i = 0; i < 16; ++i)
		prog.push_back(NopPair());
	prog.push_back(WaitQPair());
	prog.push_back(EBitNopPair());
	h.LoadProgram(prog);
}
} // namespace

// "+1 / -0" and "+1 / -denorm" were red on all three engines; the rest hold
// the surrounding behaviour still.
TEST(VuRsqrtDivisorSign, InvalidComesFromTheDivisorSignBitAlone)
{
	for (const Row& r : kRows)
	{
		SCOPED_TRACE(r.what);

		EeRecTestHarness hj;
		BuildMacro(hj, r.fs, r.ft);
		hj.RunJitNoDiff();
		EXPECT_EQ(hj.GetGprJit(kRStatus) & kDiMask, r.di) << "[macro jit] STATUS";

		EeRecTestHarness hi;
		BuildMacro(hi, r.fs, r.ft);
		hi.RunInterpOnly();
		EXPECT_EQ(hi.GetGprInterp(kRStatus) & kDiMask, r.di) << "[macro interp] STATUS";

		VuTestHarness m(0);
		BuildMicro(m, r.fs, r.ft);
		// also diffs micro JIT against micro interp
		if (Saturated(r.q))
			m.RunRequiringDivergence(kWhyCeiling);
		else
			m.Run();
		EXPECT_EQ(m.GetViJit(REG_STATUS_FLAG) & kDiMask, r.di) << "[micro jit] STATUS";
		EXPECT_EQ(m.GetViInterp(REG_STATUS_FLAG) & kDiMask, r.di) << "[micro interp] STATUS";
	}
}

// Asserted as the whole word so the magnitude is held still too, then as the
// sign alone, which is the half the console can arbitrate.
TEST(VuRsqrtDivisorSign, QuotientSignIsTheDividendsAlone)
{
	for (const Row& r : kRows)
	{
		SCOPED_TRACE(r.what);

		EeRecTestHarness hj;
		BuildMacro(hj, r.fs, r.ft);
		hj.RunJitNoDiff();
		EXPECT_EQ(hj.GetGprJit(kRQ), r.q) << "[macro jit] Q";

		EeRecTestHarness hi;
		BuildMacro(hi, r.fs, r.ft);
		hi.RunInterpOnly();
		EXPECT_EQ(hi.GetGprInterp(kRQ), r.q) << "[macro interp] Q";

		VuTestHarness m(0);
		BuildMicro(m, r.fs, r.ft);
		if (Saturated(r.q))
			m.RunRequiringDivergence(kWhyCeiling);
		else
			m.Run();
		EXPECT_EQ(m.GetViJit(REG_Q), MicroQ(r.q)) << "[micro jit] Q";
		EXPECT_EQ(m.GetViInterp(REG_Q), r.q) << "[micro interp] Q";

		EXPECT_EQ(r.q & 0x80000000u, r.fs & 0x80000000u) << "quotient sign is the dividend's";
	}
}

// The row autocases_vusticky.h already held before the div-unit grid existed:
// cause and sticky D|I together, and a positive quotient over a negative zero.
TEST(VuRsqrtDivisorSign, MatchesTheConsoleRowForOneOverNegativeZero)
{
	EeRecTestHarness h;
	BuildMacro(h, 0x3F800000u, 0x80000000u);
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGprJit(kRStatus) & kDiMask, 0xC30u);
	EXPECT_EQ(h.GetGprJit(kRQ) & 0x80000000u, 0x00000000u);
	EXPECT_EQ(h.GetGprJit(kRQ), 0x7FFFFFFFu);
}

// Three of the four engines used to raise both causes here and return ±0.
TEST(VuRsqrtDivisorSign, ZeroOverZeroRaisesInvalidWithoutDivideByZero)
{
	struct { const char* what; u32 fs; u32 ft; u32 q; } rows[] = {
		{"+0 / +0", 0x00000000u, 0x00000000u, 0x7FFFFFFFu},
		{"+0 / -0", 0x00000000u, 0x80000000u, 0x7FFFFFFFu},
		{"-0 / +0", 0x80000000u, 0x00000000u, 0xFFFFFFFFu},
		{"-0 / -0", 0x80000000u, 0x80000000u, 0xFFFFFFFFu},
	};

	for (const auto& r : rows)
	{
		SCOPED_TRACE(r.what);

		EeRecTestHarness hj;
		BuildMacro(hj, r.fs, r.ft);
		hj.RunJitNoDiff();
		EXPECT_EQ(hj.GetGprJit(kRStatus) & kDiMask, kI) << "[macro jit] STATUS";
		EXPECT_EQ(hj.GetGprJit(kRQ), r.q) << "[macro jit] Q";

		EeRecTestHarness hi;
		BuildMacro(hi, r.fs, r.ft);
		hi.RunInterpOnly();
		EXPECT_EQ(hi.GetGprInterp(kRStatus) & kDiMask, kI) << "[macro interp] STATUS";
		EXPECT_EQ(hi.GetGprInterp(kRQ), r.q) << "[macro interp] Q";

		VuTestHarness m(0);
		BuildMicro(m, r.fs, r.ft);
		m.RunRequiringDivergence(kWhyCeiling);
		EXPECT_EQ(m.GetViJit(REG_STATUS_FLAG) & kDiMask, kI) << "[micro jit] STATUS";
		EXPECT_EQ(m.GetViJit(REG_Q), MicroQ(r.q)) << "[micro jit] Q";
		EXPECT_EQ(m.GetViInterp(REG_STATUS_FLAG) & kDiMask, kI) << "[micro interp] STATUS";
		EXPECT_EQ(m.GetViInterp(REG_Q), r.q) << "[micro interp] Q";
	}
}

// VSQRT shared the defect: only mVU_SQRT tested the sign bit, so the other
// three lost I on -0 and on the denormals vuDouble flushes to it.
TEST(VuRsqrtDivisorSign, SqrtInvalidComesFromTheSignBitToo)
{
	struct { const char* what; u32 ft; u32 di; } rows[] = {
		{"sqrt -0",           0x80000000u, kI},
		{"sqrt -denorm.min",  0x80000001u, kI},
		{"sqrt -denorm.max",  0x807FFFFFu, kI},
		{"sqrt +0",           0x00000000u, 0},
		{"sqrt +denorm.max",  0x007FFFFFu, 0},
		{"sqrt -4",           0xC0800000u, kI},
		{"sqrt +4",           0x40800000u, 0},
	};

	for (const auto& r : rows)
	{
		SCOPED_TRACE(r.what);

		EeRecTestHarness hj;
		hj.EnableVu0Capture();
		hj.SeedVu0VfBits(kFt, r.ft, r.ft, r.ft, r.ft);
		hj.LoadProgram(std::vector<u32>{
			CTC2(0, REG_STATUS_FLAG),
			VSQRT_C2(0, kFt),
			CFC2(kRStatus, REG_STATUS_FLAG),
		});
		hj.RunJitNoDiff();
		EXPECT_EQ(hj.GetGprJit(kRStatus) & kDiMask, r.di) << "[macro jit] STATUS";

		EeRecTestHarness hi;
		hi.EnableVu0Capture();
		hi.SeedVu0VfBits(kFt, r.ft, r.ft, r.ft, r.ft);
		hi.LoadProgram(std::vector<u32>{
			CTC2(0, REG_STATUS_FLAG),
			VSQRT_C2(0, kFt),
			CFC2(kRStatus, REG_STATUS_FLAG),
		});
		hi.RunInterpOnly();
		EXPECT_EQ(hi.GetGprInterp(kRStatus) & kDiMask, r.di) << "[macro interp] STATUS";

		VuTestHarness m(0);
		m.SetVfBits(kFt, r.ft, r.ft, r.ft, r.ft);
		std::vector<VuOp> prog;
		prog.push_back(LowerOnly(VSQRT_L(kFt, 0)));
		for (int i = 0; i < 16; ++i)
			prog.push_back(NopPair());
		prog.push_back(WaitQPair());
		prog.push_back(EBitNopPair());
		m.LoadProgram(prog);
		m.Run();
		EXPECT_EQ(m.GetViJit(REG_STATUS_FLAG) & kDiMask, r.di) << "[micro jit] STATUS";
		EXPECT_EQ(m.GetViInterp(REG_STATUS_FLAG) & kDiMask, r.di) << "[micro interp] STATUS";
	}
}
} // namespace recompiler_tests

