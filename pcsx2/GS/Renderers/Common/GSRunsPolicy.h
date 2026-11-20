// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSAlphaBitLogicOp.h"
#include "GS/Renderers/Common/GSDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>

/// Which rows the run emitter (GS/Renderers/Vulkan/GSRunEmitterVK.h) may merge, and what a merged row's table entry says.
///
/// A run is a stretch of consecutive draws of one render pass drawn as one host draw under one pipeline. Each draw
/// keeps a row of per-draw state, and the run program (tfx_runs.glsl) does what the draw's classic program would have
/// done for the state set accepted here and nothing else: no palette, no shader bilinear, no region clamp, a 32-bit
/// colour target, alpha fail KEEP, the texture functions, fog, an alpha test, and the blend family (Cs - 0) * C + 0
/// with the rest in the blend unit. How the classic path chose to blend becomes one of eight ways under a dual-source
/// pipeline, (ONE, INV_SRC1_COLOR / ONE, ZERO) for ways 0 to 5 and (SRC1_COLOR, INV_SRC1_COLOR / ONE, ZERO) for 6 and 7:
///   0  the shader blend, C = As, second output As
///   1  the shader blend, C = Af (the blend constant, or the second output where the driver ignores the constant)
///   2  blending elided, every channel written                (second output 1: the target's term is 0)
///   3  blending elided, alpha written alone                  (colour 0 and second output 0: RGB kept)
///   4  additive, Cs * As in the shader, the target added     (second output 0)
///   5  additive, Cs * Af likewise
///   6  the blend unit's Cs * As + Cd * (1 - As), as Minimum blending draws it: Cs and As written unblended
///   7  the same with Af, where the classic draw sends Af through the second output (a driver that ignores the
///      blend constant). Through the constant it has no way: the blend unit does not take the constant and the second
///      output to the same number (Mali differs by up to 8 levels).
///
/// Header-only so the tests can pin it without a device.
namespace GSRunsPolicy
{
	enum class Refusal : u8
	{
		None,
		Road,  ///< the draw needs something besides one plain draw: a barrier, a second pass, DATE, colour clip, a copy
		Topo,  ///< not an indexed triangle list in ST coordinates
		PS,    ///< a pixel-shader state outside the run program's set
		Blend, ///< a blend that none of the ways reproduces
		Tex,   ///< a texture the run program cannot sample as its classic program does
		Const, ///< a fog colour or blend constant that does not fit the row's bytes
	};

	/// The texture facts the decision needs, so it does not reach into a device texture.
	struct TextureFacts
	{
		bool present = false;
		bool is_target = false; ///< a render target or depth buffer
		bool rgba8 = false;     ///< sampled as normalized RGBA8 (GSTexture::Format::Color)
	};

	/// The row's flags (tfx_runs.glsl): the per-draw states, in this order. Each is a compile-time constant of the
	/// run's program where the run holds one value of it.
	enum State : u32
	{
		STATE_TFX, STATE_TCC, STATE_FOG, STATE_IIP, STATE_ATST, STATE_WAY, STATE_LOD, STATE_RTASRC, NUM_STATES
	};
	constexpr u32 STATE_SHIFT[NUM_STATES] = {0, 3, 4, 5, 6, 9, 12, 14};
	constexpr u32 STATE_WIDTH[NUM_STATES] = {3, 1, 1, 1, 3, 3, 2, 1};
	constexpr const char* STATE_NAME[NUM_STATES] = {"TFX", "TCC", "FOG", "IIP", "ATST", "WAY", "LOD", "RTASRC"};
	/// States that mean nothing on an untextured draw (texture function 4): such a draw neither sets nor varies them.
	constexpr u32 STATES_TEXTURED_ONLY = (1u << STATE_LOD) | (1u << STATE_RTASRC);
	/// The lod field: the classic program's three sampling calls.
	constexpr u32 LOD_NONE = 0, LOD_MANUAL = 1, LOD_HARDWARE = 2;

