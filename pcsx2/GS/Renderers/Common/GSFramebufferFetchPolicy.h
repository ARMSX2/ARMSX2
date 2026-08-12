// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The OpenGL framebuffer-fetch decision, as one pure function.
//
// It used to be made imperatively in three places roughly a hundred lines apart in
// GSDeviceOGL::CheckFeatures, and the last of them -- the Mali profile block -- tested the raw
// GL_ARM_shader_framebuffer_fetch extension instead of the decision the first two had already
// made. So on a Mali r44p1 device the driver guard turned fetch off and the profile block turned
// it straight back on, 0.1 ms apart in the same log; DisableFramebufferFetch was eaten the same
// way, leaving no way to turn fetch off on Mali GL from settings at all. The bug was not subtle
// -- the log states the contradiction in plain language -- it survived because there was no
// single place a reader or a test could look at to see what the decision was.
//
// Hence: one function, all inputs explicit, no GL types, constexpr so the cases below are pinned
// at compile time and again by name in gs_framebuffer_fetch_policy_tests.cpp.

enum class GSFramebufferFetchBackend
{
	None,
	ARM, // gl_LastFragColorARM (GL_ARM_shader_framebuffer_fetch)
	EXT, // `inout` colour output (GL_EXT_shader_framebuffer_fetch / pixel local storage)
};

// Why fetch is off. Carried out of the policy so the caller can log the specific reason and raise
// the OSD message for the one case that is a user setting rather than a hardware fact.
enum class GSFramebufferFetchVeto
{
	None,
	NoExtension, // neither ARM nor EXT fetch is advertised
	DriverBlocklist, // a driver build known to render it incorrectly
	UserSetting, // GSConfig.DisableFramebufferFetch
};

struct GSFramebufferFetchDecision
{
	bool enabled = false;
	GSFramebufferFetchBackend backend = GSFramebufferFetchBackend::None;
	GSFramebufferFetchVeto veto = GSFramebufferFetchVeto::NoExtension;

	// A Mali profile that cannot reach the ARM shader path has to move to the PowerVR profile,
	// which shares the EXT/PLS arm with the catch-all default.
	bool demote_mali_to_powervr = false;
};

// `driver_blocklisted` is the caller's driver-version test (currently Mali r44p1, which loses the
// GL context under the in-tile blend path exactly as it loses the Vulkan device under
// attachment-feedback-loop). `mali_profile` is the runtime GPU profile, which is what tfx_fs.glsl
// keys its backend selection off -- not the extension set.
constexpr GSFramebufferFetchDecision DecideGLFramebufferFetch(bool has_arm_fetch, bool has_ext_fetch,
	bool has_pls_fetch, bool driver_blocklisted, bool user_disabled, bool mali_profile)
{
	GSFramebufferFetchDecision decision;

	// Demotion is a property of the EXTENSIONS alone. A driver blocklist or the user's setting
	// turns fetch off, and turning fetch off is not a reason to move the device to a different
	// profile: the Mali profile still wants its own texture-preference and shader tuning, it
	// just takes the non-fetch (copy) blend path like any GPU without the extension. Demoting
	// there would silently swap in PowerVR's tuning as a side effect of a correctness gate.
	decision.demote_mali_to_powervr = mali_profile && !has_arm_fetch;
	const bool effective_mali_profile = mali_profile && !decision.demote_mali_to_powervr;

	if (!has_arm_fetch && !has_ext_fetch)
		decision.veto = GSFramebufferFetchVeto::NoExtension;
	else if (driver_blocklisted)
		decision.veto = GSFramebufferFetchVeto::DriverBlocklist;
	else if (user_disabled)
		decision.veto = GSFramebufferFetchVeto::UserSetting;
	else
		decision.veto = GSFramebufferFetchVeto::None;

	decision.enabled = (decision.veto == GSFramebufferFetchVeto::None);

	// Mirrors the `#if GPU_PROFILE_MALI` selection in tfx_fs.glsl: Mali reads back through
	// gl_LastFragColorARM even when EXT is also advertised, because the EXT inout path is broken
	// on every Mali driver tested; everything else prefers the EXT/PLS inout output and falls
	// back to the ARM builtin only when EXT is absent.
	if (!decision.enabled)
		decision.backend = GSFramebufferFetchBackend::None;
	else if (effective_mali_profile && has_arm_fetch)
		decision.backend = GSFramebufferFetchBackend::ARM;
	else if (has_ext_fetch || has_pls_fetch)
		decision.backend = GSFramebufferFetchBackend::EXT;
	else
		decision.backend = GSFramebufferFetchBackend::ARM;

	return decision;
}

