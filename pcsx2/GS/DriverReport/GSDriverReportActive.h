// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Which kind of Vulkan driver the open device is on, kept where the Android host can read it from
// any thread, and the decision the host makes from it.
//
// The Android host warns a Mali user who is on the Vulkan renderer without malisx2 (and says whether
// the pack is installed), and an Adreno user who is on Qualcomm's own driver. Whether the GPU is one the app offers malisx2 for is the
// app's own list; whether the driver in use IS malisx2 can only be read off the open device (a
// pack that failed to load falls back to Arm's driver without saying so), and that device lives on
// the GS thread. The GS thread writes the answer here when the device comes up and takes it back
// when the device goes away.

#include <cstdint>
#include <string>
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

	/// Which driver the user picked for this boot, read off the driver request the Vulkan library
	/// was loaded with (Vulkan::GetCustomDriverStatus), which is the per-game choice for the
	/// booting game.
	enum class SelectedVulkanPack : uint8_t
	{
		/// No pack: the system Vulkan driver.
		System,
		/// A malisx2 pack.
		MaliSX2,
		/// Any other pack.
		Other,
	};

	/// What to tell a user whose hardware renderer is on a Vulkan driver that is not malisx2, on a
	/// GPU the app offers malisx2 for. The software renderer uses a Vulkan device only to present
	/// the CPU-drawn frame, so its driver does not matter.
	enum class MaliSX2Notice : uint8_t
	{
		None,
		/// The system driver is selected and no malisx2 pack is installed.
		Get,
		/// Another pack is selected and no malisx2 pack is installed.
		OtherPackSelected,
		/// A malisx2 pack is installed and something else is selected. The notice names the pack
		/// and says where to switch.
		InstalledNotSelected,
	};

	/// Not when the user has selected a malisx2 pack for this boot: the device is then on another
	/// driver because the pack did not load (malisx2 needs a recent Mali kernel driver; on an older
	/// one it fails to open and the system loader answers), and telling the user to get or switch
	/// to what they already selected would be wrong.
	constexpr MaliSX2Notice MaliSX2NoticeFor(bool hardware_renderer, bool gpu_offers_malisx2, ActiveVulkanDriver driver,
		SelectedVulkanPack selected, bool malisx2_pack_installed)
	{
		if (!hardware_renderer || !gpu_offers_malisx2 || driver != ActiveVulkanDriver::Other ||
			selected == SelectedVulkanPack::MaliSX2)
			return MaliSX2Notice::None;
		if (malisx2_pack_installed)
			return MaliSX2Notice::InstalledNotSelected;
		return selected == SelectedVulkanPack::Other ? MaliSX2Notice::OtherPackSelected : MaliSX2Notice::Get;
	}

	/// The OSD text for a notice, empty for None. `installed_pack` is how the app names the
	/// installed malisx2 pack ("malisx2 0.4.1"), used only by InstalledNotSelected. A toast has no
	/// button, so the text says where the app's driver list is; the driver list itself offers the
	/// switch.
	std::string MaliSX2NoticeText(MaliSX2Notice notice, std::string_view installed_pack);

	/// Whether to tell the user to get a Turnip driver: the hardware renderer is on Qualcomm's own
	/// driver. On Adreno the project's target driver is Turnip, and Qualcomm's 8xx generation refuses
	/// the run machine's texture sets. A Turnip pack that failed to load leaves the device on
	/// Qualcomm's driver too, and the notice is then still true.
	constexpr bool ShouldWarnQualcomm(bool hardware_renderer, ActiveVulkanDriver driver)
	{
		return hardware_renderer && driver == ActiveVulkanDriver::QualcommProprietary;
	}
} // namespace GSDriverReport
