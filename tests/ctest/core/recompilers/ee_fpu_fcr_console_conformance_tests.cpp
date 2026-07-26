// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE FPU control registers (FCR0 / FCR31) against real PS2 hardware.
//
// Built from the console capture in unknownbrackets/ps2autotests,
// tests/cpu/ee_fpu/fcr.expected. That file is free-form text rather than a
// table, so the values it prints are transcribed here directly.
//
// Three things come out of it:
//
// 1. `cfc1 rt, $N` for N = 0..15 all return FCR0 and N = 16..31 all return
//    FCR31 — the EE COP1 decodes bit 4 of the register field and nothing else.
//
// 2. FCR31 writes follow a single model:
//        readback = (written & 0x0083C078) | 0x01000001
//    Only the four sticky flags (bits 3-6), the four cause bits (14-17) and
//    C (23) are implemented; RM, the enables, FS/FO/FN, FCC1-7 and bits 18-20
//    are not; bits 0 and 24 read as one always.
//
// 3. Which flags exceptional arithmetic actually raises.
//
// The two engines legitimately disagree here — recCFC1
// (pcsx2/arm64/iFPU-arm64.cpp) applies the mask model above and honours the
// >= 16 aliasing, while the shared interpreter CFC1 (pcsx2/FPU.cpp) returns
// the raw word, hardcodes 0x2E00 for FCR0, and returns zero for every other
// index. So each engine is scored on its own rather than through Run()'s diff.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"

#include <string>
#include <vector>

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee;

constexpr u32 kRt = 8;    // ctc1 source (single-read tests)
constexpr u32 kRd = 9;    // cfc1 destination
constexpr u32 kSrc = 24;  // ctc1 source in the 16-read aliasing sweep
constexpr u32 kFd = 4, kFs = 5, kFt = 6;

// "fcr0: 00002e30" on every line of the capture, before and after a write of
// 0xDEADBEEF. Also what pcsx2/R5900.cpp seeds fpuRegs.fprc[0] with.
constexpr u32 kFcr0 = 0x00002E30;

constexpr u32 kFcr31Writable = 0x0083C078;
constexpr u32 kFcr31FixedOnes = 0x01000001;

// One (ctc1 value -> cfc1 read-back) pair per isolated FCR31 field, in capture
// order. The trailing string is the capture's own name for the field.
struct Fcr31Write { u32 written, readback; const char* what; };
constexpr Fcr31Write kFcr31Writes[] = {
	{0x00000003, 0x01000001, "rounding mode (RM)"},
	{0x0000007C, 0x01000079, "flags"},
	{0x00000F80, 0x01000001, "enables"},
	{0x0001F000, 0x0101C001, "cause"},
	{0x00020000, 0x01020001, "unimplemented (E)"},
	{0x01000000, 0x01000001, "flushing (FS)"},
	{0x00400000, 0x01000001, "flushing (FO)"},
	{0x00200000, 0x01000001, "flushing (FN)"},
	{0x00800000, 0x01800001, "FCC"},
	{0xFE000000, 0x01000001, "FCC1-7"},
	{0x001C0000, 0x01000001, "unknown (bits 18-20)"},
};
constexpr int kFcr31WriteCount =
	static_cast<int>(sizeof(kFcr31Writes) / sizeof(kFcr31Writes[0]));

// Runs `prog` on one engine from a given FCR31 pre-state and returns kRd.
u32 RunAndReadGpr(const std::vector<u32>& prog, u32 fcr31_pre, bool jit,
                  u32 gpr = kRd)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFcr31(fcr31_pre);
	h.SetGpr128(gpr, 0, 0);
	h.LoadProgram(prog);
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	return jit ? h.GetGprJit(gpr) : h.GetGprInterp(gpr);
}

// Where this emulator does not reproduce the console. Recorded from a real
// run, not derived from a rule. Every one of them is the shared interpreter;
// the arm64 JIT's recCFC1 already implements the hardware model.
struct Divergence { const char* what; bool bad_interp, bad_jit; };
constexpr Divergence kDivergences[] = {
	// CFC1 hardcodes 0x2E00 instead of reading fprc[0].
	{"fcr0", true, false},
	// CFC1 returns 0 for every index that is not 0 or 31; hardware aliases
	// 0-15 onto FCR0 and 16-31 onto FCR31.
	{"alias", true, false},
	// Neither CFC1 nor CTC1 applies the mask model in the interpreter; it
	// stores and returns the raw word.
	{"mask", true, false},
};

bool KnownBad(const char* what, bool jit)
{
	for (const Divergence& d : kDivergences)
		if (std::string(d.what) == what)
			return jit ? d.bad_jit : d.bad_interp;
	return false;
}
} // namespace