	constexpr u32 Field(u32 flags, u32 state) { return (flags >> STATE_SHIFT[state]) & ((1u << STATE_WIDTH[state]) - 1); }
	constexpr bool Textured(u32 flags) { return Field(flags, STATE_TFX) != 4; }

	/// On a driver with DriverBug::BrokenMixedSamplingDerivatives (Turnip), a run's textured draws all take one of the
	/// three sampling calls (one lod field): a draw that samples with the hardware's lod in a run beside draws that
	/// sample another way got some pixels wrong there, and differently from one replay to the next (Spider-Man 3 at
	/// Minimum blending: its lighting; Basic on a smaller scale). One sampling call for all three, the hardware's lod
	/// read with a query, or derivatives taken before any branch, did not help. Elsewhere (malisx2, the M2) mixed runs
	/// are exact and stay. LOD_ANY: the run has no textured draw yet.
	constexpr u32 LOD_ANY = 0xFF;
	constexpr bool LodJoins(bool one_call, u32 run_lod, u32 flags)
	{
		return !one_call || !Textured(flags) || run_lod == LOD_ANY || run_lod == Field(flags, STATE_LOD);
	}

	/// The row's slot word (tfx_runs.glsl s0.y): the texture's slot in the low bits, and above it, where the run program
	/// fetches (DriverBug::StallsOnFilteredRunSampling), how the row samples: by hand from texelFetch, and with which of
	/// its sampler's address modes and filter. A row without ROW_FETCH samples through its sampler.
	constexpr u32 ROW_SLOT_MASK = 0xFFFFu;
	constexpr u32 ROW_FETCH = 1u << 16;
	constexpr u32 ROW_FETCH_REPEAT_U = 1u << 17;
	constexpr u32 ROW_FETCH_REPEAT_V = 1u << 18;
	constexpr u32 ROW_FETCH_LINEAR = 1u << 19;

	/// Where the run program fetches (DriverBug::StallsOnFilteredRunSampling): the GPU stalls when a run draw samples many
	/// textures through the filtered sampler, and a group that sampled at most four that way did not stall. So a group
	/// samples at most SAMPLER_TEXTURES distinct textures through the sampler. Past that a row either fetches, which costs
	/// GPU time for every pixel it covers, or closes the group, which costs a host draw: a row smaller than FETCH_AREA
	/// target pixels fetches, a larger one closes the group.
	constexpr u32 SAMPLER_TEXTURES = 4;
	constexpr u64 FETCH_AREA = 1024;

	/// How a textured row is placed in its group, on a device where the run program fetches.
	enum class Placement : u8
	{
		Sampler, ///< through the sampler; its texture is in the group's sampler set or joins it
		Fetch,   ///< filtered by hand from texelFetch
		Close,   ///< the group closes and the row opens the next one
	};

	/// eligible: the row could fetch (FetchBits). in_set: its texture is already sampled through the sampler in the group.
	/// set_size: textures the group samples through the sampler. area: the row's area in target pixels.
	inline Placement PlaceRow(bool eligible, bool in_set, u32 set_size, u64 area, u32 sampler_textures = SAMPLER_TEXTURES,
		u64 fetch_area = FETCH_AREA)
	{
		if (in_set || set_size < sampler_textures)
			return Placement::Sampler;
		return (eligible && area < fetch_area) ? Placement::Fetch : Placement::Close;
	}

	/// The sampling bits of a textured row whose texture has `levels` mip levels, if it can fetch; 0 where it must keep
	/// its sampler. A row can fetch when the sampler would read level 0 with one filter whatever the level of detail: the
	/// texture has one level, and the lod is fixed at 0 (the sampler magnifies) or both filters are the same. A texture
	/// with more levels keeps the sampler, which picks and blends them.
	inline u32 FetchBits(u32 flags, GSHWDrawConfig::SamplerSelector ss, int levels)
	{
		if (!Textured(flags) || levels != 1)
			return 0;
		const bool linear = ss.IsMagFilterLinear();
		if (Field(flags, STATE_LOD) != LOD_NONE && ss.IsMinFilterLinear() != linear)
			return 0;
		return ROW_FETCH | (ss.tau ? ROW_FETCH_REPEAT_U : 0u) | (ss.tav ? ROW_FETCH_REPEAT_V : 0u) |
		       (linear ? ROW_FETCH_LINEAR : 0u);
	}

