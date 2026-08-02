// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The EE divide/sqrt unit's zero-divisor result, scored against real hardware
// across the EE clamp modes.
//
// DIV.S and RSQRT.S both special-case a divisor whose exponent field is zero
// (denormals included) and return a saturated value instead of dividing. Two
// things about that value differ from the console, and both are here.
//
// The hardware rows, from unknownbrackets/ps2autotests -- the explicit hex
// rows, not the CF_* aliases:
//
//   tests/cpu/ee_fpu/muldiv.expected            tests/cpu/ee_fpu/sqrt.expected
//     div 3f800000, 00000000: 7fffffff            rsqrt 3f800000, 00000000: 7fffffff
//     div 00000000, 00000000: 7fffffff            rsqrt 00000000, 00000000: 7fffffff
//     div 00000000, 80000000: ffffffff            rsqrt 00000000, 80000000: 7fffffff
//     div 80000000, 00000000: ffffffff            rsqrt 80000000, 00000000: ffffffff
//     div 80000000, 80000000: 7fffffff            rsqrt 80000000, 80000000: ffffffff
//
// Read the two columns against each other and the rules fall out.
//
//   Magnitude: the console saturates to 0x7FFFFFFF, not the 0x7F7FFFFF
//   (+FLT_MAX) PCSX2's fast path uses. On the EE that is not a NaN -- the EE
//   has no Inf/NaN encoding, exponent 255 is an ordinary large exponent, which
//   the capture proves directly: `div 7fffffff, 7fffffff: 3f800000` divides it
//   by itself and gets 1.0.
//
//   Sign: DIV takes sign(Fs ^ Ft) -- the four signed-zero rows are +,-,-,+.
//   RSQRT takes sign(Fs) alone -- its four rows are +,+,-,-, keyed to the
//   dividend and ignoring Ft's sign, which is the physical rule: RSQRT divides
//   by sqrt(|Ft|), and that is never negative. PCSX2 takes RSQRT's sign from
//   Ft, which agrees with the console on the two rows where Fs and Ft share a
//   sign and is inverted on the two where they differ.
//
// Neither is simply a bug to fix. The EE clamp modes (GameDB eeClampMode,
// EmuConfig.Cpu.Recompiler fpuOverflow/fpuExtraOverflow/fpuFullMode) trade
// console exactness for speed and host sanity, and they are what users run.
// Measured here, all four modes, both engines:
//
//   mode 0/1/2   both engines      0x7F7FFFFF, RSQRT sign from Ft
//   mode 3 FULL  arm64 JIT         0x7FFFFFFF, RSQRT sign from Fs  <- console
//   mode 3 FULL  interpreter       0x7F7FFFFF, RSQRT sign from Ft
//
// So the console behaviour already ships, in the FULL path (iFPUd-arm64.cpp
// SetMaxValueS, whose comment records the same 0x7fffffff-not-0x7f7fffff
// point), and eeClampMode:3 is not exotic -- FFX, Max Payne, Dark Cloud 2,
// Klonoa 2, ~150 serials. The fast path's +FLT_MAX is the deliberate
// compromise: it keeps host-illegal exponent-255 patterns out of an arithmetic
// pipeline that, unlike the EE's, does have Inf and NaN. The gap is that the
// interpreter has no FULL path at all, so a user on the EE interpreter running
// an eeClampMode:3 game gets the fast-path value where the recompiler gives
// the console one. That is what the two disabled tripwires below pin.
//
// The surface a fix would have to cover: pcsx2/FPU.cpp:14 posFmax (used at :68
// checkOverflow, :111 checkDivideByZero, :346 RSQRT_S), arm64
// iFPU-arm64.cpp:940 and :1059, iCOP2-arm64.cpp:470 s_cop2MaxFloat and
// :2603/:2721, microVU_Compile-arm64.inl:674, VUflags.cpp:40,
// VUops.cpp:451/:966/:1009.

