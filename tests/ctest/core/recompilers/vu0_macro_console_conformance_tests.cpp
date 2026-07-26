// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU0 COP2 macro mode against real PS2 hardware.
//
// autocases_vu0macro.h is generated from unknownbrackets/ps2autotests
// tests/cpu/vu0_macro/{integer,transfer}.expected.
//
// The integer half is the five integer-ALU ops over eleven constants, issued
// as EE COP2-CO instructions — which in this emulator means the R5900
// COP2 decoder tables, the EE recompiler's macro-mode emitters (not microVU),
// and a different register-transfer path than a VU micro program takes. The
// capture seeds VI with lui/ori/ctc2, so a register holds exactly what its
// label says: `viadd -1, -1: fffe` is a real 0xFFFF + 0xFFFF.
//
// Three things come off the console there:
//   1. the result value;
//   2. that an integer op leaves STATUS/MAC/CLIP alone (the capture printed
//      "(no flag changes)" on all 101 cases, having cleared the three flag
//      registers immediately beforehand);
//   3. that a macro-mode write to VI00 is discarded (each op's block ends with
//      the op targeting vi00 and a `vior vi01, vi00, vi00` read-back, and the
//      console printed 0000 for every one).
//
// The transfer half sweeps every architecturally-named VI index with
// `ctc2 ~0; cfc2` and is the control-register write-mask ground truth.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"

#include "VU.h"

#include <string>

#include "autocases_vu0macro.h"

using namespace ps2auto_vu0macro;

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee;

// vi01 is the destination the capture used, vi02/vi03 its two sources.
constexpr u32 kVd = 1, kVs = 2, kVt = 3;
// EE GPRs for the CFC2 flag read-back. $0 supplies the CTC2 clear value.
constexpr u32 kRStatus = 8, kRMac = 9, kRClip = 10;
constexpr u32 kMaskXyzw = 0xF;

u32 Encode(const VuMacroCase& c, u32 vd)
{
	const std::string op = c.op;
	if (c.form == VM_RRR)
	{
		if (op == "viadd") return VIADD_C2(vd, kVs, kVt);
		if (op == "visub") return VISUB_C2(vd, kVs, kVt);
		if (op == "viand") return VIAND_C2(vd, kVs, kVt);
		if (op == "vior") return VIOR_C2(vd, kVs, kVt);
		return 0;
	}
	if (op == "viaddi") return VIADDI_C2(vd, kVs, c.imm);
	return 0;
}

// Same operand seeding for every test below.
void Seed(EeRecTestHarness& h, const VuMacroCase& c)
{
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vi(kVd, kVdPre);
	h.SeedVu0Vi(kVs, c.vs);
	h.SeedVu0Vi(kVt, c.vt);
}
} // namespace

TEST(Vu0MacroConsoleConformance, IntegerValuesMatchConsole)
{
	int checked = 0;
	for (int i = 0; i < kVuMacroCaseCount; ++i)
	{
		const VuMacroCase& c = kVuMacroCases[i];
		const u32 word = Encode(c, kVd);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;

		SCOPED_TRACE(::testing::Message() << c.label);
		EeRecTestHarness h;
		Seed(h, c);
		h.LoadProgram({word});
		h.Run(); // also gtest-diffs JIT against interpreter
		EXPECT_EQ(h.GetVu0ViJit(kVd) & 0xFFFFu, c.vd);
		EXPECT_EQ(h.GetVu0ViInterp(kVd) & 0xFFFFu, c.vd);
		++checked;
	}
	EXPECT_EQ(checked, kVuMacroCaseCount);
	EXPECT_GT(checked, 95);
}

// The capture clears STATUS/MAC/CLIP with `ctc2 $0` immediately before each op
// and re-reads them after; it printed "(no flag changes)" on all 101 cases.
// Reproduced literally, including the CFC2 read-back, rather than by
// inspecting the VU0 snapshot — CFC2 is the path the console observed through.
TEST(Vu0MacroConsoleConformance, IntegerOpsLeaveFlagsAlone)
{
	int checked = 0;
	for (int i = 0; i < kVuMacroCaseCount; ++i)
	{
		const VuMacroCase& c = kVuMacroCases[i];
		const u32 word = Encode(c, kVd);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;

		SCOPED_TRACE(::testing::Message() << c.label);
		EeRecTestHarness h;
		Seed(h, c);
		h.LoadProgram({
			CTC2(0, REG_STATUS_FLAG),
			CTC2(0, REG_MAC_FLAG),
			CTC2(0, REG_CLIP_FLAG),
			word,
			CFC2(kRStatus, REG_STATUS_FLAG),
			CFC2(kRMac, REG_MAC_FLAG),
			CFC2(kRClip, REG_CLIP_FLAG),
		});
		h.Run();
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			const auto gpr = [&](u32 r) {
				return jit ? h.GetGprJit(r) : h.GetGprInterp(r);
			};
			EXPECT_EQ(gpr(kRStatus), 0u) << "integer op touched STATUS";
			EXPECT_EQ(gpr(kRMac), 0u) << "integer op touched MAC";
			EXPECT_EQ(gpr(kRClip), 0u) << "integer op touched CLIP";
		}
		++checked;
	}
	EXPECT_EQ(checked, kVuMacroCaseCount);
}