// Whether a GL framebuffer-fetch backend also orders overlapping primitives within one draw.
//
// This is a per-EXTENSION property, not a per-API one, and conflating the two cost every Mali
// device a large amount of performance in 2.6.6.5.
//
// ARM_shader_framebuffer_fetch is the coherent spelling. Its spec is explicit: "when an individual
// sample is covered by multiple primitives, rendering for that sample is performed sequentially in
// the order in which the primitives were submitted", and a read of gl_LastFragColorARM "must wait
// for the processing of all previous fragments destined for the current pixel to complete". That
// is the same contract Vulkan's rasterization-order attachment access and Metal's programmable
// blending provide, so the ARM path earns the same barrier-free treatment they get. It is also
// what the hardware does anyway -- a tiler blends in tile memory in primitive order.
//
// EXT_shader_framebuffer_fetch does not earn it. Measured on Mesa 25.3.6 / Apple M2 through the
// EXT path, a 640x480 MGS3 frame came out 101x further from the software rasteriser than the
// RT-copy path (76872 pixels wrong by >=8 against 759), and 18% of the frame changed between
// identical replays -- the nondeterminism is what identified it as an ordering failure rather than
// an arithmetic one. So EXT keeps its barrier.
//
// ⚠️ The measurement above was taken on ONE driver through the EXT extension, and the conclusion
// drawn from it was applied to all of GL, including ARM's extension, which guarantees the opposite.
// A driver that violates the ARM guarantee is a driver bug and belongs in the driver-bug database
// as a fetch blocklist entry (see UseRenderTargetCopyForFeedback), not in a blanket rule here --
// that is the mechanism r44p1 already uses.
constexpr bool FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend backend)
{
	return backend == GSFramebufferFetchBackend::ARM;
}
static_assert(FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::ARM));
static_assert(!FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::EXT));
static_assert(!FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::None));

// Whether a draw may drop its barrier requirement because framebuffer fetch is available.
//
// Fetch replaces the destination READ. Whether it also orders overlapping primitives WITHIN one
// draw depends on which spelling of it the backend has, and that distinction is load-bearing:
// GSRendererHW switches an overlapping draw to software blending *because* fetch is available
// ("on fbfetch, one barrier is like full barrier") and asks for a full barrier to get the
// per-primitive ordering that path needs. Dropping the barrier on the same reasoning removed the
// mechanism that supplied the ordering, so a primitive blended against a destination its
// predecessor had not written yet.
//
// Vulkan's rasterization-order attachment access, Metal's programmable blending and GL's ARM
// extension all guarantee the ordering by contract, so they drop the barrier and keep their render
// passes intact; the GL EXT path does not (see FbFetchOrdersOverlappingPrims). Where the draw's own
// primitives do not overlap the question is moot -- a live in-tile read and a pre-draw snapshot are
// the same value -- so the barrier is dropped there on every backend, which covered 76 of the 84
// affected draws in that frame.
//
// `prims_may_overlap` must be true when overlap is merely unknown; guessing "no" is what costs
// correctness, and guessing "yes" costs only a split draw.
constexpr bool FbFetchDropsDrawBarriers(
	bool fetch_orders_overlapping_prims, bool prims_may_overlap, bool needs_barriers_for_depth)
{
	// Depth feedback reads through a texture, not the colour attachment, so fetch says nothing
	// about it and its barriers stand regardless.
	if (needs_barriers_for_depth)
		return false;

	return fetch_orders_overlapping_prims || !prims_may_overlap;
}

