// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSDeviceVK::GetWindowSystemExtensions decides which surface and swap chain extensions a Vulkan
// device is created with. A device made without them can never present: when the window's surface
// arrives later, vkCreate*SurfaceKHR was never loaded and the call jumps to address 0. So the answer
// has to follow the window system the window presents to, not whether it has a surface at the moment
// the device opens. An Android app between surfaces (backgrounded, a settings screen over the game)
// reports a Surfaceless window that will return, and a GS device reopened then must still get them.

#include "GS/Renderers/Vulkan/GSDeviceVK.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace
{
	// A window system this build has Vulkan surface support for.
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
	constexpr WindowInfo::Type kPlatform = WindowInfo::Type::Android;
	constexpr const char* kPlatformExtension = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
	constexpr WindowInfo::Type kPlatform = WindowInfo::Type::Wayland;
	constexpr const char* kPlatformExtension = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
	constexpr WindowInfo::Type kPlatform = WindowInfo::Type::X11;
	constexpr const char* kPlatformExtension = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
	constexpr WindowInfo::Type kPlatform = WindowInfo::Type::Win32;
	constexpr const char* kPlatformExtension = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_METAL_EXT)
	constexpr WindowInfo::Type kPlatform = WindowInfo::Type::MacOS;
	constexpr const char* kPlatformExtension = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#else
	constexpr WindowInfo::Type kPlatform = WindowInfo::Type::VulkanDirect;
	constexpr const char* kPlatformExtension = VK_KHR_DISPLAY_EXTENSION_NAME;
#endif

	bool Has(const std::vector<const char*>& list, const char* name)
	{
		return std::any_of(list.begin(), list.end(), [name](const char* n) { return std::strcmp(n, name) == 0; });
	}

	void ExpectCanPresent(const GSDeviceVK::WindowSystemExtensions& exts)
	{
		EXPECT_EQ(exts.type, kPlatform);
		EXPECT_EQ(exts.instance.size(), 2u);
		EXPECT_TRUE(Has(exts.instance, VK_KHR_SURFACE_EXTENSION_NAME));
		EXPECT_TRUE(Has(exts.instance, kPlatformExtension));
		EXPECT_EQ(exts.device.size(), 1u);
		EXPECT_TRUE(Has(exts.device, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
	}
} // namespace

TEST(VkWindowSystem, WindowWithSurfaceGetsSurfaceAndSwapChain)
{
	WindowInfo wi;
	wi.type = kPlatform;
	ExpectCanPresent(GSDeviceVK::GetWindowSystemExtensions(wi));
}

// The case that crashed: the device opens while the surface is gone, and the surface comes back.
TEST(VkWindowSystem, SurfacePendingGetsSurfaceAndSwapChain)
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.pending_type = kPlatform;
	ExpectCanPresent(GSDeviceVK::GetWindowSystemExtensions(wi));
}

// Runners, libretro and tests never present, and must not need any window-system extension.
TEST(VkWindowSystem, HeadlessGetsNothing)
{
	const GSDeviceVK::WindowSystemExtensions exts = GSDeviceVK::GetWindowSystemExtensions(WindowInfo());
	EXPECT_EQ(exts.type, WindowInfo::Type::Surfaceless);
	EXPECT_TRUE(exts.instance.empty());
	EXPECT_TRUE(exts.device.empty());
}
