// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSRegs.h"
#include "GS/Renderers/Tile/GSTileOracle.h"

#include <gtest/gtest.h>

#include <vector>

// The per-draw oracle's comparison core. It is the part that decides what a divergence
// IS, so it gets pinned here rather than trusted: the three-way split (which arm moved
// the pixel) is the whole reason the instrument exists, and getting it backwards would
// silently relabel every coverage defect as an arithmetic one.

using GSTileOracle::Compare;
using GSTileOracle::Metric;
using GSTileOracle::Tally;

namespace
{
	Tally Score(const std::vector<u32>& pre, const std::vector<u32>& sw, const std::vector<u32>& gpu,
		Metric metric = Metric::Rgba8, int w = -1)
	{
		const int width = (w < 0) ? static_cast<int>(pre.size()) : w;
		const int height = static_cast<int>(pre.size()) / width;
		return Compare(pre.data(), sw.data(), gpu.data(), width, height, 0, 0, metric);
	}
} // namespace

TEST(GSTileOracleCompare, AgreementIsSilentEvenWhenBothArmsWrote)
{
	// Both arms moved every pixel, identically. That is the overwhelmingly common case
	// and it must produce no finding of any kind.
	const Tally t = Score({0, 0, 0, 0}, {1, 2, 3, 4}, {1, 2, 3, 4});
	EXPECT_EQ(t.compared, 4u);
	EXPECT_EQ(t.differing(), 0u);
	EXPECT_EQ(t.max_delta, 0u);
	EXPECT_EQ(t.first_x, -1);
}

TEST(GSTileOracleCompare, ArithmeticDivergenceCountsAsBoth)
{
	// Both arms moved the pixel off `pre`, to different values: a shading, blending or
	// sampling difference.
	const Tally t = Score({0x00000000}, {0x00000010}, {0x00000012});
	EXPECT_EQ(t.both, 1u);
	EXPECT_EQ(t.sw_only, 0u);
	EXPECT_EQ(t.gpu_only, 0u);
	EXPECT_EQ(t.max_delta, 2u);
}

TEST(GSTileOracleCompare, NativeMissedAPixelTheSoftwareArmWrote)
{
	// The native arm left the pre value in place: coverage lost, or a plane/page it
	// never claimed. NOT an arithmetic bug, and the ledger must not call it one.
	const Tally t = Score({0x11223344}, {0x11223355}, {0x11223344});
	EXPECT_EQ(t.sw_only, 1u);
	EXPECT_EQ(t.both, 0u);
	EXPECT_EQ(t.gpu_only, 0u);
}

TEST(GSTileOracleCompare, NativeWroteWhereTheSoftwareArmDidNot)
{
	// The mirror case: a claim or a coverage rule reaching too far.
	const Tally t = Score({0x11223344}, {0x11223344}, {0x11223355});
	EXPECT_EQ(t.gpu_only, 1u);
	EXPECT_EQ(t.sw_only, 0u);
	EXPECT_EQ(t.both, 0u);
}

TEST(GSTileOracleCompare, AllThreeClassesAreCountedSeparatelyInOneDraw)
{
	// The case the instrument exists for: one draw carrying three unrelated defects,
	// which a single "pixels differing" number would report as one population of 3.
	const Tally t = Score(
		/*pre*/ {0x00, 0x00, 0x00, 0x00},
		/*sw */ {0x10, 0x20, 0x00, 0x44},
		/*gpu*/ {0x11, 0x00, 0x30, 0x44});
	EXPECT_EQ(t.both, 1u); // pixel 0
	EXPECT_EQ(t.sw_only, 1u); // pixel 1
	EXPECT_EQ(t.gpu_only, 1u); // pixel 2
	EXPECT_EQ(t.differing(), 3u);
	EXPECT_EQ(t.compared, 4u);
}

TEST(GSTileOracleCompare, FirstDivergentPixelScansRowsThenColumns)
{
	// 3x2, the only disagreement at (1, 1) -- reported in draw coordinates, offset by
	// the rect origin so the ledger's x/y can be read straight onto a frame dump.
	std::vector<u32> pre(6, 0), sw(6, 0), gpu(6, 0);
	sw[4] = 0x77;
	const Tally t = Compare(pre.data(), sw.data(), gpu.data(), 3, 2, 100, 200, Metric::Rgba8);
	EXPECT_EQ(t.first_x, 101);
	EXPECT_EQ(t.first_y, 201);
	EXPECT_EQ(t.first_pre, 0u);
	EXPECT_EQ(t.first_sw, 0x77u);
	EXPECT_EQ(t.first_gpu, 0u);
	EXPECT_EQ(t.sw_only, 1u);
}

