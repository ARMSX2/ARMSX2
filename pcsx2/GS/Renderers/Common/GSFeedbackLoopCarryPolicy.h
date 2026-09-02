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

struct GSFeedbackLoopCarryInputs
{
	/// Devices that carried the flag before this policy existed and keep carrying it
	/// unconditionally (Broadcom/V3D). Their behaviour is not this policy's to change.
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

	/// EmuCore/GS/FeedbackLoopCarry.
	bool carry_key = false;

	/// The draw asks for a feedback barrier of its own. Carrying the flag onto such a draw would
	/// hand the backend a render target to barrier against where it previously had none, which
	/// would emit a barrier that did not exist before -- a behaviour change, not a pass saving.
	/// On the fetch path no non-reader asks for one (GSRendererHW::DetermineBarriers sets the
	/// barrier flags only for target readers, and clears them outright for colour readers under
	/// fetch), so this term costs nothing today. It is here so the invariant is enforced rather
	/// than assumed.
	bool draw_needs_own_barrier = false;
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

	if (!in.carry_key)
		return false;

	if (!in.device_is_measured_vendor || !in.framebuffer_fetch || in.feedback_loop_layout)
		return false;

	return !in.draw_needs_own_barrier;
}

// The unconditional carry is exactly as unconditional as it was: the key does not reach it, and
// neither does a barrier-requesting draw.
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_always_carries = true}));
static_assert(CarryFeedbackLoopAcrossTargetRun({.device_always_carries = true, .draw_needs_own_barrier = true}));

// The fetch path carries; without fetch, or with the key off, nothing changes.
static_assert(CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = true, .carry_key = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = false, .carry_key = true}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = true, .framebuffer_fetch = true, .carry_key = false}));
static_assert(!CarryFeedbackLoopAcrossTargetRun(
	{.device_is_measured_vendor = false, .framebuffer_fetch = true, .carry_key = true}));
