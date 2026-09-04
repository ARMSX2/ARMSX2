// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The divide unit's recurrence exists twice in FPU.cpp: the portable loop,
// which is the reference, and the arm64 form, which is what runs. These
// tests hold the second to the first over the significand pairs: random
// ones, and every dividend at a stride against divisors that include the
// ones the console rows were taken on.
//
// Both return the quotient in the form eeDivide normalises, and on A>=B the
// cap shortcut supplies a 0 for the 25th digit rather than reproducing it,
// so the comparison is on the 24 bits the caller keeps.
//
// The liveness clause is that the rows exercised produce both T and T+1 in
// both branches: the cap shortcut never returns T+1, so an up-rounded row is
// one the loop decided, and a test that only ever saw the shortcut agree
// with itself would pass with the loop broken.

#include "EeFpuModel.h"

#include <gtest/gtest.h>

namespace {

struct Xorshift64s
{
	u64 s;
	u64 Next()
	{
		u64 x = s;
		x ^= x >> 12;
		x ^= x << 25;
		x ^= x >> 27;
		s = x;
		return x * 0x2545F4914F6CDD1DULL;
	}
	u32 Significand() { return 0x800000u | static_cast<u32>(Next() & 0x7FFFFFu); }
};

// The 24 bits the caller keeps, and whether they are T+1.
struct Outcome
{
	u32 bits;
	bool up;
};

Outcome Keep(u32 ma, u32 mb, u32 q)
{
	const u32 lt = ma < mb ? 1u : 0u;
	const u32 T = static_cast<u32>((static_cast<u64>(ma) << (23 + lt)) / mb);
	const u32 bits = lt ? q : (q >> 1);
	return {bits, bits == T + 1u};
}

struct Tally
{
	u64 rows = 0;
	u64 up_lt = 0, down_lt = 0, up_ge = 0, down_ge = 0;
	u64 mismatches = 0;
	u32 first_ma = 0, first_mb = 0;

	void Row(u32 ma, u32 mb)
	{
		const Outcome ref = Keep(ma, mb, EeFpuModel::Internal::DivideSignificandPortable(ma, mb));
		const Outcome got = Keep(ma, mb, EeFpuModel::Internal::DivideSignificand(ma, mb));
		++rows;
		if (ref.bits != got.bits)
		{
			if (mismatches++ == 0)
			{
				first_ma = ma;
				first_mb = mb;
			}
			return;
		}
		if (ma < mb)
			++(ref.up ? up_lt : down_lt);
		else
			++(ref.up ? up_ge : down_ge);
	}

	void Check() const
	{
		EXPECT_EQ(mismatches, 0u) << "first at ma=" << std::hex << first_ma << " mb=" << first_mb;
		// liveness: the loop decided rows in both directions on both branches
		EXPECT_GT(up_lt, 0u);
		EXPECT_GT(down_lt, 0u);
		EXPECT_GT(up_ge, 0u);
		EXPECT_GT(down_ge, 0u);
	}
};

// The divisors the console rows and the modelling rounds were taken on, plus
// the ends and the middle of the range.
constexpr u32 kDivisors[] = {
	0x800000u, 0x800001u, 0x800003u, 0x8000FFu, 0x80FFFFu, 0x8E0E31u, 0x900000u, 0x9F0000u,
	0xA00000u, 0xA59AFFu, 0xAAAAAAu, 0xB504F3u, 0xB6885Bu, 0xBFFFFFu, 0xC00000u, 0xC00001u,
	0xC97D64u, 0xCCCCCCu, 0xD15210u, 0xDBBA9Cu, 0xE00000u, 0xE214E9u, 0xF00000u, 0xF7758Eu,
	0xFFFFFEu, 0xFFFFFFu,
};

} // namespace

TEST(EeFpuDivUnitArm64Form, MatchesThePortableLoopOnRandomPairs)
{
	Xorshift64s rng{0x9E3779B97F4A7C15ULL};
	Tally t;
	for (int i = 0; i < 2000000; ++i)
	{
		const u32 ma = rng.Significand();
		const u32 mb = rng.Significand();
		t.Row(ma, mb);
	}
	t.Check();
}

TEST(EeFpuDivUnitArm64Form, MatchesThePortableLoopAcrossTheDividendsOfTheWitnessDivisors)
{
	Tally t;
	for (u32 mb : kDivisors)
		for (u32 ma = 0x800000u; ma < 0x1000000u; ma += 31u)
			t.Row(ma, mb);
	t.Check();
}

// Every dividend against the witness divisors and as many again drawn at
// random. Minutes, so not in the default run.
TEST(EeFpuDivUnitArm64Form, DISABLED_MatchesThePortableLoopOnEveryDividend)
{
	Xorshift64s rng{0x2545F4914F6CDD1DULL};
	Tally t;
	for (int k = 0; k < 52; ++k)
	{
		const u32 mb = k < 26 ? kDivisors[k] : rng.Significand();
		for (u32 ma = 0x800000u; ma < 0x1000000u; ++ma)
			t.Row(ma, mb);
	}
	t.Check();
}
