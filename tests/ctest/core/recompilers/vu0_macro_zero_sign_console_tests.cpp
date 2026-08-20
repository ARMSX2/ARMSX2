// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The sign of a zero as it crosses a VU0 macro-mode FMAC's two stages, against
// real hardware.
//
// A previous capture settled the two halves of this and left the join open. A
// result below 2^-126 flushes to a SIGNED zero, and a product that underflows
// becomes zero BEFORE the accumulate. What no row asked was whether the zero
// the product turns into is the signed one -- every row it took accumulated
// onto +0, where a mixed-sign zero sum is +0 whichever way the question is
// answered. These rows accumulate onto -0, where it is not:
//
//     madd(-0, -2^-127)   signed -> (-0)+(-0) = -0    plain -> (-0)+(+0) = +0
//     msub(-0, -2^-127)   signed -> (-0)-(-0) = +0    plain -> (-0)-(+0) = -0
//
// The console returns -0 and +0 respectively, so the flushed product keeps its
// sign; and because the two point opposite ways, a run that answered them the
// same way would have refuted both readings rather than picking one.
//
// The second result is in the status column. The multiply stage contributes a
// full ZSUO nibble to the sticky field, S included, and its S is simply the
// product's sign bit -- an ordinary negative product with a positive result
// leaves MAC entirely clear and sticky S set (case 32). All four arms say so:
// ordinary (32, 34), zero (35), underflow (13, 18, 22, 29) and overflow (41),
// the last needing an MSUB because an overflowing negative product drags a
// MADD's result negative and its cause nibble would set sticky S anyway.
//
// Scoring is per engine and per column, not JIT-versus-interp: the two engines
// are wrong here in overlapping ways, so a differential between them would
// report agreement on rows where they agree about the wrong number.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>

#include "autocases_vuzsign.h"

using namespace console_vuzsign;

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;

namespace {

// The console probe's register assignment, kept so the emitted pair is the one
// that was measured.
constexpr u32 kFs = 1, kFt = 2, kAcc = 3, kFd = 4, kOne = 5, kZero = 7;
constexpr u32 kMaskXyzw = 0xF;

u32 Encode(const VuZCase& c)
{
	switch (c.op)
	{
		case VZ_NONE:  return 0; // seed only: the row measures the seed itself
		case VZ_ADD:   return VADD_C2  (kMaskXyzw, kFd, kFs, kFt);
		case VZ_SUB:   return VSUB_C2  (kMaskXyzw, kFd, kFs, kFt);
		case VZ_MUL:   return VMUL_C2  (kMaskXyzw, kFd, kFs, kFt);
		case VZ_MADD:  return VMADD_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VZ_MSUB:  return VMSUB_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VZ_MULA:  return VMULAx_C2 (kMaskXyzw, kFs, kFt);
		case VZ_MADDA: return VMADDAx_C2(kMaskXyzw, kFs, kFt);
		default:       return 0;
	}
}

struct Observed
{
	u32 out;
	u32 mac;
	u32 stat;
};

// Runs one row's seed/op pair and reads back one engine's answer.
//
// The ACC seed is VMULAx against a 1.0, not the VADDA an earlier capture used:
// (-0) + (+0) is +0, so an adder seed cannot put a -0 in the accumulator, and
// half these rows need one. Its own flags are part of the status column, which
// is why the status clear goes before it and not after.
//
// The probe read ACC back through `vmsub $vf4, $vf7, $vf7` because macro mode
// has no ACC move; here the harness reads the register directly. Cases 24 and
// 26 are what license that substitution -- they show the readback and the seed
// each carrying a -0 -- so the two paths agree on every word this file asserts.
Observed RunCase(const VuZCase& c, u32 word, bool jit)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	// Both engines are read here and each is scored against the console on its
	// own, so Run()'s JIT-vs-interp auto-diff would fail on the very rows this
	// file exists to record.
	h.ExpectVu0Divergence();
	h.EnableCop1();
	h.SeedVu0VfBits(kFs, c.fs, c.fs, c.fs, c.fs);
	h.SeedVu0VfBits(kFt, c.ft, c.ft, c.ft, c.ft);
	h.SeedVu0VfBits(kAcc, c.acc, c.acc, c.acc, c.acc);
	h.SeedVu0VfBits(kOne, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);
	h.SeedVu0VfBits(kZero, 0, 0, 0, 0);
	h.SeedVu0VfBits(kFd, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu);

	std::vector<u32> prog{CTC2(0, REG_STATUS_FLAG), VMULAx_C2(kMaskXyzw, kAcc, kOne)};
	if (word != 0)
		prog.push_back(word);
	h.LoadProgram(prog);
	h.Run();

