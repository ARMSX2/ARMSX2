// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/DriverReport/GSDriverReportJson.h"
#include "GS/Renderers/Common/GSGPUProfile.h"

namespace GSDriverReport
{
	/// Writes what the driver-profile resolver decided: the GPU identity, the driver it matched,
	/// the bug and workaround sets by name, and the self-read facts the renderer's road choice
	/// reads. `rules` is optional (null off Vulkan). `identified_driver`, when given, is the driver
	/// as ClassifyServedDriver identifies it from driverInfo, written beside the rule table's own
	/// driver so a driver that claims another's identity (libmali as Arm's) shows both.
	void WriteGpuProfile(JsonWriter& w, const GpuProfileSelection& selection, const VulkanDeviceRules* rules,
		std::string_view identified_driver = {});

	/// Resolves the profile the Vulkan backend would resolve for a device with these properties,
	/// with the profile override on Auto. For the command-line tool, which has no running backend.
	GpuProfileSelection ResolveVulkanProfile(const MobileDriverContext& context, std::string_view device_name,
		VulkanDeviceRules* rules_out);
} // namespace GSDriverReport
