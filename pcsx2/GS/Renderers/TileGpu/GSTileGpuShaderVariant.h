// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include "common/Pcsx2Defs.h"

#include "fmt/format.h"

#include <string>

/// The #define block that turns tilegpu.glsl into one particular fragment program.
///
/// It lives here rather than inside the Vulkan device because two things have to agree about it and
/// only one of them is a device: the executor, which compiles a module per pass, and the size gate
/// (gs_tilegpu_shader_budget_tests), which compiles every variant the corpus plans and fails the
/// suite if one crosses the Adreno 650 instruction-size budget. A gate that assembled its own
/// defines would be measuring a program nothing runs, which is the failure it exists to catch.
namespace GSTileGpuShaderVariant
{
	/// The device-capability half: what this GPU can serve at all. Constant for a session.
	/// `tex` is "the page swizzle tables fitted closed forms", `bindless` is "a sampled-image array
	/// can be indexed by a value the shader computes" (rules 2 and 3 both ride it), and
	/// `static_byte_sel` is the Honeykrisp workaround for a computed sub-word shift.
	inline std::string DeviceDefines(bool tex, bool static_byte_sel, bool bindless)
	{
		return fmt::format("#define TILEGPU_TEX {}\n"
						   "#define TILEGPU_STATIC_BYTE_SEL {}\n"
						   "#define TILEGPU_TEX_TARGETS {}\n"
						   "#define TILEGPU_MAX_TEX_SOURCES {}\n"
						   "#define TILEGPU_TEX_SOURCES {}\n"
						   "#define TILEGPU_MAX_SOURCES {}\n",
			tex ? 1 : 0, static_byte_sel ? 1 : 0, bindless ? 1 : 0,
			GSDevice::GSTileGpuPassPlan::kMaxTexSourcesPerPass, bindless ? 1 : 0,
			GSDevice::GSTileGpuPassPlan::kMaxSources);
	}

	/// The per-pass half: which texel roads and which of the byte road's decode arms this module
	/// carries. Everything the mask does not name is not in the SPIR-V at all — on a tiler an
	/// instruction that never executes still costs program size, and on the Adreno 650 crossing one
	/// size threshold swings the whole frame.
	inline std::string VariantDefines(u32 road_mask, u32 texel_mask)
	{
		return fmt::format("#define TILEGPU_ROAD_BYTE {}\n"
						   "#define TILEGPU_ROAD_TARGET {}\n"
						   "#define TILEGPU_ROAD_SOURCE {}\n"
						   "#define TILEGPU_FMT_D32 {}\n"
						   "#define TILEGPU_FMT_IDX8 {}\n"
						   "#define TILEGPU_FMT_IDX4 {}\n"
						   "#define TILEGPU_FMT_IDXHI {}\n"
						   "#define TILEGPU_FMT_D16 {}\n"
						   "#define TILEGPU_FMT_PALGATHER {}\n",
			(road_mask & GSDevice::kGSTileGpuRoadByte) ? 1 : 0,
			(road_mask & GSDevice::kGSTileGpuRoadTarget) ? 1 : 0,
			(road_mask & GSDevice::kGSTileGpuRoadSource) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelDirect32) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelIndex8) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelIndex4) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelIndexHi) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelDirect16) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelPalGather) ? 1 : 0);
	}

	/// A name for one variant, for the compile log and the budget gate's table.
	inline std::string VariantName(u32 road_mask, u32 texel_mask)
	{
		static constexpr const char* kRoad[8] = {"untextured", "byte", "target", "byte+target", "source",
			"byte+source", "target+source", "byte+target+source"};
		std::string name = kRoad[road_mask & 7u];
		if ((road_mask & GSDevice::kGSTileGpuRoadByte) == 0)
			return name;
		static constexpr const char* kArm[GSDevice::kGSTileGpuTexelArms] = {
			"D32", "IDX8", "IDX4", "IDXHI", "D16", "PALGATHER"};
		name += '[';
		bool first = true;
		for (u32 a = 0; a < GSDevice::kGSTileGpuTexelArms; a++)
		{
			if ((texel_mask & (1u << a)) == 0)
				continue;
			if (!first)
				name += '+';
			name += kArm[a];
			first = false;
		}
		if (first)
			name += '-';
		name += ']';
		return name;
	}
} // namespace GSTileGpuShaderVariant
