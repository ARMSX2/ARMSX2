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
//
// The second device bit, GSDevice::TileGpuSegregatesSelfRead, has TWO consequences and both are
// pinned here for the same reason -- it is one bit, so its two effects have to be read off one
// answer. The pass-key half is below in the reader-dimension section; the ADMISSION half is at the
// end of the file, because narrowing which draws read at all is the only lever left on a title
// whose passes are reader-saturated and where segregation therefore has nothing to segregate.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
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

/// The two depth polarities, named so a failure says which one it was.
constexpr bool kMerged = false; ///< depth mode out of the key: the tiler answer, and the default
constexpr bool kUniform = true; ///< depth mode in the key: the Adreno answer, and what shipped

/// ...and the two reader polarities, the second independent dimension.
constexpr bool kShared = false;     ///< readers share a pass with non-readers: the default
constexpr bool kSegregated = true;  ///< a declaring pass holds only readers: the Adreno answer

/// The SAME bit as kShared/kSegregated, spelt for its other consequence. Two names for one device
/// answer is the point: the admission tests at the end of this file and the pass-key tests above
/// have to be reading the same bit, or the design claim behind having only one is false.
constexpr bool kDeclaringIsFree = false;
constexpr bool kDeclaringIsTaxed = true;

/// Whether the device serves an in-pass destination read at all.
constexpr bool kNoRoad = false;
constexpr bool kRoad = true;

/// The pass key of a draw expressed the way the renderer holds it: the two surfaces plus the three
/// depth facts it derives out of ZTE / ZTST / ZMSK and the alpha test's AFAIL fold, plus whether
/// the draw reads its own destination.
constexpr GSTileGpuPassKey KeyOf(GSTileSurfaceId color, GSTileSurfaceId z, bool z_test, bool z_write,
	bool depth_uniform, bool reads_self = false, bool segregate = kShared)
{
	const bool z_used = z_test || z_write;
	return gsTileGpuPassKeyFor(
		color, z, gsTileGpuDepthModeFor(z_used, z_test, z_write), depth_uniform, reads_self, segregate);
}

/// The passes a run of draws falls into, as [first, end) pairs, cut exactly where the key changes.
/// Both grouping sites cut on nothing else, so this is what they produce.
struct DrawRow
{
	GSTileSurfaceId color;
	bool reads_self;
	DepthMode depth;
};

std::vector<std::pair<u32, u32>> PassesOf(
	const std::vector<DrawRow>& rows, bool depth_uniform, bool segregate)
{
	const auto key_of = [&](const DrawRow& r) {
		return gsTileGpuPassKeyFor(r.color, kZ, r.depth, depth_uniform, r.reads_self, segregate);
	};
	std::vector<std::pair<u32, u32>> out;
	const u32 end = static_cast<u32>(rows.size());
	for (u32 i = 0; i < end;)
	{
		u32 j = i + 1;
		while (j < end && key_of(rows[j]) == key_of(rows[i]))
			j++;
		out.emplace_back(i, j);
		i = j;
	}
	return out;
}

/// A plan of n draws whose per-draw pipeline state the test sets by hand. Only the arrays RunKeyAt
/// reads are populated -- it is the run-cut rule under test, not the submission.
struct RunPlan
{
	std::vector<GSDevice::GSTileGpuIndirectDraw> draws;
	std::vector<Topology> topologies;
	std::vector<u32> blend_keys;
	std::vector<DepthMode> depth_modes;
	std::vector<u32> variant_keys;
	GSDevice::GSTileGpuPassPlan plan;

	RunPlan(std::initializer_list<std::tuple<Topology, u32, DepthMode>> rows)
	{
		for (const auto& [topo, blend, depth] : rows)
			Push(topo, blend, depth, 0u);
		Bind();
	}

	/// The same, with the fourth run-key field: the draw's packed fragment variant.
	RunPlan(std::initializer_list<std::tuple<Topology, u32, DepthMode, u32>> rows)
	{
		for (const auto& [topo, blend, depth, variant] : rows)
			Push(topo, blend, depth, variant);
		Bind();
	}

	void Push(Topology topo, u32 blend, DepthMode depth, u32 variant)
	{
		draws.push_back(GSDevice::GSTileGpuIndirectDraw{});
		topologies.push_back(topo);
		blend_keys.push_back(blend);
		depth_modes.push_back(depth);
		variant_keys.push_back(variant);
	}

