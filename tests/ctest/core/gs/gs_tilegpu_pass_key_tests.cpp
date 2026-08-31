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
#include "GS/Renderers/TileGpu/GSTileGpuShaderVariant.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string>
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
// The alpha split's second half, and why its depth write is a RUN cut and not a PASS one.
//
// gsTileGpuPlanAlphaSplit realizes an alpha test the plan could not decide as two draws over the
// same index range, and under the channel splits the principal writes no depth while the half
// writes it. That difference has to reach the PIPELINE, and it does: GSTileGpuRunKey carries the
// depth mode unconditionally, so the executor cuts an indirect run at the half and binds its
// variant whatever the pass key says. Putting the same difference in the PASS key as well bought
// nothing and cost a pass boundary each way on every device that keys passes on depth -- enough of
// them on R&C UYA that the per-frame depth predictor flipped the whole frame to merged keying to
// escape a 3.3x pass explosion (454 -> 1496 passes a frame under forced-uniform keying), and merged
// is itself a measured loss on Adreno.
//
// EmuCore/GS/TileGpuSplitSharesPassKey takes it back out: both halves key on the DRAW's own depth
// write, which the split's two passes union back to by its own contract. What follows pins that
// equality, that the run key still cuts, and that the key changes nothing off the split.
// ---------------------------------------------------------------------------------------------

namespace
{
/// One draw's alpha-split shape, and the two pass keys the two halves come out with.
struct SplitKeys
{
	u8 pass_count;
	GSTileGpuPassKey principal;
	GSTileGpuPassKey half;
	DepthMode principal_pipeline;
	DepthMode half_pipeline;
};

/// The renderer's own derivation, in the order AccumulateDraw runs it: plan the split, take the
/// principal's depth write from pass[0], and ask gsTileGpuPassDepthModeFor for each half's PASS
/// variant with the DRAW's write alongside the half's own.
SplitKeys SplitKeysFor(u32 afail, u8 color_mask, bool z_write, bool z_test, bool shares_pass_key,
	bool depth_uniform = kUniform)
{
	const GSTileAlphaSplit split = gsTileGpuPlanAlphaSplit(GSTileAlphaTestFold::Varies, ATST_GEQUAL,
		0x80, afail, color_mask, z_write, z_test, /*independent_z=*/true, /*independent_colour=*/true,
		/*lever=*/true, /*refuse_fb_only=*/false);
	const bool z_used = z_write || z_test;
	const bool p1_z_write = (split.pass_count == 2) ? split.pass[1].z_write : split.pass[0].z_write;
	const auto key = [&](bool half_z_write) {
		return gsTileGpuPassKeyFor(kColor, kZ,
			gsTileGpuPassDepthModeFor(z_used, z_test, z_write, half_z_write, shares_pass_key),
			depth_uniform, false, kShared);
	};
	return SplitKeys{split.pass_count, key(split.pass[0].z_write), key(p1_z_write),
		gsTileGpuDepthModeFor(z_used, z_test, split.pass[0].z_write),
		gsTileGpuDepthModeFor(z_used, z_test, p1_z_write)};
}

/// The three AFAIL modes the split serves, with a shape that actually splits under each. KEEP is
/// absent because the discard is exact there and no split is planned.
struct SplitShape
{
	const char* name;
	u32 afail;
	u8 color_mask;
	bool z_write;
};
constexpr SplitShape kSplitShapes[] = {
	// The whole colour for every fragment, depth for the ones that pass. Needs a depth write, or
	// the two sides write the same channels and the test goes away instead of splitting.
	{"FB_ONLY", AFAIL_FB_ONLY, kGSTileChannelsRGBA, true},
	// RGB for every fragment, alpha and depth for the ones that pass.
	{"RGB_ONLY", AFAIL_RGB_ONLY, kGSTileChannelsRGBA, true},
	// By fragment on the inverted comparison, because a channel split's first pass would write the
	// depth its second then tests against. Both halves write depth, so this one never differed.
	{"ZB_ONLY", AFAIL_ZB_ONLY, kGSTileChannelsRGBA, true},
};
} // namespace

TEST(TileGpuSplitPassKey, TheSecondHalfKeysItsPassLikeThePrincipal)
{
	// The whole point, under the polarity that can see it: on a device that keys passes on the
	// depth mode, the split must not cut one.
	for (const SplitShape& shape : kSplitShapes)
	{
		const SplitKeys k = SplitKeysFor(shape.afail, shape.color_mask, shape.z_write,
			/*z_test=*/true, /*shares_pass_key=*/true);
		EXPECT_EQ(k.pass_count, 2) << shape.name << " did not split";
		EXPECT_EQ(k.principal, k.half) << shape.name;
	}
}

