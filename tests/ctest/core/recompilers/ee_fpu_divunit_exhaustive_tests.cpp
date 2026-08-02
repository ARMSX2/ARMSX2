// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// What the EE's divide unit is not.
//
// ee_fpu_divunit_console_tests.cpp measures the gap between this tree and
// silicon on a corpus of arbitrary operands. This file pins the handful of rows
// that constrain what may ever be written to close that gap, and it exists
// because five rounds of modelling built on a frame that two of these rows
// refute outright.
//
// Provenance. Every value below was measured on an SCPH-90000 (FCR0 00002E40)
// over ps2link, twice: once inside a 150,994,944-row exhaustive capture
// (captures/fpmatrix/divsqrt/scatter/exh.c, exh2.c and exh3.c -- eighteen
// divisors, every numerator significand in [2^23, 2^24) at each, one byte per
// row, FNV-1a chained and spot re-measured), and once individually by wit.c and
// wit2.c, which re-run exactly these twenty-one rows with FCR31 cleared, 32-nop
// spacers and a double read. The two agree on every row. Analysis and the full
// tables: captures/fpmatrix/divsqrt/scatter/FINDINGS-div-round11-exhaustive.md.
//
// The frame these rows are read in: with ma and mb the 24-bit significands,
// lt = (ma < mb), k = 23 + lt, num = ma << k, T = num / mb and rem = num % mb,
// the result significand is always T or T+1 -- exhaustively true on all
// 50,331,648 rows -- and u = mb - rem is how far the exact quotient sits below
// T+1. Correct rounding is 2*rem >= mb.
//
// The findings, in the order they constrain a model:
//
// 1. The unit is not a rounding rule. TheErrorSpansNearlyAWholeUlpBothWays
//    holds a row where silicon returns T although the exact quotient is
//    0.9596 of the way to T+1, and one where it returns T+1 from 0.0000002 of
//    the way. Round-to-nearest cannot exceed half an ULP; a directed mode can
//    approach a whole one but only ever in one direction. This unit does both,
//    on the same divisor.
//
// 2. The decision is not a function of (branch, divisor, u, nu2(T+1)).
//    TheDecisionNeedsMoreThanTheExactQuotient holds two rows that agree in
//    every one of those coordinates -- same divisor 0x3FC00000, same A>=B
//    branch, same u = 2^22, the same exact fraction 2/3, the same nu2(T+1) = 0
//    -- and round opposite ways. That is the coordinate system of every div
//    model this tree has considered, and the rows are a proof rather than a
//    score because divisor 0xC00000's classes were captured complete.
//
// 3. What survives is a statement about the A>=B branch, not about a class of
//    divisors: on that branch silicon never rounds up with
//    u > 2^22 -- 0 exceptions in 12,585,601 UP rows across all twelve exhaustive
//    divisors, odd and even alike -- and it reaches u = 2^22 exactly, on seven
//    of them. Both halves are pinned by LawAHoldsOnEveryDivisorAndIsAttained,
//    because a bound with no attainment witness passes vacuously the moment the
//    unit gets more conservative.
//
//    The A<B analogue is capped rather than shifted: UP => u <= max(2^23,
//    mb - 2^22), equivalently rem >= min(mb - 2^23, 2^22). That is 0 violations
//    in 28,425,919 A<B UP rows over twelve exhaustive divisors, and it is
//    attained by rows whose remainder is exactly 2^22. The naive shifted form
//    (u <= 2^23) looks right on every divisor below 1.5 * 2^23 -- which is why
//    nine rounds of sampled data never caught it -- and is wrong on 262,318
//    rows of mb = 0xE8B4C4. TheAlbBoundIsCappedNotShifted holds both.
//
// 4. What the missing coordinate looks like (round 11b). Mining divisor
//    0xC00000 exhaustively -- it forces rem into {0, 2^22, 2*2^22}, so a third
//    of all numerators land in one class where branch, u, j and the exact
//    fraction are all constant -- the UP rows turn out to be 14,329 isolated
//    singletons, never two adjacent. That kills every model whose error grows
//    with the numerator (TheUpSetIsIsolatedSingletonsNotATopSlice). What
//    separates them is the carry-propagation distance in the trial product
//    mb*(T+1): UP => distance >= 13 + lt, 0 exceptions in 2,587,960 rows
//    (CarryDistanceSeparatesTheUndecidedShell). Six further degenerate
//    divisors show the up-rate rising monotonically with that distance but at
//    an odd-part-specific cut, so the coordinate is real and this closed form
//    is not general -- it is a joint function of mb and T+1, which is exactly
//    why no frame carrying them separately could work.
//
// 5. The cap is not sufficient, and 0xC00000 cannot show it: u < cap rounds
//    DOWN on 27.5% of 56,531,072 rows, but that divisor has no A>=B rows below
//    the cap at all (TheCapIsNecessaryAndNotSufficient). The A<B cap is only
//    an upper envelope -- not attained on two divisors, and not monotone in mb
//    (TheAlbCapIsAnEnvelopeNotTheLaw).
//
// 6. What shipped is finding 3 and nothing else. FPU.cpp's
//    eeDivide() and eeSqrtBits() now truncate when u exceeds the cap, and keep
//    the correctly rounded answer everywhere else, because that implication is
//    the only part of the unit with zero measured exceptions. The rows in
//    kLawRows are its witnesses, re-measured individually on silicon by wit3.c
//    (FCR0 00002E40, FCR31 cleared per op, 32-nop spacers, every result read
//    twice, no read disagreed with itself) after being found in the bulk
//    captures. Three groups, all needed:
//
//      * six div rows and four sqrt rows where the law changes the answer --
//        without them the model is unpinned and could quietly stop firing;
//      * two sqrt rows where u is exactly 2^23 and silicon still rounds up --
//        without them `u > 2^23 => DOWN` would pass vacuously the moment the
//        unit got more conservative;
//      * two sqrt rows where the law is silent and silicon truncates anyway --
//        the residual, kept so "still wrong here" has names.
//
//    The tests below also state that the A<B half of the div cap never changes
//    an answer: cap = max(2^23, mb-2^22) is always greater than mb/2, so
//    u > cap already implies 2*rem < mb and correct rounding says DOWN by
//    itself. Verified over all eighteen
//    exhaustive divisors -- 18,878,960 A<B rows above the cap, every one of
//    them a row correct rounding got right anyway. The A>=B half does all the
//    work: 6,118,759 rows of the 150,994,944 change, and all of them improve.
//
// The interpreter expectations in kRows below are still the ieee column,
// because every one of those rows sits inside u <= cap where nothing is
// settled. The tripwire at the bottom is the acceptance test for the rest.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include <algorithm>
#include <iterator>

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFs = 5, kFt = 6, kFd = 7;

