// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu max-pass-draws CAP: a ceiling on how many draws one render pass may carry.
//
// It is not a choice between two arrangements the way the depth polarity is -- it is a limit. When a
// pass reaches it the planner closes it and opens another with the SAME key: same target pair, same
// attachments, same draws, same order, nothing re-sorted. So the cap moves frame time and moves no
// pixel, which is exactly the hazard class of the two depth-policy force keys -- large invisible
// effect, no visible symptom when it is set by accident.
//
// Why it exists: Adreno charges for pass LENGTH. Two instruments found the same number. A device
// pass-size sweep on an SD865 drove the cap from unset down to one draw and watched the driver's
// occlusion-autotune term (worth +9.05 ms on Armored Core 3) vanish by 64; a CPU-side census of the
// depth-policy A/B, on another machine with no device access, found that the fraction of a frame's
// fill relocated into passes of >= 64 draws is what ranks that A/B's per-title GPU cost, and swept
// the threshold to a peak at 64. Below 8 draws the sweep HARMS two of three dumps, so the cap is
// deliberately far above the knee rather than as small as it can be.
//
// What is pinned here, and why each one:
//
//  - The THREE-STATE setting. "Ask the device" and "force uncapped" are different instructions, and
//    a two-state key cannot give the second on a device that answers a cap -- which is precisely the
//    A/B the device round needs (Adreno, cap off).
//  - The BOUNDARY, at exactly the cap and not one draw either side, driven through the planner's own
//    gsTileGpuPassEnd rather than a re-implementation of it.
//  - That the split is a PARTITION: every draw in exactly one pass, in the original order, with no
//    pass empty. That is what "the split moves no pixel" rests on before any frame is rendered.
//  - COMPOSITION with the depth polarity, because the deciding device round is a four-arm crossing
//    of the two and an implementation where the cap only worked on one polarity would run two of
//    those arms as duplicates of the other two.
//  - The two PER-PASS mechanisms a split touches -- the prep-op range and the DATE snapshot. Those
//    two tests replicate the plan build's arithmetic (the cut is the real function; the consequence
//    is modelled), because the plan build itself needs a device to run.
//
// The snapshot argument in full, since it is the one place a pure split could have moved a pixel and
// the test below only pins half of it: a pass with a DATE draw snapshots its colour target before
// opening, so splitting a run of DATE draws gives the second pass a snapshot taken AFTER the first
// pass's output. That is still exact, because a DATE draw on the snapshot road already forces a pass
// break whenever anything since the run started wrote under its own rectangle (m_run_written, in
// AccumulateDraw, which is not reset by an ordinary key-change break and so spans the whole run). So
// no draw of the first half ever wrote a pixel the second half's DATE test reads, and the two
// snapshots agree everywhere either of them is looked at.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace
{
using DepthMode = GSDevice::GSTileGpuDepthMode;

constexpr GSTileSurfaceId kColor = 1;
constexpr GSTileSurfaceId kOtherColor = 2;
constexpr GSTileSurfaceId kZ = 3;

/// The two depth polarities, named so a failure says which one it was. Same spelling as the pass-key
/// suite's, because these tests cross the two policies and a second vocabulary for one bit is how
/// the two halves of a crossing come to disagree.
constexpr bool kMerged = false; ///< depth mode out of the key: the tiler answer, and the default
constexpr bool kUniform = true; ///< depth mode in the key: the Adreno answer
constexpr bool kShared = false; ///< readers may share a pass with non-readers: the default

/// The device answers a planner consults. Named rather than spelt as bare numbers so the setting
/// tests read as the questions they are.
constexpr u32 kDeviceUncapped = 0;
constexpr u32 kDeviceAdreno = 64;

/// The uncapped spelling, which is 0 and not "no value": every site takes 0 to mean no limit.
constexpr u32 kNoCap = 0;

/// One accumulated draw, in the fields the pass cut reads.
struct Row
{
	GSTileSurfaceId color = kColor;
	DepthMode depth = DepthMode::TestWrite;
	bool breaks = false; ///< the draw forces a break of its own (break_before)
};

/// The passes a draw list falls into, as [first, end) pairs, cut by the planner's OWN function.
std::vector<std::pair<u32, u32>> PassesOf(
	const std::vector<Row>& rows, u32 max_pass_draws, bool depth_uniform = kMerged)
{
	const auto key_at = [&](u32 d) {
		return gsTileGpuPassKeyFor(rows[d].color, kZ, rows[d].depth, depth_uniform, false, kShared);
	};
	const auto breaks_at = [&](u32 d) { return rows[d].breaks; };
	std::vector<std::pair<u32, u32>> out;
	const u32 count = static_cast<u32>(rows.size());
	for (u32 i = 0; i < count;)
	{
		const u32 j = gsTileGpuPassEnd(i, count, max_pass_draws, key_at, breaks_at);
		out.emplace_back(i, j);
		i = j;
	}
	return out;
}

/// A run of `n` draws nothing but the cap can separate.
std::vector<Row> UniformRun(u32 n)
{
	return std::vector<Row>(n, Row{});
}

/// Every draw in exactly one pass, in the original order, no pass empty, none longer than the cap.
/// The whole of "a pure split" as a checkable property, asserted by every test below that cuts.
void ExpectPartition(const std::vector<std::pair<u32, u32>>& passes, u32 count, u32 max_pass_draws)
{
	ASSERT_FALSE(passes.empty());
	u32 next = 0;
	for (const auto& [first, end] : passes)
	{
		EXPECT_EQ(first, next) << "a pass starts where the previous one ended, or draws were lost";
		EXPECT_GT(end, first) << "no pass is empty";
		if (max_pass_draws != kNoCap)
			EXPECT_LE(end - first, max_pass_draws) << "no pass exceeds the cap";
		next = end;
	}
	EXPECT_EQ(next, count) << "the passes cover every draw";
}
} // namespace

