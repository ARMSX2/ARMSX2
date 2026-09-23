// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Whether a backend may keep the feedback-loop flag set across a run of draws on one target.
//
// The backend ends a render pass whenever the feedback-loop flag word of the next draw differs
// from the one the open pass was built with (GSDeviceVK::OMSetRenderTargets). A reader sets the
// flag and a non-reader clears it, so one reader between two non-readers costs two pass
// boundaries, and on a tiler each boundary is a full tile store and reload. The carry instead:
//
//   once a target run has a reader, the flag stays set for the following non-readers on that
//   target until the pass ends for another reason (a different colour or depth target, a colclip
//   blit, a command-buffer submit).
//
// The rule is CARRY WHERE THE ROAD ORDERS THE READ:
//
//   * Fetch road. The read is a tile-local subpassLoad under rasterization-order attachment
//     access, and a pass declared self-reading by a draw that never reads is the same pass with
//     one unused input attachment. Scoped to Mali, where it was measured.
//
//   * Layout road, driver-ordered (Adreno under Turnip). The backend samples through an ordinary
//     sampler with the image in VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT. Every
//     pipeline bound in such a pass must carry the colour feedback-loop declaration, and the carry
//     supplies it, since the word it ORs onto following draws is the pipeline-selector word the
//     declaration is derived from: what keeps the pass open is what makes its pipelines legal.
//     Turnip refuses to tile a pass that declares the loop and programs the primitive mode that
//     orders the read, so the ordering comes from the driver. Without the carry, declaring the
//     loop multiplies pass counts by exactly the alternation described above. Reached when
//     GSSelfReadRoadPolicy puts a Turnip device on the declared loop; every other Adreno stays on
//     the copy road and carries nothing.
//
//   * Layout road, barrier-ordered (Apple silicon under Honeykrisp, which has no
//     rasterization-order extension). With texture barriers on, a reader asks for a feedback
//     barrier and SendHWDraw emits a framebuffer-local self-dependency inside the pass. Scoped to
//     Honeykrisp: desktop NVIDIA, AMD and Intel take the same road with the same barriers, but the
//     carry was never timed there and it changes how their passes are cut, so they keep the
//     draw-local flag.
//
// A carried non-reader needs no barrier and gets none: the barrier term below excludes every draw
// that asks for its own. A reader is excluded by the same term, so it sets its own flag word and
// SendHWDraw issues its barrier inside the pass. That barrier is legal because a pass keyed with
// color_feedback_loop declares the matching subpass self-dependency (SHADER_READ, fragment stage)
// and the carry is what keeps that key. The only change is which pass the barrier sits in.
//
// ⚠️ Declaring a feedback loop can cost tile residency on any tiler, and the cost is invisible to
// pixels. Every pipeline in a latched pass carries the create flag, and on Turnip the gmem-disable
// term is read from it ahead of the bandwidth autotuner, so the carry spreads the untiling from
// the reading draw to the whole run. A title whose passes the autotuner would tile pays
// tile-store bandwidth. Pass count down with GPU time flat or up is the signature; check the
// driver's tiled/untiled census or GPU time, not the picture or the pass count.
//
// Pure function of device facts so it can be pinned without a device. See
// gs_feedback_loop_carry_tests.cpp.

struct GSFeedbackLoopCarryInputs
{
	/// Devices that carry the flag unconditionally (Broadcom/V3D). Their colour carry is not this
	/// policy's to change. The depth term below still applies: it is a correctness rule and V3D is
	/// a tiler with the same hazard.
	bool device_always_carries = false;

	/// The fetch road's carry was measured on this device. Mali only.
	bool device_is_measured_vendor = false;

	/// The driver orders the layout road's read: Adreno under Turnip, where the declaration
	/// programs the untiled coherent primitive mode.
	bool device_is_layout_road_vendor = false;

	/// The backend orders the read with an explicit per-draw feedback barrier (texture barriers
	/// on). Consulted only on the layout road. With barriers off there is no ordering, so the
	/// layout road carries nothing, which keeps -no-tex-barriers inert.
	bool barriers_order_reads = false;

	/// The barrier-ordered layout carry was measured on this device (Apple silicon under
	/// Honeykrisp). barriers_order_reads alone does not carry.
	bool device_is_barrier_road_vendor = false;

	/// The in-tile self-read path is live (Vulkan rasterization-order attachment access, which is
	/// what makes declaring a pass self-reading free).
	bool framebuffer_fetch = false;

	/// The backend reaches the render target through the attachment-feedback-loop image layout
	/// rather than subpassLoad. Mutually exclusive with framebuffer_fetch on Vulkan. Selects which
	/// vendor term is consulted, not whether a carry is allowed.
	bool feedback_loop_layout = false;

	/// The draw asks for a feedback barrier of its own. Carrying the flag onto it would emit a
	/// barrier that did not exist before. No non-reader asks for one today
	/// (GSRendererHW::DetermineBarriers); the term enforces that rather than assuming it.
	bool draw_needs_own_barrier = false;

	/// The draw writes depth (the pipeline's depth-stencil selector, or the alpha second pass's).
	/// Only the depth half of the carry looks at this -- see CarryDepthFeedbackAcrossTargetRun.
	bool draw_writes_depth = false;
};

