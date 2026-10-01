// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/DriverReport/GSDriverReportJson.h"
#include "GS/Renderers/Vulkan/VKLoader.h"

#include <string>
#include <vector>

namespace GSDriverReport
{
	/// The identity facts of one physical device, for the served-driver verdict and the profile.
	struct VulkanDeviceSummary
	{
		VkPhysicalDevice handle = VK_NULL_HANDLE;
		bool selected = false;
		uint32_t vendor_id = 0;
		uint32_t device_id = 0;
		uint32_t driver_version = 0;
		uint32_t api_version = 0;
		uint32_t max_draw_indirect_count = 0;
		uint32_t driver_id = 0;
		bool has_driver_properties = false;
		std::string device_name;
		std::string driver_name;
		std::string driver_info;
	};

	/// What the running renderer asked for, which only it knows. Left null by the command-line tool.
	struct VulkanLiveFacts
	{
		std::vector<std::string> enabled_instance_extensions;
		std::vector<std::string> enabled_device_extensions;
		/// Optional device extensions the renderer asked for and the device did not have.
		std::vector<std::string> missing_device_extensions;
		/// The instance's requested API version (the renderer asks for 1.1).
		uint32_t instance_api_version = 0;
	};

	/// The device extensions ARMSX2's Vulkan backend asks for, required or optional. The running
	/// renderer records what it actually asked for; this list is what the command-line tool, which
	/// has no renderer, checks a device against.
	const std::vector<const char*>& GetArmsx2DeviceExtensionWishlist();

	/// Writes one driver's whole Vulkan picture as a JSON object: the loader and instance
	/// (version, extensions, layers), then every physical device in full -- properties and limits
	/// through the Properties2 chain, features through the Features2 chain, device extensions with
	/// spec versions, queue families, memory types and heaps, and format support for the formats
	/// ARMSX2 uses. `selected` marks the device in use (may be null). Every query is its own step.
	void WriteVulkanInstance(JsonWriter& w, StepLog& steps, std::string_view step_prefix, VkInstance instance,
		VkPhysicalDevice selected, const VulkanLiveFacts* live, std::vector<VulkanDeviceSummary>* summaries);

	/// Every field of a VkPhysicalDeviceFeatures, e.g. the set a device was created with.
	void WriteVulkanCoreFeatures(JsonWriter& w, const VkPhysicalDeviceFeatures& features);

	/// The version the loader itself supports (vkEnumerateInstanceVersion), or 1.0 when it has no
	/// such entry point.
	uint32_t QueryLoaderInstanceVersion();
} // namespace GSDriverReport
