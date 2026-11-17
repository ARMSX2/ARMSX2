// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The pass compiler's order and groups (GS/Renderers/Common/GSRunCompiler.h): the twin sort's windows and what it never
// moves, and each of the run machine's join rules as a group boundary.

#include "GS/Renderers/Common/GSRunCompiler.h"

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

using namespace GSRunCompiler;

namespace
{
	constexpr u32 Flags(u32 tfx, u32 lod, u32 atst = 0)
	{
		using namespace GSRunsPolicy;
		return (tfx << STATE_SHIFT[STATE_TFX]) | (lod << STATE_SHIFT[STATE_LOD]) | (atst << STATE_SHIFT[STATE_ATST]);
	}

	Row Cand(u32 fixed = 0, u32 way = 2, u32 flags = Flags(0, GSRunsPolicy::LOD_NONE), u8 tex = 0)
	{
		Row r;
		r.fixed = fixed;
		r.way = way;
		r.flags = flags;
		r.tex = tex;
		r.cand = true;
		r.area = 64;
		return r;
	}

	std::vector<u32> Order(const std::vector<Row>& rows, bool sort = true)
	{
		std::vector<u32> o(rows.size());
		SortTwins(rows.data(), static_cast<u32>(rows.size()), o.data(), sort);
		return o;
	}

	std::vector<u32> Sizes(const std::vector<Row>& rows, const Rules& rules = {})
	{
		std::vector<u32> o(rows.size());
		std::iota(o.begin(), o.end(), 0u);
		std::vector<Group> g;
		MakeGroups(rows.data(), o.data(), static_cast<u32>(rows.size()), rules, g);
		std::vector<u32> s;
		u32 next = 0;
		for (const Group& x : g)
		{
			EXPECT_EQ(x.first, next);
			EXPECT_EQ(x.merged, x.count > 1);
			next += x.count;
			s.push_back(x.count);
		}
		EXPECT_EQ(next, rows.size());
		return s;
	}

	Row Base()
	{
		Row r = Cand(0, 2);
		r.role = ROLE_BASE;
		return r;
	}
	Row Twin()
	{
		Row r = Cand(1, 2);
		r.role = ROLE_TWIN;
		return r;
	}
} // namespace

