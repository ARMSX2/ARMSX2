// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Which road a backend takes for a draw that reads the render target it is also writing, and what
// that road implies for texture barriers, the in-tile read and primitive ordering. One function
// instead of separate expressions in GSDeviceVK::CheckFeatures, so the road a device is on can be
// read in one place.
//
// THE THREE ROADS
//
//   Copy           -- the target is snapshotted into its own texture, the render pass ends, and the
//                     draw samples the snapshot. Always correct, the most expensive, and wrong for
//                     primitives that overlap within one draw: each composites against the same
//                     pre-draw snapshot.
//   InPassBarrier  -- the draw reads the live attachment, ordered by pipeline barriers the backend
//                     emits (one per draw, or per primitive group when primitives overlap). Desktop.
//   InPassOrdered  -- the draw reads the live attachment and the DRIVER orders it; no barrier. Two
//                     mechanisms: the in-tile read under rasterization-order attachment access, and
//                     the declared attachment feedback loop on a driver known to order it.
//
// THE SPELLING IS A SEPARATE AXIS. Shader variant, image layout, usage bit and descriptor type are
// decided once at device creation:
//
//   Clone              -- texelFetch of a separate copy (GSDeviceVK's draw_rt_clone).
//   InputAttachment    -- subpassLoad of the attachment as a subpass input.
//   FeedbackLoopLayout -- texelFetch of the live attachment in
//                         VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, with
//                         VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT on every
//                         pipeline bound while it is in that layout.
//
// Desktop runs FeedbackLoopLayout + InPassBarrier by default: it advertises
// VK_EXT_attachment_feedback_loop_layout and not the rasterization-order extension, so
// GSDeviceVK::UseFeedbackLoopLayout() is true there.
//
// THE DECLARED ROAD (Adreno under Turnip)
//
// Turnip's tiled in-tile read returns render-pass-start content: writes sit unresolved in tile
// memory while the fetch goes through UCHE, invalidated per subpass begin rather than per draw. So
// the driver database (vk-turnip-attachment-self-read) puts Turnip on the Copy road.
//
// Turnip refuses to tile a pass in which a bound pipeline declares a feedback loop that may involve
// textures, before its bandwidth autotuner is consulted. On the untiled path the declaration
// programs GRAS_SC_CNTL.SINGLE_PRIM_MODE = FLUSH_PER_OVERLAP_AND_OVERWRITE, the bypass-mode
// coherent-blend value. The declared road forces the layout spelling to reach that.
//
// ⚠️ The granularity is the render pass, not the draw. One declared pipeline untiles every draw
// sharing the pass, and our passes are deliberately coalesced.
//
// ⚠️ The ordering is per pixel, not per target. It orders primitives covering the same sample, not
// a read of a different pixel an earlier primitive wrote. An offset read keeps its one barrier,
// which the untiled pass honours, and takes no copy (GSDrawRoad.h).
//
// ENTRANCES TO THE DECLARED ROAD
//
// `driver_orders_declared_loop` and `driver_prefers_declared_loop_with_barriers` are the shipping
// ones: the driver database recognising a build that orders overlapping self-reads in a declared
// loop (Turnip on a6xx with the fix), or a part whose best road is that loop with our barriers kept
// (Turnip on a7xx, where Turnip never emits the ordering state, so dropping barriers races). No
// setting is involved; every other driver is unchanged.
//
// `arm` is the harness's -declare-feedback-loop, a process global set by the runner
// (GSMeasurementOverrides.h), not a setting. It wins where set, because DeclaredKeepBarriers is the
// reference picture the ordering claim is measured against.
//
// Pure function so every no-change case is pinned without the device. See
// gs_self_read_road_tests.cpp.

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

/// gsrunner -declare-feedback-loop. Measurement scaffolding; see the header note.
enum class GSSelfReadArm : u8
{
	/// The shipped decision. The default, and the only value that ships enabled.
	Off = 0,
	/// Declare the loop and trust the driver's ordering: barriers dropped.
	Declared = 1,
	/// Declare the loop and keep the per-draw barriers. The difference from Declared is exactly
	/// the ordering claim, so right here and wrong there means the ordering did not arrive.
	DeclaredKeepBarriers = 2,
};