TEST(GSTileOracleCompare, WorstColourDeltaIsPerLaneNotWholeWord)
{
	// 0x00FF0000 vs 0x01000000 is a whole-word gap of 0x00010000, but only one level in
	// green and one in blue. Pricing the word would report a nonsense magnitude.
	const Tally t = Score({0}, {0x00FF0000}, {0x01000000});
	EXPECT_EQ(t.max_delta, 255u);
}

TEST(GSTileOracleCompare, WorstColourDeltaTakesTheLoudestLane)
{
	const Tally t = Score({0}, {0x00010203}, {0x00010A03});
	EXPECT_EQ(t.max_delta, 8u);
}

TEST(GSTileOracleCompare, SixteenBitDeltasAreReportedInEightBitLevels)
{
	// A 5-bit lane moving by one is eight 8-bit levels, so a 16-bit destination's
	// numbers stay comparable with a 32-bit one's -- and with the frame scorer's
	// threshold, which is what anyone reading this column will compare against.
	const Tally t = Score({0}, {0x0000}, {0x0001}, Metric::Rgba5551);
	EXPECT_EQ(t.max_delta, 8u);

	// The alpha bit is worth the whole range.
	const Tally a = Score({0}, {0x0000}, {0x8000}, Metric::Rgba5551);
	EXPECT_EQ(a.max_delta, 255u);
	EXPECT_EQ(a.gpu_only, 1u);
}

TEST(GSTileOracleCompare, DepthIsPricedAsOneUnsignedNumber)
{
	// Depth has no lanes: a Z ladder decided by one unit must report 1, and a large gap
	// must report the large gap rather than a per-byte fragment of it.
	const Tally one = Score({0}, {0x00123456}, {0x00123457}, Metric::Raw);
	EXPECT_EQ(one.max_delta, 1u);

	const Tally big = Score({0}, {0x00000000}, {0x00FF0000}, Metric::Raw);
	EXPECT_EQ(big.max_delta, 0x00FF0000u);
}

TEST(GSTileOracleCompare, MagnitudeHistogramSeparatesResidueFromRealDamage)
{
	// The question a per-draw MAXIMUM cannot answer. Four pixels off by one level and a
	// single pixel off by fifty share a maximum of fifty; only the histogram says the
	// draw is 80% accepted residue.
	const Tally t = Score(
		/*pre*/ {0, 0, 0, 0, 0},
		/*sw */ {0x10, 0x10, 0x10, 0x10, 0x10},
		/*gpu*/ {0x11, 0x11, 0x11, 0x11, 0x42});
	EXPECT_EQ(t.max_delta, 50u);
	EXPECT_EQ(t.d1, 4u);
	EXPECT_EQ(t.d16_127, 1u);
	EXPECT_EQ(t.d2 + t.d3_15 + t.d128p, 0u);
	EXPECT_EQ(t.d1 + t.d2 + t.d3_15 + t.d16_127 + t.d128p, t.differing());
}

TEST(GSTileOracleCompare, HistogramBucketBoundaries)
{
	auto one = [](u32 sw, u32 gpu) { return Score({0}, {sw}, {gpu}); };
	EXPECT_EQ(one(0, 1).d1, 1u);
	EXPECT_EQ(one(0, 2).d2, 1u);
	EXPECT_EQ(one(0, 3).d3_15, 1u);
	EXPECT_EQ(one(0, 15).d3_15, 1u);
	EXPECT_EQ(one(0, 16).d16_127, 1u);
	EXPECT_EQ(one(0, 127).d16_127, 1u);
	EXPECT_EQ(one(0, 128).d128p, 1u);
	EXPECT_EQ(one(0, 255).d128p, 1u);
}

TEST(GSTileOracleCompare, EmptyRectComparesNothing)
{
	const Tally t = Compare(nullptr, nullptr, nullptr, 0, 0, 0, 0, Metric::Rgba8);
	EXPECT_EQ(t.compared, 0u);
	EXPECT_EQ(t.differing(), 0u);
	EXPECT_EQ(t.first_x, -1);
}

TEST(GSTileOracleCompare, MetricFollowsTheDestinationFormat)
{
	EXPECT_EQ(GSTileOracle::MetricForPsm(PSMCT32), Metric::Rgba8);
	EXPECT_EQ(GSTileOracle::MetricForPsm(PSMCT24), Metric::Rgba8);
	EXPECT_EQ(GSTileOracle::MetricForPsm(PSMCT16), Metric::Rgba5551);
	EXPECT_EQ(GSTileOracle::MetricForPsm(PSMCT16S), Metric::Rgba5551);
	EXPECT_EQ(GSTileOracle::MetricForPsm(PSMZ32), Metric::Raw);
	EXPECT_EQ(GSTileOracle::MetricForPsm(PSMZ24), Metric::Raw);
	EXPECT_EQ(GSTileOracle::MetricForPsm(PSMZ16S), Metric::Raw);
}
