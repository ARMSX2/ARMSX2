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
} // namespace GSDriverReport