struct DivRow
{
	u32 fs, ft;
	u32 con_div, ieee_div;    // div.s Fs, Ft
	u32 con_sqrt, ieee_sqrt;  // sqrt.s Ft
	u32 con_rsqrt;            // rsqrt.s Fs, Ft
	u32 con_two_step;         // div.s Fs, (sqrt.s Ft), measured separately
	const char* what;
};

// wit.c's output, verbatim. The exact fraction f = rem/mb and the signed error
// in ULP are in the comments because they are what makes each row worth
// keeping; both are re-derivable from fs and ft alone.
constexpr DivRow kRows[] = {
	// f = 0.6666667, err = +0.333  -- pairs with the next row
	{0x3FC02C86u, 0x3FC00000u, 0x3F801DAFu, 0x3F801DAFu, 0x3F9CC471u, 0x3F9CC471u, 0x3F9CE8CAu,
	 0x3F9CE8CAu, "u = 2^22, rounds UP"},
	// f = 0.6666667, err = -0.667  -- same divisor, branch, u, f and nu2(T+1)
	{0x3FC00001u, 0x3FC00000u, 0x3F800000u, 0x3F800001u, 0x3F9CC471u, 0x3F9CC471u, 0x3F9CC471u,
	 0x3F9CC471u, "u = 2^22, rounds DOWN"},
	// f = 0.9596187, err = -0.960  -- the worst low row of the whole capture
	{0x3F852B38u, 0x3F800001u, 0x3F852B36u, 0x3F852B37u, 0x3F800000u, 0x3F800000u, 0x3F852B38u,
	 0x3F852B38u, "returns T from f = 0.96"},
	// f = 0.5000001, err = +0.500  -- the only u = 2^22 row on this divisor
	{0x3FC00001u, 0x3F800001u, 0x3FC00000u, 0x3FC00000u, 0x3F800000u, 0x3F800000u, 0x3FC00001u,
	 0x3FC00001u, "law A attained: u = 2^22 rounds UP"},
	// f = 0.3388671, err = +0.661  -- u > 2^23 and it still rounds up
	{0x3F80001Au, 0x3FE8B4C4u, 0x3F0CD031u, 0x3F0CD030u, 0x3FAC965Au, 0x3FAC965Bu, 0x3F3DDD29u,
	 0x3F3DDD29u, "counterexample to the shifted bound"},
	// f = 0.3899285, err = +0.610  -- round 9's A<B witness, re-measured
	{0x3FA278E8u, 0x3FC8DF87u, 0x3F4F0F81u, 0x3F4F0F80u, 0x3FA05950u, 0x3FA05950u, 0x3F81B1EEu,
	 0x3F81B1EEu, "A<B rounds UP below halfway"},
	// f = 0.0000002, err = +1.000  -- 1.0 / (1 + 2^-23)
	{0x3F800000u, 0x3F800001u, 0x3F7FFFFFu, 0x3F7FFFFEu, 0x3F800000u, 0x3F800000u, 0x3F800000u,
	 0x3F800000u, "returns T+1 from f = 2.4e-7"},
	// f = 0.2750250, err = +0.725  -- rem is exactly 2^22: the A<B cap, attained
	{0x3F9FFC47u, 0x3FE8B4C4u, 0x3F300001u, 0x3F300000u, 0x3FAC965Au, 0x3FAC965Bu, 0x3F6D4EBEu,
	 0x3F6D4EBEu, "A<B cap attained"},
	// f = 0.2750248, err = +0.725  -- the same, on a second divisor
	{0x3F8DCE2Fu, 0x3FE8B4D0u, 0x3F1C0001u, 0x3F1C0000u, 0x3FAC965Fu, 0x3FAC965Fu, 0x3F525744u,
	 0x3F525744u, "A<B cap attained, second divisor"},
	{0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u,
	 0x3F800000u, "control 1/1"},
	{0x3F800000u, 0x40000000u, 0x3F000000u, 0x3F000000u, 0x3FB504F3u, 0x3FB504F3u, 0x3F3504F3u,
	 0x3F3504F3u, "control 1/2"},
	{0x3F800000u, 0x40400000u, 0x3EAAAAABu, 0x3EAAAAABu, 0x3FDDB3D7u, 0x3FDDB3D7u, 0x3F13CD3Au,
	 0x3F13CD3Au, "control 1/3"},

	// --- round 11b: the missing coordinate (wit2.c) -------------------------
	// The three rows below share divisor, branch, u = 2^22 and f = 2/3 with the
	// first two rows of the table. 0x3FC02C86 rounds UP and BOTH of its class
	// neighbours round DOWN, so the UP set is isolated singletons.
	// f = 0.6666667, err = -0.667
	{0x3FC02C83u, 0x3FC00000u, 0x3F801DACu, 0x3F801DADu, 0x3F9CC471u, 0x3F9CC471u, 0x3F9CE8C8u,
	 0x3F9CE8C8u, "left class neighbour of the UP singleton, DOWN"},
	// f = 0.6666667, err = -0.667
	{0x3FC02C89u, 0x3FC00000u, 0x3F801DB0u, 0x3F801DB1u, 0x3F9CC471u, 0x3F9CC471u, 0x3F9CE8CDu,
	 0x3F9CE8CDu, "right class neighbour of the UP singleton, DOWN"},
	// f = 0.6666667, err = -0.667 -- carry distance 12, and its low 13 bits are
	// maximal, so what stops it is not a magnitude threshold
	{0x3FC017FEu, 0x3FC00000u, 0x3F800FFEu, 0x3F800FFFu, 0x3F9CC471u, 0x3F9CC471u, 0x3F9CD807u,
	 0x3F9CD807u, "carry distance 12: DOWN with maximal low bits"},
	// f = 0.6666667, err = -0.667 -- u = 3*2^20, strictly under the 2^22 cap
	{0x3F900003u, 0x3F900000u, 0x3F800002u, 0x3F800003u, 0x3F87C3B6u, 0x3F87C3B6u, 0x3F87C3B9u,
	 0x3F87C3B9u, "u < cap and still DOWN"},
	// f = 0.7777778, err = -0.778 -- u = 2^21, half the cap, still DOWN
	{0x3F90000Bu, 0x3F900000u, 0x3F800009u, 0x3F80000Au, 0x3F87C3B6u, 0x3F87C3B6u, 0x3F87C3C1u,
	 0x3F87C3C1u, "u = half the cap and still DOWN"},
	// f = 0.2857143, err = -0.286 -- u is exactly max(2^23, mb - 2^22) and the
	// whole class of 898,779 rows rounds DOWN: that bound is not attained here
	{0x3F800005u, 0x3FE00000u, 0x3F12492Au, 0x3F12492Au, 0x3FA953FDu, 0x3FA953FDu, 0x3F418497u,
	 0x3F418497u, "A<B assumed cap, DOWN"},
	// f = 0.4285714, err = +0.571 -- the largest u that ever rounds UP here
	{0x3F802C7Cu, 0x3FE00000u, 0x3F127BFCu, 0x3F127BFBu, 0x3FA953FDu, 0x3FA953FDu, 0x3F41C7D1u,
	 0x3F41C7D1u, "A<B true maximum u = 2^23, UP"},
	// f = 0.2666667, err = -0.267 -- same story on a second divisor
	{0x3F80000Bu, 0x3FF00000u, 0x3F088894u, 0x3F088894u, 0x3FAF456Eu, 0x3FAF456Fu, 0x3F3AF4CBu,
	 0x3F3AF4CBu, "A<B assumed cap, DOWN, second divisor"},
	// f = 0.7333333, err = +0.267 -- an UP row from a shell whose rate is 0.100%
	{0x3FF0EC38u, 0x3FF00000u, 0x3F807DFCu, 0x3F807DFCu, 0x3FAF456Eu, 0x3FAF456Fu, 0x3FAFF1F2u,
	 0x3FAFF1F2u, "A>=B shell UP at a 0.100% class rate"},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));

