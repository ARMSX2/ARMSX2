// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the feedback-loop declaration spelling (GS/Renderers/Common/GSDynamicFeedbackLoopPolicy.h).
//
// The same declaration can be made once per pipeline, with a create flag, or once per draw, with
// vkCmdSetAttachmentFeedbackLoopEnableEXT. On Turnip they are charged differently: the create
// flag is read per pipeline, and the feedback-loop carry puts it on every pipeline in a latched
// pass, so the driver's serialising primitive mode reaches draws that never read anything. On the
// SD865 that is 51.8 ms against 18.5 on wrc3@1x -- one title straight over its frame budget.
//
// ⚠️ Since 2026-09-22 the per-draw spelling is the DEFAULT on the drivers it was run on (Turnip,
// Honeykrisp). What these tests now pin is that a run with no flag, no key and no setting gets per
// draw there wherever there is a loop to declare and an extension to declare it with; that desktop
// Vulkan keeps the create flag it always had; that forcing the create flag still works, for pricing the
// fallback; and that the one case worth a line in the log -- the layout road live with no
// dynamic-state extension, so the declaration falls back to the expensive spelling -- is reported
// while the ordinary copy-road case stays silent. The old shape of that report fired whenever the
// per-draw spelling was asked for and refused, which as a default would have printed on every
// copy-road device on earth.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSDynamicFeedbackLoopPolicy.h"
#include "GS/Renderers/Common/GSMeasurementOverrides.h"

#include <gtest/gtest.h>

namespace
{
	// A measured driver (Turnip) on the layout road with the extension: the one configuration the
	// per-draw spelling can be applied on.
	constexpr GSDynamicFeedbackLoopInputs Capable()
	{
		GSDynamicFeedbackLoopInputs in;
		in.layout_road_live = true;
		in.dynamic_state_available = true;
		in.device_measured = true;
		return in;
	}

	// Desktop Vulkan -- NVIDIA, AMD (RADV) or Intel (ANV): the layout road by default, the
	// extension possibly present, and never run with the per-draw spelling.
	constexpr GSDynamicFeedbackLoopInputs DesktopVulkan()
	{
		GSDynamicFeedbackLoopInputs in;
		in.layout_road_live = true;
		in.dynamic_state_available = true;
		return in;
	}

	constexpr GSDynamicFeedbackLoopInputs Forced(GSDynamicFeedbackLoopInputs in)
	{
		in.spelling = GSLoopDeclarationSpelling::PipelineCreateFlag;
		return in;
	}
} // namespace

// The default is per draw, and it takes no flag to get there. This is the row that says the road
// a user is on is the road that was measured.
TEST(GSDynamicFeedbackLoop, TheDefaultIsPerDraw)
{
	EXPECT_EQ(GSDynamicFeedbackLoopInputs{}.spelling, GSLoopDeclarationSpelling::DynamicPerDraw);
	EXPECT_TRUE(GSDeclaresLoopPerDraw(Capable()));
	EXPECT_FALSE(GSLoopSpellingFallsBackToCreateFlag(Capable()));
}

// Forced back to the create flag on a device that could have done either: deliberate, so it is
// not a fallback and nothing is reported.
TEST(GSDynamicFeedbackLoop, ForcedToTheCreateFlagItIsNotAFallback)
{
	EXPECT_FALSE(GSDeclaresLoopPerDraw(Forced(Capable())));
	EXPECT_FALSE(GSLoopSpellingFallsBackToCreateFlag(Forced(Capable())));
}

// On the layout road without the extension the loop still has to be declared, and the create flag
// is the only spelling left. That IS the fallback, and it is said out loud.
TEST(GSDynamicFeedbackLoop, WithoutTheExtensionItFallsBackAndIsReported)
{
	GSDynamicFeedbackLoopInputs in = Capable();
	in.dynamic_state_available = false;
	EXPECT_FALSE(GSDeclaresLoopPerDraw(in));
	EXPECT_TRUE(GSLoopSpellingFallsBackToCreateFlag(in));

	// Forcing the create flag on the same device is the same emission and a different report.
	EXPECT_FALSE(GSLoopSpellingFallsBackToCreateFlag(Forced(in)));
}

