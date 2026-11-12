// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Which kind of Vulkan driver the open device is on, kept where the Android host can read it from
// any thread, and the decision the host makes from it.
//
// The Android host warns a Mali user who is on the Vulkan renderer without malisx2, and an Adreno
// user who is on Qualcomm's own driver. Whether the GPU is one the app offers malisx2 for is the
// app's own list; whether the driver in use IS malisx2 can only be read off the open device (a
// pack that failed to load falls back to Arm's driver without saying so), and that device lives on
// the GS thread. The GS thread writes the answer here when the device comes up and takes it back
// when the device goes away.

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
		/// A Vulkan device is open on Qualcomm's own driver (its driverID): the phone's built-in one
		/// or a pack of Qualcomm's driver.
		QualcommProprietary,
		/// A Vulkan device is open on any other driver.
		Other,
	};

	/// The Vulkan device came up. `driver_id` and `driver_info` are VkPhysicalDeviceDriverProperties'
	/// driverID and driverInfo, 0 and empty when the driver does not report them.
	void NoteActiveVulkanDriver(uint32_t driver_id, std::string_view driver_info);

	void ClearActiveVulkanDriver();

	/// Safe from any thread.
	ActiveVulkanDriver GetActiveVulkanDriver();

	/// Whether to tell the user to get malisx2: the hardware renderer is on a Vulkan device whose
	/// driver is not malisx2, on a GPU the app offers malisx2 for. The software renderer uses a
	/// Vulkan device only to present the CPU-drawn frame, so its driver does not matter.
	///
	/// Not when the user has already selected a malisx2 pack for this boot: the device is then on
	/// another driver because the pack did not load (malisx2 needs a recent Mali kernel driver; on
	/// an older one it fails to open and the system loader answers). The selected pack is read off
	/// the driver request the device was created from (Vulkan::GetCustomDriverStatus), which is the
	/// per-game choice for the booting game.
	constexpr bool ShouldWarnMaliSX2(
		bool hardware_renderer, bool gpu_offers_malisx2, ActiveVulkanDriver driver, bool malisx2_pack_selected)
	{
		return hardware_renderer && gpu_offers_malisx2 && driver == ActiveVulkanDriver::Other && !malisx2_pack_selected;
	}

	/// Whether to tell the user to get a Turnip driver: the hardware renderer is on Qualcomm's own
	/// driver. On Adreno the project's target driver is Turnip, and Qualcomm's 8xx generation refuses
	/// the run machine's texture sets. A Turnip pack that failed to load leaves the device on
	/// Qualcomm's driver too, and the notice is then still true.
	constexpr bool ShouldWarnQualcomm(bool hardware_renderer, ActiveVulkanDriver driver)
	{
		return hardware_renderer && driver == ActiveVulkanDriver::QualcommProprietary;
	}
} // namespace GSDriverReport