u32 RunDiv(u32 fs, u32 ft)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(kFs, fs);
	h.SetFprBits(kFt, ft);
	h.LoadProgram({DIV_S(kFd, kFs, kFt)});
	h.RunInterpOnly();
	return h.GetFprBitsInterp(kFd);
}

u32 RunSqrt(u32 ft)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(kFt, ft);
	h.LoadProgram({SQRT_S(kFd, kFt)});
	h.RunInterpOnly();
	return h.GetFprBitsInterp(kFd);
}

u32 RunRsqrt(u32 fs, u32 ft)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(kFs, fs);
	h.SetFprBits(kFt, ft);
	h.LoadProgram({RSQRT_S(kFd, kFs, kFt)});
	h.RunInterpOnly();
	return h.GetFprBitsInterp(kFd);
}

// The frame, recomputed here from the operand bits so the table cannot drift
// away from the arithmetic it claims to describe.
struct Frame
{
	u32 ma, mb, T, rem, u;
	int lt, nu2_tp1;
	u32 down;  // the result word for T
};

Frame Decode(u32 fs, u32 ft)
{
	Frame f{};
	f.ma = (fs & 0x7FFFFFu) | 0x800000u;
	f.mb = (ft & 0x7FFFFFu) | 0x800000u;
	f.lt = f.ma < f.mb ? 1 : 0;
	const u64 num = static_cast<u64>(f.ma) << (23 + f.lt);
	f.T = static_cast<u32>(num / f.mb);
	f.rem = static_cast<u32>(num - static_cast<u64>(f.T) * f.mb);
	f.u = f.mb - f.rem;
	const s32 er = static_cast<s32>((fs >> 23) & 0xFF) - static_cast<s32>((ft >> 23) & 0xFF) +
				   127 - f.lt;
	f.down = (static_cast<u32>(er) << 23) | (f.T - 0x800000u);
	f.nu2_tp1 = 0;
	for (u32 x = f.T + 1u; !(x & 1u); x >>= 1)
		++f.nu2_tp1;
	return f;
}

// The carry-propagation distance in the trial product mb*(T+1), for the case
// odd(mb) = 3 -- the product is then 2^s * ((T+1) + 2*(T+1)) and a carry dies
// exactly where two adjacent bits of T+1 are both clear. Round 11b found this
// separates the undecided shell at mb = 0xC00000 with zero exceptions in
// 2,587,960 rows; see CarryDistanceSeparatesTheUndecidedShell for what it is
// and, just as importantly, what it is not.
int CarryDistance(u32 x)
{
	for (int i = 0; i < 40; ++i)
		if (!((x >> i) & 1u) && !((x >> (i + 1)) & 1u))
			return i;
	return 40;
}

// The cap the round-11 law puts on u, per branch.
u32 CapForBranch(u32 mb, int lt)
{
	if (!lt)
		return 1u << 22;
	return mb - (1u << 22) > (1u << 23) ? mb - (1u << 22) : (1u << 23);
}