// Which shape the OpenGL backend's blend fallback takes when it has no texture barrier.
//
// Two different fallbacks exist and the flag picks between them. With multidraw_fb_copy set, the
// backend copies the render target once per PRIMITIVE GROUP inside a full-barrier draw, which buys
// the per-primitive blend ordering a real barrier would have given. With it clear, GSRendererHW
// sees no feedback loop at all, drops require_full_barrier, and the backend takes a single
// render-target copy per DRAW -- the same shape Vulkan, D3D12 and Metal already use, since none of
// them sets the flag.
//
// The per-primitive copy is affordable on an immediate-mode desktop GPU, where a blit is a blit. On
// a tiler it is not a copy at all: reading back the render target forces the current tile's work to
// flush and resolve to main memory, so a draw with a few hundred primitive groups pays a few
// hundred full-screen flushes. Measured on an Anbernic RG 477V (Mali-G615 r44p1) with MGS3, that is
// the difference between 0.33 fps and full speed -- the same game on the same device runs at ~30
// fps on Vulkan, which reaches the identical copy-based concept only without this flag.
//
// So the trade is not accuracy-versus-speed in any useful sense: 0.33 fps is not a mode anyone
// runs, and the per-draw fallback is what every other backend has always shipped.
//
// `tile_based_gpu` is the caller's detection. GLES is the proxy the OpenGL backend uses, which also
// classifies ANGLE as a tiler -- correct by accident, since ANGLE's per-primitive copy is no cheaper.
constexpr bool GLUsesPerPrimitiveFbCopy(bool has_texture_barrier, bool tile_based_gpu)
{
	// With a barrier in hand the copy path is never entered, so the flag is inert. Report it off
	// anyway: several call sites read it as "copies are happening", and a set-but-unused flag is
	// how a reader concludes the wrong thing about which path a device is on.
	if (has_texture_barrier)
		return false;

	return !tile_based_gpu;
}

// A tiler with no barrier takes the per-draw copy; an immediate-mode GPU keeps the per-primitive one.
static_assert(!GLUsesPerPrimitiveFbCopy(false, true));
static_assert(GLUsesPerPrimitiveFbCopy(false, false));
// A barrier means the copy path is unreachable either way.
static_assert(!GLUsesPerPrimitiveFbCopy(true, true));
static_assert(!GLUsesPerPrimitiveFbCopy(true, false));

// The regression: GL fetch does not order overlapping primitives, so an overlapping draw keeps its
// barrier. Everything else about the old unconditional drop is preserved.
static_assert(!FbFetchDropsDrawBarriers(false, true, false));
static_assert(FbFetchDropsDrawBarriers(false, false, false));
// Backends whose fetch *is* an ordering guarantee keep the barrier-free fast path, overlap or not.
static_assert(FbFetchDropsDrawBarriers(true, true, false));
static_assert(FbFetchDropsDrawBarriers(true, false, false));
// Depth feedback outranks all of it.
static_assert(!FbFetchDropsDrawBarriers(true, false, true));
static_assert(!FbFetchDropsDrawBarriers(false, false, true));

// The regression itself: a blocklisted driver, or the user's setting, must survive the Mali
// profile -- and must not drag the profile to PowerVR on the way.
static_assert(!DecideGLFramebufferFetch(true, true, true, true, false, true).enabled);
static_assert(!DecideGLFramebufferFetch(true, true, true, true, false, true).demote_mali_to_powervr);
static_assert(!DecideGLFramebufferFetch(true, true, true, false, true, true).enabled);
static_assert(!DecideGLFramebufferFetch(true, true, true, false, true, true).demote_mali_to_powervr);
static_assert(DecideGLFramebufferFetch(true, true, true, false, false, true).enabled);
static_assert(DecideGLFramebufferFetch(true, true, true, false, false, true).backend ==
			  GSFramebufferFetchBackend::ARM);
static_assert(DecideGLFramebufferFetch(false, true, true, false, false, true).demote_mali_to_powervr);
