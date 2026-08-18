// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The VU FMAC does not fuse its multiply-accumulate.
//
// pcsx2/CMakeLists.txt compiles VUops.cpp with -ffp-contract=off, because the
// project-wide -ffp-contract=fast otherwise lets the compiler turn
// `acc + fs * ft` into a single-rounded fmadd -- 423 of them in that one
// translation unit (aarch64, clang 23). The only test behind that line was
// EeVu0Cop2Macro.Jak3CameraBasisKernelChainMatchesInterp, which compares the
// two engines to each other: it says they round the same number of times, not
// how many times a PlayStation 2 does.
//
// captures/vumadd/vumadd.c runs VU0 macro-mode MADDA/MSUBA on an SCPH-90000
// over ten operand triples; eight of them are chosen so that a fused
// accumulate and two separate roundings give different words, and one of those
// eight separates three models rather than two:
//
//     acc = 0x3F800001 (1 + 2^-23), fs = 0x3EAAAAAB (1/3), ft = 0x40400000 (3)
//     exact product = 1.00000002980232239
//
//     0x33C00000   fused, no intermediate rounding
//     0x34000000   product rounded to 0x3F800000 first   <-- the console
//     0x34400000   product rounded to 0x3F7FFFFF first (one ULP below the
//                  round-toward-zero value, the EE multiplier's other outcome)
//
// The console took the two-rounding answer on all eight, and the same words on
// operand-order swaps; two runs, byte-identical. So -ffp-contract=off on
// VUops.cpp is an accuracy setting rather than a consistency one.
//
// Both engines are read out of a single Run(), which is the only entry point
// that captures VU0 state for each of them: RunJitNoDiff() mirrors the JIT's
// VU snapshot into the interpreter's, and RunInterpOnly() never captures VU
// state at all, so GetVu0AccBitsInterp() after either one reports something
// other than what the interpreter computed.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"

#include "autocases_vumadd.h"

using namespace console_vumadd;

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee;

constexpr u32 kFs = 4, kFt = 5;
constexpr u32 kMaskX = 0x8;  // dest mask, x lane only

// COP2-CO SPECIAL2, dispatched on (sa << 2) | (funct & 3) with funct in
// 0x3C-0x3F -- see COP2_SPEC2 in ee_vu0_cop2_madda_bc_tests.cpp. VADDA is
// table index 0x28, VMADDA 0x29, VSUBA 0x2C, VMSUBA 0x2D
// (R5900OpcodeTables.cpp), so the two accumulate forms this file needs are
// sa 0x0A/0x0B with funct 0x3D.
constexpr u32 VMADDA_C2(u32 mask_xyzw, u32 fs, u32 ft)
{
	return COP2_FMAC(mask_xyzw, 0x0A, fs, ft, 0x3D);
}
constexpr u32 VMSUBA_C2(u32 mask_xyzw, u32 fs, u32 ft)
{
	return COP2_FMAC(mask_xyzw, 0x0B, fs, ft, 0x3D);
}

void Build(EeRecTestHarness& h, const VuMaddCase& c)
{
	h.EnableVu0Capture();
	h.SeedVu0AccBits(c.acc, 0, 0, 0);
	h.SeedVu0VfBits(kFs, c.fs, 0, 0, 0);
	h.SeedVu0VfBits(kFt, c.ft, 0, 0, 0);
	h.LoadProgram({c.sub ? VMSUBA_C2(kMaskX, kFs, kFt) : VMADDA_C2(kMaskX, kFs, kFt)});
}

struct Both
{
	u32 jit, interp;
};

Both RunBoth(const VuMaddCase& c)
{
	EeRecTestHarness h;
	Build(h, c);
	// Each engine is scored against the console below, which is what a
	// JIT-versus-interp diff would have to be replaced by anyway now that the
	// two disagree: the interpreter carries a multiply stage's flags into the
	// sticky field and the emitters do not.
	h.ExpectVu0Divergence();
	h.Run();
	return {h.GetVu0AccBitsJit('x'), h.GetVu0AccBitsInterp('x')};
}

std::string Describe(const VuMaddCase& c)
{
	char buf[192];
	std::snprintf(buf, sizeof(buf), "%s acc=%08X fs=%08X ft=%08X -- %s",
		c.sub ? "vmsuba.x" : "vmadda.x", c.acc, c.fs, c.ft, c.what);
	return buf;
}

// Rows 0 and 1 are the controls: an identity, and the same seed-and-read-back
// path with a different constant. If they ever produce the same word the
// read-back is constant and every expectation below is satisfied by an engine
// that does nothing, so this runs first.
TEST(VuMaddContractConsole, CanTellTheRowsApart)
{
	ASSERT_GE(std::size(kVuMaddCases), 2u);
	EXPECT_NE(kVuMaddCases[0].acc_out, kVuMaddCases[1].acc_out)
		<< "the capture's own controls collapsed -- regenerate autocases_vumadd.h";
	const Both a = RunBoth(kVuMaddCases[0]);
	const Both b = RunBoth(kVuMaddCases[1]);
	EXPECT_NE(a.interp, b.interp) << "interpreter ACC read-back does not vary with the case";
	EXPECT_NE(a.jit, b.jit) << "JIT ACC read-back does not vary with the case";
}

TEST(VuMaddContractConsole, InterpreterMatchesConsole)
{
	for (const VuMaddCase& c : kVuMaddCases)
		EXPECT_EQ(RunBoth(c).interp, c.acc_out) << Describe(c);
}

TEST(VuMaddContractConsole, MacroModeJitMatchesConsole)
{
	for (const VuMaddCase& c : kVuMaddCases)
		EXPECT_EQ(RunBoth(c).jit, c.acc_out) << Describe(c);
}

} // namespace
} // namespace recompiler_tests
