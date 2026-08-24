// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What ends a TileGpu render pass, what only ends an indirect run inside one, and the one bit of
// that the device gets to decide.
//
// A pass is its ATTACHMENTS. Vulkan cannot change which images a render pass renders into without
// ending it, so the colour surface, whether there is a depth attachment at all, and which depth
// surface it is are always in the key. Depth WRITE-ENABLE and depth COMPARE-ENABLE are not
// attachments -- they are two bits of VkPipelineDepthStencilStateCreateInfo, and the executor
// already rebinds a pipeline per indirect run inside a pass, on topology and on the blend key. So
// they COULD leave the key and cut the run instead. Whether they should is a hardware question with
// two measured answers, and GSDevice::TileGpuPrefersDepthUniformPasses is where it is asked:
//
//  - MERGED (the default, and what a tiler wants). The depth write-enable used to be in the pass
//    key unconditionally, and when the alpha test's plan-time fold widened from a single alpha
//    value to an interval it started deciding ~287 more Shadow of the Colossus draws a frame
//    all-fail. Those draws are ATST=GEQUAL with AFAIL=FB_ONLY -- "a failing fragment still writes
//    the frame buffer but must not touch depth" -- so each lost its depth write, and each therefore
//    opened a render pass going in and another coming out. SotC went from 80.95 passes a frame to
//    574.95 and from 10.4 ms to 35.0 ms on an M2; the F4 scene went to 1293.65 passes and 69.9 ms.
//    Not one pixel moved. Merging gives all of it back.
//  - DEPTH-UNIFORM (what Adreno wants). The same merge measured a net LOSS on an SD865 on every
//    dump tried, while the pass-count deltas matched the model to the pass -- something on that
//    architecture charges more for mixed depth state inside a pass than the pass boundaries cost.
//
// Both polarities are pinned here, because the failure mode of either is invisible in the frame and
// enormous in the frame time, and because the two grouping sites reading different answers is the
// bug class this subsystem keeps meeting.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
using DepthMode = GSDevice::GSTileGpuDepthMode;
using Topology = GSDevice::GSTileGpuTopology;
using RunKey = GSDevice::GSTileGpuPassPlan::GSTileGpuRunKey;

constexpr GSTileSurfaceId kColor = 1;
constexpr GSTileSurfaceId kOtherColor = 2;
constexpr GSTileSurfaceId kZ = 3;
constexpr GSTileSurfaceId kOtherZ = 4;

/// The two polarities, named so a failure says which one it was.
constexpr bool kMerged = false; ///< depth mode out of the key: the tiler answer, and the default
constexpr bool kUniform = true; ///< depth mode in the key: the Adreno answer, and what shipped

/// The pass key of a draw expressed the way the renderer holds it: the two surfaces plus the three
/// depth facts it derives out of ZTE / ZTST / ZMSK and the alpha test's AFAIL fold.
constexpr GSTileGpuPassKey KeyOf(
	GSTileSurfaceId color, GSTileSurfaceId z, bool z_test, bool z_write, bool depth_uniform)
{
	const bool z_used = z_test || z_write;
	return gsTileGpuPassKeyFor(color, z, gsTileGpuDepthModeFor(z_used, z_test, z_write), depth_uniform);
}

/// A plan of n draws whose per-draw pipeline state the test sets by hand. Only the arrays RunKeyAt
/// reads are populated -- it is the run-cut rule under test, not the submission.
struct RunPlan
{
	std::vector<GSDevice::GSTileGpuIndirectDraw> draws;
	std::vector<Topology> topologies;
	std::vector<u32> blend_keys;
	std::vector<DepthMode> depth_modes;
	GSDevice::GSTileGpuPassPlan plan;

	RunPlan(std::initializer_list<std::tuple<Topology, u32, DepthMode>> rows)
	{
		for (const auto& [topo, blend, depth] : rows)
		{
			draws.push_back(GSDevice::GSTileGpuIndirectDraw{});
			topologies.push_back(topo);
			blend_keys.push_back(blend);
			depth_modes.push_back(depth);
		}
		plan.draws = draws;
		plan.topologies = topologies;
		plan.blend_keys = blend_keys;
		plan.depth_modes = depth_modes;
	}