TEST(GSRunCompiler, TwinWindowDrawsBasesThenTwins)
{
	const std::vector<Row> rows = {Cand(), Base(), Twin(), Base(), Twin(), Base(), Twin(), Cand()};
	EXPECT_EQ(Order(rows), (std::vector<u32>{0, 1, 3, 5, 2, 4, 6, 7}));
	EXPECT_EQ(Order(rows, false), (std::vector<u32>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(GSRunCompiler, OnePairIsNotAWindow)
{
	const std::vector<Row> rows = {Base(), Twin(), Cand(), Base(), Twin()};
	EXPECT_EQ(Order(rows), (std::vector<u32>{0, 1, 2, 3, 4}));
}

TEST(GSRunCompiler, AnyOtherRowEndsTheWindow)
{
	// A base without its twin, or any other row between pairs, is a boundary: the sort moves twins only past bases.
	const std::vector<Row> rows = {Base(), Twin(), Base(), Twin(), Base(), Cand(), Base(), Twin(), Base(), Twin()};
	EXPECT_EQ(Order(rows), (std::vector<u32>{0, 2, 1, 3, 4, 5, 6, 8, 7, 9}));
}

TEST(GSRunCompiler, PinnedRowsNeverMove)
{
	std::vector<Row> rows = {Base(), Twin(), Base(), Twin(), Base(), Twin()};
	rows[3].pinned = true; // a second-pass row: the pair it is in ends the window, which leaves one pair on each side
	EXPECT_EQ(Order(rows), (std::vector<u32>{0, 1, 2, 3, 4, 5}));
	rows.insert(rows.begin(), {Base(), Twin()});
	EXPECT_EQ(Order(rows), (std::vector<u32>{0, 2, 1, 3, 4, 5, 6, 7}));
}

TEST(GSRunCompiler, SortedTwinsGroupApart)
{
	const std::vector<Row> rows = {Base(), Twin(), Base(), Twin(), Base(), Twin()};
	const std::vector<u32> o = Order(rows);
	std::vector<Group> g;
	MakeGroups(rows.data(), o.data(), 6, {}, g);
	ASSERT_EQ(g.size(), 2u);
	EXPECT_EQ(g[0].count, 3u);
	EXPECT_EQ(g[1].count, 3u);
	EXPECT_TRUE(g[0].merged && g[1].merged);
}

TEST(GSRunCompiler, ConsecutiveSameFixedRowsShareAGroup)
{
	EXPECT_EQ(Sizes({Cand(), Cand(), Cand()}), (std::vector<u32>{3}));
	EXPECT_EQ(Sizes({Cand(0), Cand(1), Cand(1), Cand(0)}), (std::vector<u32>{1, 2, 1}));
}

TEST(GSRunCompiler, ARowTheProgramCannotTakeIsAlone)
{
	std::vector<Row> rows = {Cand(), Cand(), Cand(), Cand()};
	rows[2].cand = false;
	EXPECT_EQ(Sizes(rows), (std::vector<u32>{2, 1, 1}));
}

TEST(GSRunCompiler, StrictKeyIsABoundary)
{
	std::vector<Row> rows = {Cand(), Cand(), Cand()};
	rows[2].strict = 0x80000001u;
	EXPECT_EQ(Sizes(rows), (std::vector<u32>{2, 1}));
}

TEST(GSRunCompiler, WaysThatShareNoEquationAreABoundary)
{
	// A unit-equation way (6) beside a shader-blend way (0).
	EXPECT_EQ(Sizes({Cand(0, 6), Cand(0, 6), Cand(0, 0)}), (std::vector<u32>{2, 1}));
	EXPECT_EQ(Sizes({Cand(0, 2), Cand(0, 6), Cand(0, 2)}), (std::vector<u32>{3}));
}

TEST(GSRunCompiler, OneSamplingCallIsAGroupBoundaryOnlyWhereTheDriverNeedsIt)
{
	using GSRunsPolicy::LOD_HARDWARE;
	using GSRunsPolicy::LOD_MANUAL;
	const std::vector<Row> rows = {Cand(0, 2, Flags(0, LOD_HARDWARE)), Cand(0, 2, Flags(0, LOD_HARDWARE)),
		Cand(0, 2, Flags(0, LOD_MANUAL)), Cand(0, 2, Flags(4, 0))};
	Rules turnip;
	turnip.one_call = true;
	turnip.device = &GSRunsPolicy::DEVICE_CLASSES[4]; // G57's prices never split at dear for these areas
	EXPECT_EQ(Sizes(rows, turnip), (std::vector<u32>{2, 2}));
	Rules other = turnip;
	other.one_call = false;
	EXPECT_EQ(Sizes(rows, other), (std::vector<u32>{4}));
}

TEST(GSRunCompiler, DearSplitWeighsTheGroupsFill)
{
	using GSRunsPolicy::LOD_NONE;
	std::vector<Row> rows = {Cand(0, 2, Flags(0, LOD_NONE)), Cand(0, 2, Flags(0, LOD_NONE, 1))}; // the second alpha-tests
	Rules r; // Adreno 650: 38 ps a pixel against 900 ns a host draw: splits past 23,684 pixels
	rows[0].area = rows[1].area = 1000;
	EXPECT_EQ(Sizes(rows, r), (std::vector<u32>{2}));
	rows[0].area = rows[1].area = 20000;
	EXPECT_EQ(Sizes(rows, r), (std::vector<u32>{1, 1}));
	// Once a group holds both classes it splits no more.
	rows = {Cand(0, 2, Flags(0, LOD_NONE)), Cand(0, 2, Flags(0, LOD_NONE, 1)), Cand(0, 2, Flags(0, LOD_NONE))};
	rows[0].area = rows[1].area = 1000;
	rows[2].area = 100000;
	EXPECT_EQ(Sizes(rows, r), (std::vector<u32>{3}));
}

TEST(GSRunCompiler, TextureArrayAndTableWindowAreBoundaries)
{
	Rules r;
	r.max_textures = 2;
	EXPECT_EQ(Sizes({Cand(0, 2, Flags(0, 0), 0), Cand(0, 2, Flags(0, 0), 1), Cand(0, 2, Flags(0, 0), 0),
				  Cand(0, 2, Flags(0, 0), 2)},
				  r),
		(std::vector<u32>{3, 1}));
	r = {};
	r.max_draws = 2;
	EXPECT_EQ(Sizes({Cand(), Cand(), Cand(), Cand(), Cand()}, r), (std::vector<u32>{2, 2, 1}));
}
