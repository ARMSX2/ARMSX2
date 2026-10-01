// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The framebuffer-fetch decisions, OpenGL's and then Vulkan's (DecideVulkanFramebufferFetch at
// the bottom), as pure functions.
//
// The GL decision lives in one place so that a driver blocklist or the user's setting cannot be
// overridden by a later check that tests the raw extension instead of the decision. All inputs
// explicit, no GL types, constexpr so the cases below are pinned at compile time and again in
// gs_framebuffer_fetch_policy_tests.cpp.

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

// `driver_blocklisted` is the driver-bug database's UseRenderTargetCopyForFeedback workaround,
// which no GL rule sets today. `mali_profile` is the runtime GPU profile, which is
// what tfx_fs.glsl keys its backend selection off, not the extension set.
constexpr GSFramebufferFetchDecision DecideGLFramebufferFetch(bool has_arm_fetch, bool has_ext_fetch,
	bool has_pls_fetch, bool driver_blocklisted, bool user_disabled, bool mali_profile)
{
	GSFramebufferFetchDecision decision;

	// Demotion depends on the extensions alone. A blocklist or the user's setting turns fetch off
	// but the Mali profile keeps its own tuning and takes the copy blend path; demoting there would
	// swap in PowerVR's tuning as a side effect of a correctness gate.
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

	// Mirrors `#if GPU_PROFILE_MALI` in tfx_fs.glsl: Mali reads through gl_LastFragColorARM even
	// when EXT is advertised, because the EXT inout path is broken on Mali drivers; everything else
	// prefers EXT/PLS and falls back to the ARM builtin only when EXT is absent.
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
// This is per extension, not per API.
//
// ARM_shader_framebuffer_fetch's spec requires a sample covered by several primitives to be
// rendered in submission order, and a read of gl_LastFragColorARM to wait for earlier fragments on
// that pixel. That is the contract of Vulkan's rasterization-order attachment access and Metal's
// programmable blending, so the ARM path gets the same barrier-free treatment.
//
// EXT_shader_framebuffer_fetch gives no such guarantee: on Mesa (Apple M2) its reads were
// nondeterministic between identical replays, an ordering failure. So EXT keeps its barrier.
//
// A driver that violates the ARM guarantee is a driver bug and belongs in the driver-bug database
// as a fetch blocklist entry (see UseRenderTargetCopyForFeedback), not in a rule here.
constexpr bool FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend backend)
{
	return backend == GSFramebufferFetchBackend::ARM;
}
static_assert(FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::ARM));
static_assert(!FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::EXT));
static_assert(!FbFetchOrdersOverlappingPrims(GSFramebufferFetchBackend::None));

