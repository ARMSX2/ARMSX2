// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Which road a backend takes for a draw that reads the render target it is also writing, and what
// that road implies for texture barriers, the in-tile read and primitive ordering.
//
// Those four bits used to be decided by four expressions a hundred and fifty lines apart in
// GSDeviceVK::CheckFeatures -- the framebuffer-fetch decision, `OverrideTextureBarriers != 0`, the
// driver database's RT-copy workaround, and `framebuffer_fetch &= texture_barrier`. Reading them
// in order is the only way to find out which road a device is on, and the same shape in the OpenGL
// fetch decision is what let a driver guard turn fetch off and a profile block turn it straight
// back on 0.1 ms apart in one log (see the note at the top of GSFramebufferFetchPolicy.h).
//
// THE THREE ROADS
//
//   Copy           -- the target is snapshotted into its own texture, the render pass ends, and the
//                     draw samples the snapshot. Always correct, the most expensive thing we have,
//                     and structurally wrong for primitives that overlap WITHIN one draw: every
//                     such primitive composites against the same pre-draw snapshot.
//   InPassBarrier  -- the draw reads the live attachment, and the ordering comes from explicit
//                     pipeline barriers the backend emits (one per draw, or one per primitive group
//                     when the draw's own primitives overlap). Desktop's road.
//   InPassOrdered  -- the draw reads the live attachment and the DRIVER orders it, so no barrier is
//                     emitted at all. Two ways to earn that, and they are different mechanisms:
//                     the in-tile read under Vulkan rasterization-order attachment access, and the
//                     declared attachment feedback loop this campaign is measuring on Adreno.
//
// THE SPELLING IS A SEPARATE AXIS, and conflating it with the road is how the July 2026 arm
// produced an unreadable result. Which shader variant, image layout, image usage bit and descriptor
// type the read uses is decided ONCE at device creation and cannot move afterwards:
//
//   Clone              -- texelFetch of a separate copy (GSDeviceVK's draw_rt_clone).
//   InputAttachment    -- subpassLoad of the attachment as a subpass input.
//   FeedbackLoopLayout -- texelFetch of the live attachment in
//                         VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, with the matching
//                         VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT on every
//                         pipeline bound while it is in that layout.
//
// Desktop already runs FeedbackLoopLayout + InPassBarrier every day: it advertises
// VK_EXT_attachment_feedback_loop_layout and not the rasterization-order extension, so
// GSDeviceVK::UseFeedbackLoopLayout() is already true there and the pipeline create flag already
// fires. Nothing about the declaration is new or untested -- what is new is reaching that spelling
// on a driver that also advertises the rasterization-order extension, and dropping the barriers.
//
// WHY THE ARM EXISTS
//
// On Adreno under Turnip the in-tile read returns render-pass-START content: writes sit unresolved
// in tile memory while the fetch goes out through UCHE, which is invalidated once per subpass begin
// rather than per draw. The driver database says so (vk-turnip-attachment-self-read) and puts every
// Adreno on Turnip onto the Copy road. That rule is about the TILED in-tile read, which is what it
// was measured on.
//
// Turnip refuses to tile a render pass at all when a pipeline bound in it declares a feedback loop
// that may involve textures, and it decides that before its bandwidth autotuner is consulted. On
// the untiled path the same declaration programs GRAS_SC_CNTL.SINGLE_PRIM_MODE =
// FLUSH_PER_OVERLAP_AND_OVERWRITE, the documented bypass-mode coherent-blend value. So the
// application can ask for an untiled pass with a coherent, primitive-ordered destination read using
// nothing but a standard extension. That configuration has never been on a device: the one arm that
// tried it (2026-07-26, branch tota-442-adreno-fbfetch, now deleted) predated the pipeline create
// flag by three days, and the create flag is the single thing Turnip keys the sysmem decision on.
//
// ⚠️ THE GRANULARITY IS THE RENDER PASS, NOT THE DRAW. One declared pipeline untiles every other
// draw sharing the pass, and our passes are deliberately coalesced. That bill is what the campaign's
// E1 sizes; nothing in this header reduces it.
//
// ⚠️ THE ORDERING IS PER PIXEL, NOT PER TARGET. FLUSH_PER_OVERLAP_AND_OVERWRITE orders primitives
// that cover the same sample. A read of a DIFFERENT pixel that an earlier primitive in the same
// draw wrote is not covered -- exactly the limit the in-tile read has. GSSelfReadCopyPolicy.h is
// where that is handled, and it takes `declared_feedback_loop_orders_overlap` for the purpose.
//
// EXPERIMENT SCAFFOLDING. `arm` is EmuCore/GS/DeclareAttachmentFeedbackLoop, and it exists so one
// binary can run base against the candidate on a device. It is not a user setting and has no UI
// row: a user cannot tell which road their driver wants, and producing that answer as a
// driver-database rule is what the campaign is for. If the road lands, the key is replaced by a
// DriverWorkaround bit and deleted, not promoted.
//
// Written as a pure function so every no-change case is pinned without the device that takes the
// changed one. See gs_self_read_road_tests.cpp.