// ---------------------------------------------------------------------------
// The truncation law's own witnesses. Measured by wit3.c; see note 6 above.
// ---------------------------------------------------------------------------
enum LawKind
{
	LAW_DIV_FIRES,   // u > cap, correct rounding says T+1, silicon says T
	LAW_SQRT_FIRES,  // u > 2^23, correct rounding says R+1, silicon says R
	LAW_SQRT_TIGHT,  // u == 2^23 exactly and silicon rounds UP
	LAW_SQRT_SILENT, // u <= 2^23 and silicon truncates anyway: the residual
};

struct LawRow
{
	LawKind kind;
	u32 fs, ft;   // fs is unused by the sqrt kinds and carries 1.0 there
	u32 console;  // wit3.c, this exact row
	u32 ieee;     // the correctly rounded value, which is what the JIT gives
	u32 u;        // the frame's u, recomputed by the tests and compared
};

constexpr LawRow kLawRows[] = {
	{LAW_DIV_FIRES,   0x3FAF934Fu, 0x3F9E3779u, 0x3F8E0B23u, 0x3F8E0B24u,  4194308u},
	{LAW_DIV_FIRES,   0x3FE8CA3Fu, 0x3FE8B4C4u, 0x3F800BD0u, 0x3F800BD1u,  4194308u},
	{LAW_DIV_FIRES,   0x3FFEC940u, 0x3FB504F3u, 0x3FB42937u, 0x3FB42938u,  4194344u},
	{LAW_DIV_FIRES,   0x3FE8B4C5u, 0x3FE8B4C4u, 0x3F800000u, 0x3F800001u,  6862020u},
	{LAW_DIV_FIRES,   0x3FB504F8u, 0x3FB504F3u, 0x3F800003u, 0x3F800004u,  5510092u},
	{LAW_DIV_FIRES,   0x3F9E3786u, 0x3F9E3779u, 0x3F80000Au, 0x3F80000Bu,  5005875u},

	{LAW_SQRT_FIRES,  0x3F800000u, 0x3F000002u, 0x3F3504F4u, 0x3F3504F5u,  9081465u},
	{LAW_SQRT_FIRES,  0x3F800000u, 0x3F000009u, 0x3F3504F9u, 0x3F3504FAu, 10273828u},
	{LAW_SQRT_FIRES,  0x3F800000u, 0x3F802734u, 0x3F801398u, 0x3F801399u,  8393073u},
	{LAW_SQRT_FIRES,  0x3F800000u, 0x3F802D45u, 0x3F8016A0u, 0x3F8016A1u,  8393025u},

	{LAW_SQRT_TIGHT,  0x3F800000u, 0x3F802001u, 0x3F801000u, 0x3F801000u,  8388608u},
	{LAW_SQRT_TIGHT,  0x3F800000u, 0x3F804007u, 0x3F802000u, 0x3F802000u,  8388608u},

	{LAW_SQRT_SILENT, 0x3F800000u, 0x3F000005u, 0x3F3504F6u, 0x3F3504F7u,  6202961u},
	{LAW_SQRT_SILENT, 0x3F800000u, 0x3F80092Eu, 0x3F800496u, 0x3F800497u,  1380625u},
};

constexpr int kLawRowCount = static_cast<int>(std::size(kLawRows));

// The sqrt frame: X = m << k with k picked by the exponent field's parity,
// R = floor(sqrt(X)), u = (R+1)^2 - X. Correct rounding takes R+1 when
// X > (R+0.5)^2, i.e. rem > R, and (R+0.5)^2 is never an integer so there is
// never a tie to break.
struct SqrtFrame
{
	u32 R, u;
	u64 rem;
	bool ieee_rounds_up;
};

SqrtFrame DecodeSqrt(u32 ft)
{
	const u32 E = (ft >> 23) & 0xFFu;
	const u64 X = static_cast<u64>(0x800000u | (ft & 0x7FFFFFu)) << ((E & 1u) ? 23 : 24);
	// Bit-by-bit integer square root. Deliberately not the host's sqrt with a
	// fixup, which is what FPU.cpp does: a test that reproduces the
	// implementation's seed cannot catch the seed being wrong.
	u64 R = 0;
	for (int b = 24; b >= 0; --b)
	{
		const u64 t = R | (1ull << b);
		if (t * t <= X)
			R = t;
	}
	SqrtFrame f{};
	f.R = static_cast<u32>(R);
	f.rem = X - R * R;
	f.u = static_cast<u32>(2 * R + 1 - f.rem);
	f.ieee_rounds_up = f.rem > R;
	return f;
}

bool ConsoleRoundedUp(const DivRow& r)
{
	const Frame f = Decode(r.fs, r.ft);
	EXPECT_TRUE(r.con_div == f.down || r.con_div == f.down + 1u)
		<< "the console result is neither T nor T+1 -- the frame is wrong for this row";
	return r.con_div == f.down + 1u;
}

} // namespace

// ---------------------------------------------------------------------------
// 0. The table describes the silicon it claims to. Cheap, and it catches a
//    mistyped constant before any of the interesting tests read meaning into
//    one.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, EveryRowIsEitherTOrTPlusOne)
{
	ASSERT_EQ(kRowCount, 21) << "row table truncated";
	for (const DivRow& r : kRows)
	{
		const Frame f = Decode(r.fs, r.ft);
		EXPECT_TRUE(r.con_div == f.down || r.con_div == f.down + 1u)
			<< std::hex << "fs=" << r.fs << " ft=" << r.ft << " console=" << r.con_div
			<< " T=" << f.down << " (" << r.what << ")";
		// and the ieee column really is the correctly-rounded one
		const bool ieee_up = 2ull * f.rem > f.mb || (2ull * f.rem == f.mb && (f.T & 1u));
		EXPECT_EQ(r.ieee_div, f.down + (ieee_up ? 1u : 0u))
			<< std::hex << "fs=" << r.fs << " ft=" << r.ft;
	}
}