TEST(TileGpuSplitPassKey, WithTheKeyOffTheChannelSplitsStillCutAPass)
{
	// The shipped-tip road, kept reachable and pinned so the OFF arm is a real revert rather than a
	// second spelling of the ON one. ZB_ONLY is not in this list: it splits by fragment and both of
	// its halves write depth, so it never cut a pass under either arm.
	for (const SplitShape& shape : {kSplitShapes[0], kSplitShapes[1]})
	{
		const SplitKeys k = SplitKeysFor(shape.afail, shape.color_mask, shape.z_write,
			/*z_test=*/true, /*shares_pass_key=*/false);
		EXPECT_EQ(k.pass_count, 2) << shape.name;
		EXPECT_NE(k.principal, k.half) << shape.name;
	}
	const SplitKeys zb = SplitKeysFor(AFAIL_ZB_ONLY, kGSTileChannelsRGBA, /*z_write=*/true,
		/*z_test=*/true, /*shares_pass_key=*/false);
	EXPECT_EQ(zb.principal, zb.half);
}

TEST(TileGpuSplitPassKey, TheDifferenceStillRidesTheRunKey)
{
	// What makes the pass-key merge sound: the halves' real difference is still in the PIPELINE,
	// so the executor cuts a run at the half and binds the variant that writes depth. The pass key
	// is not where that fact was being carried.
	for (const SplitShape& shape : {kSplitShapes[0], kSplitShapes[1]})
	{
		const SplitKeys k = SplitKeysFor(shape.afail, shape.color_mask, shape.z_write,
			/*z_test=*/true, /*shares_pass_key=*/true);
		EXPECT_NE(k.principal_pipeline, k.half_pipeline) << shape.name;
		const RunPlan plan{{Topology::Triangle, 0u, k.principal_pipeline},
			{Topology::Triangle, 0u, k.half_pipeline}};
		EXPECT_NE(plan.At(0), plan.At(1)) << shape.name;
		EXPECT_EQ(plan.Runs().size(), 2u) << shape.name;
	}
}

TEST(TileGpuSplitPassKey, TheDepthAttachmentIsTheSameFactUnderBothArms)
{
	// A render pass cannot gain or lose an attachment, so whatever else the key does, the two
	// halves must agree on whether there IS a depth attachment -- which is `depth_mode != None`,
	// and which comes off the DRAW's z_used under both arms. Checked on the shape where the
	// principal writes no depth at all.
	for (const bool shares : {false, true})
	{
		const SplitKeys k = SplitKeysFor(AFAIL_FB_ONLY, kGSTileChannelsRGBA, /*z_write=*/true,
			/*z_test=*/true, shares);
		EXPECT_TRUE(k.principal.depth_used) << "shares=" << shares;
		EXPECT_TRUE(k.half.depth_used) << "shares=" << shares;
	}
}

TEST(TileGpuSplitPassKey, ADrawThatDoesNotSplitKeysTheSameUnderBothArms)
{
	// The key's blast radius is the split and nothing else: where the plan returns one pass,
	// pass[0].z_write IS the draw's z_write, so both arms ask gsTileGpuDepthModeFor the same
	// question. This is why the corpus grid is byte-identical on the dumps with no splits.
	constexpr u32 kAfails[] = {AFAIL_KEEP, AFAIL_FB_ONLY, AFAIL_RGB_ONLY, AFAIL_ZB_ONLY};
	for (const u32 afail : kAfails)
	{
		for (const u8 mask : {u8(0), kGSTileChannelsRGB, kGSTileChannelsRGBA})
		{
			for (const bool z_write : {false, true})
			{
				for (const bool z_test : {false, true})
				{
					const SplitKeys on = SplitKeysFor(afail, mask, z_write, z_test, true);
					const SplitKeys off = SplitKeysFor(afail, mask, z_write, z_test, false);
					if (on.pass_count != 1)
						continue;
					EXPECT_EQ(on.principal, off.principal)
						<< "afail " << afail << " mask " << u32(mask) << " zw " << z_write
						<< " zt " << z_test;
				}
			}
		}
	}
}

