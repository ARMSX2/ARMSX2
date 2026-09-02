// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU div unit against a first-party console capture: VDIV, VSQRT and
// VRSQRT over zero, denormal, normal, exponent-255 and saturated operands,
// scoring STATUS and Q. 502 cases, table in autocases_vurs.h.
//
// The capture was taken in VU0 macro mode, where VU0's registers are
// EE-readable, so the macro engines are scored against it directly and the
// micro engines are scored on the D/I cause and sticky pair, which is the div
// unit's own output rather than the mode's bookkeeping.
//
// What it settles, and what it does not:
//
//   STATUS. One rule fits all 502 rows. I comes from the operand's SIGN BIT
//   for the two ops that contain a square root -- exponent field ignored, so
//   -0 and the negative denormals raise it -- decided ahead of the zero test
//   and independently of it. D comes from a zero divisor with a nonzero
//   dividend; a zero dividend over a zero divisor raises I instead, and the
//   two are exclusive. So VRSQRT over -0 is the one operand class where both
//   causes stand together: the root's I and the division's D.
//
//   VDIV is the control that says the sign clause belongs to the square root
//   and not to the unit: a negative divisor raises nothing there. It is also
//   where the two ops' quotient signs part company -- VDIV's saturated
//   quotient takes the xor of the operand signs, VRSQRT's takes the dividend's
//   alone, because its divisor is a square root and never negative.
//
//   Q. Settled on the interpreters, which read the EE's divide-unit model and
//   match every row. The recompilers run a host divide, so their two gaps --
//   the ceiling and the arithmetic -- are pinned as exact tallies instead, for
//   whoever emits the model there to move. microVU's ceiling closes at
//   vuClampMode 3 and the macro path's at 4; the arithmetic closes at 4 on
//   both.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "VU.h"

#include <vector>

#include "autocases_vurs.h"

using namespace console_vurs;

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee;

constexpr u32 kFs = 4, kFt = 5;
constexpr u32 kRSeed = 20, kRStatus = 8, kRQ = 9;

// STATUS bits the div unit owns: the D/I cause pair and the two stickies they
// set. The ZSUO cause is the FMAC's and is zero throughout the capture.
constexpr u32 kDiMask = 0xC30u;

u32 MacroOp(const VursCase& c)
{
	switch (c.op)
	{
		case VURS_DIV:  return VDIV_C2(0, 0, kFs, kFt);
		case VURS_SQRT: return VSQRT_C2(0, kFt);
		default:        return VRSQRT_C2(0, 0, kFs, kFt);
	}
}

void BuildMacro(EeRecTestHarness& h, const VursCase& c)
{
	h.EnableVu0Capture();
	h.SeedVu0VfBits(kFs, c.fs, c.fs, c.fs, c.fs);
	h.SeedVu0VfBits(kFt, c.ft, c.ft, c.ft, c.ft);
	h.LoadProgram(std::vector<u32>{
		ORI(kRSeed, 0, c.seed),
		CTC2(kRSeed, REG_STATUS_FLAG),
		MacroOp(c),
		CFC2(kRStatus, REG_STATUS_FLAG),
		CFC2(kRQ, REG_Q),
	});
}

u32 MicroOp(const VursCase& c)
{
	switch (c.op)
	{
		case VURS_DIV:  return vu::VDIV_L(kFs, 0, kFt, 0);
		case VURS_SQRT: return vu::VSQRT_L(kFt, 0);
		default:        return vu::VRSQRT_L(kFs, 0, kFt, 0);
	}
}

// The div unit's flags reach STATUS up to 13 cycles downstream in micro mode
// (mVUanalyzeFDIV), so the program has to outrun that before either side can
// be read. Micro mode has no CTC2, so only the unseeded rows run here.
void BuildMicro(VuTestHarness& h, const VursCase& c)
{
	h.SetVfBits(kFs, c.fs, c.fs, c.fs, c.fs);
	h.SetVfBits(kFt, c.ft, c.ft, c.ft, c.ft);
	std::vector<vu::VuOp> prog;
	prog.push_back(vu::VuOp{MicroOp(c), vu::VNOP_U()});
	for (int i = 0; i < 16; ++i)
		prog.push_back(vu::NopPair());
	prog.push_back(vu::VuOp{vu::VWAITQ_L(), vu::VNOP_U()});
	prog.push_back(vu::EBitNopPair());
	h.LoadProgram(prog);
}

// Where an engine's Q lands relative to the console's.
struct QTally
{
	int ok = 0;   // the console's word
	int sat = 0;  // the console saturated and the engine saturated a binade low
	int unit = 0; // everything else: the divide unit's arithmetic
};