	void Bind()
	{
		plan.draws = draws;
		plan.topologies = topologies;
		plan.blend_keys = blend_keys;
		plan.depth_modes = depth_modes;
		plan.variant_keys = variant_keys;
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
			const GSTileGpuPassKey ka = gsTileGpuPassKeyFor(kColor, kZ, a, kUniform, false, kShared);
			const GSTileGpuPassKey kb = gsTileGpuPassKeyFor(kColor, kZ, b, kUniform, false, kShared);
			EXPECT_EQ(a == b, ka == kb) << static_cast<u32>(a) << " vs " << static_cast<u32>(b);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// The reader dimension: whether a draw that reads its own destination may share a pass with one
// that does not.
//
// A pass that declares the in-pass destination read binds its own colour attachment as an input
// attachment, and on Adreno that costs EVERY draw in the pass, reader or not: Turnip's
// tu_render_pass_check_feedback_loop marks the subpass, tu_pipeline inherits the flag onto every
// pipeline built against it, and on sysmem such a pipeline sets GRAS_SC_CNTL.single_prim_mode to
// FLUSH_PER_OVERLAP_AND_OVERWRITE -- a render-backend flush per overlapping primitive. So on that
// architecture six admitted draws sitting inside a giant effect pass tax the whole pass.
//
// Segregating costs passes and is the wrong trade on a tiler, where a pass boundary is a
// tile-buffer resolve and reload and an in-pass read is free below stride-1 density. Hence a
// device bit, and hence both polarities pinned here.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuPassKeyShared, AReaderJoinsANonReadersPass)
{
	// The default, and what a tiler wants: the read is a per-draw property, the pass just declares
	// that someone in it reads.
	EXPECT_EQ(KeyOf(kColor, kZ, true, true, kMerged, /*reads_self=*/false, kShared),
		KeyOf(kColor, kZ, true, true, kMerged, /*reads_self=*/true, kShared));
}

TEST(TileGpuPassKeySegregated, AReaderDoesNotJoinANonReadersPass)
{
	// Same attachments, same depth mode; one draw reads its own destination and the other does not.
	// Under segregation those are two passes, so only the reader's pass declares.
	for (const bool depth_policy : {kMerged, kUniform})
	{
		EXPECT_NE(KeyOf(kColor, kZ, true, true, depth_policy, /*reads_self=*/false, kSegregated),
			KeyOf(kColor, kZ, true, true, depth_policy, /*reads_self=*/true, kSegregated))
			<< "depth_uniform_passes=" << depth_policy;
	}
}

TEST(TileGpuPassKeySegregated, ConsecutiveReadersShareOnePass)
{
	// Segregation separates readers from non-readers, NOT readers from each other. A run of
	// admitted draws is one declared pass, or the fix trades one toll for another.
	for (const bool depth_policy : {kMerged, kUniform})
	{
		EXPECT_EQ(KeyOf(kColor, kZ, true, true, depth_policy, true, kSegregated),
			KeyOf(kColor, kZ, true, true, depth_policy, true, kSegregated))
			<< "depth_uniform_passes=" << depth_policy;
	}
}

TEST(TileGpuPassKeySegregated, TheAttachmentHalfStillDominates)
{
	// Two readers into different colour surfaces are still two passes: segregation may only ever
	// ADD boundaries.
	EXPECT_NE(KeyOf(kColor, kZ, true, true, kMerged, true, kSegregated),
		KeyOf(kOtherColor, kZ, true, true, kMerged, true, kSegregated));
}

TEST(TileGpuPassKeySegregated, TheTwoDimensionsAreIndependent)
{
	// Composition by construction rather than by inspection: Adreno turns BOTH bits on, so the key
	// has to carry them separately. Over the four policy combinations, two keys are equal exactly
	// when they agree on every dimension that policy keys.
	for (const bool depth_policy : {kMerged, kUniform})
	{
		for (const bool reader_policy : {kShared, kSegregated})
		{
			for (const DepthMode a : {DepthMode::TestWrite, DepthMode::TestNoWrite})
			{
				for (const DepthMode b : {DepthMode::TestWrite, DepthMode::TestNoWrite})
				{
					for (const bool ra : {false, true})
					{
						for (const bool rb : {false, true})
						{
							const bool want = (!depth_policy || a == b) && (!reader_policy || ra == rb);
							EXPECT_EQ(want,
								gsTileGpuPassKeyFor(kColor, kZ, a, depth_policy, ra, reader_policy) ==
									gsTileGpuPassKeyFor(kColor, kZ, b, depth_policy, rb, reader_policy))
								<< "depth_uniform=" << depth_policy << " segregate=" << reader_policy
								<< " modes " << static_cast<u32>(a) << "/" << static_cast<u32>(b)
								<< " reads " << ra << "/" << rb;
						}
					}
				}
			}
		}
	}
}

TEST(TileGpuPassKeySegregated, PassesAreMaximalConsecutiveRunsInDrawOrder)
{
	// The same load-bearing property the run cut has, for the new dimension: passes are maximal
	// consecutive spans, never buckets. Sorting the readers together would move a later draw's
	// output under an earlier one's, which is the whole of GS draw order.
	constexpr DepthMode kW = DepthMode::TestWrite;
	const std::vector<DrawRow> rows{{kColor, false, kW}, {kColor, true, kW}, {kColor, true, kW},
		{kColor, false, kW}, {kColor, true, kW}};

	EXPECT_EQ(PassesOf(rows, kMerged, kSegregated),
		(std::vector<std::pair<u32, u32>>{{0, 1}, {1, 3}, {3, 4}, {4, 5}}));
	// ...and with segregation off the same five draws are one pass, which is the arm that pays the
	// Adreno toll on all five.
	EXPECT_EQ(PassesOf(rows, kMerged, kShared), (std::vector<std::pair<u32, u32>>{{0, 5}}));
}

TEST(TileGpuPassKeySegregated, TheDisplaySeedPseudoDrawReadsNothing)
{
	// The pseudo-draw BuildAndExecutePlan appends for its prep ops carries no self mask and no
	// depth attachment, so under segregation it keys as a non-reader and cannot pull a declaration
	// onto a pass that renders nothing.
	EXPECT_EQ(gsTileGpuPassKeyFor(kColor, kZ, DepthMode::None, kUniform, false, kSegregated),
		gsTileGpuPassKeyFor(kColor, kOtherZ, DepthMode::None, kUniform, false, kSegregated));
	EXPECT_NE(gsTileGpuPassKeyFor(kColor, kZ, DepthMode::None, kUniform, false, kSegregated),
		gsTileGpuPassKeyFor(kColor, kZ, DepthMode::None, kUniform, true, kSegregated));
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

// ---------------------------------------------------------------------------------------------
// The fourth run-key field: the FRAGMENT VARIANT.
//
// A pass is one attachment configuration; a fragment program is not attachment state. Keyed on the
// pass, the program every draw ran was the UNION of what its pass's draws needed -- so merging two
// passes widened the program for both, which is what the corpus census found: 99.6% of Bloodrayne
// 2's materialised-source draws were executing a byte decoder they never entered (1039-1748
// instructions against their own road's 390), and merging pushed untextured draws off the Adreno
// 650's 4-register wave128 program onto a 12-register wave64 one. Keyed on the run, each draw
// executes the smallest program that serves it and merging widens nothing.
//
// The cost is the same cost the sampled-binding key has: a cut, i.e. one more indirect call, priced
// by the census at 30 a frame worst case in the corpus. Unlike that one it is pipeline state, so it
// also costs the pipeline bind.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuRunKey, TheFragmentVariantCutsTheRun)
{
	using Plan = GSDevice::GSTileGpuPassPlan;
	const u32 byte_d32 = Plan::PackVariantKey(GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuTexelDirect32, 0, false);
	const u32 source = Plan::PackVariantKey(GSDevice::kGSTileGpuRoadSource, 0, 0, false);
	const RunPlan p{{Topology::Triangle, 0u, DepthMode::TestWrite, byte_d32},
		{Topology::Triangle, 0u, DepthMode::TestWrite, source}};
	EXPECT_NE(p.At(0), p.At(1));
	EXPECT_EQ(p.At(0).variant, byte_d32);
	EXPECT_EQ(p.At(1).variant, source);
}

TEST(TileGpuRunKey, EveryVariantAxisCutsTheRunOnItsOwn)
{
	// Four axes, one key, and a run that shared a pipeline across any of them would execute a
	// program that is wrong for half its draws -- silently, because the state row's gates make the
	// missing arm a no-op rather than a fault. So each is pinned separately.
	using Plan = GSDevice::GSTileGpuPassPlan;
	constexpr u32 kByte = GSDevice::kGSTileGpuRoadByte;
	const u32 base = Plan::PackVariantKey(kByte, GSDevice::kGSTileGpuTexelIndex8, 0, false);
	const u32 other_road = Plan::PackVariantKey(kByte | GSDevice::kGSTileGpuRoadTarget, GSDevice::kGSTileGpuTexelIndex8, 0, false);
	const u32 other_arm = Plan::PackVariantKey(kByte, GSDevice::kGSTileGpuTexelIndex4, 0, false);
	const u32 other_self = Plan::PackVariantKey(kByte, GSDevice::kGSTileGpuTexelIndex8, GSDevice::kGSTileGpuSelfBlend, false);
	const u32 other_q16 = Plan::PackVariantKey(kByte, GSDevice::kGSTileGpuTexelIndex8, 0, true);
	for (const u32 v : {other_road, other_arm, other_self, other_q16})
	{
		const RunPlan p{{Topology::Triangle, 0u, DepthMode::None, base}, {Topology::Triangle, 0u, DepthMode::None, v}};
		EXPECT_NE(p.At(0), p.At(1)) << "variant " << v << " shared a run with " << base;
	}
}

TEST(TileGpuRunKey, AMixedVariantPassSplitsExactlyAtTheVariantBoundaries)
{
	// The partition property again, now driven by the variant alone: same topology, same blend, same
	// depth throughout, so every cut here is the variant's and the boundaries are exactly where the
	// variant changes -- not one draw earlier, not one later, and never re-sorted into buckets.
	using Plan = GSDevice::GSTileGpuPassPlan;
	const u32 a = Plan::PackVariantKey(GSDevice::kGSTileGpuRoadSource, 0, 0, false);
	const u32 b = Plan::PackVariantKey(GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuTexelDirect32, 0, false);
	const u32 c = 0; // untextured: the smallest program there is, and the one merging used to lose
	constexpr Topology kT = Topology::Triangle;
	constexpr DepthMode kD = DepthMode::TestWrite;
	const RunPlan p{{kT, 0u, kD, a}, {kT, 0u, kD, b}, {kT, 0u, kD, b}, {kT, 0u, kD, c}, {kT, 0u, kD, b}};

	const std::vector<std::pair<u32, u32>> runs = p.Runs();
	EXPECT_EQ(runs, (std::vector<std::pair<u32, u32>>{{0, 1}, {1, 3}, {3, 4}, {4, 5}}));

	// ...and the partition itself, stated as the property: every draw once, in submission order.
	// Interleaving b,c,b must submit three runs and not two -- an untextured draw between two
	// textured ones does not get hoisted out to join them.
	std::vector<u32> visited;
	for (const auto& [first, end] : runs)
	{
		for (u32 d = first; d < end; d++)
			visited.push_back(d);
	}
	EXPECT_EQ(visited, (std::vector<u32>{0, 1, 2, 3, 4}));
}

TEST(TileGpuRunKey, AnAbsentVariantArrayFallsBackToThePassUnion)
{
	// The variant stream has a fallback and it is load-bearing twice over: it is what an executor
	// sees from a plan built before this axis existed, and it is the forced-vs-narrowed gate's other
	// arm (TileGpuUnionFragmentVariant withholds the stream, so every run compiles its pass's union
	// program and the two arms must render byte-identical frames).
	using Plan = GSDevice::GSTileGpuPassPlan;
	RunPlan p{{Topology::Triangle, 0u, DepthMode::None, Plan::PackVariantKey(GSDevice::kGSTileGpuRoadByte, 1u, 0, false)},
		{Topology::Triangle, 0u, DepthMode::None, Plan::PackVariantKey(GSDevice::kGSTileGpuRoadSource, 0u, 0, false)}};
	EXPECT_NE(p.At(0), p.At(1));
	p.plan.variant_keys = {};
	EXPECT_EQ(p.At(0), p.At(1));
	EXPECT_EQ(p.At(0).variant, 0u);
}

TEST(TileGpuVariantKey, EveryAxisRoundTripsAndNoneCollides)
{
	// One packer, three unpackers and thirteen bits: the renderer fills it, the run key compares it
	// and the executor turns it back into #defines. A field that bled into its neighbour would
	// compile the wrong program with no symptom but the pixels.
	using Plan = GSDevice::GSTileGpuPassPlan;
	for (u32 road = 0; road <= GSDevice::kGSTileGpuRoadMaskAll; road++)
	{
		for (u32 texel = 0; texel <= GSDevice::kGSTileGpuTexelMaskAll; texel++)
		{
			for (u32 self = 0; self <= GSDevice::kGSTileGpuSelfMaskAll; self++)
			{
				for (const bool q : {false, true})
				{
					const u32 key = Plan::PackVariantKey(road, texel, self, q);
					ASSERT_EQ(Plan::VariantRoadMask(key), road);
					ASSERT_EQ(Plan::VariantTexelMask(key), texel);
					ASSERT_EQ(Plan::VariantSelfMask(key), self);
					ASSERT_EQ(Plan::VariantQuantises(key), q);
				}
			}
		}
	}
	// ...and the key is INJECTIVE over the whole product, which is what makes comparing two packed
	// words the same question as comparing four masks.
	std::vector<u32> keys;
	for (u32 road = 0; road <= GSDevice::kGSTileGpuRoadMaskAll; road++)
		for (u32 texel = 0; texel <= GSDevice::kGSTileGpuTexelMaskAll; texel++)
			for (u32 self = 0; self <= GSDevice::kGSTileGpuSelfMaskAll; self++)
				for (const bool q : {false, true})
					keys.push_back(Plan::PackVariantKey(road, texel, self, q));
	const size_t total = keys.size();
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	EXPECT_EQ(keys.size(), total);
}

TEST(TileGpuRunKey, TheIndirectDrawStructStaysByteIdenticalToVulkansCommand)
{
	// The executor memcpys the plan's draw array straight into a VkDrawIndexedIndirectBuffer and
	// hands vkCmdDrawIndexedIndirect sizeof(GSTileGpuIndirectDraw) as the stride, so this struct IS
	// VkDrawIndexedIndirectCommand -- five u32 in that order, the fifth being firstInstance, which
	// is how a draw names its state row without a descriptor rebind.
	//
	// Pinned here because the variant went into a PARALLEL array for exactly this reason: a field
	// added to the draw struct would not fail to compile and would not fail a test, it would feed
	// the GPU a misaligned command stream at runtime.
	using Draw = GSDevice::GSTileGpuIndirectDraw;
	static_assert(sizeof(Draw) == 5 * sizeof(u32), "the indirect draw is VkDrawIndexedIndirectCommand");
	static_assert(alignof(Draw) == alignof(u32));
	static_assert(offsetof(Draw, index_count) == 0);
	static_assert(offsetof(Draw, instance_count) == 4);
	static_assert(offsetof(Draw, first_index) == 8);
	static_assert(offsetof(Draw, vertex_offset) == 12);
	static_assert(offsetof(Draw, state_index) == 16); // -> firstInstance
	SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// The admission half of the same device bit: WHICH draws read, not which pass they read in.
//
// Segregation confines the declaration toll to the draws that need the read, and can do nothing for
// a title whose passes are reader-SATURATED, because there the readers already are the pass. The
// question is then which classes are worth their toll -- and the corpus says three times over that
// it cannot be answered per class:
//
//   * Refusing the partial-FBMSK class buys Xenosaga its entire toll (1,930 declaring passes a frame
//     down to 21.5) for TWO changed pixels, and costs Katamari its punctuation -- 395 pixels at up
//     to 154 levels, exactly what C3 landed to repair.
//   * Refusing the 16-bit-quantised-blend class buys Baldur's Gate Dark Alliance II its toll (477.88
//     admitted draws a frame down to 0.88) for 0.14 of a channel level, and costs Katamari 2.1
//     levels -- a single full-screen composite whose quantisation is the whole frame's.
//
// Same class, opposite right answers, and the variable that separates them is never the class. It is
// the DENSITY, which only the renderer knows and only from the frame it just drew. Hence a per-class
// budget, and hence the unit it is kept in: not the class's draw count and not its declaring-pass
// count, but the draws that would stand BEHIND another draw of the class in a declaring pass, since
// the tax is a render-backend flush per overlapping primitive and a pass holding one draw has
// nothing to charge. FlatOut 2 puts 448 draws in 448 declaring passes and costs zero by that
// measure; Baldur's Gate puts 478 in 4.88 and costs 473.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuAdmissionClass, EachClassAsksForTheUseItNeeds)
{
	// Five classes, three uses. The many-to-one is the reason the budget is kept per CLASS: on
	// FlatOut 2 the exotic blends are 448 draws a frame and the 16-bit blends are zero, and both want
	// kGSTileGpuSelfBlend, so a budget kept per USE would price them together and get both wrong.
	EXPECT_EQ(GSDevice::kGSTileGpuSelfDate, gsTileGpuClassUses(kGSTileGpuClassDate));
	EXPECT_EQ(GSDevice::kGSTileGpuSelfBlend, gsTileGpuClassUses(kGSTileGpuClassExoticBlend));
	EXPECT_EQ(GSDevice::kGSTileGpuSelfBlend, gsTileGpuClassUses(kGSTileGpuClassQuantisedBlend));
	EXPECT_EQ(GSDevice::kGSTileGpuSelfMask, gsTileGpuClassUses(kGSTileGpuClassPartialMask));
	EXPECT_EQ(GSDevice::kGSTileGpuSelfMask, gsTileGpuClassUses(kGSTileGpuClassAfailKeep));
	EXPECT_EQ(GSDevice::kGSTileGpuSelfMaskAll, gsTileGpuClassUses(kGSTileGpuClassAll));
	EXPECT_EQ(0u, gsTileGpuClassUses(0));
}

TEST(TileGpuAdmissionClass, TheClassifierNamesTheReasonAndNothingElse)
{
	EXPECT_EQ(kGSTileGpuClassDate, gsTileGpuWantedClasses(0, /*date=*/true, true, false, false));
	EXPECT_EQ(kGSTileGpuClassPartialMask, gsTileGpuWantedClasses(0, false, /*fbmsk_exact=*/false, false, false));
	EXPECT_EQ(kGSTileGpuClassQuantisedBlend,
		gsTileGpuWantedClasses(0, false, true, /*blend_needs_quantised_result=*/true, false));
	EXPECT_EQ(kGSTileGpuClassAfailKeep, gsTileGpuWantedClasses(0, false, true, false, /*afail_keep_alpha=*/true));
	for (const u32 flag : {GSTilePassSim::ReaderWrap, GSTilePassSim::ReaderPabe, GSTilePassSim::ReaderCoeffGt1,
			 GSTilePassSim::ReaderAdFactor, GSTilePassSim::ReaderFacGt1})
	{
		EXPECT_EQ(kGSTileGpuClassExoticBlend, gsTileGpuWantedClasses(flag, false, true, false, false))
			<< "reader flag " << flag;
	}
	// The class next door, and the reason the classifier has to name classes rather than "blended
	// draws": C=As at or below 0x80 is exactly what dual-source blending expresses, so it is worth
	// nothing anywhere and is never wanted.
	EXPECT_EQ(0u, gsTileGpuWantedClasses(GSTilePassSim::ReaderAsDualSource, false, true, false, false));
	EXPECT_EQ(0u, gsTileGpuWantedClasses(0, false, true, false, false));
}

TEST(TileGpuAdmissionClass, TheClassifierTakesNoDeviceOpinion)
{
	// The load-bearing property, stated as a test because it is what stops the budget measuring its
	// own output: what a draw WANTS is a function of the draw. A refused class must still be seen to
	// want, or a class that declares nothing measures nothing and re-admits itself every second frame
	// forever. The signature is the guarantee -- there is no device argument to pass -- so this pins
	// the consequence: the same draw asks for the same thing whatever any budget last decided.
	const u32 all_reasons = gsTileGpuWantedClasses(GSTilePassSim::ReaderWrap, true, false, true, true);
	EXPECT_EQ(kGSTileGpuClassAll, all_reasons);
	EXPECT_EQ(all_reasons, gsTileGpuWantedClasses(GSTilePassSim::ReaderWrap, true, false, true, true));
}

TEST(TileGpuSelfReadUses, AdmissionIsWhatWasWantedLessWhatTheBudgetRefused)
{
	EXPECT_EQ(GSDevice::kGSTileGpuSelfBlend,
		gsTileGpuSelfReadUses(kGSTileGpuClassQuantisedBlend, kGSTileGpuClassAll, kRoad));
	EXPECT_EQ(0u, gsTileGpuSelfReadUses(kGSTileGpuClassQuantisedBlend,
					  kGSTileGpuClassAll & ~kGSTileGpuClassQuantisedBlend, kRoad));
	// Refusing one class does not refuse the draw's other reasons: a COLCLAMP-wrapping blend on a
	// 16-bit frame is inexpressible whatever the format quantises, so it still reads.
	EXPECT_EQ(GSDevice::kGSTileGpuSelfBlend,
		gsTileGpuSelfReadUses(kGSTileGpuClassQuantisedBlend | kGSTileGpuClassExoticBlend,
			kGSTileGpuClassAll & ~kGSTileGpuClassQuantisedBlend, kRoad));
	// ...and no road means no admission, whatever the budget says.
	EXPECT_EQ(0u, gsTileGpuSelfReadUses(kGSTileGpuClassAll, kGSTileGpuClassAll, kNoRoad));
}

// ---------------------------------------------------------------------------------------------
// The budget itself.
// ---------------------------------------------------------------------------------------------

namespace
{
/// Run one frame's worth of draws through a budget: `runs` runs of `per_run` draws each, all wanting
/// `classes`, then the frame boundary. Returns the verdict now in force.
u32 RunFrame(GSTileGpuDeclaringBudget& b, u32 classes, u32 runs, u32 per_run)
{
	for (u32 r = 0; r < runs; r++)
	{
		b.Charge(classes, /*opening=*/classes); // the run's first draw stands behind nobody
		for (u32 d = 1; d < per_run; d++)
			b.Charge(classes, /*opening=*/0);
	}
	b.Roll();
	return b.admitted;
}

/// One frame of `draws` draws of `classes`, all in ONE run -- the Baldur's Gate shape.
u32 RunDenseFrame(GSTileGpuDeclaringBudget& b, u32 classes, u32 draws)
{
	return RunFrame(b, classes, 1, draws);
}

/// One frame of `draws` draws of `classes`, each alone in its own run -- the FlatOut 2 shape.
u32 RunSpreadFrame(GSTileGpuDeclaringBudget& b, u32 classes, u32 draws)
{
	return RunFrame(b, classes, draws, 1);
}
} // namespace

// -- the cost model ----------------------------------------------------------------------------

TEST(TileGpuClassCost, ADeclaredRunCostsMoreThanATaxedDraw)
{
	// alpha = 8/5, measured. A declaring pass is the more expensive of the two halves: MGS3's exotic
	// blends are 162 draws in 162 SOLO declared passes -- no overlap for the render-backend flush to
	// charge at all -- and force-refusing that one class on the SD865 took the frame from 29.1 ms to
	// 19.73, with the device pass count dropping by exactly the class's own count, 633 -> 471. That
	// is ~9.5 ms across 162 passes, about 59 us each, and consistent with the ~77 us FlatOut 2's 896
	// declared passes implied.
	EXPECT_EQ(100u, gsTileGpuClassCost(/*taxed=*/100, /*runs=*/0));
	EXPECT_EQ(160u, gsTileGpuClassCost(/*taxed=*/0, /*runs=*/100));
	EXPECT_LT(gsTileGpuClassCost(100, 0), gsTileGpuClassCost(0, 100));
}

TEST(TileGpuClassCost, TheRunCounterCanOnlyEVERUNDERSTATE)
{
	// ⚠️ The caveat that outlived the alpha it was an argument for. At alpha = 1 the cost was the
	// class's draw count and so was invariant to how the draws split into passes -- which mattered,
	// because the run counter is the number this planner cannot measure: it counts runs broken by
	// the attachment group changing, and cannot see the pass breaks a prep op or a texture bind
	// makes later in the same draw. On Ratchet & Clank's effects it counts SEVEN runs where the
	// device declared 357 passes.
	//
	// The device overruled the invariance argument -- a declared pass is really more expensive, so
	// alpha really is above one -- and the caveat survives with its DIRECTION stated: undercounting
	// runs UNDERPRICES a class, never the reverse, so the failure mode is admitting something that
	// should have been refused. On the one class where the undercount is known to be enormous the
	// taxed term alone already refuses it, so nothing rides on the runs there.
	EXPECT_LT(gsTileGpuClassCost(357, 7), gsTileGpuClassCost(7, 357));
	EXPECT_LT(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(357, 7));
}

TEST(TileGpuDeclaringBudget, TheBudgetSitsWhereTheCorpusSeparates)
{
	// The calibration, pinned where a re-tune will see it. Per-class (taxed, runs) over the 18-dump
	// corpus under the Adreno pass shape, with the SD865's verdict on each where it ran:
	//
	//   must refuse   Xenosaga's alpha-MSB FBMSK (950, 3855), FlatOut 2's exotic blends (0, 896),
	//                 Baldur's Gate's 16-bit blends (473, 5), Ratchet & Clank's effect blends
	//                 (357, 7), and MGS3's exotic blends (0, 162) -- the last added by the alpha
	//                 probe, which force-refused that class and took MGS3 from 29.1 ms to 19.73,
	//                 the 19.725 pre-regression floor to within 0.05%.
	//   must admit    Katamari's punctuation FBMSK (195, 16), Beyond Good & Evil's FBMSK (42, 20),
	//                 Shadow of the Colossus' exotic blends (44, 8), Yu-Gi-Oh's (29, 17), GT4
	//                 Online Beta's (43, 2)
	//
	// MGS3 and Katamari are the pair that binds now, and they are what fixes alpha rather than the
	// line: 162a > 195 + 16a needs a > 1.336 merely to SEPARATE them, and 162a > 256 needs a > 1.581
	// to put MGS3 on the far side of the line where it already sat.
	EXPECT_LT(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(0, 162));   // MGS3
	EXPECT_LT(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(357, 7));   // Ratchet
	EXPECT_LT(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(0, 896));   // FlatOut 2
	EXPECT_LT(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(473, 5));   // Baldur's Gate
	EXPECT_LT(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(950, 3855)); // Xenosaga
	EXPECT_GE(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(195, 16));  // Katamari
	EXPECT_GE(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(42, 20));   // Beyond Good & Evil
	EXPECT_GE(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(44, 8));    // Shadow of the Colossus
	EXPECT_GE(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(29, 17));   // Yu-Gi-Oh
	EXPECT_GE(kGSTileGpuDeclaringRefuseAbove, gsTileGpuClassCost(43, 2));    // GT4 Online Beta

	// ⚠️ ...and the UPPER bound on alpha, which is the spike-recovery property and not a cost at all.
	// A class that must be admitted has to be able to come BACK after a transient spike refuses it,
	// so its cost has to stay under the RE-ADMIT line, not merely under the refuse line. Katamari's
	// (195, 16) is 220 at alpha = 8/5 and would be 227 at alpha = 2 -- over the line, latched off for
	// good, which is exactly the bug a 192 re-admit line would have caused at alpha = 1. So alpha is
	// bounded above by 1.8125 as well as below by 1.581, and 8/5 is the fraction in that window that
	// leaves BOTH lines where they were.
	EXPECT_GE(kGSTileGpuDeclaringReadmitAtOrUnder, gsTileGpuClassCost(195, 16));
	EXPECT_LT(kGSTileGpuDeclaringReadmitAtOrUnder, kGSTileGpuDeclaringRefuseAbove);
}

// -- the device gate ---------------------------------------------------------------------------

TEST(TileGpuDeclaringBudget, ADeviceThatDoesNotChargeForDeclaringAdmitsEverythingAlways)
{
	// The gate on the whole mechanism. Nothing about the budget may be observable where declaring is
	// free, however dense the frame -- that is the byte-identity the default polarity has to keep,
	// and it covers the mid-frame bootstrap too.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsFree);
	EXPECT_EQ(kGSTileGpuClassAll, b.admitted);
	for (int frame = 0; frame < 4; frame++)
	{
		EXPECT_EQ(kGSTileGpuClassAll, RunDenseFrame(b, kGSTileGpuClassAll, 100000));
		EXPECT_EQ(kGSTileGpuClassAll, b.admitted) << "mid-frame flip fired on a device that is not charging";
	}
}

// -- the in-frame bootstrap --------------------------------------------------------------------

TEST(TileGpuDeclaringBudget, AnUnpricedClassStartsAdmitted)
{
	// There is no startup polarity any more. Assuming the worst for an unmeasured class cost one
	// frame of approximation, and one frame is NOT transient: targets persist, so anything drawn
	// once into a target later frames do not redraw keeps what frame one wrote. On the device that
	// was Katamari's punctuation, missing from the screen, permanently.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	EXPECT_EQ(kGSTileGpuClassAll, b.admitted);
	EXPECT_EQ(0u, b.priced);
}

TEST(TileGpuDeclaringBudget, AClassThatFitsUnderTheLineIsExactFromTheFirstDraw)
{
	// ★ The headline: Katamari's punctuation. Its FBMSK class costs 211 a frame, under the line, so
	// the bootstrap never trips and frame one writes the glyphs exactly. Under the startup polarity
	// this frame was approximated and the damage was permanent.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (u32 d = 0; d < 211; d++)
	{
		b.Charge(kGSTileGpuClassPartialMask, d == 0 ? kGSTileGpuClassPartialMask : 0);
		ASSERT_EQ(kGSTileGpuClassPartialMask, b.admitted & kGSTileGpuClassPartialMask) << "draw " << d;
	}
	b.Roll();
	EXPECT_EQ(kGSTileGpuClassPartialMask, b.admitted & kGSTileGpuClassPartialMask);
}

TEST(TileGpuDeclaringBudget, AnUnpricedClassIsRefusedTheMomentItCrossesTheLine)
{
	// The other side of the same bet: exposure is bounded by the LINE, not by the frame. Xenosaga's
	// FBMSK is 4805 draws a frame; a whole admitted frame of it measured 303 ms on the device and
	// took a kernel-level GPU fault. Here it is stopped after about 256.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	u32 admitted_draws = 0;
	for (u32 d = 0; d < 4805; d++)
	{
		const bool was_admitted = (b.admitted & kGSTileGpuClassPartialMask) != 0;
		admitted_draws += was_admitted ? 1u : 0u;
		b.Charge(kGSTileGpuClassPartialMask, d == 0 ? kGSTileGpuClassPartialMask : 0);
		// Monotone: exactness only ever goes away within a frame, it never comes back.
		EXPECT_TRUE(was_admitted || (b.admitted & kGSTileGpuClassPartialMask) == 0) << "draw " << d;
	}
	EXPECT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);
	EXPECT_GE(kGSTileGpuDeclaringRefuseAbove + 2, admitted_draws);
	EXPECT_LE(kGSTileGpuDeclaringRefuseAbove, admitted_draws);
}

TEST(TileGpuDeclaringBudget, TheBootstrapCountsRunsAsWellAsTaxedDraws)
{
	// FlatOut 2's shape: 896 draws, every one alone in its own declared run. Under the old one-term
	// cost this frame was free and stayed admitted; it must now trip like any other.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (u32 d = 0; d < 896; d++)
		b.Charge(kGSTileGpuClassExoticBlend, kGSTileGpuClassExoticBlend);
	EXPECT_EQ(0u, b.admitted & kGSTileGpuClassExoticBlend);
	b.Roll();
	EXPECT_EQ(0u, b.admitted & kGSTileGpuClassExoticBlend);
}

TEST(TileGpuDeclaringBudget, APricedClassKeepsItsVerdictForTheWholeFrame)
{
	// The bootstrap is an exception for the unpriced case ONLY. A class whose price is known keeps
	// the frame-constant verdict, because a verdict moving mid-frame keys two draws of one class
	// into different passes for no reason the measurement asked for.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	RunDenseFrame(b, kGSTileGpuClassDate, 4); // cheap: priced and admitted
	ASSERT_EQ(kGSTileGpuClassDate, b.priced & kGSTileGpuClassDate);
	ASSERT_EQ(kGSTileGpuClassDate, b.admitted & kGSTileGpuClassDate);

	// Now the scene explodes. The class is priced, so it rides the whole frame admitted...
	for (u32 d = 0; d < kGSTileGpuDeclaringRefuseAbove * 4; d++)
	{
		b.Charge(kGSTileGpuClassDate, d == 0 ? kGSTileGpuClassDate : 0);
		ASSERT_EQ(kGSTileGpuClassDate, b.admitted & kGSTileGpuClassDate) << "draw " << d;
	}
	// ...and pays for it at the boundary, not before.
	b.Roll();
	EXPECT_EQ(0u, b.admitted & kGSTileGpuClassDate);
}

TEST(TileGpuDeclaringBudget, ReAdmittingAClassMakesItUnpricedAgain)
{
	// A re-admission is a guess: the peak that justified it was measured while the class was
	// refused. So the frame that tries the class again gets the bootstrap's mid-frame guard rather
	// than a whole frame at whatever the class now costs -- which is the 303 ms frame, arriving late.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	RunDenseFrame(b, kGSTileGpuClassPartialMask, kGSTileGpuDeclaringRefuseAbove * 4);
	ASSERT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);

	int quiet = 0;
	while ((b.admitted & kGSTileGpuClassPartialMask) == 0 && quiet < 64)
	{
		RunDenseFrame(b, kGSTileGpuClassDate, 1); // frames that draw, just not this class
		quiet++;
	}
	ASSERT_NE(0u, b.admitted & kGSTileGpuClassPartialMask) << "never came back";
	EXPECT_EQ(0u, b.priced & kGSTileGpuClassPartialMask) << "a re-admitted class must be bootstrapped again";

	// ...and the guard really is armed: the retry frame stops at the line.
	for (u32 d = 0; d < kGSTileGpuDeclaringRefuseAbove * 4; d++)
		b.Charge(kGSTileGpuClassPartialMask, d == 0 ? kGSTileGpuClassPartialMask : 0);
	EXPECT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);
}

// -- the peak, the hysteresis and the handoff ----------------------------------------------------

TEST(TileGpuDeclaringBudget, TheFrameJustDrawnDecidesTheNextOne)
{
	// The handoff, for a class already priced: a dense frame turns it off, and it stays off.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	ASSERT_EQ(kGSTileGpuClassExoticBlend, b.admitted & kGSTileGpuClassExoticBlend);
	EXPECT_EQ(0u, RunDenseFrame(b, kGSTileGpuClassExoticBlend, kGSTileGpuDeclaringRefuseAbove * 3) &
					  kGSTileGpuClassExoticBlend);
	EXPECT_EQ(0u, RunDenseFrame(b, kGSTileGpuClassExoticBlend, kGSTileGpuDeclaringRefuseAbove * 3) &
					  kGSTileGpuClassExoticBlend);
}

TEST(TileGpuDeclaringBudget, ARefusedClassKeepsMeasuringWhatItWouldHaveCost)
{
	// ⚠️ The property the whole design turns on. The budget is charged with what draws WANT, not with
	// what they were admitted for, so a class that is refused goes on measuring its own density and
	// stays refused. Charge it with its admitted population instead and it measures zero the moment
	// it is refused, re-admits, floods, refuses -- a two-frame oscillator that renders a different
	// picture every frame and is subtle enough to survive review, because both pictures are valid.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (int frame = 0; frame < 8; frame++)
	{
		EXPECT_EQ(0u, RunDenseFrame(b, kGSTileGpuClassPartialMask, kGSTileGpuDeclaringRefuseAbove * 3) &
						  kGSTileGpuClassPartialMask)
			<< "frame " << frame;
	}
}

TEST(TileGpuDeclaringBudget, AClassExpensiveEveryOtherFrameStaysRefused)
{
	// ⚠️ The defect that a one-frame-lagged budget has and this one does not. Half the corpus presents
	// at 30 Hz over a 60 Hz vsync and draws NOTHING in every second frame, so last-frame's-cost is
	// read off the empty frame and the heavy frame is admitted -- every time. Measured before the
	// peak existed: Xenosaga's FBMSK alternates 4805 and 0, and was refused in only 62% of frames
	// while the frames it was refused in were the cheap ones.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (int frame = 0; frame < 12; frame++)
	{
		if (frame % 2 == 0)
			RunDenseFrame(b, kGSTileGpuClassPartialMask, kGSTileGpuDeclaringRefuseAbove * 3);
		else
			b.Roll();
		EXPECT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask) << "frame " << frame;
	}
}