TEST(TileGpuSplitPassKey, UnderMergedKeyingTheKeyChangesNothing)
{
	// On every device that does not key passes on the depth mode -- which is every device but
	// Adreno -- the two halves already shared a pass, so this key is inert there. Stated because it
	// is also why the M2 corpus grid cannot see the change and the device grid has to.
	for (const SplitShape& shape : kSplitShapes)
	{
		const SplitKeys on = SplitKeysFor(shape.afail, shape.color_mask, shape.z_write,
			/*z_test=*/true, /*shares_pass_key=*/true, kMerged);
		const SplitKeys off = SplitKeysFor(shape.afail, shape.color_mask, shape.z_write,
			/*z_test=*/true, /*shares_pass_key=*/false, kMerged);
		EXPECT_EQ(on.principal, on.half) << shape.name;
		EXPECT_EQ(off.principal, off.half) << shape.name;
		EXPECT_EQ(on.principal, off.principal) << shape.name;
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
// The one-class force-refuse probe (EmuCore/GS/TileGpuRefuseExoticBlendClass).
//
// A dev instrument, not a policy, and the suite's job is to hold it to the one thing that makes it
// usable as one: it refuses the class it names and touches nothing else -- not the two lines, not
// another class's verdict, not a draw's other reasons for reading.
//
// What it was for, and what it bought. The probe took arm D1 on the SD865: force-refusing Stuntman's
// exotic-blend class measured -13.783 ms of GPU across 154 declaring passes -- 96.6 us a declaring
// pass -- with OutRun's two scenes reading 111.6 and 103.3. Those three numbers are what the
// declaring budget's cost model was re-fitted to on 2026-08-31.
//
// ⚠️ It is no longer how that class gets refused. The re-fitted budget refuses it by itself, and
// OutRun's, Dirge's and Xenosaga's exotic blends with it. The key stays as an instrument, and the one
// thing it can still do that nothing else can is ask what a single class costs on a device that
// admits everything, where the budget is inert and cannot be made to say anything.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuDeclaringProbe, OffItIsTheExpressionThatWasThereBefore)
{
	// The gate the shipped default rides on: with no probe set, admission is exactly `wanted &
	// admitted` for every one of the 32 wanted sets against every one of the 32 verdicts. Nothing
	// about a frame can change if this holds.
	EXPECT_EQ(0u, gsTileGpuProbeRefusedClasses(false));
	for (u32 wanted = 0; wanted <= kGSTileGpuClassAll; wanted++)
	{
		for (u32 admitted = 0; admitted <= kGSTileGpuClassAll; admitted++)
		{
			EXPECT_EQ(wanted & admitted,
				gsTileGpuAdmittedClasses(wanted, admitted, gsTileGpuProbeRefusedClasses(false)))
				<< "wanted " << wanted << " admitted " << admitted;
		}
	}
}

TEST(TileGpuDeclaringProbe, OnItRefusesTheExoticBlendClassAndNoOther)
{
	EXPECT_EQ(kGSTileGpuClassExoticBlend, gsTileGpuProbeRefusedClasses(true));

	const u32 probe = gsTileGpuProbeRefusedClasses(true);
	for (u32 wanted = 0; wanted <= kGSTileGpuClassAll; wanted++)
	{
		for (u32 admitted = 0; admitted <= kGSTileGpuClassAll; admitted++)
		{
			const u32 shipped = gsTileGpuAdmittedClasses(wanted, admitted, 0);
			const u32 probed = gsTileGpuAdmittedClasses(wanted, admitted, probe);
			// The named class is gone...
			EXPECT_EQ(0u, probed & kGSTileGpuClassExoticBlend)
				<< "wanted " << wanted << " admitted " << admitted;
			// ...and every other class's verdict is bit-for-bit what it was.
			EXPECT_EQ(shipped & ~kGSTileGpuClassExoticBlend, probed & ~kGSTileGpuClassExoticBlend)
				<< "wanted " << wanted << " admitted " << admitted;
		}
	}
}

TEST(TileGpuDeclaringProbe, ItRefusesTheClassAndNotTheDrawsOtherReasons)
{
	const u32 probe = gsTileGpuProbeRefusedClasses(true);
	// A draw whose only reason was the exotic blend loses the read entirely.
	EXPECT_EQ(0u, gsTileGpuSelfReadUses(kGSTileGpuClassExoticBlend,
					  gsTileGpuAdmittedClasses(kGSTileGpuClassExoticBlend, kGSTileGpuClassAll, probe), kRoad));
	// A draw that also quantises its blend result keeps reading for THAT, which is the whole
	// difference between refusing a class and refusing a draw.
	const u32 both = kGSTileGpuClassExoticBlend | kGSTileGpuClassQuantisedBlend;
	EXPECT_EQ(GSDevice::kGSTileGpuSelfBlend,
		gsTileGpuSelfReadUses(both, gsTileGpuAdmittedClasses(both, kGSTileGpuClassAll, probe), kRoad));
	// ...and a DATE draw that happens to blend exotically keeps its destination-alpha test exact.
	const u32 date_too = kGSTileGpuClassExoticBlend | kGSTileGpuClassDate;
	EXPECT_EQ(GSDevice::kGSTileGpuSelfDate,
		gsTileGpuSelfReadUses(date_too, gsTileGpuAdmittedClasses(date_too, kGSTileGpuClassAll, probe), kRoad));
}

TEST(TileGpuDeclaringProbe, ItMovesNeitherLineAndTheBudgetNowRefusesStuntmanWithoutIt)
{
	// Both lines stay where the re-fit put them, and the probe still moves neither. What changed
	// underneath it is that the class it was built to isolate is refused by the arithmetic now.
	EXPECT_EQ(3800u, kGSTileGpuDeclaringRefuseAboveUs);
	EXPECT_EQ(3200u, kGSTileGpuDeclaringReadmitAtOrUnderUs);

	// Stuntman's class, from the SD865 census: 7 taxed draws behind 150 runs. Under the old
	// dimensionless score that was 247 against a line of 256 -- nine units under, admitted every
	// frame, which is the admission the probe existed to overrule. It is 14,170 us of GPU a frame,
	// and the device measured 13,783 when the probe refused it.
	EXPECT_EQ(14170u, gsTileGpuClassCostUs(/*taxed=*/7, /*runs=*/150));
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(7, 150));
	// MGS3's twin, whose refusal was measured at -9.37 ms, was twelve units the other side of the
	// old line and is four times the other side of this one.
	EXPECT_EQ(15228u, gsTileGpuClassCostUs(/*taxed=*/0, /*runs=*/162));
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(0, 162));
}

