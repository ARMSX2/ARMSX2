// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The per-frame DEPTH-PASS PREDICTOR: which of the two depth polarities the planner runs a frame
// under, decided from the frame before it (EmuCore/GS/TileGpuAdaptiveDepthPasses).
//
// Why it exists rather than a flat default. A full-corpus device round (SD865, Adreno 650, Turnip,
// 19 dumps, both arms, `-loop 10`) put merging ahead on 7 titles, level on 5 and behind on 7:
// Xenosaga -29.5% against Baldur's Gate: Dark Alliance II +7.2%. So there is no flat answer to flip
// to, and the question becomes which scenes merging is for.
//
// What separates them is not how much of the pass structure collapses and not how many passes go
// away. It is how many passes go away PER DRAW: `(uniform_passes - merged_passes) / draws`, the
// fraction of a frame's draws that stop opening a pass when the depth mode leaves the pass key.
// Ranked by it the seven winners are the top seven of nineteen, and the eighth place is a title
// merging ties on -- winners 0.084 (Beyond Good & Evil) to 0.841 (Xenosaga), losers 0.002 (Dirge of
// Cerberus) to 0.033 (Armored Core 3). The threshold sits in that gap.
//
// The two candidates that fail are pinned below too, because "we tried the obvious thing" is exactly
// what gets re-tried: the collapse RATIO puts BG&E (a win) beneath BGDA2 (the round's cleanest
// loss), and the ABSOLUTE pass saving puts BG&E beneath Armored Core 3 (a loss).
//
// What is pinned here, and why each one:
//
//  - The METRIC's shape -- that it is per-draw, on the corpus's own numbers, including the two
//    orderings that make the other two candidates unusable.
//  - The THRESHOLD's placement in the measured gap, from both sides.
//  - HYSTERESIS: the band the policy has to cross back through, and that a frame sitting inside it
//    keeps whatever polarity it has. The polarity is a pass-key input, so an oscillating decision
//    re-cuts every pass in the frame twice a frame for nothing.
//  - The CONFIRMATION count: one anomalous frame may not move the policy, two agreeing frames may.
//  - The START state -- uniform, which is what ships on the device this decides for. The predictor
//    may only ever move a frame off the shipped arrangement after seeing evidence, never before.
//  - That the counterfactual pass count is the PLAN BUILD's own cut (gsTileGpuCountPasses is
//    gsTileGpuPassEnd in a loop), because a count that drifts from the plan is a metric measuring
//    a grouping nothing would have built.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
using DepthMode = GSDevice::GSTileGpuDepthMode;

constexpr GSTileSurfaceId kColor = 1;
constexpr GSTileSurfaceId kZ = 3;

// Same spelling as the pass-key and pass-cap suites'. A second vocabulary for one bit is how two
// halves of a crossing come to disagree.
constexpr bool kMerged = false; ///< depth mode out of the key
constexpr bool kUniform = true; ///< depth mode in the key: the Adreno answer, and the start state
constexpr bool kShared = false; ///< readers may share a pass with non-readers

constexpr u32 kNoCap = 0;
constexpr u32 kCap64 = 64; ///< the shipped Adreno cap, present in both arms of the corpus round

/// One accumulated draw, in the fields the pass cut reads.
struct Row
{
	DepthMode depth = DepthMode::TestWrite;
	bool breaks = false;
};

u32 PassesUnder(const std::vector<Row>& rows, bool depth_uniform, u32 max_pass_draws = kNoCap)
{
	return gsTileGpuCountPasses(
		static_cast<u32>(rows.size()), max_pass_draws,
		[&](u32 d) { return gsTileGpuPassKeyFor(kColor, kZ, rows[d].depth, depth_uniform, false, kShared); },
		[&](u32 d) { return rows[d].breaks; });
}

/// Alternating depth modes: the population the polarity is about. Under uniform every draw opens its
/// own pass; under merged the whole run is one.
std::vector<Row> AlternatingDepth(u32 n)
{
	std::vector<Row> rows(n);
	for (u32 i = 0; i < n; i++)
		rows[i].depth = (i & 1) ? DepthMode::TestNoWrite : DepthMode::TestWrite;
	return rows;
}