// ---------------------------------------------------------------------------------------------
// The setting: three states, because two cannot express the arm the device round needs.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuPassCap, ZeroAsksTheDevice)
{
	// The shipped arm. Whatever the device answers is what runs, including nothing.
	EXPECT_EQ(gsTileGpuMaxPassDraws(0, kDeviceAdreno), kDeviceAdreno);
	EXPECT_EQ(gsTileGpuMaxPassDraws(0, kDeviceUncapped), kNoCap);
}

TEST(TileGpuPassCap, NegativeForcesUncappedOverTheDevice)
{
	// The arm a two-state key could not express: an Adreno running with no cap, which is the control
	// half of the device round's crossing.
	EXPECT_EQ(gsTileGpuMaxPassDraws(-1, kDeviceAdreno), kNoCap);
	EXPECT_EQ(gsTileGpuMaxPassDraws(-64, kDeviceAdreno), kNoCap);
	EXPECT_EQ(gsTileGpuMaxPassDraws(-1, kDeviceUncapped), kNoCap);
}

TEST(TileGpuPassCap, PositivePinsThatCapOnAnyDevice)
{
	// ...and the other arm nobody else can reach: the cap on a machine whose device asks for none,
	// which is how the corpus A/B runs at all (the M2 answers uncapped).
	EXPECT_EQ(gsTileGpuMaxPassDraws(64, kDeviceUncapped), 64u);
	EXPECT_EQ(gsTileGpuMaxPassDraws(1, kDeviceUncapped), 1u);
	EXPECT_EQ(gsTileGpuMaxPassDraws(8, kDeviceAdreno), 8u) << "the setting outranks the device";
}

TEST(TileGpuPassCap, AFullPassAdmitsNothingMore)
{
	// The predicate both grouping sites share. Off-by-one here is a pass of 65 draws under a cap of
	// 64 on one side and 64 on the other, which is the two sites disagreeing about where a pass ends.
	EXPECT_TRUE(gsTileGpuPassAdmitsMore(0, kNoCap));
	EXPECT_TRUE(gsTileGpuPassAdmitsMore(1000000, kNoCap));
	EXPECT_TRUE(gsTileGpuPassAdmitsMore(63, 64));
	EXPECT_FALSE(gsTileGpuPassAdmitsMore(64, 64));
	EXPECT_FALSE(gsTileGpuPassAdmitsMore(65, 64));
	EXPECT_TRUE(gsTileGpuPassAdmitsMore(0, 1)) << "an empty pass always takes its first draw";
	EXPECT_FALSE(gsTileGpuPassAdmitsMore(1, 1));
}