TEST(TileGpuDeclaringProbe, TheBudgetItselfNeverSeesIt)
{
	// The probe is applied downstream of the budget, so the budget keeps measuring what the class
	// WOULD cost -- the same property that stops a refused class re-admitting itself forever.
	//
	// ⚠️ Pinned on Shadow of the Colossus' exotic blends (8 runs with 44 draws behind them, 1,192 us)
	// and no longer on Stuntman's. The re-fit refuses Stuntman's class by itself, so on that shape
	// "the budget admits it, the probe refuses it" stopped being two different answers. This is a
	// class the budget really does admit.
	GSTileGpuDeclaringBudget b;
	b.Start(/*declaring_taxes_the_pass=*/true);
	for (u32 r = 0; r < 8; r++)
		b.Charge(kGSTileGpuClassExoticBlend, kGSTileGpuClassExoticBlend);
	for (u32 t = 0; t < 44; t++)
		b.Charge(kGSTileGpuClassExoticBlend, 0);
	b.Roll();
	EXPECT_EQ(1192u, b.peak[1]); // kGSTileGpuClassExoticBlend is bit 1
	EXPECT_EQ(kGSTileGpuClassExoticBlend, b.admitted & kGSTileGpuClassExoticBlend)
		<< "the budget's own verdict is ADMIT; the probe is what refuses it";
	// ...and it is the probe, applied after, that produces the arm.
	EXPECT_EQ(0u, gsTileGpuAdmittedClasses(kGSTileGpuClassExoticBlend, b.admitted,
					  gsTileGpuProbeRefusedClasses(true)));
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

/// One frame of a class in exactly the shape the per-class census reports it -- `runs` declared runs
/// and `taxed` draws standing behind them -- so a corpus row can be replayed literally.
u32 RunCensusFrame(GSTileGpuDeclaringBudget& b, u32 classes, u32 taxed, u32 runs)
{
	for (u32 r = 0; r < runs; r++)
		b.Charge(classes, classes);
	for (u32 t = 0; t < taxed; t++)
		b.Charge(classes, 0);
	b.Roll();
	return b.admitted;
}

/// ⚠️ The line is MICROSECONDS now, not a draw count, so these tests can no longer spell "a frame
/// that certainly crosses it" as `kDrawsWellPastTheLine`. A dense frame is one run plus
/// taxed draws, so it crosses at (line - run price) / taxed price + 1 taxed draws; round up and add
/// a margin.
constexpr u32 kTaxedNetUs = kGSTileGpuTaxedDrawUs - kGSTileGpuRefusedDrawUs;
constexpr u32 kRunNetUs = kGSTileGpuDeclaringPassUs - kGSTileGpuRefusedDrawUs;
constexpr u32 kDrawsPastTheLine = (kGSTileGpuDeclaringRefuseAboveUs - kRunNetUs) / kTaxedNetUs + 8;
/// ...and one four times over, for the tests that need the peak to take several frames to decay.
constexpr u32 kDrawsWellPastTheLine = kDrawsPastTheLine * 4;
/// ...and a frame that stays comfortably under the RE-ADMIT line, let alone the refuse line.
constexpr u32 kDrawsUnderTheLine = kDrawsPastTheLine / 4;
} // namespace

