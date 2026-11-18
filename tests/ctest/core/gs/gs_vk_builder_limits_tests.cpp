// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Vulkan::GraphicsPipelineBuilder keeps up to MAX_VERTEX_BUFFERS vertex buffer bindings and MAX_VERTEX_ATTRIBUTES vertex
// attributes. Each array is checked against its own limit: a pipeline with eight attributes on one buffer can still take
// a second buffer and more attributes.

#include "GS/Renderers/Vulkan/VKBuilders.h"

#include <gtest/gtest.h>

TEST(GSVulkanPipelineBuilder, EightAttributesStillTakeASecondBufferAndANinthAttribute)
{
	Vulkan::GraphicsPipelineBuilder gpb;
	gpb.AddVertexBuffer(0, 32);
	for (u32 i = 0; i < 8; i++)
		gpb.AddVertexAttribute(i, 0, VK_FORMAT_R32_UINT, i * 4);
	gpb.AddVertexBuffer(1, 32);
	gpb.AddVertexAttribute(8, 1, VK_FORMAT_R32G32B32A32_UINT, 0);
	gpb.AddVertexAttribute(9, 1, VK_FORMAT_R32G32B32A32_UINT, 16);
	EXPECT_EQ(gpb.GetVertexBufferCount(), 2u);
	EXPECT_EQ(gpb.GetVertexAttributeCount(), 10u);
}

TEST(GSVulkanPipelineBuilder, EveryAttributeUpToTheLimitFits)
{
	Vulkan::GraphicsPipelineBuilder gpb;
	gpb.AddVertexBuffer(0, 64);
	for (u32 i = 0; i < Vulkan::GraphicsPipelineBuilder::MAX_VERTEX_ATTRIBUTES; i++)
		gpb.AddVertexAttribute(i, 0, VK_FORMAT_R32_UINT, i * 4);
	EXPECT_EQ(gpb.GetVertexAttributeCount(), static_cast<u32>(Vulkan::GraphicsPipelineBuilder::MAX_VERTEX_ATTRIBUTES));
}
