// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the feedback-loop declaration spelling (GS/Renderers/Common/GSDynamicFeedbackLoopPolicy.h).
//
// The same declaration can be made once per pipeline, with a create flag, or once per draw, with
// vkCmdSetAttachmentFeedbackLoopEnableEXT. On Turnip they are charged differently: the create
// flag is read per pipeline, and the feedback-loop carry puts it on every pipeline in a latched
// pass, so the driver's serialising primitive mode reaches draws that never read anything.
//
// What needs pinning is the pair of preconditions. The per-draw spelling needs the extension AND
// the feedback-loop layout road -- off that road the in-tile spelling states the loop with an
// input attachment and the copy road states nothing, so there is no declaration to respell. Both
// misses have to be REPORTED rather than silently ignored, because an arm that quietly does
// nothing is a device round that measures the other arm twice.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSDynamicFeedbackLoopPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// A device on the layout road with the extension: the one configuration the override can be
	// applied on.
	constexpr GSDynamicFeedbackLoopInputs Capable()
	{
		GSDynamicFeedbackLoopInputs in;
		in.layout_road_live = true;
		in.dynamic_state_available = true;
		return in;
	}

	constexpr GSDynamicFeedbackLoopInputs Asked(GSDynamicFeedbackLoopInputs in)
	{
		in.spelling = GSLoopDeclarationSpelling::DynamicPerDraw;
		return in;
	}
} // namespace

// The default is the create flag, and nothing about the device changes that. This is the row that
// makes the byte-identity gate a statement about the binary rather than about one road.
TEST(GSDynamicFeedbackLoop, TheDefaultIsThePipelineCreateFlag)
{
	EXPECT_FALSE(GSDeclaresLoopPerDraw(Capable()));
	EXPECT_FALSE(GSDynamicLoopRequestedButUnavailable(Capable()));
	EXPECT_FALSE(GSDeclaresLoopPerDraw({}));
	EXPECT_FALSE(GSDynamicLoopRequestedButUnavailable({}));
}

// Asked for, with the road and the extension, it applies and nothing is reported.
TEST(GSDynamicFeedbackLoop, AskedForOnACapableDeviceItApplies)
{
	EXPECT_TRUE(GSDeclaresLoopPerDraw(Asked(Capable())));
	EXPECT_FALSE(GSDynamicLoopRequestedButUnavailable(Asked(Capable())));
}

// Without the extension there is no per-draw spelling to use. The request is refused and said
// out loud.
TEST(GSDynamicFeedbackLoop, WithoutTheExtensionItIsRefusedAndReported)
{
	GSDynamicFeedbackLoopInputs in = Asked(Capable());
	in.dynamic_state_available = false;
	EXPECT_FALSE(GSDeclaresLoopPerDraw(in));
	EXPECT_TRUE(GSDynamicLoopRequestedButUnavailable(in));
}

// Off the layout road there is no declaration to respell: the in-tile road states the loop with
// an input attachment and the copy road states nothing at all. Also refused, also reported.
TEST(GSDynamicFeedbackLoop, OffTheLayoutRoadItIsRefusedAndReported)
{
	GSDynamicFeedbackLoopInputs in = Asked(Capable());
	in.layout_road_live = false;
	EXPECT_FALSE(GSDeclaresLoopPerDraw(in));
	EXPECT_TRUE(GSDynamicLoopRequestedButUnavailable(in));
}

// Swept: applying is the conjunction, and "requested but unavailable" is exactly the requests
// that did not apply -- the two are never both true and never both false under a request.
TEST(GSDynamicFeedbackLoop, AppliedAndUnavailablePartitionTheRequests)
{
	for (int bits = 0; bits < 8; bits++)
	{
		GSDynamicFeedbackLoopInputs in;
		in.spelling = (bits & 1) != 0 ? GSLoopDeclarationSpelling::DynamicPerDraw :
		                                GSLoopDeclarationSpelling::PipelineCreateFlag;
		in.layout_road_live = (bits & 2) != 0;
		in.dynamic_state_available = (bits & 4) != 0;

		const bool asked = in.spelling == GSLoopDeclarationSpelling::DynamicPerDraw;
		const bool applied = asked && in.layout_road_live && in.dynamic_state_available;
		EXPECT_EQ(GSDeclaresLoopPerDraw(in), applied) << "bits=" << bits;
		EXPECT_EQ(GSDynamicLoopRequestedButUnavailable(in), asked && !applied) << "bits=" << bits;
		EXPECT_FALSE(GSDeclaresLoopPerDraw(in) && GSDynamicLoopRequestedButUnavailable(in)) << "bits=" << bits;
	}
}

// The process-wide switch: default the create flag, settable, and its banner name says which arm
// a log is.
TEST(GSDynamicFeedbackLoop, TheProcessSpellingDefaultsToTheCreateFlag)
{
	EXPECT_EQ(GSDynamicFeedbackLoopPolicy::GetSpelling(), GSLoopDeclarationSpelling::PipelineCreateFlag);
	EXPECT_FALSE(GSDynamicFeedbackLoopPolicy::WantsDynamicPerDraw());
	EXPECT_STREQ(GSDynamicFeedbackLoopPolicy::Name(), "pipeline create flag");

	GSDynamicFeedbackLoopPolicy::SetSpelling(GSLoopDeclarationSpelling::DynamicPerDraw);
	EXPECT_TRUE(GSDynamicFeedbackLoopPolicy::WantsDynamicPerDraw());
	EXPECT_STREQ(GSDynamicFeedbackLoopPolicy::Name(), "dynamic per draw");

	GSDynamicFeedbackLoopPolicy::SetSpelling(GSLoopDeclarationSpelling::PipelineCreateFlag);
	EXPECT_FALSE(GSDynamicFeedbackLoopPolicy::WantsDynamicPerDraw());
}