// ---------------------------------------------------------------------------
// 1. The unit is not a rounding rule.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, TheErrorSpansNearlyAWholeUlpBothWays)
{
	// 1.0 / (1 + 2^-23): the exact quotient is 2.4e-7 ULP above T and silicon
	// returns T+1.
	const Frame hi = Decode(0x3F800000u, 0x3F800001u);
	EXPECT_EQ(hi.rem, 2u);
	EXPECT_EQ(RunDiv(0x3F800000u, 0x3F800001u), 0x3F7FFFFEu)
		<< "this tree rounds to nearest, which is T here";
	EXPECT_EQ(0x3F7FFFFFu, hi.down + 1u) << "the console value is T+1";

	// and on the SAME divisor, a row where the exact quotient is 0.96 of the
	// way to T+1 and silicon returns T.
	const Frame lo = Decode(0x3F852B38u, 0x3F800001u);
	EXPECT_EQ(lo.u, 338743u);
	EXPECT_EQ(0x3F852B36u, lo.down) << "the console value is T";
	EXPECT_EQ(RunDiv(0x3F852B38u, 0x3F800001u), 0x3F852B37u)
		<< "this tree rounds to nearest, which is T+1 here";

	// The signed error in ULP: returning T costs -rem/mb, returning T+1 gains
	// +u/mb. Round-to-nearest is confined to [-1/2, +1/2] and a directed mode
	// to one side of zero. These two rows are on the same divisor.
	const double err_hi = +static_cast<double>(hi.u) / hi.mb;
	const double err_lo = -static_cast<double>(lo.rem) / lo.mb;
	EXPECT_GT(err_hi, 0.999) << "silicon is nearly a whole ULP HIGH here";
	EXPECT_LT(err_lo, -0.95) << "silicon is nearly a whole ULP LOW here";
	EXPECT_EQ(hi.mb, lo.mb) << "and both are the same divisor";
}

// ---------------------------------------------------------------------------
// 2. The decision needs more than (branch, divisor, u, nu2(T+1)) -- which is
//    every coordinate any div model in this tree's history has used.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, TheDecisionNeedsMoreThanTheExactQuotient)
{
	const Frame up = Decode(0x3FC02C86u, 0x3FC00000u);
	const Frame dn = Decode(0x3FC00001u, 0x3FC00000u);

	ASSERT_EQ(up.mb, dn.mb) << "same divisor";
	ASSERT_EQ(up.lt, dn.lt) << "same branch";
	ASSERT_EQ(up.u, dn.u) << "same u";
	ASSERT_EQ(up.u, 1u << 22);
	ASSERT_EQ(up.rem, dn.rem) << "same exact quotient fraction, 2/3";
	ASSERT_EQ(3ull * up.rem, 2ull * up.mb);
	ASSERT_EQ(up.nu2_tp1, dn.nu2_tp1) << "same nu2(T+1)";
	ASSERT_EQ(up.nu2_tp1, 0);

	// Everything a threshold model can see is equal, and silicon disagrees.
	EXPECT_EQ(0x3F801DAFu, up.down + 1u) << "the first row rounds UP on silicon";
	EXPECT_EQ(0x3F800000u, dn.down) << "the second rounds DOWN on silicon";

	// The only coordinates left are T itself and the numerator.
	EXPECT_NE(up.T, dn.T);
	EXPECT_NE(up.ma, dn.ma);
}

// ---------------------------------------------------------------------------
// 3. What survives on the A>=B branch: the bound, AND its attainment.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, LawAHoldsOnEveryDivisorAndIsAttained)
{
	// Attainment. On mb = 0x800001 the single row with u = 2^22 exactly rounds
	// UP, so the rounding constant reaches 2^22 and a model that used anything
	// smaller would miss this row. Without this half the bound below passes
	// vacuously.
	const Frame at = Decode(0x3FC00001u, 0x3F800001u);
	ASSERT_EQ(at.mb & 1u, 1u) << "odd divisor";
	EXPECT_EQ(at.u, 1u << 22);
	EXPECT_EQ(0x3FC00000u, at.down + 1u) << "u = 2^22 rounds UP";

	// The bound. Every A>=B row that rounded UP across all twelve exhaustive
	// divisors had u <= 2^22: 12,585,601 UP rows, zero exceptions, attained
	// 14,344 times on seven of them, and the divisors span nu2(mb) of 0 through
	// 4 plus 22. Spot-checked
	// here on the A>=B rows of this table.
	int checked = 0;
	for (const DivRow& r : kRows)
	{
		const Frame f = Decode(r.fs, r.ft);
		if (f.lt)
			continue;  // the A<B analogue is a different statement -- see below
		++checked;
		if (!ConsoleRoundedUp(r))
			continue;
		EXPECT_LE(f.u, 1u << 22)
			<< std::hex << "fs=" << r.fs << " ft=" << r.ft
			<< ": an A>=B row rounded up above 2^22 (" << r.what << ")";
	}
	EXPECT_EQ(checked, 12) << "the A>=B population moved";
}

// ---------------------------------------------------------------------------
// 4. The A<B bound is CAPPED, not shifted with k -- and it is also attained.
//    The naive shift (u <= 2^23) looks right on nine of the twelve exhaustive
//    divisors and is wrong; the capped form is exception-free on all of them.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, TheAlbBoundIsCappedNotShifted)
{
	// The shifted form's counterexample. 262,318 rows on this divisor round up
	// with u > 2^23; the divisors that never do are the ones below 1.5 * 2^23,
	// which is why nine rounds of data never showed it.
	const Frame bad = Decode(0x3F80001Au, 0x3FE8B4C4u);
	ASSERT_EQ(bad.lt, 1);
	EXPECT_GT(bad.u, 1u << 23) << "u is above the shifted bound";
	EXPECT_EQ(bad.u, 10082692u);
	EXPECT_EQ(0x3F0CD031u, bad.down + 1u) << "and silicon rounded UP anyway";

	// The capped form. UP => u <= max(2^23, mb - 2^22), equivalently
	// rem >= min(mb - 2^23, 2^22): 0 violations in 28,425,919 A<B UP rows over
	// twelve exhaustive divisors. It holds on the row above:
	EXPECT_LE(bad.u, std::max(1u << 23, bad.mb - (1u << 22)));

	// ...and it is ATTAINED, on two different divisors, by rows whose remainder
	// is exactly 2^22. Without these the cap could be any larger number.
	for (u32 fs : {0x3F9FFC47u, 0x3F8DCE2Fu})
	{
		const DivRow* row = nullptr;
		for (const DivRow& r : kRows)
			if (r.fs == fs)
				row = &r;
		ASSERT_NE(row, nullptr);
		const Frame f = Decode(row->fs, row->ft);
		EXPECT_EQ(f.lt, 1);
		EXPECT_EQ(f.rem, 1u << 22) << "the remainder sits exactly on the cap";
		EXPECT_EQ(f.u, f.mb - (1u << 22));
		EXPECT_GT(f.u, 1u << 23) << "so the cap is the binding term here";
		EXPECT_EQ(row->con_div, f.down + 1u) << "and silicon rounds UP";
	}
}