TEST(TileGpuDeclaringBudget, AnEmptyFrameIsNotEvidence)
{
	// ⚠️ The same trap one level down, and it is not hypothetical: it is what stopped alpha = 8/5
	// doing anything for MGS3. Its exotic blends cost 259 against a 256 line; one decay step takes
	// that to 195; 195 is under the RE-ADMIT line, so on every empty off-frame the class came back
	// -- and a re-admitted class is unpriced, so the bootstrap then let 161 of its 162 draws through
	// before the guard fired. The teardown counter said "refused in 100% of frames" the whole time,
	// because it samples the verdict at the END of a frame.
	//
	// So a frame that charged NOTHING changes nothing at all.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	RunSpreadFrame(b, kGSTileGpuClassExoticBlend, 162); // MGS3's shape: 162 solo declared passes
	ASSERT_EQ(0u, b.admitted & kGSTileGpuClassExoticBlend);
	const u32 peak_after = b.peak[1];
	const u32 priced_after = b.priced;
	for (int frame = 0; frame < 8; frame++)
	{
		b.Roll(); // the 30 Hz title's off-frame: nothing drawn at all
		EXPECT_EQ(peak_after, b.peak[1]) << "an empty frame decayed the peak, frame " << frame;
		EXPECT_EQ(priced_after, b.priced) << "an empty frame changed pricedness, frame " << frame;
		EXPECT_EQ(0u, b.admitted & kGSTileGpuClassExoticBlend) << "frame " << frame;
	}
}

