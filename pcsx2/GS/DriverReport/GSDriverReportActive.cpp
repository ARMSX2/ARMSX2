// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/DriverReport/GSDriverReportActive.h"
#include "GS/DriverReport/GSDriverReportClassify.h"

#include <atomic>

namespace GSDriverReport
{
	namespace
	{
		std::atomic<ActiveVulkanDriver> s_active_driver{ActiveVulkanDriver::None};
	} // namespace

	void NoteActiveVulkanDriver(uint32_t driver_id, std::string_view driver_info)
	{
		ActiveVulkanDriver driver = ActiveVulkanDriver::Other;
		if (IsMaliSX2Driver(driver_info))
			driver = ActiveVulkanDriver::MaliSX2;
		else if (driver_id == DriverIdValue::QualcommProprietary)
			driver = ActiveVulkanDriver::QualcommProprietary;
		s_active_driver.store(driver, std::memory_order_release);
	}

	void ClearActiveVulkanDriver()
	{
		s_active_driver.store(ActiveVulkanDriver::None, std::memory_order_release);
	}

	ActiveVulkanDriver GetActiveVulkanDriver()
	{
		return s_active_driver.load(std::memory_order_acquire);
	}

	std::string MaliSX2NoticeText(MaliSX2Notice notice, std::string_view installed_pack)
	{
		static constexpr const char* where = "Settings > Renderer > GPU Driver";
		switch (notice)
		{
			case MaliSX2Notice::Get:
				return std::string("malisx2 is not installed. Get it in ") + where + ".";
			case MaliSX2Notice::OtherPackSelected:
				return std::string("Incompatible driver selected. Get malisx2 in ") + where + ".";
			case MaliSX2Notice::InstalledNotSelected:
				return std::string(installed_pack.empty() ? "malisx2" : installed_pack) +
				       " is installed but not selected. Switch to it in " + where + ".";
			case MaliSX2Notice::None:
			default:
				return {};
		}
	}
} // namespace GSDriverReport