// A ctc1 of 0xDEADBEEF into $0 leaves fcr0 at 00002e30.
TEST(EeFpuFcrConsoleConformance, Fcr0IsReadOnly)
{
	for (int jit = 0; jit < 2; ++jit)
	{
		const std::vector<u32> prog = {
			LUI(kRt, 0xDEAD),
			ORI(kRt, kRt, 0xBEEF),
			CTC1(kRt, 0),
			CFC1(kRd, 0),
		};
		const bool ok = RunAndReadGpr(prog, kFcr31FixedOnes, jit != 0) == kFcr0;
		if (!KnownBad("fcr0", jit != 0))
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			EXPECT_TRUE(ok) << "new divergence from silicon";
		}
		else
		{
			EXPECT_FALSE(ok)
				<< (jit ? "[jit]" : "[interp]")
				<< " FCR0 now matches silicon; drop the row from kDivergences.";
		}
	}
}

// All 32 control-register indices: 0-15 hold FCR0 and 16-31 hold FCR31. The
// capture snapshot reproduced here is the one taken after the `flags` write,
// because it is reachable from a known write rather than from BIOS state.
TEST(EeFpuFcrConsoleConformance, ControlRegisterIndicesAliasOnBit4)
{
	constexpr u32 kWritten = 0x0000007C;
	constexpr u32 kExpectHigh = 0x01000079;

	for (int jit = 0; jit < 2; ++jit)
	{
		bool ok = true;
		for (u32 base = 0; base < 32; base += 16)
		{
			// 16 reads per program, into $t0-$t7 ($8-$15) and $s0-$s7
			// ($16-$23). $24 ($t8) carries the ctc1 source. Nothing above
			// $24 is usable: the harness parks through $ra and reserves the
			// k/gp/sp/fp block.
			std::vector<u32> prog = {
				LUI(kSrc, kWritten >> 16),
				ORI(kSrc, kSrc, kWritten & 0xFFFF),
				CTC1(kSrc, 31),
			};
			for (u32 i = 0; i < 16; ++i)
				prog.push_back(CFC1(8 + i, base + i));

			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFcr31(kFcr31FixedOnes);
			for (u32 i = 0; i < 16; ++i)
				h.SetGpr128(8 + i, 0, 0);
			h.LoadProgram(prog);
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();

			const u32 expect = (base == 0) ? kFcr0 : kExpectHigh;
			for (u32 i = 0; i < 16; ++i)
			{
				const u32 got = jit ? h.GetGprJit(8 + i) : h.GetGprInterp(8 + i);
				if (got != expect)
					ok = false;
			}
		}
		if (!KnownBad("alias", jit != 0))
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			EXPECT_TRUE(ok) << "new divergence from silicon: the 32 control "
			                   "register indices must alias onto FCR0/FCR31";
		}
		else
		{
			EXPECT_FALSE(ok)
				<< (jit ? "[jit]" : "[interp]")
				<< " aliasing now matches silicon; drop the row from "
				   "kDivergences.";
		}
	}
}

// Eleven ctc1/cfc1 pairs, one per isolated FCR31 field.
TEST(EeFpuFcrConsoleConformance, Fcr31WriteMaskMatchesConsole)
{
	int checked = 0;
	for (int i = 0; i < kFcr31WriteCount; ++i)
	{
		const Fcr31Write& w = kFcr31Writes[i];
		// The captured numbers must all be consistent with the single model
		// they imply; if a line is added later that breaks it, say so here
		// rather than silently widening the mask.
		EXPECT_EQ((w.written & kFcr31Writable) | kFcr31FixedOnes, w.readback)
			<< "capture line `" << w.what << "` does not fit the derived "
			   "FCR31 model";

		const std::vector<u32> prog = {
			LUI(kRt, w.written >> 16),
			ORI(kRt, kRt, w.written & 0xFFFF),
			CTC1(kRt, 31),
			CFC1(kRd, 31),
		};
		for (int jit = 0; jit < 2; ++jit)
		{
			const bool ok =
				RunAndReadGpr(prog, kFcr31FixedOnes, jit != 0) == w.readback;
			if (!KnownBad("mask", jit != 0))
			{
				SCOPED_TRACE(::testing::Message()
				             << "Update " << w.what
				             << (jit ? " [jit]" : " [interp]"));
				EXPECT_TRUE(ok) << "new divergence from silicon";
			}
			else
			{
				EXPECT_FALSE(ok)
					<< "Update " << w.what << (jit ? " [jit]" : " [interp]")
					<< " now matches silicon; drop the row from kDivergences.";
			}
		}
		++checked;
	}
	EXPECT_EQ(checked, kFcr31WriteCount);
}