// ---------------------------------------------------------------------------------------------
// The boundary: at exactly the cap.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuPassCap, UncappedLeavesTheRunWhole)
{
	// The off road, checked first: with no cap a run nothing else separates is ONE pass, however long.
	// This is what the shipped non-Adreno arm has to keep doing, and it is the arm the corpus
	// byte-identity gate compares against.
	const std::vector<Row> rows = UniformRun(500);
	const auto passes = PassesOf(rows, kNoCap);
	ASSERT_EQ(passes.size(), 1u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 500u));
}

TEST(TileGpuPassCap, ARunSplitsAtExactlyTheCap)
{
	// 200 draws under a cap of 64: three full passes and a remainder, cut at 64/128/192 and nowhere
	// else. The boundaries are the assertion -- "four passes" alone would pass on a planner that cut
	// at 50 each.
	const std::vector<Row> rows = UniformRun(200);
	const auto passes = PassesOf(rows, 64);
	ASSERT_EQ(passes.size(), 4u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 64u));
	EXPECT_EQ(passes[1], std::make_pair(64u, 128u));
	EXPECT_EQ(passes[2], std::make_pair(128u, 192u));
	EXPECT_EQ(passes[3], std::make_pair(192u, 200u));
	ExpectPartition(passes, 200, 64);
}

TEST(TileGpuPassCap, ARunExactlyTheCapIsNotSplit)
{
	// The fencepost on the other side: 64 draws under a cap of 64 is ONE pass. A planner that closed
	// the pass on reaching the cap rather than on the next draw would emit an empty second one.
	const auto passes = PassesOf(UniformRun(64), 64);
	ASSERT_EQ(passes.size(), 1u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 64u));
}

TEST(TileGpuPassCap, ACapAboveTheRunDoesNothing)
{
	const auto passes = PassesOf(UniformRun(10), 64);
	ASSERT_EQ(passes.size(), 1u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 10u));
}

TEST(TileGpuPassCap, ACapOfOneGivesEveryDrawItsOwnPass)
{
	// The degenerate end of the device sweep. Not a shipping value -- the sweep says caps below 8
	// harm two of three dumps -- but it is the arm the sweep's own bottom stop ran, so it has to work.
	const auto passes = PassesOf(UniformRun(5), 1);
	ASSERT_EQ(passes.size(), 5u);
	for (u32 d = 0; d < 5; d++)
		EXPECT_EQ(passes[d], std::make_pair(d, d + 1));
	ExpectPartition(passes, 5, 1);
}

TEST(TileGpuPassCap, TheCapNeverEXTENDSAPass)
{
	// A cap is a ceiling, not a target. Everything that cut a pass before still cuts it, at the same
	// draw, whether or not the cap would have allowed more.
	std::vector<Row> rows = UniformRun(10);
	rows[3].color = kOtherColor; // a different attachment: not negotiable by any policy
	rows[4].color = kOtherColor;
	rows[7].breaks = true; // a prep op that could not be hoisted
	const auto uncapped = PassesOf(rows, kNoCap);
	ASSERT_EQ(uncapped.size(), 4u);
	EXPECT_EQ(uncapped[0], std::make_pair(0u, 3u));
	EXPECT_EQ(uncapped[1], std::make_pair(3u, 5u));
	EXPECT_EQ(uncapped[2], std::make_pair(5u, 7u));
	EXPECT_EQ(uncapped[3], std::make_pair(7u, 10u));
	EXPECT_EQ(PassesOf(rows, 64), uncapped) << "a cap nothing reaches changes no boundary";
}