// -- the cost model ----------------------------------------------------------------------------

TEST(TileGpuClassCost, ADeclaredRunCostsNearlyTenTimesWhatATaxedDrawDoes)
{
	// The gap the re-fit opened. A declared run is a declaring pass and a declaring pass is measured:
	// 100 us of premium, less the 6 us the draw's refusal would itself have cost, is 94. A taxed draw
	// is priced at 16 us less the same 6, and that 10 is a fitted risk premium rather than a
	// measurement -- it is what the run counter's known undercount is worth on average.
	EXPECT_EQ(1000u, gsTileGpuClassCostUs(/*taxed=*/100, /*runs=*/0));
	EXPECT_EQ(9400u, gsTileGpuClassCostUs(/*taxed=*/0, /*runs=*/100));
	EXPECT_LT(gsTileGpuClassCostUs(100, 0), gsTileGpuClassCostUs(0, 100));
	// ...and the ratio, spelled out, because it is the number a re-tune will want first.
	EXPECT_EQ(94u, gsTileGpuClassCostUs(0, 1));
	EXPECT_EQ(10u, gsTileGpuClassCostUs(1, 0));
}

TEST(TileGpuClassCost, ItReproducesTheThreeDeltasTheDeviceMeasured)
{
	// ★ The whole justification for the arithmetic, as an assertion. Arm D1 (SD865, suite
	// 20260831-0621) force-refused one admission class on three dumps and measured the GPU each time.
	// The score is what it says would be given back, in microseconds a frame, and it lands inside 8%
	// on all three -- with no per-title term anywhere in it.
	struct Arm
	{
		const char* what;
		u32 taxed, runs, measured_us;
	};
	// stuntman / outrun-a / outrun-b: the census shapes, against the measured GPU deltas.
	for (const Arm& a : {Arm{"stuntman exotic", 7, 150, 13783}, Arm{"outrun-a exotic", 17, 96, 9614},
			 Arm{"outrun-b exotic", 11, 88, 7787}})
	{
		const u32 score = gsTileGpuClassCostUs(a.taxed, a.runs);
		const double err = (static_cast<double>(score) - a.measured_us) / a.measured_us;
		EXPECT_LT(-0.08, err) << a.what << " scored " << score << " against " << a.measured_us;
		EXPECT_GT(0.08, err) << a.what << " scored " << score << " against " << a.measured_us;
		EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, score) << a.what << " must be refused";
	}
}

TEST(TileGpuClassCost, TheRunCounterCanOnlyEVERUNDERSTATE)
{
	// ⚠️ The caveat that has now outlived two cost models. The run counter is the number this planner
	// cannot measure: it breaks a run when the attachment group changes, and cannot see the pass
	// breaks a prep op or a texture bind makes later in the same draw. On Ratchet & Clank's effects
	// it counts SEVEN runs where the device declared 357 passes.
	//
	// Its DIRECTION is what makes it survivable: undercounting runs UNDERPRICES a class, never the
	// reverse, so the failure mode is admitting something that should have been refused. And on the
	// one class where the undercount is known to be enormous, the taxed term alone still refuses it
	// -- which is the entire job the taxed term's fitted 10 us is doing.
	EXPECT_LT(gsTileGpuClassCostUs(357, 7), gsTileGpuClassCostUs(7, 357));
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(357, 7));
	// ...and the taxed draws are 3,570 of that 4,228. The seven runs the counter CAN see score 658
	// on their own, nowhere near the line: strike the taxed term out and this class is admitted, on a
	// title where the device declared 357 passes for it.
	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(0, 7));
	EXPECT_EQ(3570u, gsTileGpuClassCostUs(357, 0));
}

TEST(TileGpuClassCost, TheRunTermIsNeverForgivenAndNeitherIsTheTaxedOne)
{
	// Xenosaga's alpha-MSB FBMSK, the class the whole budget was built for: 2,892 draws standing
	// behind 1,912 declared runs. Both halves of the score refuse it on their own, and the sum is
	// what is actually compared -- a re-tune that quietly drops either term still has to explain
	// this row.
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(2892, 1912));
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(0, 1912)) << "the run term alone";
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(2892, 0)) << "the taxed term alone";
	EXPECT_LT(gsTileGpuClassCostUs(0, 1912), gsTileGpuClassCostUs(2892, 1912)) << "the taxed term is intact";
	EXPECT_EQ(208648u, gsTileGpuClassCostUs(2892, 1912));
}

