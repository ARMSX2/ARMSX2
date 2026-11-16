// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Which name of the calibrated-timestamps extension the Vulkan device enables
// (Vulkan::PickCalibratedTimestampsName). Spin GPU During Readbacks uses calibrated timestamps to
// place GPU completion times on the CPU clock, so a driver that offers only one of the two names
// still has to get them.

#include "GS/Renderers/Vulkan/VKCalibratedTimestamps.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace
{
struct Device
{
	std::vector<std::string> extensions;
	// Every name the picker asked about, in order. The device code enables each name it asks about
	// and finds, so a name asked about after the first hit would be enabled too.
	std::vector<std::string> asked;

	explicit Device(std::initializer_list<const char*> names)
		: extensions(names.begin(), names.end())
	{
	}

	Vulkan::CalibratedTimestampsName Pick()
	{
		return Vulkan::PickCalibratedTimestampsName([this](const char* name) {
			asked.emplace_back(name);
			for (const std::string& ext : extensions)
			{
				if (ext == name)
					return true;
			}
			return false;
		});
	}
};

const char* const kKHR = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
const char* const kEXT = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
} // namespace

TEST(GSVulkanCalibratedTimestamps, KhrOnlyDriverGetsKhr)
{
	Device d({VK_KHR_SWAPCHAIN_EXTENSION_NAME, kKHR});
	EXPECT_EQ(d.Pick(), Vulkan::CalibratedTimestampsName::KHR);
}

TEST(GSVulkanCalibratedTimestamps, ExtOnlyDriverGetsExt)
{
	Device d({VK_KHR_SWAPCHAIN_EXTENSION_NAME, kEXT});
	EXPECT_EQ(d.Pick(), Vulkan::CalibratedTimestampsName::EXT);
}

TEST(GSVulkanCalibratedTimestamps, BothNamesEnableOnlyKhr)
{
	Device d({kEXT, kKHR});
	EXPECT_EQ(d.Pick(), Vulkan::CalibratedTimestampsName::KHR);
	EXPECT_EQ(d.asked, std::vector<std::string>({kKHR}));
}

TEST(GSVulkanCalibratedTimestamps, NeitherNameIsUnsupported)
{
	Device d({VK_KHR_SWAPCHAIN_EXTENSION_NAME});
	EXPECT_EQ(d.Pick(), Vulkan::CalibratedTimestampsName::Unsupported);
}