TEST(TileGpuDeclaringBudget, MgsThreesShapeIsRefusedFromTheSecondFrameOn)
{
	// The stage-4 gate as a unit test: 162 draws, every one its own declared pass, alternating with
	// empty frames. Frame one is the bootstrap's bounded exposure and everything after it is zero.
	// Force-refusing this class on the SD865 took MGS3 from 29.1 ms to 19.73, the pre-regression
	// floor to within 0.05%.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (int frame = 0; frame < 8; frame++)
	{
		u32 admitted_draws = 0;
		if (frame % 2 == 0)
		{
			for (u32 d = 0; d < 162; d++)
			{
				admitted_draws += (b.admitted & kGSTileGpuClassExoticBlend) ? 1u : 0u;
				b.Charge(kGSTileGpuClassExoticBlend, kGSTileGpuClassExoticBlend);
			}
		}
		b.Roll();
		if (frame == 0)
			EXPECT_LT(0u, admitted_draws) << "the bootstrap must let the first frame through";
		else
			EXPECT_EQ(0u, admitted_draws) << "frame " << frame << " admitted draws it should not have";
	}
}

TEST(TileGpuDeclaringBudget, APeakDecaysSoAQuietSceneIsReAdmitted)
{
	// Recently-expensive must not mean permanently refused. A quarter a frame is about eight frames
	// of genuine quiet.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	RunDenseFrame(b, kGSTileGpuClassPartialMask, kGSTileGpuDeclaringRefuseAbove * 3);
	ASSERT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);

	// ⚠️ The quiet frames have to DRAW something, or they are not evidence and do not decay anything.
	// A scene going quiet for this class is a scene still drawing other classes.
	int frames = 0;
	while ((b.admitted & kGSTileGpuClassPartialMask) == 0 && frames < 64)
	{
		RunDenseFrame(b, kGSTileGpuClassDate, 1);
		frames++;
	}
	EXPECT_LT(0, frames);
	EXPECT_GT(20, frames) << "a class that has calmed down waited " << frames << " frames to come back";
}

