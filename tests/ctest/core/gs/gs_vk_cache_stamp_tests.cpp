// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the Vulkan driver's pipeline blob is stamped with (VKShaderCache::MakePipelineCacheStamp): the
// driver, which the binaries in it belong to, and never the app's build, which changes them not at all.

#include "GS/GSCacheFile.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"

#include <gtest/gtest.h>

#include <cstring>

namespace
{
	struct Driver
	{
		VkPhysicalDeviceProperties props = {};
		VkPhysicalDeviceDriverPropertiesKHR drv = {};

		Driver()
		{
			props.vendorID = 0x5143;
			props.deviceID = 0x43050a01;
			props.driverVersion = 0x06863063;
			props.apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 300);
			std::memset(props.pipelineCacheUUID, 0x5a, sizeof(props.pipelineCacheUUID));
			drv.driverID = VK_DRIVER_ID_MESA_TURNIP;
			std::strcpy(drv.driverName, "turnip Mesa driver");
			std::strcpy(drv.driverInfo, "Mesa 26.3.0-devel (git-axfl3-001)");
		}

		GSCacheFile::Digest Digest() const { return VKShaderCache::MakePipelineCacheStamp(props, drv, false).GetDigest(); }
	};
} // namespace

TEST(GSVkCacheStamp, PipelineBlobDoesNotNameTheBuild)
{
	const Driver d;
	const std::string text = VKShaderCache::MakePipelineCacheStamp(d.props, d.drv, false).GetText();
	EXPECT_EQ(text.find(GSCacheFile::GetBuildId()), std::string::npos) << text;
	EXPECT_EQ(text.find("build["), std::string::npos) << text;
}

TEST(GSVkCacheStamp, PipelineBlobFollowsEveryPartOfTheDriver)
{
	const Driver base;
	Driver d = base;
	EXPECT_EQ(d.Digest(), base.Digest());
	d.props.driverVersion++;
	EXPECT_NE(d.Digest(), base.Digest());
	d = base;
	d.props.pipelineCacheUUID[3] ^= 1;
	EXPECT_NE(d.Digest(), base.Digest());
	d = base;
	std::strcpy(d.drv.driverInfo, "Mesa 26.3.0-devel (git-axfl3-002)");
	EXPECT_NE(d.Digest(), base.Digest());
	d = base;
	d.props.deviceID++;
	EXPECT_NE(d.Digest(), base.Digest());
	EXPECT_NE(VKShaderCache::MakePipelineCacheStamp(base.props, base.drv, true).GetDigest(), base.Digest());
}
