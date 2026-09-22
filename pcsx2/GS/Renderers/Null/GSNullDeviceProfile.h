// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <optional>
#include <string_view>

// Which device the deviceless Null device pretends to be.
//
// `-renderer nullhw` runs the real GSRendererHW against GSDeviceNone to price the CPU half of a
// frame with no GPU behind it. That only measures anything if the renderer takes the same CPU
// decisions it would take on the device the number is about, and a large number of those decisions
// read GSDevice::FeatureSupport. GSDeviceNone used to leave it default-constructed -- every bit
// false but dual_source_blend -- which is not any real device, and the difference is not small:
// with fast_stencil_shadow off, auto-flush cuts Jak II's shadow-volume alpha counter into
// one-to-three-triangle draws and the null arm reported 4,836 draws a frame at 2x where the SD865
// runs 1,479. A 3.3x error in the headline counter, produced silently.
//
// So the null device is handed a named profile: the RESOLVED feature set of a device we actually
// run on, copied from that device's own start-up banner and from the rules in
// GSGPUDriverProfile.cpp that produced it. It is a table, not a re-derivation -- GSDeviceVK reaches
// these values through extension queries, limits and the driver-bug database, none of which exist
// here, and a second implementation of that logic would drift from the first without anyone
// noticing. When a device's resolved features change, this table is edited to match, and
// gs_null_device_profile_tests.cpp pins every bit so the edit is deliberate.
//
// ⚠️ The profile is not a claim that the null arm reproduces the device's GPU work. It reproduces
// the device's CPU DECISIONS. Render passes, copies, barriers and area still report n/a, and Draw
// Calls is still the floor Lane R described (one per RenderHW call, with no backend fan-out).
namespace GSNullDeviceProfile
{
	enum class Id
	{
		// Snapdragon 865 / Adreno 650 on Turnip (Mesa 26.1.2), the primary perf target, and the
		// default. Its shape is the render-target-copy feedback road: no texture barriers, no
		// in-tile framebuffer fetch, dual-source blending present, so the alpha stencil counter
		// goes through the blend unit instead of a per-face frame read.
		Sd865,
		// Dimensity 8300 / Mali-G615 MC6 (Anbernic RG 477V) on the ARM r44p1 blob. The opposite
		// road: texture barriers on and an in-tile framebuffer fetch, no dual-source blending, so
		// SRC1 blend equations are emulated in-shader per draw and the stencil counter stays on
		// the per-face read. MT6897 is exempt from both MediaTek/Mali rules that would otherwise
		// take the fetch away (GSGPUDriverProfile.cpp, MEASURED_SOC_MT6897), which is why this is
		// the in-tile profile and the 2026-08 banner logs from the same part are not.
		MaliG615,
		// The historical GSDeviceNone: FeatureSupport's own default, every bit false except
		// dual_source_blend. Not a device. Here so that numbers taken before profiles existed
		// (Lane R's 20-dump table, C12's per-title table) stay reproducible.
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
		// Neither device declares a feedback loop, so neither claims the ordering. All three ways
		// onto that road miss the drivers these profiles name: the experiment key is off by
		// default; the driver fact that claims ordering needs a Turnip build carrying the a6xx
		// feedback-loop fix, which the SD865 profile's stock ROCKNIX Turnip is not; and the fact
		// that takes the road with barriers kept needs Turnip on an Adreno 7xx, which an a650 and
		// a Mali-G615 are not. A row for an a7xx part, or for the same part on our driver pack, is
		// a NEW row -- the same part on a different driver is a different resolved feature set,
		// and editing one of these would silently restate every number taken under its name.
		f.declared_feedback_loop_orders_overlap = false;
		f.broken_mad_deinterlace = false; // that bug is Mali-G57 only

		// --- where the two roads part ---
		// The RG 477V keeps its texture barriers (MT6897 is exempt from vk-arm-r44p1-attachment-
		// self-read); every Adreno part carries UseRenderTargetCopyForFeedback, which turns them
		// off and makes each frame read a render-pass break plus a copy of the target.
		f.texture_barrier = mali;
		// The in-tile read follows the barriers: GSDeviceVK does framebuffer_fetch &= texture_barrier.
		// MT6897 is also exempt from vk-mediatek-mali-roaa-destination-read, so its ROAA fetch stands.
		f.framebuffer_fetch = mali;
		f.framebuffer_fetch_orders_overlap = mali;
		// texture_barrier && !is_adreno.
		f.test_and_sample_depth = mali;
		// Mali Vulkan stacks report dualSrcBlend false; GSRendererHW then software-blends the
		// specific draws that need SRC1 instead of raising the global blending level.
		f.dual_source_blend = !mali;
		// GSFastStencilShadow::DeviceQualifies: Vulkan, dual-source blending, and a road on which
		// a frame read costs the renderer its cheap path -- the copy road, the backend's own
		// per-draw barriers, or a declared feedback loop, but not the in-tile read. The Adreno is
		// on the copy road, so it qualifies; the Mali is on the in-tile read and reports no
		// dual-source blend, so it fails on both halves. Neither declares a feedback loop --
		// these profiles model the shipped road, and the arm is off. True on the Adreno road
		// only, which is what stops auto-flush splitting the counter.
		//
		// Neither answer moved when the rule took the barrier-ordered road (upscale-unify C33):
		// no modelled part is on it. The sentence above is what changed, not the value below.
		f.fast_stencil_shadow = !mali;
		// Mali gets the gl_FragDepth skip so DepthReplacing does not kill early-ZS.
		f.no_ps2_z_quantization = mali;
		// Turnip below Mesa 26.2 wedges on D32S8 + EARLY_Z_LATE_Z + discard, so the Adreno road
		// takes DisableStencilBuffer; the Mali road loses it to stencil_buffer &= !framebuffer_fetch.
		// Both end at false, by different routes.
		f.stencil_buffer = false;
		// vk-turnip-blend-constant-ignored: Turnip applies a CONST_COLOR factor as if the constant
		// were zero, so AFIX rides the second fragment output instead.
		f.broken_blend_constant = !mali;

		// Post-processing and upscaler bits (cas_sharpening, fsr1, sgsr, metalfx_spatial) stay
		// false. They pick a presentation filter and cannot reach a draw, a pass or a texture-cache
		// decision, and nothing a replay measures runs them.
		return f;
	}
} // namespace GSNullDeviceProfile
