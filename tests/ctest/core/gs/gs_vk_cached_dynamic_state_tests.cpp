// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The Vulkan device sets viewport, scissor, blend constants and line width only when the value it last set changes
// (GSDeviceVK::ApplyBaseState). A pipeline that holds one of them static overwrites it when it is bound, and the next
// draw that wants the value the device last set draws with the pipeline's instead. So every pipeline the device builds
// takes all four dynamic. The stencil counter resolve pipeline once declared only viewport and scissor, and a
// constant-blended draw after it drew with a blend constant of 0.

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"

#include <gtest/gtest.h>

namespace
{
constexpr VkDynamicState kCached[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
	VK_DYNAMIC_STATE_LINE_WIDTH};

void ExpectCached(const Vulkan::GraphicsPipelineBuilder& gpb)
{
	for (VkDynamicState state : kCached)
		EXPECT_TRUE(gpb.HasDynamicState(state)) << "dynamic state " << static_cast<int>(state);
}
} // namespace

TEST(GSVulkanCachedDynamicState, TheSharedDeclarationCoversEveryCachedState)
{
	Vulkan::GraphicsPipelineBuilder gpb;
	for (VkDynamicState state : kCached)
		EXPECT_FALSE(gpb.HasDynamicState(state));
	GSDeviceVK::AddCachedDynamicState(gpb);
	ExpectCached(gpb);
}

TEST(GSVulkanCachedDynamicState, TheStencilCounterResolvePipelineTakesThemDynamic)
{
	for (u32 rgb_mask = 1; rgb_mask < 8; rgb_mask++)
	{
		Vulkan::GraphicsPipelineBuilder gpb;
		GSDeviceVK::SetStencilCounterResolveState(gpb, rgb_mask);
		ExpectCached(gpb);
	}
}

TEST(GSVulkanCachedDynamicState, DeclaringAStateTwiceKeepsOneEntry)
{
	// Vulkan forbids a state listed twice in VkPipelineDynamicStateCreateInfo.
	Vulkan::GraphicsPipelineBuilder gpb;
	gpb.SetDynamicViewportAndScissorState();
	GSDeviceVK::AddCachedDynamicState(gpb);
	GSDeviceVK::AddCachedDynamicState(gpb);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	ExpectCached(gpb);
	// Four distinct states: a fifth new one still fits and is the only addition.
	gpb.AddDynamicState(VK_DYNAMIC_STATE_DEPTH_BIAS);
	EXPECT_TRUE(gpb.HasDynamicState(VK_DYNAMIC_STATE_DEPTH_BIAS));
}
