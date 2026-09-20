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
//
// The attachment-feedback-loop LAYOUT road reaches the same conclusion by a different route, and
// the price it saves is larger. There the backend samples the attachment through an ordinary
// sampler while the image sits in VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT: no input
// attachment, no subpassLoad (GSDeviceVK builds the input reference only when
// UseFeedbackLoopLayout() is false). What the declaration costs there is a pipeline create flag --
// every pipeline bound in a pass whose attachment is in that layout must carry
// VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT or the pass is undefined -- and the
// carry is what supplies it, because the word it ORs onto the following draws is the same
// pipeline-selector word those create flags are derived from. A latched pass is self-consistent:
// what keeps the pass open is what makes the pipelines inside it legal.
//
// Carried onto a non-reader the declaration states nothing that draw falsifies. The draw writes
// colour, which is what the pass was already for, and the ordering the loop declares is over a
// read it does not perform.
//
// Scoped to Adreno, for a reason narrower than "it is a tiler": on Turnip the declaration is what
// makes the read coherent at all. The driver refuses to tile a pass containing a pipeline that
// declares a feedback loop which may involve textures, and on the untiled path that same
// declaration programs the primitive mode that orders the read. So the ordering comes from the
// driver, which is the invariant the fetch road gets from rasterization-order attachment access.
// Campaign gs-adreno-inpass-read measured the road on an SD865 under Turnip/Mesa 26.1.2: God of
// War II's presented frames were identical across three runs of each arm and identical between
// the two roads, and its pass census is why this carry has to reach the road at all -- declaring
// the loop WITHOUT the carry raised the pass count on three of seven titles (Splashdown 4,766 to
// 9,451, Black 880 to 2,106, Brian Lara 228 to 435), which is exactly the alternation this file
// describes: a declared draw between two ordinary ones costs two pass boundaries.
//
// The M2 also takes the layout road -- Honeykrisp advertises no rasterization-order extension, so
// UseFeedbackLoopLayout() is true there by default -- and must NOT carry. Its self-read is ordered
// by the backend's own barriers rather than by any driver primitive mode, so the paragraph above
// is not an argument about it, and it is this programme's byte-identity guard device. Same
// discipline as the fetch road's Mali scoping: the carry stays on the device it was reasoned about.
//
// In this tree the layout branch is not reached on an Adreno at all. UseFeedbackLoopLayout()
// requires the rasterization-order extension to be ABSENT and every Turnip device advertises it,
// so feedback_loop_layout is false there and an Adreno takes the copy road. What makes the road
// selectable is campaign gs-adreno-inpass-read's own change, which is not here; this rule is
// written now so the carry arrives with that road rather than one round after it.
//
// Where the layout carry is NOT free. Every pipeline in a latched pass carries the colour
// feedback-loop create flag, and on Turnip that flag is what forces the pass untiled -- the
// gmem-disable term is read from the pipeline create flag, ahead of the bandwidth autotuner. So
// the carry spreads the untiling from the reading draw to the whole latched run. On the
// seven-dump census it costs nothing, because the autotuner had already picked sysmem for those
// passes for its own reasons; it is not free in principle, and a title whose passes the autotuner
// tiles would pay tile-store bandwidth across the run. Pass COUNT cannot see that. The
// tiled/untiled mode census is the instrument.
//
// VK_EXT_attachment_feedback_loop_dynamic_state is the future way to keep the create flag off
// pipelines that never read, and the interesting part is that it would only work for those. On
// Turnip 26.1.2 the extension is advertised unconditionally and the dynamic value does reach the
// primitive mode, but the gmem-disable term is set only from the pipeline CREATE flag -- there is
// no dynamic path to it. So for the carried never-reading pipelines dynamic state would satisfy
// the validation rule without contributing to untiling, while for the declared READING draw the
// create flag IS the road: the untiling is what makes its read coherent, and the primitive mode
// alone inside a tiled pass is the incoherent case. That asymmetry is the note; it is source
// reading of Turnip 26.1.2, not a measurement, and nothing here builds on it.

struct GSFeedbackLoopCarryInputs
{
	/// Devices that carried the flag before this policy existed and keep carrying it
	/// unconditionally (Broadcom/V3D). Their COLOUR carry is not this policy's to change.
	/// The depth term below does reach them, because it is a correctness rule about what the
	/// render pass declares and V3D is a tiler with the same hazard, not a tuning decision.
	bool device_always_carries = false;

	/// The device is one the FETCH road's carry has been measured on. Mali only -- see the note
	/// on the return value below.
	bool device_is_measured_vendor = false;

	/// The device is the one the LAYOUT road's carry was reasoned about and measured on: Adreno
	/// under Turnip, where the driver's untiled primitive mode is what orders the read. Every
	/// other device on that road -- the M2 is the one this tree's gates run on -- keeps the flag
	/// draw-local.
	bool device_is_layout_road_vendor = false;

	/// The in-tile self-read path is live (Vulkan rasterization-order attachment access, which is
	/// what makes declaring a pass self-reading free).
	bool framebuffer_fetch = false;

	/// The backend reaches the render target through the attachment-feedback-loop image layout
	/// rather than through subpassLoad. Mutually exclusive with framebuffer_fetch on Vulkan
	/// (UseFeedbackLoopLayout tests for the absence of the ROAA extension). It picks WHICH road's
	/// reasoning applies and therefore which vendor term is consulted, not whether a carry is
	/// allowed at all.
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
// Two roads to the same saving, each pinned to the one device it was reasoned about and measured
// on. Every device that reaches either road is a tiler and would in principle profit, but a
// vendor-scoped carry was once widened past its evidence here and had to be reverted (see the
// GSDeviceVK call site), so widening either road is a separate decision with its own device round.
constexpr bool CarryFeedbackLoopAcrossTargetRun(const GSFeedbackLoopCarryInputs& in)
{
	if (in.device_always_carries)
		return true;

	if (in.draw_needs_own_barrier)
		return false;

	// The two roads are mutually exclusive on Vulkan, so this is a choice of which argument
	// applies, not a priority.
	if (in.feedback_loop_layout)
		return in.device_is_layout_road_vendor;

	return in.device_is_measured_vendor && in.framebuffer_fetch;
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

// The layout road carries on the device whose driver orders the read, and on no other device that
// takes it -- the second row is the M2, which takes this road by default.
static_assert(CarryFeedbackLoopAcrossTargetRun(
	{.device_is_layout_road_vendor = true, .feedback_loop_layout = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun({.feedback_loop_layout = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun({.device_is_layout_road_vendor = true,
	.feedback_loop_layout = true, .draw_needs_own_barrier = true}));

// The vendor term does not carry without its road, and neither road's term changes the other's
// answer.
static_assert(!CarryFeedbackLoopAcrossTargetRun({.device_is_layout_road_vendor = true}));
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_is_measured_vendor = true,
	.device_is_layout_road_vendor = true, .framebuffer_fetch = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun({.device_is_measured_vendor = true,
	.framebuffer_fetch = true, .feedback_loop_layout = true}));

// A depth writer never inherits the depth bits, on any device -- including the one whose colour
// carry is unconditional.
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
