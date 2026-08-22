// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU FMAC below 2^-126, against real hardware.
//
// autocases_vuflow.h is generated from a capture taken on an SCPH-90000
// (captures/vuflow). The question is one captures/vusat could not answer: it
// has underflow rows, but every one of them lands on a k that is a power of
// two, where "flushed to a signed zero" and "the mantissa left where
// normalisation put it" are the same word. Zero separating rows is zero
// evidence, not agreement.
//
// The region decides itself. Every VU normal is an integer multiple of
// 2^(e-23) with e >= -126, so a sum or difference of two normals is an integer
// multiple of 2^-149, and a result below 2^-126 is exactly k*2^-149 with
// 1 <= k < 2^23 -- which makes
//
//     flush               0
//     the true value      k          (k IS the denormal single's mantissa)
//     mantissa in place   (k << (24 - bitlen(k))) & 0x7FFFFF
//
// three different words wherever k is not a power of two. Of the 51 rows here
// that separate all three, 48 say mantissa-in-place and 3 say flush, and the
// split is by opcode: every ADD, SUB, MADD and MSUB keeps the bits, every MUL
// clears them. None says the true value. That is the EE FPU's law exactly
// (captures/fpuflow, ee_fpu_underflow_console_tests.cpp), which is the third
// place the VU FMAC has turned out to be the EE's unit rather than a relative
// of it.
//
// Two consequences the emitters have to carry, beyond the word itself:
//
//   * MAC Z is set on rows whose result word is NOT zero (case 8 returns
//     0x00400000 with Z), so Z is an exponent-field test and not a test of the
//     word. An emitter that flushes gets Z right by accident.
//   * MAC U marks the underflow and is absent on an exact cancellation
//     (cases 68, 69) and on a result of exactly 2^-126 (case 70), so it is the
//     region and not the zero that raises it.
//
// Scoring is per engine and per column, like vu0_macro_fmac_range_console_tests
// and for the same reason: both engines are wrong here in overlapping ways, so
// a differential between them would report agreement on the rows this file
// exists to record.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>

#include "autocases_vuflow.h"

using namespace console_vuflow;

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;

namespace {

// The console probe's register assignment, kept so the emitted pair is the
// same one that was measured.
constexpr u32 kFs = 1, kFt = 2, kAcc = 3, kFd = 4, kZero = 7;
constexpr u32 kMaskXyzw = 0xF;

// The mode the exact models live at.
constexpr int kVuFlowScoredMode = 4;

u32 Encode(const VuFlowCase& c)
{
	switch (c.op)
	{
		case VF_ADD:  return VADD_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VF_SUB:  return VSUB_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VF_MUL:  return VMUL_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VF_MADD: return VMADD_C2(kMaskXyzw, kFd, kFs, kFt);
		case VF_MSUB: return VMSUB_C2(kMaskXyzw, kFd, kFs, kFt);
		default:      return 0;
	}
}

struct Observed
{
	u32 out;
	u32 mac;
	u32 stat;
};

// The ACC is seeded through VADDA rather than written directly because that is
// what the capture measured -- the seed's own flags are part of the status
// column.
Observed RunCase(const VuFlowCase& c, u32 word, bool jit)
{
	EeRecTestHarness h;
	h.SetVu0ClampMode(kVuFlowScoredMode);
	h.EnableVu0Capture();
	h.ExpectVu0Divergence();
	h.EnableCop1();
	h.SeedVu0VfBits(kFs, c.fs, c.fs, c.fs, c.fs);
	h.SeedVu0VfBits(kFt, c.ft, c.ft, c.ft, c.ft);
	h.SeedVu0VfBits(kAcc, c.acc, c.acc, c.acc, c.acc);
	h.SeedVu0VfBits(kZero, 0, 0, 0, 0);
	h.SeedVu0VfBits(kFd, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu);
	h.LoadProgram({VADDA_C2(kMaskXyzw, kAcc, kZero), word});
	h.Run();

	Observed o{};
	o.out = jit ? h.GetVu0VfBitsJit(kFd, 'x') : h.GetVu0VfBitsInterp(kFd, 'x');
	o.mac = (jit ? h.GetVu0ViJit(REG_MAC_FLAG) : h.GetVu0ViInterp(REG_MAC_FLAG)) & 0xFFFFu;
	o.stat = (jit ? h.GetVu0ViJit(REG_STATUS_FLAG) : h.GetVu0ViInterp(REG_STATUS_FLAG)) & 0xFFFFu;
	return o;
}

u8 Misses(const VuFlowCase& c, const Observed& o)
{
	u8 m = 0;
	if (o.out != c.out) m |= VFB_VALUE;
	if (o.mac != c.mac) m |= VFB_MAC;
	if (o.stat != c.stat) m |= VFB_STAT;
	return m;
}

const char* kColName[] = {"value", "mac", "stat"};
constexpr u8 kColBit[] = {VFB_VALUE, VFB_MAC, VFB_STAT};

int PopCount(u8 v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1); }

} // namespace