// Desktop Vulkan keeps origin/master's pipeline create flag, with or without the extension, and
// says nothing about it: it is the spelling that driver has always had, not a fallback.
TEST(GSDynamicFeedbackLoop, DesktopVulkanKeepsTheCreateFlagSilently)
{
	EXPECT_FALSE(GSDeclaresLoopPerDraw(DesktopVulkan()));
	EXPECT_FALSE(GSLoopSpellingFallsBackToCreateFlag(DesktopVulkan()));

	GSDynamicFeedbackLoopInputs no_extension = DesktopVulkan();
	no_extension.dynamic_state_available = false;
	EXPECT_FALSE(GSDeclaresLoopPerDraw(no_extension));
	EXPECT_FALSE(GSLoopSpellingFallsBackToCreateFlag(no_extension));
}

// Off the layout road there is no declaration to spell: the in-tile road states the loop with an
// input attachment and the copy road states nothing at all. Neither spelling applies, and it is
// silent -- this is the ordinary case on most devices, so a report here would be noise in every
// log rather than a warning in a few.
TEST(GSDynamicFeedbackLoop, OffTheLayoutRoadNothingIsDeclaredAndNothingIsReported)
{
	GSDynamicFeedbackLoopInputs in = Capable();
	in.layout_road_live = false;
	EXPECT_FALSE(GSDeclaresLoopPerDraw(in));
	EXPECT_FALSE(GSLoopSpellingFallsBackToCreateFlag(in));

	EXPECT_FALSE(GSDeclaresLoopPerDraw({}));
	EXPECT_FALSE(GSLoopSpellingFallsBackToCreateFlag({}));
}

// Swept: applying is the conjunction, the fallback is exactly the layout-road cases that wanted
// per draw and did not get it, and the two are never both true.
TEST(GSDynamicFeedbackLoop, AppliedAndFallbackPartitionTheLayoutRoad)
{
	for (int bits = 0; bits < 16; bits++)
	{
		GSDynamicFeedbackLoopInputs in;
		in.spelling = (bits & 1) != 0 ? GSLoopDeclarationSpelling::DynamicPerDraw :
		                                GSLoopDeclarationSpelling::PipelineCreateFlag;
		in.layout_road_live = (bits & 2) != 0;
		in.dynamic_state_available = (bits & 4) != 0;
		in.device_measured = (bits & 8) != 0;

		const bool wants = in.spelling == GSLoopDeclarationSpelling::DynamicPerDraw && in.device_measured;
		const bool applied = wants && in.layout_road_live && in.dynamic_state_available;
		const bool fell_back = wants && in.layout_road_live && !in.dynamic_state_available;
		EXPECT_EQ(GSDeclaresLoopPerDraw(in), applied) << "bits=" << bits;
		EXPECT_EQ(GSLoopSpellingFallsBackToCreateFlag(in), fell_back) << "bits=" << bits;
		EXPECT_FALSE(GSDeclaresLoopPerDraw(in) && GSLoopSpellingFallsBackToCreateFlag(in)) << "bits=" << bits;
	}
}

// The harness switch: per draw unless -loop-create-flag asked for the other spelling.
TEST(GSDynamicFeedbackLoop, TheHarnessSpellingDefaultsToPerDraw)
{
	EXPECT_EQ(g_gs_measurement_overrides.LoopSpelling(), GSLoopDeclarationSpelling::DynamicPerDraw);

	GSMeasurementOverrides forced;
	forced.loop_create_flag = true;
	EXPECT_EQ(forced.LoopSpelling(), GSLoopDeclarationSpelling::PipelineCreateFlag);
	EXPECT_TRUE(forced.Any());
}