/// Drive the picker over `frames` identical frames and hand back its state.
GSTileGpuDepthPolicyPicker RunFrames(u32 frames, u32 uniform_passes, u32 merged_passes, u32 draws,
	GSTileGpuDepthPolicyPicker picker = {})
{
	for (u32 i = 0; i < frames; i++)
		picker.Observe(uniform_passes, merged_passes, draws);
	return picker;
}

/// The corpus round's own structural numbers, per drawn frame, median of three reps
/// (`perf/tilegpu-flipgate-19dump-sd865-2026-08-24` for the merged arm, the
/// `20260824-1858-1049069a70` suite's tilegpu arm for the shipped uniform one). Rounded to whole
/// passes and whole draws, which is what the planner counts.
struct CorpusTitle
{
	const char* name;
	u32 uniform_passes;
	u32 merged_passes;
	u32 draws;
	bool merging_wins; ///< the device's verdict, frame time, merged vs shipped
};

// The nine titles the round called SOLID -- outside their own reps' spread and consistent across
// all three. The four weak or sign-unstable ones are deliberately absent: a threshold trained to
// force those is a threshold trained on noise.
constexpr CorpusTitle kSolid[] = {
	{"xenosaga", 5877, 209, 6737, true},   // -29.54%
	{"sotc-gate", 1335, 144, 1743, true},  //  -7.69%
	{"outrun-b", 687, 263, 939, true},     //  -5.20%
	{"sotc", 595, 108, 1043, true},        //  -4.82%
	{"ac5", 424, 100, 454, true},          //  -4.45%
	{"bge", 89, 81, 96, true},             //  -3.95%  the tightest win
	{"katamari-a", 37, 35, 69, false},     //  +5.18%
	{"dirge", 1112, 1109, 1550, false},    //  +5.54%
	{"bgda2", 18, 15, 481, false},         //  +7.21%  the cleanest loss
};

// ...and the ones that must not be forced either way. Every one of these is expected to come out
// UNIFORM, which is the shipped arrangement: abstaining is the conservative answer, and a predictor
// that captures only the clear winners is the point.
constexpr CorpusTitle kAbstain[] = {
	{"ac3", 108, 54, 1619, false},         //  +3.67%, and it WON an earlier round: sign-unstable
	{"gt4", 458, 431, 1359, false},        //  +2.59%, small
	{"katamari-b", 38, 34, 208, false},    //  +1.66%, inside its own spread
	{"yugioh", 55, 52, 133, false},        //  +1.84%, inside its own spread
	{"flatout2", 503, 453, 2543, false},   //  -0.01%, tie
	{"gt4opb", 936, 920, 1748, false},     //  +0.24%, tie
	{"rcuya-effects", 464, 454, 1895, false}, // -0.11%, tie
	{"rcuya-gameplay", 104, 103, 1614, false}, // +0.85%, tie
};