// Control for the test above. 101x2x3 assertions that only ever read zero pass
// just as happily against a mis-wired CFC2, so one op that DOES set flags,
// observed through the same instruction, is what makes those zeros mean
// something: VSUB of a register with itself yields +0.0 in every lane, so
// STATUS must come back with Z and sticky-Z set and MAC non-zero.
TEST(Vu0MacroConsoleConformance, FlagReadbackSeesAFloatOpSettingFlags)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, 1.0f, 2.0f, 3.0f, 4.0f);
	h.LoadProgram({
		CTC2(0, REG_STATUS_FLAG),
		CTC2(0, REG_MAC_FLAG),
		VSUB_C2(kMaskXyzw, /*fd*/3, /*fs*/1, /*ft*/1),
		CFC2(kRStatus, REG_STATUS_FLAG),
		CFC2(kRMac, REG_MAC_FLAG),
	});
	h.Run();
	for (int jit = 0; jit < 2; ++jit)
	{
		SCOPED_TRACE(jit ? "[jit]" : "[interp]");
		const u32 status = jit ? h.GetGprJit(kRStatus) : h.GetGprInterp(kRStatus);
		const u32 mac = jit ? h.GetGprJit(kRMac) : h.GetGprInterp(kRMac);
		EXPECT_NE(status & 0x01u, 0u) << "Z not set by a zero result";
		EXPECT_NE(status & 0x40u, 0u) << "sticky Z not set by a zero result";
		EXPECT_NE(mac, 0u) << "MAC not set by a zero result";
	}
}

// Each op's block in the capture ends with the op writing vi00 and a
// `vior vi01, vi00, vi00` read-back; the console printed 0000 every time.
// VI00 is hardwired to zero even as a macro-mode destination.
TEST(Vu0MacroConsoleConformance, WritesToVi00AreDiscarded)
{
	int checked = 0;
	for (int z = 0; z < kVuMacroZeroCaseCount; ++z)
	{
		const VuMacroZeroCase& zc = kVuMacroZeroCases[z];
		// Reuse the first case of the matching op purely for its form/imm;
		// operands below are the capture's own $0 block, not that case's.
		const VuMacroCase* proto = nullptr;
		for (int i = 0; i < kVuMacroCaseCount && !proto; ++i)
			if (std::string(kVuMacroCases[i].op) == zc.op)
				proto = &kVuMacroCases[i];
		ASSERT_NE(proto, nullptr) << zc.op;

		VuMacroCase c = *proto;
		c.vs = 0xDEADu;
		c.vt = 0x7331u;
		c.imm = 4;
		const u32 word = Encode(c, /*vd*/0);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;

		SCOPED_TRACE(::testing::Message() << zc.op << " -> $0");
		EeRecTestHarness h;
		Seed(h, c);
		h.LoadProgram({
			word,
			VIOR_C2(/*id*/kVd, /*is*/0, /*it*/0),
		});
		h.Run();
		EXPECT_EQ(h.GetVu0ViJit(0) & 0xFFFFu, 0u) << "VI00 was written";
		EXPECT_EQ(h.GetVu0ViInterp(0) & 0xFFFFu, 0u) << "VI00 was written";
		EXPECT_EQ(h.GetVu0ViJit(kVd) & 0xFFFFu, zc.vd);
		EXPECT_EQ(h.GetVu0ViInterp(kVd) & 0xFFFFu, zc.vd);
		++checked;
	}
	EXPECT_EQ(checked, kVuMacroZeroCaseCount);
}

// CTC2 write masks over the whole VU0 control bank.
//
// The capture sweeps every architecturally-named VI index with
//     addiu $t0, $0, -1 ; ctc2 $t0, viNN ; cfc2 rd, viNN
// and prints the 32-bit read-back. Reproduced instruction-for-instruction, and
// read back through CFC2 rather than the VU0 snapshot, because CFC2 is what
// the console observed through.
//
// The engines legitimately disagree here (the macro JIT masks the STATUS write
// to the 0xFC0 sticky field; the shared interpreter CTC2 does a plain
// full-width store), so each side runs on its own and is scored separately
// rather than through Run()'s auto-diff.

