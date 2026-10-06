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

	void NoteActiveVulkanDriver(std::string_view driver_info)
	{
		s_active_driver.store(IsMaliSX2Driver(driver_info) ? ActiveVulkanDriver::MaliSX2 : ActiveVulkanDriver::Other,
			std::memory_order_release);
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