TEST(TileGpuPassCap, ABreakOnTheCapBoundaryIsOneBoundaryNotTwo)
{
	// The draw that hits the cap ALSO forces its own break. Both clauses cut in the same place, and
	// cutting twice there would leave a pass holding no draws -- which the executor would open and
	// close for nothing, and which the framebuffer-height backstop would then walk with an empty
	// range.
	std::vector<Row> rows = UniformRun(8);
	rows[4].breaks = true;
	const auto passes = PassesOf(rows, 4);
	ASSERT_EQ(passes.size(), 2u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 4u));
	EXPECT_EQ(passes[1], std::make_pair(4u, 8u));
	ExpectPartition(passes, 8, 4);
}

TEST(TileGpuPassCap, TheCapCountsDrawsNotPassesSoFarInTheFrame)
{
	// The count is per-pass and resets with the pass. A key change part way through resets it too, so
	// the run after the change gets its own full allowance rather than the remainder of the previous
	// one -- which is what BreakOpenPass clearing m_open_draw_count buys on the accumulation side.
	std::vector<Row> rows = UniformRun(10);
	for (u32 d = 3; d < 10; d++)
		rows[d].color = kOtherColor;
	const auto passes = PassesOf(rows, 4);
	ASSERT_EQ(passes.size(), 3u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 3u)); // ended by the colour change, at 3 draws
	EXPECT_EQ(passes[1], std::make_pair(3u, 7u)); // a fresh allowance of 4
	EXPECT_EQ(passes[2], std::make_pair(7u, 10u));
	ExpectPartition(passes, 10, 4);
}

// ---------------------------------------------------------------------------------------------
// Composition with the depth polarity: the four arms of the device round's crossing.
// ---------------------------------------------------------------------------------------------

namespace
{
/// Twelve draws alternating between two depth modes in runs of three. Under the merged polarity the
/// depth mode is out of the key and the whole thing is one run; under uniform it is four.
std::vector<Row> AlternatingDepth()
{
	std::vector<Row> rows = UniformRun(12);
	for (u32 d = 0; d < 12; d++)
		rows[d].depth = ((d / 3) % 2 == 0) ? DepthMode::TestWrite : DepthMode::TestNoWrite;
	return rows;
}
} // namespace

TEST(TileGpuPassCap, MergedUncappedIsOnePass)
{
	// Arm 1 of the crossing, and the thing the cap has to leave alone.
	const auto passes = PassesOf(AlternatingDepth(), kNoCap, kMerged);
	ASSERT_EQ(passes.size(), 1u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 12u));
}

TEST(TileGpuPassCap, UniformUncappedSplitsOnDepthStateOnly)
{
	// Arm 2. Four passes, cut by the depth mode and by nothing else.
	const auto passes = PassesOf(AlternatingDepth(), kNoCap, kUniform);
	ASSERT_EQ(passes.size(), 4u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 3u));
	EXPECT_EQ(passes[3], std::make_pair(9u, 12u));
}

TEST(TileGpuPassCap, MergedPlusCapIsMergedGroupingWithCappedPasses)
{
	// Arm 4, the unmeasured combination the whole lane exists to make runnable: the merged grouping's
	// pass-count collapse, with the cap deleting the giant passes it collapses INTO. Four passes of
	// three, from one pass of twelve -- not the uniform arm's four, which happen to be the same
	// ranges here by construction and are cut for a different reason (see below).
	const auto passes = PassesOf(AlternatingDepth(), 3, kMerged);
	ASSERT_EQ(passes.size(), 4u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 3u));
	EXPECT_EQ(passes[1], std::make_pair(3u, 6u));
	EXPECT_EQ(passes[2], std::make_pair(6u, 9u));
	EXPECT_EQ(passes[3], std::make_pair(9u, 12u));
	ExpectPartition(passes, 12, 3);

	// ...and that the two reasons really are independent: at a cap of 5 the merged arm cuts at 5 and
	// 10, which is a partition no depth polarity can produce.
	const auto coarse = PassesOf(AlternatingDepth(), 5, kMerged);
	ASSERT_EQ(coarse.size(), 3u);
	EXPECT_EQ(coarse[0], std::make_pair(0u, 5u));
	EXPECT_EQ(coarse[1], std::make_pair(5u, 10u));
	EXPECT_EQ(coarse[2], std::make_pair(10u, 12u));
}

