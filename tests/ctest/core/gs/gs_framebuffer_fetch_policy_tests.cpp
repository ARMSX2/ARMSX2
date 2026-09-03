// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the framebuffer-fetch decisions (GS/Renderers/Common/GSFramebufferFetchPolicy.h): the
// OpenGL one first, the Vulkan one at the bottom of the file.
//
// This suite exists because of a specific bug, not for coverage. GSDeviceOGL::CheckFeatures used
// to decide framebuffer fetch three separate times across ~100 lines: an extension test, a
// driver-version guard plus the DisableFramebufferFetch setting, and finally a Mali-profile block
// that set the flag back to true off the raw ARM extension. On a Mali r44p1 handheld the log
// printed "Mali r44p1: disabling framebuffer fetch" and "Active framebuffer fetch backend (Mali
// profile): ARM" a tenth of a millisecond apart, and there was no way to turn fetch off on Mali GL
// from settings at all -- which is why the artifact it caused had to be isolated on desktop.
//
// So the tests that matter here are the invariants at the bottom, swept over every input
// combination: "enabled" must imply nothing vetoed it, and the profile demotion must not depend on
// the veto inputs at all. A later block re-deciding fetch fails those regardless of which knob it
// reaches for.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSFramebufferFetchPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// Named so the call sites below read as prose rather than as six anonymous bools.
	struct Caps
	{
		bool arm = false;
		bool ext = false;
		bool pls = false;
		bool blocklisted = false;
		bool user_disabled = false;
		bool mali_profile = false;
	};

	GSFramebufferFetchDecision Decide(const Caps& c)
	{
		return DecideGLFramebufferFetch(c.arm, c.ext, c.pls, c.blocklisted, c.user_disabled, c.mali_profile);
	}
} // namespace

// The regression. A Mali device on a blocklisted driver build advertises ARM fetch -- that is
// exactly why the old code turned it back on.
TEST(GSFramebufferFetchPolicy, BlocklistedDriverSurvivesTheMaliProfile)
{
	const GSFramebufferFetchDecision d =
		Decide({.arm = true, .ext = true, .pls = true, .blocklisted = true, .mali_profile = true});
	EXPECT_FALSE(d.enabled);
	EXPECT_EQ(d.veto, GSFramebufferFetchVeto::DriverBlocklist);
	EXPECT_EQ(d.backend, GSFramebufferFetchBackend::None);
	// Losing fetch must not move the device off the Mali profile: that would swap in PowerVR's
	// texture and shader tuning as a side effect of a correctness gate.
	EXPECT_FALSE(d.demote_mali_to_powervr);
}

TEST(GSFramebufferFetchPolicy, UserSettingSurvivesTheMaliProfile)
{
	const GSFramebufferFetchDecision d =
		Decide({.arm = true, .ext = true, .pls = true, .user_disabled = true, .mali_profile = true});
	EXPECT_FALSE(d.enabled);
	EXPECT_EQ(d.veto, GSFramebufferFetchVeto::UserSetting);
	EXPECT_EQ(d.backend, GSFramebufferFetchBackend::None);
	EXPECT_FALSE(d.demote_mali_to_powervr);
}

// A hardware fact outranks a user preference in the reported reason: if the driver is blocklisted,
// the setting is not why fetch is off, and saying so sends the user hunting for a toggle that
// would not have helped.
TEST(GSFramebufferFetchPolicy, DriverBlocklistOutranksUserSettingInTheReason)
{
	const GSFramebufferFetchDecision d = Decide(
		{.arm = true, .ext = true, .blocklisted = true, .user_disabled = true, .mali_profile = true});
	EXPECT_FALSE(d.enabled);
	EXPECT_EQ(d.veto, GSFramebufferFetchVeto::DriverBlocklist);
}

