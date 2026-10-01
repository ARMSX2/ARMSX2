// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The driver report written beside every GS dump as `<dump>.driver.json`: which GPU driver served
// the game, what it reports about itself, what ARMSX2's driver profile decided for it and which
// roads the renderer took, plus the device, build and GPU kernel driver. A dump says what the game
// drew; this says what drew it.
//
// Collected from the live renderer, so the served driver is the one that rendered the dump. Never
// fails the dump: a failed check is logged in the report's "steps" and the rest is still written.

#include "GS/DriverReport/GSDriverReportJson.h"
#include "GS/Renderers/Common/GSDevice.h"

#include <cstdint>
#include <string>

namespace GSDriverReport
{
	/// What a backend contributes. Each string, when non-empty, is one complete JSON object.
	struct BackendReport
	{
		/// GSDevice::RenderAPIToString.
		std::string api;
		/// GSDevice::GetDriverInfo's text.
		std::string driver_info_text;
		/// The backend's decisions: features, driver profile, roads, enabled extensions.
		std::string backend_json;
		/// Vulkan only: WriteVulkanInstance's object for the live instance.
		std::string vulkan_json;
		/// OpenGL only: vendor, renderer, version, GLSL version and extensions.
		std::string gl_json;

		/// The served driver's own identity, when the API reports one (Vulkan).
		bool has_served_facts = false;
		uint32_t vendor_id = 0;
		uint32_t driver_id = 0;
		std::string driver_name;
		std::string driver_info;
		std::string device_name;

		StepLog steps;
	};

	/// Writes the renderer-independent device state every backend shares: the feature table and the
	/// resolved GPU/driver profile as the device holds it.
	void WriteFeatureSupport(JsonWriter& w, const GSDevice::FeatureSupport& f);

	/// Collects the report and writes `<dump_base>.driver.json`. Called on the GS thread right after
	/// a dump opens. Work that touches the file system or the kernel runs on a helper
	/// thread with a deadline, so a node that blocks costs the report its section, not the dump.
	void WriteSidecarForDump(const std::string& dump_base);

	/// The sidecar's path for a dump base name.
	std::string SidecarPath(const std::string& dump_base);
} // namespace GSDriverReport
