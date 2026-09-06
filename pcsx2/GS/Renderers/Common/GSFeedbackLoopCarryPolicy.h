// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Whether a backend may keep the feedback-loop flag set across a run of draws on one target.
//
// The backend ends a render pass whenever the feedback-loop flag word of the draw it is about to
// submit differs from the flag word the open pass was built with (GSDeviceVK::OMSetRenderTargets).
// A draw that reads the render target sets that flag; a draw that does not, clears it. So a single
// reader sitting between two non-readers costs two pass boundaries, and on a tiler a pass boundary
// is a full tile store and reload.
//
// That price is only worth paying if declaring a pass self-reading costs something. On the
// framebuffer-fetch path it does not: the read is a tile-local subpassLoad under
// rasterization-order attachment access, and a pass declared self-reading by a draw that never
// reads is the same pass with one unused input attachment. So the flag can simply stay set:
//
//   once a target run has a reader, the flag stays set for the following non-readers on that
//   target until the pass ends for another reason.
//
// "Another reason" is every reason there already was -- a different colour or depth target, a
// colclip blit, a command-buffer submit -- none of which this touches.
//
// The decision is a pure function of the device's facts so it can be pinned without a device; the
// backend supplies the facts. See gs_feedback_loop_carry_tests.cpp.
//
// This shipped for one round behind EmuCore/GS/FeedbackLoopCarry so the device suite could run both
// arms off one binary. The round decided it: on the RG 477V with fetch on, frames are identical
// with and without the carry on all 22 corpus dumps, while OutRun 2006 goes from 599.5 render
// passes a frame to 31.1 and Xenosaga from 75,899 per run to 133, taking its frame time from about
// 32 ms to 16.7. The key is gone; the carry is what the function returns.

struct GSFeedbackLoopCarryInputs
{
	/// Devices that carried the flag before this policy existed and keep carrying it
	/// unconditionally (Broadcom/V3D). Their COLOUR carry is not this policy's to change.
	/// The depth term below does reach them, because it is a correctness rule about what the
	/// render pass declares and V3D is a tiler with the same hazard, not a tuning decision.
	bool device_always_carries = false;

	/// The device is one the carry has been measured on. Mali only for now -- see the note on
	/// the return value below.
	bool device_is_measured_vendor = false;

	/// The in-tile self-read path is live (Vulkan rasterization-order attachment access, which is
	/// what makes declaring a pass self-reading free).
	bool framebuffer_fetch = false;

	/// The backend reaches the render target through the attachment-feedback-loop image layout
	/// rather than through subpassLoad. Mutually exclusive with framebuffer_fetch on Vulkan
	/// (UseFeedbackLoopLayout tests for the absence of the ROAA extension); listed anyway so the
	/// carry stays tied to the path it was reasoned about.
	bool feedback_loop_layout = false;

	/// The draw asks for a feedback barrier of its own. Carrying the flag onto such a draw would
	/// hand the backend a render target to barrier against where it previously had none, which
	/// would emit a barrier that did not exist before -- a behaviour change, not a pass saving.
	/// On the fetch path no non-reader asks for one (GSRendererHW::DetermineBarriers sets the
	/// barrier flags only for target readers, and clears them outright for colour readers under
	/// fetch), so this term costs nothing today. It is here so the invariant is enforced rather
	/// than assumed.
	bool draw_needs_own_barrier = false;

	/// The draw writes depth (the pipeline's depth-stencil selector, or the alpha second pass's).
	/// Only the depth half of the carry looks at this -- see CarryDepthFeedbackAcrossTargetRun.
	bool draw_writes_depth = false;
};

// Returns true when the open pass's feedback-loop flags may be carried onto this draw.
//
// Deliberately narrow on vendor. Every device that reaches framebuffer_fetch is a tiler and would
// in principle profit, but the carry was measured on Mali with fetch forced on, and the same
// vendor-scoped carry was once widened past its evidence and had to be reverted (see the
// GSDeviceVK call site). Widening it is a separate decision with its own device round.
constexpr bool CarryFeedbackLoopAcrossTargetRun(const GSFeedbackLoopCarryInputs& in)
{
	if (in.device_always_carries)
		return true;

	if (!in.device_is_measured_vendor || !in.framebuffer_fetch || in.feedback_loop_layout)
		return false;

	return !in.draw_needs_own_barrier;
}

// Whether the open pass's DEPTH feedback bits may be carried onto this draw, on top of the
// enclosing decision above.
//
// The depth bits say something about the pass that the colour bit does not. The read-only depth
// bit is set for a draw that samples the depth buffer it has attached, and the renderer only lets
// such a draw exist when it does not write depth (GSRendererHW::HandleTextureHazards' direct
// depth read requires !DepthWrite()). So a pass carrying the depth bits is a pass in which nothing
// wrote the depth being sampled -- and that, not any barrier, is what makes the in-tile depth
// sample well defined. Carrying the bits onto a depth WRITER states the opposite of what the draw
// does: it puts a depth writer and a depth sampler in one pass with nothing between them, and it
// leaves the depth image in the feedback/GENERAL layout while it is written. Rasterization-order
// depth access would order those two, but that subpass flag is set only when depth_feedback,
// framebuffer_fetch and vk_ext_roaa_depth all hold, so it cannot be relied on to cover this.
//
// The colour bit is safe under the same reasoning, which is why it is not gated here. There is no
// read-only colour flag to contradict: the colour attachment is written by every draw in the pass
// whether or not the bit is set, and FeedbackLoopFlag_ReadAndWriteRT adds an input-attachment
// reference, VK_IMAGE_LAYOUT_GENERAL and -- on the fetch path -- the rasterization-order colour
// access subpass flag. Carried onto a non-reader that is an ordinary writer, that ADDS an ordering
// guarantee over a read the draw does not perform. Nothing it declares is falsified by writing
// colour, because writing colour is what the pass was already for.
//
// Both depth bits are dropped, not only the read-only one. Whether a draw that writes depth is
// safe inside a pass declared ReadAndWriteDepth is a question nothing here has measured, and the
// conservative word -- the one that matches what the draw actually does -- costs at most the pass
// boundary the carry was trying to save, on depth-sampling passes only. No corpus title has one.
constexpr bool CarryDepthFeedbackAcrossTargetRun(const GSFeedbackLoopCarryInputs& in)
{
	return CarryFeedbackLoopAcrossTargetRun(in) && !in.draw_writes_depth;
}

// The unconditional carry is exactly as unconditional as it was: the key does not reach it, and
// neither does a barrier-requesting draw.
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_always_carries = true}));
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_always_carries = true, .draw_needs_own_barrier = true}));

// The fetch path carries; without fetch, or on a vendor the carry was not measured on, nothing
// changes.
static_assert(CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = false}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = false, .framebuffer_fetch = true}));

// A depth writer never inherits the depth bits, on any device -- including the one whose colour
// carry is unconditional.
static_assert(!CarryDepthFeedbackAcrossTargetRun({.device_always_carries = true, .draw_writes_depth = true}));
static_assert(CarryDepthFeedbackAcrossTargetRun({.device_always_carries = true}));
static_assert(!CarryDepthFeedbackAcrossTargetRun({.device_is_measured_vendor = true, .framebuffer_fetch = true,
	.draw_writes_depth = true}));
static_assert(CarryDepthFeedbackAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = true}));