enum class GSSelfReadRoad : u8
{
	/// Snapshot the target, end the pass, sample the snapshot.
	Copy,
	/// Read the live attachment; the backend's own barriers order it.
	InPassBarrier,
	/// Read the live attachment; the driver orders it and no barrier is emitted.
	InPassOrdered,
};

enum class GSSelfReadSpelling : u8
{
	/// No in-pass read is configured (the Copy road's shader still texelFetches, but of a clone).
	Clone,
	/// subpassLoad of a subpass input attachment.
	InputAttachment,
	/// texelFetch of the live attachment in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL.
	FeedbackLoopLayout,
};

/// EmuCore/GS/DeclareAttachmentFeedbackLoop. Experiment scaffolding; see the header note.
enum class GSSelfReadArm : u8
{
	/// The shipped decision, whatever it is on this device. The default, and the only value that
	/// ships enabled.
	Off = 0,
	/// Declare the loop and trust the driver's ordering: barriers dropped.
	Declared = 1,
	/// Declare the loop and keep the per-draw barriers. The diagnostic arm -- the difference
	/// between this and Declared IS the ordering claim, so a picture that is right here and wrong
	/// there says the declaration arrived and the ordering did not.
	DeclaredKeepBarriers = 2,
};

struct GSSelfReadRoadInputs
{
	/// The in-tile read is available and not denied for this part -- i.e.
	/// DecideVulkanFramebufferFetch(...).enabled, which already folds in the rasterization-order
	/// extension, the vendor denies and the user's DisableFramebufferFetch.
	bool in_tile_read_available = false;

	/// VK_EXT_attachment_feedback_loop_layout is present AND its feature bit survived
	/// reconciliation. Without it the layout spelling does not exist and the arm cannot run.
	bool layout_road_available = false;

	/// VK_EXT_rasterization_order_attachment_access is present. Only used to reproduce
	/// UseFeedbackLoopLayout()'s preference for the in-tile spelling where the device has one.
	bool roaa_available = false;

	/// DriverWorkaround::UseRenderTargetCopyForFeedback -- the driver database says no form of
	/// in-pass self-read this device has been measured in is reliable.
	bool rt_self_read_is_broken = false;

	/// GSConfig.OverrideTextureBarriers: -1 auto, 0 force off, 1 force on. An explicit 0 is the
	/// documented way back to the copy road and outranks the arm.
	s8 override_texture_barriers = -1;

	/// GSConfig.DeclareAttachmentFeedbackLoop, as GSSelfReadArm.
	u8 arm = static_cast<u8>(GSSelfReadArm::Off);
};

struct GSSelfReadRoadDecision
{
	GSSelfReadRoad road = GSSelfReadRoad::Copy;
	GSSelfReadSpelling spelling = GSSelfReadSpelling::Clone;

	/// -> GSDevice::FeatureSupport::texture_barrier. The renderer reads it as "may I resolve a
	/// hazard in the pass", and the Vulkan backend reads it as "do NOT clone the target".
	bool texture_barrier = false;

	/// -> GSDevice::FeatureSupport::framebuffer_fetch. Deliberately FALSE on the declared road:
	/// leaving it on would put two spellings of the same read in one binary -- the shader
	/// compiling texelFetch while the render pass skipped its self-dependency and the pipeline
	/// carried the rasterization-order blend flag.
	bool in_tile_read = false;

