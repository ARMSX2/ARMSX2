// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <optional>
#include <string_view>

// Which device the deviceless Null device pretends to be.
//
// `-renderer nullhw` runs the real GSRendererHW against GSDeviceNone to price the CPU half of a
// frame. That is only meaningful if the renderer makes the same CPU decisions as on the target
// device, and many of those read GSDevice::FeatureSupport. A default-constructed FeatureSupport
// matches no real device and can skew draw counts several-fold (e.g. fast_stencil_shadow off
// lets auto-flush split shadow-volume draws).
//
// Each profile is the resolved feature set of a real device, copied from its start-up banner and
// the GSGPUDriverProfile.cpp rules that produced it. A table, not a re-derivation: GSDeviceVK
// gets these from extension queries, limits and the driver-bug database, none of which exist
// here. When a device's resolved features change, edit this table;
// gs_null_device_profile_tests.cpp pins every bit.
//
// ⚠️ This reproduces the device's CPU decisions, not its GPU work. Render passes, copies,
// barriers and area still report n/a, and Draw Calls is a floor (no backend fan-out).
namespace GSNullDeviceProfile
{
	enum class Id
	{
		// Snapdragon 865 / Adreno 650 on Turnip; the default. Render-target-copy feedback road: no
		// texture barriers, no in-tile fetch, dual-source blending present.
		Sd865,
		// Dimensity 8300 / Mali-G615 MC6 (RG 477V) on ARM r44p1. In-tile road: texture barriers
		// and framebuffer fetch on, no dual-source blending, so SRC1 blends are emulated in-shader.
		// Depends on MT6897's exemption from both MediaTek/Mali rules that would remove the fetch
		// (GSGPUDriverProfile.cpp, MEASURED_SOC_MT6897).
		MaliG615,
		// FeatureSupport's own default: every bit false except dual_source_blend. Not a device;
		// kept so numbers taken before profiles existed stay reproducible.
		Blank,
	};

	inline constexpr Id kDefault = Id::Sd865;

	inline std::optional<Id> Parse(std::string_view name)
	{
		if (name == "sd865")
			return Id::Sd865;
		if (name == "mali-g615")
			return Id::MaliG615;
		if (name == "blank")
			return Id::Blank;
		return std::nullopt;
	}

	inline const char* Name(Id id)
	{
		switch (id)
		{
			case Id::Sd865:
				return "sd865";
			case Id::MaliG615:
				return "mali-g615";
			case Id::Blank:
				return "blank";
		}
		return "?";
	}

	inline const char* Description(Id id)
	{
		switch (id)
		{
			case Id::Sd865:
				return "Snapdragon 865 / Adreno 650, Turnip (RT-copy feedback road)";
			case Id::MaliG615:
				return "Dimensity 8300 / Mali-G615 MC6, ARM r44p1 (in-tile fetch road)";
			case Id::Blank:
				return "no device -- FeatureSupport defaults, the pre-profile null arm";
		}
		return "?";
	}

	/// Every profile name, for usage text and for the error on an unknown one.
	inline const char* NameList() { return "sd865, mali-g615, blank"; }

	/// The resolved FeatureSupport of the named device.
	inline GSDevice::FeatureSupport Features(Id id)
	{
		GSDevice::FeatureSupport f; // ctor: all false but dual_source_blend
		if (id == Id::Blank)
			return f;

		const bool mali = (id == Id::MaliG615);

		// --- the same on both, and on every mobile Vulkan part we ship to ---
		f.vs_expand = true; // GSConfig.DisableVertexShaderExpand is off by default
		f.primitive_id = true; // geometryShader is present on both
		f.provoking_vertex_last = true; // VK_EXT_provoking_vertex on both
		f.point_expand = true; // largePoints, range covers every upscale we run
		f.line_expand = true; // wideLines, likewise
		f.prefer_new_textures = true; // neither part resolves to the constrained mobile tuning
		f.astc_textures = true; // both; neither has BC, so dxt/bptc stay false
		f.multidraw_fb_copy = false; // GSDeviceVK sets this false unconditionally
		f.cheap_rt_feedback_read = false; // only DX11 and Metal ever set it
		f.broken_point_sampler = false;
		f.rov = false; // tilers: GSDeviceVK forces it off on Android/Linux
		f.depth_feedback = false; // EmuCore/GS HWROV defaults off
		f.aa1 = false; // EmuCore/GS HWAA1 defaults off
		f.feedback_loop_layout = false; // both advertise ROAA, which excludes the layout road
		// Neither profile is on the declared feedback loop road: the experiment key is off by
		// default, the ordering fact needs a tagged Turnip on a6xx (the SD865 profile is stock
		// Turnip), and the barrier-kept preference needs Turnip on a7xx. The same part on a
		// different driver is a different feature set: add a new row rather than editing one,
		// or numbers recorded under the existing name silently change meaning.
		f.declared_feedback_loop_orders_overlap = false;
		f.broken_mad_deinterlace = false; // that bug is Mali-G57 only

		// --- where the two roads part ---
		// The Mali keeps texture barriers (MT6897 is exempt from vk-arm-r44p1-attachment-self-read);
		// Adreno carries UseRenderTargetCopyForFeedback, which turns them off.
		f.texture_barrier = mali;
		// GSDeviceVK does framebuffer_fetch &= texture_barrier. MT6897 is also exempt from
		// vk-mediatek-mali-roaa-destination-read, so its ROAA fetch stands.
		f.framebuffer_fetch = mali;
		f.framebuffer_fetch_orders_overlap = mali;
		// texture_barrier && !is_adreno.
		f.test_and_sample_depth = mali;
		// Mali Vulkan reports dualSrcBlend false; GSRendererHW software-blends the SRC1 draws.
		f.dual_source_blend = !mali;
		// GSFastStencilShadow::DeviceQualifies needs Vulkan, dual-source blending, and a road
		// where a frame read is costly (copy, per-draw barriers or declared loop, not in-tile read).
		// The Adreno qualifies; the Mali fails both halves. This is what stops auto-flush splitting
		// the stencil counter on the Adreno.
		f.fast_stencil_shadow = !mali;
		// Mali gets the gl_FragDepth skip so DepthReplacing does not kill early-ZS.
		f.no_ps2_z_quantization = mali;
		// Both false by different routes: Adreno takes DisableStencilBuffer (Turnip < 26.2 D32S8
		// hang); Mali loses it to stencil_buffer &= !framebuffer_fetch.
		f.stencil_buffer = false;
		// vk-turnip-blend-constant-ignored: Turnip applies a CONST_COLOR factor as if the constant
		// were zero, so AFIX rides the second fragment output instead.
		f.broken_blend_constant = !mali;

		// Presentation filters (cas_sharpening, fsr1, sgsr, metalfx_spatial) stay false; they
		// cannot affect a draw, pass or texture-cache decision.
		return f;
	}
} // namespace GSNullDeviceProfile