	Observed o{};
	if (c.acc_dest)
		o.out = jit ? h.GetVu0AccBitsJit('x') : h.GetVu0AccBitsInterp('x');
	else
		o.out = jit ? h.GetVu0VfBitsJit(kFd, 'x') : h.GetVu0VfBitsInterp(kFd, 'x');
	o.mac = (jit ? h.GetVu0ViJit(REG_MAC_FLAG) : h.GetVu0ViInterp(REG_MAC_FLAG)) & 0xFFFFu;
	o.stat = (jit ? h.GetVu0ViJit(REG_STATUS_FLAG) : h.GetVu0ViInterp(REG_STATUS_FLAG)) & 0xFFFFu;
	return o;
}

// One bit per column, set where the engine disagrees with the console.
u8 Misses(const VuZCase& c, const Observed& o)
{
	u8 m = 0;
	if (o.out != c.out) m |= VZB_VALUE;
	if (o.mac != c.mac) m |= VZB_MAC;
	if (o.stat != c.stat) m |= VZB_STAT;
	return m;
}

const char* kColName[] = {"value", "mac", "stat"};
constexpr u8 kColBit[] = {VZB_VALUE, VZB_MAC, VZB_STAT};

int PopCount(u8 v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1); }

} // namespace

// Asserts the columns this emulator DOES reproduce, and asserts that the ones
// it does not still fail -- so a fix trips the test rather than quietly
// widening the allowance. Regenerate the masks from
// DISABLED_DumpConsoleComparison below; do not hand-edit them.
TEST(Vu0MacroZeroSignConsole, ZeroSignMatchesConsole)
{
	int checked = 0, bad_interp = 0, bad_jit = 0;
	for (int i = 0; i < kVuZCaseCount; ++i)
	{
		const VuZCase& c = kVuZCases[i];
		const u32 word = Encode(c);
		ASSERT_TRUE(word != 0 || c.op == VZ_NONE)
			<< "case " << i << ": no encoder for op " << int(c.op);

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
						<< "now MATCHES the console. Regenerate autocases_vuzsign.h "
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
	EXPECT_EQ(checked, kVuZCaseCount);
	EXPECT_EQ(bad_interp, kVuZBadInterp);
	EXPECT_EQ(bad_jit, kVuZBadJit);
}

// The controls the capture was taken with, re-asserted against the values in
// the header. Neither engine gets these wrong, so the point of them is the one
// above: if the plumbing here ever returns a fixed word, or the flag read-back
// stops moving, the other rows say nothing.
//
// The last two are the vehicle's own liveness clause rather than the FMAC's. A
// -0 has to survive both the seed and the read-back for the accumulator rows to
// mean anything, and both paths erase it in the obvious implementation.
TEST(Vu0MacroZeroSignConsole, ControlsSeparate)
{
	ASSERT_GE(kVuZCaseCount, 27);
	const VuZCase& a = kVuZCases[0];
	const VuZCase& b = kVuZCases[1];
	EXPECT_NE(a.out, b.out);
	EXPECT_NE(a.mac, b.mac);
	for (int jit = 0; jit < 2; ++jit)
	{
		SCOPED_TRACE(jit ? "jit" : "interp");
		const Observed oa = RunCase(a, Encode(a), jit != 0);
		const Observed ob = RunCase(b, Encode(b), jit != 0);
		EXPECT_EQ(oa.out, a.out);
		EXPECT_EQ(ob.out, b.out);
		EXPECT_EQ(oa.mac, a.mac);
		EXPECT_EQ(ob.mac, b.mac);
		EXPECT_NE(oa.out, ob.out);
		EXPECT_NE(oa.mac, ob.mac);

		// 24: ACC = -0 * 1.0, no flush involved.  26: ACC seeded -0 directly.
		EXPECT_EQ(RunCase(kVuZCases[24], Encode(kVuZCases[24]), jit != 0).out, 0x80000000u)
			<< "the accumulator read-back cannot see a -0; every acc row is void";
		EXPECT_EQ(RunCase(kVuZCases[26], Encode(kVuZCases[26]), jit != 0).out, 0x80000000u)
			<< "the accumulator seed cannot express a -0; every acc row is void";
	}
}

// What passing looks like once the model is right, and the source of the
// known-bad masks. Run it with --gtest_also_run_disabled_tests and feed its
// VUZSIGN-MISS lines to the capture's gen_autocases.py.
TEST(Vu0MacroZeroSignConsole, DISABLED_DumpConsoleComparison)
{
	for (int i = 0; i < kVuZCaseCount; ++i)
	{
		const VuZCase& c = kVuZCases[i];
		const u32 word = Encode(c);
		for (int jit = 0; jit < 2; ++jit)
		{
			const char* engine = jit ? "jit" : "interp";
			const Observed o = RunCase(c, word, jit != 0);
			const u8 got = Misses(c, o);
			for (int col = 0; col < 3; ++col)
			{
				if (got & kColBit[col])
					std::printf("VUZSIGN-MISS %d %s %s\n", i, engine, kColName[col]);
			}
			std::printf("VUZSIGN-ROW %2d %-6s out %08X/%08X mac %04X/%04X stat %04X/%04X  %s\n",
				i, engine, c.out, o.out, c.mac, o.mac, c.stat, o.stat, c.what);
		}
	}
}

} // namespace recompiler_tests