	/// The states that cost a run program most when the run carries them: the hardware's lod and an alpha test (a
	/// discard). Split rule "dear" ends a run where these differ from the run's (measured in probe K1b).
	constexpr u32 Dear(u32 flags)
	{
		return ((Textured(flags) && Field(flags, STATE_LOD) == LOD_HARDWARE) ? 1u : 0u) | ((Field(flags, STATE_ATST) != 0) ? 2u : 0u);
	}

	/// The ways whose classic draw leaves the whole blend to the blend unit (Minimum blending): Cs * As + Cd * (1 - As)
	/// or the same with Af, under SRC1_COLOR, INV_SRC1_COLOR. A run holding one takes that equation, not the run's usual
	/// (ONE, INV_SRC1_COLOR); the elided ways give the same result under either, the shader-blend ways do not.
	constexpr u32 UNIT_WAYS = (1u << 6) | (1u << 7);
	constexpr u32 SHADER_WAYS = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5);
	constexpr bool UnitEquation(u32 ways) { return (ways & UNIT_WAYS) != 0; }
	constexpr bool WaysShareAnEquation(u32 ways) { return (ways & UNIT_WAYS) == 0 || (ways & SHADER_WAYS) == 0; }

	/// The states that decide a draw's colour arithmetic.
	constexpr u32 COLOUR_STATES_MASK = 0xE3Fu; // tfx, tcc, fog, iip, way

	/// Whether a channel the draw writes is its interpolated vertex colour times a constant: the colour itself
	/// (texture function 4, no fog, blending elided), or the alpha alone (texture function 4, or decal without the
	/// texture's alpha) scaled by 1/255. A driver that links the pipeline's stages may move that multiply into the
	/// vertex program (Mesa's nir_opt_varyings does, on the M2), and interpolating the scaled colour rounds differently
	/// from scaling the interpolated one: up to 5 levels at a few pixels a frame (Spider-Man 3, 2x). The driver decides
	/// on the classic program's shape, so the run program must have the same shape for such a draw: a run holding one
	/// holds one value of every colour state (Strict()). The alpha with RTA correction is scaled by 1/128, which
	/// commutes with interpolation exactly, and a flat colour is not interpolated.
	constexpr bool Passthrough(u32 flags, bool rta_correction)
	{
		if (Field(flags, STATE_IIP) == 0)
			return false;
		const u32 tfx = Field(flags, STATE_TFX);
		const u32 way = Field(flags, STATE_WAY);
		const bool rgb = tfx == 4 && Field(flags, STATE_FOG) == 0 && (way == 2 || way == 6 || way == 7);
		const bool alpha = !rta_correction && (tfx == 4 || (tfx == 1 && Field(flags, STATE_TCC) == 0));
		return rgb || alpha;
	}

	/// The part of the run key a passthrough draw adds: its colour states, or none for any other draw.
	constexpr u32 StrictKey(u32 flags, bool rta_correction)
	{
		return Passthrough(flags, rta_correction) ? (0x80000000u | (flags & COLOUR_STATES_MASK)) : 0u;
	}

	/// The split rule's constants for one class of GPU, chosen from the device's reported name (DeviceClassFor).
	///   host_draw_ns  what one more host draw costs the GPU (the price of ending a run)
	///   dear_px_ps    what a run program that carries a dear state (reads it from the rows) costs per pixel of fill over
	///                 one that holds it constant (the price of not ending the run)
	/// A run ends where a draw's dear class differs from the run's when the second price, over the run's fill so far and
	/// the draw's, exceeds the first (SplitAtDear). Rows marked measured were fitted from that GPU's storm passes; the
	/// others are the nearest measured class's until measured.
	struct DeviceClass
	{
		const char* name;
		const char* match; ///< a substring of VkPhysicalDeviceProperties::deviceName
		float host_draw_ns;
		float dear_px_ps;
		const char* source;
	};

	// Adreno 650, Turnip (Retroid Flip 2, 2x): a host draw from the no-fill arm (4.65 ms classic floor against 0.54 ms for
	// 171 host draws, 4,496 fewer), 0.9 us; the dear program from probe K1b (Spider-Man 3 heavy's storm pass 4.83 ms
	// without the dear split against 4.08 with it, 151 more host draws, over 23.5 M pixels of draw rectangles), 38 ps.
	// Mali-G615 (2x) and G57 (1x), malisx2: the Mali probe's storm passes (results/mali), the same fit.
	inline constexpr DeviceClass DEVICE_CLASSES[] = {
		{"Adreno 650", "650", 900.0f, 38.0f, "measured, Flip 2 (m1, probe K1b)"},
		{"Adreno 613", "613", 900.0f, 38.0f, "the 650's, not measured"},
		{"Adreno 740", "740", 900.0f, 38.0f, "the 650's, not measured"},
		{"Mali-G615", "G615", 200.0f, 76.0f, "measured, Mali probe (merge-spec 3.57 ms against split 1.81)"},
		{"Mali-G57", "G57", 2300.0f, 300.0f, "measured, Mali probe at 1x (spec 5.64 ms against split 4.20)"},
	};
	inline constexpr DeviceClass DEVICE_CLASS_DEFAULT = {"other", "", 900.0f, 38.0f, "the 650's, not measured"};

	inline const DeviceClass& DeviceClassFor(const char* device_name)
	{
		for (const DeviceClass& c : DEVICE_CLASSES)
		{
			if (device_name && std::strstr(device_name, c.match))
				return c;
		}
		return DEVICE_CLASS_DEFAULT;
	}

	/// Whether a draw of another dear class ends the run (true) or joins it, the run's program then reading the dear
	/// states from its rows. run_area and draw_area are draw-rectangle pixels.
	inline bool SplitAtDear(const DeviceClass& dc, u64 run_area, u64 draw_area)
	{
		return static_cast<double>(dc.dear_px_ps) * static_cast<double>(run_area + draw_area) > static_cast<double>(dc.host_draw_ns) * 1000.0;
	}

	struct Draw
	{
		Refusal why = Refusal::None;
		u32 flags = 0;
		u32 way = 0;
	};

	/// The PS selector fields a run's draws may set: the row's flags, the run's compile-time switches (rta_correction,
	/// zfloor), and the blend fields the way is read from. Any other bit set refuses the draw.
	inline GSHWDrawConfig::PSSelector MakeAllowedPS()
	{
		GSHWDrawConfig::PSSelector m;
		m.tfx = 7;
		m.tcc = 1;
		m.fog = 1;
		m.iip = 1;
		m.atst = static_cast<GSShader::PS_ATST>(7);
		m.automatic_lod = 1;
		m.manual_lod = 1;
		m.blend_a = 3;
		m.blend_b = 3;
		m.blend_c = 3;
		m.blend_d = 3;
		m.blend_mix = 3;
		m.no_color1 = 1;
		m.wms = 3;
		m.wmt = 3;
		m.rta_source_correction = 1;
		m.rta_correction = 1;
		m.zfloor = 1;
		m.af_in_src1 = 1;
		return m;
	}
	/// Built once, at namespace scope, so a draw's check reads two words with no initialisation guard.
	inline const GSHWDrawConfig::PSSelector ALLOWED_PS = MakeAllowedPS();
	inline const GSHWDrawConfig::PSSelector& AllowedPS() { return ALLOWED_PS; }

	/// The cheapest refusal, taken first: a pixel-shader bit outside the run program's set. Most refused draws of a
	/// title that forms no runs stop here, so the machine costs such a title two masks a draw.
	inline bool OutsidePS(const GSHWDrawConfig::PSSelector& ps)
	{
		return ((ps.key_lo & ~ALLOWED_PS.key_lo) | (ps.key_hi & ~ALLOWED_PS.key_hi)) != 0;
	}

	/// The vertex-selector bits Classify always refuses as a topology: one AND, taken first with OutsidePS.
	inline GSHWDrawConfig::VSSelector MakeRefusedVS()
	{
		GSHWDrawConfig::VSSelector vs;
		vs.fst = 1;
		vs.point_size = 1;
		vs.sprite_edge_clamp = 1;
		return vs;
	}
	inline const u16 REFUSED_VS = MakeRefusedVS().key;
	inline bool OutsideVS(const GSHWDrawConfig::VSSelector& vs) { return (vs.key & REFUSED_VS) != 0; }

	inline bool ByteExact(float v) { return v >= 0.0f && v <= 255.0f && v == std::floor(v); }

	/// The way the classic path's blend maps to, or -1.
	inline int Way(const GSHWDrawConfig& config)
	{
		const GSHWDrawConfig::PSSelector& ps = config.ps;
		const GSHWDrawConfig::BlendState& bs = config.blend;
		const u32 cms = config.colormask.wrgba;
		const bool family = ps.blend_a == 0 && ps.blend_b == 2 && ps.blend_d == 2 && ps.blend_mix == 1;
		const bool none = ps.blend_a == 0 && ps.blend_b == 0 && ps.blend_c == 0 && ps.blend_d == 0 && ps.blend_mix == 0;
		const bool add_one = bs.enable && bs.op == GSDevice::OP_ADD && bs.src_factor == GSDevice::CONST_ONE &&
		                     bs.src_factor_alpha == GSDevice::CONST_ONE && bs.dst_factor_alpha == GSDevice::CONST_ZERO && cms == 0xF &&
		                     family;
		if (add_one && !bs.constant_enable && bs.dst_factor == GSDevice::INV_SRC1_COLOR && ps.blend_c == 0 && !ps.no_color1 &&
			!ps.af_in_src1)
			return 0;
		if (add_one && bs.constant_enable && bs.dst_factor == GSDevice::INV_CONST_COLOR && ps.blend_c == 2 && !ps.af_in_src1 &&
			static_cast<float>(bs.constant) == config.cb_ps.TA_MaxDepth_Af.w * 128.0f)
			return 1;
		// The same blend, its factor already in the second output (a driver that ignores the blend constant).
		if (add_one && !bs.constant_enable && bs.dst_factor == GSDevice::INV_SRC1_COLOR && ps.blend_c == 2 && ps.af_in_src1 &&
			!ps.no_color1)
			return 1;
		if (!bs.enable && cms == 0xF && none && !ps.af_in_src1)
			return 2;
		if (!bs.enable && cms == 0x8 && none && !ps.af_in_src1)
			return 3;
		// Additive: (Cs - 0) * C + Cd with the product in the shader (no BLEND_MIX) and the target added by a ONE, ONE
		// blend. Under the run's equation the second output 0 adds the target.
		const bool additive = bs.enable && !bs.constant_enable && bs.op == GSDevice::OP_ADD && bs.src_factor == GSDevice::CONST_ONE &&
		                      bs.dst_factor == GSDevice::CONST_ONE && bs.src_factor_alpha == GSDevice::CONST_ONE &&
		                      bs.dst_factor_alpha == GSDevice::CONST_ZERO && cms == 0xF && ps.blend_a == 0 && ps.blend_b == 2 &&
		                      ps.blend_d == 2 && ps.blend_mix == 0 && !ps.af_in_src1;
		if (additive && ps.blend_c == 0)
			return 4;
		if (additive && ps.blend_c == 2)
			return 5;
		// The whole blend in the blend unit (Minimum blending): no blend in the shader, which writes Cs and the factor.
		// The run takes the same equation (UnitEquation), so the run program writes what the classic program writes.
		const bool unit = bs.enable && !bs.constant_enable && bs.op == GSDevice::OP_ADD && bs.src_factor == GSDevice::SRC1_COLOR &&
		                  bs.dst_factor == GSDevice::INV_SRC1_COLOR && bs.src_factor_alpha == GSDevice::CONST_ONE &&
		                  bs.dst_factor_alpha == GSDevice::CONST_ZERO && cms == 0xF && ps.blend_a == 0 && ps.blend_b == 0 &&
		                  ps.blend_d == 0 && ps.blend_mix == 0 && !ps.no_color1;
		if (unit && ps.blend_c == 0 && !ps.af_in_src1)
			return 6;
		if (unit && ps.blend_c == 2 && ps.af_in_src1)
			return 7;
		return -1;
	}

	/// Whether the way's row carries Af.
	constexpr bool WayReadsAf(u32 way) { return way == 1 || way == 5 || way == 7; }

	/// Whether the draw can join a run, and if so its row's flags and its way. colclip_live: a colour-clip target is in
	/// progress on the device (the draw then renders to it, not to its target).
	inline Draw Classify(const GSHWDrawConfig& config, const TextureFacts& tex, bool colclip_live)
	{
		Draw d;
		const GSHWDrawConfig::PSSelector& ps = config.ps;
		if (config.stencil_counter.enable || config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off ||
			config.require_one_barrier || config.require_full_barrier || config.blend_multi_pass.enable ||
			config.alpha_second_pass.enable || config.colormask.logic_op != GSAlphaBitLogicOp::Off || ps.colclip_hw || colclip_live ||
			config.road.clone_rt || config.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE || config.line_expand || ps.HasColorROV() ||
			ps.HasDepthROV() || config.road.rt_loop || config.road.depth_loop || config.road.depth_read || config.raster_order ||
			config.date_copy != GSAlphaBitLogicOp::NoDateCopy || config.colclip_native_scale > 1.0f || !config.rt)
		{
			d.why = Refusal::Road;
			return d;
		}
		// Triangles, or sprites the classic path expands in its vertex program (the emitter expands them on the CPU).
		const bool triangles = config.vs.expand == GSHWDrawConfig::VSExpand::None && config.indices_per_prim == 3;
		const bool sprites = config.vs.expand == GSHWDrawConfig::VSExpand::Sprite && config.indices_per_prim == 6;
		if (config.topology != GSHWDrawConfig::Topology::Triangle || !(triangles || sprites) || config.vs.point_size || config.vs.fst ||
			config.vs.sprite_edge_clamp)
		{
			d.why = Refusal::Topo;
			return d;
		}
		const GSHWDrawConfig::PSSelector& allowed = AllowedPS();
		if ((ps.key_lo & ~allowed.key_lo) != 0 || (ps.key_hi & ~allowed.key_hi) != 0 || ps.wms > 1 || ps.wmt > 1 ||
			static_cast<u32>(ps.atst) > 4 || config.pal)
		{
			d.why = Refusal::PS;
			return d;
		}
		const int way = Way(config);
		if (way < 0)
		{
			d.why = Refusal::Blend;
			return d;
		}
		const GSVector4 fog = config.cb_ps.FogColor_AREF;
		if ((ps.fog && (!ByteExact(fog.x) || !ByteExact(fog.y) || !ByteExact(fog.z))) ||
			(WayReadsAf(static_cast<u32>(way)) && !ByteExact(config.cb_ps.TA_MaxDepth_Af.w * 128.0f)))
		{
			d.why = Refusal::Const;
			return d;
		}
		u32 flags = ps.tfx | (ps.tcc << 3) | (ps.fog << 4) | (ps.iip << 5) | (static_cast<u32>(ps.atst) << 6) | (static_cast<u32>(way) << 9);
		if (ps.tfx != 4)
		{
			// The draw's own texture through its own sampler: any size, levels and sampler, as long as the program's
			// arithmetic (a normalized RGBA8 colour) is the texture's.
			if (!tex.present || tex.is_target || !tex.rgba8)
			{
				d.why = Refusal::Tex;
				return d;
			}
			// tfx.glsl's sample_c takes the hardware's lod before the manual one.
			const u32 lod = ps.automatic_lod ? LOD_HARDWARE : (ps.manual_lod ? LOD_MANUAL : LOD_NONE);
			flags |= (lod << STATE_SHIFT[STATE_LOD]) | (ps.rta_source_correction << STATE_SHIFT[STATE_RTASRC]);
		}
		d.flags = flags;
		d.way = static_cast<u32>(way);
		return d;
	}

	/// The rows a group must reach before it pays for itself (B): the fixed overhead of one run-program group over what
	/// each row in it saves, on the CPU thread that binds (its texture set, binds, and the classic path binding all of
	/// its state again afterwards) and on the GPU (the run program's own cost, which is per group), each priced apart:
	/// B = max(ceil(cpu_group_ns / cpu_saved_row_ns), ceil(gpu_group_ns / gpu_saved_row_ns)). A saving that is not
	/// positive while there is an overhead forbids groups (NEVER). A price of zero is not measured and takes no part.
	constexpr u32 NEVER = 0xFFFFFFFFu;
	inline u32 BreakEven(float cpu_group_ns, float cpu_saved_row_ns, float gpu_group_ns, float gpu_saved_row_ns)
	{
		const auto rows = [](float group, float saved) -> u32 {
			if (group <= 0.0f)
				return 1;
			if (saved <= 0.0f)
				return NEVER;
			return std::max(1u, static_cast<u32>(std::ceil(group / saved)));
		};
		return std::max(rows(cpu_group_ns, cpu_saved_row_ns), rows(gpu_group_ns, gpu_saved_row_ns));
	}

	/// The key of a row's streak class (Streaks): a hash of the fixed state one group holds one value of. vs_fixed: the
	/// vertex selector without the bits a group's rows may differ in. rt_size, ds_size: the targets' sizes, which stand
	/// for the targets: their host addresses differ from one run to the next, and the class decides which rows open
	/// groups, so it must be the same for the same draws on every run. Targets of one size share classes; a collision
	/// only changes when a group opens.
	inline u64 StreakClass(const GSHWDrawConfig& config, u16 vs_fixed, GSVector2i rt_size, GSVector2i ds_size)
	{
		u64 w[9];
		w[0] = static_cast<u64>(static_cast<u32>(rt_size.x)) | (static_cast<u64>(static_cast<u32>(rt_size.y)) << 32);
		w[1] = static_cast<u64>(static_cast<u32>(ds_size.x)) | (static_cast<u64>(static_cast<u32>(ds_size.y)) << 32);
		w[2] = static_cast<u64>(config.depth.key) | (static_cast<u64>(vs_fixed) << 8) |
		       (static_cast<u64>(config.ps.rta_correction) << 24) | (static_cast<u64>(config.ps.zfloor) << 25) |
		       (static_cast<u64>(config.cb_vs.max_depth) << 32);
		std::memcpy(&w[3], &config.scissor, 16);
		std::memcpy(&w[5], &config.cb_vs.vertex_scale, 8);
		std::memcpy(&w[6], &config.cb_vs.vertex_offset, 8);
		std::memcpy(&w[7], &config.cb_vs.texture_offset, 8);
		std::memcpy(&w[8], &config.cb_ps.STScale, 8);
		u64 h = 0x9E3779B97F4A7C15ull;
		for (u64 v : w)
		{
			h ^= v;
			h *= 0xBF58476D1CE4E5B9ull;
			h ^= h >> 31;
		}
		return h;
	}

	/// The break-even length B in rows (see Streaks), one value for every device class until each class's CPU overhead
	/// is measured.
	constexpr u32 INTERIM_OPENING_ROWS = 4;

	/// Per side of the emitter (bases, held twins), the streak of consecutive rows of one fixed-state class that could
	/// share a run-program group. A group opens at the streak's L-th row; the rows before it stay classic and nothing is
	/// rewritten. Each class's next streak is predicted from its own last two, as the longer of them: streaks repeat
	/// per class (Sly 3's long ones stay long and its short ones alternate 2 and 4; Call of Duty 3's stay at 2 or 3), so a
	/// class predicted to reach the break-even length B groups the whole streak, L = 1, and any other class opens only if
	/// its streak reaches B after all, L = B. L is also 1 for a row of the class whose group just ended at a split (lod,
	/// dear, the texture cap, the table window, a ring, or a recording). A class drawn classically still learns: what is
	/// remembered is each streak's length, not each group's. Classes are keyed by a hash of their fixed state: a
	/// collision only changes when a group opens, never which rows share one.
	class Streaks
	{
	public:
		static constexpr u32 SIDES = 2;
		static constexpr u32 TABLE = 1024;

		/// A row of class `cls` on `side` that could join a group: whether a group may be open for it now.
		bool Note(u32 side, u64 cls, u32 B)
		{
			Side& s = m_side[side];
			if (s.count == 0 || s.cls != cls)
			{
				End(side);
				s.cls = cls;
			}
			const Recent& last = m_last[side][cls % TABLE];
			const u32 L = (s.split || std::max(last.a, last.b) >= B) ? 1u : B;
			s.split = false;
			s.count++;
			return s.count >= L;
		}
		/// The side's group of its current class ended at a split: the class's next row opens a group at once.
		void Split(u32 side)
		{
			if (m_side[side].count != 0)
				m_side[side].split = true;
		}
		/// A row on `side` that could not join any group: the side's streak ends.
		void Break(u32 side) { End(side); }
		/// The streak length a class last reached on a side (for tests).
		u32 Last(u32 side, u64 cls) const { return m_last[side][cls % TABLE].a; }

	private:
		struct Side
		{
			u64 cls = 0;
			u32 count = 0;
			bool split = false;
		};
		void End(u32 side)
		{
			Side& s = m_side[side];
			if (s.count != 0)
			{
				Recent& last = m_last[side][s.cls % TABLE];
				last.b = last.a;
				last.a = static_cast<u16>(std::min<u32>(s.count, 0xFFFFu));
			}
			s.count = 0;
			s.split = false;
		}
		struct Recent
		{
			u16 a = 0; ///< the last streak's length
			u16 b = 0; ///< the one before
		};
		Side m_side[SIDES];
		Recent m_last[SIDES][TABLE] = {};
	};

	struct StateSet
	{
		u32 seen = 0;
		u32 vary = 0;
		u32 value[NUM_STATES] = {};

		void Note(u32 flags)
		{
			for (u32 i = 0; i < NUM_STATES; i++)
			{
				if ((STATES_TEXTURED_ONLY >> i & 1) && !Textured(flags))
					continue;
				const u32 v = Field(flags, i);
				if (!(seen >> i & 1))
				{
					seen |= 1u << i;
					value[i] = v;
				}
				else if (value[i] != v)
					vary |= 1u << i;
			}
		}

		/// The program key, four bits a state: 0xF where the state is read from the rows, else its one value.
		u32 Key() const
		{
			u32 key = 0;
			for (u32 i = 0; i < NUM_STATES; i++)
			{
				const u32 v = (vary >> i & 1) ? 0xF : ((seen >> i & 1) ? value[i] : 0);
				key |= v << (4 * i);
			}
			return key;
		}
	};
} // namespace GSRunsPolicy
