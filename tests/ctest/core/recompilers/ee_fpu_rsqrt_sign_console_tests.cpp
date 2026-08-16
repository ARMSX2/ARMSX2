// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// RSQRT.S and the divisor's sign bit, against silicon.
//
// Why these operands. A divisor that is negative and has a zero exponent field
// is the only class in which the EE's two divide-unit causes can co-occur:
// SQRT.S raises only I, DIV.S splits I and D on the dividend, and RSQRT.S's
// I-from-a-negative-divisor and D-from-a-zero-divisor cannot both fire
// otherwise. The 8137-case corpus has no row there, so "the console never sets
// both" was an empty cell rather than a measurement. It sets both.
//
// Captured on SCPH-90000 (FCR0 0x2e40) over ps2link, 772 cases, two
// byte-identical runs; 57 of the operand triples are also in corpus v4's
// console column, taken by a different guest program, and agree on result and
// FCR31 on all 57. Probe and decoder in the session archive under
// captures/rsqzero/.
//
// The 40 rows that separate the two candidate models are rsqrt of a nonzero
// dividend by one of {-0, -dmin, -dmid, -dmax}.

#include "harness/EeRecTestHarness.h"

#include "Config.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kI = 0x00020000u, kD = 0x00010000u, kSI = 0x40u, kSD = 0x20u;
constexpr u32 kCauseSticky = kI | kD | kSI | kSD;

enum RsqOp { kRsqrt, kDiv, kSqrt, kSqrtDiv };

// `console` and `flags` are the hardware words. `seed` is the FCR31 pre-state
// the probe wrote through CTC1 before the op, which is what pins the clear: a
// seeded I|D must be gone from the answer unless the op raises it again, while
// the sticky pair it came with must survive.
struct Row
{
	RsqOp op;
	u32 fs, ft, seed;
	u32 console, flags;
	const char* what;
};