namespace
{
constexpr u32 kRt = 8, kRdst = 9;

u32 Ctc2Readback(int reg, bool jit)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.LoadProgram({
		ADDIU(kRt, 0, -1),
		CTC2(kRt, static_cast<u32>(reg)),
		CFC2(kRdst, static_cast<u32>(reg)),
	});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	return jit ? h.GetGprJit(kRdst) : h.GetGprInterp(kRdst);
}

// Where this emulator does not reproduce the console, recorded from a real
// run and NOT derived from a rule. Two distinct stories here:
//
//   vi01, vi16 — the arm64 macro JIT already narrows these deliberately
//     (Strh for the 16-bit integer VIs; And #0xFC0 for STATUS, iCOP2-arm64.cpp
//     recCTC2). The shared interpreter CTC2 (pcsx2/VU0.cpp) does a plain
//     32-bit store. The JIT is the correct side.
//
//   vi18, vi27, vi31 — both engines are wrong. CLIP is a 24-bit field and
//     CMSAR0/CMSAR1 are 16-bit, but CTC2's default arm stores all 32 bits; and
//     the REG_CMSAR1 case kicks VU1 without ever storing the value, so a
//     read-back returns whatever VI[31] held before.
struct Ctc2Divergence { int reg; bool bad_interp, bad_jit; };
constexpr Ctc2Divergence kCtc2Divergences[] = {
	{1, true, false},   // interp stores 32 bits into a 16-bit VI
	{16, true, false},  // interp ignores the 0xFC0 STATUS write mask
	{18, true, true},   // CLIP is 24-bit on silicon; both store 32
	{27, true, true},   // CMSAR0 is 16-bit on silicon; both store 32
	{31, true, true},   // CMSAR1 never stored at all; CTC2 only kicks VU1
};

bool Ctc2KnownBad(int reg, bool jit)
{
	for (const Ctc2Divergence& d : kCtc2Divergences)
		if (d.reg == reg)
			return jit ? d.bad_jit : d.bad_interp;
	return false;
}
} // namespace

// Asserts the registers this emulator DOES get right, and asserts that the
// ones it does not still fail — so both a regression and a fix trip the test
// rather than quietly shifting the allowance.
TEST(Vu0MacroConsoleConformance, Ctc2WriteMasksMatchConsole)
{
	int checked = 0, diverged = 0;
	for (int i = 0; i < kCtc2MaskCaseCount; ++i)
	{
		const Ctc2MaskCase& c = kCtc2MaskCases[i];
		if (!c.defined)
			continue; // reserved index: recorded in the table, never asserted
		for (int jit = 0; jit < 2; ++jit)
		{
			const bool ok = Ctc2Readback(c.reg, jit != 0) == c.readback;
			if (!Ctc2KnownBad(c.reg, jit != 0))
			{
				SCOPED_TRACE(::testing::Message()
				             << "vi" << c.reg << " " << c.name
				             << (jit ? " [jit]" : " [interp]"));
				EXPECT_TRUE(ok) << "new divergence from silicon";
			}
			else
			{
				++diverged;
				EXPECT_FALSE(ok)
					<< "vi" << c.reg << " " << c.name
					<< (jit ? " [jit]" : " [interp]")
					<< " now MATCHES silicon. If CTC2 was fixed, drop this row "
					   "from kCtc2Divergences.";
			}
		}
		++checked;
	}
	EXPECT_EQ(checked, 12);
	EXPECT_EQ(diverged, 8);
}

// What passing looks like once CTC2's write masks are right.
TEST(Vu0MacroConsoleConformance, DISABLED_AllCtc2WriteMasksMatchConsole)
{
	for (int i = 0; i < kCtc2MaskCaseCount; ++i)
	{
		const Ctc2MaskCase& c = kCtc2MaskCases[i];
		if (!c.defined)
			continue;
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(::testing::Message()
			             << "vi" << c.reg << " " << c.name
			             << (jit ? " [jit]" : " [interp]"));
			EXPECT_EQ(Ctc2Readback(c.reg, jit != 0), c.readback);
		}
	}
}

// CFC2 / QMFC2 / QMTC2 / LQC2 destination semantics.

