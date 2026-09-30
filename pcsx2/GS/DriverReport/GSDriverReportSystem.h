// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The parts of the driver report that come from the operating system rather than the graphics
// API: device and build identity, the vendor graphics libraries on disk, the selected driver
// pack, and the GPU kernel driver. Every read is best-effort and logged as its own step.

#include "GS/DriverReport/GSDriverReportJson.h"

#include <cstdint>
#include <string>

namespace GSDriverReport
{
	/// The "device" object: model, manufacturer, board, platform, SoC, SDK, kernel, page size, plus
	/// the build fingerprints, uname, /proc/version and the CPU cores.
	void WriteDeviceFacts(JsonWriter& w, StepLog& steps);

	/// The vendor Vulkan/GLES libraries on disk (path, size, time) and, on Android, the
	/// ro.hardware.* properties that choose among them.
	void WriteVendorLibraries(JsonWriter& w, StepLog& steps);

	/// The selected custom driver pack, as far as it can be read.
	struct PackInfo
	{
		bool custom_selected = false;
		/// adrenotools opened it. False with custom_selected means the system loader answered.
		bool opened = false;
		bool required = false;
		std::string pack_path;
		std::string library_name;
		std::string redirect_dir;
		std::string hook_lib_dir;
		std::string failure;

		/// From meta.json where present, else the pack directory's name.
		std::string pack_name;
		std::string meta_description;
		std::string meta_vendor;
		std::string meta_driver_version;
		/// meta.json verbatim when it parses, for embedding; empty otherwise.
		std::string meta_json;
		std::string meta_error;
	};

	/// Reads meta.json from the pack directory. Never fails; problems land in meta_error.
	void ReadPackMeta(PackInfo* pack, StepLog& steps);

	/// The "selected_driver" object: kind, pack name and path, meta.json, the driver file's size
	/// and SHA-256, and the adrenotools hook libraries.
	void WriteSelectedDriver(JsonWriter& w, StepLog& steps, const PackInfo& pack);

	/// What the kernel section found, for the served-driver verdict.
	struct KernelFacts
	{
		bool kgsl_present = false;
		bool kgsl_opened = false;
		uint32_t kgsl_chip_id = 0;
		uint32_t kgsl_gpu_id = 0;
		std::string kgsl_gpu_model;
		bool mali_present = false;
	};

	/// The "kernel" object, chosen by which device node exists: KGSL on Adreno, kbase on Mali, and
	/// the DRM render nodes' kernel drivers wherever there are any.
	void WriteKernel(JsonWriter& w, StepLog& steps, KernelFacts* facts);
} // namespace GSDriverReport