TEST(GSFramebufferFetchPolicy, MaliWithArmFetchKeepsTheArmBackend)
{
	// Mali reads back through gl_LastFragColorARM even when EXT is also advertised -- the EXT
	// inout path is broken on every Mali driver tested. Mirrors `#if GPU_PROFILE_MALI` in
	// tfx_fs.glsl.
	const GSFramebufferFetchDecision d = Decide({.arm = true, .ext = true, .pls = true, .mali_profile = true});
	EXPECT_TRUE(d.enabled);
	EXPECT_EQ(d.veto, GSFramebufferFetchVeto::None);
	EXPECT_EQ(d.backend, GSFramebufferFetchBackend::ARM);
	EXPECT_FALSE(d.demote_mali_to_powervr);
}

TEST(GSFramebufferFetchPolicy, MaliWithoutArmFetchDemotesToPowerVRAndTakesTheExtPath)
{
	// Reachable when a device is force-overridden to the Mali profile but has no ARM fetch.
	const GSFramebufferFetchDecision d = Decide({.ext = true, .mali_profile = true});
	EXPECT_TRUE(d.demote_mali_to_powervr);
	EXPECT_TRUE(d.enabled);
	// After demotion the effective profile is no longer Mali, so the shader takes its EXT arm.
	EXPECT_EQ(d.backend, GSFramebufferFetchBackend::EXT);
}

TEST(GSFramebufferFetchPolicy, NonMaliProfilesPreferExtAndFallBackToArm)
{
	EXPECT_EQ(Decide({.ext = true}).backend, GSFramebufferFetchBackend::EXT);
	// Pixel local storage counts as the EXT arm in the shader even without EXT fetch itself.
	EXPECT_EQ(Decide({.arm = true, .pls = true}).backend, GSFramebufferFetchBackend::EXT);
	// `#elif HAS_ARM_SHADER_FRAMEBUFFER_FETCH` in tfx_fs.glsl: a non-Mali profile with only the
	// ARM builtin still has a working read, so fetch stays on rather than falling off the fast path.
	EXPECT_EQ(Decide({.arm = true}).backend, GSFramebufferFetchBackend::ARM);
	EXPECT_TRUE(Decide({.arm = true}).enabled);
}

TEST(GSFramebufferFetchPolicy, NoExtensionMeansNoFetchAndNoBackend)
{
	const GSFramebufferFetchDecision d = Decide({});
	EXPECT_FALSE(d.enabled);
	EXPECT_EQ(d.veto, GSFramebufferFetchVeto::NoExtension);
	EXPECT_EQ(d.backend, GSFramebufferFetchBackend::None);
	EXPECT_FALSE(d.demote_mali_to_powervr);
	// Desktop GL: no fetch extensions, and PLS alone is not enough to run the path.
	EXPECT_FALSE(Decide({.pls = true}).enabled);
}

// The generic statements of the bug, swept over all 64 input combinations. Any future block that
// re-decides fetch from something other than this function fails these no matter which input it
// reaches for.
TEST(GSFramebufferFetchPolicy, EnabledImpliesNothingVetoedIt)
{
	for (int bits = 0; bits < 64; bits++)
	{
		const Caps c{.arm = (bits & 1) != 0, .ext = (bits & 2) != 0, .pls = (bits & 4) != 0,
			.blocklisted = (bits & 8) != 0, .user_disabled = (bits & 16) != 0,
			.mali_profile = (bits & 32) != 0};
		const GSFramebufferFetchDecision d = Decide(c);
		SCOPED_TRACE(testing::Message() << "bits=" << bits);

		if (d.enabled)
		{
			EXPECT_TRUE(c.arm || c.ext);
			EXPECT_FALSE(c.blocklisted);
			EXPECT_FALSE(c.user_disabled);
			EXPECT_EQ(d.veto, GSFramebufferFetchVeto::None);
			EXPECT_NE(d.backend, GSFramebufferFetchBackend::None);
		}
		else
		{
			EXPECT_NE(d.veto, GSFramebufferFetchVeto::None);
			EXPECT_EQ(d.backend, GSFramebufferFetchBackend::None);
		}
	}
}