TEST(TileGpuDeclaringBudget, TheBudgetSitsWhereTheCorpusSeparates)
{
	// The calibration, pinned where a re-tune will see it. Per-class (taxed, runs) over the 22-dump
	// corpus, taken on the M2 -- which computes the SD865's numbers exactly, because a class is
	// charged against its ATTACHMENT GROUP and never against passes -- with the device's own verdict
	// beside every class that has one, and the score in microseconds of GPU a frame.
	//
	//   must refuse   Xenosaga's FBMSK    (2892, 1912)  208,648     the toll the budget was built for
	//                 Stuntman's DATE     (   0, 1340)  125,960
	//                 FlatOut 2's exotic  (   0,  896)   84,224     +66.5 ms when it was admitted
	//                 Spider-Man 3 FBMSK  (2989,  147)   43,708
	//                 Spider-Man 3 exotic (  60,  390)   37,260
	//                 MGS3's exotic       (   0,  162)   15,228      -9.37 ms measured
	//                 Stuntman's exotic   (   7,  150)   14,170     -13.78 ms measured
	//                 OutRun exotic (a)   (  17,   96)    9,194      -9.61 ms measured
	//                 OutRun exotic (b)   (  11,   88)    8,382      -7.79 ms measured
	//                 Baldur's Gate 16bit ( 476,    2)    4,948
	//                 Dirge's exotic      (   5,   47)    4,468
	//                 Ratchet's effects   ( 357,    7)    4,228     <- binds the taxed price
	//                 Xenosaga's exotic   (   9,   43)    4,132
	//   must admit    Ace Combat 5 exotic (  11,   34)    3,306     <- inside the hysteresis band
	//                 Katamari's glyphs   ( 202,    9)    2,866     <- binds the taxed price
	//                 GT4's exotic        (   5,   26)    2,494
	//                 God of War 2 FBMSK  (   8,   18)    1,772
	//                 Yu-Gi-Oh's 16-bit   (  30,   16)    1,804
	//                 Beyond G&E's FBMSK  (  51,   11)    1,544
	//                 SotC's exotic       (  44,    8)    1,192
	//                 GT4 Online Beta     (  43,    2)      468
	//
	// ⚠️ Four of these rows changed side in the 2026-08-31 re-fit and the device is the reason:
	// Stuntman's, OutRun's two, and MGS3's were the classes arm D1 refused and measured. Dirge's and
	// Xenosaga's exotic blends changed side with them, on the arithmetic rather than on a
	// measurement of their own.
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(2892, 1912)); // Xenosaga FBMSK
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(0, 1340));    // Stuntman DATE
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(0, 896));     // FlatOut 2
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(2989, 147));  // Spider-Man 3
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(0, 162));     // MGS3
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(7, 150));     // Stuntman exotic
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(17, 96));     // OutRun a
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(11, 88));     // OutRun b
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(476, 2));     // Baldur's Gate
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(5, 47));      // Dirge
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(357, 7));     // Ratchet
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(9, 43));      // Xenosaga exotic

	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(11, 34));     // Ace Combat 5
	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(202, 9));     // Katamari
	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(5, 26));      // GT4
	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(8, 18));      // God of War 2
	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(30, 16));     // Yu-Gi-Oh
	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(51, 11));     // Beyond Good & Evil
	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(44, 8));      // Shadow of the Colossus
	EXPECT_GE(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(43, 2));      // GT4 Online Beta
}