void ScoreQ(QTally& t, const VursCase& c, u32 got)
{
	if (got == c.q)
		t.ok++;
	else if ((c.q & 0x7FFFFFFFu) == 0x7FFFFFFFu && got == ((c.q & 0x80000000u) | 0x7F7FFFFFu))
		t.sat++;
	else
		t.unit++;
}

struct DivUnitScore
{
	QTally macro;
	QTally micro;
	int consoleSaturated = 0;
};

// Both recompilers' Q over the whole capture. A clamp mode of 0 leaves each
// harness on its own default, which is what the tallies below were taken at.
DivUnitScore ScoreDivUnit(int clamp_mode)
{
	DivUnitScore s;
	for (const VursCase& c : kVursCases)
	{
		if ((c.q & 0x7FFFFFFFu) == 0x7FFFFFFFu)
			++s.consoleSaturated;

		EeRecTestHarness hj;
		if (clamp_mode != 0)
			hj.SetVu0ClampMode(clamp_mode);
		BuildMacro(hj, c);
		hj.RunJitNoDiff();
		ScoreQ(s.macro, c, hj.GetGprJit(kRQ));

		if (c.seed != 0)
			continue;
		VuTestHarness m(0);
		if (clamp_mode != 0)
			m.SetVuClampMode(clamp_mode);
		m.IgnoreViInDiff(REG_STATUS_FLAG);
		m.IgnoreViInDiff(REG_Q);
		BuildMicro(m, c);
		m.Run();
		ScoreQ(s.micro, c, m.GetViJit(REG_Q));
	}
	return s;
}
} // namespace

TEST(VuDivUnitConsole, MacroStatusMatchesConsoleOnEveryRow)
{
	int checked = 0;
	for (const VursCase& c : kVursCases)
	{
		SCOPED_TRACE(c.tag);

		EeRecTestHarness hj;
		BuildMacro(hj, c);
		hj.RunJitNoDiff();
		EXPECT_EQ(hj.GetGprJit(kRStatus) & 0xFFFu, c.status) << "[macro jit] STATUS";

		EeRecTestHarness hi;
		BuildMacro(hi, c);
		hi.RunInterpOnly();
		EXPECT_EQ(hi.GetGprInterp(kRStatus) & 0xFFFu, c.status) << "[macro interp] STATUS";
		++checked;
	}
	EXPECT_EQ(checked, static_cast<int>(std::size(kVursCases)));
}

TEST(VuDivUnitConsole, MicroCauseAndStickyMatchConsole)
{
	int checked = 0;
	for (const VursCase& c : kVursCases)
	{
		if (c.seed != 0)
			continue;
		SCOPED_TRACE(c.tag);

		VuTestHarness m(0);
		m.IgnoreViInDiff(REG_Q); // scored separately, and it diverges by class
		BuildMicro(m, c);
		m.Run(); // also diffs micro JIT against micro interp
		EXPECT_EQ(m.GetViJit(REG_STATUS_FLAG) & kDiMask, c.status & kDiMask)
			<< "[micro jit] STATUS D/I";
		EXPECT_EQ(m.GetViInterp(REG_STATUS_FLAG) & kDiMask, c.status & kDiMask)
			<< "[micro interp] STATUS D/I";
		++checked;
	}
	EXPECT_EQ(checked, 422);
}

TEST(VuDivUnitConsole, InterpreterQMatchesConsoleOnEveryRow)
{
	int macro = 0, micro = 0;
	for (const VursCase& c : kVursCases)
	{
		SCOPED_TRACE(c.tag);

		EeRecTestHarness hi;
		BuildMacro(hi, c);
		hi.RunInterpOnly();
		EXPECT_EQ(hi.GetGprInterp(kRQ), c.q) << "[macro interp] Q";
		++macro;

		if (c.seed != 0)
			continue;
		VuTestHarness m(0);
		m.IgnoreViInDiff(REG_STATUS_FLAG);
		m.IgnoreViInDiff(REG_Q); // the JIT column diverges by class, scored below
		BuildMicro(m, c);
		m.Run();
		EXPECT_EQ(m.GetViInterp(REG_Q), c.q) << "[micro interp] Q";
		++micro;
	}
	EXPECT_EQ(macro, static_cast<int>(std::size(kVursCases)));
	EXPECT_EQ(micro, 422);
}

