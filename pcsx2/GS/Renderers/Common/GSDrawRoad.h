// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

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

/// Which draws may keep the open render pass's feedback-loop bits instead of ending the pass to
/// clear them. A reader sets the bits and a non-reader clears them, so without the carry one reader
/// between two non-readers costs two pass boundaries, and on a tiler each is a tile store and
/// reload.
enum class GSFeedbackCarry : u8
{
	/// Feedback bits are draw-local.
	None,
	/// Carried onto draws that ask for no barrier of their own. A reader asks for one and so sets
	/// its own bits and issues its barrier inside the held pass.
	NonReaders,
	/// Carried onto every draw.
	All,
};

/// The Vulkan device facts behind GSFeedbackCarry, resolved in GSDeviceVK::CheckFeatures.
struct GSFeedbackLoopCarryInputs
{
	/// Broadcom/V3D: carries unconditionally.
	bool device_always_carries = false;

	/// Mali: the fetch road's carry was measured here.
	bool device_is_measured_vendor = false;

	/// Adreno under Turnip: the driver orders the layout road's read (a declared loop untiles the
	/// pass and programs the coherent primitive mode).
	bool device_is_layout_road_vendor = false;

	/// Texture barriers are on, so a reader's own feedback barrier orders its read.
	bool barriers_order_reads = false;

	/// Apple silicon under Honeykrisp: the barrier-ordered layout carry was measured here.
	bool device_is_barrier_road_vendor = false;

	bool framebuffer_fetch = false;
	bool feedback_loop_layout = false;
};

/// Carry only where the road orders the read, and only on the device each road was measured on.
///
///   - Fetch road: a pass declared self-reading by a draw that never reads is the same pass with
///     one unused input attachment. Mali.
///   - Layout road, driver-ordered: the carried bit is also what keeps the pass's pipelines
///     legal, since every pipeline bound while the attachment is in the feedback layout must
///     declare the loop. Adreno.
///   - Layout road, barrier-ordered: the readers keep their own barriers inside the held pass.
///     Honeykrisp. Desktop drivers on the same road were never timed and keep draw-local bits.
///
/// ⚠️ Do not widen it without measuring the device: carried state has left later draws in a stale
/// feedback pass or layout and flickered on Vulkan, and an earlier vendor-scoped carry was reverted.
///
/// ⚠️ On a tiler the carry spreads a declared loop's untiling from the reading draw to the whole
/// run. That costs bandwidth, not pixels: pass count down with GPU time flat or up is the sign.
constexpr GSFeedbackCarry GSFeedbackCarryForDevice(const GSFeedbackLoopCarryInputs& in)
{
	if (in.device_always_carries)
		return GSFeedbackCarry::All;

	// The two roads are exclusive on Vulkan, so this picks a term, not a priority.
	const bool ordered = in.feedback_loop_layout ?
	                         (in.device_is_layout_road_vendor ||
	                             (in.barriers_order_reads && in.device_is_barrier_road_vendor)) :
	                         (in.device_is_measured_vendor && in.framebuffer_fetch);
	return ordered ? GSFeedbackCarry::NonReaders : GSFeedbackCarry::None;
}

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

	GSFeedbackCarry carry = GSFeedbackCarry::None;
};

/// What the renderer knows about a draw when it decides its road.
struct GSDrawRoadDraw
{
	/// The draw reads its colour target: the shader reads the destination, or it samples the
	/// target as its texture (GSHWDrawConfig::IsFeedbackLoopRT).
	bool reads_rt = false;

	/// The same for the alpha test's second pass.
	bool second_pass_reads_rt = false;

	/// The draw reads and writes its depth buffer (GSHWDrawConfig::IsFeedbackLoopDepth).
	bool reads_depth = false;

	/// The draw's texture is its attached depth buffer.
	bool samples_attached_depth = false;

	/// The draw asks for one barrier (require_one_barrier).
	bool one_barrier = false;

	/// The draw or its alpha second pass asks for any barrier.
	bool any_barrier = false;