TEST(TileGpuPassCap, UniformPlusCapCapsTheUniformGrouping)
{
	// Arm 3. The cap applies inside each depth-uniform run, not across them: a run of three under a
	// cap of two is 2 + 1, four times over.
	const auto passes = PassesOf(AlternatingDepth(), 2, kUniform);
	ASSERT_EQ(passes.size(), 8u);
	EXPECT_EQ(passes[0], std::make_pair(0u, 2u));
	EXPECT_EQ(passes[1], std::make_pair(2u, 3u)); // the depth mode changes at 3, not the cap
	EXPECT_EQ(passes[2], std::make_pair(3u, 5u));
	EXPECT_EQ(passes[3], std::make_pair(5u, 6u));
	ExpectPartition(passes, 12, 2);
}

TEST(TileGpuPassCap, TheFourArmsAreFourDistinctGroupings)
{
	// The point of the crossing, as one assertion: no two arms produce the same passes, so a device
	// round that runs all four is measuring four things and not two.
	const std::vector<Row> rows = AlternatingDepth();
	const auto uniform = PassesOf(rows, kNoCap, kUniform);
	const auto merged = PassesOf(rows, kNoCap, kMerged);
	const auto uniform_cap = PassesOf(rows, 2, kUniform);
	const auto merged_cap = PassesOf(rows, 2, kMerged);
	EXPECT_NE(uniform, merged);
	EXPECT_NE(uniform, uniform_cap);
	EXPECT_NE(merged, merged_cap);
	EXPECT_NE(uniform_cap, merged_cap);
}

// ---------------------------------------------------------------------------------------------
// What a split does to the two things the plan build keys per PASS.
//
// The cut below is the planner's own gsTileGpuPassEnd; the consequence is the plan build's
// arithmetic replicated here, because the plan build needs a device to run and this suite has none.
// ---------------------------------------------------------------------------------------------

namespace
{
/// Draw d's prep ops occupy [first_op(d), first_op(d) + op_count(d)). Accumulation appends each
/// draw's ops contiguously and in draw order, which is what makes a pass's range expressible as one
/// interval at all.
constexpr u32 kOpsPerDraw = 2;
constexpr u32 FirstOp(u32 d) { return d * kOpsPerDraw; }

/// A pass's prep range, exactly as BuildAndExecutePlan computes it: from the first draw's first op to
/// the end of the last draw's.
std::pair<u32, u32> PrepRange(std::pair<u32, u32> pass)
{
	const u32 first = FirstOp(pass.first);
	const u32 end = FirstOp(pass.second - 1) + kOpsPerDraw;
	return {first, end};
}
} // namespace

TEST(TileGpuPassCap, ASplitPartitionsThePrepOpsAndHoistsNothingFurther)
{
	// The executor runs a pass's whole prep range BEFORE the pass opens, so an op belonging to any
	// draw but the first is hoisted over the draws ahead of it. A split can only SHORTEN the distance
	// an op is hoisted -- it never moves an op into a pass whose draws do not own it, and it never
	// leaves one in no pass at all.
	const std::vector<Row> rows = UniformRun(10);
	const auto passes = PassesOf(rows, 4);
	ASSERT_EQ(passes.size(), 3u);

	u32 next_op = 0;
	for (const auto& pass : passes)
	{
		const auto [first_op, end_op] = PrepRange(pass);
		EXPECT_EQ(first_op, next_op) << "no op falls outside every pass's range";
		// Every op in the range belongs to a draw of this pass.
		EXPECT_EQ(first_op, FirstOp(pass.first));
		EXPECT_EQ(end_op, FirstOp(pass.second - 1) + kOpsPerDraw);
		next_op = end_op;
	}
	EXPECT_EQ(next_op, FirstOp(10)) << "the ranges cover every op";

	// ...and the hoist distance really does fall. Draw 9's ops sit at the head of the third pass,
	// ahead of one draw, where uncapped they would have been hoisted ahead of nine.
	const auto whole = PassesOf(rows, kNoCap);
	ASSERT_EQ(whole.size(), 1u);
	EXPECT_EQ(9u - whole[0].first, 9u);
	EXPECT_EQ(9u - passes[2].first, 1u);
}

