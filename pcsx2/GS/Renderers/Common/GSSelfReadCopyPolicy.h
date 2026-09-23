// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Whether a draw that samples the render target it is also writing has to read from a COPY of
// that target instead of from the live attachment.
//
// GSRendererHW::HandleTextureHazards resolves such a draw one of three ways:
//
//   1. tex_is_fb -- the read lands on the pixel the fragment is writing (the destination read).
//      This is what an in-tile read (rasterization-order attachment access,
//      GL_ARM_shader_framebuffer_fetch, Metal programmable blending) serves.
//   2. a barrier -- a real texture sample of the live attachment, ordered by a pipeline barrier.
//   3. a copy -- the source is snapshotted first and the draw samples the snapshot. Always
//      correct, and the most expensive.
//
// Two roads skip the copy while reading the target at a location the draw is not writing: the
// disjoint-rect shortcut (write and read rects do not intersect, so one barrier and no copy), and
// a channel shuffle whose source page differs from its destination page (samples the live target
// at gl_FragCoord + ChannelShuffleOffset). On a framebuffer-fetch device both end with neither a
// copy nor a barrier: draw_rt_clone only copies when there is no texture barrier, and
// FbFetchDropsDrawBarriers drops the barrier on the reasoning that fetch replaces the destination
// read, which does not cover a read of a different pixel. The sample then reads a tiled
// attachment whose recent writes have not resolved, and the result depends on driver scheduling.
// Keeping the barrier would not help: it is VK_DEPENDENCY_BY_REGION_BIT, which is framebuffer-
// local and wrong for a read of a different location on a tiler.
//
// So on a device whose destination read is in-tile, an offset read takes the copy. The shuffle's
// copy must be coordinate-identity so its shader offset still finds the source; that is the copy
// draw_rt_clone already makes.
//
// The declared attachment feedback loop (GSSelfReadRoadPolicy.h) is not a tiled read: declaring
// the loop makes Turnip run the pass untiled, so the one barrier an offset read asks for
// (GSRendererHW::DetermineBarriers keeps it) orders earlier draws' writes. That road takes no
// copy; cloning would cost a copy and a render-pass break per draw.
//
// Pure function of device facts so the no-change cases can be pinned without the device. See
// gs_self_read_copy_policy_tests.cpp.

struct GSSelfReadCopyInputs
{
	/// The read lands on the pixel the fragment is writing (tex_is_fb). The read framebuffer fetch
	/// exists to serve; this policy must never touch it.
	///
	/// A channel shuffle reaching the offset road has this false: a same-page shuffle is resolved
	/// as tex_is_fb before either offset road is reached.
	bool same_pixel_read = false;

	/// The backend reads the destination in tile memory (rasterization-order attachment access,
	/// GL_ARM_shader_framebuffer_fetch, Metal programmable blending). Every such device is a
	/// tiler, and none can serve a read of a different pixel from it.
	bool framebuffer_fetch = false;

	/// The backend has a texture barrier, so GSRendererHW may resolve a hazard with one and the
	/// backend does not clone the target itself. False is the RT-copy road, which already copies.
	bool texture_barrier = false;

	/// The backend reaches the attachment through the attachment-feedback-loop image layout and an
	/// ordinary sampler rather than an in-tile read. Mutually exclusive with framebuffer_fetch on
	/// Vulkan and absent elsewhere; carried explicitly so the rule does not rest on that.
	bool feedback_loop_layout = false;

	/// The backend declares an attachment feedback loop and the driver orders overlapping
	/// primitives within the draw (see GSSelfReadRoadPolicy.h). That ordering is per pixel, which
	/// is enough: both offset roads only reach the barrier when nothing in this draw writes what it
	/// samples, so the only ordering needed is against earlier draws, and the declared pass is
	/// untiled so the barrier is a real one. DetermineBarriers keeps it on this road.
	bool declared_feedback_loop_orders_overlap = false;

};

// Returns true when this draw's self-read must be served from a copy of the target.
//
// False means "leave the existing roads alone", not that the draw needs no synchronisation.
constexpr bool SelfReadNeedsSourceCopy(const GSSelfReadCopyInputs& in)
{
	// The destination read: fetch serves it, a copy would undo the point of fetch.
	if (in.same_pixel_read)
		return false;

	// The in-tile read cannot serve a different pixel, and its barrier is by-region. Checked first
	// so no other bit can release it.
	if (in.framebuffer_fetch && in.texture_barrier && !in.feedback_loop_layout)
		return true;

	// Declared feedback loop: untiled pass, barrier kept, so no clone. Spelled out rather than left
	// to the feedback_loop_layout term, which this road also sets.
	if (in.declared_feedback_loop_orders_overlap)
		return false;

	// A real barrier orders the sample (desktop), or the backend already clones.
	return false;
}

// The fetch road: an offset read copies, the destination read does not.
static_assert(SelfReadNeedsSourceCopy({.framebuffer_fetch = true, .texture_barrier = true}));
static_assert(!SelfReadNeedsSourceCopy(
	{.same_pixel_read = true, .framebuffer_fetch = true, .texture_barrier = true}));

// Desktop: a texture barrier and no in-tile read.
static_assert(!SelfReadNeedsSourceCopy({.texture_barrier = true}));

// The RT-copy road already copies; the feedback-loop-layout road is not in scope.
static_assert(!SelfReadNeedsSourceCopy({.framebuffer_fetch = true}));
static_assert(!SelfReadNeedsSourceCopy(
	{.framebuffer_fetch = true, .texture_barrier = true, .feedback_loop_layout = true}));

// The channel-shuffle page-offset road gets the same answer as the disjoint-rect road.
static_assert(SelfReadNeedsSourceCopy(
	{.same_pixel_read = false, .framebuffer_fetch = true, .texture_barrier = true}));
static_assert(!SelfReadNeedsSourceCopy({.same_pixel_read = false, .texture_barrier = true}));

// The declared-feedback-loop road: nothing copies.
static_assert(!SelfReadNeedsSourceCopy({.texture_barrier = true, .feedback_loop_layout = true,
	.declared_feedback_loop_orders_overlap = true}));
// And the declared bit cannot release the in-tile read.
static_assert(SelfReadNeedsSourceCopy({.framebuffer_fetch = true, .texture_barrier = true,
	.declared_feedback_loop_orders_overlap = true}));
static_assert(!SelfReadNeedsSourceCopy({.same_pixel_read = true, .texture_barrier = true,
	.feedback_loop_layout = true, .declared_feedback_loop_orders_overlap = true}));