// ---------------------------------------------------------------------------
// 5. RSQRT.S is SQRT.S then DIV.S, re-verified on silicon for these rows
//    (wit.c's last column: 12 of 12). rsqrt gets no model of its own; it
//    inherits whatever div and sqrt do.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, RsqrtIsStillSqrtThenDivideOnSilicon)
{
	// On silicon: rsqrt.s Fs, Ft and div.s Fs, (sqrt.s Ft) were measured as two
	// separate instructions and came out bit-identical on all twelve rows.
	for (const DivRow& r : kRows)
	{
		EXPECT_EQ(r.con_rsqrt, r.con_two_step)
			<< std::hex << "fs=" << r.fs << " ft=" << r.ft
			<< ": the console's rsqrt stopped being its own div of its own sqrt";
	}
	// The interpreter must do the same two steps.
	for (const DivRow& r : kRows)
	{
		const u32 s = RunSqrt(r.ft);
		EXPECT_EQ(RunRsqrt(r.fs, r.ft), RunDiv(r.fs, s))
			<< std::hex << "fs=" << r.fs << " ft=" << r.ft << " sqrt=" << s;
	}
}

// ---------------------------------------------------------------------------
// 5a. The UP set inside an undecided shell is isolated singletons. 0x3FC02C86
//     rounds UP; both of its class neighbours (the class is ma = 1 mod 3, so
//     they are +-3) round DOWN. All three agree on divisor, branch, u = 2^22
//     and the exact fraction 2/3. Any model whose error grows with the
//     numerator -- multiply by a fixed approximate reciprocal and truncate is
//     the obvious one -- makes the UP set a contiguous top slice, and dies
//     here. Measured exhaustively: 14,329 UP rows, 14,329 runs, longest 1.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, TheUpSetIsIsolatedSingletonsNotATopSlice)
{
	const Frame lo = Decode(0x3FC02C83u, 0x3FC00000u);
	const Frame mid = Decode(0x3FC02C86u, 0x3FC00000u);
	const Frame hi = Decode(0x3FC02C89u, 0x3FC00000u);

	// same class in every coordinate the old frame carries
	for (const Frame& f : {lo, mid, hi})
	{
		EXPECT_EQ(f.mb, 0xC00000u);
		EXPECT_EQ(f.lt, 0);
		EXPECT_EQ(f.u, 1u << 22);
		EXPECT_EQ(f.rem, 1u << 23);  // f = rem/mb = 2/3 exactly
		EXPECT_EQ(f.nu2_tp1, 0);
	}
	// consecutive members of the class, and only the middle one rounds up
	EXPECT_EQ(mid.ma - lo.ma, 3u);
	EXPECT_EQ(hi.ma - mid.ma, 3u);
	EXPECT_FALSE(ConsoleRoundedUp(kRows[12]));  // 0x3FC02C83
	EXPECT_TRUE(ConsoleRoundedUp(kRows[0]));    // 0x3FC02C86
	EXPECT_FALSE(ConsoleRoundedUp(kRows[13]));  // 0x3FC02C89
}

// ---------------------------------------------------------------------------
// 5b. What DOES separate that shell, at this divisor: the carry-propagation
//     distance in the trial product mb*(T+1). mb = 3*2^22, so the product is
//     (T+1) + 2*(T+1) and the carry dies at the first adjacent zero pair.
//     UP => distance >= 13 + lt, with zero exceptions in 2,587,960 shell rows
//     across both branches.
//
//     Scope this claim carefully -- it is a fact about THIS divisor. Six more
//     degenerate divisors (odd parts 5, 7, 9, 11, 13, 15) show the up-rate
//     rising monotonically with the same kind of carry distance, but the exact
//     cut moves with the odd part and "no 00 in T+1" has no cut at all on four
//     of them. The coordinate is real; this closed form is not general.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, CarryDistanceSeparatesTheUndecidedShell)
{
	const Frame up = Decode(0x3FC02C86u, 0x3FC00000u);
	const Frame stopped = Decode(0x3FC017FEu, 0x3FC00000u);

	EXPECT_GE(CarryDistance(up.T + 1u), 13);
	EXPECT_EQ(CarryDistance(stopped.T + 1u), 12);
	EXPECT_TRUE(ConsoleRoundedUp(kRows[0]));
	EXPECT_FALSE(ConsoleRoundedUp(kRows[14]));

	// and it is not a magnitude threshold in disguise: the row whose carry
	// stops early has the LARGEST possible low 13 bits of any such row, and it
	// still rounds down, while the row that rounds up has smaller ones.
	EXPECT_EQ((stopped.T + 1u) & 0x1FFFu, 0x0FFFu);
	EXPECT_LT((up.T + 1u) & 0x1FFFu, 0x1FFFu);
	EXPECT_GT((stopped.T + 1u) & 0x1FFFu, 0u);

	// every shell row in the table obeys the implication
	for (const DivRow& r : kRows)
	{
		const Frame f = Decode(r.fs, r.ft);
		if (f.mb != 0xC00000u || f.u != CapForBranch(f.mb, f.lt))
			continue;
		if (!ConsoleRoundedUp(r))
			continue;
		EXPECT_GE(CarryDistance(f.T + 1u), 13 + f.lt)
			<< std::hex << "fs=" << r.fs << " ft=" << r.ft;
	}
}

