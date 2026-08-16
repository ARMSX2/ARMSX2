// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU FMAC at the top and the bottom of its range, against real hardware.
//
// autocases_vusat.h is generated from a capture taken on an SCPH-90000
// (captures/vusat). The rows are the answer to a question neither engine had
// ever been asked: microVU's per-op operand-clamp table names the games that
// broke without each clamp rather than a rule, and the interpreter's two
// range functions -- vuDouble() on the way in, VU_MAC_UPDATE() on the way out
// -- both treat exponent 255 as out of range and substitute 0x7F7FFFFF.
//
// What the console says instead, in one line: the VU's largest value is
// 0x7FFFFFFF, one binade above FLT_MAX, exactly like the EE FPU's. So
//
//   * an exponent-255 operand is read at its full value (cases 2-5);
//   * an exponent-255 RESULT is an ordinary number, not an overflow -- it
//     raises no MAC O bit and keeps its mantissa (cases 16, 17);
//   * a result past 0x7FFFFFFF saturates to it and raises O (cases 9-14);
//   * below 2^-126 the result flushes to a signed zero and raises U and Z,
//     while a denormal OPERAND is flushed on the way in, so the op sees a
//     plain zero and raises Z without U (cases 18-23);
//   * MAX and MINI do not flush a denormal operand, order +0 above -0, and
//     write neither flag register (cases 37-40, 47-50).
//
// Two structural results come out of the MADD rows. An overflowed product does
// not become 0x7FFFFFFF before the accumulate: an addend of -0x7FFFFFFF cannot
// cancel it and the result still saturates with the product's sign (cases 28,
// 51, 52). A product that underflows does become zero before the accumulate
// (cases 32, 33). And the status flag's sticky bits carry the multiply stage's
// flags as well as the sum's, which is visible where MAC is clear but a sticky
// bit is set (cases 27, 32).
//
// Scoring is per engine and per column, not JIT-versus-interp: at the time of
// writing both engines are wrong here in overlapping ways, so a differential
// between them would report agreement on rows where they are agreeing about
// the wrong number.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>

#include "autocases_vusat.h"

using namespace console_vusat;

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;

namespace {

// The console probe's register assignment, kept so the emitted pair is the
// same one that was measured.
constexpr u32 kFs = 1, kFt = 2, kAcc = 3, kFd = 4, kZero = 7;
constexpr u32 kMaskXyzw = 0xF;

u32 Encode(const VuSatCase& c)
{
	switch (c.op)
	{
		case VS_ADD:  return VADD_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VS_SUB:  return VSUB_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VS_MUL:  return VMUL_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VS_MADD: return VMADD_C2(kMaskXyzw, kFd, kFs, kFt);
		case VS_MSUB: return VMSUB_C2(kMaskXyzw, kFd, kFs, kFt);
		case VS_MAX:  return VMAX_C2 (kMaskXyzw, kFd, kFs, kFt);
		case VS_MINI: return VMINI_C2(kMaskXyzw, kFd, kFs, kFt);
		default:      return 0;
	}
}

struct Observed
{
	u32 out;
	u32 mac;
	u32 stat;
};

// Runs one row's VADDA/op pair and reads back one engine's answer. The ACC is
// seeded through VADDA rather than written directly because that is what the
// capture measured -- the seed's own flags are part of the status column.
Observed RunCase(const VuSatCase& c, u32 word, bool jit)
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

// One bit per column, set where the engine disagrees with the console.
u8 Misses(const VuSatCase& c, const Observed& o)
{
	u8 m = 0;
	if (o.out != c.out) m |= VSB_VALUE;
	if (o.mac != c.mac) m |= VSB_MAC;
	if (o.stat != c.stat) m |= VSB_STAT;
	return m;
}

const char* kColName[] = {"value", "mac", "stat"};
constexpr u8 kColBit[] = {VSB_VALUE, VSB_MAC, VSB_STAT};

int PopCount(u8 v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1); }

} // namespace

// Asserts the columns this emulator DOES reproduce, and asserts that the ones
// it does not still fail -- so a fix trips the test rather than quietly
// widening the allowance. Regenerate the masks from
// DISABLED_DumpConsoleComparison below; do not hand-edit them.
TEST(Vu0MacroFmacRangeConsole, FmacRangeMatchesConsole)
{
	int checked = 0, bad_interp = 0, bad_jit = 0;
	for (int i = 0; i < kVuSatCaseCount; ++i)
	{
		const VuSatCase& c = kVuSatCases[i];
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
						<< "now MATCHES the console. Regenerate autocases_vusat.h "
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
	EXPECT_EQ(checked, kVuSatCaseCount);
	EXPECT_EQ(bad_interp, kVuSatBadInterp);
	EXPECT_EQ(bad_jit, kVuSatBadJit);
}

// The controls the capture was taken with, re-asserted against the values in
// the header. Neither engine gets these wrong, so the point of them is the
// one above: if the plumbing here ever returns a fixed word, or the flag
// read-back stops moving, the sweep's other 66 rows say nothing.
TEST(Vu0MacroFmacRangeConsole, ControlsSeparate)
{
	ASSERT_GE(kVuSatCaseCount, 2);
	const VuSatCase& a = kVuSatCases[0];
	const VuSatCase& b = kVuSatCases[1];
	EXPECT_NE(a.out, b.out);
	EXPECT_NE(a.mac, b.mac);
	for (int jit = 0; jit < 2; ++jit)
	{
		const Observed oa = RunCase(a, Encode(a), jit != 0);
		const Observed ob = RunCase(b, Encode(b), jit != 0);
		SCOPED_TRACE(jit ? "jit" : "interp");
		EXPECT_EQ(oa.out, a.out);
		EXPECT_EQ(ob.out, b.out);
		EXPECT_EQ(oa.mac, a.mac);
		EXPECT_EQ(ob.mac, b.mac);
		EXPECT_NE(oa.out, ob.out);
		EXPECT_NE(oa.mac, ob.mac);
	}
}

// What passing looks like once the FMAC range model is right, and the source
// of the known-bad masks. Run it with --gtest_also_run_disabled_tests and feed
// its VUSAT-MISS lines to captures/vusat/gen_autocases.py.
TEST(Vu0MacroFmacRangeConsole, DISABLED_DumpConsoleComparison)
{
	for (int i = 0; i < kVuSatCaseCount; ++i)
	{
		const VuSatCase& c = kVuSatCases[i];
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
					std::printf("VUSAT-MISS %d %s %s\n", i, engine, kColName[col]);
			}
			std::printf("VUSAT-ROW %2d %-6s out %08X/%08X mac %04X/%04X stat %04X/%04X  %s\n",
				i, engine, c.out, o.out, c.mac, o.mac, c.stat, o.stat, c.what);
			SCOPED_TRACE(::testing::Message() << "case " << i << " [" << engine << "] " << c.what);
			EXPECT_EQ(o.out, c.out);
			EXPECT_EQ(o.mac, c.mac);
			EXPECT_EQ(o.stat, c.stat);
		}
	}
}

} // namespace recompiler_tests
