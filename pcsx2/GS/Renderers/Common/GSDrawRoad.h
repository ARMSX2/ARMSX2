// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The per-draw self-read decisions: how a draw that reads the render target or depth buffer it is
// also writing gets served. GSRendererHW makes them; the backend applies them.
//
// Which ways a device has is decided once, at device creation (GSSelfReadRoadPolicy.h):
//
//   copy     -- no in-pass read (texture_barrier off). The backend snapshots the target and the
//               draw samples the snapshot.
//   barrier  -- the draw samples the live attachment, ordered by our texture barriers.
//   ordered  -- something other than our barriers orders the read of the fragment's own pixel:
//               the in-tile read (framebuffer fetch), or a declared attachment feedback loop on a
//               driver known to order one (declared_loop_orders_overlap).
//
// ⚠️ Both ordered spellings order the fragment's OWN pixel only. A draw that samples its target
// somewhere else depends on earlier draws' writes, and neither spelling says anything about those.

/// The device facts the per-draw decisions read, from GSDevice::FeatureSupport.
struct GSDrawRoadDevice
{
	/// A draw may read the live attachment inside the pass. False is the copy road.
	bool texture_barrier = false;

	/// The destination read happens in tile memory: rasterization-order attachment access,
	/// GL_ARM/EXT_shader_framebuffer_fetch, or Metal programmable blending. Every such device is a
	/// tiler.
	bool framebuffer_fetch = false;

	/// The attachment is sampled through an ordinary sampler in the attachment-feedback-loop image
	/// layout rather than read in tile. Vulkan only, and exclusive with framebuffer_fetch there.
	bool feedback_loop_layout = false;

	/// Framebuffer fetch also orders overlapping primitives within one draw.
	bool fetch_orders_overlap = false;

	/// The backend declares an attachment feedback loop and the driver orders overlapping
	/// primitives within one draw. The pass is untiled, so a barrier this road keeps is a real one.
	bool declared_loop_orders_overlap = false;
};

/// An offset self-read (the draw samples its target somewhere other than the pixel it writes)
/// must read a copy of the target.
///
/// True only on the in-tile read. It cannot serve another pixel, and the barrier the offset read
/// would otherwise keep is framebuffer-local, which does not order a read of a different location
/// on a tiler. The layout road samples through an ordinary sampler and the declared road is
/// untiled, so both keep their one barrier and take no copy. Without texture barriers the backend
/// already copies.
constexpr bool GSOffsetSelfReadNeedsCopy(const GSDrawRoadDevice& dev)
{
	return dev.framebuffer_fetch && dev.texture_barrier && !dev.feedback_loop_layout;
}

/// Whether a draw may drop its texture barriers because its in-pass read is ordered without them.
///
/// Only the ordered spellings qualify. The draw keeps its barriers when:
///   - it samples the target somewhere other than the pixel it writes (see the header note);
///   - it reads depth through a texture, which neither spelling covers;
///   - its primitives may overlap and the spelling does not order overlapping primitives. A full
///     barrier is what gives per-primitive ordering there; without one a primitive can blend
///     against a destination its predecessor has not written yet.
///
/// `prims_may_overlap` must be true when overlap is unknown: "no" risks correctness, "yes" only
/// costs a split draw.
constexpr bool GSDrawDropsBarriers(const GSDrawRoadDevice& dev, bool samples_target_elsewhere,
	bool prims_may_overlap, bool needs_barriers_for_depth)
{
	if (!dev.framebuffer_fetch && !dev.declared_loop_orders_overlap)
		return false;

	if (samples_target_elsewhere || needs_barriers_for_depth)
		return false;

	return dev.fetch_orders_overlap || dev.declared_loop_orders_overlap || !prims_may_overlap;
}

// The in-tile read copies an offset read; nothing else does.
static_assert(GSOffsetSelfReadNeedsCopy({.texture_barrier = true, .framebuffer_fetch = true}));
static_assert(!GSOffsetSelfReadNeedsCopy({.texture_barrier = true}));
static_assert(!GSOffsetSelfReadNeedsCopy({.framebuffer_fetch = true}));
static_assert(!GSOffsetSelfReadNeedsCopy(
	{.texture_barrier = true, .feedback_loop_layout = true, .declared_loop_orders_overlap = true}));
// The declared bit cannot release the in-tile read.
static_assert(GSOffsetSelfReadNeedsCopy(
	{.texture_barrier = true, .framebuffer_fetch = true, .declared_loop_orders_overlap = true}));

// No ordered spelling, no drop.
static_assert(!GSDrawDropsBarriers({.texture_barrier = true}, false, false, false));
// Unordered fetch drops only where primitives cannot overlap.
static_assert(GSDrawDropsBarriers({.texture_barrier = true, .framebuffer_fetch = true}, false, false, false));
static_assert(!GSDrawDropsBarriers({.texture_barrier = true, .framebuffer_fetch = true}, false, true, false));
// Ordered fetch and the declared road drop with overlap too.
static_assert(GSDrawDropsBarriers(
	{.texture_barrier = true, .framebuffer_fetch = true, .fetch_orders_overlap = true}, false, true, false));
static_assert(GSDrawDropsBarriers(
	{.texture_barrier = true, .feedback_loop_layout = true, .declared_loop_orders_overlap = true}, false, true, false));
// An offset read and a depth read keep their barriers on every spelling.
static_assert(!GSDrawDropsBarriers(
	{.texture_barrier = true, .feedback_loop_layout = true, .declared_loop_orders_overlap = true}, true, false, false));
static_assert(!GSDrawDropsBarriers(
	{.texture_barrier = true, .framebuffer_fetch = true, .fetch_orders_overlap = true}, false, false, true));
