// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "common/Pcsx2Defs.h"

namespace Vulkan
{
	// VK_KHR_calibrated_timestamps and VK_EXT_calibrated_timestamps are one API under two names. The
	// entry points differ only in their suffix and take the same structures and time-domain values.
	// Some drivers offer only the KHR name (malisx2), older ones only the EXT name.
	enum class CalibratedTimestampsName : u8
	{
		Unsupported,
		KHR,
		EXT,
	};

	// The name to enable: KHR when the device offers it, EXT otherwise. has_extension(name) says
	// whether the device offers an extension; EXT is not asked about once KHR is found.
	template <typename HasExtension>
	CalibratedTimestampsName PickCalibratedTimestampsName(HasExtension&& has_extension)
	{
		if (has_extension(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
			return CalibratedTimestampsName::KHR;
		if (has_extension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
			return CalibratedTimestampsName::EXT;
		return CalibratedTimestampsName::Unsupported;
	}
} // namespace Vulkan
