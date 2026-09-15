// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the fast stencil shadow road (GS/Renderers/Common/GSFastStencilShadow.h): which devices
// take it.
//
// The device rule has no setting behind it, so these cases are the whole contract. It is on for
// Vulkan with texture barriers off and dual-source blending, and off when any one of the three is
// missing. The case that matters most is D3D11: it also runs without texture barriers, and reading
// "no barriers" as "frame reads are expensive" would put a draw the D3D11 shader cannot express on
// a backend where the read it replaces is cheap.
//
// Rides gs_vertex_tests.

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSFastStencilShadow.h"

#include <gtest/gtest.h>

namespace
{
	constexpr RenderAPI kAllApis[] = {
		RenderAPI::None, RenderAPI::D3D11, RenderAPI::Metal, RenderAPI::D3D12, RenderAPI::Vulkan, RenderAPI::OpenGL};
} // namespace

// The Adreno shape: Vulkan, the RT-copy workaround has turned barriers off, and the blend unit has
// a second input.
TEST(GSFastStencilShadow, OnForVulkanWithoutBarriersWithDualSource)
{
	EXPECT_TRUE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, false, true));
}

TEST(GSFastStencilShadow, OffWhenTextureBarriersAreOn)
{
	// Desktop Vulkan, and OverrideTextureBarriers=1 on an Adreno part.
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, true, true));
}

TEST(GSFastStencilShadow, OffWithoutDualSourceBlend)
{
	// A Mali part on the RT-copy workaround.
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, false, false));
}

TEST(GSFastStencilShadow, OffOnD3D11WithoutBarriers)
{
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::D3D11, false, true));
}

// Every combination: on exactly when all three facts hold.
TEST(GSFastStencilShadow, OnExactlyWhenAllThreeHold)
{
	for (RenderAPI api : kAllApis)
	{
		for (bool texture_barrier : {false, true})
		{
			for (bool dual_source : {false, true})
			{
				const bool expected = api == RenderAPI::Vulkan && !texture_barrier && dual_source;
				EXPECT_EQ(GSFastStencilShadow::DeviceQualifies(api, texture_barrier, dual_source), expected)
					<< "api " << static_cast<int>(api) << " texture_barrier " << texture_barrier << " dual_source "
					<< dual_source;
			}
		}
	}
}

// Backends other than Vulkan never assign the bit, so it has to start off.
TEST(GSFastStencilShadow, FeatureBitStartsOff)
{
	const GSDevice::FeatureSupport features;
	EXPECT_FALSE(features.fast_stencil_shadow);
}