// Which flags the arithmetic raises. Each capture line clears FCR31, runs one
// exceptional operation and prints FCR31. Reproduced with FCR31 preset to
// 0x01000001 (what a `ctc1 $0` write leaves on silicon) rather than to zero,
// so the always-one bits are present on both engines and what is under test is
// purely which flag and cause bits the operation sets.
//
// Two of the seven print a result unambiguous enough to assert as well. The
// rest print through the test program's float formatter, which renders
// anything at exponent 255 as "NaN" — not enough to pin bits, so only FCR31 is
// checked for those.
namespace
{
enum FlagOp { FO_SQRT, FO_DIV, FO_ADD, FO_MUL };
struct FlagSituation
{
	const char* what;
	int op;
	u32 fs, ft;
	u32 fcr31;
	bool check_fd;
	u32 fd;
	// The recompiler never raises O/SO: every FPUflagO write in the x86
	// emitter is commented out and the arm64 port does not define the
	// constant, so the two saturating cases below come back with a bare
	// 0x01000001. The interpreter's checkOverflow does raise them and does
	// match silicon.
	bool bad_jit;
};
constexpr FlagSituation kFlagSituations[] = {
	// sqrt(-1) is 1.0 on the PS2, not NaN: SQRT takes the magnitude. Both the
	// invalid sticky flag (bit 6) and its cause bit (17) come up.
	{"sqrt(-1)", FO_SQRT, 0xBF800000, 0xBF800000, 0x01020041, true, 0x3F800000, false},
	{"Divide zero by zero", FO_DIV, 0x00000000, 0x00000000, 0x01020041, false, 0, false},
	{"Divide one by zero", FO_DIV, 0x3F800000, 0x00000000, 0x01010021, false, 0, false},
	{"NAN math", FO_ADD, 0x7F800001, 0x7F800001, 0x01008011, false, 0, true},
	{"Overflow", FO_MUL, 0x7F7FFFFF, 0x7F7FFFFF, 0x01008011, false, 0, true},
	// FLT_MIN/3 is a denormal, which the PS2 flushes to zero — and raises
	// nothing doing it.
	{"Underflow", FO_DIV, 0x00800000, 0x40400000, 0x01000001, true, 0x00000000, false},
	// 1 / 3.0155 — an ordinary inexact result. The EE has no inexact flag,
	// which is the same fact bit 2 being unwritable shows above.
	{"Inexact", FO_DIV, 0x3F800000, 0x4040FFFF, 0x01000001, false, 0, false},
};
constexpr int kFlagSituationCount =
	static_cast<int>(sizeof(kFlagSituations) / sizeof(kFlagSituations[0]));

u32 FlagOpWord(const FlagSituation& s)
{
	switch (s.op)
	{
		case FO_SQRT: return SQRT_S(kFd, kFt);
		case FO_DIV: return DIV_S(kFd, kFs, kFt);
		case FO_ADD: return ADD_S(kFd, kFs, kFt);
		case FO_MUL: return MUL_S(kFd, kFs, kFt);
		default: return 0;
	}
}
} // namespace

TEST(EeFpuFcrConsoleConformance, ExceptionFlagsMatchConsole)
{
	int checked = 0;
	for (int i = 0; i < kFlagSituationCount; ++i)
	{
		const FlagSituation& s = kFlagSituations[i];
		const u32 word = FlagOpWord(s);
		ASSERT_NE(word, 0u) << s.what;

		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFcr31(kFcr31FixedOnes);
			h.SetFprBits(kFd, 0x00001337);
			h.SetFprBits(kFs, s.fs);
			h.SetFprBits(kFt, s.ft);
			h.SetGpr128(kRd, 0, 0);
			h.LoadProgram({word, CFC1(kRd, 31)});
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();

			const u32 got = jit ? h.GetGprJit(kRd) : h.GetGprInterp(kRd);
			if (jit && s.bad_jit)
			{
				EXPECT_NE(got, s.fcr31)
					<< s.what
					<< " [jit] now matches silicon; the recompiler must have "
					   "gained O/SO handling — drop bad_jit for this row.";
				continue;
			}

			SCOPED_TRACE(::testing::Message()
			             << s.what << (jit ? " [jit]" : " [interp]"));
			EXPECT_EQ(got, s.fcr31);
			if (s.check_fd)
			{
				EXPECT_EQ(jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd),
				          s.fd);
			}
		}
		++checked;
	}
	EXPECT_EQ(checked, kFlagSituationCount);
}

// What passing looks like once the interpreter's CFC1/CTC1 model the hardware
// the way the recompiler already does.
TEST(EeFpuFcrConsoleConformance, DISABLED_BothEnginesMatchConsoleFcrModel)
{
	for (int jit = 0; jit < 2; ++jit)
	{
		SCOPED_TRACE(jit ? "[jit]" : "[interp]");
		EXPECT_EQ(RunAndReadGpr({CFC1(kRd, 0)}, kFcr31FixedOnes, jit != 0),
		          kFcr0);
		EXPECT_EQ(RunAndReadGpr({CFC1(kRd, 7)}, kFcr31FixedOnes, jit != 0),
		          kFcr0);
		EXPECT_EQ(RunAndReadGpr({CFC1(kRd, 20)}, kFcr31FixedOnes, jit != 0),
		          kFcr31FixedOnes);
		for (int i = 0; i < kFcr31WriteCount; ++i)
		{
			const Fcr31Write& w = kFcr31Writes[i];
			SCOPED_TRACE(w.what);
			EXPECT_EQ(RunAndReadGpr({LUI(kRt, w.written >> 16),
			                         ORI(kRt, kRt, w.written & 0xFFFF),
			                         CTC1(kRt, 31), CFC1(kRd, 31)},
			                        kFcr31FixedOnes, jit != 0),
			          w.readback);
		}
	}
}

} // namespace recompiler_tests
