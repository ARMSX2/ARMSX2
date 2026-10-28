// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSGPUProfilePrivate.h"

namespace GpuProfileDetail
{
bool LooksLikePowerVR(std::string_view lowered_hints)
{
	// "img" is far too broad for a substring search (it appears in unrelated model/property values).
	return ContainsAny(lowered_hints, {"imagination technologies", "imgtec", "powervr"});
}

ResolvedGpuProfile ResolvePowerVRProfile(std::string_view lowered_hints)
{
	ResolvedGpuProfile resolved;
	resolved.gpu.architecture = MobileGpuArchitecture::PowerVR;
	resolved.gpu.name = "PowerVR";
	resolved.tuning = MakeMobileGsTuning(72, 6, 72, 5);

	// C-Series: the CXTP-48-1536 in Tensor G6 (Pixel 11). Its Vulkan driver reports
	// "PowerVR C-Series CXTP-48-1536 MC1". A flagship part in a 12-16 GB phone, so it gets the pool
	// the Mali-G715 / G720 it follows in the Pixel line get, not the unrecognised-part default.
	// Keyed on the CXT parts only: "c-series" would also take in the small CXM parts.
	if (ContainsAny(lowered_hints, {"cxtp", "cxt-"}))
	{
		resolved.gpu.recognized = true;
		resolved.gpu.name = "PowerVR C-Series";
		resolved.tuning = MakeMobileGsTuning(144, 10, 144, 8, true);
		return resolved;
	}

	if (ContainsAny(lowered_hints, {"b-series", "bxs", "bxt", "d-series", "dxt", "dmtp"}))
	{
		resolved.gpu.recognized = true;
		resolved.gpu.name = ContainsAny(lowered_hints, {"d-series", "dxt", "dmtp"}) ?
			"PowerVR D-Series" : "PowerVR B-Series";
		resolved.tuning = MakeMobileGsTuning(112, 8, 112, 7);
	}
	return resolved;
}
} // namespace GpuProfileDetail
