// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// How many guard bits the VU adder keeps, against real hardware.
//
// fpuGuardMask (FPU.cpp) clears the low (|diff| - 1) mantissa bits of the
// smaller-exponent operand before the add and erases it entirely at
// |diff| >= 25. That was measured on the EE, over 2.1M console rows, and
// VUops.cpp applies it to every VU FMAC add on the strength of that alone:
// of the 68 VU rows in autocases_vusat.h not one separates it. The only
// adder-underflow row there has a zero mantissa, and both chop rows give the
// same word masked or not. So the VU's largest arithmetic assumption was an
// EE property nothing had checked on the VU.
//
// Say it as "the adder keeps g guard bits below its 24-bit significand" and
// it becomes one number rather than 23 independent rows: the smaller
// operand's bit k lands at position -diff-24+k, so it survives iff
// k >= diff - g, and fpuGuardMask is g = 1. Which bit of the smaller operand
// is set is then the measurement, and a sweep over (separation, bit) picks g.
//
// The masked bits sit below half an ULP of the result, so they are invisible
// unless the add cancels. Every row here is an unlike-signed ADD or a
// like-signed SUB, where clearing them moves the exact sum across an ULP
// boundary and the chop reports a different word.
//
// The recompiler emits the mask at vuClampMode 4 only, so the columns here are
// scored at 4; UnmaskedRecompilerMissesTheseRows is the same table at every mode
// below it, where the bare host add puts each row this file collected back on
// the wrong word.
//
// The console says g = 1, over separations 2..24: the inherited rule is the
// VU's rule. Each row's `sep` column names the readings it tells apart, so a
// regenerated table that stopped discriminating is caught by
// TheTableSeparatesEveryGuardBitReading below rather than passing silently.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <set>
#include <string>

#include "autocases_vgmask.h"

using namespace console_vgmask;

namespace recompiler_tests
{
using namespace mips;
using namespace mips::ee;

namespace {

// What the recompiler's column comes to below vuClampMode 4, where no mask is
// emitted: the score autocases_vgmask.h carried before the emitter existed.
constexpr int kVgMaskUnmaskedBadJit = 455;

// The console probe's register assignment, kept so the emitted op is the one
// that was measured.
constexpr u32 kFs = 1, kFt = 2, kFd = 4;
constexpr u32 kMaskXyzw = 0xF;

u32 Encode(const VgMaskCase& c)
{
	switch (c.op)
	{
		case VG_ADD: return VADD_C2(kMaskXyzw, kFd, kFs, kFt);
		case VG_SUB: return VSUB_C2(kMaskXyzw, kFd, kFs, kFt);
		default:     return 0;
	}
}

struct Observed
{
	u32 out;
	u32 mac;
	u32 stat;
};

Observed RunCase(const VgMaskCase& c, u32 word, bool jit, int clamp_mode)
{
	EeRecTestHarness h;
	h.SetVu0ClampMode(clamp_mode);
	h.EnableVu0Capture();
	// Each engine is scored against the console on its own: below clamp_mode 4
	// the recompiler emits a bare four-lane add, so Run()'s JIT-vs-interp
	// auto-diff would fail on every row this file exists to record.
	h.ExpectVu0Divergence();
	h.EnableCop1();
	h.SeedVu0VfBits(kFs, c.fs, c.fs, c.fs, c.fs);
	h.SeedVu0VfBits(kFt, c.ft, c.ft, c.ft, c.ft);
	h.SeedVu0VfBits(kFd, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu);
	h.LoadProgram({word});
	h.Run();

	Observed o{};
	o.out = jit ? h.GetVu0VfBitsJit(kFd, 'x') : h.GetVu0VfBitsInterp(kFd, 'x');
	o.mac = (jit ? h.GetVu0ViJit(REG_MAC_FLAG) : h.GetVu0ViInterp(REG_MAC_FLAG)) & 0xFFFFu;
	o.stat = (jit ? h.GetVu0ViJit(REG_STATUS_FLAG) : h.GetVu0ViInterp(REG_STATUS_FLAG)) & 0xFFFFu;
	return o;
}

// One bit per column, set where the engine disagrees with the console.
u8 Misses(const VgMaskCase& c, const Observed& o)
{
	u8 m = 0;
	if (o.out != c.out) m |= VGB_VALUE;
	if (o.mac != c.mac) m |= VGB_MAC;
	if (o.stat != c.stat) m |= VGB_STAT;
	return m;
}

const char* kColName[] = {"value", "mac", "stat"};
constexpr u8 kColBit[] = {VGB_VALUE, VGB_MAC, VGB_STAT};

int PopCount(u8 v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1); }

} // namespace