TEST(TileGpuDeclaringBudget, AMustAdmitClassComesBackAfterATransientSpike)
{
	// The reason the re-admit line is 224 and not a round fraction of the other one. Katamari's
	// punctuation costs 211 every frame. One bad frame refuses it; if the re-admit line sat below
	// 211 the peak would decay to 211, stop there, and the punctuation would be off for good.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	RunDenseFrame(b, kGSTileGpuClassPartialMask, kGSTileGpuDeclaringRefuseAbove * 3); // the spike
	ASSERT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);
	for (int frame = 0; frame < 40 && (b.admitted & kGSTileGpuClassPartialMask) == 0; frame++)
		RunDenseFrame(b, kGSTileGpuClassPartialMask, 211); // Katamari's ordinary frame
	EXPECT_EQ(kGSTileGpuClassPartialMask, b.admitted & kGSTileGpuClassPartialMask)
		<< "a class that must be admitted was latched off by one bad frame";
}

TEST(TileGpuDeclaringBudget, ADrawThatOpensARunIsCountedAsARunAndNotAsATaxedDraw)
{
	// The two terms, kept apart in the counters even though alpha makes their sum the verdict: they
	// were measured separately and a device round that prices a declared pass differently has to be
	// able to see them.
	GSTileGpuDeclaringBudget spread;
	spread.Start(kDeclaringIsTaxed);
	for (u32 r = 0; r < 100; r++)
		spread.Charge(kGSTileGpuClassExoticBlend, kGSTileGpuClassExoticBlend);
	EXPECT_EQ(0u, spread.taxed[1]);
	EXPECT_EQ(100u, spread.runs[1]);

	GSTileGpuDeclaringBudget dense;
	dense.Start(kDeclaringIsTaxed);
	dense.Charge(kGSTileGpuClassExoticBlend, kGSTileGpuClassExoticBlend);
	for (u32 d = 1; d < 100; d++)
		dense.Charge(kGSTileGpuClassExoticBlend, 0);
	EXPECT_EQ(99u, dense.taxed[1]);
	EXPECT_EQ(1u, dense.runs[1]);

	// ...and the SPREAD one costs MORE, which is the whole of alpha above one: the same hundred
	// draws are dearer when each of them is a declared pass of its own than when they share one.
	// MGS3 is the dump that proved it -- 162 draws, 162 solo passes, 9.5 ms.
	EXPECT_LT(gsTileGpuClassCost(dense.taxed[1], dense.runs[1]),
		gsTileGpuClassCost(spread.taxed[1], spread.runs[1]));
}

