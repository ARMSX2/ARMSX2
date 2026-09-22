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
// THREE ENTRANCES TO THE DECLARED ROAD, and they are not the same kind of thing.
//
// `driver_orders_declared_loop` and `driver_prefers_declared_loop_with_barriers` are the shipping
// two: the driver database recognising, respectively, a driver build measured to order overlapping
// self-reads inside a declared loop (Turnip on a6xx carrying the fix) and a part whose best
// in-pass road is that loop with our own barriers left in place (Turnip on a7xx). Both put a user
// on the road with no setting touched. A user on any other driver gets exactly what they got
// before, because no other driver has been measured.
//
// They differ only in the barriers, and that is not a detail -- it is the whole of what was
// measured. The a7xx part takes the same declaration and keeps the barriers because Turnip never
// emits the ordering state there, so dropping them races.
//
// `arm` is EmuCore/GS/DeclareAttachmentFeedbackLoop -- experiment scaffolding, so one binary can
// run base against the candidate on a device. It is not a user setting and has no UI row: a user
// cannot tell which road their driver wants. It still wins where it is set, because arm 2 on our
// own driver is the reference picture the ordering claim is measured against.
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

	/// MobileDriverProfile::orders_declared_feedback_loop -- the driver database says THIS driver
	/// build orders overlapping self-reads inside a declared attachment feedback loop. False for
	/// every driver that has not been measured doing it, which is every driver but ours.
	///
	/// It outranks rt_self_read_is_broken, and that is not a contradiction: the RT-copy rule was
	/// measured on the in-pass read Turnip had in July 2026, and this is a different driver build.
	/// A driver that carries the fix is not the driver the rule is about.
	bool driver_orders_declared_loop = false;

	/// MobileDriverProfile::prefers_declared_loop_with_barriers -- the driver database says THIS
	/// part's best in-pass road is the declared loop with our own per-draw barriers still doing
	/// the ordering. True for Turnip on Adreno 7xx and nothing else.
	///
	/// It outranks rt_self_read_is_broken on auto for the same reason the ordering fact does, and
	/// a stronger one: on the a740 the copy road that rule selects is not merely slow, it is
	/// WRONG. The Godfather comes out a third wrong against software and NASCAR's sky wrong, and
	/// the declared road with barriers is correct on every scored cell over seven reps (campaign
	/// gs-adreno-inpass-read, E18).
	///
	/// ⚠️ It claims NOTHING about ordering, and that is the whole difference from
	/// driver_orders_declared_loop. Turnip never emits the sysmem ordering state on a7xx, so the
	/// barrier-less declared road races there -- up to seven distinct pictures from seven runs on
	/// ten of sixteen cells. Where a driver somehow carried both facts the ordering one is
	/// strictly more and answers.
	bool driver_prefers_declared_loop_with_barriers = false;

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
	///
	/// ⚠️ **This asserts a fact about the DRIVER, not about the road.** Declaring a feedback loop
	/// buys the layout and the validity relaxation. It does not buy ordering between overlapping
	/// fragments that sample the attachment they write -- that is what
	/// VK_EXT_rasterization_order_attachment_access promises, and the layout extension says nothing
	/// about it. Two drivers, and NEITHER of them orders -- for different reasons, both in mesa at
	/// c71af679f08:
	///
	/// - Turnip EMITS the ordering mode and does not deliver it. A declared loop forces sysmem
	///   (tu_cmd_buffer.cc:5568) and sets SINGLE_PRIM_MODE = FLUSH_PER_OVERLAP_AND_OVERWRITE for
	///   `feedback_loops` -- the same enum on the same register it uses for ROAA
	///   (tu_pipeline.cc:3813), and that forced sysmem is also why E4a counted zero tiled passes on
	///   declared passes. The term is unchanged in the device's own release (introduced
	///   a99600322c1, an ancestor of mesa-26.1.2; the function diffs empty against our clone).
	///   ⚠️ **And the picture still moves and still races.** On an Adreno 650, claiming ordering
	///   moves 47 of 94 corpus cells and is nondeterministic in 20 of them -- worst case 11
	///   distinct outputs from 11 runs, against an arm that keeps the barriers and never once
	///   disagreed with itself over 658 runs (campaign gs-adreno-inpass-read, E5). Whether that is
	///   the distro's build, our own pipelines, or the mode not doing what its name says is
	///   unseparated.
	/// - Honeykrisp does not even emit it. It advertises the layout extension
	///   (hk_physical_device.c:144) and has no ordering machinery anywhere in src/asahi/vulkan.
	///   Claiming ordering there moves 70 of 94 corpus cells (upscale-unify/Q-copyroad2x).
	///
	/// ⚠️ **The strongest case for this guard is the first bullet**: the one driver whose source
	/// says it orders, measurably does not. Reading the emission and concluding the behaviour is
	/// exactly the mistake this comment existed to prevent, and its first version made it.
	///
	/// ⚠️ **So nothing a device ADVERTISES may set this**, and nothing ever will: the extensions do
	/// not promise ordering and the one driver whose source emits it does not deliver it. Two
	/// inputs set it, and both name a specific driver build rather than a capability:
	///
	/// - `driver_orders_declared_loop`, the driver database recognising a build that was MEASURED
	///   to order. That is the shape the paragraph above asked for -- false unless that driver has
	///   been measured, so an unrecognised driver keeps its barriers and comes out
	///   correct-and-slower instead of fast-and-wrong. What earns it today: a Turnip build carrying
	///   the a6xx feedback-loop fix, on which the barrier-dropping road came out byte-identical to
	///   the barrier-keeping reference over the corpus and faster than the copy road on the titles
	///   that matter (campaign gs-adreno-inpass-read, E11 and E12; the fix itself is E13).
	/// - `arm` = GSSelfReadArm::Declared, which is how the claim gets measured in the first place.
	///   GSSelfReadArm::DeclaredKeepBarriers is its control.
	bool orders_overlapping_prims = false;

	/// This road declares an attachment feedback loop -- the experiment key asked for one, or the
	/// driver fact selected the road that does.
	///
	/// ⚠️ Everything downstream that needs to know a loop is declared reads THIS, not arm_applied.
	/// The two were the same thing while the key was the only way onto the road, and the alpha
	/// stencil counter and the depth probe were both written against arm_applied when that was
	/// true. It is not true any more.
	bool loop_declared = false;

	/// The declared road came from a driver fact rather than from the experiment key. Reported so
	/// the banner can say which, because a device round quotes that line and "why is this machine
	/// on the declared road" has exactly two answers.
	///
	/// WHICH driver fact is readable off the road: the ordering fact lands on InPassOrdered and
	/// the a7xx preference on InPassBarrier, so the pair (road, this bit) names the entrance. The
	/// backend still prints the rule by name, because a road name does not say what measurement
	/// stands behind it.
	bool selected_by_driver_fact = false;

	/// The arm was asked for and applied -- the EXPERIMENT KEY specifically, which is what the
	/// arm_unavailable diagnostic below is about. For "is a loop declared", read loop_declared.
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
	// An explicit OverrideTextureBarriers=0 still wins, over the arm and over the driver fact
	// alike. It is the documented way back to the copy road, and a road that quietly overrode it
	// would make that lever untrustworthy on the one build the device round is holding.
	const bool barriers_allowed = (in.override_texture_barriers != 0);
	const bool arm_applies = arm_requested && in.layout_road_available && barriers_allowed;
	d.arm_applied = arm_applies;
	d.arm_unavailable = arm_requested && !arm_applies;

	// Two driver facts take the same road the arm does, under the same two preconditions, and only
	// when no arm was asked for. The key still wins where it is set: arm 2 on our own driver is the
	// reference picture the ordering claim is measured against, so it has to stay reachable there.
	//
	// The ordering fact is strictly more than the barrier one -- same road, barriers dropped --
	// so where a driver somehow carried both, it is the one that answers. No part carries both
	// today: one is a6xx and the other a7xx.
	const bool ordering_fact_applies =
		!arm_requested && in.driver_orders_declared_loop && in.layout_road_available && barriers_allowed;
	const bool barrier_fact_applies = !arm_requested && !in.driver_orders_declared_loop &&
	                                  in.driver_prefers_declared_loop_with_barriers &&
	                                  in.layout_road_available && barriers_allowed;
	const bool fact_applies = ordering_fact_applies || barrier_fact_applies;

	if (arm_applies || fact_applies)
	{
		// The ordering fact IS the ordering claim -- a driver gets it by being measured to order
		// -- so it lands on arm 1's decision exactly. Arm 1 asserts the same thing on request; arm
		// 2 declares the identical loop and KEEPS the barriers, which is the only way to tell the
		// declaration from the ordering on a device, and it is also what the barrier fact lands
		// on. Nothing else may assert it: see the field's note, and the never-claims assertions
		// below, which hold whatever the device advertises.
		const bool claims_ordering =
			ordering_fact_applies || (in.arm == static_cast<u8>(GSSelfReadArm::Declared));
		d.road = claims_ordering ? GSSelfReadRoad::InPassOrdered : GSSelfReadRoad::InPassBarrier;
		d.spelling = GSSelfReadSpelling::FeedbackLoopLayout;
		d.texture_barrier = true;
		d.in_tile_read = false;
		d.force_feedback_loop_layout = true;
		d.orders_overlapping_prims = claims_ordering;
		d.loop_declared = true;
		d.selected_by_driver_fact = fact_applies;
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

/// One phrase naming both axes and, on the declared road, what put the machine there. The device
/// round quotes this line, so it says what was declared rather than what was configured -- and
/// since the declared road now has two entrances, it says which one was used. "experiment key" is
/// EmuCore/GS/DeclareAttachmentFeedbackLoop; "driver fact" is the driver database recognising this
/// driver build as one that was measured to order.
constexpr const char* GSSelfReadRoadName(const GSSelfReadRoadDecision& d)
{
	switch (d.road)
	{
		case GSSelfReadRoad::Copy:
			return "copy (clone the target per feedback draw)";
		case GSSelfReadRoad::InPassBarrier:
			if (d.spelling != GSSelfReadSpelling::FeedbackLoopLayout)
				return "in-pass, barrier-ordered, input attachment";
			// Desktop and Honeykrisp land here WITHOUT declaring anything -- the layout spelling is
			// simply what UseFeedbackLoopLayout() picks for them -- so only say the loop was
			// declared when it actually was.
			if (!d.loop_declared)
				return "in-pass, barrier-ordered, declared feedback loop";
			// The a7xx preference lands here: same declaration as the experiment key's arm 2, same
			// barriers, chosen by the driver database instead of by a setting.
			return d.selected_by_driver_fact ?
			           "in-pass, barrier-ordered, declared feedback loop (driver fact)" :
			           "in-pass, barrier-ordered, declared feedback loop (experiment key)";
		case GSSelfReadRoad::InPassOrdered:
		default:
			if (d.spelling != GSSelfReadSpelling::FeedbackLoopLayout)
				return "in-pass, driver-ordered, in-tile fetch";
			return d.selected_by_driver_fact ?
			           "in-pass, driver-ordered, declared feedback loop (driver fact)" :
			           "in-pass, driver-ordered, declared feedback loop (experiment key)";
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

// ⚠️ NO COMBINATION OF EXTENSIONS EVER CLAIMS THE ORDERING, whatever the device advertises. None of
// them promise it, and the one driver whose source emits it does not deliver it. These four are the
// guard on that, and they are written with every extension bit and every override value set: the
// only things that reach the claim are the experiment key and a driver the database has been told
// was measured to order, and both have to come through here to get it.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = true})
				   .orders_overlapping_prims);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = false,
					.override_texture_barriers = 1})
				   .orders_overlapping_prims);