TEST(GSFramebufferFetchPolicy, ProfileDemotionIgnoresTheVetoInputs)
{
	// Whether fetch is switched off is a blend-path choice; which profile the device is on is a
	// hardware fact. Turning one off must never change the other, so demotion has to be a function
	// of the extension set and the profile alone.
	for (int caps = 0; caps < 8; caps++)
	{
		const bool arm = (caps & 1) != 0;
		const bool ext = (caps & 2) != 0;
		const bool pls = (caps & 4) != 0;
		for (bool mali : {false, true})
		{
			const bool expected = mali && !arm;
			for (bool blocklisted : {false, true})
			{
				for (bool user_disabled : {false, true})
				{
					SCOPED_TRACE(testing::Message() << "caps=" << caps << " mali=" << mali
													<< " blocklisted=" << blocklisted
													<< " user_disabled=" << user_disabled);
					EXPECT_EQ(DecideGLFramebufferFetch(arm, ext, pls, blocklisted, user_disabled, mali)
								  .demote_mali_to_powervr,
						expected);
				}
			}
		}
	}
}

// The barrier half of the same policy (FbFetchDropsDrawBarriers).
//
// Same class of bug as the one above, in the opposite direction: not a decision re-made in three
// places, but one decision standing in for two different questions. "Can I read the destination
// without a barrier?" and "are overlapping primitives ordered against each other?" were both
// answered by features.framebuffer_fetch, and only the first of them is what fetch actually
// guarantees on GL. The blending path asks for a full barrier so overlapping primitives observe
// each other; DetermineBarriers then deleted it because fetch was available. Measured on the GL
// arm, that cost 101x the per-pixel error of the RT-copy path and made the frame nondeterministic.

TEST(GSFramebufferFetchPolicy, GLFetchKeepsTheBarrierWhenPrimitivesOverlap)
{
	// The regression. GL's fetch orders nothing, so an overlapping draw must keep its barrier --
	// the software blend path enabled for it reads a destination its own predecessor writes.
	EXPECT_FALSE(FbFetchDropsDrawBarriers(false, true, false));
}

TEST(GSFramebufferFetchPolicy, NonOverlappingDrawsDropTheBarrierOnEveryBackend)
{
	// With no overlap, a live in-tile read and a pre-draw snapshot are the same value, so the
	// barrier buys nothing. This is the common case and the reason the fix is nearly free.
	EXPECT_TRUE(FbFetchDropsDrawBarriers(false, false, false));
	EXPECT_TRUE(FbFetchDropsDrawBarriers(true, false, false));
}

TEST(GSFramebufferFetchPolicy, OrderingBackendsKeepTheBarrierFreeFastPath)
{
	// Vulkan's rasterization-order attachment access and Metal's programmable blending order
	// overlapping fragments by contract. Making them pay for a barrier would reintroduce exactly
	// the render-pass breaks the fetch path exists to remove, for no correctness gain.
	EXPECT_TRUE(FbFetchDropsDrawBarriers(true, true, false));
}

TEST(GSFramebufferFetchPolicy, DepthFeedbackBarriersSurviveEveryFetchCapability)
{
	// Depth feedback reads through a texture rather than the colour attachment, so no colour-fetch
	// capability says anything about it.
	for (bool orders : {false, true})
	{
		for (bool overlap : {false, true})
		{
			SCOPED_TRACE(testing::Message() << "orders=" << orders << " overlap=" << overlap);
			EXPECT_FALSE(FbFetchDropsDrawBarriers(orders, overlap, true));
		}
	}
}

TEST(GSFramebufferFetchPolicy, DroppingBarriersNeverDependsOnOverlapAloneWithoutAnOrderingGuarantee)
{
	// The invariant that fails if someone reinstates the unconditional drop: without an ordering
	// guarantee, the answer must track overlap exactly.
	for (bool overlap : {false, true})
	{
		SCOPED_TRACE(testing::Message() << "overlap=" << overlap);
		EXPECT_EQ(FbFetchDropsDrawBarriers(false, overlap, false), !overlap);
	}
}