	/// The draw or its alpha second pass writes depth.
	bool writes_depth = false;
};

/// The per-draw result, carried in GSHWDrawConfig::road. The renderer decides it once per draw;
/// the Vulkan backend applies it and derives none of it.
struct GSDrawRoad
{
	/// Read the colour attachment inside the pass: the pipeline's colour feedback-loop bit.
	bool rt_loop : 1;

	/// Read and write the depth attachment inside the pass.
	bool depth_loop : 1;

	/// Sample the attached depth buffer without writing it (read-only depth feedback).
	bool depth_read : 1;

	/// No in-pass read: copy the colour target before the draw and read the copy.
	bool clone_rt : 1;

	/// Keep the open pass's colour feedback bit on this draw, if the pass is on the same target.
	bool carry_rt : 1;

	/// Keep the open pass's depth feedback bits on this draw, if the pass is on the same depth
	/// buffer. Never onto a depth writer: a pass carrying depth feedback bits is one in which
	/// nothing wrote the depth being sampled, which is what makes the in-tile depth sample defined.
	bool carry_depth : 1;
};

constexpr GSDrawRoad GSDecideDrawRoad(const GSDrawRoadDevice& dev, const GSDrawRoadDraw& draw)
{
	GSDrawRoad road = {};
	road.rt_loop = dev.texture_barrier && draw.reads_rt;
	road.depth_loop = dev.texture_barrier && draw.reads_depth;
	road.depth_read = draw.samples_attached_depth && !road.depth_loop;
	road.clone_rt = !dev.texture_barrier && draw.one_barrier && (draw.reads_rt || draw.second_pass_reads_rt);
	road.carry_rt = dev.carry == GSFeedbackCarry::All || (dev.carry == GSFeedbackCarry::NonReaders && !draw.any_barrier);
	road.carry_depth = road.carry_rt && !draw.writes_depth;
	return road;
}

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

// The carry, by device.
static_assert(GSFeedbackCarryForDevice({.device_always_carries = true}) == GSFeedbackCarry::All);
static_assert(GSFeedbackCarryForDevice({.device_is_measured_vendor = true, .framebuffer_fetch = true}) ==
			  GSFeedbackCarry::NonReaders);
static_assert(GSFeedbackCarryForDevice({.device_is_layout_road_vendor = true, .feedback_loop_layout = true}) ==
			  GSFeedbackCarry::NonReaders);
static_assert(GSFeedbackCarryForDevice({.barriers_order_reads = true, .device_is_barrier_road_vendor = true,
				  .feedback_loop_layout = true}) == GSFeedbackCarry::NonReaders);
// Desktop on the layout road, and the copy road, keep draw-local bits.
static_assert(GSFeedbackCarryForDevice({.barriers_order_reads = true, .feedback_loop_layout = true}) ==
			  GSFeedbackCarry::None);
static_assert(GSFeedbackCarryForDevice({.device_is_layout_road_vendor = true}) == GSFeedbackCarry::None);

// A reader never inherits a carried bit, except on Broadcom; a depth writer never inherits depth bits.
static_assert(!GSDecideDrawRoad({.carry = GSFeedbackCarry::NonReaders}, {.any_barrier = true}).carry_rt);
static_assert(GSDecideDrawRoad({.carry = GSFeedbackCarry::All}, {.any_barrier = true}).carry_rt);
static_assert(!GSDecideDrawRoad({.carry = GSFeedbackCarry::All}, {.writes_depth = true}).carry_depth);

// The copy road clones for a one-barrier reader; an in-pass road never clones.
static_assert(GSDecideDrawRoad({}, {.reads_rt = true, .one_barrier = true}).clone_rt);
static_assert(!GSDecideDrawRoad({.texture_barrier = true}, {.reads_rt = true, .one_barrier = true}).clone_rt);
static_assert(GSDecideDrawRoad({.texture_barrier = true}, {.reads_rt = true, .one_barrier = true}).rt_loop);