/// The metric, as a double, for reading a corpus row. The decision itself never computes this --
/// it compares integers -- but a test that says which side of the gap a title falls on wants it.
double Metric(const CorpusTitle& t)
{
	return static_cast<double>(t.uniform_passes - t.merged_passes) / static_cast<double>(t.draws);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// The counterfactual count: the plan build's own cut, run over a grouping the frame did not take.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuDepthPolicy, CountingIsThePlanBuildsOwnCut)
{
	// Sixteen draws alternating between two depth-carrying modes on one target pair. Under merged
	// they are one pass; under uniform each draw's mode differs from its neighbour's, so each opens
	// its own. Nothing else in the key moves.
	const std::vector<Row> rows = AlternatingDepth(16);
	EXPECT_EQ(PassesUnder(rows, kMerged), 1u);
	EXPECT_EQ(PassesUnder(rows, kUniform), 16u);
}

TEST(TileGpuDepthPolicy, CountingComposesWithTheCapAndWithBreaks)
{
	// The cap is a ceiling on either polarity, not a property of one: 100 mergeable draws under a
	// 64-cap are two passes, and under uniform the cap never binds because the modes already split.
	const std::vector<Row> rows = AlternatingDepth(100);
	EXPECT_EQ(PassesUnder(rows, kMerged, kCap64), 2u);
	EXPECT_EQ(PassesUnder(rows, kUniform, kCap64), 100u);

	// A draw's own break_before cuts both groupings, which is why the metric is a DIFFERENCE: a
	// break the frame would have taken anyway cancels out of it.
	std::vector<Row> broken = AlternatingDepth(16);
	broken[8].breaks = true;
	EXPECT_EQ(PassesUnder(broken, kMerged), 2u);
	EXPECT_EQ(PassesUnder(broken, kUniform), 16u);
}

TEST(TileGpuDepthPolicy, AnEmptyDrawListCutsIntoNoPasses)
{
	EXPECT_EQ(PassesUnder({}, kMerged), 0u);
	EXPECT_EQ(PassesUnder({}, kUniform), 0u);
}

// ---------------------------------------------------------------------------------------------
// The threshold, against the corpus round it was read off.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuDepthPolicy, SeparatesEverySolidCorpusTitle)
{
	for (const CorpusTitle& t : kSolid)
	{
		// From the uniform side, which is the side that actually asks: the predictor starts uniform
		// and only the ON threshold can move it.
		EXPECT_EQ(gsTileGpuWantsMergedDepthPasses(t.uniform_passes, t.merged_passes, t.draws, false),
			t.merging_wins)
			<< t.name << ": metric " << Metric(t) << " against the ON threshold";
		// ...and from the merged side, so no solid title lands INSIDE the band, where the verdict
		// would depend on which polarity the scene happened to arrive in.
		EXPECT_EQ(gsTileGpuWantsMergedDepthPasses(t.uniform_passes, t.merged_passes, t.draws, true),
			t.merging_wins)
			<< t.name << ": metric " << Metric(t) << " against the OFF threshold";
	}
}

TEST(TileGpuDepthPolicy, AbstainsOnEveryWeakAndUnstableTitle)
{
	// None of these is asserted to be a loss -- four are ties and one won a previous round. What is
	// asserted is that the predictor leaves them exactly where they ship.
	for (const CorpusTitle& t : kAbstain)
	{
		EXPECT_FALSE(gsTileGpuWantsMergedDepthPasses(t.uniform_passes, t.merged_passes, t.draws, false))
			<< t.name << ": metric " << Metric(t) << " must not reach the ON threshold";
	}
}

TEST(TileGpuDepthPolicy, TheThresholdSitsInsideTheMeasuredGap)
{
	// The gap the corpus leaves: everything that must go merged is above 1/16, everything that must
	// stay uniform is below it, and the two nearest points are 2.5x apart. Stated as the two
	// bounding titles rather than as a number, so a threshold change has to face them.
	//   bge (a win):  8 passes saved over 96 draws  = 0.0833
	//   ac3 (a loss): 54 passes saved over 1619     = 0.0334
	EXPECT_TRUE(gsTileGpuWantsMergedDepthPasses(89, 81, 96, false)) << "bge is the tightest win";
	EXPECT_FALSE(gsTileGpuWantsMergedDepthPasses(108, 54, 1619, false)) << "ac3 is the nearest abstainer";

	// The threshold is exactly 1/16 and inclusive: 4 passes saved over 64 draws crosses, 63 draws
	// short of it does not.
	EXPECT_TRUE(gsTileGpuWantsMergedDepthPasses(4, 0, 64, false));
	EXPECT_FALSE(gsTileGpuWantsMergedDepthPasses(4, 0, 65, false));
}

TEST(TileGpuDepthPolicy, TheTwoRejectedCandidatesAreWhyTheMetricIsPerDraw)
{
	// The collapse RATIO. BG&E wins with 89/81 = 1.10; BGDA2 loses with 18/15 = 1.20. Any ratio
	// threshold that takes BG&E takes BGDA2 with it.
	const double bge_ratio = 89.0 / 81.0;
	const double bgda2_ratio = 18.0 / 15.0;
	EXPECT_LT(bge_ratio, bgda2_ratio) << "the ratio orders a win below the round's cleanest loss";

	// The ABSOLUTE saving. BG&E wins on 8 passes; Armored Core 3 loses on 54.
	EXPECT_LT(89u - 81u, 108u - 54u) << "the absolute saving orders a win below a loss";

	// Per draw, both invert -- which is the whole claim.
	const CorpusTitle& bge = kSolid[5];
	const CorpusTitle& bgda2 = kSolid[8];
	const CorpusTitle& ac3 = kAbstain[0];
	ASSERT_STREQ(bge.name, "bge");
	ASSERT_STREQ(bgda2.name, "bgda2");
	ASSERT_STREQ(ac3.name, "ac3");
	EXPECT_GT(Metric(bge), Metric(bgda2)) << "bge over bgda2, per draw";
	EXPECT_GT(Metric(bge), Metric(ac3)) << "bge over ac3, per draw";
}

TEST(TileGpuDepthPolicy, AFrameWithNoDrawsDoesNotVote)
{
	// A frame that drew nothing says nothing about either arrangement, and dividing by it would
	// make every idle frame a maximal signal.
	EXPECT_FALSE(gsTileGpuWantsMergedDepthPasses(0, 0, 0, false));
	EXPECT_TRUE(gsTileGpuWantsMergedDepthPasses(0, 0, 0, true));
}

TEST(TileGpuDepthPolicy, MergedNeverCountsAsSavingLessThanNothing)
{
	// Merging cannot ADD passes -- it can only remove key fields -- but the counterfactual side is
	// estimated from break flags the other polarity would not have produced identically, so the
	// subtraction has to be floor-clamped rather than trusted to stay positive.
	EXPECT_FALSE(gsTileGpuWantsMergedDepthPasses(10, 400, 100, false));
	EXPECT_FALSE(gsTileGpuWantsMergedDepthPasses(10, 400, 100, true));
}

// ---------------------------------------------------------------------------------------------
// Hysteresis and stickiness.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuDepthPolicy, StartsUniformWhateverTheFirstFrameLooksLike)
{
	// Frame one runs before any evidence exists, so it runs the shipped arrangement. Even Xenosaga,
	// the round's biggest win by a factor of four, does not get frame one.
	GSTileGpuDepthPolicyPicker picker;
	EXPECT_FALSE(picker.merged);
	EXPECT_FALSE(picker.Observe(5877, 209, 6737)) << "one frame is not a sustained signal";
	EXPECT_EQ(picker.switches, 0u);
}

TEST(TileGpuDepthPolicy, TakesTwoAgreeingFramesToMove)
{
	GSTileGpuDepthPolicyPicker picker;
	EXPECT_FALSE(picker.Observe(5877, 209, 6737));
	EXPECT_TRUE(picker.Observe(5877, 209, 6737)) << "the second agreeing frame is the one that moves it";
	EXPECT_EQ(picker.switches, 1u);
	EXPECT_TRUE(picker.Observe(5877, 209, 6737)) << "and it stays without switching again";
	EXPECT_EQ(picker.switches, 1u);
}

TEST(TileGpuDepthPolicy, AnEmptyFrameIsNeutralAndNotAgreement)
{
	// ⚠️ The regression that cost the two biggest wins in the corpus. Xenosaga and MGS3 present a
	// drawn frame and an empty one alternately (6750 draws, 0, 6725, 0, ...). An empty frame that
	// counts as agreement clears the confirmation, so two consecutive votes never accumulate and the
	// policy is pinned to uniform for ever -- measured at 0/8 frames merged with a mean metric of
	// 0.84, on the title merging wins by 29.5%.
	GSTileGpuDepthPolicyPicker picker;
	EXPECT_FALSE(picker.Observe(5877, 209, 6737));
	EXPECT_FALSE(picker.Observe(0, 0, 0)) << "the empty frame between them";
	EXPECT_TRUE(picker.Observe(5877, 209, 6737)) << "the second drawn frame still moves it";
	EXPECT_EQ(picker.switches, 1u);

	// ...and an empty frame is not in the census either: a merged-frame share has to name frames
	// that had pixels in them.
	EXPECT_EQ(picker.frames, 2u);
	EXPECT_EQ(picker.draws, 2u * 6737);
}

TEST(TileGpuDepthPolicy, OneAnomalousFrameDoesNotMoveIt)
{
	// A load screen, a full-screen wipe, one frame of a cutscene: the shape the confirmation count
	// exists for. The policy must survive it in both directions.
	GSTileGpuDepthPolicyPicker picker = RunFrames(4, 5877, 209, 6737);
	ASSERT_TRUE(picker.merged);
	EXPECT_TRUE(picker.Observe(1112, 1109, 1550)) << "one frame far below the OFF threshold";
	EXPECT_TRUE(picker.Observe(5877, 209, 6737)) << "and the scene resumes";
	EXPECT_EQ(picker.switches, 1u) << "the anomaly cost no switch";
}

TEST(TileGpuDepthPolicy, TheBandIsWhatStopsAFrameOnTheThresholdOscillating)
{
	// A frame inside the band -- past 1/32, short of 1/16 -- asks for nothing. It cannot start a
	// merge from uniform, and it cannot end one from merged. That is the entire purpose of two
	// thresholds instead of one: with a single threshold this frame flips the policy every frame,
	// re-cutting every pass in it twice.
	constexpr u32 kDraws = 1024;
	constexpr u32 kInsideBand = 48; // 48/1024 = 1/21.3: above 1/32, below 1/16

	GSTileGpuDepthPolicyPicker from_uniform;
	for (u32 i = 0; i < 8; i++)
		EXPECT_FALSE(from_uniform.Observe(kInsideBand, 0, kDraws));
	EXPECT_EQ(from_uniform.switches, 0u);

	GSTileGpuDepthPolicyPicker from_merged = RunFrames(4, 5877, 209, 6737);
	ASSERT_TRUE(from_merged.merged);
	for (u32 i = 0; i < 8; i++)
		EXPECT_TRUE(from_merged.Observe(kInsideBand, 0, kDraws));
	EXPECT_EQ(from_merged.switches, 1u) << "the merge it arrived with, and nothing since";
}

TEST(TileGpuDepthPolicy, FallsBackOnlyBelowTheOffThreshold)
{
	GSTileGpuDepthPolicyPicker picker = RunFrames(4, 5877, 209, 6737);
	ASSERT_TRUE(picker.merged);
	// Dirge of Cerberus: 3 passes saved over 1550 draws, far under 1/32.
	EXPECT_TRUE(picker.Observe(1112, 1109, 1550)) << "one frame is not enough in this direction either";
	EXPECT_FALSE(picker.Observe(1112, 1109, 1550));
	EXPECT_EQ(picker.switches, 2u);
}

// ---------------------------------------------------------------------------------------------
// The census the device gate reads.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuDepthPolicy, CensusCountsFramesAsTheyRanNotAsTheyWereDecided)
{
	// The frame that triggers the switch RAN uniform -- the new polarity applies to the next one --
	// so a merged-frame count that included it would name a frame whose pixels came out uniform.
	GSTileGpuDepthPolicyPicker picker = RunFrames(5, 5877, 209, 6737);
	EXPECT_EQ(picker.frames, 5u);
	EXPECT_EQ(picker.frames_merged, 3u) << "frames 1 and 2 ran uniform; 3, 4 and 5 ran merged";
	EXPECT_EQ(picker.switches, 1u);
}

TEST(TileGpuDepthPolicy, CensusCarriesTheMetricItDecidedOn)
{
	GSTileGpuDepthPolicyPicker picker = RunFrames(4, 89, 81, 96);
	EXPECT_EQ(picker.saved_passes, 32u);
	EXPECT_EQ(picker.draws, 384u);
	EXPECT_NEAR(picker.MeanMetric(), 8.0 / 96.0, 1e-12);

	// ...and reads zero rather than dividing by nothing on a run that never drew.
	EXPECT_EQ(GSTileGpuDepthPolicyPicker{}.MeanMetric(), 0.0);
}
