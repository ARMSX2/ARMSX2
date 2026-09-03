// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Whether a draw that samples the render target it is also writing has to read from a COPY of
// that target instead of from the live attachment.
//
// GSRendererHW::HandleTextureHazards resolves such a draw one of three ways:
//
//   1. tex_is_fb -- the read lands on the pixel the fragment is writing. That is the destination
//      read, and it is what an in-tile read (Vulkan rasterization-order attachment access,
//      GL_ARM_shader_framebuffer_fetch, Metal programmable blending) is FOR.
//   2. a barrier -- the read is a real texture sample of the live attachment, ordered against the
//      draw's own writes by a pipeline barrier.
//   3. a copy -- the source is snapshotted into its own texture first, the render pass ends, and
//      the draw samples the snapshot. Always correct, and the most expensive.
//
// Road 2 has a shortcut: when the rect the draw writes and the rect it reads do not intersect,
// HandleTextureHazards asks for one barrier and skips the copy. On a device with framebuffer
// fetch that shortcut lands the draw with neither. The copy is gated on the backend having no
// texture barrier (GSDeviceVK's draw_rt_clone), which fetch devices do have; and the barrier is
// then dropped by FbFetchDropsDrawBarriers, whose reasoning -- "fetch replaces the destination
// read" -- is about road 1 and says nothing about a sample of a DIFFERENT pixel.
//
// Nothing is left. The draw samples the live colour attachment while it is the pass's colour
// attachment, on a tiler, where the recent writes to the region being read are in tile memory and
// have not resolved. Which of them have resolved by the time the sample executes is the driver's
// scheduling, not program order. Measured on an Anbernic RG 477V (Mali-G615 r44p1) with fetch
// forced on: MGS3 and OutRun 2006 render differently every run, and they are the only two titles
// in the 22-dump corpus that read their own render target at an offset (census
// devs/bmdhacks/campaigns/gs-classic-tiler/phase2-0-fetch-ini/NONDETERMINISM-CENSUS.md).
//
// So: on a device whose destination read is in-tile, an offset read of the target takes the copy.
//
// Note the barrier would not have been enough anyway. The one this road asks for is
// VK_DEPENDENCY_BY_REGION_BIT, and so is the render pass's self-dependency; by-region is
// framebuffer-local, which is the wrong dependency for a read of a different location on any
// tiler. It survives on desktop because immediate-mode drivers over-synchronise it. That is why
// this rule is "take the copy", not "keep the barrier".
//
// Written as a pure function of the device's facts so the no-change cases can be pinned without
// the device that takes the changed one. See gs_self_read_copy_policy_tests.cpp.

struct GSSelfReadCopyInputs
{
	/// The read lands on the pixel the fragment is writing (tex_is_fb). This is the read
	/// framebuffer fetch exists to serve, and the one road this policy must never touch --
	/// it is where step 2.0's speed win lives.
	bool same_pixel_read = false;

	/// The backend reads the destination in tile memory: Vulkan rasterization-order attachment
	/// access, GL_ARM_shader_framebuffer_fetch, Metal programmable blending. Every device that
	/// has one is a tiler, and none of them can serve a read of a different pixel from it.
	bool framebuffer_fetch = false;

	/// The backend has a texture barrier, so GSRendererHW is allowed to resolve a hazard with one
	/// and the backend does not clone the target behind its back. False is the RT-copy road
	/// (Adreno under ARMSX2 #442, Mali at its default, iOS Metal), which already copies -- there
	/// is nothing there to fix and nothing to change.
	bool texture_barrier = false;

	/// The backend reaches the attachment through the attachment-feedback-loop image layout and an
	/// ordinary sampler rather than through an in-tile read. Mutually exclusive with
	/// framebuffer_fetch on Vulkan (UseFeedbackLoopLayout tests for the ABSENCE of the
	/// rasterization-order extension that framebuffer_fetch requires), and absent on the other
	/// backends. Carried explicitly so the rule stays tied to the road it was reasoned about
	/// rather than resting on that coincidence.
	bool feedback_loop_layout = false;

	/// EmuCore/GS/FetchOffsetReadCopies. Scaffolding for the device round; false is exactly the
	/// behaviour every device had before this policy existed.
	bool copy_key = false;
};

// Returns true when this draw's self-read must be served from a copy of the target.
//
// False means "leave the existing roads alone" -- it is NOT a claim that the draw needs no
// synchronisation, only that whatever HandleTextureHazards was going to do is unchanged.
constexpr bool SelfReadNeedsSourceCopy(const GSSelfReadCopyInputs& in)
{
	if (!in.copy_key)
		return false;

	// The destination read. Fetch serves it, tex_is_fb expresses it, and forcing a copy here
	// would undo the entire reason the fetch path exists.
	if (in.same_pixel_read)
		return false;

	// No in-tile read: either a real barrier orders the sample (desktop), or the backend is
	// already cloning the target for it. Both are roads this policy does not touch.
	if (!in.framebuffer_fetch || in.feedback_loop_layout)
		return false;

	if (!in.texture_barrier)
		return false;

	return true;
}

// The fetch road: an offset read copies, the destination read does not.
static_assert(SelfReadNeedsSourceCopy({.framebuffer_fetch = true, .texture_barrier = true, .copy_key = true}));
static_assert(!SelfReadNeedsSourceCopy(
	{.same_pixel_read = true, .framebuffer_fetch = true, .texture_barrier = true, .copy_key = true}));

// Desktop: a texture barrier and no in-tile read. The existing shortcut stands.
static_assert(!SelfReadNeedsSourceCopy({.texture_barrier = true, .copy_key = true}));

// The RT-copy road already copies; the feedback-loop-layout road is not the one reasoned about.
static_assert(!SelfReadNeedsSourceCopy({.framebuffer_fetch = true, .copy_key = true}));
static_assert(!SelfReadNeedsSourceCopy(
	{.framebuffer_fetch = true, .texture_barrier = true, .feedback_loop_layout = true, .copy_key = true}));

// The key off is the old behaviour, everywhere.
static_assert(!SelfReadNeedsSourceCopy({.framebuffer_fetch = true, .texture_barrier = true}));