	/// -> GSDeviceVK::m_force_feedback_loop_layout, which is OR-ed into UseFeedbackLoopLayout().
	/// Never set off the arm, so the helper's expression is unchanged on every shipping device.
	bool force_feedback_loop_layout = false;

	/// -> GSDevice::FeatureSupport::declared_feedback_loop_orders_overlap. Licenses
	/// DetermineBarriers to drop the per-draw barriers, and tells GSSelfReadCopyPolicy that an
	/// OFFSET read still needs its copy.
	bool orders_overlapping_prims = false;

	/// The arm was asked for and applied. Everything above is derived from it, but the depth probe
	/// and the banner need to ask the question directly rather than inferring it from a bit that
	/// happens to be set only there.
	bool arm_applied = false;

	/// The arm was asked for and could not be given -- no layout extension, or texture barriers
	/// forced off. Reported so the caller can say so once; a silently inert arm is a device round
	/// that measures base twice.
	bool arm_unavailable = false;
};

// The shipped decision, reproduced exactly: texture barriers follow the override tri-state with the
// driver database's RT-copy workaround applied only on auto, and the in-tile read needs barriers
// because it IS the in-pass read.
constexpr GSSelfReadRoadDecision DecideSelfReadRoad(const GSSelfReadRoadInputs& in)
{
	GSSelfReadRoadDecision d;

	const bool arm_requested = (in.arm != static_cast<u8>(GSSelfReadArm::Off));
	// An explicit OverrideTextureBarriers=0 still wins. It is the documented way back to the copy
	// road, and an arm that quietly overrode it would make that lever untrustworthy on the one
	// build the device round is holding.
	const bool arm_applies = arm_requested && in.layout_road_available && (in.override_texture_barriers != 0);
	d.arm_applied = arm_applies;
	d.arm_unavailable = arm_requested && !arm_applies;

	if (arm_applies)
	{
		d.road = (in.arm == static_cast<u8>(GSSelfReadArm::Declared)) ? GSSelfReadRoad::InPassOrdered :
																		GSSelfReadRoad::InPassBarrier;
		d.spelling = GSSelfReadSpelling::FeedbackLoopLayout;
		d.texture_barrier = true;
		d.in_tile_read = false;
		d.force_feedback_loop_layout = true;
		d.orders_overlapping_prims = (in.arm == static_cast<u8>(GSSelfReadArm::Declared));
		return d;
	}

	d.texture_barrier =
		(in.override_texture_barriers != 0) && !(in.rt_self_read_is_broken && in.override_texture_barriers < 0);
	d.in_tile_read = in.in_tile_read_available && d.texture_barrier;
	d.force_feedback_loop_layout = false;
	d.orders_overlapping_prims = false;

	if (!d.texture_barrier)
	{
		d.road = GSSelfReadRoad::Copy;
		d.spelling = GSSelfReadSpelling::Clone;
		return d;
	}

	d.road = d.in_tile_read ? GSSelfReadRoad::InPassOrdered : GSSelfReadRoad::InPassBarrier;
	// Mirrors GSDeviceVK::UseFeedbackLoopLayout() off the arm: prefer the in-tile spelling wherever
	// the device advertises the extension that makes it ordered.
	d.spelling = (in.layout_road_available && !in.roaa_available) ? GSSelfReadSpelling::FeedbackLoopLayout :
																	GSSelfReadSpelling::InputAttachment;
	return d;
}

/// One phrase naming both axes, for the start-up banner. The device round quotes this line, so it
/// says what was declared rather than what was configured.
constexpr const char* GSSelfReadRoadName(const GSSelfReadRoadDecision& d)
{
	switch (d.road)
	{
		case GSSelfReadRoad::Copy:
			return "copy (clone the target per feedback draw)";
		case GSSelfReadRoad::InPassBarrier:
			return (d.spelling == GSSelfReadSpelling::FeedbackLoopLayout) ?
					   "in-pass, barrier-ordered, declared feedback loop" :
					   "in-pass, barrier-ordered, input attachment";
		case GSSelfReadRoad::InPassOrdered:
		default:
			return (d.spelling == GSSelfReadSpelling::FeedbackLoopLayout) ?
					   "in-pass, driver-ordered, declared feedback loop" :
					   "in-pass, driver-ordered, in-tile fetch";
	}
}