#include "harness/EeRecTestHarness.h"

#include "Config.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFd = 4, kFs = 5, kFt = 6;

// One capture row. `console` is the full 32-bit hardware result; the tests
// below split it into sign and magnitude so each defect is asserted alone.
struct ZeroDivisorRow
{
	u32 fs, ft;
	bool rsqrt;
	u32 console;
	const char* what;
};

constexpr ZeroDivisorRow kRows[] = {
	// muldiv.expected -- DIV.S, sign(Fs ^ Ft)
	{0x3F800000u, 0x00000000u, false, 0x7FFFFFFFu, "div  1.0 / +0"},
	{0x00000000u, 0x00000000u, false, 0x7FFFFFFFu, "div  +0  / +0"},
	{0x00000000u, 0x80000000u, false, 0xFFFFFFFFu, "div  +0  / -0"},
	{0x80000000u, 0x00000000u, false, 0xFFFFFFFFu, "div  -0  / +0"},
	{0x80000000u, 0x80000000u, false, 0x7FFFFFFFu, "div  -0  / -0"},
	// sqrt.expected -- RSQRT.S, sign(Fs) alone
	{0x3F800000u, 0x00000000u, true,  0x7FFFFFFFu, "rsqrt 1.0 / sqrt(+0)"},
	{0x00000000u, 0x00000000u, true,  0x7FFFFFFFu, "rsqrt +0  / sqrt(+0)"},
	{0x00000000u, 0x80000000u, true,  0x7FFFFFFFu, "rsqrt +0  / sqrt(-0)"},
	{0x80000000u, 0x00000000u, true,  0xFFFFFFFFu, "rsqrt -0  / sqrt(+0)"},
	{0x80000000u, 0x80000000u, true,  0xFFFFFFFFu, "rsqrt -0  / sqrt(-0)"},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// What the fast path produces instead: the console's sign, and a magnitude a
// binade low at FLT_MAX because it saturates in host singles. Both engines take
// RSQRT's sign from Fs now, so there is no rule left to recompute here.
u32 FastPathValue(const ZeroDivisorRow& r)
{
	return (r.console & 0x80000000u) | 0x7F7FFFFFu;
}

// Runs one row on one engine and returns the result register.
u32 RunRow(const ZeroDivisorRow& r, bool jit, bool full_mode)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (full_mode)
		h.EnableFpuFullMode();
	h.SetFcr31(0);
	h.SetFprBits(kFs, r.fs);
	h.SetFprBits(kFt, r.ft);
	h.LoadProgram({r.rsqrt ? RSQRT_S(kFd, kFs, kFt) : DIV_S(kFd, kFs, kFt)});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	return jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd);
}

} // namespace

// ---------------------------------------------------------------------------
// The compromise, pinned on the fast path, which is the only tier that still
// makes it: default clamp mode saturates at +/-FLT_MAX, a binade below the
// console. Asserting the non-console value on purpose, so a later change cannot
// quietly move what the mode nearly every game runs in produces. The
// interpreter is out of that compromise and matches the console on every row
// here, which is asserted alongside so the two tiers cannot swap roles
// unnoticed.
//
// If you are here because the [jit] leg failed: you changed the fast path, which
// is the thing the clamp modes exist to avoid. Scope the change to the FULL path
// instead.
// ---------------------------------------------------------------------------
TEST(EeFpuZeroDivisorConsole, DefaultClampModeSaturatesToFltMaxOnTheFastPath)
{
	ASSERT_FALSE(EmuConfig.Cpu.Recompiler.fpuFullMode)
		<< "this test describes the NON-full path; something enabled FULL mode";

	for (int i = 0; i < kRowCount; ++i)
	{
		const ZeroDivisorRow& r = kRows[i];
		SCOPED_TRACE(::testing::Message() << r.what);
		EXPECT_EQ(RunRow(r, /*jit=*/true, /*full_mode=*/false), FastPathValue(r))
			<< "[jit] the fast path's saturation moved";
		EXPECT_EQ(RunRow(r, /*jit=*/false, /*full_mode=*/false), r.console)
			<< "[interp] must match the console, sign and magnitude";
	}
}