TEST(TileGpuDeclaringBudget, RatchetAndKatamariAreWhatFixTheTaxedPrice)
{
	// ⚠️ Why the taxed draw is 16 us and not something rounder. It is not a measurement -- the two
	// corpus anchors disagree about what a taxed draw costs by a factor of a hundred, and both
	// readings are real:
	//
	//   Ratchet's effects   357 taxed behind 7 runs, and the device declared 357 passes. Nearly
	//                       every taxed draw was a declaring pass of its own.
	//   Katamari's glyphs   202 taxed behind 9 runs, and the whole title declares 14-20 passes a
	//                       frame. Nearly none of them was.
	//
	// So the price is set by the ORDER the two have to come in, not by either reading: Ratchet
	// refused, Katamari not merely admitted but able to come BACK after a transient spike, which
	// means under the RE-ADMIT line and not just under the refuse line. With x the taxed price net of
	// the 6 us a refusal costs, 658 + 357x > 3800 needs x > 8.80 and 846 + 202x <= 3200 needs
	// x <= 11.65. 16 - 6 = 10 sits near the middle.
	EXPECT_LT(kGSTileGpuDeclaringRefuseAboveUs, gsTileGpuClassCostUs(357, 7));
	EXPECT_GE(kGSTileGpuDeclaringReadmitAtOrUnderUs, gsTileGpuClassCostUs(202, 9));
	EXPECT_LT(kGSTileGpuDeclaringReadmitAtOrUnderUs, kGSTileGpuDeclaringRefuseAboveUs);
	// ...and the window really is a window: one unit either side of it breaks one of the two.
	EXPECT_LT(8.80, static_cast<double>(kGSTileGpuTaxedDrawUs - kGSTileGpuRefusedDrawUs));
	EXPECT_GE(11.65, static_cast<double>(kGSTileGpuTaxedDrawUs - kGSTileGpuRefusedDrawUs));
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
	// ★ The headline: Katamari's punctuation. Its FBMSK class is 202 draws standing behind 9 declared
	// runs, 2,866 us a frame against a 3,800 line, so the bootstrap never trips and frame one writes
	// the glyphs exactly. Under the startup polarity this frame was approximated and the damage was
	// permanent, because the glyph store is written once.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (u32 r = 0; r < 9; r++)
	{
		b.Charge(kGSTileGpuClassPartialMask, kGSTileGpuClassPartialMask);
		ASSERT_EQ(kGSTileGpuClassPartialMask, b.admitted & kGSTileGpuClassPartialMask) << "run " << r;
	}
	for (u32 d = 0; d < 202; d++)
	{
		b.Charge(kGSTileGpuClassPartialMask, 0);
		ASSERT_EQ(kGSTileGpuClassPartialMask, b.admitted & kGSTileGpuClassPartialMask) << "draw " << d;
	}
	b.Roll();
	EXPECT_EQ(kGSTileGpuClassPartialMask, b.admitted & kGSTileGpuClassPartialMask);
	EXPECT_EQ(2866u, b.peak[3]) << "kGSTileGpuClassPartialMask is bit 3";
}

