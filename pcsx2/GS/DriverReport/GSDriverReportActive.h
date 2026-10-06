// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Which kind of Vulkan driver the open device is on, kept where the Android app can read it from
// any thread.
//
// The app warns a Mali user who is on the Vulkan renderer without malisx2. Whether the GPU is one
// the app offers malisx2 for is the app's own list; whether the driver in use IS malisx2 can only
// be read off the open device (a pack that failed to load falls back to Arm's driver without
// saying so), and that device lives on the GS thread. The GS thread writes the answer here when
// the device comes up and takes it back when the device goes away.

#include <cstdint>
#include <string_view>

namespace GSDriverReport
{
	/// The values are what the Android app reads over JNI (NativeApp.getActiveVulkanDriver), so
	/// they are fixed. MaliDriverNotice.kt carries the same three.
	enum class ActiveVulkanDriver : uint8_t
	{
		/// No Vulkan device is open: another renderer is running, or none has started.
		None = 0,
		/// A Vulkan device is open and its driverInfo names malisx2.
		MaliSX2 = 1,
		/// A Vulkan device is open on any other driver.
		Other = 2,
	};

	/// The Vulkan device came up. `driver_info` is VkPhysicalDeviceDriverProperties::driverInfo,
	/// empty when the driver does not report it.
	void NoteActiveVulkanDriver(std::string_view driver_info);

	/// The Vulkan device went away.
	void ClearActiveVulkanDriver();

	/// Safe from any thread.
	ActiveVulkanDriver GetActiveVulkanDriver();
} // namespace GSDriverReport
