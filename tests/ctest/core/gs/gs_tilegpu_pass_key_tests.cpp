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
u32 RunFrame(GSTileGpuDeclaringBudget& b, u32 classes, u32 runs, u32 per_run, bool taxes)
{
	for (u32 r = 0; r < runs; r++)
	{
		b.Charge(classes, /*opening=*/classes); // the run's first draw stands behind nobody
		for (u32 d = 1; d < per_run; d++)
			b.Charge(classes, /*opening=*/0);
	}
	b.Roll(taxes);
	return b.admitted;
}
} // namespace

TEST(TileGpuDeclaringBudget, ADeviceThatDoesNotChargeForDeclaringAdmitsEverythingAlways)
{
	// The gate on the whole mechanism. Nothing about the budget may be observable where declaring is
	// free, however dense the frame -- that is the byte-identity the default polarity has to keep.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsFree);
	EXPECT_EQ(kGSTileGpuClassAll, b.admitted);
	for (int frame = 0; frame < 4; frame++)
		EXPECT_EQ(kGSTileGpuClassAll, RunFrame(b, kGSTileGpuClassAll, 1, 100000, kDeclaringIsFree));
}

TEST(TileGpuDeclaringBudget, TheStartupPolarityRefusesOnlyTheClassesThatCanGoBulk)
{
	// Frame one runs before any measurement exists. The bet is taken toward not hanging the GPU: the
	// two classes the census has seen reach thousands of draws a frame start off, the three it bounds
	// at tens start on.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	EXPECT_EQ(0u, b.admitted & kGSTileGpuClassQuantisedBlend);
	EXPECT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);
	EXPECT_EQ(kGSTileGpuClassDate, b.admitted & kGSTileGpuClassDate);
	EXPECT_EQ(kGSTileGpuClassExoticBlend, b.admitted & kGSTileGpuClassExoticBlend);
	EXPECT_EQ(kGSTileGpuClassAfailKeep, b.admitted & kGSTileGpuClassAfailKeep);
}

TEST(TileGpuDeclaringBudget, TheFrameJustDrawnDecidesTheNextOne)
{
	// The handoff. A bulk-capable class starts refused and turns ON once a frame proves it sparse;
	// a sparse class starts admitted and turns OFF once a frame proves it dense.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	ASSERT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);
	// One run of 9 draws costs 8 -- comfortably under the budget.
	EXPECT_EQ(kGSTileGpuClassPartialMask,
		RunFrame(b, kGSTileGpuClassPartialMask, 1, 9, kDeclaringIsTaxed) & kGSTileGpuClassPartialMask);

	GSTileGpuDeclaringBudget d;
	d.Start(kDeclaringIsTaxed);
	ASSERT_EQ(kGSTileGpuClassExoticBlend, d.admitted & kGSTileGpuClassExoticBlend);
	EXPECT_EQ(0u,
		RunFrame(d, kGSTileGpuClassExoticBlend, 1, kGSTileGpuDeclaringRefuseAbove + 2, kDeclaringIsTaxed) &
			kGSTileGpuClassExoticBlend);
}

TEST(TileGpuDeclaringBudget, ADrawThatOpensARunCostsNothing)
{
	// The cost measure, stated on its own: the tax is a flush per OVERLAPPING primitive, so the first
	// draw of a declaring pass has nothing to overlap. A frame of N one-draw runs costs zero -- which
	// is FlatOut 2, 448 draws in 448 declaring passes -- and one run of N costs N-1, which is Baldur's
	// Gate, 478 draws in 4.88.
	GSTileGpuDeclaringBudget spread;
	spread.Start(kDeclaringIsTaxed);
	for (u32 r = 0; r < 1000; r++)
		spread.Charge(kGSTileGpuClassExoticBlend, kGSTileGpuClassExoticBlend);
	EXPECT_EQ(0u, spread.exposed[1]);

	GSTileGpuDeclaringBudget dense;
	dense.Start(kDeclaringIsTaxed);
	dense.Charge(kGSTileGpuClassExoticBlend, kGSTileGpuClassExoticBlend);
	for (u32 d = 1; d < 1000; d++)
		dense.Charge(kGSTileGpuClassExoticBlend, 0);
	EXPECT_EQ(999u, dense.exposed[1]);
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
		const u32 verdict = RunFrame(b, kGSTileGpuClassPartialMask, 1, kGSTileGpuDeclaringRefuseAbove + 2,
			kDeclaringIsTaxed);
		EXPECT_EQ(0u, verdict & kGSTileGpuClassPartialMask) << "frame " << frame;
	}
}

