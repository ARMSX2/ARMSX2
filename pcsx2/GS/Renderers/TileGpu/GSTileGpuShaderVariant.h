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
	///
	/// `vs_clip` is the scissor road: 1 writes the four clip planes, 0 leaves the whole block out.
	/// It has to be a compile-time define and not a value the shader writes zeros into -- a vertex
	/// module that merely DECLARES ClipDistance is a module a driver without shaderClipDistance may
	/// refuse, whatever the values are.
	inline std::string DeviceDefines(bool tex, bool static_byte_sel, bool bindless, bool vs_clip = true)
	{
		return fmt::format("#define TILEGPU_TEX {}\n"
						   "#define TILEGPU_STATIC_BYTE_SEL {}\n"
						   "#define TILEGPU_TEX_TARGETS {}\n"
						   "#define TILEGPU_MAX_TEX_SOURCES {}\n"
						   "#define TILEGPU_TEX_SOURCES {}\n"
						   "#define TILEGPU_MAX_SOURCES {}\n"
						   "#define TILEGPU_VS_CLIP {}\n",
			tex ? 1 : 0, static_byte_sel ? 1 : 0, bindless ? 1 : 0,
			GSDevice::GSTileGpuPassPlan::kMaxTexSourcesPerPass, bindless ? 1 : 0,
			GSDevice::GSTileGpuPassPlan::kMaxSources, vs_clip ? 1 : 0);
	}

	/// The per-pass half: which texel roads and which of the byte road's decode arms this module
	/// carries. Everything the mask does not name is not in the SPIR-V at all — on a tiler an
	/// instruction that never executes still costs program size, and on the Adreno 650 crossing one
	/// size threshold swings the whole frame.
	/// The per-DRAW half: the GS state every draw this program serves carries, frozen so the fragment
	/// stage neither loads it nor branches on it. -1 in a field means "read the state row", which is
	/// what an unspecialized program and the pass-union fallback both get.
	///
	/// Each one deletes its own state-row load (a cat5 `isam` on a650, unhoistable because `v_row` is
	/// a flat per-primitive varying) along with the compares and selects it fed and the delay slots
	/// the scheduler could not fill around them — and, past a point, the live state that holds the
	/// textured roads above the eight-register wave-size threshold. TILEGPU_SPEC_TEX is not keyed:
	/// the road mask already says whether this program samples, so the device derives it.
	inline std::string SpecDefines(const GSDevice::GSTileGpuFragmentSpec& spec)
	{
		if (!spec.valid)
			return std::string();
		return fmt::format("#define TILEGPU_SPEC_TEX 1\n"
						   "#define TILEGPU_SPEC_FST {}\n"
						   "#define TILEGPU_SPEC_LTF {}\n"
						   "#define TILEGPU_SPEC_TFX {}\n"
						   "#define TILEGPU_SPEC_TCC {}\n"
						   "#define TILEGPU_SPEC_ATST {}\n"
						   "#define TILEGPU_SPEC_FGE {}\n"
						   "#define TILEGPU_SPEC_DATE {}\n"
						   "#define TILEGPU_SPEC_WMS {}\n"
						   "#define TILEGPU_SPEC_WMT {}\n"
						   "#define TILEGPU_SPEC_TEXA {}\n",
			spec.fst, spec.ltf, spec.tfx, spec.tcc, spec.atst, spec.fge, spec.date, spec.wms, spec.wmt,
			spec.texa_frozen ? static_cast<int>(spec.texa) : -1);
	}

	/// A name for one frozen-state set, for the compile log. Empty when nothing is frozen.
	inline std::string SpecName(const GSDevice::GSTileGpuFragmentSpec& spec)
	{
		if (!spec.valid)
			return std::string();
		static constexpr const char* kTfx[4] = {"modulate", "decal", "highlight", "highlight2"};
		static constexpr const char* kWrap[4] = {"repeat", "clamp", "rgnclamp", "rgnrepeat"};
		static constexpr const char* kAtst[9] = {
			"-", "?", "?", "less", "leq", "eq", "geq", "gt", "neq"};
		return fmt::format(" spec{{{} {} {}{} atst={} {}{}wrap={}/{} texa={}}}", spec.fst ? "uv" : "stq",
			spec.ltf ? "linear" : "nearest", kTfx[spec.tfx & 3], spec.tcc ? "+tcc" : "",
			kAtst[(spec.atst < 9) ? spec.atst : 0], spec.fge ? "fog " : "",
			(spec.date != 0) ? ((spec.date == 2) ? "date1 " : "date0 ") : "", kWrap[spec.wms & 3],
			kWrap[spec.wmt & 3], spec.texa_frozen ? std::to_string(spec.texa) : std::string("row"));
	}

	inline std::string VariantDefines(u32 road_mask, u32 texel_mask, u32 self_mask = 0, bool quantise = false)
	{
		return fmt::format("#define TILEGPU_ROAD_BYTE {}\n"
						   "#define TILEGPU_ROAD_TARGET {}\n"
						   "#define TILEGPU_ROAD_SOURCE {}\n"
						   "#define TILEGPU_FMT_D32 {}\n"
						   "#define TILEGPU_FMT_IDX8 {}\n"
						   "#define TILEGPU_FMT_IDX4 {}\n"
						   "#define TILEGPU_FMT_IDXHI {}\n"
						   "#define TILEGPU_FMT_D16 {}\n"
						   "#define TILEGPU_FMT_PALGATHER {}\n"
						   "#define TILEGPU_SELF_DATE {}\n"
						   "#define TILEGPU_SELF_BLEND {}\n"
						   "#define TILEGPU_SELF_MASK {}\n"
						   "#define TILEGPU_QUANT16 {}\n",
			(road_mask & GSDevice::kGSTileGpuRoadByte) ? 1 : 0,
			(road_mask & GSDevice::kGSTileGpuRoadTarget) ? 1 : 0,
			(road_mask & GSDevice::kGSTileGpuRoadSource) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelDirect32) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelIndex8) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelIndex4) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelIndexHi) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelDirect16) ? 1 : 0,
			(texel_mask & GSDevice::kGSTileGpuTexelPalGather) ? 1 : 0,
			(self_mask & GSDevice::kGSTileGpuSelfDate) ? 1 : 0,
			(self_mask & GSDevice::kGSTileGpuSelfBlend) ? 1 : 0,
			(self_mask & GSDevice::kGSTileGpuSelfMask) ? 1 : 0, quantise ? 1 : 0);
	}

	/// A name for one variant, for the compile log and the budget gate's table.
	inline std::string VariantName(u32 road_mask, u32 texel_mask, u32 self_mask = 0, bool quantise = false)
	{
		static constexpr const char* kRoad[8] = {"untextured", "byte", "target", "byte+target", "source",
			"byte+source", "target+source", "byte+target+source"};
		std::string name = kRoad[road_mask & 7u];
		if ((road_mask & GSDevice::kGSTileGpuRoadByte) != 0)
		{
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
		}
		if (self_mask != 0)
		{
			static constexpr const char* kSelf[GSDevice::kGSTileGpuSelfUses] = {"date", "blend", "mask"};
			name += " self{";
			bool first = true;
			for (u32 u = 0; u < GSDevice::kGSTileGpuSelfUses; u++)
			{
				if ((self_mask & (1u << u)) == 0)
					continue;
				if (!first)
					name += '+';
				name += kSelf[u];
				first = false;
			}
			name += '}';
		}
		if (quantise)
			name += " q16";
		return name;
	}
} // namespace GSTileGpuShaderVariant
