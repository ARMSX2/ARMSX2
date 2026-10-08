// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Which Vulkan driver actually answered, and whether that is the one that was asked for.
//
// A custom driver pack loads through libadrenotools, and a load that fails can fall back to the
// system driver without the user seeing anything. The driver that answered is therefore read off
// the device's own properties, never inferred from the selection.
//
// Pure functions of plain values, with no Vulkan headers, so they are unit-testable.

#include <cstdint>
#include <string>
#include <string_view>

namespace GSDriverReport
{
	/// VkDriverId values the classification needs. Numeric so this header does not pull in Vulkan.
	namespace DriverIdValue
	{
		constexpr uint32_t QualcommProprietary = 8;
		constexpr uint32_t ArmProprietary = 9;
		constexpr uint32_t MesaTurnip = 18;
		constexpr uint32_t MesaPanVK = 20;
	} // namespace DriverIdValue

	/// What the device reports about itself.
	struct ServedDriverFacts
	{
		uint32_t vendor_id = 0;
		uint32_t driver_id = 0;
		std::string_view driver_name;
		std::string_view driver_info;
		std::string_view device_name;
	};

	struct ServedDriverClassification
	{
		/// "turnip", "malisx2", "vendor-qualcomm", "vendor-arm", "panvk", "mesa-<driverName>" for
		/// another Mesa driver, "other", or "unknown".
		std::string answered;
		/// The property values the verdict rests on, for a human to check.
		std::string evidence;
		/// From a `git-axfl<G>-` tag in driverInfo; 0 when there is none.
		uint32_t axfl_generation = 0;
	};

	/// Whether driverInfo names malisx2, our Vulkan driver for Mali. Forwards to
	/// GpuProfileDetector::IsMaliSX2Driver, which owns the rule.
	bool IsMaliSX2Driver(std::string_view driver_info);

	/// Whether an installed custom driver pack is a malisx2 pack, from the directory it was
	/// installed in and the library file it loads. The app names a pack's directory by its id
	/// ("armsx2libmali-..." for the ones it downloads, kept from before the rename) and the pack's
	/// library is libvulkan_malisx2.so. Only the last component of `pack_dir` is read, so the
	/// app's own data path cannot match. Same two spellings as IsMaliSX2Driver, case-insensitive.
	bool IsMaliSX2Pack(std::string_view pack_dir, std::string_view library_name);

	/// Classifies the driver from its own properties. malisx2 deliberately reports Arm's driverID
	/// and a stock-looking device name, so it is told apart by IsMaliSX2Driver on its driverInfo
	/// and never by driverID.
	ServedDriverClassification ClassifyServedDriver(const ServedDriverFacts& facts);

	/// What a selected custom pack should be, from its name, library file name and meta.json text:
	/// "turnip", "malisx2", or "custom" when neither can be told. Returns "system" for no pack.
	std::string ExpectedDriverForPack(bool custom_selected, std::string_view pack_name,
		std::string_view library_name, std::string_view description);

	/// Whether `answered` is what `expected` asked for. With the system driver selected, whatever
	/// answered is the system driver, so that always matches. A "custom" pack of unknown kind
	/// matches anything except a vendor driver, which is what a silent fallback looks like.
	bool ServedDriverMatches(std::string_view expected, std::string_view answered);

	/// The generation from a `git-axfl<G>-` tag, or 0. Same rule as the driver profile's parser.
	uint32_t ParseAxflGeneration(std::string_view driver_info);

	/// A Vulkan API version as "major.minor.patch" (with "variant N " in front if non-zero).
	std::string FormatApiVersion(uint32_t version);

	/// driverVersion decoded by the scheme its vendor uses. `scheme` names the scheme.
	std::string DecodeDriverVersion(uint32_t vendor_id, uint32_t driver_id, uint32_t version, std::string* scheme);
} // namespace GSDriverReport
