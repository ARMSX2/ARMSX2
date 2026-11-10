// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSAlphaBitLogicOp.h"
#include "GS/Renderers/Common/GSDevice.h"

#include <cmath>
#include <cstring>

/// Which draws the run machine (GS/Renderers/Vulkan/GSRunsVK.h) may merge, and what a merged draw's row says.
///
/// A run is a stretch of consecutive draws of one render pass drawn as one host draw under one pipeline. Each draw
/// keeps a row of per-draw state, and the run program (tfx_runs.glsl) does what the draw's classic program would have
/// done for the state set accepted here and nothing else: no palette, no shader bilinear, no region clamp, a 32-bit
/// colour target, alpha fail KEEP, the texture functions, fog, an alpha test, and the blend family (Cs - 0) * C + 0
/// with the rest in the blend unit. How the classic path chose to blend becomes one of four ways under the run's one
/// dual-source pipeline (ONE, INV_SRC1_COLOR / ONE, ZERO):
///   0  the shader blend, C = As, second output As
///   1  the shader blend, C = Af (the blend constant, or the second output where the driver ignores the constant)
///   2  blending elided, every channel written                (second output 1: the target's term is 0)
///   3  blending elided, alpha written alone                  (colour 0 and second output 0: RGB kept)
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
		Blend, ///< a blend that none of the four ways reproduces
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

	/// The states that cost a run program most when the run carries them: the hardware's lod and an alpha test (a
	/// discard). Split rule "dear" ends a run where these differ from the run's (measured in probe K1b).
	constexpr u32 Dear(u32 flags)
	{
		return ((Textured(flags) && Field(flags, STATE_LOD) == LOD_HARDWARE) ? 1u : 0u) | ((Field(flags, STATE_ATST) != 0) ? 2u : 0u);
	}

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
		const bool rgb = tfx == 4 && Field(flags, STATE_FOG) == 0 && Field(flags, STATE_WAY) == 2;
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
	///   run_px_ps     what a run program costs per pixel of fill over the classic programs of its draws (the price of
	///                 merging at all; a pass whose host draws saved are worth less is declined, PassPays)
	/// A run ends where a draw's dear class differs from the run's when the second price, over the run's fill so far and
	/// the draw's, exceeds the first (SplitAtDear). Rows marked measured were fitted from that GPU's storm passes; the
	/// others are the nearest measured class's until measured.
	struct DeviceClass
	{
		const char* name;
		const char* match; ///< a substring of VkPhysicalDeviceProperties::deviceName
		float host_draw_ns;
		float dear_px_ps;
		float run_px_ps;
		const char* source;
	};

	// Adreno 650, Turnip (Retroid Flip 2, 2x): a host draw from the no-fill arm (4.65 ms classic floor against 0.54 ms for
	// 171 host draws, 4,496 fewer), 0.9 us; the dear program from probe K1b (Spider-Man 3 heavy's storm pass 4.83 ms
	// without the dear split against 4.08 with it, 151 more host draws, over 23.5 M pixels of draw rectangles), 38 ps.
	// Mali-G615 (2x) and G57 (1x), malisx2: the Mali probe's storm passes (results/mali), the same fit.
	inline constexpr DeviceClass DEVICE_CLASSES[] = {
		{"Adreno 650", "650", 900.0f, 38.0f, 43.0f, "measured, Flip 2 (m1, probe K1b)"},
		{"Adreno 613", "613", 900.0f, 38.0f, 43.0f, "the 650's, not measured"},
		{"Adreno 740", "740", 900.0f, 38.0f, 43.0f, "the 650's, not measured"},
		{"Mali-G615", "G615", 200.0f, 76.0f, 60.0f, "from the Mali probe (storm passes at 2x), not verified on a device here"},
		{"Mali-G57", "G57", 2300.0f, 300.0f, 60.0f, "from the Mali probe (storm passes at 1x), not verified on a device here"},
	};
	inline constexpr DeviceClass DEVICE_CLASS_DEFAULT = {"other", "", 900.0f, 38.0f, 43.0f, "the 650's, not measured"};

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

	/// Whether a pass's runs paid: the host draws they saved (draws merged less runs) against what their programs cost
	/// over the classic ones on the runs' fill. A pass that did not pay is declined on later frames (GSRunsVK).
	inline bool PassPays(const DeviceClass& dc, u64 merged, u64 runs, u64 run_area)
	{
		const double saved = static_cast<double>(merged > runs ? merged - runs : 0) * dc.host_draw_ns * 1000.0;
		return saved >= static_cast<double>(dc.run_px_ps) * static_cast<double>(run_area);
	}

	struct Draw
	{
		Refusal why = Refusal::None;
		u32 flags = 0;
		u32 way = 0;
	};

	/// The PS selector fields a run's draws may set: the row's flags, the run's compile-time switches (rta_correction,
	/// zfloor), and the blend fields the way is read from. Any other bit set refuses the draw.
	inline const GSHWDrawConfig::PSSelector& AllowedPS()
	{
		static const GSHWDrawConfig::PSSelector mask = [] {
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
		}();
		return mask;
	}

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
		return -1;
	}

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
		// Triangles, or sprites the classic path expands in its vertex program (GSRunsVK expands them on the CPU).
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
			((way == 1 || way == 5) && !ByteExact(config.cb_ps.TA_MaxDepth_Af.w * 128.0f)))
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

	/// Which states a run holds one value of: those become constants of its program.
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

		/// The program key, four bits a state: 0xF where the state is read from the rows, else its one value. States
		/// in read_mask are read from the rows regardless (a development switch).
		u32 Key(u32 read_mask = 0) const
		{
			u32 key = 0;
			for (u32 i = 0; i < NUM_STATES; i++)
			{
				const u32 v = ((vary | read_mask) >> i & 1) ? 0xF : ((seen >> i & 1) ? value[i] : 0);
				key |= v << (4 * i);
			}
			return key;
		}
	};
} // namespace GSRunsPolicy