// ---------------------------------------------------------------------------
// 5c. The cap is NECESSARY and nowhere near SUFFICIENT. Over twelve exhaustive
//     divisors, u > cap rounds up 0 times in 41,335,987 rows -- but u < cap
//     rounds DOWN on 15,549,202 of 56,531,072 rows, 27.5%.
//
//     This is worth a test because mb = 0xC00000 makes the converse look true:
//     it has zero A>=B rows with u < cap, so it is structurally incapable of
//     showing the counterexample. A law checked only against a degenerate
//     divisor is a law checked against an empty cell.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, TheCapIsNecessaryAndNotSufficient)
{
	int under = 0;
	for (const DivRow& r : kRows)
	{
		const Frame f = Decode(r.fs, r.ft);
		const u32 cap = CapForBranch(f.mb, f.lt);
		if (ConsoleRoundedUp(r))
			EXPECT_LE(f.u, cap) << std::hex << "fs=" << r.fs << " ft=" << r.ft
								<< ": UP above the cap would refute the frame, not just "
								   "the model";
		if (f.u < cap && !ConsoleRoundedUp(r))
			++under;
	}
	// the two 0x3F900000 rows are here precisely to keep this non-zero
	EXPECT_GE(under, 2) << "no row with u strictly under the cap rounds down -- "
						   "the sufficiency counterexample has been lost";
	EXPECT_LT(Decode(0x3F900003u, 0x3F900000u).u, CapForBranch(0x900000u, 0));
	EXPECT_LT(Decode(0x3F90000Bu, 0x3F900000u).u, CapForBranch(0x900000u, 0));
}

// ---------------------------------------------------------------------------
// 5d. The A<B cap max(2^23, mb - 2^22) is an upper envelope, not the law: on
//     mb = 0xE00000 its whole shell of 898,779 rows rounds DOWN, and the
//     largest u that ever rounds up there is 2^23, two 2^20 steps lower. The
//     bound is also NOT MONOTONE in mb -- mb = 0xE8B4C4 reaches mb - 2^22
//     while the larger mb = 0xF00000 never exceeds 9*2^20 -- so no formula of
//     that shape can be tight.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, TheAlbCapIsAnEnvelopeNotTheLaw)
{
	const Frame at_cap = Decode(0x3F800005u, 0x3FE00000u);
	EXPECT_EQ(at_cap.lt, 1);
	EXPECT_EQ(at_cap.u, CapForBranch(0xE00000u, 1));
	EXPECT_FALSE(ConsoleRoundedUp(kRows[17])) << "the assumed cap is not attained here";

	const Frame real_max = Decode(0x3F802C7Cu, 0x3FE00000u);
	EXPECT_EQ(real_max.u, 1u << 23);
	EXPECT_LT(real_max.u, CapForBranch(0xE00000u, 1));
	EXPECT_TRUE(ConsoleRoundedUp(kRows[18]));

	const Frame second = Decode(0x3F80000Bu, 0x3FF00000u);
	EXPECT_EQ(second.u, CapForBranch(0xF00000u, 1));
	EXPECT_FALSE(ConsoleRoundedUp(kRows[19]));

	// non-monotone: a smaller divisor reaches a strictly larger bound
	EXPECT_LT(0xE8B4C4u, 0xF00000u);
	EXPECT_GT(0xE8B4C4u - (1u << 22), 9u << 20);
}

// ---------------------------------------------------------------------------
// 6. Where silicon and correct rounding agree, this tree must reproduce it.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, InterpMatchesConsoleWhereTheUnitIsExact)
{
	int checked = 0;
	for (const DivRow& r : kRows)
	{
		if (r.con_div != r.ieee_div)
			continue;
		++checked;
		EXPECT_EQ(RunDiv(r.fs, r.ft), r.con_div)
			<< std::hex << "fs=" << r.fs << " ft=" << r.ft << " (" << r.what << ")";
	}
	EXPECT_EQ(checked, 8);
}

// ---------------------------------------------------------------------------
// 7. Where they disagree, the miss is one ULP and this tree sits on the
//    correctly-rounded side. Reaching the console value here is not a failure,
//    it is the model landing -- and then the tripwire below is what to enable.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, SiliconIsOneUlpOffOnTheseRows)
{
	int off = 0;
	for (const DivRow& r : kRows)
	{
		if (r.con_div == r.ieee_div)
			continue;
		++off;
		const u32 a = r.con_div & 0x7FFFFFFFu, b = r.ieee_div & 0x7FFFFFFFu;
		EXPECT_EQ(a > b ? a - b : b - a, 1u) << std::hex << "fs=" << r.fs << " ft=" << r.ft;
		EXPECT_EQ(RunDiv(r.fs, r.ft), r.ieee_div)
			<< std::hex << "fs=" << r.fs << " ft=" << r.ft
			<< ": this tree computes the correctly-rounded value";
	}
	EXPECT_EQ(off, 13);
}