struct GSSelfReadRoadInputs
{
	/// DecideVulkanFramebufferFetch(...).enabled: the extension, vendor denies and the user's
	/// DisableFramebufferFetch already folded in.
	bool in_tile_read_available = false;

	/// VK_EXT_attachment_feedback_loop_layout is present and its feature bit is on. Without it the
	/// layout spelling does not exist and the declared road cannot be taken.
	bool layout_road_available = false;

	/// VK_EXT_rasterization_order_attachment_access is present. Only used to reproduce
	/// UseFeedbackLoopLayout()'s preference for the in-tile spelling.
	bool roaa_available = false;

	/// DriverWorkaround::UseRenderTargetCopyForFeedback: no in-pass self-read on this device is
	/// known to be reliable.
	bool rt_self_read_is_broken = false;

	/// MobileDriverProfile::orders_declared_feedback_loop: this driver build is known to order
	/// overlapping self-reads inside a declared feedback loop. False for any unverified driver.
	///
	/// Outranks rt_self_read_is_broken: that rule is about the tiled in-pass read on builds
	/// without the fix.
	bool driver_orders_declared_loop = false;

	/// MobileDriverProfile::prefers_declared_loop_with_barriers: this part's best in-pass road is
	/// the declared loop with our per-draw barriers doing the ordering. Turnip on Adreno 730 and up
	/// only. Outranks rt_self_read_is_broken on auto: on those parts the copy road also renders
	/// some titles wrong.
	///
	/// ⚠️ It claims nothing about ordering. Turnip never emits the sysmem ordering state on a7xx, so
	/// the barrier-less declared road races there. If both facts are set, the ordering one wins.
	bool driver_prefers_declared_loop_with_barriers = false;

	/// GSConfig.OverrideTextureBarriers: -1 auto, 0 force off, 1 force on. An explicit 0 is the
	/// way back to the copy road and outranks the arm. The driver facts apply on auto only, so an
	/// explicit 1 gets the in-pass read the extension list picks.
	s8 override_texture_barriers = -1;

	/// GSMeasurementOverrides::self_read_arm, as GSSelfReadArm. Off on every run but a harness's.
	u8 arm = static_cast<u8>(GSSelfReadArm::Off);
};

struct GSSelfReadRoadDecision
{
	GSSelfReadRoad road = GSSelfReadRoad::Copy;
	GSSelfReadSpelling spelling = GSSelfReadSpelling::Clone;

	/// -> GSDevice::FeatureSupport::texture_barrier. The renderer reads it as "may I resolve a
	/// hazard in the pass", and the Vulkan backend reads it as "do NOT clone the target".
	bool texture_barrier = false;

	/// -> GSDevice::FeatureSupport::framebuffer_fetch. False on the declared road: leaving it on
	/// would mix two spellings of the read (texelFetch in the shader while the render pass skips
	/// its self-dependency and the pipeline carries the rasterization-order blend flag).
	bool in_tile_read = false;

	/// -> GSDeviceVK::m_force_feedback_loop_layout, OR-ed into UseFeedbackLoopLayout(). Set only on
	/// the declared road.
	bool force_feedback_loop_layout = false;

	/// -> GSDevice::FeatureSupport::declared_feedback_loop_orders_overlap. Licenses
	/// DetermineBarriers to drop the per-draw barriers of a read of the fragment's own pixel. An
	/// OFFSET read keeps its one barrier (GSDrawRoad.h).
	///
	/// ⚠️ This is a fact about the DRIVER, not the road. Declaring a feedback loop buys the layout
	/// and the validity relaxation, not ordering between overlapping fragments that sample what
	/// they write; that is what VK_EXT_rasterization_order_attachment_access promises, and the
	/// layout extension says nothing about it. Stock Turnip emits FLUSH_PER_OVERLAP_AND_OVERWRITE
	/// for declared loops, yet claiming ordering there still produces nondeterministic output on
	/// Adreno 650. Honeykrisp advertises the layout extension and has no ordering machinery. The
	/// emitted state is not evidence of the behaviour.
	///
	/// So nothing a device advertises may set this. Two inputs do, both naming a driver build:
	///
	/// - `driver_orders_declared_loop`: the database recognises a build verified to order (Turnip
	///   with the a6xx feedback-loop fix). An unrecognised driver keeps its barriers:
	///   correct-and-slower rather than fast-and-wrong.
	/// - `arm` = GSSelfReadArm::Declared, to measure the claim. DeclaredKeepBarriers is its
	///   control.
	bool orders_overlapping_prims = false;