// Which GL fetch extension orders overlapping primitives (FbFetchOrdersOverlappingPrims).
//
// The regression these pin: the ordering question was answered "no" for the whole GL API, on a
// measurement taken through the EXT extension on one desktop driver. ARM's extension guarantees
// the opposite in its spec, and every Mali device on Android takes the ARM path, so the blanket
// answer put the entire Mali install base on a split draw -- one draw call per primitive group --
// for every overlapping blended draw. That is the 2.6.6.5 slowdown.

TEST(GSFramebufferFetchPolicy, ArmFetchOrdersOverlappingPrimitives)
{
	// ARM_shader_framebuffer_fetch spec: "when an individual sample is covered by multiple
	// primitives, rendering for that sample is performed sequentially in the order in which the
	// primitives were submitted", and a gl_LastFragColorARM read "must wait for the processing of
	// all previous fragments destined for the current pixel to complete". That is the same
	// contract Vulkan and Metal provide, so it earns the same barrier-free path.
	EXPECT_TRUE(FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::ARM));
	EXPECT_TRUE(FbFetchDropsDrawBarriers(
		FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::ARM), /*prims_may_overlap=*/true, false));
}

TEST(GSFramebufferFetchPolicy, ExtFetchDoesNotOrderOverlappingPrimitives)
{
	// The EXT path is where the reordering was actually measured (Mesa 25.3.6 / Apple M2: 18% of an
	// MGS3 frame changing between identical replays). It keeps its barrier.
	EXPECT_FALSE(FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::EXT));
	EXPECT_FALSE(FbFetchDropsDrawBarriers(
		FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::EXT), /*prims_may_overlap=*/true, false));
}

TEST(GSFramebufferFetchPolicy, AVetoedFetchNeverClaimsAnOrderingGuarantee)
{
	// A device that lost fetch has no in-tile read at all, so it cannot be claiming to order
	// anything with one. Swept over the policy's own vetoes so a new one cannot miss this: the
	// r44p1 blocklist is exactly a driver that violates the ARM guarantee, and the database entry
	// is where that belongs -- not a blanket rule that also penalises every healthy Mali.
	for (bool blocklisted : {false, true})
	{
		for (bool user_disabled : {false, true})
		{
			const GSFramebufferFetchDecision d = Decide({.arm = true, .ext = true,
				.blocklisted = blocklisted, .user_disabled = user_disabled, .mali_profile = true});
			SCOPED_TRACE(testing::Message() << "blocklisted=" << blocklisted
											<< " user_disabled=" << user_disabled);

			EXPECT_EQ(FbFetchOrdersOverlappingPrims(d.backend), d.enabled);
		}
	}
}

// The blend-fallback shape (GLUsesPerPrimitiveFbCopy).
//
// Third face of the same coupling. Turning fetch off answers "how do we read the destination?" but
// not "how often do we copy it", and the two were decided ~100 lines apart: the copy flag is set
// early, from the absence of a texture-barrier extension, and the fetch veto that makes it
// load-bearing happens much later. A blocklisted Mali device therefore came out with no fetch, no
// barrier, and a render-target copy per primitive group -- 0.33 fps in MGS3 on the RG 477V, against
// ~30 fps for the same game on Vulkan, which reaches the same copy-based concept per draw instead.

TEST(GSFramebufferFetchPolicy, TilerWithoutABarrierTakesThePerDrawCopy)
{
	// The regression. This is the blocklisted-Mali configuration: no fetch, so no barrier, on a
	// tile-based GPU where reading the render target back flushes and resolves the tile.
	EXPECT_FALSE(GLUsesPerPrimitiveFbCopy(false, true));
}