// The console's saturated quotient is the EE maximum 0x7FFFFFFF. At the harness
// default neither recompiler ceiling reaches it, the zero-divisor branch's
// included, so `sat` is nearly every saturated row. The sign is not part of the
// difference, so the class is defined with the sign carried over -- which is
// what makes it a statement about the clamp and not a place for a sign bug to
// hide. What is left over is the host divide's arithmetic.
TEST(VuDivUnitConsole, OnlyDividedQuotientsSaturateABinadeLowOfTheConsole)
{
	const DivUnitScore s = ScoreDivUnit(0);
	const QTally& mj = s.macro;
	const QTally& uj = s.micro;

	EXPECT_EQ(s.consoleSaturated, 226);

	// Rows that saturate on the console and come back as something other than
	// the sign-matched ceiling fall in `unit` below rather than being counted as
	// clamp misses.
	EXPECT_EQ(mj.sat, 224);
	EXPECT_EQ(uj.sat, 176);

	// The arithmetic gap left once the ceiling is accounted for.
	EXPECT_EQ(mj.unit, 86);
	EXPECT_EQ(uj.unit, 82);

	EXPECT_EQ(mj.ok, 192);
	EXPECT_EQ(uj.ok, 164);

	EXPECT_EQ(mj.ok + mj.sat + mj.unit, static_cast<int>(std::size(kVursCases)));
	EXPECT_EQ(uj.ok + uj.sat + uj.unit, 422);
}

// Where the two gaps above go. vuClampMode 4 reads the divide unit's own
// arithmetic out of line instead of the host's Fdiv and Fsqrt, and both halves
// close together: `unit` because the recurrence is what silicon runs, and `sat`
// because the model's ceiling is the VU's 0x7FFFFFFF rather than the FLT_MAX
// the clamp it replaces holds. Modes 1 to 3 are asserted alongside so the
// counts are pinned as a step and not as a single reading -- an emitter that
// took the model unconditionally would pass the mode-4 half on its own.
TEST(VuDivUnitConsole, TheDivideUnitsArithmeticLandsAtClampModeFour)
{
	for (int mode = 1; mode <= 4; ++mode)
	{
		SCOPED_TRACE(::testing::Message() << "vuClampMode " << mode);
		const DivUnitScore s = ScoreDivUnit(mode);

		EXPECT_EQ(s.macro.ok + s.macro.sat + s.macro.unit, static_cast<int>(std::size(kVursCases)));
		EXPECT_EQ(s.micro.ok + s.micro.sat + s.micro.unit, 422);

		if (mode < 4)
		{
			// microVU's zero-divisor branch reaches the console word from mode 3,
			// where mVUclamp2 bounds it back to a number sign-preservingly; the
			// macro path's q-forms have no operand clamp, so its ceiling waits
			// for the model.
			EXPECT_EQ(s.macro.sat, 224);
			EXPECT_EQ(s.micro.sat, mode < 3 ? 176 : 14);
			EXPECT_EQ(s.macro.unit, 86);
			EXPECT_EQ(s.micro.unit, 82);
			continue;
		}

		EXPECT_EQ(s.macro.sat, 0);
		EXPECT_EQ(s.micro.sat, 0);
		EXPECT_EQ(s.macro.unit, 0);
		EXPECT_EQ(s.micro.unit, 0);
		EXPECT_EQ(s.macro.ok, static_cast<int>(std::size(kVursCases)));
		EXPECT_EQ(s.micro.ok, 422);
	}
}

// The two ops' saturated quotients take their sign by different rules, and the
// capture pins both. VDIV xors the operand signs; VRSQRT cannot, because its
// divisor is a square root. Stated on the smallest witnesses because the
// tallies above would still pass if the two rules were swapped.
TEST(VuDivUnitConsole, SaturatedQuotientSignsDifferBetweenDivAndRsqrt)
{
	int div = 0, rsqrt = 0;
	for (const VursCase& c : kVursCases)
	{
		if ((c.q & 0x7FFFFFFFu) != 0x7FFFFFFFu || (c.ft & 0x7F800000u) != 0)
			continue;
		const u32 sign = c.q & 0x80000000u;
		if (c.op == VURS_DIV)
		{
			EXPECT_EQ(sign, (c.fs ^ c.ft) & 0x80000000u) << c.tag;
			++div;
		}
		else if (c.op == VURS_RSQRT)
		{
			EXPECT_EQ(sign, c.fs & 0x80000000u) << c.tag;
			++rsqrt;
		}
	}
	// 80 VDIV rows: the 8 zero-exponent divisors over all 10 dividends. 130
	// VRSQRT rows: the same 80, the seeded pass's 8 x 6, and the two rig-check
	// repeats of the row the earlier capture already held.
	EXPECT_EQ(div, 80);
	EXPECT_EQ(rsqrt, 130);
}
} // namespace recompiler_tests
