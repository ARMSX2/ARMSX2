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
	/// The GS scissor is not keyed here at all: it is a vkCmdSetScissor per indirect call on every
	/// device, so nothing about it reaches the shader.
	///
	/// `scalarize_vec_and` is the second shader-compiler workaround, beside `static_byte_sel`: the Mali
	/// proprietary compiler miscompiles a bitwise AND on vector operands, so the fragment stage does
	/// those a component at a time. It is the ScalarizeVectorBitwiseAnd answer out of the driver-bug
	/// database -- the same one the classic renderer's tfx.glsl reads -- and it defaults OFF so a
	/// caller that has not been taught to pass it emits the program every other driver already gets.
	///
	/// `clut_merge` and `clut_merge_pages` are the entries here that are SETTINGS rather than
	/// capabilities (TileGpuClutMergeRegions and TileGpuClutMergePages), and they are here rather
	/// than in the per-draw state row because dead code is not free on this hardware: the merged
	/// palette arm costs 304 SPIR-V words in the widest paletted variants, and the per-draw stride
	/// the page merge needs on top of it costs another 72 -- together most of a unit of Adreno 650
	/// instruction length, against a budget with one to spare. A lever that ships OFF must not
	/// enlarge the program every device runs, and the two levers pay separately for the same reason:
	/// a session that merges squares but not pages keeps the margin the page stride would spend.
	///
	/// Read ONCE, when the session's module source is assembled, so a mid-session settings flip
	/// changes nothing until the device is rebuilt; the renderer asks the device which arms are
	/// actually compiled before it emits a draw that needs one, rather than assuming the two read the
	/// same setting at the same moment. `clut_merge_pages` without `clut_merge` is not a shape --
	/// the page road is the square road with a wider tile -- so the caller must not ask for it.
	inline std::string DeviceDefines(bool tex, bool static_byte_sel, bool bindless, bool dual_src = true,
		bool scalarize_vec_and = false, bool clut_merge = false, bool clut_merge_pages = false)
	{
		return fmt::format("#define TILEGPU_TEX {}\n"
						   "#define TILEGPU_STATIC_BYTE_SEL {}\n"
						   "#define TILEGPU_TEX_TARGETS {}\n"
						   "#define TILEGPU_MAX_TEX_SOURCES {}\n"
						   "#define TILEGPU_TEX_SOURCES {}\n"
						   "#define TILEGPU_MAX_SOURCES {}\n"
						   "#define TILEGPU_DUAL_SRC {}\n"
						   "#define TILEGPU_SCALARIZE_VECTOR_AND {}\n"
						   "#define TILEGPU_CLUT_MERGE {}\n"
						   "#define TILEGPU_CLUT_MERGE_PAGES {}\n",
			tex ? 1 : 0, static_byte_sel ? 1 : 0, bindless ? 1 : 0,
			GSDevice::GSTileGpuPassPlan::kMaxTexSourcesPerPass, bindless ? 1 : 0,
			GSDevice::GSTileGpuPassPlan::kMaxSources, dual_src ? 1 : 0, scalarize_vec_and ? 1 : 0,
			clut_merge ? 1 : 0, (clut_merge && clut_merge_pages) ? 1 : 0);
	}

	/// How many 32-bit words tilegpu.glsl's own `struct StateRow` declares, or 0 if the declaration is
	/// not there or names a type this cannot size.
	///
	/// The shader is read from disk at runtime, so the file the device compiles is not necessarily the
	/// file this binary was built beside -- a staged resource tree that did not get re-pushed is a
	/// shader from another revision, and nothing else notices. A row-size disagreement is the worst
	/// shape that can take: the executor's own gate checks the PLAN's stride against the binary, which
	/// still agrees, while the shader indexes `state_rows[]` at a different stride and reads every row
	/// but the first from the wrong place. Every field lands somewhere plausible, no compile fails, no
	/// counter moves, and the frame is wrong. So the shader's copy is counted and compared.
	///
	/// Deliberately a word count and not a byte count: what has to match is std430's array stride over
	/// these members, every one of which is 4-byte-aligned, so counting words needs no alignment model.
	inline u32 StateRowWordsIn(const std::string& source)
	{
		const size_t decl = source.find("struct StateRow");
		if (decl == std::string::npos)
			return 0;
		const size_t open = source.find('{', decl);
		if (open == std::string::npos)
			return 0;
		const size_t close = source.find('}', open);
		if (close == std::string::npos)
			return 0;

		// Comments out first: every member here carries one, and a `;` inside a comment would split a
		// declaration in half.
		std::string body;
		body.reserve(close - open);
		for (size_t i = open + 1; i < close;)
		{
			if (source.compare(i, 2, "//") == 0)
			{
				i = source.find('\n', i);
				if (i == std::string::npos)
					break;
				continue;
			}
			if (source.compare(i, 2, "/*") == 0)
			{
				const size_t end = source.find("*/", i + 2);
				if (end == std::string::npos)
					break;
				i = end + 2;
				continue;
			}
			body += source[i++];
		}

		u32 words = 0;
		for (size_t at = 0; at < body.size();)
		{
			const size_t semi = body.find(';', at);
			if (semi == std::string::npos)
				break;
			std::string decl_text = body.substr(at, semi - at);
			at = semi + 1;
			for (char& c : decl_text)
			{
				if (c == ',' || c == '\n' || c == '\t' || c == '\r')
					c = ' ';
			}
			// "<type> <name> <name> ..." after the comma flattening; empty between the last member and
			// the closing brace.
			const size_t tstart = decl_text.find_first_not_of(' ');
			if (tstart == std::string::npos)
				continue;
			const size_t tend = decl_text.find(' ', tstart);
			if (tend == std::string::npos)
				return 0; // a type with no name is not a member declaration this understands
			const std::string type = decl_text.substr(tstart, tend - tstart);
			u32 per_name = 0;
			if (type == "float" || type == "int" || type == "uint" || type == "bool")
				per_name = 1;
			else if (type == "vec2" || type == "ivec2" || type == "uvec2")
				per_name = 2;
			else if (type == "vec3" || type == "ivec3" || type == "uvec3")
				per_name = 3;
			else if (type == "vec4" || type == "ivec4" || type == "uvec4")
				per_name = 4;
			else
				return 0; // an aggregate or a matrix: std430 alignment is no longer a word count
			u32 names = 0;
			for (size_t n = tend; n < decl_text.size();)
			{
				const size_t nstart = decl_text.find_first_not_of(' ', n);
				if (nstart == std::string::npos)
					break;
				const size_t nend = decl_text.find(' ', nstart);
				const size_t len = ((nend == std::string::npos) ? decl_text.size() : nend) - nstart;
				// An array member's stride is its own alignment question, not a word count.
				if (decl_text.find('[', nstart) < nstart + len)
					return 0;
				names++;
				n = nstart + len;
			}
			words += per_name * names;
		}
		return words;
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
						   "#define TILEGPU_FMT_PALGATHER16 {}\n"
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
			(texel_mask & GSDevice::kGSTileGpuTexelPalGather16) ? 1 : 0,
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
				"D32", "IDX8", "IDX4", "IDXHI", "D16", "PALGATHER", "PALGATHER16"};
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