TEST(TileGpuDeclaringBudget, AnUnpricedClassIsRefusedTheMomentItCrossesTheLine)
{
	// The other side of the same bet: exposure is bounded by the LINE, not by the frame. Xenosaga's
	// FBMSK is 4805 draws a frame; a whole admitted frame of it measured 303 ms on the device and
	// took a kernel-level GPU fault. In this shape -- one run with everything else behind it -- the
	// line falls after 94 us of run plus 371 taxed draws at 10 us, so 372 draws get through.
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
	const u32 taxed_to_the_line = (kGSTileGpuDeclaringRefuseAboveUs - kRunNetUs) / kTaxedNetUs + 1;
	EXPECT_EQ(taxed_to_the_line + 1, admitted_draws);
	EXPECT_EQ(372u, admitted_draws);
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
	for (u32 d = 0; d < kDrawsWellPastTheLine; d++)
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
	RunDenseFrame(b, kGSTileGpuClassPartialMask, kDrawsWellPastTheLine);
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
	for (u32 d = 0; d < kDrawsWellPastTheLine; d++)
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
	EXPECT_EQ(0u, RunDenseFrame(b, kGSTileGpuClassExoticBlend, kDrawsWellPastTheLine) &
					  kGSTileGpuClassExoticBlend);
	EXPECT_EQ(0u, RunDenseFrame(b, kGSTileGpuClassExoticBlend, kDrawsWellPastTheLine) &
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
		EXPECT_EQ(0u, RunDenseFrame(b, kGSTileGpuClassPartialMask, kDrawsWellPastTheLine) &
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
			RunDenseFrame(b, kGSTileGpuClassPartialMask, kDrawsWellPastTheLine);
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
	RunDenseFrame(b, kGSTileGpuClassPartialMask, kDrawsWellPastTheLine);
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
	// The reason the re-admit line is 3,200 and not a round fraction of the other one. Katamari's
	// punctuation costs 2,866 us every frame. One bad frame refuses it; if the re-admit line sat
	// below 2,866 -- as the obvious 25% band's 2,850 does -- the peak would decay to 2,866, stop
	// there, and the punctuation would be off for good.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	RunDenseFrame(b, kGSTileGpuClassPartialMask, kDrawsWellPastTheLine); // the spike
	ASSERT_EQ(0u, b.admitted & kGSTileGpuClassPartialMask);
	for (int frame = 0; frame < 40 && (b.admitted & kGSTileGpuClassPartialMask) == 0; frame++)
		RunCensusFrame(b, kGSTileGpuClassPartialMask, /*taxed=*/202, /*runs=*/9); // an ordinary frame
	EXPECT_EQ(kGSTileGpuClassPartialMask, b.admitted & kGSTileGpuClassPartialMask)
		<< "a class that must be admitted was latched off by one bad frame";
	EXPECT_GE(kGSTileGpuDeclaringReadmitAtOrUnderUs, b.peak[3])
		<< "the peak the re-admission was made on";
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
	EXPECT_LT(gsTileGpuClassCostUs(dense.taxed[1], dense.runs[1]),
		gsTileGpuClassCostUs(spread.taxed[1], spread.runs[1]));
}

TEST(TileGpuDeclaringBudget, TheCountersResetWithTheFrame)
{
	// Two lean frames in a row must not add up to one fat one.
	GSTileGpuDeclaringBudget b;
	b.Start(kDeclaringIsTaxed);
	for (int frame = 0; frame < 8; frame++)
	{
		EXPECT_EQ(kGSTileGpuClassQuantisedBlend,
			RunDenseFrame(b, kGSTileGpuClassQuantisedBlend, kDrawsUnderTheLine) &
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
	// The spec half lives entirely in bits 14-31 and nothing below 14 moves. The word is FULL now:
	// the seventh texel arm spent the bit that used to sit free at 31, so the next axis needs a wider
	// key rather than another shuffle.
	for (const u32 key : keys)
		ASSERT_EQ(key & ~(0x3FFFu | Plan::kVariantSpecMask), 0u);
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

TEST(TileGpuVariantKey, TheDriverCanTakeAnAxisBackOutOfTheFrozenSet)
{
	// One axis is not frozen everywhere. Honeykrisp moves six pixels of one corpus frame when TEXA
	// is a compile-time constant -- not because the constant is wrong (it provably equals the state
	// row's) but because the program shape changes, which is the same driver and the same class as
	// the dynamic byte-extract miscompile TILEGPU_STATIC_BYTE_SEL already works around. So the
	// device may hand an axis back, and the rule is pinned here rather than only inside the Vulkan
	// backend, because three things have to agree about it: the #define block, the fragment module
	// cache key and the pipeline cache key.
	using Spec = GSDevice::GSTileGpuFragmentSpec;
	Spec frozen;
	frozen.valid = true;
	frozen.texa = 3;

	Spec keep = frozen;
	keep.NarrowToDriver(/*freeze_texa=*/true);
	EXPECT_TRUE(keep.texa_frozen);
	EXPECT_EQ(keep.texa, 3);
	EXPECT_EQ(keep, frozen);

	Spec give_back = frozen;
	give_back.NarrowToDriver(/*freeze_texa=*/false);
	EXPECT_FALSE(give_back.texa_frozen);
	EXPECT_EQ(give_back.texa, 0) << "the key field has to be zero, or two specs that compile the "
									"identical program become two modules";
	EXPECT_NE(give_back, frozen);
	// ...and nothing else moves. The other nine axes are the driver's business only through this
	// one call, and it must not touch them.
	Spec expect = frozen;
	expect.texa = 0;
	expect.texa_frozen = false;
	EXPECT_EQ(give_back, expect);

	// The defines follow the flag, not the value: -1 is the shader's "read the state row".
	EXPECT_NE(GSTileGpuShaderVariant::SpecDefines(keep).find("#define TILEGPU_SPEC_TEXA 3\n"),
		std::string::npos);
	EXPECT_NE(GSTileGpuShaderVariant::SpecDefines(give_back).find("#define TILEGPU_SPEC_TEXA -1\n"),
		std::string::npos);
	// A frozen ZERO and a given-back axis are different programs and must not spell the same define.
	Spec frozen_zero = frozen;
	frozen_zero.texa = 0;
	EXPECT_NE(GSTileGpuShaderVariant::SpecDefines(frozen_zero).find("#define TILEGPU_SPEC_TEXA 0\n"),
		std::string::npos);

	// The two narrowings compose in either order, which is what lets the executor apply the road's
	// and the driver's without caring which came first.
	for (const u32 road : {0u, GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuRoadSource,
			 GSDevice::kGSTileGpuRoadByte | GSDevice::kGSTileGpuRoadTarget})
	{
		for (const bool freeze : {false, true})
		{
			Spec a = frozen, b = frozen;
			a.NarrowToRoad(road);
			a.NarrowToDriver(freeze);
			b.NarrowToDriver(freeze);
			b.NarrowToRoad(road);
			EXPECT_EQ(a, b) << "road " << road << " freeze_texa " << freeze;
		}
	}

	// An unspecialized spec is untouched by either narrowing -- the pass-union fallback reads every
	// field from the row on every driver.
	Spec none;
	none.NarrowToRoad(GSDevice::kGSTileGpuRoadByte);
	none.NarrowToDriver(false);
	EXPECT_FALSE(none.valid);
	EXPECT_TRUE(GSTileGpuShaderVariant::SpecDefines(none).empty());
}