TEST(TileGpuDeclaringBudget, TheCountersResetWithTheFrame)
{
	// Two lean frames in a row must not add up to one fat one.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (int frame = 0; frame < 8; frame++)
	{
		EXPECT_EQ(kGSTileGpuClassQuantisedBlend,
			RunDenseFrame(b, kGSTileGpuClassQuantisedBlend, kGSTileGpuDeclaringRefuseAbove / 2) &
				kGSTileGpuClassQuantisedBlend)
			<< "frame " << frame;
		for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
		{
			EXPECT_EQ(0u, b.taxed[c]) << "class " << c << " frame " << frame;
			EXPECT_EQ(0u, b.runs[c]) << "class " << c << " frame " << frame;
		}
	}
}

TEST(TileGpuDeclaringBudget, OneClassCrossingTheLineDoesNotTakeTheOthersWithIt)
{
	// The classes are budgeted independently, and the bootstrap does not change that. Xenosaga's
	// frame is exactly this shape: the FBMSK class refused, the exotic blends admitted alongside it.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	const u32 both = kGSTileGpuClassPartialMask | kGSTileGpuClassExoticBlend;
	for (u32 d = 0; d < 4805; d++)
	{
		const u32 wanted = (d % 100 == 0) ? both : kGSTileGpuClassPartialMask;
		b.Charge(wanted, d == 0 ? both : 0);
	}
	EXPECT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);
	EXPECT_EQ(kGSTileGpuClassExoticBlend, b.admitted & kGSTileGpuClassExoticBlend);
}