// ---------------------------------------------------------------------------
// 7b. The law that shipped, and the three things it needs pinned: that it
//     fires, that its bound is attained, and that where it stays silent the
//     tree is still on the correctly-rounded side.
//
//     The frame is recomputed from the operand bits for every row, so the
//     annotated `u` cannot drift away from the arithmetic, and a mistyped
//     operand shows up as a frame mismatch rather than as a mysterious value
//     failure.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, TheTruncationLawFiresAndTheInterpreterFollowsSilicon)
{
	int fires_div = 0, fires_sqrt = 0, tight = 0, silent = 0;

	for (const LawRow& r : kLawRows)
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "fs=" << r.fs << " ft=" << r.ft);

		if (r.kind == LAW_DIV_FIRES)
		{
			++fires_div;
			const Frame f = Decode(r.fs, r.ft);
			ASSERT_EQ(f.u, r.u) << "the annotated u does not match the frame";
			EXPECT_EQ(f.lt, 0) << "the A<B half of the cap cannot change an answer, "
								  "so a witness on that branch would be vacuous";
			EXPECT_GT(f.u, CapForBranch(f.mb, f.lt)) << "the law does not even fire here";
			EXPECT_GE(2ull * f.rem, static_cast<u64>(f.mb))
				<< "correct rounding already says T, so this row cannot show the law";
			EXPECT_EQ(r.console, f.down) << "silicon must be the TRUNCATED candidate";
			EXPECT_EQ(r.ieee, f.down + 1u);
			EXPECT_EQ(RunDiv(r.fs, r.ft), r.console)
				<< "the interpreter stopped applying the truncation law to DIV.S";
		}
		else
		{
			const SqrtFrame f = DecodeSqrt(r.ft);
			ASSERT_EQ(f.u, r.u) << "the annotated u does not match the frame";

			if (r.kind == LAW_SQRT_FIRES)
			{
				++fires_sqrt;
				EXPECT_GT(f.u, 1u << 23) << "the law does not even fire here";
				EXPECT_TRUE(f.ieee_rounds_up)
					<< "correct rounding already says R, so this row cannot show the law";
				EXPECT_NE(r.console, r.ieee);
				EXPECT_EQ(RunSqrt(r.ft), r.console)
					<< "the interpreter stopped applying the truncation law to SQRT.S";
			}
			else if (r.kind == LAW_SQRT_TIGHT)
			{
				++tight;
				EXPECT_EQ(f.u, 1u << 23) << "this row exists to sit ON the bound";
				EXPECT_TRUE(f.ieee_rounds_up);
				EXPECT_EQ(r.console, r.ieee)
					<< "silicon rounds UP at u = 2^23, which is what makes the bound "
					   "a measurement rather than a free inequality";
				EXPECT_EQ(RunSqrt(r.ft), r.console)
					<< "the law fired one unit too early -- the comparison must be "
					   "strictly greater than 2^23";
			}
			else
			{
				++silent;
				EXPECT_LE(f.u, 1u << 23) << "this row exists BELOW the bound";
				EXPECT_TRUE(f.ieee_rounds_up);
				EXPECT_NE(r.console, r.ieee) << "silicon truncates here even though the "
												"law is silent -- that is the residual";
				EXPECT_EQ(RunSqrt(r.ft), r.ieee)
					<< "where the law says nothing the tree stays correctly rounded; "
					   "reaching the console value means someone modelled more of the "
					   "unit, and this row should move to LAW_SQRT_FIRES";
			}
		}
	}

	EXPECT_EQ(fires_div, 6);
	EXPECT_EQ(fires_sqrt, 4);
	EXPECT_EQ(tight, 2);
	EXPECT_EQ(silent, 2);
	EXPECT_EQ(kLawRowCount, 14);
}

// The vacuity note from the header, asserted here: on the A<B branch the cap
// sits above mb/2 for every representable divisor, so
// u > cap already implies correct rounding says DOWN and that half of the law
// can never change a result. Arithmetic over the whole divisor domain, not a
// sample -- it is a statement about the bound's shape, and it is here so that
// anyone tightening the A<B cap sees immediately what would make it live.
TEST(EeFpuDivUnitExhaustive, TheAlbHalfOfTheCapCannotChangeAnAnswer)
{
	// Every representable divisor significand, not a sample: the loop body is
	// two comparisons, so exhaustive costs nothing and a strided version could
	// step over the counterexample it exists to look for.
	u32 checked = 0, violations = 0, worst_mb = 0, worst_margin = 0xFFFFFFFFu;
	for (u32 mb = 0x800000u; mb < 0x1000000u; ++mb)
	{
		const u32 cap = CapForBranch(mb, 1);
		++checked;
		if (cap <= mb / 2u)
		{
			++violations;
			worst_mb = mb;
		}
		if (cap - mb / 2u < worst_margin)
			worst_margin = cap - mb / 2u;
	}
	EXPECT_EQ(checked, 1u << 23);
	EXPECT_EQ(violations, 0u) << "mb=" << std::hex << worst_mb
							  << ": the A<B cap dropped to or below half the divisor, "
								 "so u > cap no longer implies that correct rounding "
								 "rounds down";
	// How much room there is, so "0 violations" is a size and not just a claim.
	// The minimum is 2^21, at mb = 3*2^22 where the two arms of the max() meet;
	// the A<B cap would have to come down by more than that before that half of
	// the law could change any result at all.
	EXPECT_EQ(worst_margin, 1u << 21) << "narrowest gap between the A<B cap and mb/2";

	// And the A>=B cap does NOT have that property, which is why that half is
	// the one doing the work.
	EXPECT_LE(CapForBranch(0xFFFFFFu, 0), 0xFFFFFFu / 2u);
}

// ---------------------------------------------------------------------------
// 8. The tripwire. Enabling it means someone modelled the unit. Any model that
//    passes it has to satisfy tests 1-4 above, which is the point of them.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitExhaustive, DISABLED_InterpMatchesConsoleOnEveryRow)
{
	for (const DivRow& r : kRows)
	{
		EXPECT_EQ(RunDiv(r.fs, r.ft), r.con_div)
			<< std::hex << "div.s fs=" << r.fs << " ft=" << r.ft << " (" << r.what << ")";
		EXPECT_EQ(RunSqrt(r.ft), r.con_sqrt) << std::hex << "sqrt.s ft=" << r.ft;
		EXPECT_EQ(RunRsqrt(r.fs, r.ft), r.con_rsqrt)
			<< std::hex << "rsqrt.s fs=" << r.fs << " ft=" << r.ft;
	}
}