// Asserts the columns this emulator DOES reproduce, and asserts that the ones
// it does not still fail -- so a fix trips the test rather than quietly
// widening the allowance. Regenerate the masks from
// DISABLED_DumpConsoleComparison below; do not hand-edit them.
TEST(Vu0MacroFmacUnderflowConsole, UnderflowMatchesConsole)
{
	int checked = 0, bad_interp = 0, bad_jit = 0;
	for (int i = 0; i < kVuFlowCaseCount; ++i)
	{
		const VuFlowCase& c = kVuFlowCases[i];
		const u32 word = Encode(c);
		ASSERT_NE(word, 0u) << "case " << i << ": no encoder for op " << int(c.op);

		for (int jit = 0; jit < 2; ++jit)
		{
			const u8 known = jit ? c.bad_jit : c.bad_interp;
			const u8 got = Misses(c, RunCase(c, word, jit != 0));
			(jit ? bad_jit : bad_interp) += PopCount(known);

			for (int col = 0; col < 3; ++col)
			{
				SCOPED_TRACE(::testing::Message()
					<< "case " << i << " [" << (jit ? "jit" : "interp") << "] "
					<< kColName[col] << ": " << c.what);
				if (known & kColBit[col])
				{
					EXPECT_TRUE(got & kColBit[col])
						<< "now MATCHES the console. Regenerate autocases_vuflow.h "
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
	EXPECT_EQ(checked, kVuFlowCaseCount);
	EXPECT_EQ(bad_interp, kVuFlowBadInterp);
	EXPECT_EQ(bad_jit, kVuFlowBadJit);
}

// The controls the capture was taken with, re-asserted against the values in
// the header: if the plumbing here ever returns a fixed word, or the flag
// read-back stops moving, the other 72 rows say nothing.
TEST(Vu0MacroFmacUnderflowConsole, ControlsSeparate)
{
	ASSERT_GE(kVuFlowCaseCount, 2);
	const VuFlowCase& a = kVuFlowCases[0];
	const VuFlowCase& b = kVuFlowCases[1];
	EXPECT_NE(a.out, b.out);
	EXPECT_NE(a.mac, b.mac);
	for (int jit = 0; jit < 2; ++jit)
	{
		const Observed oa = RunCase(a, Encode(a), jit != 0);
		const Observed ob = RunCase(b, Encode(b), jit != 0);
		SCOPED_TRACE(jit ? "jit" : "interp");
		EXPECT_EQ(oa.out, a.out);
		EXPECT_EQ(ob.out, b.out);
		EXPECT_NE(oa.out, ob.out);
		EXPECT_NE(oa.mac, ob.mac);
	}
}

// What passing looks like once the underflow region is right, and the source of
// the known-bad masks. Run it with --gtest_also_run_disabled_tests and feed its
// VUFLOW-MISS lines to captures/vuflow/gen_autocases.py.
TEST(Vu0MacroFmacUnderflowConsole, DISABLED_DumpConsoleComparison)
{
	for (int i = 0; i < kVuFlowCaseCount; ++i)
	{
		const VuFlowCase& c = kVuFlowCases[i];
		const u32 word = Encode(c);
		ASSERT_NE(word, 0u);
		for (int jit = 0; jit < 2; ++jit)
		{
			const char* engine = jit ? "jit" : "interp";
			const Observed o = RunCase(c, word, jit != 0);
			const u8 got = Misses(c, o);
			for (int col = 0; col < 3; ++col)
			{
				if (got & kColBit[col])
					std::printf("VUFLOW-MISS %d %s %s\n", i, engine, kColName[col]);
			}
			std::printf("VUFLOW-ROW %2d %-6s out %08X/%08X mac %04X/%04X stat %04X/%04X  %s\n",
				i, engine, c.out, o.out, c.mac, o.mac, c.stat, o.stat, c.what);
			SCOPED_TRACE(::testing::Message() << "case " << i << " [" << engine << "] " << c.what);
			EXPECT_EQ(o.out, c.out);
			EXPECT_EQ(o.mac, c.mac);
			EXPECT_EQ(o.stat, c.stat);
		}
	}
}

} // namespace recompiler_tests
