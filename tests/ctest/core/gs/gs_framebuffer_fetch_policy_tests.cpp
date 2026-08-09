// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the OpenGL framebuffer-fetch decision (GS/Renderers/Common/GSFramebufferFetchPolicy.h).
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
