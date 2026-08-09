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

// Whether a draw may drop its barrier requirement because framebuffer fetch is available.
//
// Fetch replaces the destination READ. Whether it also orders overlapping primitives WITHIN one
// draw depends on which spelling of it the backend has, and that distinction is load-bearing:
// GSRendererHW switches an overlapping draw to software blending *because* fetch is available
// ("on fbfetch, one barrier is like full barrier") and asks for a full barrier to get the
// per-primitive ordering that path needs. Dropping the barrier on the same reasoning removed the
// mechanism that supplied the ordering, so a primitive blended against a destination its
// predecessor had not written yet. It raced: on Mesa 25.3.6 / Apple M2, a 640x480 MGS3 frame came
// out 101x further from the software rasteriser than the RT-copy path (76872 pixels wrong by >=8
// against 759), and 18% of the frame changed between identical replays.
//
// Vulkan's rasterization-order attachment access and Metal's programmable blending guarantee the
// ordering by contract, so they still drop the barrier and keep their render passes intact. The
// GL fetch extensions do not deliver it in practice. Where the draw's own primitives do not
// overlap the question is moot -- a live in-tile read and a pre-draw snapshot are the same value --
// so the barrier is dropped there on every backend, which covered 76 of the 84 affected draws in
// that frame.
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