	/// This road declares an attachment feedback loop, by the experiment key or by a driver fact.
	///
	/// ⚠️ Downstream code that needs to know a loop is declared must read this, not arm_applied,
	/// which misses the driver-fact entrance.
	bool loop_declared = false;

	/// The declared road came from a driver fact rather than the experiment key. Reported in the
	/// banner. The ordering fact lands on InPassOrdered and the a7xx preference on InPassBarrier,
	/// so (road, this bit) names the entrance.
	bool selected_by_driver_fact = false;

	/// The experiment key was asked for and applied. For "is a loop declared", read loop_declared.
	bool arm_applied = false;

	/// The arm was asked for and could not be given (no layout extension, or texture barriers
	/// forced off). Reported so an inert arm is not mistaken for a measurement.
	bool arm_unavailable = false;
};

// Texture barriers follow the override tri-state, with the RT-copy workaround applied only on
// auto; the in-tile read needs barriers because it is the in-pass read.
constexpr GSSelfReadRoadDecision DecideSelfReadRoad(const GSSelfReadRoadInputs& in)
{
	GSSelfReadRoadDecision d;

	const bool arm_requested = (in.arm != static_cast<u8>(GSSelfReadArm::Off));
	// An explicit OverrideTextureBarriers=0 wins over the arm and the driver facts: it is the way
	// back to the copy road.
	const bool barriers_allowed = (in.override_texture_barriers != 0);
	const bool arm_applies = arm_requested && in.layout_road_available && barriers_allowed;
	d.arm_applied = arm_applies;
	d.arm_unavailable = arm_requested && !arm_applies;

	// The driver facts apply only with no arm requested and OverrideTextureBarriers on auto. The
	// key wins where set, so DeclaredKeepBarriers stays reachable as the reference picture. An
	// explicit 1 gets the in-pass read the extension list picks (the in-tile read on Turnip).
	//
	// The ordering fact is strictly more than the barrier one, so it answers if both are set.
	const bool barriers_on_auto = (in.override_texture_barriers < 0);
	const bool ordering_fact_applies =
		!arm_requested && in.driver_orders_declared_loop && in.layout_road_available && barriers_on_auto;
	const bool barrier_fact_applies = !arm_requested && !in.driver_orders_declared_loop &&
	                                  in.driver_prefers_declared_loop_with_barriers &&
	                                  in.layout_road_available && barriers_on_auto;
	const bool fact_applies = ordering_fact_applies || barrier_fact_applies;

	if (arm_applies || fact_applies)
	{
		// The ordering fact lands on Declared's decision exactly. DeclaredKeepBarriers and the
		// barrier fact declare the same loop and keep the barriers. Nothing else may claim
		// ordering (see orders_overlapping_prims).
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
	// Mirrors GSDeviceVK::UseFeedbackLoopLayout(): prefer the in-tile spelling wherever the device
	// advertises the extension that makes it ordered.
	d.spelling = (in.layout_road_available && !in.roaa_available) ? GSSelfReadSpelling::FeedbackLoopLayout :
																	GSSelfReadSpelling::InputAttachment;
	return d;
}

// The road a backend is on, read back from the three bits it publishes in
// GSDevice::FeatureSupport. Used by GSApplyCopyRoadBlendingCap in GS.cpp, which needs the cost of
// a destination read on every backend while only Vulkan holds a GSSelfReadRoadDecision.
// `texture_barrier` alone is not enough: it is true on the barrier road and both driver-ordered
// roads, so it cannot tell a per-draw barrier from a free read.
//
// Pinned against DecideSelfReadRoad below over every input it accepts.
constexpr GSSelfReadRoad GSSelfReadRoadFromPublishedBits(
	bool in_tile_read, bool texture_barrier, bool declared_loop_orders_overlap)
{
	// No in-pass read is legal at all: the target is cloned per feedback draw.
	if (!texture_barrier)
		return GSSelfReadRoad::Copy;

	// The driver orders the read and no barrier is emitted -- either in tile memory, or inside a
	// declared feedback loop on a driver build measured to order one.
	if (in_tile_read || declared_loop_orders_overlap)
		return GSSelfReadRoad::InPassOrdered;

	// The live attachment, ordered by our own barriers, one per draw or per primitive group.
	return GSSelfReadRoad::InPassBarrier;
}

// Checks the read-back names the road the decision chose, over every input DecideSelfReadRoad
// accepts. Failure means callers that ask for the device's road get the wrong one.
constexpr bool PublishedBitsNameTheSameRoad()
{
	for (int bits = 0; bits < (1 << 6); bits++)
	{
		for (s8 override_barriers = -1; override_barriers <= 1; override_barriers++)
		{
			for (u8 arm = 0; arm <= static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers); arm++)
			{
				GSSelfReadRoadInputs in;
				in.in_tile_read_available = (bits & 1) != 0;
				in.layout_road_available = (bits & 2) != 0;
				in.roaa_available = (bits & 4) != 0;
				in.rt_self_read_is_broken = (bits & 8) != 0;
				in.driver_orders_declared_loop = (bits & 16) != 0;
				in.driver_prefers_declared_loop_with_barriers = (bits & 32) != 0;
				in.override_texture_barriers = override_barriers;
				in.arm = arm;

				const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
				if (GSSelfReadRoadFromPublishedBits(d.in_tile_read, d.texture_barrier,
						d.orders_overlapping_prims) != d.road)
					return false;
			}
		}
	}
	return true;
}
static_assert(PublishedBitsNameTheSameRoad());

