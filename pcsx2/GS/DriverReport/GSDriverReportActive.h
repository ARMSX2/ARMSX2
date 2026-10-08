// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Which kind of Vulkan driver the open device is on, kept where the Android host can read it from
// any thread, and the decision the host makes from it.
//
// The Android host warns a Mali user who is on the Vulkan renderer without malisx2. Whether the GPU
// is one the app offers malisx2 for is the app's own list; whether the driver in use IS malisx2 can
// only be read off the open device (a pack that failed to load falls back to Arm's driver without
// saying so), and that device lives on the GS thread. The GS thread writes the answer here when
// the device comes up and takes it back when the device goes away.

#include <cstdint>
#include <string_view>

namespace GSDriverReport
{
	enum class ActiveVulkanDriver : uint8_t
	{
		/// No Vulkan device is open: another renderer is running, or none has started.
		None,
		/// A Vulkan device is open and its driverInfo names malisx2.
		MaliSX2,
		/// A Vulkan device is open on any other driver.
		Other,
	};

	/// The Vulkan device came up. `driver_info` is VkPhysicalDeviceDriverProperties::driverInfo,
	/// empty when the driver does not report it.
	void NoteActiveVulkanDriver(std::string_view driver_info);

	/// The Vulkan device went away.
	void ClearActiveVulkanDriver();

	/// Safe from any thread.
	ActiveVulkanDriver GetActiveVulkanDriver();

	/// Whether to tell the user to get malisx2: the hardware renderer is running on a Vulkan device
	/// whose driver is not malisx2, on a GPU the app offers malisx2 for. The software renderer can
	/// sit on a Vulkan device only to present the frame the CPU drew, so a different driver costs
	/// that user nothing.
	///
	/// Not when the user has already selected a malisx2 pack for this boot. Then the device is on
	/// another driver because the pack did not load (malisx2 needs a recent Mali kernel driver, and
	/// on an older one it fails to open and the system loader answers), and telling them to download
	/// what they selected is wrong. The pack is read off the driver request the device was created
	/// from (Vulkan::GetCustomDriverStatus), which is the per-game choice for the booting game.
	constexpr bool ShouldWarnMaliSX2(
		bool hardware_renderer, bool gpu_offers_malisx2, ActiveVulkanDriver driver, bool malisx2_pack_selected)
	{
		return hardware_renderer && gpu_offers_malisx2 && driver == ActiveVulkanDriver::Other && !malisx2_pack_selected;
	}
} // namespace GSDriverReport