// The Honeykrisp shape, and the reason this guard is not hypothetical: layout extension present, no
// ROAA, no in-tile read, nothing broken -- a device that takes the layout road and does not order.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = false, .layout_road_available = true,
					.roaa_available = false, .rt_self_read_is_broken = false})
				   .orders_overlapping_prims);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = false, .layout_road_available = true,
					.roaa_available = false, .rt_self_read_is_broken = false,
					.override_texture_barriers = 0})
				   .orders_overlapping_prims);

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

// --- The driver fact. ----------------------------------------------------------------------------
//
// One driver build, recognised by the database, on the Turnip shape: every extension advertised and
// the RT-copy workaround claimed, which is the shipped Adreno device. With no key set it must land
// on arm 1's decision, bit for bit -- the fact is the same claim arm 1 makes, arrived at by
// measurement instead of by asking.

static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
				  .road == GSSelfReadRoad::InPassOrdered);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
				  .spelling == GSSelfReadSpelling::FeedbackLoopLayout);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.texture_barrier);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.force_feedback_loop_layout);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.orders_overlapping_prims);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.loop_declared);
// No key was set, so the arm diagnostics stay quiet and the banner says the driver put us here.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.selected_by_driver_fact);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.arm_applied);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.arm_unavailable);

// OverrideTextureBarriers=0 is still the copy road. The fact does not take that lever away any more
// than the arm did.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .override_texture_barriers = 0})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .override_texture_barriers = 0})
		.orders_overlapping_prims);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .override_texture_barriers = 0})
		.loop_declared);