/// One phrase naming both axes and, on the declared road, which entrance was used: "experiment
/// key" is gsrunner -declare-feedback-loop, "driver fact" is the driver database.
constexpr const char* GSSelfReadRoadName(const GSSelfReadRoadDecision& d)
{
	switch (d.road)
	{
		case GSSelfReadRoad::Copy:
			return "copy (clone the target per feedback draw)";
		case GSSelfReadRoad::InPassBarrier:
			if (d.spelling != GSSelfReadSpelling::FeedbackLoopLayout)
				return "in-pass, barrier-ordered, input attachment";
			// Desktop and Honeykrisp get the layout spelling from UseFeedbackLoopLayout(), not the declared road.
			if (!d.loop_declared)
				return "in-pass, barrier-ordered, declared feedback loop";
			// The a7xx preference lands here: the same road as DeclaredKeepBarriers.
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

// --- The arm off, on every device shape we ship to. ---------------------------------------------
//
// If any of these changes, a device has moved road.

// Turnip / the Qualcomm blob: RT-copy workaround on auto. Copy road, no barriers, no in-tile read.
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

// The same part with OverrideTextureBarriers=1: the in-tile road.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true, .override_texture_barriers = 1})
				  .in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true, .rt_self_read_is_broken = true, .override_texture_barriers = 1})
				  .spelling == GSSelfReadSpelling::InputAttachment);

// Mali at its default: barriers on, in-tile read on, no layout spelling because ROAA is present.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true})
				  .in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true})
				  .spelling == GSSelfReadSpelling::InputAttachment);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
				  .roaa_available = true})
				  .road == GSSelfReadRoad::InPassOrdered);

// Desktop: no rasterization-order extension, so the layout spelling with real barriers.
static_assert(DecideSelfReadRoad({.layout_road_available = true}).texture_barrier);
static_assert(DecideSelfReadRoad({.layout_road_available = true}).road == GSSelfReadRoad::InPassBarrier);
static_assert(DecideSelfReadRoad({.layout_road_available = true}).spelling ==
			  GSSelfReadSpelling::FeedbackLoopLayout);
static_assert(!DecideSelfReadRoad({.layout_road_available = true}).orders_overlapping_prims);

// OverrideTextureBarriers=0 is the copy road everywhere, with or without a broken self-read.
static_assert(!DecideSelfReadRoad({.layout_road_available = true, .override_texture_barriers = 0}).texture_barrier);

// --- The arm ON. --------------------------------------------------------------------------------