// ---------------------------------------------------------------------------------------------
// ...and the quantise-on-write half, which is what a refused 16-bit blend falls back to. It
// executes on EVERY device, needs no read, and is where most of the 16-bit accuracy lives: on an M2
// with no read road at all it moved Yu-Gi-Oh from 4.07 to 2.01 mean absolute channel error against
// the software golden, OutRun 10.77 to 6.04 and FlatOut 2 14.42 to 12.73.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuQuantiseOnWrite, AnUnblendedDrawQuantisesWhateverTheDeviceSays)
{
	// Nothing runs after the fragment stage, so the shader's output IS what lands.
	EXPECT_TRUE(gsTileGpuQuantisesOnWrite(/*frame_quantises=*/true, /*blend_active=*/false, /*shader_blend=*/false));
}

TEST(TileGpuQuantiseOnWrite, AFixedFunctionBlendedDrawQuantisesNothing)
{
	// The blend unit runs after the fragment stage and the console quantises after the blend, so
	// there is no value here worth truncating.
	EXPECT_FALSE(gsTileGpuQuantisesOnWrite(/*frame_quantises=*/true, /*blend_active=*/true, /*shader_blend=*/false));
}

TEST(TileGpuQuantiseOnWrite, AShaderBlendedDrawQuantisesTheBlendsResult)
{
	EXPECT_TRUE(gsTileGpuQuantisesOnWrite(/*frame_quantises=*/true, /*blend_active=*/true, /*shader_blend=*/true));
}

TEST(TileGpuQuantiseOnWrite, AFrameThatStoresEightBitsQuantisesNobody)
{
	// Which formats those are is pinned against GSLocalMemory's own table in
	// TileGpuCT16Road.OnlyTheSixteenBitFamilyQuantises; this is only that the answer gates everything
	// else, so a 32-bit frame cannot be truncated by any combination of the two blend questions.
	for (const bool blend : {false, true})
	{
		for (const bool shader : {false, true})
			EXPECT_FALSE(gsTileGpuQuantisesOnWrite(/*frame_quantises=*/false, blend, shader));
	}
}

TEST(TileGpuQuantiseOnWrite, ARefusedSixteenBitBlendFallsBackToTheWHOLEApproximation)
{
	// The composition, as one fact, because getting half of it right is a picture rather than an
	// error. A refused draw takes fixed-function blending AND is not truncated -- the console
	// quantises the blend's result, and the blend unit runs after the shader, so a shader that
	// truncated here would truncate the wrong value.
	for (const bool admitted : {false, true})
	{
		const u32 verdict = admitted ? kGSTileGpuClassAll : (kGSTileGpuClassAll & ~kGSTileGpuClassQuantisedBlend);
		const u32 uses = gsTileGpuSelfReadUses(kGSTileGpuClassQuantisedBlend, verdict, kRoad);
		const bool shader_blend = (uses & GSDevice::kGSTileGpuSelfBlend) != 0;
		EXPECT_EQ(admitted, shader_blend);
		EXPECT_EQ(admitted,
			gsTileGpuQuantisesOnWrite(/*frame_quantises=*/true, /*blend_active=*/true, shader_blend));
	}
}