namespace
{
/// Whether the pass [first, end) would take a DATE snapshot, on the plan build's rule: any draw of it
/// tests destination alpha, and the pass does not serve DATE through the in-pass read.
bool TakesSnapshot(const std::vector<bool>& date, std::pair<u32, u32> pass)
{
	for (u32 d = pass.first; d < pass.second; d++)
	{
		if (date[d])
			return true;
	}
	return false;
}
} // namespace

TEST(TileGpuPassCap, EachSplitPassTakesItsOwnDateSnapshot)
{
	// A run of DATE draws split by the cap takes one snapshot per split pass, each at that pass's own
	// head. The second one therefore sees the first pass's output -- and that is correct, not a
	// tolerated difference: a DATE draw on the snapshot road has already forced a break of its own if
	// anything since the run started wrote under it, so nothing the first half wrote is anywhere the
	// second half's test looks. See the file header for the argument in full.
	const std::vector<bool> date(9, true);
	const std::vector<Row> rows = UniformRun(9);

	const auto whole = PassesOf(rows, kNoCap);
	ASSERT_EQ(whole.size(), 1u);
	EXPECT_TRUE(TakesSnapshot(date, whole[0]));

	const auto split = PassesOf(rows, 4);
	ASSERT_EQ(split.size(), 3u);
	for (const auto& pass : split)
		EXPECT_TRUE(TakesSnapshot(date, pass)) << "pass starting at draw " << pass.first;
}

TEST(TileGpuPassCap, ASplitAwayFromTheDateDrawsTakesNoExtraSnapshot)
{
	// The other half: a snapshot is a property of the draws in the pass, so a split whose second half
	// holds no DATE draw takes no second copy. A cap that snapshotted per pass rather than per pass
	// WITH a DATE draw would charge every long pass in the frame a full-target copy.
	std::vector<bool> date(8, false);
	date[0] = true;
	const std::vector<Row> rows = UniformRun(8);
	const auto split = PassesOf(rows, 4);
	ASSERT_EQ(split.size(), 2u);
	EXPECT_TRUE(TakesSnapshot(date, split[0]));
	EXPECT_FALSE(TakesSnapshot(date, split[1]));
}

// ---------------------------------------------------------------------------------------------
// The union masks each pass computes over ITS draws.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuPassCap, ASplitNarrowsAPassUnionAndNeverWidensIt)
{
	// A pass's road/texel/self masks are the union over its draws, and the executor's fallback program
	// (when no per-draw variant stream is supplied) is compiled from them. A split can only remove
	// draws from a union, so every split pass's union is a SUBSET of the union it came from -- the
	// direction that is safe, because a superset renders correctly and slowly while a subset decodes
	// nothing. Pinned as the property, over the real cut.
	const std::vector<Row> rows = UniformRun(8);
	const std::vector<u32> road = {1, 1, 2, 2, 4, 4, 8, 8};

	const auto union_over = [&road](std::pair<u32, u32> pass) {
		u32 m = 0;
		for (u32 d = pass.first; d < pass.second; d++)
			m |= road[d];
		return m;
	};

	const auto whole = PassesOf(rows, kNoCap);
	ASSERT_EQ(whole.size(), 1u);
	const u32 whole_mask = union_over(whole[0]);
	EXPECT_EQ(whole_mask, 1u | 2u | 4u | 8u);

	const auto split = PassesOf(rows, 4);
	ASSERT_EQ(split.size(), 2u);
	for (const auto& pass : split)
	{
		const u32 m = union_over(pass);
		EXPECT_EQ(m & ~whole_mask, 0u) << "a split pass's union is a subset of the pass it came from";
	}
	EXPECT_EQ(union_over(split[0]), 1u | 2u);
	EXPECT_EQ(union_over(split[1]), 4u | 8u);
}