// ---------------------------------------------------------------------------
// In FULL mode the arm64 JIT matches the console on every row, sign and
// magnitude both, and nothing covered that before.
//
// JIT only: the interpreter has no FULL path (see ee_rec_fpu_full_mode_tests.cpp
// header), which is what the tripwires below are about.
// ---------------------------------------------------------------------------
TEST(EeFpuZeroDivisorConsole, FullClampModeJitMatchesConsoleExactly)
{
	for (int i = 0; i < kRowCount; ++i)
	{
		const ZeroDivisorRow& r = kRows[i];
		SCOPED_TRACE(::testing::Message() << r.what << " [jit, FULL]");
		EXPECT_EQ(RunRow(r, /*jit=*/true, /*full_mode=*/true), r.console);
	}
}

// ---------------------------------------------------------------------------
// MAGNITUDE. The console saturates to 0x7FFFFFFF, and in FULL mode both engines
// now do too. Sign is masked off deliberately: the test below owns that, and
// neither should fail for the other's reason.
//
// A tripwire until 2026-07-31, expecting the fix to be a FULL path for the
// interpreter. What happened instead is that the interpreter stopped doing its
// arithmetic in host singles at all, so the header's warning about pushing an
// exponent-255 word into a downstream op no longer reaches it. It still reaches
// the fast path: posFmax is unchanged, and
// DefaultClampModeSaturatesToFltMaxOnTheFastPath above holds that line.
// ---------------------------------------------------------------------------
TEST(EeFpuZeroDivisorConsole, FullClampModeBothEnginesMatchConsoleMagnitude)
{
	for (int i = 0; i < kRowCount; ++i)
	{
		const ZeroDivisorRow& r = kRows[i];
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(::testing::Message()
				<< r.what << (jit ? " [jit]" : " [interp]") << " FULL, magnitude only");
			EXPECT_EQ(RunRow(r, jit != 0, /*full_mode=*/true) & 0x7FFFFFFFu,
				r.console & 0x7FFFFFFFu);
		}
	}
}

// ---------------------------------------------------------------------------
// SIGN. RSQRT's zero-divisor result takes its sign from Fs on the console; the
// header has the rule and the rows it came off. Magnitude is masked off; the
// test above owns that.
//
// A tripwire until 2026-07-31, and it took two fixes rather than the one it
// expected: the interpreter now reads Fs's sign, and so does the arm64 fast
// path, which was reading Ft's and was alone in it -- upstream x86
// recRSQRThelper1 already took Fs's. Capture case 59, rsqrt(+0, -0), is the row
// that separates the two rules;
// EeRecFpuRsqrt.ZeroDivisorSignComesFromTheDividend pins it as well.
//
// The DIV rows are carried along as a control. DIV's sign(Fs ^ Ft) is already
// correct in both engines and both modes, and it must stay that way -- these
// two ops sit next to each other in every emitter and share a helper in the
// interpreter (checkDivideByZero), so a fix aimed at RSQRT that also moves DIV
// has broken something that was right. If a DIV row fails here, the fix is
// wrong regardless of what the RSQRT rows do.
// ---------------------------------------------------------------------------
TEST(EeFpuZeroDivisorConsole, FullClampModeBothEnginesMatchConsoleSign)
{
	for (int i = 0; i < kRowCount; ++i)
	{
		const ZeroDivisorRow& r = kRows[i];
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(::testing::Message()
				<< r.what << (jit ? " [jit]" : " [interp]") << " FULL, sign only"
				<< (r.rsqrt ? "" : "  (DIV control: must not regress)"));
			EXPECT_EQ(RunRow(r, jit != 0, /*full_mode=*/true) >> 31,
				r.console >> 31);
		}
	}
}