constexpr Row kRows[] = {
	{kRsqrt, 0x3F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 1.0 / +0"},
	{kDiv, 0x3F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 1.0 / +0"},
	{kRsqrt, 0xBF800000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "rsqrt -1.0 / +0"},
	{kDiv, 0xBF800000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div -1.0 / +0"},
	{kRsqrt, 0x00000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +0 / +0"},
	{kDiv, 0x00000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +0 / +0"},
	{kRsqrt, 0x80000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -0 / +0"},
	{kDiv, 0x80000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div -0 / +0"},
	{kRsqrt, 0x00000001u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmin / +0"},
	{kDiv, 0x00000001u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +dmin / +0"},
	{kRsqrt, 0x80000001u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -dmin / +0"},
	{kDiv, 0x80000001u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div -dmin / +0"},
	{kRsqrt, 0x007FFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmax / +0"},
	{kDiv, 0x007FFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +dmax / +0"},
	{kRsqrt, 0x41200000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 10.0 / +0"},
	{kDiv, 0x41200000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 10.0 / +0"},
	{kRsqrt, 0x7FFFFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt EEMAX / +0"},
	{kDiv, 0x7FFFFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div EEMAX / +0"},
	{kRsqrt, 0x7F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 2^128 / +0"},
	{kDiv, 0x7F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 2^128 / +0"},
	{kSqrt, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, "sqrt +0"},
	{kRsqrt, 0x3F800000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 1.0 / -0"},
	{kDiv, 0x3F800000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 1.0 / -0"},
	{kRsqrt, 0xBF800000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00030060u, "rsqrt -1.0 / -0"},
	{kDiv, 0xBF800000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div -1.0 / -0"},
	{kRsqrt, 0x00000000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +0 / -0"},
	{kDiv, 0x00000000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +0 / -0"},
	{kRsqrt, 0x80000000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -0 / -0"},
	{kDiv, 0x80000000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div -0 / -0"},
	{kRsqrt, 0x00000001u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmin / -0"},
	{kDiv, 0x00000001u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +dmin / -0"},
	{kRsqrt, 0x80000001u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -dmin / -0"},
	{kDiv, 0x80000001u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div -dmin / -0"},
	{kRsqrt, 0x007FFFFFu, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmax / -0"},
	{kDiv, 0x007FFFFFu, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +dmax / -0"},
	{kRsqrt, 0x41200000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 10.0 / -0"},
	{kDiv, 0x41200000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 10.0 / -0"},
	{kRsqrt, 0x7FFFFFFFu, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt EEMAX / -0"},
	{kDiv, 0x7FFFFFFFu, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div EEMAX / -0"},
	{kRsqrt, 0x7F800000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 2^128 / -0"},
	{kDiv, 0x7F800000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 2^128 / -0"},
	{kSqrt, 0x00000000u, 0x80000000u, 0x00000000u, 0x00000000u, 0x00020040u, "sqrt -0"},
	{kRsqrt, 0x3F800000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 1.0 / +dmin"},
	{kDiv, 0x3F800000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 1.0 / +dmin"},
	{kRsqrt, 0xBF800000u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "rsqrt -1.0 / +dmin"},
	{kDiv, 0xBF800000u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div -1.0 / +dmin"},
	{kRsqrt, 0x00000000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +0 / +dmin"},
	{kDiv, 0x00000000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +0 / +dmin"},
	{kRsqrt, 0x80000000u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -0 / +dmin"},
	{kDiv, 0x80000000u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div -0 / +dmin"},
	{kRsqrt, 0x00000001u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmin / +dmin"},
	{kDiv, 0x00000001u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +dmin / +dmin"},
	{kRsqrt, 0x80000001u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -dmin / +dmin"},
	{kDiv, 0x80000001u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div -dmin / +dmin"},
	{kRsqrt, 0x007FFFFFu, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmax / +dmin"},
	{kDiv, 0x007FFFFFu, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +dmax / +dmin"},
	{kRsqrt, 0x41200000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 10.0 / +dmin"},
	{kDiv, 0x41200000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 10.0 / +dmin"},
	{kRsqrt, 0x7FFFFFFFu, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt EEMAX / +dmin"},
	{kDiv, 0x7FFFFFFFu, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div EEMAX / +dmin"},
	{kRsqrt, 0x7F800000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 2^128 / +dmin"},
	{kDiv, 0x7F800000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 2^128 / +dmin"},
	{kSqrt, 0x00000000u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, "sqrt +dmin"},
	{kRsqrt, 0x3F800000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 1.0 / -dmin"},
	{kDiv, 0x3F800000u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 1.0 / -dmin"},
	{kRsqrt, 0xBF800000u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00030060u, "rsqrt -1.0 / -dmin"},
	{kDiv, 0xBF800000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div -1.0 / -dmin"},
	{kRsqrt, 0x00000000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +0 / -dmin"},
	{kDiv, 0x00000000u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +0 / -dmin"},
	{kRsqrt, 0x80000000u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -0 / -dmin"},
	{kDiv, 0x80000000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div -0 / -dmin"},
	{kRsqrt, 0x00000001u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmin / -dmin"},
	{kDiv, 0x00000001u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +dmin / -dmin"},
	{kRsqrt, 0x80000001u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -dmin / -dmin"},
	{kDiv, 0x80000001u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div -dmin / -dmin"},
	{kRsqrt, 0x007FFFFFu, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmax / -dmin"},
	{kDiv, 0x007FFFFFu, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +dmax / -dmin"},
	{kRsqrt, 0x41200000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 10.0 / -dmin"},
	{kDiv, 0x41200000u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 10.0 / -dmin"},
	{kRsqrt, 0x7FFFFFFFu, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt EEMAX / -dmin"},
	{kDiv, 0x7FFFFFFFu, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div EEMAX / -dmin"},
	{kRsqrt, 0x7F800000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 2^128 / -dmin"},
	{kDiv, 0x7F800000u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 2^128 / -dmin"},
	{kSqrt, 0x00000000u, 0x80000001u, 0x00000000u, 0x00000000u, 0x00020040u, "sqrt -dmin"},
	{kRsqrt, 0x3F800000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 1.0 / +dmid"},
	{kDiv, 0x3F800000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 1.0 / +dmid"},
	{kRsqrt, 0xBF800000u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "rsqrt -1.0 / +dmid"},
	{kDiv, 0xBF800000u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div -1.0 / +dmid"},
	{kRsqrt, 0x00000000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +0 / +dmid"},
	{kDiv, 0x00000000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +0 / +dmid"},
	{kRsqrt, 0x80000000u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -0 / +dmid"},
	{kDiv, 0x80000000u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div -0 / +dmid"},
	{kRsqrt, 0x00000001u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmin / +dmid"},
	{kDiv, 0x00000001u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +dmin / +dmid"},
	{kRsqrt, 0x80000001u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -dmin / +dmid"},
	{kDiv, 0x80000001u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div -dmin / +dmid"},
	{kRsqrt, 0x007FFFFFu, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmax / +dmid"},
	{kDiv, 0x007FFFFFu, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +dmax / +dmid"},
	{kRsqrt, 0x41200000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 10.0 / +dmid"},
	{kDiv, 0x41200000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 10.0 / +dmid"},
	{kRsqrt, 0x7FFFFFFFu, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt EEMAX / +dmid"},
	{kDiv, 0x7FFFFFFFu, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div EEMAX / +dmid"},
	{kRsqrt, 0x7F800000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 2^128 / +dmid"},
	{kDiv, 0x7F800000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 2^128 / +dmid"},
	{kSqrt, 0x00000000u, 0x00400000u, 0x00000000u, 0x00000000u, 0x00000000u, "sqrt +dmid"},
	{kRsqrt, 0x3F800000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 1.0 / -dmid"},
	{kDiv, 0x3F800000u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 1.0 / -dmid"},
	{kRsqrt, 0xBF800000u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00030060u, "rsqrt -1.0 / -dmid"},
	{kDiv, 0xBF800000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div -1.0 / -dmid"},
	{kRsqrt, 0x00000000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +0 / -dmid"},
	{kDiv, 0x00000000u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +0 / -dmid"},
	{kRsqrt, 0x80000000u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -0 / -dmid"},
	{kDiv, 0x80000000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div -0 / -dmid"},
	{kRsqrt, 0x00000001u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmin / -dmid"},
	{kDiv, 0x00000001u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +dmin / -dmid"},
	{kRsqrt, 0x80000001u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -dmin / -dmid"},
	{kDiv, 0x80000001u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div -dmin / -dmid"},
	{kRsqrt, 0x007FFFFFu, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmax / -dmid"},
	{kDiv, 0x007FFFFFu, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +dmax / -dmid"},
	{kRsqrt, 0x41200000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 10.0 / -dmid"},
	{kDiv, 0x41200000u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 10.0 / -dmid"},
	{kRsqrt, 0x7FFFFFFFu, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt EEMAX / -dmid"},
	{kDiv, 0x7FFFFFFFu, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div EEMAX / -dmid"},
	{kRsqrt, 0x7F800000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 2^128 / -dmid"},
	{kDiv, 0x7F800000u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 2^128 / -dmid"},
	{kSqrt, 0x00000000u, 0x80400000u, 0x00000000u, 0x00000000u, 0x00020040u, "sqrt -dmid"},
	{kRsqrt, 0x3F800000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 1.0 / +dmax"},
	{kDiv, 0x3F800000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 1.0 / +dmax"},
	{kRsqrt, 0xBF800000u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "rsqrt -1.0 / +dmax"},
	{kDiv, 0xBF800000u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div -1.0 / +dmax"},
	{kRsqrt, 0x00000000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +0 / +dmax"},
	{kDiv, 0x00000000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +0 / +dmax"},
	{kRsqrt, 0x80000000u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -0 / +dmax"},
	{kDiv, 0x80000000u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div -0 / +dmax"},
	{kRsqrt, 0x00000001u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmin / +dmax"},
	{kDiv, 0x00000001u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +dmin / +dmax"},
	{kRsqrt, 0x80000001u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -dmin / +dmax"},
	{kDiv, 0x80000001u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div -dmin / +dmax"},
	{kRsqrt, 0x007FFFFFu, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmax / +dmax"},
	{kDiv, 0x007FFFFFu, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div +dmax / +dmax"},
	{kRsqrt, 0x41200000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 10.0 / +dmax"},
	{kDiv, 0x41200000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 10.0 / +dmax"},
	{kRsqrt, 0x7FFFFFFFu, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt EEMAX / +dmax"},
	{kDiv, 0x7FFFFFFFu, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div EEMAX / +dmax"},
	{kRsqrt, 0x7F800000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "rsqrt 2^128 / +dmax"},
	{kDiv, 0x7F800000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div 2^128 / +dmax"},
	{kSqrt, 0x00000000u, 0x007FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "sqrt +dmax"},
	{kRsqrt, 0x3F800000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 1.0 / -dmax"},
	{kDiv, 0x3F800000u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 1.0 / -dmax"},
	{kRsqrt, 0xBF800000u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00030060u, "rsqrt -1.0 / -dmax"},
	{kDiv, 0xBF800000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "div -1.0 / -dmax"},
	{kRsqrt, 0x00000000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +0 / -dmax"},
	{kDiv, 0x00000000u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +0 / -dmax"},
	{kRsqrt, 0x80000000u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -0 / -dmax"},
	{kDiv, 0x80000000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div -0 / -dmax"},
	{kRsqrt, 0x00000001u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmin / -dmax"},
	{kDiv, 0x00000001u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +dmin / -dmax"},
	{kRsqrt, 0x80000001u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "rsqrt -dmin / -dmax"},
	{kDiv, 0x80000001u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "div -dmin / -dmax"},
	{kRsqrt, 0x007FFFFFu, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt +dmax / -dmax"},
	{kDiv, 0x007FFFFFu, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "div +dmax / -dmax"},
	{kRsqrt, 0x41200000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 10.0 / -dmax"},
	{kDiv, 0x41200000u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 10.0 / -dmax"},
	{kRsqrt, 0x7FFFFFFFu, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt EEMAX / -dmax"},
	{kDiv, 0x7FFFFFFFu, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div EEMAX / -dmax"},
	{kRsqrt, 0x7F800000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 2^128 / -dmax"},
	{kDiv, 0x7F800000u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "div 2^128 / -dmax"},
	{kSqrt, 0x00000000u, 0x807FFFFFu, 0x00000000u, 0x00000000u, 0x00020040u, "sqrt -dmax"},
	{kRsqrt, 0x3F800000u, 0x00800000u, 0x00000000u, 0x5F000000u, 0x00000000u, "rsqrt 1.0 / +minnorm"},
	{kDiv, 0x3F800000u, 0x00800000u, 0x00000000u, 0x7E800000u, 0x00000000u, "div 1.0 / +minnorm"},
	{kRsqrt, 0xBF800000u, 0x00800000u, 0x00000000u, 0xDF000000u, 0x00000000u, "rsqrt -1.0 / +minnorm"},
	{kDiv, 0xBF800000u, 0x00800000u, 0x00000000u, 0xFE800000u, 0x00000000u, "div -1.0 / +minnorm"},
	{kRsqrt, 0x00000000u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +0 / +minnorm"},
	{kDiv, 0x00000000u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +0 / +minnorm"},
	{kRsqrt, 0x80000000u, 0x00800000u, 0x00000000u, 0x80000000u, 0x00000000u, "rsqrt -0 / +minnorm"},
	{kDiv, 0x80000000u, 0x00800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div -0 / +minnorm"},
	{kRsqrt, 0x00000001u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +dmin / +minnorm"},
	{kDiv, 0x00000001u, 0x00800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +dmin / +minnorm"},
	{kRsqrt, 0x80000001u, 0x00800000u, 0x00000000u, 0x80000000u, 0x00000000u, "rsqrt -dmin / +minnorm"},
	{kDiv, 0x80000001u, 0x00800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div -dmin / +minnorm"},
	{kRsqrt, 0x007FFFFFu, 0x00800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +dmax / +minnorm"},
	{kDiv, 0x007FFFFFu, 0x00800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +dmax / +minnorm"},
	{kRsqrt, 0x41200000u, 0x00800000u, 0x00000000u, 0x60A00000u, 0x00000000u, "rsqrt 10.0 / +minnorm"},
	{kDiv, 0x41200000u, 0x00800000u, 0x00000000u, 0x7FFFFFFFu, 0x00000000u, "div 10.0 / +minnorm"},
	{kRsqrt, 0x7FFFFFFFu, 0x00800000u, 0x00000000u, 0x7FFFFFFFu, 0x00000000u, "rsqrt EEMAX / +minnorm"},
	{kDiv, 0x7FFFFFFFu, 0x00800000u, 0x00000000u, 0x7FFFFFFFu, 0x00000000u, "div EEMAX / +minnorm"},
	{kRsqrt, 0x7F800000u, 0x00800000u, 0x00000000u, 0x7FFFFFFFu, 0x00000000u, "rsqrt 2^128 / +minnorm"},
	{kDiv, 0x7F800000u, 0x00800000u, 0x00000000u, 0x7FFFFFFFu, 0x00000000u, "div 2^128 / +minnorm"},
	{kSqrt, 0x00000000u, 0x00800000u, 0x00000000u, 0x20000000u, 0x00000000u, "sqrt +minnorm"},
	{kRsqrt, 0x3F800000u, 0x80800000u, 0x00000000u, 0x5F000000u, 0x00020040u, "rsqrt 1.0 / -minnorm"},
	{kDiv, 0x3F800000u, 0x80800000u, 0x00000000u, 0xFE800000u, 0x00000000u, "div 1.0 / -minnorm"},
	{kRsqrt, 0xBF800000u, 0x80800000u, 0x00000000u, 0xDF000000u, 0x00020040u, "rsqrt -1.0 / -minnorm"},
	{kDiv, 0xBF800000u, 0x80800000u, 0x00000000u, 0x7E800000u, 0x00000000u, "div -1.0 / -minnorm"},
	{kRsqrt, 0x00000000u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +0 / -minnorm"},
	{kDiv, 0x00000000u, 0x80800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +0 / -minnorm"},
	{kRsqrt, 0x80000000u, 0x80800000u, 0x00000000u, 0x80000000u, 0x00020040u, "rsqrt -0 / -minnorm"},
	{kDiv, 0x80000000u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div -0 / -minnorm"},
	{kRsqrt, 0x00000001u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +dmin / -minnorm"},
	{kDiv, 0x00000001u, 0x80800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +dmin / -minnorm"},
	{kRsqrt, 0x80000001u, 0x80800000u, 0x00000000u, 0x80000000u, 0x00020040u, "rsqrt -dmin / -minnorm"},
	{kDiv, 0x80000001u, 0x80800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div -dmin / -minnorm"},
	{kRsqrt, 0x007FFFFFu, 0x80800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +dmax / -minnorm"},
	{kDiv, 0x007FFFFFu, 0x80800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +dmax / -minnorm"},
	{kRsqrt, 0x41200000u, 0x80800000u, 0x00000000u, 0x60A00000u, 0x00020040u, "rsqrt 10.0 / -minnorm"},
	{kDiv, 0x41200000u, 0x80800000u, 0x00000000u, 0xFFFFFFFFu, 0x00000000u, "div 10.0 / -minnorm"},
	{kRsqrt, 0x7FFFFFFFu, 0x80800000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt EEMAX / -minnorm"},
	{kDiv, 0x7FFFFFFFu, 0x80800000u, 0x00000000u, 0xFFFFFFFFu, 0x00000000u, "div EEMAX / -minnorm"},
	{kRsqrt, 0x7F800000u, 0x80800000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt 2^128 / -minnorm"},
	{kDiv, 0x7F800000u, 0x80800000u, 0x00000000u, 0xFFFFFFFFu, 0x00000000u, "div 2^128 / -minnorm"},
	{kSqrt, 0x00000000u, 0x80800000u, 0x00000000u, 0x20000000u, 0x00020040u, "sqrt -minnorm"},
	{kRsqrt, 0x3F800000u, 0x3F800000u, 0x00000000u, 0x3F800000u, 0x00000000u, "rsqrt 1.0 / 1.0"},
	{kDiv, 0x3F800000u, 0x3F800000u, 0x00000000u, 0x3F800000u, 0x00000000u, "div 1.0 / 1.0"},
	{kRsqrt, 0xBF800000u, 0x3F800000u, 0x00000000u, 0xBF800000u, 0x00000000u, "rsqrt -1.0 / 1.0"},
	{kDiv, 0xBF800000u, 0x3F800000u, 0x00000000u, 0xBF800000u, 0x00000000u, "div -1.0 / 1.0"},
	{kRsqrt, 0x00000000u, 0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +0 / 1.0"},
	{kDiv, 0x00000000u, 0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +0 / 1.0"},
	{kRsqrt, 0x80000000u, 0x3F800000u, 0x00000000u, 0x80000000u, 0x00000000u, "rsqrt -0 / 1.0"},
	{kDiv, 0x80000000u, 0x3F800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div -0 / 1.0"},
	{kRsqrt, 0x00000001u, 0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +dmin / 1.0"},
	{kDiv, 0x00000001u, 0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +dmin / 1.0"},
	{kRsqrt, 0x80000001u, 0x3F800000u, 0x00000000u, 0x80000000u, 0x00000000u, "rsqrt -dmin / 1.0"},
	{kDiv, 0x80000001u, 0x3F800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div -dmin / 1.0"},
	{kRsqrt, 0x007FFFFFu, 0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +dmax / 1.0"},
	{kDiv, 0x007FFFFFu, 0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +dmax / 1.0"},
	{kRsqrt, 0x41200000u, 0x3F800000u, 0x00000000u, 0x41200000u, 0x00000000u, "rsqrt 10.0 / 1.0"},
	{kDiv, 0x41200000u, 0x3F800000u, 0x00000000u, 0x41200000u, 0x00000000u, "div 10.0 / 1.0"},
	{kRsqrt, 0x7FFFFFFFu, 0x3F800000u, 0x00000000u, 0x7FFFFFFFu, 0x00000000u, "rsqrt EEMAX / 1.0"},
	{kDiv, 0x7FFFFFFFu, 0x3F800000u, 0x00000000u, 0x7FFFFFFFu, 0x00000000u, "div EEMAX / 1.0"},
	{kRsqrt, 0x7F800000u, 0x3F800000u, 0x00000000u, 0x7F800000u, 0x00000000u, "rsqrt 2^128 / 1.0"},
	{kDiv, 0x7F800000u, 0x3F800000u, 0x00000000u, 0x7F800000u, 0x00000000u, "div 2^128 / 1.0"},
	{kSqrt, 0x00000000u, 0x3F800000u, 0x00000000u, 0x3F800000u, 0x00000000u, "sqrt 1.0"},
	{kRsqrt, 0x3F800000u, 0xBF800000u, 0x00000000u, 0x3F800000u, 0x00020040u, "rsqrt 1.0 / -1.0"},
	{kDiv, 0x3F800000u, 0xBF800000u, 0x00000000u, 0xBF800000u, 0x00000000u, "div 1.0 / -1.0"},
	{kRsqrt, 0xBF800000u, 0xBF800000u, 0x00000000u, 0xBF800000u, 0x00020040u, "rsqrt -1.0 / -1.0"},
	{kDiv, 0xBF800000u, 0xBF800000u, 0x00000000u, 0x3F800000u, 0x00000000u, "div -1.0 / -1.0"},
	{kRsqrt, 0x00000000u, 0xBF800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +0 / -1.0"},
	{kDiv, 0x00000000u, 0xBF800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +0 / -1.0"},
	{kRsqrt, 0x80000000u, 0xBF800000u, 0x00000000u, 0x80000000u, 0x00020040u, "rsqrt -0 / -1.0"},
	{kDiv, 0x80000000u, 0xBF800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div -0 / -1.0"},
	{kRsqrt, 0x00000001u, 0xBF800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +dmin / -1.0"},
	{kDiv, 0x00000001u, 0xBF800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +dmin / -1.0"},
	{kRsqrt, 0x80000001u, 0xBF800000u, 0x00000000u, 0x80000000u, 0x00020040u, "rsqrt -dmin / -1.0"},
	{kDiv, 0x80000001u, 0xBF800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div -dmin / -1.0"},
	{kRsqrt, 0x007FFFFFu, 0xBF800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +dmax / -1.0"},
	{kDiv, 0x007FFFFFu, 0xBF800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +dmax / -1.0"},
	{kRsqrt, 0x41200000u, 0xBF800000u, 0x00000000u, 0x41200000u, 0x00020040u, "rsqrt 10.0 / -1.0"},
	{kDiv, 0x41200000u, 0xBF800000u, 0x00000000u, 0xC1200000u, 0x00000000u, "div 10.0 / -1.0"},
	{kRsqrt, 0x7FFFFFFFu, 0xBF800000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "rsqrt EEMAX / -1.0"},
	{kDiv, 0x7FFFFFFFu, 0xBF800000u, 0x00000000u, 0xFFFFFFFFu, 0x00000000u, "div EEMAX / -1.0"},
	{kRsqrt, 0x7F800000u, 0xBF800000u, 0x00000000u, 0x7F800000u, 0x00020040u, "rsqrt 2^128 / -1.0"},
	{kDiv, 0x7F800000u, 0xBF800000u, 0x00000000u, 0xFF800000u, 0x00000000u, "div 2^128 / -1.0"},
	{kSqrt, 0x00000000u, 0xBF800000u, 0x00000000u, 0x3F800000u, 0x00020040u, "sqrt -1.0"},
	{kRsqrt, 0x3F800000u, 0x40800000u, 0x00000000u, 0x3F000000u, 0x00000000u, "rsqrt 1.0 / 4.0"},
	{kDiv, 0x3F800000u, 0x40800000u, 0x00000000u, 0x3E800000u, 0x00000000u, "div 1.0 / 4.0"},
	{kRsqrt, 0xBF800000u, 0x40800000u, 0x00000000u, 0xBF000000u, 0x00000000u, "rsqrt -1.0 / 4.0"},
	{kDiv, 0xBF800000u, 0x40800000u, 0x00000000u, 0xBE800000u, 0x00000000u, "div -1.0 / 4.0"},
	{kRsqrt, 0x00000000u, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +0 / 4.0"},
	{kDiv, 0x00000000u, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +0 / 4.0"},
	{kRsqrt, 0x80000000u, 0x40800000u, 0x00000000u, 0x80000000u, 0x00000000u, "rsqrt -0 / 4.0"},
	{kDiv, 0x80000000u, 0x40800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div -0 / 4.0"},
	{kRsqrt, 0x00000001u, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +dmin / 4.0"},
	{kDiv, 0x00000001u, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +dmin / 4.0"},
	{kRsqrt, 0x80000001u, 0x40800000u, 0x00000000u, 0x80000000u, 0x00000000u, "rsqrt -dmin / 4.0"},
	{kDiv, 0x80000001u, 0x40800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div -dmin / 4.0"},
	{kRsqrt, 0x007FFFFFu, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +dmax / 4.0"},
	{kDiv, 0x007FFFFFu, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div +dmax / 4.0"},
	{kRsqrt, 0x41200000u, 0x40800000u, 0x00000000u, 0x40A00000u, 0x00000000u, "rsqrt 10.0 / 4.0"},
	{kDiv, 0x41200000u, 0x40800000u, 0x00000000u, 0x40200000u, 0x00000000u, "div 10.0 / 4.0"},
	{kRsqrt, 0x7FFFFFFFu, 0x40800000u, 0x00000000u, 0x7F7FFFFFu, 0x00000000u, "rsqrt EEMAX / 4.0"},
	{kDiv, 0x7FFFFFFFu, 0x40800000u, 0x00000000u, 0x7EFFFFFFu, 0x00000000u, "div EEMAX / 4.0"},
	{kRsqrt, 0x7F800000u, 0x40800000u, 0x00000000u, 0x7F000000u, 0x00000000u, "rsqrt 2^128 / 4.0"},
	{kDiv, 0x7F800000u, 0x40800000u, 0x00000000u, 0x7E800000u, 0x00000000u, "div 2^128 / 4.0"},
	{kSqrt, 0x00000000u, 0x40800000u, 0x00000000u, 0x40000000u, 0x00000000u, "sqrt 4.0"},
	{kRsqrt, 0x3F800000u, 0xC0800000u, 0x00000000u, 0x3F000000u, 0x00020040u, "rsqrt 1.0 / -4.0"},
	{kDiv, 0x3F800000u, 0xC0800000u, 0x00000000u, 0xBE800000u, 0x00000000u, "div 1.0 / -4.0"},
	{kRsqrt, 0xBF800000u, 0xC0800000u, 0x00000000u, 0xBF000000u, 0x00020040u, "rsqrt -1.0 / -4.0"},
	{kDiv, 0xBF800000u, 0xC0800000u, 0x00000000u, 0x3E800000u, 0x00000000u, "div -1.0 / -4.0"},
	{kRsqrt, 0x00000000u, 0xC0800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +0 / -4.0"},
	{kDiv, 0x00000000u, 0xC0800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +0 / -4.0"},
	{kRsqrt, 0x80000000u, 0xC0800000u, 0x00000000u, 0x80000000u, 0x00020040u, "rsqrt -0 / -4.0"},
	{kDiv, 0x80000000u, 0xC0800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div -0 / -4.0"},
	{kRsqrt, 0x00000001u, 0xC0800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +dmin / -4.0"},
	{kDiv, 0x00000001u, 0xC0800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +dmin / -4.0"},
	{kRsqrt, 0x80000001u, 0xC0800000u, 0x00000000u, 0x80000000u, 0x00020040u, "rsqrt -dmin / -4.0"},
	{kDiv, 0x80000001u, 0xC0800000u, 0x00000000u, 0x00000000u, 0x00000000u, "div -dmin / -4.0"},
	{kRsqrt, 0x007FFFFFu, 0xC0800000u, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +dmax / -4.0"},
	{kDiv, 0x007FFFFFu, 0xC0800000u, 0x00000000u, 0x80000000u, 0x00000000u, "div +dmax / -4.0"},
	{kRsqrt, 0x41200000u, 0xC0800000u, 0x00000000u, 0x40A00000u, 0x00020040u, "rsqrt 10.0 / -4.0"},
	{kDiv, 0x41200000u, 0xC0800000u, 0x00000000u, 0xC0200000u, 0x00000000u, "div 10.0 / -4.0"},
	{kRsqrt, 0x7FFFFFFFu, 0xC0800000u, 0x00000000u, 0x7F7FFFFFu, 0x00020040u, "rsqrt EEMAX / -4.0"},
	{kDiv, 0x7FFFFFFFu, 0xC0800000u, 0x00000000u, 0xFEFFFFFFu, 0x00000000u, "div EEMAX / -4.0"},
	{kRsqrt, 0x7F800000u, 0xC0800000u, 0x00000000u, 0x7F000000u, 0x00020040u, "rsqrt 2^128 / -4.0"},
	{kDiv, 0x7F800000u, 0xC0800000u, 0x00000000u, 0xFE800000u, 0x00000000u, "div 2^128 / -4.0"},
	{kSqrt, 0x00000000u, 0xC0800000u, 0x00000000u, 0x40000000u, 0x00020040u, "sqrt -4.0"},
	{kRsqrt, 0x3F800000u, 0x7F7FFFFFu, 0x00000000u, 0x1F800001u, 0x00000000u, "rsqrt 1.0 / +FLT_MAX"},
	{kDiv, 0x3F800000u, 0x7F7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "div 1.0 / +FLT_MAX"},
	{kRsqrt, 0xBF800000u, 0x7F7FFFFFu, 0x00000000u, 0x9F800001u, 0x00000000u, "rsqrt -1.0 / +FLT_MAX"},
	{kDiv, 0xBF800000u, 0x7F7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "div -1.0 / +FLT_MAX"},
	{kRsqrt, 0x00000000u, 0x7F7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +0 / +FLT_MAX"},
	{kDiv, 0x00000000u, 0x7F7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "div +0 / +FLT_MAX"},
	{kRsqrt, 0x80000000u, 0x7F7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "rsqrt -0 / +FLT_MAX"},
	{kDiv, 0x80000000u, 0x7F7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "div -0 / +FLT_MAX"},
	{kRsqrt, 0x00000001u, 0x7F7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +dmin / +FLT_MAX"},
	{kDiv, 0x00000001u, 0x7F7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "div +dmin / +FLT_MAX"},
	{kRsqrt, 0x80000001u, 0x7F7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "rsqrt -dmin / +FLT_MAX"},
	{kDiv, 0x80000001u, 0x7F7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "div -dmin / +FLT_MAX"},
	{kRsqrt, 0x007FFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "rsqrt +dmax / +FLT_MAX"},
	{kDiv, 0x007FFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "div +dmax / +FLT_MAX"},
	{kRsqrt, 0x41200000u, 0x7F7FFFFFu, 0x00000000u, 0x21200001u, 0x00000000u, "rsqrt 10.0 / +FLT_MAX"},
	{kDiv, 0x41200000u, 0x7F7FFFFFu, 0x00000000u, 0x01200001u, 0x00000000u, "div 10.0 / +FLT_MAX"},
	{kRsqrt, 0x7FFFFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x60000000u, 0x00000000u, "rsqrt EEMAX / +FLT_MAX"},
	{kDiv, 0x7FFFFFFFu, 0x7F7FFFFFu, 0x00000000u, 0x40000000u, 0x00000000u, "div EEMAX / +FLT_MAX"},
	{kRsqrt, 0x7F800000u, 0x7F7FFFFFu, 0x00000000u, 0x5F800001u, 0x00000000u, "rsqrt 2^128 / +FLT_MAX"},
	{kDiv, 0x7F800000u, 0x7F7FFFFFu, 0x00000000u, 0x3F800001u, 0x00000000u, "div 2^128 / +FLT_MAX"},
	{kSqrt, 0x00000000u, 0x7F7FFFFFu, 0x00000000u, 0x5F7FFFFFu, 0x00000000u, "sqrt +FLT_MAX"},
	{kRsqrt, 0x3F800000u, 0xFF7FFFFFu, 0x00000000u, 0x1F800001u, 0x00020040u, "rsqrt 1.0 / -FLT_MAX"},
	{kDiv, 0x3F800000u, 0xFF7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "div 1.0 / -FLT_MAX"},
	{kRsqrt, 0xBF800000u, 0xFF7FFFFFu, 0x00000000u, 0x9F800001u, 0x00020040u, "rsqrt -1.0 / -FLT_MAX"},
	{kDiv, 0xBF800000u, 0xFF7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "div -1.0 / -FLT_MAX"},
	{kRsqrt, 0x00000000u, 0xFF7FFFFFu, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +0 / -FLT_MAX"},
	{kDiv, 0x00000000u, 0xFF7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "div +0 / -FLT_MAX"},
	{kRsqrt, 0x80000000u, 0xFF7FFFFFu, 0x00000000u, 0x80000000u, 0x00020040u, "rsqrt -0 / -FLT_MAX"},
	{kDiv, 0x80000000u, 0xFF7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "div -0 / -FLT_MAX"},
	{kRsqrt, 0x00000001u, 0xFF7FFFFFu, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +dmin / -FLT_MAX"},
	{kDiv, 0x00000001u, 0xFF7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "div +dmin / -FLT_MAX"},
	{kRsqrt, 0x80000001u, 0xFF7FFFFFu, 0x00000000u, 0x80000000u, 0x00020040u, "rsqrt -dmin / -FLT_MAX"},
	{kDiv, 0x80000001u, 0xFF7FFFFFu, 0x00000000u, 0x00000000u, 0x00000000u, "div -dmin / -FLT_MAX"},
	{kRsqrt, 0x007FFFFFu, 0xFF7FFFFFu, 0x00000000u, 0x00000000u, 0x00020040u, "rsqrt +dmax / -FLT_MAX"},
	{kDiv, 0x007FFFFFu, 0xFF7FFFFFu, 0x00000000u, 0x80000000u, 0x00000000u, "div +dmax / -FLT_MAX"},
	{kRsqrt, 0x41200000u, 0xFF7FFFFFu, 0x00000000u, 0x21200001u, 0x00020040u, "rsqrt 10.0 / -FLT_MAX"},
	{kDiv, 0x41200000u, 0xFF7FFFFFu, 0x00000000u, 0x81200001u, 0x00000000u, "div 10.0 / -FLT_MAX"},
	{kRsqrt, 0x7FFFFFFFu, 0xFF7FFFFFu, 0x00000000u, 0x60000000u, 0x00020040u, "rsqrt EEMAX / -FLT_MAX"},
	{kDiv, 0x7FFFFFFFu, 0xFF7FFFFFu, 0x00000000u, 0xC0000000u, 0x00000000u, "div EEMAX / -FLT_MAX"},
	{kRsqrt, 0x7F800000u, 0xFF7FFFFFu, 0x00000000u, 0x5F800001u, 0x00020040u, "rsqrt 2^128 / -FLT_MAX"},
	{kDiv, 0x7F800000u, 0xFF7FFFFFu, 0x00000000u, 0xBF800001u, 0x00000000u, "div 2^128 / -FLT_MAX"},
	{kSqrt, 0x00000000u, 0xFF7FFFFFu, 0x00000000u, 0x5F7FFFFFu, 0x00020040u, "sqrt -FLT_MAX"},
	{kRsqrt, 0x3F800000u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 1.0 / +0 [seeded]"},
	{kDiv, 0x3F800000u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 1.0 / +0 [seeded]"},
	{kRsqrt, 0xBF800000u, 0x00000000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "rsqrt -1.0 / +0 [seeded]"},
	{kDiv, 0xBF800000u, 0x00000000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div -1.0 / +0 [seeded]"},
	{kRsqrt, 0x00000000u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +0 / +0 [seeded]"},
	{kDiv, 0x00000000u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +0 / +0 [seeded]"},
	{kRsqrt, 0x80000000u, 0x00000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -0 / +0 [seeded]"},
	{kDiv, 0x80000000u, 0x00000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div -0 / +0 [seeded]"},
	{kRsqrt, 0x00000001u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmin / +0 [seeded]"},
	{kDiv, 0x00000001u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +dmin / +0 [seeded]"},
	{kRsqrt, 0x80000001u, 0x00000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -dmin / +0 [seeded]"},
	{kDiv, 0x80000001u, 0x00000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div -dmin / +0 [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmax / +0 [seeded]"},
	{kDiv, 0x007FFFFFu, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +dmax / +0 [seeded]"},
	{kRsqrt, 0x41200000u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 10.0 / +0 [seeded]"},
	{kDiv, 0x41200000u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 10.0 / +0 [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt EEMAX / +0 [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div EEMAX / +0 [seeded]"},
	{kRsqrt, 0x7F800000u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 2^128 / +0 [seeded]"},
	{kDiv, 0x7F800000u, 0x00000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 2^128 / +0 [seeded]"},
	{kSqrt, 0x00000000u, 0x00000000u, 0x00030060u, 0x00000000u, 0x00000060u, "sqrt +0 [seeded]"},
	{kRsqrt, 0x3F800000u, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 1.0 / -0 [seeded]"},
	{kDiv, 0x3F800000u, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 1.0 / -0 [seeded]"},
	{kRsqrt, 0xBF800000u, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00030060u, "rsqrt -1.0 / -0 [seeded]"},
	{kDiv, 0xBF800000u, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div -1.0 / -0 [seeded]"},
	{kRsqrt, 0x00000000u, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +0 / -0 [seeded]"},
	{kDiv, 0x00000000u, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +0 / -0 [seeded]"},
	{kRsqrt, 0x80000000u, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -0 / -0 [seeded]"},
	{kDiv, 0x80000000u, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div -0 / -0 [seeded]"},
	{kRsqrt, 0x00000001u, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmin / -0 [seeded]"},
	{kDiv, 0x00000001u, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +dmin / -0 [seeded]"},
	{kRsqrt, 0x80000001u, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -dmin / -0 [seeded]"},
	{kDiv, 0x80000001u, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div -dmin / -0 [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmax / -0 [seeded]"},
	{kDiv, 0x007FFFFFu, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +dmax / -0 [seeded]"},
	{kRsqrt, 0x41200000u, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 10.0 / -0 [seeded]"},
	{kDiv, 0x41200000u, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 10.0 / -0 [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt EEMAX / -0 [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div EEMAX / -0 [seeded]"},
	{kRsqrt, 0x7F800000u, 0x80000000u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 2^128 / -0 [seeded]"},
	{kDiv, 0x7F800000u, 0x80000000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 2^128 / -0 [seeded]"},
	{kSqrt, 0x00000000u, 0x80000000u, 0x00030060u, 0x00000000u, 0x00020060u, "sqrt -0 [seeded]"},
	{kRsqrt, 0x3F800000u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 1.0 / +dmin [seeded]"},
	{kDiv, 0x3F800000u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 1.0 / +dmin [seeded]"},
	{kRsqrt, 0xBF800000u, 0x00000001u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "rsqrt -1.0 / +dmin [seeded]"},
	{kDiv, 0xBF800000u, 0x00000001u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div -1.0 / +dmin [seeded]"},
	{kRsqrt, 0x00000000u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +0 / +dmin [seeded]"},
	{kDiv, 0x00000000u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +0 / +dmin [seeded]"},
	{kRsqrt, 0x80000000u, 0x00000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -0 / +dmin [seeded]"},
	{kDiv, 0x80000000u, 0x00000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div -0 / +dmin [seeded]"},
	{kRsqrt, 0x00000001u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmin / +dmin [seeded]"},
	{kDiv, 0x00000001u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +dmin / +dmin [seeded]"},
	{kRsqrt, 0x80000001u, 0x00000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -dmin / +dmin [seeded]"},
	{kDiv, 0x80000001u, 0x00000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div -dmin / +dmin [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmax / +dmin [seeded]"},
	{kDiv, 0x007FFFFFu, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +dmax / +dmin [seeded]"},
	{kRsqrt, 0x41200000u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 10.0 / +dmin [seeded]"},
	{kDiv, 0x41200000u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 10.0 / +dmin [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt EEMAX / +dmin [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div EEMAX / +dmin [seeded]"},
	{kRsqrt, 0x7F800000u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 2^128 / +dmin [seeded]"},
	{kDiv, 0x7F800000u, 0x00000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 2^128 / +dmin [seeded]"},
	{kSqrt, 0x00000000u, 0x00000001u, 0x00030060u, 0x00000000u, 0x00000060u, "sqrt +dmin [seeded]"},
	{kRsqrt, 0x3F800000u, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 1.0 / -dmin [seeded]"},
	{kDiv, 0x3F800000u, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 1.0 / -dmin [seeded]"},
	{kRsqrt, 0xBF800000u, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00030060u, "rsqrt -1.0 / -dmin [seeded]"},
	{kDiv, 0xBF800000u, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div -1.0 / -dmin [seeded]"},
	{kRsqrt, 0x00000000u, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +0 / -dmin [seeded]"},
	{kDiv, 0x00000000u, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +0 / -dmin [seeded]"},
	{kRsqrt, 0x80000000u, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -0 / -dmin [seeded]"},
	{kDiv, 0x80000000u, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div -0 / -dmin [seeded]"},
	{kRsqrt, 0x00000001u, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmin / -dmin [seeded]"},
	{kDiv, 0x00000001u, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +dmin / -dmin [seeded]"},
	{kRsqrt, 0x80000001u, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -dmin / -dmin [seeded]"},
	{kDiv, 0x80000001u, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div -dmin / -dmin [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmax / -dmin [seeded]"},
	{kDiv, 0x007FFFFFu, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +dmax / -dmin [seeded]"},
	{kRsqrt, 0x41200000u, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 10.0 / -dmin [seeded]"},
	{kDiv, 0x41200000u, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 10.0 / -dmin [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt EEMAX / -dmin [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div EEMAX / -dmin [seeded]"},
	{kRsqrt, 0x7F800000u, 0x80000001u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 2^128 / -dmin [seeded]"},
	{kDiv, 0x7F800000u, 0x80000001u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 2^128 / -dmin [seeded]"},
	{kSqrt, 0x00000000u, 0x80000001u, 0x00030060u, 0x00000000u, 0x00020060u, "sqrt -dmin [seeded]"},
	{kRsqrt, 0x3F800000u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 1.0 / +dmid [seeded]"},
	{kDiv, 0x3F800000u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 1.0 / +dmid [seeded]"},
	{kRsqrt, 0xBF800000u, 0x00400000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "rsqrt -1.0 / +dmid [seeded]"},
	{kDiv, 0xBF800000u, 0x00400000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div -1.0 / +dmid [seeded]"},
	{kRsqrt, 0x00000000u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +0 / +dmid [seeded]"},
	{kDiv, 0x00000000u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +0 / +dmid [seeded]"},
	{kRsqrt, 0x80000000u, 0x00400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -0 / +dmid [seeded]"},
	{kDiv, 0x80000000u, 0x00400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div -0 / +dmid [seeded]"},
	{kRsqrt, 0x00000001u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmin / +dmid [seeded]"},
	{kDiv, 0x00000001u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +dmin / +dmid [seeded]"},
	{kRsqrt, 0x80000001u, 0x00400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -dmin / +dmid [seeded]"},
	{kDiv, 0x80000001u, 0x00400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div -dmin / +dmid [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmax / +dmid [seeded]"},
	{kDiv, 0x007FFFFFu, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +dmax / +dmid [seeded]"},
	{kRsqrt, 0x41200000u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 10.0 / +dmid [seeded]"},
	{kDiv, 0x41200000u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 10.0 / +dmid [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt EEMAX / +dmid [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div EEMAX / +dmid [seeded]"},
	{kRsqrt, 0x7F800000u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 2^128 / +dmid [seeded]"},
	{kDiv, 0x7F800000u, 0x00400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 2^128 / +dmid [seeded]"},
	{kSqrt, 0x00000000u, 0x00400000u, 0x00030060u, 0x00000000u, 0x00000060u, "sqrt +dmid [seeded]"},
	{kRsqrt, 0x3F800000u, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 1.0 / -dmid [seeded]"},
	{kDiv, 0x3F800000u, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 1.0 / -dmid [seeded]"},
	{kRsqrt, 0xBF800000u, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00030060u, "rsqrt -1.0 / -dmid [seeded]"},
	{kDiv, 0xBF800000u, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div -1.0 / -dmid [seeded]"},
	{kRsqrt, 0x00000000u, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +0 / -dmid [seeded]"},
	{kDiv, 0x00000000u, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +0 / -dmid [seeded]"},
	{kRsqrt, 0x80000000u, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -0 / -dmid [seeded]"},
	{kDiv, 0x80000000u, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div -0 / -dmid [seeded]"},
	{kRsqrt, 0x00000001u, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmin / -dmid [seeded]"},
	{kDiv, 0x00000001u, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +dmin / -dmid [seeded]"},
	{kRsqrt, 0x80000001u, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -dmin / -dmid [seeded]"},
	{kDiv, 0x80000001u, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div -dmin / -dmid [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmax / -dmid [seeded]"},
	{kDiv, 0x007FFFFFu, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +dmax / -dmid [seeded]"},
	{kRsqrt, 0x41200000u, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 10.0 / -dmid [seeded]"},
	{kDiv, 0x41200000u, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 10.0 / -dmid [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt EEMAX / -dmid [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div EEMAX / -dmid [seeded]"},
	{kRsqrt, 0x7F800000u, 0x80400000u, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 2^128 / -dmid [seeded]"},
	{kDiv, 0x7F800000u, 0x80400000u, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 2^128 / -dmid [seeded]"},
	{kSqrt, 0x00000000u, 0x80400000u, 0x00030060u, 0x00000000u, 0x00020060u, "sqrt -dmid [seeded]"},
	{kRsqrt, 0x3F800000u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 1.0 / +dmax [seeded]"},
	{kDiv, 0x3F800000u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 1.0 / +dmax [seeded]"},
	{kRsqrt, 0xBF800000u, 0x007FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "rsqrt -1.0 / +dmax [seeded]"},
	{kDiv, 0xBF800000u, 0x007FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div -1.0 / +dmax [seeded]"},
	{kRsqrt, 0x00000000u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +0 / +dmax [seeded]"},
	{kDiv, 0x00000000u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +0 / +dmax [seeded]"},
	{kRsqrt, 0x80000000u, 0x007FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -0 / +dmax [seeded]"},
	{kDiv, 0x80000000u, 0x007FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div -0 / +dmax [seeded]"},
	{kRsqrt, 0x00000001u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmin / +dmax [seeded]"},
	{kDiv, 0x00000001u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +dmin / +dmax [seeded]"},
	{kRsqrt, 0x80000001u, 0x007FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -dmin / +dmax [seeded]"},
	{kDiv, 0x80000001u, 0x007FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div -dmin / +dmax [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmax / +dmax [seeded]"},
	{kDiv, 0x007FFFFFu, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div +dmax / +dmax [seeded]"},
	{kRsqrt, 0x41200000u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 10.0 / +dmax [seeded]"},
	{kDiv, 0x41200000u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 10.0 / +dmax [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt EEMAX / +dmax [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div EEMAX / +dmax [seeded]"},
	{kRsqrt, 0x7F800000u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "rsqrt 2^128 / +dmax [seeded]"},
	{kDiv, 0x7F800000u, 0x007FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div 2^128 / +dmax [seeded]"},
	{kSqrt, 0x00000000u, 0x007FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "sqrt +dmax [seeded]"},
	{kRsqrt, 0x3F800000u, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 1.0 / -dmax [seeded]"},
	{kDiv, 0x3F800000u, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 1.0 / -dmax [seeded]"},
	{kRsqrt, 0xBF800000u, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00030060u, "rsqrt -1.0 / -dmax [seeded]"},
	{kDiv, 0xBF800000u, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00010060u, "div -1.0 / -dmax [seeded]"},
	{kRsqrt, 0x00000000u, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +0 / -dmax [seeded]"},
	{kDiv, 0x00000000u, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +0 / -dmax [seeded]"},
	{kRsqrt, 0x80000000u, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -0 / -dmax [seeded]"},
	{kDiv, 0x80000000u, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div -0 / -dmax [seeded]"},
	{kRsqrt, 0x00000001u, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmin / -dmax [seeded]"},
	{kDiv, 0x00000001u, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +dmin / -dmax [seeded]"},
	{kRsqrt, 0x80000001u, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "rsqrt -dmin / -dmax [seeded]"},
	{kDiv, 0x80000001u, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "div -dmin / -dmax [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt +dmax / -dmax [seeded]"},
	{kDiv, 0x007FFFFFu, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00020060u, "div +dmax / -dmax [seeded]"},
	{kRsqrt, 0x41200000u, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 10.0 / -dmax [seeded]"},
	{kDiv, 0x41200000u, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 10.0 / -dmax [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt EEMAX / -dmax [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div EEMAX / -dmax [seeded]"},
	{kRsqrt, 0x7F800000u, 0x807FFFFFu, 0x00030060u, 0x7FFFFFFFu, 0x00030060u, "rsqrt 2^128 / -dmax [seeded]"},
	{kDiv, 0x7F800000u, 0x807FFFFFu, 0x00030060u, 0xFFFFFFFFu, 0x00010060u, "div 2^128 / -dmax [seeded]"},
	{kSqrt, 0x00000000u, 0x807FFFFFu, 0x00030060u, 0x00000000u, 0x00020060u, "sqrt -dmax [seeded]"},
	{kRsqrt, 0x3F800000u, 0x00800000u, 0x00030060u, 0x5F000000u, 0x00000060u, "rsqrt 1.0 / +minnorm [seeded]"},
	{kDiv, 0x3F800000u, 0x00800000u, 0x00030060u, 0x7E800000u, 0x00000060u, "div 1.0 / +minnorm [seeded]"},
	{kRsqrt, 0xBF800000u, 0x00800000u, 0x00030060u, 0xDF000000u, 0x00000060u, "rsqrt -1.0 / +minnorm [seeded]"},
	{kDiv, 0xBF800000u, 0x00800000u, 0x00030060u, 0xFE800000u, 0x00000060u, "div -1.0 / +minnorm [seeded]"},
	{kRsqrt, 0x00000000u, 0x00800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +0 / +minnorm [seeded]"},
	{kDiv, 0x00000000u, 0x00800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +0 / +minnorm [seeded]"},
	{kRsqrt, 0x80000000u, 0x00800000u, 0x00030060u, 0x80000000u, 0x00000060u, "rsqrt -0 / +minnorm [seeded]"},
	{kDiv, 0x80000000u, 0x00800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div -0 / +minnorm [seeded]"},
	{kRsqrt, 0x00000001u, 0x00800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +dmin / +minnorm [seeded]"},
	{kDiv, 0x00000001u, 0x00800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +dmin / +minnorm [seeded]"},
	{kRsqrt, 0x80000001u, 0x00800000u, 0x00030060u, 0x80000000u, 0x00000060u, "rsqrt -dmin / +minnorm [seeded]"},
	{kDiv, 0x80000001u, 0x00800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div -dmin / +minnorm [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x00800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +dmax / +minnorm [seeded]"},
	{kDiv, 0x007FFFFFu, 0x00800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +dmax / +minnorm [seeded]"},
	{kRsqrt, 0x41200000u, 0x00800000u, 0x00030060u, 0x60A00000u, 0x00000060u, "rsqrt 10.0 / +minnorm [seeded]"},
	{kDiv, 0x41200000u, 0x00800000u, 0x00030060u, 0x7FFFFFFFu, 0x00000060u, "div 10.0 / +minnorm [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x00800000u, 0x00030060u, 0x7FFFFFFFu, 0x00000060u, "rsqrt EEMAX / +minnorm [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x00800000u, 0x00030060u, 0x7FFFFFFFu, 0x00000060u, "div EEMAX / +minnorm [seeded]"},
	{kRsqrt, 0x7F800000u, 0x00800000u, 0x00030060u, 0x7FFFFFFFu, 0x00000060u, "rsqrt 2^128 / +minnorm [seeded]"},
	{kDiv, 0x7F800000u, 0x00800000u, 0x00030060u, 0x7FFFFFFFu, 0x00000060u, "div 2^128 / +minnorm [seeded]"},
	{kSqrt, 0x00000000u, 0x00800000u, 0x00030060u, 0x20000000u, 0x00000060u, "sqrt +minnorm [seeded]"},
	{kRsqrt, 0x3F800000u, 0x80800000u, 0x00030060u, 0x5F000000u, 0x00020060u, "rsqrt 1.0 / -minnorm [seeded]"},
	{kDiv, 0x3F800000u, 0x80800000u, 0x00030060u, 0xFE800000u, 0x00000060u, "div 1.0 / -minnorm [seeded]"},
	{kRsqrt, 0xBF800000u, 0x80800000u, 0x00030060u, 0xDF000000u, 0x00020060u, "rsqrt -1.0 / -minnorm [seeded]"},
	{kDiv, 0xBF800000u, 0x80800000u, 0x00030060u, 0x7E800000u, 0x00000060u, "div -1.0 / -minnorm [seeded]"},
	{kRsqrt, 0x00000000u, 0x80800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +0 / -minnorm [seeded]"},
	{kDiv, 0x00000000u, 0x80800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +0 / -minnorm [seeded]"},
	{kRsqrt, 0x80000000u, 0x80800000u, 0x00030060u, 0x80000000u, 0x00020060u, "rsqrt -0 / -minnorm [seeded]"},
	{kDiv, 0x80000000u, 0x80800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div -0 / -minnorm [seeded]"},
	{kRsqrt, 0x00000001u, 0x80800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +dmin / -minnorm [seeded]"},
	{kDiv, 0x00000001u, 0x80800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +dmin / -minnorm [seeded]"},
	{kRsqrt, 0x80000001u, 0x80800000u, 0x00030060u, 0x80000000u, 0x00020060u, "rsqrt -dmin / -minnorm [seeded]"},
	{kDiv, 0x80000001u, 0x80800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div -dmin / -minnorm [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x80800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +dmax / -minnorm [seeded]"},
	{kDiv, 0x007FFFFFu, 0x80800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +dmax / -minnorm [seeded]"},
	{kRsqrt, 0x41200000u, 0x80800000u, 0x00030060u, 0x60A00000u, 0x00020060u, "rsqrt 10.0 / -minnorm [seeded]"},
	{kDiv, 0x41200000u, 0x80800000u, 0x00030060u, 0xFFFFFFFFu, 0x00000060u, "div 10.0 / -minnorm [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x80800000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt EEMAX / -minnorm [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x80800000u, 0x00030060u, 0xFFFFFFFFu, 0x00000060u, "div EEMAX / -minnorm [seeded]"},
	{kRsqrt, 0x7F800000u, 0x80800000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt 2^128 / -minnorm [seeded]"},
	{kDiv, 0x7F800000u, 0x80800000u, 0x00030060u, 0xFFFFFFFFu, 0x00000060u, "div 2^128 / -minnorm [seeded]"},
	{kSqrt, 0x00000000u, 0x80800000u, 0x00030060u, 0x20000000u, 0x00020060u, "sqrt -minnorm [seeded]"},
	{kRsqrt, 0x3F800000u, 0x3F800000u, 0x00030060u, 0x3F800000u, 0x00000060u, "rsqrt 1.0 / 1.0 [seeded]"},
	{kDiv, 0x3F800000u, 0x3F800000u, 0x00030060u, 0x3F800000u, 0x00000060u, "div 1.0 / 1.0 [seeded]"},
	{kRsqrt, 0xBF800000u, 0x3F800000u, 0x00030060u, 0xBF800000u, 0x00000060u, "rsqrt -1.0 / 1.0 [seeded]"},
	{kDiv, 0xBF800000u, 0x3F800000u, 0x00030060u, 0xBF800000u, 0x00000060u, "div -1.0 / 1.0 [seeded]"},
	{kRsqrt, 0x00000000u, 0x3F800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +0 / 1.0 [seeded]"},
	{kDiv, 0x00000000u, 0x3F800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +0 / 1.0 [seeded]"},
	{kRsqrt, 0x80000000u, 0x3F800000u, 0x00030060u, 0x80000000u, 0x00000060u, "rsqrt -0 / 1.0 [seeded]"},
	{kDiv, 0x80000000u, 0x3F800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div -0 / 1.0 [seeded]"},
	{kRsqrt, 0x00000001u, 0x3F800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +dmin / 1.0 [seeded]"},
	{kDiv, 0x00000001u, 0x3F800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +dmin / 1.0 [seeded]"},
	{kRsqrt, 0x80000001u, 0x3F800000u, 0x00030060u, 0x80000000u, 0x00000060u, "rsqrt -dmin / 1.0 [seeded]"},
	{kDiv, 0x80000001u, 0x3F800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div -dmin / 1.0 [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x3F800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +dmax / 1.0 [seeded]"},
	{kDiv, 0x007FFFFFu, 0x3F800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +dmax / 1.0 [seeded]"},
	{kRsqrt, 0x41200000u, 0x3F800000u, 0x00030060u, 0x41200000u, 0x00000060u, "rsqrt 10.0 / 1.0 [seeded]"},
	{kDiv, 0x41200000u, 0x3F800000u, 0x00030060u, 0x41200000u, 0x00000060u, "div 10.0 / 1.0 [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x3F800000u, 0x00030060u, 0x7FFFFFFFu, 0x00000060u, "rsqrt EEMAX / 1.0 [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x3F800000u, 0x00030060u, 0x7FFFFFFFu, 0x00000060u, "div EEMAX / 1.0 [seeded]"},
	{kRsqrt, 0x7F800000u, 0x3F800000u, 0x00030060u, 0x7F800000u, 0x00000060u, "rsqrt 2^128 / 1.0 [seeded]"},
	{kDiv, 0x7F800000u, 0x3F800000u, 0x00030060u, 0x7F800000u, 0x00000060u, "div 2^128 / 1.0 [seeded]"},
	{kSqrt, 0x00000000u, 0x3F800000u, 0x00030060u, 0x3F800000u, 0x00000060u, "sqrt 1.0 [seeded]"},
	{kRsqrt, 0x3F800000u, 0xBF800000u, 0x00030060u, 0x3F800000u, 0x00020060u, "rsqrt 1.0 / -1.0 [seeded]"},
	{kDiv, 0x3F800000u, 0xBF800000u, 0x00030060u, 0xBF800000u, 0x00000060u, "div 1.0 / -1.0 [seeded]"},
	{kRsqrt, 0xBF800000u, 0xBF800000u, 0x00030060u, 0xBF800000u, 0x00020060u, "rsqrt -1.0 / -1.0 [seeded]"},
	{kDiv, 0xBF800000u, 0xBF800000u, 0x00030060u, 0x3F800000u, 0x00000060u, "div -1.0 / -1.0 [seeded]"},
	{kRsqrt, 0x00000000u, 0xBF800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +0 / -1.0 [seeded]"},
	{kDiv, 0x00000000u, 0xBF800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +0 / -1.0 [seeded]"},
	{kRsqrt, 0x80000000u, 0xBF800000u, 0x00030060u, 0x80000000u, 0x00020060u, "rsqrt -0 / -1.0 [seeded]"},
	{kDiv, 0x80000000u, 0xBF800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div -0 / -1.0 [seeded]"},
	{kRsqrt, 0x00000001u, 0xBF800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +dmin / -1.0 [seeded]"},
	{kDiv, 0x00000001u, 0xBF800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +dmin / -1.0 [seeded]"},
	{kRsqrt, 0x80000001u, 0xBF800000u, 0x00030060u, 0x80000000u, 0x00020060u, "rsqrt -dmin / -1.0 [seeded]"},
	{kDiv, 0x80000001u, 0xBF800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div -dmin / -1.0 [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0xBF800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +dmax / -1.0 [seeded]"},
	{kDiv, 0x007FFFFFu, 0xBF800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +dmax / -1.0 [seeded]"},
	{kRsqrt, 0x41200000u, 0xBF800000u, 0x00030060u, 0x41200000u, 0x00020060u, "rsqrt 10.0 / -1.0 [seeded]"},
	{kDiv, 0x41200000u, 0xBF800000u, 0x00030060u, 0xC1200000u, 0x00000060u, "div 10.0 / -1.0 [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0xBF800000u, 0x00030060u, 0x7FFFFFFFu, 0x00020060u, "rsqrt EEMAX / -1.0 [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0xBF800000u, 0x00030060u, 0xFFFFFFFFu, 0x00000060u, "div EEMAX / -1.0 [seeded]"},
	{kRsqrt, 0x7F800000u, 0xBF800000u, 0x00030060u, 0x7F800000u, 0x00020060u, "rsqrt 2^128 / -1.0 [seeded]"},
	{kDiv, 0x7F800000u, 0xBF800000u, 0x00030060u, 0xFF800000u, 0x00000060u, "div 2^128 / -1.0 [seeded]"},
	{kSqrt, 0x00000000u, 0xBF800000u, 0x00030060u, 0x3F800000u, 0x00020060u, "sqrt -1.0 [seeded]"},
	{kRsqrt, 0x3F800000u, 0x40800000u, 0x00030060u, 0x3F000000u, 0x00000060u, "rsqrt 1.0 / 4.0 [seeded]"},
	{kDiv, 0x3F800000u, 0x40800000u, 0x00030060u, 0x3E800000u, 0x00000060u, "div 1.0 / 4.0 [seeded]"},
	{kRsqrt, 0xBF800000u, 0x40800000u, 0x00030060u, 0xBF000000u, 0x00000060u, "rsqrt -1.0 / 4.0 [seeded]"},
	{kDiv, 0xBF800000u, 0x40800000u, 0x00030060u, 0xBE800000u, 0x00000060u, "div -1.0 / 4.0 [seeded]"},
	{kRsqrt, 0x00000000u, 0x40800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +0 / 4.0 [seeded]"},
	{kDiv, 0x00000000u, 0x40800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +0 / 4.0 [seeded]"},
	{kRsqrt, 0x80000000u, 0x40800000u, 0x00030060u, 0x80000000u, 0x00000060u, "rsqrt -0 / 4.0 [seeded]"},
	{kDiv, 0x80000000u, 0x40800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div -0 / 4.0 [seeded]"},
	{kRsqrt, 0x00000001u, 0x40800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +dmin / 4.0 [seeded]"},
	{kDiv, 0x00000001u, 0x40800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +dmin / 4.0 [seeded]"},
	{kRsqrt, 0x80000001u, 0x40800000u, 0x00030060u, 0x80000000u, 0x00000060u, "rsqrt -dmin / 4.0 [seeded]"},
	{kDiv, 0x80000001u, 0x40800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div -dmin / 4.0 [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x40800000u, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +dmax / 4.0 [seeded]"},
	{kDiv, 0x007FFFFFu, 0x40800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div +dmax / 4.0 [seeded]"},
	{kRsqrt, 0x41200000u, 0x40800000u, 0x00030060u, 0x40A00000u, 0x00000060u, "rsqrt 10.0 / 4.0 [seeded]"},
	{kDiv, 0x41200000u, 0x40800000u, 0x00030060u, 0x40200000u, 0x00000060u, "div 10.0 / 4.0 [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x40800000u, 0x00030060u, 0x7F7FFFFFu, 0x00000060u, "rsqrt EEMAX / 4.0 [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x40800000u, 0x00030060u, 0x7EFFFFFFu, 0x00000060u, "div EEMAX / 4.0 [seeded]"},
	{kRsqrt, 0x7F800000u, 0x40800000u, 0x00030060u, 0x7F000000u, 0x00000060u, "rsqrt 2^128 / 4.0 [seeded]"},
	{kDiv, 0x7F800000u, 0x40800000u, 0x00030060u, 0x7E800000u, 0x00000060u, "div 2^128 / 4.0 [seeded]"},
	{kSqrt, 0x00000000u, 0x40800000u, 0x00030060u, 0x40000000u, 0x00000060u, "sqrt 4.0 [seeded]"},
	{kRsqrt, 0x3F800000u, 0xC0800000u, 0x00030060u, 0x3F000000u, 0x00020060u, "rsqrt 1.0 / -4.0 [seeded]"},
	{kDiv, 0x3F800000u, 0xC0800000u, 0x00030060u, 0xBE800000u, 0x00000060u, "div 1.0 / -4.0 [seeded]"},
	{kRsqrt, 0xBF800000u, 0xC0800000u, 0x00030060u, 0xBF000000u, 0x00020060u, "rsqrt -1.0 / -4.0 [seeded]"},
	{kDiv, 0xBF800000u, 0xC0800000u, 0x00030060u, 0x3E800000u, 0x00000060u, "div -1.0 / -4.0 [seeded]"},
	{kRsqrt, 0x00000000u, 0xC0800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +0 / -4.0 [seeded]"},
	{kDiv, 0x00000000u, 0xC0800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +0 / -4.0 [seeded]"},
	{kRsqrt, 0x80000000u, 0xC0800000u, 0x00030060u, 0x80000000u, 0x00020060u, "rsqrt -0 / -4.0 [seeded]"},
	{kDiv, 0x80000000u, 0xC0800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div -0 / -4.0 [seeded]"},
	{kRsqrt, 0x00000001u, 0xC0800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +dmin / -4.0 [seeded]"},
	{kDiv, 0x00000001u, 0xC0800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +dmin / -4.0 [seeded]"},
	{kRsqrt, 0x80000001u, 0xC0800000u, 0x00030060u, 0x80000000u, 0x00020060u, "rsqrt -dmin / -4.0 [seeded]"},
	{kDiv, 0x80000001u, 0xC0800000u, 0x00030060u, 0x00000000u, 0x00000060u, "div -dmin / -4.0 [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0xC0800000u, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +dmax / -4.0 [seeded]"},
	{kDiv, 0x007FFFFFu, 0xC0800000u, 0x00030060u, 0x80000000u, 0x00000060u, "div +dmax / -4.0 [seeded]"},
	{kRsqrt, 0x41200000u, 0xC0800000u, 0x00030060u, 0x40A00000u, 0x00020060u, "rsqrt 10.0 / -4.0 [seeded]"},
	{kDiv, 0x41200000u, 0xC0800000u, 0x00030060u, 0xC0200000u, 0x00000060u, "div 10.0 / -4.0 [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0xC0800000u, 0x00030060u, 0x7F7FFFFFu, 0x00020060u, "rsqrt EEMAX / -4.0 [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0xC0800000u, 0x00030060u, 0xFEFFFFFFu, 0x00000060u, "div EEMAX / -4.0 [seeded]"},
	{kRsqrt, 0x7F800000u, 0xC0800000u, 0x00030060u, 0x7F000000u, 0x00020060u, "rsqrt 2^128 / -4.0 [seeded]"},
	{kDiv, 0x7F800000u, 0xC0800000u, 0x00030060u, 0xFE800000u, 0x00000060u, "div 2^128 / -4.0 [seeded]"},
	{kSqrt, 0x00000000u, 0xC0800000u, 0x00030060u, 0x40000000u, 0x00020060u, "sqrt -4.0 [seeded]"},
	{kRsqrt, 0x3F800000u, 0x7F7FFFFFu, 0x00030060u, 0x1F800001u, 0x00000060u, "rsqrt 1.0 / +FLT_MAX [seeded]"},
	{kDiv, 0x3F800000u, 0x7F7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "div 1.0 / +FLT_MAX [seeded]"},
	{kRsqrt, 0xBF800000u, 0x7F7FFFFFu, 0x00030060u, 0x9F800001u, 0x00000060u, "rsqrt -1.0 / +FLT_MAX [seeded]"},
	{kDiv, 0xBF800000u, 0x7F7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "div -1.0 / +FLT_MAX [seeded]"},
	{kRsqrt, 0x00000000u, 0x7F7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +0 / +FLT_MAX [seeded]"},
	{kDiv, 0x00000000u, 0x7F7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "div +0 / +FLT_MAX [seeded]"},
	{kRsqrt, 0x80000000u, 0x7F7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "rsqrt -0 / +FLT_MAX [seeded]"},
	{kDiv, 0x80000000u, 0x7F7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "div -0 / +FLT_MAX [seeded]"},
	{kRsqrt, 0x00000001u, 0x7F7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +dmin / +FLT_MAX [seeded]"},
	{kDiv, 0x00000001u, 0x7F7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "div +dmin / +FLT_MAX [seeded]"},
	{kRsqrt, 0x80000001u, 0x7F7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "rsqrt -dmin / +FLT_MAX [seeded]"},
	{kDiv, 0x80000001u, 0x7F7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "div -dmin / +FLT_MAX [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0x7F7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "rsqrt +dmax / +FLT_MAX [seeded]"},
	{kDiv, 0x007FFFFFu, 0x7F7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "div +dmax / +FLT_MAX [seeded]"},
	{kRsqrt, 0x41200000u, 0x7F7FFFFFu, 0x00030060u, 0x21200001u, 0x00000060u, "rsqrt 10.0 / +FLT_MAX [seeded]"},
	{kDiv, 0x41200000u, 0x7F7FFFFFu, 0x00030060u, 0x01200001u, 0x00000060u, "div 10.0 / +FLT_MAX [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0x7F7FFFFFu, 0x00030060u, 0x60000000u, 0x00000060u, "rsqrt EEMAX / +FLT_MAX [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0x7F7FFFFFu, 0x00030060u, 0x40000000u, 0x00000060u, "div EEMAX / +FLT_MAX [seeded]"},
	{kRsqrt, 0x7F800000u, 0x7F7FFFFFu, 0x00030060u, 0x5F800001u, 0x00000060u, "rsqrt 2^128 / +FLT_MAX [seeded]"},
	{kDiv, 0x7F800000u, 0x7F7FFFFFu, 0x00030060u, 0x3F800001u, 0x00000060u, "div 2^128 / +FLT_MAX [seeded]"},
	{kSqrt, 0x00000000u, 0x7F7FFFFFu, 0x00030060u, 0x5F7FFFFFu, 0x00000060u, "sqrt +FLT_MAX [seeded]"},
	{kRsqrt, 0x3F800000u, 0xFF7FFFFFu, 0x00030060u, 0x1F800001u, 0x00020060u, "rsqrt 1.0 / -FLT_MAX [seeded]"},
	{kDiv, 0x3F800000u, 0xFF7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "div 1.0 / -FLT_MAX [seeded]"},
	{kRsqrt, 0xBF800000u, 0xFF7FFFFFu, 0x00030060u, 0x9F800001u, 0x00020060u, "rsqrt -1.0 / -FLT_MAX [seeded]"},
	{kDiv, 0xBF800000u, 0xFF7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "div -1.0 / -FLT_MAX [seeded]"},
	{kRsqrt, 0x00000000u, 0xFF7FFFFFu, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +0 / -FLT_MAX [seeded]"},
	{kDiv, 0x00000000u, 0xFF7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "div +0 / -FLT_MAX [seeded]"},
	{kRsqrt, 0x80000000u, 0xFF7FFFFFu, 0x00030060u, 0x80000000u, 0x00020060u, "rsqrt -0 / -FLT_MAX [seeded]"},
	{kDiv, 0x80000000u, 0xFF7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "div -0 / -FLT_MAX [seeded]"},
	{kRsqrt, 0x00000001u, 0xFF7FFFFFu, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +dmin / -FLT_MAX [seeded]"},
	{kDiv, 0x00000001u, 0xFF7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "div +dmin / -FLT_MAX [seeded]"},
	{kRsqrt, 0x80000001u, 0xFF7FFFFFu, 0x00030060u, 0x80000000u, 0x00020060u, "rsqrt -dmin / -FLT_MAX [seeded]"},
	{kDiv, 0x80000001u, 0xFF7FFFFFu, 0x00030060u, 0x00000000u, 0x00000060u, "div -dmin / -FLT_MAX [seeded]"},
	{kRsqrt, 0x007FFFFFu, 0xFF7FFFFFu, 0x00030060u, 0x00000000u, 0x00020060u, "rsqrt +dmax / -FLT_MAX [seeded]"},
	{kDiv, 0x007FFFFFu, 0xFF7FFFFFu, 0x00030060u, 0x80000000u, 0x00000060u, "div +dmax / -FLT_MAX [seeded]"},
	{kRsqrt, 0x41200000u, 0xFF7FFFFFu, 0x00030060u, 0x21200001u, 0x00020060u, "rsqrt 10.0 / -FLT_MAX [seeded]"},
	{kDiv, 0x41200000u, 0xFF7FFFFFu, 0x00030060u, 0x81200001u, 0x00000060u, "div 10.0 / -FLT_MAX [seeded]"},
	{kRsqrt, 0x7FFFFFFFu, 0xFF7FFFFFu, 0x00030060u, 0x60000000u, 0x00020060u, "rsqrt EEMAX / -FLT_MAX [seeded]"},
	{kDiv, 0x7FFFFFFFu, 0xFF7FFFFFu, 0x00030060u, 0xC0000000u, 0x00000060u, "div EEMAX / -FLT_MAX [seeded]"},
	{kRsqrt, 0x7F800000u, 0xFF7FFFFFu, 0x00030060u, 0x5F800001u, 0x00020060u, "rsqrt 2^128 / -FLT_MAX [seeded]"},
	{kDiv, 0x7F800000u, 0xFF7FFFFFu, 0x00030060u, 0xBF800001u, 0x00000060u, "div 2^128 / -FLT_MAX [seeded]"},
	{kSqrt, 0x00000000u, 0xFF7FFFFFu, 0x00030060u, 0x5F7FFFFFu, 0x00020060u, "sqrt -FLT_MAX [seeded]"},
	{kSqrtDiv, 0x3F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 1.0 / +0"},
	{kSqrtDiv, 0xBF800000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "sqrt then div -1.0 / +0"},
	{kSqrtDiv, 0x00000000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +0 / +0"},
	{kSqrtDiv, 0x80000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -0 / +0"},
	{kSqrtDiv, 0x00000001u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmin / +0"},
	{kSqrtDiv, 0x80000001u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -dmin / +0"},
	{kSqrtDiv, 0x007FFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmax / +0"},
	{kSqrtDiv, 0x41200000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 10.0 / +0"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div EEMAX / +0"},
	{kSqrtDiv, 0x7F800000u, 0x00000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 2^128 / +0"},
	{kSqrtDiv, 0x3F800000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 1.0 / -0"},
	{kSqrtDiv, 0xBF800000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00010060u, "sqrt then div -1.0 / -0"},
	{kSqrtDiv, 0x00000000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +0 / -0"},
	{kSqrtDiv, 0x80000000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -0 / -0"},
	{kSqrtDiv, 0x00000001u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmin / -0"},
	{kSqrtDiv, 0x80000001u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -dmin / -0"},
	{kSqrtDiv, 0x007FFFFFu, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmax / -0"},
	{kSqrtDiv, 0x41200000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 10.0 / -0"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div EEMAX / -0"},
	{kSqrtDiv, 0x7F800000u, 0x80000000u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 2^128 / -0"},
	{kSqrtDiv, 0x3F800000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 1.0 / +dmin"},
	{kSqrtDiv, 0xBF800000u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "sqrt then div -1.0 / +dmin"},
	{kSqrtDiv, 0x00000000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +0 / +dmin"},
	{kSqrtDiv, 0x80000000u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -0 / +dmin"},
	{kSqrtDiv, 0x00000001u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmin / +dmin"},
	{kSqrtDiv, 0x80000001u, 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -dmin / +dmin"},
	{kSqrtDiv, 0x007FFFFFu, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmax / +dmin"},
	{kSqrtDiv, 0x41200000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 10.0 / +dmin"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div EEMAX / +dmin"},
	{kSqrtDiv, 0x7F800000u, 0x00000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 2^128 / +dmin"},
	{kSqrtDiv, 0x3F800000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 1.0 / -dmin"},
	{kSqrtDiv, 0xBF800000u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00010060u, "sqrt then div -1.0 / -dmin"},
	{kSqrtDiv, 0x00000000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +0 / -dmin"},
	{kSqrtDiv, 0x80000000u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -0 / -dmin"},
	{kSqrtDiv, 0x00000001u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmin / -dmin"},
	{kSqrtDiv, 0x80000001u, 0x80000001u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -dmin / -dmin"},
	{kSqrtDiv, 0x007FFFFFu, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmax / -dmin"},
	{kSqrtDiv, 0x41200000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 10.0 / -dmin"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div EEMAX / -dmin"},
	{kSqrtDiv, 0x7F800000u, 0x80000001u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 2^128 / -dmin"},
	{kSqrtDiv, 0x3F800000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 1.0 / +dmid"},
	{kSqrtDiv, 0xBF800000u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "sqrt then div -1.0 / +dmid"},
	{kSqrtDiv, 0x00000000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +0 / +dmid"},
	{kSqrtDiv, 0x80000000u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -0 / +dmid"},
	{kSqrtDiv, 0x00000001u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmin / +dmid"},
	{kSqrtDiv, 0x80000001u, 0x00400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -dmin / +dmid"},
	{kSqrtDiv, 0x007FFFFFu, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmax / +dmid"},
	{kSqrtDiv, 0x41200000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 10.0 / +dmid"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div EEMAX / +dmid"},
	{kSqrtDiv, 0x7F800000u, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 2^128 / +dmid"},
	{kSqrtDiv, 0x3F800000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 1.0 / -dmid"},
	{kSqrtDiv, 0xBF800000u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00010060u, "sqrt then div -1.0 / -dmid"},
	{kSqrtDiv, 0x00000000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +0 / -dmid"},
	{kSqrtDiv, 0x80000000u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -0 / -dmid"},
	{kSqrtDiv, 0x00000001u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmin / -dmid"},
	{kSqrtDiv, 0x80000001u, 0x80400000u, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -dmin / -dmid"},
	{kSqrtDiv, 0x007FFFFFu, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmax / -dmid"},
	{kSqrtDiv, 0x41200000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 10.0 / -dmid"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div EEMAX / -dmid"},
	{kSqrtDiv, 0x7F800000u, 0x80400000u, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 2^128 / -dmid"},
	{kSqrtDiv, 0x3F800000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 1.0 / +dmax"},
	{kSqrtDiv, 0xBF800000u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00010020u, "sqrt then div -1.0 / +dmax"},
	{kSqrtDiv, 0x00000000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +0 / +dmax"},
	{kSqrtDiv, 0x80000000u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -0 / +dmax"},
	{kSqrtDiv, 0x00000001u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmin / +dmax"},
	{kSqrtDiv, 0x80000001u, 0x007FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -dmin / +dmax"},
	{kSqrtDiv, 0x007FFFFFu, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmax / +dmax"},
	{kSqrtDiv, 0x41200000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 10.0 / +dmax"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div EEMAX / +dmax"},
	{kSqrtDiv, 0x7F800000u, 0x007FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010020u, "sqrt then div 2^128 / +dmax"},
	{kSqrtDiv, 0x3F800000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 1.0 / -dmax"},
	{kSqrtDiv, 0xBF800000u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00010060u, "sqrt then div -1.0 / -dmax"},
	{kSqrtDiv, 0x00000000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +0 / -dmax"},
	{kSqrtDiv, 0x80000000u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -0 / -dmax"},
	{kSqrtDiv, 0x00000001u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmin / -dmax"},
	{kSqrtDiv, 0x80000001u, 0x807FFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00020040u, "sqrt then div -dmin / -dmax"},
	{kSqrtDiv, 0x007FFFFFu, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00020040u, "sqrt then div +dmax / -dmax"},
	{kSqrtDiv, 0x41200000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 10.0 / -dmax"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div EEMAX / -dmax"},
	{kSqrtDiv, 0x7F800000u, 0x807FFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00010060u, "sqrt then div 2^128 / -dmax"},
	{kSqrtDiv, 0x3F800000u, 0x40800000u, 0x00000000u, 0x3F000000u, 0x00000000u, "sqrt then div 1.0 / 4.0"},
	{kSqrtDiv, 0xBF800000u, 0x40800000u, 0x00000000u, 0xBF000000u, 0x00000000u, "sqrt then div -1.0 / 4.0"},
	{kSqrtDiv, 0x00000000u, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "sqrt then div +0 / 4.0"},
	{kSqrtDiv, 0x80000000u, 0x40800000u, 0x00000000u, 0x80000000u, 0x00000000u, "sqrt then div -0 / 4.0"},
	{kSqrtDiv, 0x00000001u, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "sqrt then div +dmin / 4.0"},
	{kSqrtDiv, 0x80000001u, 0x40800000u, 0x00000000u, 0x80000000u, 0x00000000u, "sqrt then div -dmin / 4.0"},
	{kSqrtDiv, 0x007FFFFFu, 0x40800000u, 0x00000000u, 0x00000000u, 0x00000000u, "sqrt then div +dmax / 4.0"},
	{kSqrtDiv, 0x41200000u, 0x40800000u, 0x00000000u, 0x40A00000u, 0x00000000u, "sqrt then div 10.0 / 4.0"},
	{kSqrtDiv, 0x7FFFFFFFu, 0x40800000u, 0x00000000u, 0x7F7FFFFFu, 0x00000000u, "sqrt then div EEMAX / 4.0"},
	{kSqrtDiv, 0x7F800000u, 0x40800000u, 0x00000000u, 0x7F000000u, 0x00000000u, "sqrt then div 2^128 / 4.0"},
	{kSqrtDiv, 0x3F800000u, 0xC0800000u, 0x00000000u, 0x3F000000u, 0x00000040u, "sqrt then div 1.0 / -4.0"},
	{kSqrtDiv, 0xBF800000u, 0xC0800000u, 0x00000000u, 0xBF000000u, 0x00000040u, "sqrt then div -1.0 / -4.0"},
	{kSqrtDiv, 0x00000000u, 0xC0800000u, 0x00000000u, 0x00000000u, 0x00000040u, "sqrt then div +0 / -4.0"},
	{kSqrtDiv, 0x80000000u, 0xC0800000u, 0x00000000u, 0x80000000u, 0x00000040u, "sqrt then div -0 / -4.0"},
	{kSqrtDiv, 0x00000001u, 0xC0800000u, 0x00000000u, 0x00000000u, 0x00000040u, "sqrt then div +dmin / -4.0"},
	{kSqrtDiv, 0x80000001u, 0xC0800000u, 0x00000000u, 0x80000000u, 0x00000040u, "sqrt then div -dmin / -4.0"},
	{kSqrtDiv, 0x007FFFFFu, 0xC0800000u, 0x00000000u, 0x00000000u, 0x00000040u, "sqrt then div +dmax / -4.0"},
	{kSqrtDiv, 0x41200000u, 0xC0800000u, 0x00000000u, 0x40A00000u, 0x00000040u, "sqrt then div 10.0 / -4.0"},
	{kSqrtDiv, 0x7FFFFFFFu, 0xC0800000u, 0x00000000u, 0x7F7FFFFFu, 0x00000040u, "sqrt then div EEMAX / -4.0"},
	{kSqrtDiv, 0x7F800000u, 0xC0800000u, 0x00000000u, 0x7F000000u, 0x00000040u, "sqrt then div 2^128 / -4.0"},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

std::vector<u32> Program(const Row& r)
{
	switch (r.op)
	{
		case kRsqrt: return {ee::RSQRT_S(4, 5, 6)};
		case kDiv:   return {ee::DIV_S(4, 5, 6)};
		case kSqrt:  return {ee::SQRT_S(4, 6)};
		default:     return {ee::SQRT_S(7, 6), ee::DIV_S(4, 5, 7)};
	}
}

// leg 0 interpreter, 1 the shipping fast path, 2 fpuFullMode, 3 fpuExactMode.
void RunRow(const Row& r, int leg, u32* result, u32* fcr31)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (leg == 3)
		h.EnableFpuExactMode();
	else if (leg == 2)
		h.EnableFpuFullMode();
	h.SetFcr31(r.seed);
	h.SetFprBits(5, r.fs);
	h.SetFprBits(6, r.ft);
	h.SetFprBits(4, r.fs); // so an op that writes nothing is visible as such
	h.LoadProgram(Program(r));
	if (leg == 0)
	{
		h.RunInterpOnly();
		*result = h.GetFprBitsInterp(4);
		*fcr31 = h.InterpSnapshot().fprs.fprc[31];
	}
	else
	{
		h.RunJitNoDiff();
		*result = h.GetFprBitsJit(4);
		*fcr31 = h.JitSnapshot().fprs.fprc[31];
	}
}

const char* kLegName[] = {"[interp]", "[jit fast]", "[jit full]", "[jit exact]"};

// The fast path computes in host singles, where an exponent-255 operand is an
// infinity or a NaN, so it saturates on the way in. That class belongs to
// ee_fpu_top_binade_console_tests.cpp.
bool TopBinadeOperand(const Row& r)
{
	return (r.fs & 0x7F800000u) == 0x7F800000u || (r.ft & 0x7F800000u) == 0x7F800000u;
}

// fpuFullMode divides in host doubles and rounds correctly; the console's
// divide/square-root unit does not, and the model that closes the gap is
// interpreter-only by decision. What is left is one ULP toward zero, on rsqrt
// by a top-binade divisor. A zero or denormal dividend gives an exact zero
// quotient, so those rows are not in the class.
bool DivUnitApproximation(const Row& r)
{
	return r.op == kRsqrt && (r.ft & 0x7FFFFFFFu) == 0x7F7FFFFFu
		&& (r.fs & 0x7F800000u) != 0;
}

} // namespace

// Every tier owes the flag axis: FCR31 costs the fast path none of the
// precision the accuracy tiers buy, so no tier has a tradeoff to make here.
TEST(EeFpuRsqrtSignConsole, CauseBitsMatchConsoleOnEveryTier)
{
	for (int i = 0; i < kRowCount; ++i)
	{
		const Row& r = kRows[i];
		for (int leg = 0; leg < 4; ++leg)
		{
			SCOPED_TRACE(::testing::Message() << r.what << " " << kLegName[leg]);
			u32 res = 0, fcr = 0;
			RunRow(r, leg, &res, &fcr);
			EXPECT_EQ(fcr & kCauseSticky, r.flags & kCauseSticky);
		}
	}
}

// The interpreter owes every row: it holds the EE's top binade and runs the
// divide unit's own recurrence, so it has nothing to trade.
TEST(EeFpuRsqrtSignConsole, InterpMatchesConsoleOnEveryRow)
{
	for (int i = 0; i < kRowCount; ++i)
	{
		const Row& r = kRows[i];
		SCOPED_TRACE(::testing::Message() << r.what << " " << kLegName[0]);
		u32 res = 0, fcr = 0;
		RunRow(r, 0, &res, &fcr);
		EXPECT_EQ(res, r.console);
	}
}

// fpuFullMode divides in host doubles and rounds correctly, so it owes every
// row but the divide unit's, and that one divergence is asserted as itself
// rather than skipped. eeClampMode 4 closes it, just below.
TEST(EeFpuRsqrtSignConsole, FullModeMatchesConsoleOutsideTheDivideUnitApproximation)
{
	int approximated = 0;
	for (int i = 0; i < kRowCount; ++i)
	{
		const Row& r = kRows[i];
		SCOPED_TRACE(::testing::Message() << r.what << " " << kLegName[2]);
		u32 res = 0, fcr = 0;
		RunRow(r, 2, &res, &fcr);
		if (DivUnitApproximation(r))
		{
			++approximated;
			EXPECT_EQ(res & 0x80000000u, r.console & 0x80000000u) << "sign";
			EXPECT_EQ(res & 0x7FFFFFFFu, (r.console & 0x7FFFFFFFu) - 1u)
				<< "one ULP toward zero, not more and not the other way";
		}
		else
		{
			EXPECT_EQ(res, r.console);
		}
	}
	EXPECT_EQ(approximated, 20);
}

// fpuExactMode owes every row for the same two reasons: it holds the top binade
// and it runs the divide unit's recurrence, out of line. The 20 rows above are
// what it buys, so this failing on exactly those is the island not being
// emitted rather than a new defect.
TEST(EeFpuRsqrtSignConsole, ExactModeMatchesConsoleOnEveryRow)
{
	for (int i = 0; i < kRowCount; ++i)
	{
		const Row& r = kRows[i];
		SCOPED_TRACE(::testing::Message() << r.what << " " << kLegName[3]);
		u32 res = 0, fcr = 0;
		RunRow(r, 3, &res, &fcr);
		EXPECT_EQ(res, r.console);
	}
}

// The fast path saturates in host singles, one binade below the EE's largest
// number, so a console 0x7FFFFFFF comes back 0x7F7FFFFF with the same sign.
// Top-binade operands aside, it is exact everywhere else.
TEST(EeFpuRsqrtSignConsole, FastPathSaturatesABinadeLowOnTheMaximum)
{
	int saturated = 0, skipped = 0;
	for (int i = 0; i < kRowCount; ++i)
	{
		const Row& r = kRows[i];
		if (TopBinadeOperand(r))
		{
			++skipped;
			continue;
		}
		SCOPED_TRACE(r.what);
		u32 res = 0, fcr = 0;
		RunRow(r, 1, &res, &fcr);
		if ((r.console & 0x7FFFFFFFu) == 0x7FFFFFFFu)
		{
			++saturated;
			EXPECT_EQ(res, (r.console & 0x80000000u) | 0x7F7FFFFFu);
		}
		else
		{
			EXPECT_EQ(res, r.console);
		}
	}
	EXPECT_EQ(saturated, 324);
	EXPECT_EQ(skipped, 148);
}