TEST(TileGpuDeclaringBudget, HysteresisHoldsAClassHoveringAtTheBudget)
{
	// Between the budget and twice it, whichever way the class is already set is the way it stays.
	// Without the band a class sitting near the line changes what the frame is exact about every
	// frame; both renderings are correct, so the flicker is subtle rather than broken.
	const u32 hover = kGSTileGpuDeclaringAdmitAtOrUnder + 1; // above the admit line, below the refuse line

	GSTileGpuDeclaringBudget on;
	on.Start(kDeclaringIsTaxed); // exotic blend starts ADMITTED
	for (int frame = 0; frame < 4; frame++)
	{
		EXPECT_EQ(kGSTileGpuClassExoticBlend,
			RunFrame(on, kGSTileGpuClassExoticBlend, 1, hover + 1, kDeclaringIsTaxed) & kGSTileGpuClassExoticBlend)
			<< "frame " << frame;
	}

	GSTileGpuDeclaringBudget off;
	off.Start(kDeclaringIsTaxed); // partial FBMSK starts REFUSED
	for (int frame = 0; frame < 4; frame++)
	{
		EXPECT_EQ(0u,
			RunFrame(off, kGSTileGpuClassPartialMask, 1, hover + 1, kDeclaringIsTaxed) & kGSTileGpuClassPartialMask)
			<< "frame " << frame;
	}
}

TEST(TileGpuDeclaringBudget, TheCountersResetWithTheFrame)
{
	// Two lean frames in a row must not add up to one fat one.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (int frame = 0; frame < 8; frame++)
	{
		const u32 verdict = RunFrame(b, kGSTileGpuClassQuantisedBlend, 1,
			kGSTileGpuDeclaringAdmitAtOrUnder / 2, kDeclaringIsTaxed);
		EXPECT_EQ(kGSTileGpuClassQuantisedBlend, verdict & kGSTileGpuClassQuantisedBlend) << "frame " << frame;
		for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
			EXPECT_EQ(0u, b.exposed[c]) << "class " << c << " frame " << frame;
	}
}

TEST(TileGpuDeclaringBudget, TheBudgetSitsWhereTheCorpusSeparates)
{
	// The calibration, pinned where a retune will see it. These are measured per-class costs from the
	// 18-dump corpus under the Adreno pass shape, and the budget has to fall between the two groups:
	//
	//   admitted   Katamari's FBMSK 97, GT4 Online Beta's 16-bit blend 41, Beyond Good & Evil's
	//              FBMSK 40, OutRun's exotic blends 12, FlatOut 2's and MGS3's and Ratchet's 0
	//   refused    Baldur's Gate's 16-bit blend 473, Xenosaga's FBMSK 479
	//
	// The gap between 97 and 473 is the widest in the whole distribution, and the budget is inside it.
	// A change that moves the line out of that gap is a change of policy on real titles, so it should
	// fail here and be argued rather than noticed later on a device.
	EXPECT_LT(97u, kGSTileGpuDeclaringAdmitAtOrUnder);
	EXPECT_GT(473u, kGSTileGpuDeclaringAdmitAtOrUnder);
	EXPECT_GT(473u, kGSTileGpuDeclaringRefuseAbove);
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