// A driver that claims the fix without the layout extension gets today's answer and nothing else.
// The layout spelling is the only one the declaration exists in, so there is no road to take.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.orders_overlapping_prims);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.loop_declared);
// ...and it is not an "unavailable arm" either. Nobody asked for an arm, so nothing failed.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.arm_unavailable);

// The key still wins on our own driver, and arm 2 is the reason it has to. Arm 2 declares the loop
// and keeps the barriers, so it is the reference picture the ordering claim is measured against; if
// the fact overrode it there would be nothing to measure against on the only driver that has it.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
				  .road == GSSelfReadRoad::InPassBarrier);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
		.orders_overlapping_prims);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
		.force_feedback_loop_layout);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
		.arm_applied);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
		.selected_by_driver_fact);

// The loop is declared on both entrances, which is what everything downstream of the road reads.
static_assert(DecideSelfReadRoad({.layout_road_available = true, .driver_orders_declared_loop = true})
		.loop_declared);
static_assert(DecideSelfReadRoad({.layout_road_available = true,
									 .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
		.loop_declared);
// And it is NOT declared on the desktop road, which reaches the same spelling without asking for
// anything. That distinction is the whole reason loop_declared is a separate bit from the spelling.
static_assert(!DecideSelfReadRoad({.layout_road_available = true}).loop_declared);
static_assert(DecideSelfReadRoad({.layout_road_available = true}).spelling ==
			  GSSelfReadSpelling::FeedbackLoopLayout);

// --- The a7xx preference. -------------------------------------------------------------------
//
// The second driver fact, on the same Turnip shape: every extension advertised and the RT-copy
// workaround claimed. With no key set it must land on ARM 2's decision, bit for bit -- the
// declared loop with the per-draw barriers kept, and no ordering claimed. That is what E18 and
// E19 measured on the a740, and the barrier-less road on that part races.

static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
				  .road == GSSelfReadRoad::InPassBarrier);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
				  .spelling == GSSelfReadSpelling::FeedbackLoopLayout);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.texture_barrier);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.force_feedback_loop_layout);