TEST(Vu0MacroGuardMaskConsole, GuardMaskMatchesConsole)
{
	int checked = 0, bad_interp = 0, bad_jit = 0;
	for (int i = 0; i < kVgMaskCaseCount; ++i)
	{
		const VgMaskCase& c = kVgMaskCases[i];
		const u32 word = Encode(c);
		ASSERT_NE(word, 0u) << "case " << i << ": no encoder for op " << int(c.op);

		for (int jit = 0; jit < 2; ++jit)
		{
			const u8 known = jit ? c.bad_jit : c.bad_interp;
			const u8 got = Misses(c, RunCase(c, word, jit != 0, 4));
			(jit ? bad_jit : bad_interp) += PopCount(known);

			for (int col = 0; col < 3; ++col)
			{
				SCOPED_TRACE(::testing::Message()
					<< "case " << i << " [" << (jit ? "jit" : "interp") << "] "
					<< kColName[col] << ": " << c.what);
				if (known & kColBit[col])
				{
					EXPECT_TRUE(got & kColBit[col])
						<< "now MATCHES the console. Regenerate autocases_vgmask.h "
						   "from DISABLED_DumpConsoleComparison.";
				}
				else
				{
					EXPECT_FALSE(got & kColBit[col]) << "new divergence from silicon";
				}
			}
		}
		++checked;
	}
	EXPECT_EQ(checked, kVgMaskCaseCount);
	EXPECT_EQ(bad_interp, kVgMaskBadInterp);
	EXPECT_EQ(bad_jit, kVgMaskBadJit);
}

// The other side of the gate, and where it sits. Every row here was collected
// because the mask moves it, so at each mode below 4 the recompiler has to miss
// all of them -- and miss them on the value alone, since the mask reaches
// neither flag word. Scoring 1 to 3 rather than only 3 is what says the gate is
// on the topmost mode rather than somewhere below it.
TEST(Vu0MacroGuardMaskConsole, UnmaskedRecompilerMissesTheseRows)
{
	for (int mode = 1; mode <= 3; ++mode)
	{
		SCOPED_TRACE(::testing::Message() << "vuClampMode " << mode);
		int bad = 0, value = 0, rows_hit = 0;
		for (int i = 0; i < kVgMaskCaseCount; ++i)
		{
			const VgMaskCase& c = kVgMaskCases[i];
			const u32 word = Encode(c);
			ASSERT_NE(word, 0u) << "case " << i << ": no encoder for op " << int(c.op);
			const u8 m = Misses(c, RunCase(c, word, true, mode));
			bad += PopCount(m);
			value += (m & VGB_VALUE) != 0;
			rows_hit += m != 0;
		}
		std::printf("vuClampMode %d: %d column misses over %d rows (%d value, %d rows)\n",
			mode, bad, kVgMaskCaseCount, value, rows_hit);
		EXPECT_EQ(bad, kVgMaskUnmaskedBadJit);
		EXPECT_EQ(value, bad);
	}
}

// The table asserts values; this asserts that the values are a measurement.
// Every non-control row must tell at least two readings of g apart, and all
// four adjacent boundaries must be covered -- otherwise the rows could all be
// satisfied by a guard mask of any width and the file would pin nothing.
TEST(Vu0MacroGuardMaskConsole, TheTableSeparatesEveryGuardBitReading)
{
	std::set<std::string> seps;
	int controls = 0;
	for (int i = 0; i < kVgMaskCaseCount; ++i)
	{
		const std::string sep = kVgMaskCases[i].sep;
		if (sep == "control")
		{
			++controls;
			continue;
		}
		EXPECT_NE(sep.find('='), std::string::npos)
			<< "case " << i << " separates no two readings: " << sep;
		seps.insert(sep);
	}
	EXPECT_EQ(controls, 4);

	// The four adjacent boundaries, each as the split it produces.
	for (const char* boundary : {"g0=", "=g2g3gInf", "=g3gInf", "=gInf"})
	{
		bool found = false;
		for (const std::string& s : seps)
			found = found || s.find(boundary) != std::string::npos;
		EXPECT_TRUE(found) << "no row separates at " << boundary;
	}
}

// Regenerates the bad_interp / bad_jit masks. Run with
//   --gtest_also_run_disabled_tests --gtest_filter='*GuardMask*Dump*'
TEST(Vu0MacroGuardMaskConsole, DISABLED_DumpConsoleComparison)
{
	int bi = 0, bj = 0;
	std::printf("\n%-5s %-4s %-9s %-9s %-9s %-9s %-9s %s\n",
		"case", "op", "fs", "ft", "console", "interp", "jit", "sep");
	for (int i = 0; i < kVgMaskCaseCount; ++i)
	{
		const VgMaskCase& c = kVgMaskCases[i];
		const u32 word = Encode(c);
		const Observed oi = RunCase(c, word, false, 4);
		const Observed oj = RunCase(c, word, true, 4);
		const u8 mi = Misses(c, oi), mj = Misses(c, oj);
		bi += PopCount(mi);
		bj += PopCount(mj);
		std::printf("%-5d %-4s %08X  %08X  %08X  %08X  %08X  %s%s\n",
			i, c.op == VG_ADD ? "add" : "sub", c.fs, c.ft,
			c.out, oi.out, oj.out, c.sep,
			(mi || mj) ? "" : "");
		std::printf("      bad_interp %u bad_jit %u\n", mi, mj);
	}
	std::printf("\nkVgMaskBadInterp = %d\nkVgMaskBadJit = %d\n", bi, bj);
}

} // namespace recompiler_tests