// Turnip, GSSelfReadArm::Declared: barriers on, in-tile read OFF, the layout forced, ordering claimed.
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

// DeclaredKeepBarriers declares the same thing and keeps the barriers. Everything but the ordering claim matches.
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

// ⚠️ No combination of extensions or override values claims ordering. Only the experiment key and
// a database-recognised driver reach the claim.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = true})
				   .orders_overlapping_prims);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true,
					.roaa_available = true, .rt_self_read_is_broken = false,
					.override_texture_barriers = 1})
				   .orders_overlapping_prims);
// The Honeykrisp shape: layout extension, no ROAA, nothing broken; takes the layout road and does
// not order.
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

// Off the arm, nothing is "unavailable".
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
// On the Turnip shape (every extension, RT-copy workaround claimed), the ordering fact with no key
// set lands on Declared's decision bit for bit.

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
// No key was set: arm diagnostics quiet, selected_by_driver_fact set.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.selected_by_driver_fact);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.arm_applied);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.arm_unavailable);

// OverrideTextureBarriers=0 is still the copy road.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .override_texture_barriers = 0})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .override_texture_barriers = 0})
		.orders_overlapping_prims);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .override_texture_barriers = 0})
		.loop_declared);

// Without the layout extension the fact does nothing: the declaration only exists in that spelling.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.orders_overlapping_prims);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.loop_declared);
// ...and it is not an unavailable arm, since none was asked for.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true})
		.arm_unavailable);

// The key wins over the fact, so DeclaredKeepBarriers stays available as the reference picture.
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

// The loop is declared on both entrances.
static_assert(DecideSelfReadRoad({.layout_road_available = true, .driver_orders_declared_loop = true})
		.loop_declared);
static_assert(DecideSelfReadRoad({.layout_road_available = true,
									 .arm = static_cast<u8>(GSSelfReadArm::DeclaredKeepBarriers)})
		.loop_declared);
// Not declared on the desktop road, which has the same spelling. Why loop_declared is its own bit.
static_assert(!DecideSelfReadRoad({.layout_road_available = true}).loop_declared);
static_assert(DecideSelfReadRoad({.layout_road_available = true}).spelling ==
			  GSSelfReadSpelling::FeedbackLoopLayout);

// --- The a7xx preference. -------------------------------------------------------------------
//
// On the same Turnip shape, the a7xx fact with no key set lands on DeclaredKeepBarriers' decision
// bit for bit: declared loop, barriers kept, no ordering claimed.

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
// ⚠️ The bit that separates this fact from the a6xx one: no ordering state on a7xx, so no claim.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.orders_overlapping_prims);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.loop_declared);
// No arm asked for: arm diagnostics quiet, selected_by_driver_fact set.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.selected_by_driver_fact);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.arm_applied);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.arm_unavailable);

// OverrideTextureBarriers=0 is still the copy road.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 0})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 0})
		.loop_declared);

// OverrideTextureBarriers=1 reaches the in-tile read with either fact present; the facts apply on
// auto only.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 1})
				  .road == GSSelfReadRoad::InPassOrdered);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 1})
		.loop_declared);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true, .override_texture_barriers = 1})
		.in_tile_read);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .override_texture_barriers = 1})
		.in_tile_read);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .override_texture_barriers = 1})
		.loop_declared);

// Without the layout extension the a7xx fact does nothing either.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
				  .road == GSSelfReadRoad::Copy);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.loop_declared);
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_prefers_declared_loop_with_barriers = true})
		.arm_unavailable);

// The experiment key wins both ways: Declared drops the barriers, DeclaredKeepBarriers matches the
// fact's road.
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

// Both facts set: the ordering fact answers.
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .driver_prefers_declared_loop_with_barriers = true})
				  .road == GSSelfReadRoad::InPassOrdered);
static_assert(DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true, .driver_orders_declared_loop = true, .driver_prefers_declared_loop_with_barriers = true})
		.orders_overlapping_prims);

// Without either fact, no loop is declared.
static_assert(!DecideSelfReadRoad({.in_tile_read_available = true, .layout_road_available = true, .roaa_available = true, .rt_self_read_is_broken = true})
		.loop_declared);
static_assert(!DecideSelfReadRoad({.layout_road_available = true}).selected_by_driver_fact);