	RunKey At(u32 d) const { return plan.RunKeyAt(d); }

	/// The runs the executor would submit, as [first, end) pairs, walked exactly the way its loop
	/// walks them: from draw 0, each run ending where GSTileGpuPassPlan::RunEndAt says.
	std::vector<std::pair<u32, u32>> Runs() const
	{
		std::vector<std::pair<u32, u32>> out;
		const u32 end = static_cast<u32>(draws.size());
		for (u32 d = 0; d < end;)
		{
			const u32 run_end = plan.RunEndAt(d, end);
			out.emplace_back(d, run_end);
			d = run_end;
		}
		return out;
	}
};
} // namespace

// ---------------------------------------------------------------------------------------------
// The attachment half of the pass key: true under both policies.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuPassKey, LosingTheDepthAttachmentEndsThePass)
{
	// A draw that neither tests nor writes depth needs no depth attachment, and a render pass
	// cannot gain or lose one. Nothing the device says may merge these two.
	for (const bool policy : {kMerged, kUniform})
	{
		EXPECT_NE(KeyOf(kColor, kZ, /*z_test=*/true, /*z_write=*/false, policy),
			KeyOf(kColor, kZ, /*z_test=*/false, /*z_write=*/false, policy))
			<< "depth_uniform_passes=" << policy;
	}
	EXPECT_EQ(gsTileGpuDepthModeFor(/*z_used=*/false, false, false), DepthMode::None);
}

TEST(TileGpuPassKey, ADifferentDepthSurfaceEndsThePass)
{
	for (const bool policy : {kMerged, kUniform})
	{
		EXPECT_NE(KeyOf(kColor, kZ, true, true, policy), KeyOf(kColor, kOtherZ, true, true, policy))
			<< "depth_uniform_passes=" << policy;
	}
}

TEST(TileGpuPassKey, ADifferentColourSurfaceEndsThePass)
{
	for (const bool policy : {kMerged, kUniform})
	{
		EXPECT_NE(KeyOf(kColor, kZ, true, true, policy), KeyOf(kOtherColor, kZ, true, true, policy))
			<< "depth_uniform_passes=" << policy;
	}
}

TEST(TileGpuPassKey, TheDepthSurfaceIsIgnoredWhereThereIsNoDepthAttachment)
{
	// Accumulation leaves the depth id at whatever EnsureSurface last handed back when the draw
	// uses no depth. Comparing it there would break passes on a field nothing binds.
	for (const bool policy : {kMerged, kUniform})
	{
		EXPECT_EQ(KeyOf(kColor, kZ, false, false, policy), KeyOf(kColor, kOtherZ, false, false, policy))
			<< "depth_uniform_passes=" << policy;
	}
}

TEST(TileGpuPassKey, DepthModeIsTheWholeThreeFlagTable)
{
	// GS depth grows towards the viewer: a real test is GEQUAL, a write-only draw is ALWAYS. The
	// four modes and the reachable flag combinations are in bijection, which is what lets the
	// depth-uniform key carry the mode instead of the flags and still cut where the old key cut.
	EXPECT_EQ(gsTileGpuDepthModeFor(false, false, false), DepthMode::None);
	EXPECT_EQ(gsTileGpuDepthModeFor(true, true, true), DepthMode::TestWrite);
	EXPECT_EQ(gsTileGpuDepthModeFor(true, true, false), DepthMode::TestNoWrite);
	EXPECT_EQ(gsTileGpuDepthModeFor(true, false, true), DepthMode::WriteAlways);
}

// ---------------------------------------------------------------------------------------------
// The merged policy: a depth-mode change must NOT end the pass.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuPassKeyMerged, DepthWriteEnableDoesNotEndThePass)
{
	// The regression's own shape: same colour target, same depth target, same depth test; one draw
	// writes depth and the next does not.
	EXPECT_EQ(KeyOf(kColor, kZ, /*z_test=*/true, /*z_write=*/true, kMerged),
		KeyOf(kColor, kZ, /*z_test=*/true, /*z_write=*/false, kMerged));
}