// ---------------------------------------------------------------------------------------------
// The FROZEN PER-DRAW STATE (GSDevice::GSTileGpuFragmentSpec), the second half of the same key.
//
// The four masks above say which ROADS a program carries. These ten fields say what the GS state
// IS for every draw it serves, so the fragment stage neither loads the field nor branches on it.
// Measured offline against the device's own Turnip (mesa 26.1.2, a650): the materialised-source
// road goes 390 instructions / 12 registers / wave64 to 89 / 4 / wave128, the resident-target road
// 663 / 12 / wave64 to 172 / 6 / wave128, and the untextured road 192 / 4 to 16 / 3 with no state
// read left at all. The register number is the one that matters -- mesa's rule is
// `regs * 2 <= reg_size_vec4 / 4`, a650's reg_size_vec4 is 64, so eight registers is a THRESHOLD:
// at eight a program runs wave128 with 1280 fragments in flight, at nine wave64 with 640.
//
// Unlike the masks these axes have no PASS union. Nothing about a render pass depends on which
// alpha comparison its draws run, so there is nothing to be a subset of and the executor's subset
// assert deliberately does not mention them. They are run-only, and the fallback for a plan
// carrying no variant stream is `valid == false`, which reads every field from the row.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuVariantKey, TheFrozenStateRoundTripsAndNoneCollides)
{
	// Ten fields in eighteen bits above the four masks. A field bleeding into its neighbour would
	// compile a program that freezes the WRONG constant -- no fault, no validation error, just the
	// wrong pixels, which is the failure mode this whole key exists to avoid.
	using Plan = GSDevice::GSTileGpuPassPlan;
	using Spec = GSDevice::GSTileGpuFragmentSpec;
	std::vector<u32> keys;
	// atst is the row's own encoding: 0 = no test, else TEST.ATST + 1, and only LESS..NOTEQUAL
	// (3..8) can reach the fragment stage -- NEVER and ALWAYS are folded into the write flags.
	static constexpr u8 kAtst[7] = {0, 3, 4, 5, 6, 7, 8};
	for (const u8 fst : {0, 1})
		for (const u8 ltf : {0, 1})
			for (const u8 tfx : {0, 1, 2, 3})
				for (const u8 tcc : {0, 1})
					for (const u8 atst : kAtst)
						for (const u8 fge : {0, 1})
							for (const u8 date : {0, 1, 2})
								for (const u8 wms : {0, 1, 2, 3})
									for (const u8 wmt : {0, 1, 2, 3})
										for (const u8 texa : {0, 1, 2, 3})
										{
											Spec s;
											s.valid = true;
											s.fst = fst;
											s.ltf = ltf;
											s.tfx = tfx;
											s.tcc = tcc;
											s.atst = atst;
											s.fge = fge;
											s.date = date;
											s.wms = wms;
											s.wmt = wmt;
											s.texa = texa;
											const u32 key = Plan::PackVariantKey(
												GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuTexelIndex8, 0, false, s);
											ASSERT_EQ(Plan::VariantSpec(key), s);
											// ...and the mask half is untouched by any of it.
											ASSERT_EQ(Plan::VariantRoadMask(key), GSDevice::kGSTileGpuRoadByte);
											ASSERT_EQ(Plan::VariantTexelMask(key), GSDevice::kGSTileGpuTexelIndex8);
											ASSERT_EQ(Plan::VariantSelfMask(key), 0u);
											ASSERT_FALSE(Plan::VariantQuantises(key));
											keys.push_back(key);
										}
	const size_t total = keys.size();
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	EXPECT_EQ(keys.size(), total);
	// The spec half lives entirely in bits 13-30 -- bit 31 stays free for the next axis, and
	// nothing below 13 moves.
	for (const u32 key : keys)
		ASSERT_EQ(key & ~(0x1FFFu | Plan::kVariantSpecMask), 0u);
}

TEST(TileGpuVariantKey, AnUnspecializedKeyIsTheKeyThatShippedBefore)
{
	// The fallback, stated as an identity: a key packed with no frozen state is bit-for-bit the key
	// the four-mask packer produces, so a plan built before this axis existed and the pass-union
	// arm of the gate both land on exactly the program set that shipped.
	using Plan = GSDevice::GSTileGpuPassPlan;
	for (u32 road = 0; road <= GSDevice::kGSTileGpuRoadMaskAll; road++)
	{
		for (u32 self = 0; self <= GSDevice::kGSTileGpuSelfMaskAll; self++)
		{
			for (const bool q : {false, true})
			{
				const u32 bare = Plan::PackVariantKey(road, 3u, self, q);
				const u32 with = Plan::PackVariantKey(road, 3u, self, q, GSDevice::GSTileGpuFragmentSpec());
				ASSERT_EQ(bare, with);
				ASSERT_FALSE(Plan::VariantSpec(bare).valid);
				ASSERT_EQ(bare & Plan::kVariantSpecMask, 0u);
			}
		}
	}
}

TEST(TileGpuRunKey, EveryFrozenStateAxisCutsTheRunOnItsOwn)
{
	// Ten axes, one key. Two draws sharing an indirect run share a pipeline and therefore a fragment
	// program, so a draw whose alpha test the program froze at GEQUAL must not ride in the run of a
	// draw that tests EQUAL -- and the symptom would be a wrong pixel, never a fault, because the
	// frozen program simply does not read the row. Each axis is pinned separately for that reason.
	using Plan = GSDevice::GSTileGpuPassPlan;
	using Spec = GSDevice::GSTileGpuFragmentSpec;
	Spec base;
	base.valid = true;
	const auto key = [](const Spec& s) {
		return Plan::PackVariantKey(GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuTexelIndex8, 0, false, s);
	};
	std::vector<std::pair<const char*, Spec>> others;
	const auto add = [&](const char* name, auto&& mutate) {
		Spec s = base;
		mutate(s);
		others.emplace_back(name, s);
	};
	add("fst", [](Spec& s) { s.fst = 1; });
	add("ltf", [](Spec& s) { s.ltf = 1; });
	add("tfx", [](Spec& s) { s.tfx = 2; });
	add("tcc", [](Spec& s) { s.tcc = 1; });
	add("atst", [](Spec& s) { s.atst = 5; });
	add("fge", [](Spec& s) { s.fge = 1; });
	add("date", [](Spec& s) { s.date = 2; });
	add("wms", [](Spec& s) { s.wms = 3; });
	add("wmt", [](Spec& s) { s.wmt = 3; });
	add("texa", [](Spec& s) { s.texa = 3; });
	// ...and the presence flag itself: a draw whose state is frozen may not share a run with one
	// reading the row, even when every frozen field happens to be zero.
	Spec unspecialized;
	others.emplace_back("valid", unspecialized);

	for (const auto& [name, s] : others)
	{
		const RunPlan p{{Topology::Triangle, 0u, DepthMode::None, key(base)},
			{Topology::Triangle, 0u, DepthMode::None, key(s)}};
		EXPECT_NE(p.At(0), p.At(1)) << "the " << name << " axis did not cut the run";
	}
}

TEST(TileGpuVariantKey, TheRoadNarrowsTheFrozenStateToWhatTheProgramCanRead)
{
	// A field the compiled program cannot read must not be in the key, or two programs that come out
	// character-identical become two modules, two pipelines and two indirect calls. Both rules are
	// tilegpu.glsl's own #ifs rather than a guess: the texture function, the coordinate kind, the
	// filter and the wrap modes live inside TILEGPU_TEXTURED, which no road takes out entirely; and
	// TEXA lives inside TILEGPU_TAP_ANY, the two roads that go through the per-texel tap. A
	// materialised source (rule 3) has TEXA baked into its image by the prep pass.
	using Spec = GSDevice::GSTileGpuFragmentSpec;
	Spec full;
	full.valid = true;
	full.fst = 1;
	full.ltf = 1;
	full.tfx = 3;
	full.tcc = 1;
	full.atst = 5;
	full.fge = 1;
	full.date = 2;
	full.wms = 3;
	full.wmt = 3;
	full.texa = 3;

	Spec untextured = full;
	untextured.NarrowToRoad(0);
	EXPECT_TRUE(untextured.valid);
	EXPECT_EQ(untextured.fst, 0);
	EXPECT_EQ(untextured.ltf, 0);
	EXPECT_EQ(untextured.tfx, 0);
	EXPECT_EQ(untextured.tcc, 0);
	EXPECT_EQ(untextured.wms, 0);
	EXPECT_EQ(untextured.wmt, 0);
	EXPECT_EQ(untextured.texa, 0);
	// ...and the three the untextured program still reads survive, because it still tests alpha,
	// still fogs and still runs the destination-alpha test.
	EXPECT_EQ(untextured.atst, 5);
	EXPECT_EQ(untextured.fge, 1);
	EXPECT_EQ(untextured.date, 2);

	Spec source = full;
	source.NarrowToRoad(GSDevice::kGSTileGpuRoadSource);
	EXPECT_EQ(source.texa, 0);
	EXPECT_EQ(source.wms, 3); // rule 3's NEAREST arm applies the same wrap the byte road does
	EXPECT_EQ(source.ltf, 1);

	for (const u32 road : {GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuRoadTarget,
			 GSDevice::kGSTileGpuRoadByte | GSDevice::kGSTileGpuRoadSource})
	{
		Spec tap = full;
		tap.NarrowToRoad(road);
		EXPECT_EQ(tap.texa, 3) << "road " << road << " goes through the tap and applies TEXA";
	}
}