// The capture preloads the destination GPR with 0x12345678 in all four words,
// sets vi21 (I) to ~0, then CFC2s it:
//     cfc2: 12345678 12345678 ffffffff ffffffff
// (printed high word first). So CFC2 sign-extends the 32-bit VI across the
// low 64 bits and leaves the upper 64 bits of the GPR completely alone.
TEST(Vu0MacroConsoleConformance, Cfc2SignExtendsAndPreservesUpperGprHalf)
{
	constexpr u64 kPre = 0x1234567812345678ull;
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SetGpr128(kRdst, kPre, kPre);
	h.LoadProgram({
		ADDIU(kRt, 0, -1),
		CTC2(kRt, REG_I),
		CFC2(kRdst, REG_I),
	});
	h.Run();
	EXPECT_EQ(h.GetGpr64Jit(kRdst), 0xFFFFFFFFFFFFFFFFull);
	EXPECT_EQ(h.GetGpr64Interp(kRdst), 0xFFFFFFFFFFFFFFFFull);
	EXPECT_EQ(h.GetGprUpper64Jit(kRdst), kPre);
	EXPECT_EQ(h.GetGprUpper64Interp(kRdst), kPre);
}

// `qmfc2: 3f800000 00000000 00000000 00000000` — VF0 reads back as
// (x,y,z,w) = (0,0,0,1.0), and the transfer overwrites all 128 bits of the
// GPR rather than merging with what was there (0x12345678 in every word).
TEST(Vu0MacroConsoleConformance, Qmfc2OfVf0MatchesConsole)
{
	constexpr u64 kPre = 0x1234567812345678ull;
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SetGpr128(kRdst, kPre, kPre);
	h.LoadProgram({QMFC2(kRdst, 0)});
	h.Run();
	EXPECT_EQ(h.GetGpr64Jit(kRdst), 0x0000000000000000ull);
	EXPECT_EQ(h.GetGprUpper64Jit(kRdst), 0x3F80000000000000ull);
	EXPECT_EQ(h.GetGpr64Interp(kRdst), 0x0000000000000000ull);
	EXPECT_EQ(h.GetGprUpper64Interp(kRdst), 0x3F80000000000000ull);
}

// `qmtc2 -> $vf0: 3f800000 00000000 00000000 00000000` and
// `lqc2 -> $vf0:  3f800000 00000000 00000000 00000000` — VF0 is read-only on
// both the register-transfer path and the load path, not just as an FMAC
// destination.
TEST(Vu0MacroConsoleConformance, WritesToVf0AreDiscarded)
{
	constexpr u64 kPre = 0x1234567812345678ull;
	constexpr u32 kBuf = 0x00080000;

	{
		EeRecTestHarness h;
		h.EnableVu0Capture();
		h.EnableCop1();
		h.SetGpr128(kRt, kPre, kPre);
		// VF0 already reads (0,0,0,1.0) at rest, so "unchanged" proves nothing
		// on its own — the identical transfer to vf1 is the control that shows
		// the instruction ran and could have written.
		h.LoadProgram({QMTC2(kRt, 0), QMTC2(kRt, 1)});
		h.Run();
		SCOPED_TRACE("qmtc2 -> $vf0");
		for (int jit = 0; jit < 2; ++jit)
		{
			const auto vf = [&](u32 r, char l) {
				return jit ? h.GetVu0VfBitsJit(r, l) : h.GetVu0VfBitsInterp(r, l);
			};
			EXPECT_EQ(vf(0, 'x'), 0x00000000u);
			EXPECT_EQ(vf(0, 'y'), 0x00000000u);
			EXPECT_EQ(vf(0, 'z'), 0x00000000u);
			EXPECT_EQ(vf(0, 'w'), 0x3F800000u);
			for (char l : {'x', 'y', 'z', 'w'})
				EXPECT_EQ(vf(1, l), 0x12345678u) << "control: vf1 lane " << l;
		}
	}
	{
		EeRecTestHarness h;
		h.EnableVu0Capture();
		h.EnableCop1();
		for (u32 w = 0; w < 4; ++w)
			h.WriteU32(kBuf + w * 4, 0x12345678u);
		h.SetGpr64(kRt, kBuf);
		h.LoadProgram({LQC2(/*ft*/0, /*base*/kRt, 0), LQC2(/*ft*/1, kRt, 0)});
		h.Run();
		SCOPED_TRACE("lqc2 -> $vf0");
		for (int jit = 0; jit < 2; ++jit)
		{
			const auto vf = [&](u32 r, char l) {
				return jit ? h.GetVu0VfBitsJit(r, l) : h.GetVu0VfBitsInterp(r, l);
			};
			EXPECT_EQ(vf(0, 'x'), 0x00000000u);
			EXPECT_EQ(vf(0, 'y'), 0x00000000u);
			EXPECT_EQ(vf(0, 'z'), 0x00000000u);
			EXPECT_EQ(vf(0, 'w'), 0x3F800000u);
			for (char l : {'x', 'y', 'z', 'w'})
				EXPECT_EQ(vf(1, l), 0x12345678u) << "control: vf1 lane " << l;
		}
	}
}

} // namespace recompiler_tests