TEST(TileGpuPassKeyMerged, DepthCompareEnableDoesNotEndThePass)
{
	// A ZTST=ALWAYS write-only sprite and a ZTST=GEQUAL 3D draw share a depth buffer in SotC. They
	// take different depth pipelines and the same attachments, so they share a pass.
	EXPECT_EQ(KeyOf(kColor, kZ, /*z_test=*/true, /*z_write=*/true, kMerged),
		KeyOf(kColor, kZ, /*z_test=*/false, /*z_write=*/true, kMerged));
}

TEST(TileGpuPassKeyMerged, TheAlphaTestFoldsAllFailShapeStaysInOnePass)
{
	// Not the flags by hand this time: the flags the renderer actually derives, for the draw the
	// fold flipped. ATST=GEQUAL AFAIL=FB_ONLY, ZTE on, ZMSK clear, ZTST testing -- once with the
	// fold undecided and once with it deciding every fragment fails. gsTileDepthWriteSurvives drops
	// the depth write on the second, and under this policy that must not cost a pass.
	constexpr bool kZte = true, kZmsk = false, kZtstTests = true;
	const bool write_varies = gsTileDepthWriteSurvives(kZte, kZmsk, /*atst_all_fail=*/false, AFAIL_FB_ONLY);
	const bool write_allfail = gsTileDepthWriteSurvives(kZte, kZmsk, /*atst_all_fail=*/true, AFAIL_FB_ONLY);
	ASSERT_TRUE(write_varies);
	ASSERT_FALSE(write_allfail); // the fold really does take the write away, or the test proves nothing

	EXPECT_EQ(KeyOf(kColor, kZ, kZtstTests, write_varies, kMerged),
		KeyOf(kColor, kZ, kZtstTests, write_allfail, kMerged));
}

// ---------------------------------------------------------------------------------------------
// The depth-uniform policy: the same shapes must reproduce the shipped pass structure.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuPassKeyUniform, DepthWriteEnableEndsThePass)
{
	EXPECT_NE(KeyOf(kColor, kZ, /*z_test=*/true, /*z_write=*/true, kUniform),
		KeyOf(kColor, kZ, /*z_test=*/true, /*z_write=*/false, kUniform));
}

TEST(TileGpuPassKeyUniform, DepthCompareEnableEndsThePass)
{
	EXPECT_NE(KeyOf(kColor, kZ, /*z_test=*/true, /*z_write=*/true, kUniform),
		KeyOf(kColor, kZ, /*z_test=*/false, /*z_write=*/true, kUniform));
}

TEST(TileGpuPassKeyUniform, TheAlphaTestFoldsAllFailShapeOpensANewPass)
{
	// The shipped structure, stated as the thing it is: the shape that costs SotC 494 passes a
	// frame, kept deliberately on the architecture that measured faster with it.
	const bool write_varies = gsTileDepthWriteSurvives(true, false, /*atst_all_fail=*/false, AFAIL_FB_ONLY);
	const bool write_allfail = gsTileDepthWriteSurvives(true, false, /*atst_all_fail=*/true, AFAIL_FB_ONLY);
	EXPECT_NE(KeyOf(kColor, kZ, true, write_varies, kUniform), KeyOf(kColor, kZ, true, write_allfail, kUniform));
}