// Returns true when the open pass's feedback-loop flags may be carried onto this draw. A road
// with no ordering carries nothing, and each ordering is scoped to the device it was measured on.
constexpr bool CarryFeedbackLoopAcrossTargetRun(const GSFeedbackLoopCarryInputs& in)
{
	if (in.device_always_carries)
		return true;

	if (in.draw_needs_own_barrier)
		return false;

	// The roads are mutually exclusive on Vulkan, so this picks an argument, not a priority.
	if (in.feedback_loop_layout)
		return in.device_is_layout_road_vendor || (in.barriers_order_reads && in.device_is_barrier_road_vendor);

	return in.device_is_measured_vendor && in.framebuffer_fetch;
}

// Whether the open pass's DEPTH feedback bits may be carried onto this draw, on top of the
// enclosing decision above.
//
// A draw may sample its attached depth only when it does not write depth
// (GSRendererHW::HandleTextureHazards requires !DepthWrite()), so a pass carrying the depth bits is
// one in which nothing wrote the depth being sampled; that is what makes the in-tile depth sample
// well defined. Carrying the bits onto a depth writer puts a writer and a sampler in one pass with
// nothing between them, with the depth image in the feedback/GENERAL layout while written.
// Rasterization-order depth access would order them, but its subpass flag needs depth_feedback,
// framebuffer_fetch and vk_ext_roaa_depth together, so it cannot be relied on.
//
// The colour bit is not gated: there is no read-only colour flag to contradict, every draw in the
// pass writes colour anyway, and on a non-reader the declaration only adds ordering over a read
// the draw does not perform.
//
// Both depth bits are dropped, not only the read-only one: a depth writer inside a pass declared
// ReadAndWriteDepth is unverified, and the conservative choice costs at most one pass boundary on
// depth-sampling passes.
constexpr bool CarryDepthFeedbackAcrossTargetRun(const GSFeedbackLoopCarryInputs& in)
{
	return CarryFeedbackLoopAcrossTargetRun(in) && !in.draw_writes_depth;
}

// The unconditional carry ignores a barrier-requesting draw.
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_always_carries = true}));
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_always_carries = true, .draw_needs_own_barrier = true}));

// The fetch path carries only with fetch on a measured vendor.
static_assert(CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = false}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = false, .framebuffer_fetch = true}));

// The layout road: either source of ordering carries, neither does not. Rows 3-5: the M2, desktop
// Vulkan (unmeasured), the M2 with texture barriers off.
static_assert(CarryFeedbackLoopAcrossTargetRun(
	{.device_is_layout_road_vendor = true, .feedback_loop_layout = true}));
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_is_layout_road_vendor = true,
	.barriers_order_reads = true, .feedback_loop_layout = true}));
static_assert(CarryFeedbackLoopAcrossTargetRun({.barriers_order_reads = true,
	.device_is_barrier_road_vendor = true, .feedback_loop_layout = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.barriers_order_reads = true, .feedback_loop_layout = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.device_is_barrier_road_vendor = true, .feedback_loop_layout = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun({.feedback_loop_layout = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun({.device_is_layout_road_vendor = true,
	.feedback_loop_layout = true, .draw_needs_own_barrier = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun({.barriers_order_reads = true,
	.device_is_barrier_road_vendor = true, .feedback_loop_layout = true, .draw_needs_own_barrier = true}));

// The vendor term does not carry without its road, and neither road's term changes the other's
// answer.
static_assert(!CarryFeedbackLoopAcrossTargetRun({.device_is_layout_road_vendor = true}));
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_is_measured_vendor = true,
	.device_is_layout_road_vendor = true, .framebuffer_fetch = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun({.device_is_measured_vendor = true,
	.framebuffer_fetch = true, .feedback_loop_layout = true}));

// barriers_order_reads is a layout-road term. It is also true on the fetch road, where it must not
// change the answer, and it must not carry with no road.
static_assert(!CarryFeedbackLoopAcrossTargetRun({.barriers_order_reads = true}));
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_is_measured_vendor = true,
	.barriers_order_reads = true, .framebuffer_fetch = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.barriers_order_reads = true, .framebuffer_fetch = true}));

// A depth writer never inherits the depth bits, on any device, including the unconditional one.
static_assert(!CarryDepthFeedbackAcrossTargetRun({.device_always_carries = true, .draw_writes_depth = true}));
static_assert(CarryDepthFeedbackAcrossTargetRun({.device_always_carries = true}));
static_assert(!CarryDepthFeedbackAcrossTargetRun({.device_is_measured_vendor = true, .framebuffer_fetch = true,
	.draw_writes_depth = true}));
static_assert(CarryDepthFeedbackAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = true}));
static_assert(!CarryDepthFeedbackAcrossTargetRun({.device_is_layout_road_vendor = true,
	.feedback_loop_layout = true, .draw_writes_depth = true}));
static_assert(CarryDepthFeedbackAcrossTargetRun(
	{.device_is_layout_road_vendor = true, .feedback_loop_layout = true}));
static_assert(!CarryDepthFeedbackAcrossTargetRun({.barriers_order_reads = true,
	.device_is_barrier_road_vendor = true, .feedback_loop_layout = true, .draw_writes_depth = true}));
static_assert(CarryDepthFeedbackAcrossTargetRun({.barriers_order_reads = true,
	.device_is_barrier_road_vendor = true, .feedback_loop_layout = true}));