// ⚠️ The one bit that separates this fact from E17's. Turnip never emits the sysmem ordering state
// on a7xx, so a claim here would be a claim about a race.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.orders_overlapping_prims);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.loop_declared);
// Nobody asked for an arm, so neither arm diagnostic fires and the banner says the driver chose.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.selected_by_driver_fact);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.arm_applied);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.arm_unavailable);

// OverrideTextureBarriers=0 is still the copy road. Neither fact takes that lever away.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 0})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 0})
		.loop_declared);

// OverrideTextureBarriers=1 means "force the in-pass road", and on this part the in-pass road we
// measured IS the declared one. Off the fact the same value takes the in-tile road instead (the
// assert near the top of this file), which on an a740 is correct but 1.5-3.7x slower on three
// titles -- so the lever keeps its documented meaning and gets the better of the two in-pass
// roads rather than the one the extension list happens to prefer.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 1})
				  .road == GSSelfReadRoad::InPassBarrier);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 1})
		.loop_declared);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 1})
		.in_tile_read);

// A part that prefers the road without the layout extension gets today's answer and nothing else,
// exactly as the tag does: the layout spelling is the only one the declaration exists in.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.loop_declared);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.arm_unavailable);

// The experiment key still wins, both ways. Arm 1 drops the barriers on request -- that is how the
// racing road got measured on the a740 in the first place -- and arm 2 is the road the fact
// already selects, asked for by name.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
				  .road == GSSelfReadRoad::InPassOrdered);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
		.orders_overlapping_prims);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
		.arm_applied);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .arm = static_cast<u8>(GSSelfReadArm::Declared)})
		.selected_by_driver_fact);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
				  .road == GSSelfReadRoad::InPassBarrier);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
		.selected_by_driver_fact);

// No part carries both facts today -- one is a6xx and the other a7xx -- but if one ever did, the
// ordering fact is strictly more and answers. Same road, barriers dropped.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .driver_prefers_declared_loop_with_barriers = true})
				  .road == GSSelfReadRoad::InPassOrdered);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .driver_prefers_declared_loop_with_barriers = true})
		.orders_overlapping_prims);

// And the other direction, which is the one that has to hold for every device we ship to: without
// the preference, nothing about the a7xx road exists. Every assert above this section still reads
// the same function, because the new input defaults to false.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true})
		.loop_declared);
static_assert(!DecideSelfReadRoad({.layout_road_available = true}).selected_by_driver_fact);
