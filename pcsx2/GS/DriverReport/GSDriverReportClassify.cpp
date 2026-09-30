// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/DriverReport/GSDriverReportClassify.h"

#include "GS/Renderers/Common/GSGPUProfile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace GSDriverReport
{
	static std::string Lower(std::string_view s)
	{
		std::string out(s);
		std::transform(out.begin(), out.end(), out.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	static bool Contains(std::string_view haystack, std::string_view needle)
	{
		return haystack.find(needle) != std::string_view::npos;
	}

	uint32_t ParseAxflGeneration(std::string_view driver_info)
	{
		return GpuProfileDetector::ParseDeclaredLoopFixGeneration(driver_info);
	}

	ServedDriverClassification ClassifyServedDriver(const ServedDriverFacts& facts)
	{
		ServedDriverClassification out;
		out.axfl_generation = ParseAxflGeneration(facts.driver_info);

		const std::string info = Lower(facts.driver_info);
		const std::string name = Lower(facts.driver_name);

		char ids[64];
		std::snprintf(ids, sizeof(ids), "vendorID=0x%04x driverID=%u", facts.vendor_id, facts.driver_id);
		out.evidence = std::string("driverInfo=\"") + std::string(facts.driver_info) + "\" driverName=\"" +
		               std::string(facts.driver_name) + "\" deviceName=\"" + std::string(facts.device_name) + "\" " + ids;

		// libmali first: it presents itself as Arm's driver on purpose.
		if (Contains(info, "libmali"))
			out.answered = "libmali";
		else if (facts.driver_id == DriverIdValue::MesaTurnip || Contains(name, "turnip") ||
				 (facts.vendor_id == GpuVendorID::Qualcomm && Contains(info, "mesa")))
			out.answered = "turnip";
		else if (facts.driver_id == DriverIdValue::QualcommProprietary)
			out.answered = "vendor-qualcomm";
		else if (facts.driver_id == DriverIdValue::MesaPanVK || Contains(name, "panvk"))
			out.answered = "panvk";
		else if (facts.driver_id == DriverIdValue::ArmProprietary)
			out.answered = "vendor-arm";
		else if (facts.driver_id == 0 && facts.driver_info.empty() && facts.driver_name.empty())
			out.answered = "unknown";
		else if (Contains(info, "mesa"))
		{
			// Another Mesa driver (Honeykrisp, RADV, llvmpipe...): name it.
			std::string n;
			for (const char c : name)
				n.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '-');
			out.answered = "mesa-" + n;
		}
		else
			out.answered = "other";

		return out;
	}

	std::string ExpectedDriverForPack(bool custom_selected, std::string_view pack_name,
		std::string_view library_name, std::string_view description)
	{
		if (!custom_selected)
			return "system";

		const std::string text = Lower(std::string(pack_name) + " " + std::string(library_name) + " " +
									   std::string(description));
		if (Contains(text, "libmali") || Contains(text, "mali"))
			return "libmali";
		if (Contains(text, "turnip") || Contains(text, "freedreno") || Contains(text, "mesa") ||
			Contains(text, "axfl"))
			return "turnip";
		return "custom";
	}

	bool ServedDriverMatches(std::string_view expected, std::string_view answered)
	{
		if (expected == "system")
			return true;
		if (expected == "custom")
			return !answered.empty() && answered != "unknown" && answered.substr(0, 7) != "vendor-";
		return expected == answered;
	}

	std::string FormatApiVersion(uint32_t version)
	{
		const uint32_t variant = version >> 29;
		const uint32_t major = (version >> 22) & 0x7Fu;
		const uint32_t minor = (version >> 12) & 0x3FFu;
		const uint32_t patch = version & 0xFFFu;
		char buf[64];
		if (variant != 0)
			std::snprintf(buf, sizeof(buf), "variant %u %u.%u.%u", variant, major, minor, patch);
		else
			std::snprintf(buf, sizeof(buf), "%u.%u.%u", major, minor, patch);
		return buf;
	}

	std::string DecodeDriverVersion(uint32_t vendor_id, uint32_t driver_id, uint32_t version, std::string* scheme)
	{
		char buf[64];
		if (vendor_id == GpuVendorID::NVIDIA)
		{
			if (scheme)
				*scheme = "nvidia (10.8.8.6)";
			std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", version >> 22, (version >> 14) & 0xFFu,
				(version >> 6) & 0xFFu, version & 0x3Fu);
			return buf;
		}
		if (vendor_id == GpuVendorID::Intel && driver_id == 5 /* INTEL_PROPRIETARY_WINDOWS */)
		{
			if (scheme)
				*scheme = "intel-windows (18.14)";
			std::snprintf(buf, sizeof(buf), "%u.%u", version >> 14, version & 0x3FFFu);
			return buf;
		}

		// Qualcomm's blob, Mesa (Turnip, PanVK and the rest), Arm and most others use the Vulkan
		// version packing: major 10 bits, minor 10 bits, patch 12 bits. Qualcomm's "512.676.53" and
		// Mesa's "26.2.99" both read correctly this way.
		if (scheme)
		{
			if (driver_id == DriverIdValue::QualcommProprietary)
				*scheme = "qualcomm (VK_MAKE_VERSION 10.10.12)";
			else if (driver_id == DriverIdValue::MesaTurnip || driver_id == DriverIdValue::MesaPanVK)
				*scheme = "mesa (VK_MAKE_VERSION 10.10.12)";
			else
				*scheme = "VK_MAKE_VERSION 10.10.12";
		}
		std::snprintf(buf, sizeof(buf), "%u.%u.%u", version >> 22, (version >> 12) & 0x3FFu, version & 0xFFFu);
		return buf;
	}
} // namespace GSDriverReport