TEST(GSFramebufferFetchPolicy, ImmediateModeGPUKeepsThePerPrimitiveCopy)
{
	// Desktop GL only reaches the no-barrier case on pre-4.5 hardware without ARB or NV texture
	// barrier. A blit there is just a blit, and the per-primitive copy buys real blend ordering, so
	// nothing about this fix applies to it.
	EXPECT_TRUE(GLUsesPerPrimitiveFbCopy(false, false));
}

TEST(GSFramebufferFetchPolicy, ABarrierMakesTheCopyFlagInertOnEveryGPU)
{
	// Every consumer of the flag is guarded by !texture_barrier, so its value cannot matter here.
	// Pinned because a set-but-unreachable flag reads as "this device copies per primitive" to
	// anyone auditing the log or the feature dump, which is how the wrong path gets blamed.
	for (bool tiler : {false, true})
	{
		SCOPED_TRACE(testing::Message() << "tiler=" << tiler);
		EXPECT_FALSE(GLUsesPerPrimitiveFbCopy(true, tiler));
	}
}

TEST(GSFramebufferFetchPolicy, NoVetoedFetchLeavesATilerOnThePerPrimitiveCopy)
{
	// The invariant that ties the two halves together: whichever way fetch is lost -- the driver
	// blocklist, the user's setting, or no extension at all -- a tiler must not be left copying per
	// primitive. Swept over the fetch policy's own inputs so a new veto cannot miss this.
	for (bool arm : {false, true})
	{
		for (bool blocklisted : {false, true})
		{
			for (bool user_disabled : {false, true})
			{
				const GSFramebufferFetchDecision d = Decide({.arm = arm,
					.blocklisted = blocklisted, .user_disabled = user_disabled, .mali_profile = true});
				SCOPED_TRACE(testing::Message() << "arm=" << arm << " blocklisted=" << blocklisted
												<< " user_disabled=" << user_disabled);

				// GLES: the texture barrier is fetch and nothing else.
				EXPECT_FALSE(GLUsesPerPrimitiveFbCopy(d.enabled, /*tile_based_gpu=*/true));
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// The Vulkan decision, in the two shapes the device round cares about.
//
// Both are real device configurations rather than input sweeps: the sweeps live as static_asserts
// beside the function, and these say what the two machines in the suite must do.
namespace
{
	// Anbernic RG 477V at its default settings, after the driver database exempted its SoC from
	// the two deny rules: Mali, ROAA present, nothing forced, nothing disabled.
	constexpr GSVulkanFramebufferFetchInputs Rg477vAtDefaultIni()
	{
		GSVulkanFramebufferFetchInputs in;
		in.roaa_available = true;
		in.is_mali = true;
		return in;
	}

	// SD865 / Adreno 650 on Turnip. The database denies its destination read (ARMSX2 #442,
	// vk-turnip-attachment-self-read), which is a crash-class correctness gate, not a perf trade.
	constexpr GSVulkanFramebufferFetchInputs Sd865OnTurnip()
	{
		GSVulkanFramebufferFetchInputs in;
		in.roaa_available = true;
		in.is_adreno = true;
		in.broken_destination_read = true;
		// The Android build ships this key on, so the SD865 arrives here with it set. It is not
		// what decides the answer, and pinning it set is the point.
		in.adreno_fetch_key = true;
		return in;
	}
} // namespace

// Polarity one: the RG 477V takes the in-tile read at DEFAULT settings. That is what step 2.2 is
// for -- the two INI keys the measured arm needed become the shipped behaviour on this SoC.
TEST(GSVulkanFramebufferFetchPolicy, TheMeasuredMaliPartTakesTheInTileReadAtDefaultSettings)
{
	EXPECT_TRUE(DecideVulkanFramebufferFetch(Rg477vAtDefaultIni()).enabled);
	EXPECT_FALSE(DecideVulkanFramebufferFetch(Rg477vAtDefaultIni()).force_key_ignored);
}

// Polarity two, and the reason the force key had to be gated: setting it on the SD865 must not
// move it. Before the gate this exact input turned Adreno's deny off and put it on the road
// ARMSX2 #442 established it cannot take, with nothing in the key's name saying so.
TEST(GSVulkanFramebufferFetchPolicy, TheForceKeyCannotPutAdrenoOnTheInTileRead)
{
	GSVulkanFramebufferFetchInputs forced = Sd865OnTurnip();
	forced.force_mali_fetch_key = true;

	EXPECT_FALSE(DecideVulkanFramebufferFetch(Sd865OnTurnip()).enabled);
	EXPECT_FALSE(DecideVulkanFramebufferFetch(forced).enabled);
	EXPECT_TRUE(DecideVulkanFramebufferFetch(forced).force_key_ignored);
}

// The key keeps its meaning where it was written to have one: a MediaTek part the database still
// denies is exactly who it is for.
TEST(GSVulkanFramebufferFetchPolicy, TheForceKeyStillLiftsTheDenyOnMali)
{
	GSVulkanFramebufferFetchInputs denied = Rg477vAtDefaultIni();
	denied.broken_destination_read = true;

	EXPECT_FALSE(DecideVulkanFramebufferFetch(denied).enabled);

	denied.force_mali_fetch_key = true;
	EXPECT_TRUE(DecideVulkanFramebufferFetch(denied).enabled);
	EXPECT_FALSE(DecideVulkanFramebufferFetch(denied).force_key_ignored);
}

// DisableFramebufferFetch is the user's way back, from every state including a forced one, on
// every vendor. Swept rather than spot-checked because it is the one guarantee the settings UI
// makes about this path.
TEST(GSVulkanFramebufferFetchPolicy, TheUserCanAlwaysTurnFetchOff)
{
	for (bool mali : {false, true})
	{
		for (bool adreno : {false, true})
		{
			for (bool forced : {false, true})
			{
				for (bool denied : {false, true})
				{
					GSVulkanFramebufferFetchInputs in;
					in.roaa_available = true;
					in.user_disabled = true;
					in.is_mali = mali;
					in.is_adreno = adreno;
					in.force_mali_fetch_key = forced;
					in.broken_destination_read = denied;
					in.adreno_fetch_key = true;
					SCOPED_TRACE(testing::Message() << "mali=" << mali << " adreno=" << adreno
													<< " forced=" << forced << " denied=" << denied);

					EXPECT_FALSE(DecideVulkanFramebufferFetch(in).enabled);
				}
			}
		}
	}
}

// The invariant behind the gate, over every input combination: the force key may only ever change
// the answer on Mali. Anywhere else, flipping it must leave the decision exactly as it was -- which
// is a stronger statement than "Adreno is not forced", and the one a future term in this function
// could break without any test above noticing.
TEST(GSVulkanFramebufferFetchPolicy, TheForceKeyChangesNothingOffMali)
{
	for (bool roaa : {false, true})
	{
		for (bool adreno : {false, true})
		{
			for (bool xclipse : {false, true})
			{
				for (bool denied : {false, true})
				{
					for (bool adreno_key : {false, true})
					{
						GSVulkanFramebufferFetchInputs in;
						in.roaa_available = roaa;
						in.is_adreno = adreno;
						in.is_xclipse = xclipse;
						in.broken_destination_read = denied;
						in.adreno_fetch_key = adreno_key;

						GSVulkanFramebufferFetchInputs forced = in;
						forced.force_mali_fetch_key = true;
						SCOPED_TRACE(testing::Message()
									 << "roaa=" << roaa << " adreno=" << adreno << " xclipse=" << xclipse
									 << " denied=" << denied << " adreno_key=" << adreno_key);

						EXPECT_EQ(DecideVulkanFramebufferFetch(in).enabled,
							DecideVulkanFramebufferFetch(forced).enabled);
						EXPECT_TRUE(DecideVulkanFramebufferFetch(forced).force_key_ignored);
					}
				}
			}
		}
	}
}