// --- The arm OFF is today's answer, on every device shape we ship to. ---------------------------
//
// These are the whole reason this is a function. If any of them changes, a device that is not part
// of this campaign has moved.

// Turnip / the Qualcomm blob: the database's RT-copy workaround on auto. Copy road, no barriers, no
// in-tile read however loudly the extension is advertised.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = true})
				   .texture_barrier);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = true})
				   .in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = true})
				   .force_feedback_loop_layout);

// The same part with OverrideTextureBarriers=1: the documented A/B back onto the in-tile road.
// Unchanged -- the arm did not take that lever away.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true, .override_texture_barriers = 1})
				  .in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true, .override_texture_barriers = 1})
				  .spelling == GSSelfReadSpelling::InputAttachment);

// Mali at its default: barriers on, in-tile read on, and the layout spelling refused because the
// rasterization-order extension is there.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true})
				  .in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true})
				  .spelling == GSSelfReadSpelling::InputAttachment);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true})
				  .road == GSSelfReadRoad::InPassOrdered);

// Desktop: no rasterization-order extension, so the layout spelling with real barriers. This is the
// configuration the arm reaches on Adreno, minus the barrier drop -- it has shipped for years.
static_assert(DecideSelfReadRoad({.layout_road_available = true}).texture_barrier);
static_assert(DecideSelfReadRoad({.layout_road_available = true}).road == GSSelfReadRoad::InPassBarrier);
static_assert(DecideSelfReadRoad({.layout_road_available = true}).spelling ==
			  GSSelfReadSpelling::FeedbackLoopLayout);
static_assert(!DecideSelfReadRoad({.layout_road_available = true}).orders_overlapping_prims);

// OverrideTextureBarriers=0 is the copy road everywhere, with or without a broken self-read.
static_assert(!DecideSelfReadRoad({.layout_road_available = true, .override_texture_barriers = 0}).texture_barrier);

// --- The arm ON. --------------------------------------------------------------------------------

// Turnip, arm 1: barriers on, in-tile read OFF, the layout forced, ordering claimed.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true,
				  .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				  .texture_barrier);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = true,
					.arm = static_cast<u8>(GSSelfReadArm::Declared)})
				   .in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true,
				  .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				  .force_feedback_loop_layout);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true,
				  .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				  .orders_overlapping_prims);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true,
				  .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				  .spelling == GSSelfReadSpelling::FeedbackLoopLayout);

// Arm 2 declares the same thing and keeps the barriers. Everything but the ordering claim matches.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true,
				  .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
				  .force_feedback_loop_layout);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = true,
					.arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
				   .orders_overlapping_prims);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true,
				  .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
				  .road == GSSelfReadRoad::InPassBarrier);

// The arm on a device with no layout extension does nothing, and says so.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true,
					.rt_self_read_is_broken = true, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				   .force_feedback_loop_layout);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true,
				  .rt_self_read_is_broken = true, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				  .arm_unavailable);

// The arm with barriers forced off does nothing either, and also says so.
static_assert(!DecideSelfReadRoad({.layout_road_available = true, .roaa_available = true,
					.override_texture_barriers = 0, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				   .texture_barrier);
static_assert(DecideSelfReadRoad({.layout_road_available = true, .roaa_available = true,
				  .override_texture_barriers = 0, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				  .arm_unavailable);

// Off the arm, nothing is ever "unavailable" -- the field is about a request that could not be met.
static_assert(!DecideSelfReadRoad({.rt_self_read_is_broken = true}).arm_unavailable);
static_assert(!DecideSelfReadRoad({.rt_self_read_is_broken = true}).arm_applied);
// applied and unavailable are exclusive, in both directions.
static_assert(DecideSelfReadRoad({.layout_road_available = true, .roaa_available = true,
				  .rt_self_read_is_broken = true, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				  .arm_applied);
static_assert(!DecideSelfReadRoad({.layout_road_available = true, .roaa_available = true,
					.rt_self_read_is_broken = true, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				   .arm_unavailable);
