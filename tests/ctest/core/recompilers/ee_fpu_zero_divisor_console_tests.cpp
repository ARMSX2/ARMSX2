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

// What the fast path produces instead: +FLT_MAX, and RSQRT's sign off Ft.
u32 FastPathValue(const ZeroDivisorRow& r)
{
	const u32 sign = r.rsqrt ? (r.ft & 0x80000000u)
	                         : ((r.fs ^ r.ft) & 0x80000000u);
	return sign | 0x7F7FFFFFu;
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
// The compromise, pinned: both engines, default clamp mode, +/-FLT_MAX with
// RSQRT's sign off Ft. Asserting the non-console value is the point -- it stops
// a later attempt at the tripwires below from changing the mode nearly every
// game runs in.
//
// Disabled for now: the interpreter still reads DIV/RSQRT operands through
// fpuDouble() and saturates at FLT_MAX, so it cannot agree with the fast path
// on this row yet. Re-enabled by "Fix: DIV.S and RSQRT.S, the last two ops
// holding the operand clamp".
// ---------------------------------------------------------------------------
TEST(EeFpuZeroDivisorConsole, DISABLED_DefaultClampModeSaturatesToFltMaxOnBothEngines)
{
	ASSERT_FALSE(EmuConfig.Cpu.Recompiler.fpuFullMode)
		<< "this test describes the NON-full path; something enabled FULL mode";

	for (int i = 0; i < kRowCount; ++i)
	{
		const ZeroDivisorRow& r = kRows[i];
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(::testing::Message()
				<< r.what << (jit ? " [jit]" : " [interp]"));
			EXPECT_EQ(RunRow(r, jit != 0, /*full_mode=*/false), FastPathValue(r));
		}
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
// Tripwire 1 of 2, magnitude. The console saturates to 0x7FFFFFFF; the
// interpreter saturates to 0x7F7FFFFF in every mode, including the FULL mode
// where the JIT beside it produces the console value. Sign is masked off;
// tripwire 2 owns that.
//
// Force-enable to see the current state. Expected today: all ten [jit] rows
// pass, all ten [interp] rows fail with 7f7fffff against 7fffffff.
//
// Graduating it means giving the interpreter a FULL path, not changing posFmax.
// ---------------------------------------------------------------------------
TEST(EeFpuZeroDivisorConsole, DISABLED_FullClampModeInterpMissesConsoleSaturation)
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
// Tripwire 2 of 2, sign. RSQRT's zero-divisor result takes its sign from Fs on
// the console; both engines' fast path takes it from Ft, and the JIT's FULL
// path gets it right. Magnitude is masked off; tripwire 1 owns that.
//
// The DIV rows ride along as a control: DIV's sign(Fs ^ Ft) is already correct
// in both engines and both modes, and these two ops sit next to each other in
// every emitter and share checkDivideByZero in the interpreter, so a DIV row
// failing here means the fix broke something that worked.
//
// Force-enable to see the current state. Expected today: all five DIV rows pass
// on both engines, all five [jit] RSQRT rows pass, and the two [interp] RSQRT
// rows whose Fs and Ft signs differ fail -- +0/sqrt(-0) and -0/sqrt(+0).
// ---------------------------------------------------------------------------
TEST(EeFpuZeroDivisorConsole, DISABLED_FullClampModeInterpRsqrtSignFollowsFtNotFs)
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