// Which shape the OpenGL backend's blend fallback takes when it has no texture barrier.
//
// With multidraw_fb_copy set, the backend copies the render target once per primitive group
// inside a full-barrier draw, giving per-primitive blend ordering. With it clear, GSRendererHW sees
// no feedback loop, drops require_full_barrier, and the backend takes one render-target copy per
// draw, the shape Vulkan, D3D12 and Metal use.
//
// The per-primitive copy is affordable on an immediate-mode GPU. On a tiler each readback flushes
// and resolves the tile to memory, so a draw with hundreds of primitive groups pays hundreds of
// full-screen flushes (well under 1 fps on Mali), so tilers take the per-draw copy.
//
// `tile_based_gpu` is the caller's detection. The OpenGL backend uses GLES as the proxy, which
// also classifies ANGLE as a tiler; that is fine, since ANGLE's per-primitive copy is no cheaper.
constexpr bool GLUsesPerPrimitiveFbCopy(bool has_texture_barrier, bool tile_based_gpu)
{
	// With a barrier the copy path is never entered. Report off anyway: call sites read the flag as
	// "copies are happening".
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

// A blocklisted driver or the user's setting must survive the Mali profile, and must not demote
// the profile to PowerVR.
static_assert(!DecideGLFramebufferFetch(true, true, true, true, false, true).enabled);
static_assert(!DecideGLFramebufferFetch(true, true, true, true, false, true).demote_mali_to_powervr);
static_assert(!DecideGLFramebufferFetch(true, true, true, false, true, true).enabled);
static_assert(!DecideGLFramebufferFetch(true, true, true, false, true, true).demote_mali_to_powervr);
static_assert(DecideGLFramebufferFetch(true, true, true, false, false, true).enabled);
static_assert(DecideGLFramebufferFetch(true, true, true, false, false, true).backend ==
			  GSFramebufferFetchBackend::ARM);
static_assert(DecideGLFramebufferFetch(false, true, true, false, false, true).demote_mali_to_powervr);

// ---------------------------------------------------------------------------------------------
// The Vulkan spelling: rasterization-order attachment access, read in tile memory through
// subpassLoad.
//
// The shape is a deny list, deliberately (see the call site). Every device advertising the
// extension gets the fast path unless something is known to be wrong with its read:
//
//   * Samsung Xclipse, which has no working ROAA fetch at all;
//   * the Adreno 8xx proprietary blob, which returns stale reads above Basic blending;
//   * the parts the driver-bug database marks BrokenRoaaDestinationRead (MediaTek Mali,
//     Mali-G57), which return zero or stale destination colour.
//
// Only the third is liftable, and only on Mali, via EmuCore/GS/ForceMaliFramebufferFetch for a
// user whose driver has been fixed. The first two are hardware facts; no setting reaches them.
struct GSVulkanFramebufferFetchInputs
{
	/// VK_EXT_rasterization_order_attachment_access is present. Without it there is no in-tile read.
	bool roaa_available = false;

	/// EmuCore/GS/DisableFramebufferFetch: returns to the copy path from any state.
	bool user_disabled = false;

	bool is_mali = false;
	bool is_adreno = false;

	/// Samsung Xclipse (Exynos, AMD RDNA2). No working ROAA fetch; a hard gate.
	bool is_xclipse = false;

	/// Adreno 8xx on the Qualcomm proprietary blob, which returns stale ROAA reads above Basic
	/// blending. Not seen on 6xx/7xx or Turnip, so this combination only, not the vendor.
	bool is_adreno8xx_proprietary = false;

	/// DriverBug::BrokenRoaaDestinationRead from the driver-bug database.
	bool broken_destination_read = false;

	/// EmuCore/GS/ForceMaliFramebufferFetch as set, not filtered by vendor, so the function can
	/// report that it was ignored.
	bool force_mali_fetch_key = false;

	/// Trust the extension on any vendor not denied below. True on Android builds, which makes the
	/// vendor terms a deny list there and keeps PowerVR and other unnamed vendors off the
	/// per-primitive barrier path. Elsewhere only Mali and Adreno are trusted.
	bool any_vendor_trusted = false;
};

struct GSVulkanFramebufferFetchDecision
{
	bool enabled = false;

	/// The Mali force key was set on a non-Mali GPU and did nothing. Reported so the caller can log
	/// it once.
	bool force_key_ignored = false;
};

// The force key is Mali-only. Ungated, it would lift a BrokenRoaaDestinationRead deny on an
// Adreno part and put it on an in-tile road it cannot take. Gated here once rather than at each
// site that reads it. On Mali it lifts that deny. DisableFramebufferFetch is the way back for
// everyone.
constexpr GSVulkanFramebufferFetchDecision DecideVulkanFramebufferFetch(
	const GSVulkanFramebufferFetchInputs& in)
{
	GSVulkanFramebufferFetchDecision decision;
	decision.force_key_ignored = in.force_mali_fetch_key && !in.is_mali;

	const bool force_applies = in.force_mali_fetch_key && in.is_mali;
	const bool denied_destination_read = in.broken_destination_read && !force_applies;

	const bool vendor_allows =
		!denied_destination_read && !in.is_xclipse && !in.is_adreno8xx_proprietary &&
		(in.is_mali || in.is_adreno || in.any_vendor_trusted);

	decision.enabled = vendor_allows && in.roaa_available && !in.user_disabled;
	return decision;
}

// Mali with the extension, not denied, no force key: the in-tile read is on.
static_assert(DecideVulkanFramebufferFetch({.roaa_available = true, .is_mali = true}).enabled);
// Denied by the database, and the key lifting that deny.
static_assert(!DecideVulkanFramebufferFetch(
	{.roaa_available = true, .is_mali = true, .broken_destination_read = true})
				  .enabled);
static_assert(DecideVulkanFramebufferFetch({.roaa_available = true, .is_mali = true,
	.broken_destination_read = true, .force_mali_fetch_key = true})
				  .enabled);

// The Mali key cannot force an Adreno part onto the in-tile road, and reports that it was ignored.
static_assert(!DecideVulkanFramebufferFetch({.roaa_available = true, .is_adreno = true,
	.broken_destination_read = true, .force_mali_fetch_key = true})
				   .enabled);
static_assert(DecideVulkanFramebufferFetch({.roaa_available = true, .is_adreno = true,
	.broken_destination_read = true, .force_mali_fetch_key = true})
				  .force_key_ignored);
// An Adreno part the database does not deny keeps its fetch.
static_assert(DecideVulkanFramebufferFetch(
	{.roaa_available = true, .is_adreno = true, .force_mali_fetch_key = true})
				  .enabled);
// And on Mali the key is not "ignored", whether or not there was anything to lift.
static_assert(!DecideVulkanFramebufferFetch(
	{.roaa_available = true, .is_mali = true, .force_mali_fetch_key = true})
				   .force_key_ignored);

// The hardware gates outrank the key on the vendors that have no working read at all.
static_assert(!DecideVulkanFramebufferFetch(
	{.roaa_available = true, .is_xclipse = true, .force_mali_fetch_key = true})
				   .enabled);
static_assert(!DecideVulkanFramebufferFetch({.roaa_available = true, .is_adreno = true,
	.is_adreno8xx_proprietary = true, .force_mali_fetch_key = true})
				   .enabled);

// No extension, and the user's setting: both outrank everything, on every vendor.
static_assert(!DecideVulkanFramebufferFetch({.is_mali = true}).enabled);
static_assert(!DecideVulkanFramebufferFetch(
	{.roaa_available = true, .is_mali = true, .user_disabled = true})
				   .enabled);
static_assert(!DecideVulkanFramebufferFetch({.roaa_available = true, .is_mali = true,
	.user_disabled = true, .force_mali_fetch_key = true})
				   .enabled);

// Deny list: an unnamed vendor gets the fast path on Android, which keeps PowerVR and others off the
// per-primitive barrier path.
static_assert(DecideVulkanFramebufferFetch({.roaa_available = true, .any_vendor_trusted = true}).enabled);
static_assert(!DecideVulkanFramebufferFetch({.roaa_available = true}).enabled);