TEST(TileGpuPassKeyUniform, EveryPairOfDistinctDepthModesEndsThePass)
{
	// The old key compared z_used, the depth surface, z_write and z_test. This one compares the
	// depth mode, and the two agree only because the four modes and the reachable flag combinations
	// are in bijection -- so pin that no two distinct modes collide.
	constexpr DepthMode kAll[] = {
		DepthMode::None, DepthMode::TestWrite, DepthMode::TestNoWrite, DepthMode::WriteAlways};
	for (const DepthMode a : kAll)
	{
		for (const DepthMode b : kAll)
		{
			const GSTileGpuPassKey ka = gsTileGpuPassKeyFor(kColor, kZ, a, kUniform);
			const GSTileGpuPassKey kb = gsTileGpuPassKeyFor(kColor, kZ, b, kUniform);
			EXPECT_EQ(a == b, ka == kb) << static_cast<u32>(a) << " vs " << static_cast<u32>(b);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// The run key: the depth mode is pipeline state, so it cuts the indirect run.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuRunKey, DepthModeCutsTheRun)
{
	// The other half of the merged policy. Two draws sharing a pass under the merged key must not
	// share an indirect run, or the second would be submitted under the first's depth pipeline and
	// would write depth it must not write.
	//
	// Unconditional, not gated on the policy: under depth-uniform passes every draw of a pass
	// carries the same mode, so this cut fires nowhere and the submission is byte-identical to not
	// cutting at all. One rule that is correct under both beats two rules that can disagree.
	const RunPlan p{
		{Topology::Triangle, 0u, DepthMode::TestWrite}, {Topology::Triangle, 0u, DepthMode::TestNoWrite}};
	EXPECT_NE(p.At(0), p.At(1));
	EXPECT_EQ(p.At(0).depth_mode, DepthMode::TestWrite);
	EXPECT_EQ(p.At(1).depth_mode, DepthMode::TestNoWrite);
}

TEST(TileGpuRunKey, IdenticalPipelineStateSharesTheRun)
{
	const RunPlan p{
		{Topology::Triangle, 0u, DepthMode::TestWrite}, {Topology::Triangle, 0u, DepthMode::TestWrite}};
	EXPECT_EQ(p.At(0), p.At(1));
}

TEST(TileGpuRunKey, TopologyAndBlendStillCutTheRun)
{
	const RunPlan p{{Topology::Triangle, 0u, DepthMode::TestWrite}, {Topology::Line, 0u, DepthMode::TestWrite},
		{Topology::Line, GSDevice::GSTileGpuPassPlan::kBlendEnable | 5u, DepthMode::TestWrite}};
	EXPECT_NE(p.At(0), p.At(1));
	EXPECT_NE(p.At(1), p.At(2));
}

TEST(TileGpuRunKey, RunsPartitionThePassInSubmissionOrder)
{
	// ⚠️ The load-bearing property of the run cut, and the one a "collapse the call count" idea
	// would break first: runs are MAXIMAL CONSECUTIVE spans, never buckets. A pass that interleaves
	// two depth modes submits four runs in the original order, not two runs with the draws sorted
	// into them. Sorting would put a later draw's output under an earlier draw's -- which is the
	// whole of GS draw order -- and on hardware whose depth rejection depends on the order
	// fragments arrive it is a large silent fragment-cost regression as well.
	constexpr DepthMode kW = DepthMode::TestWrite;
	constexpr DepthMode kN = DepthMode::TestNoWrite;
	const RunPlan p{{Topology::Triangle, 0u, kW}, {Topology::Triangle, 0u, kN}, {Topology::Triangle, 0u, kN},
		{Topology::Triangle, 0u, kW}, {Topology::Triangle, 0u, kN}};

	const std::vector<std::pair<u32, u32>> runs = p.Runs();
	const std::vector<std::pair<u32, u32>> want{{0, 1}, {1, 3}, {3, 4}, {4, 5}};
	EXPECT_EQ(runs, want);

	// Said again as the property rather than the answer, so a future cut that is still a partition
	// but boundaries elsewhere fails on the boundaries and not on this: every draw appears exactly
	// once, and the concatenation is 0,1,2,3,4 in order.
	std::vector<u32> visited;
	for (const auto& [first, end] : runs)
	{
		for (u32 d = first; d < end; d++)
			visited.push_back(d);
	}
	EXPECT_EQ(visited, (std::vector<u32>{0, 1, 2, 3, 4}));
}

TEST(TileGpuRunKey, AnAbsentBlendArrayReadsAsNoBlend)
{
	// blend_keys is the one run-key field with a fallback: a plan that carries none means no draw
	// blends. depth_modes has no fallback on purpose -- a guessed depth state writes depth a draw
	// must not write, where a guessed blend only fails to blend.
	RunPlan p{{Topology::Triangle, 7u, DepthMode::None}, {Topology::Triangle, 9u, DepthMode::None}};
	p.plan.blend_keys = {};
	EXPECT_EQ(p.At(0), p.At(1));
	EXPECT_EQ(p.At(0).blend_key, 0u);
}
