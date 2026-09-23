// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSSelfReadRoadPolicy.h"

// Caps a title's blending accuracy on devices where reading the destination costs something on
// every draw that does it.
//
// Blending accuracy above Minimum makes draws read the destination colour. The cost depends on the
// device's destination-read road (GSRenderer's device-loss report uses the same names):
//
//   * in-tile framebuffer fetch -- a subpassLoad from tile memory. Free.
//   * texture barrier -- a sample of the live attachment ordered by a per-draw barrier. Cheap on
//     an immediate-mode GPU; on a tiler the driver answers each barrier with a cache flush and a
//     wait for the pipeline to drain.
//   * render-target copy per primitive group -- the no-barrier fallback on immediate-mode GPUs
//     (D3D11, desktop GL). A blit is just a blit there.
//   * render-target copy per DRAW -- the no-barrier fallback on a tiler. Each read ends the render
//     pass, stores the tile, copies the target and starts a new pass. This is where Adreno sits
//     (ARMSX2 #442).
//
// On the per-draw copy road and on a tiler's barrier road, Splashdown (SLUS-20686) spends a large
// share of its frame on destination reads; capping it to Minimum removes nearly all of them. The
// cap does move pixels (the water spray behind the boat); the Minimum picture was judged acceptable
// on device. On the capped roads the copy and barrier paths produce identical output.
//
// Behaviour: where reading the destination costs something per draw, Splashdown renders at Minimum
// blending accuracy. Everywhere else nothing changes.
//
// Which barrier roads count is FeatureSupport::barrier_read_costs_per_draw, set only where it was
// measured (Adreno under Turnip, Apple silicon under Honeykrisp). Desktop Vulkan and GL also use the
// barrier road and keep their level.
//
// The rule keys on the ROAD, not on DriverWorkaround::UseRenderTargetCopyForFeedback. Other causes
// land a device on the copy road too (OverrideTextureBarriers=0, a GLES part with no fetch
// extension), and the picture should not depend on which one applied. The road is also what the
// backend publishes, so every case can be reproduced on one device and pinned at compile time.
// It is re-read whenever the renderer opens or settings change, so a driver that starts ordering
// the read drops the cap with no edit here.
//
// The cap arrives as a GameDB hardware fix (copyRoadMaximumBlendingLevel), so a game-level
// accurate_blending_unit or manual hardware fixes make applyGSHardwareFixes skip it. It ships in the
// mobile GameDB overlay only, which the ARM64 Linux build also carries; barrier_read_costs_per_draw
// keeps a desktop GPU in such a machine from applying it on the barrier road.
//
// Levels are plain integers, 0 = Minimum through 5 = Maximum, matching the database key and keeping
// this header free of Config.h. See gs_copy_road_blending_tests.cpp.

struct GSCopyRoadBlendingInputs
{
	/// Which self-read road the backend is on (GSSelfReadRoadPolicy.h). Vulkan decides it in
	/// DecideSelfReadRoad; other backends derive it via GSSelfReadRoadFromPublishedBits.
	///
	/// ⚠️ Not `texture_barrier`: that bit is true on the barrier road and on both driver-ordered
	/// roads, so it cannot tell a per-draw barrier from a free read.
	GSSelfReadRoad road = GSSelfReadRoad::Copy;

	/// The fallback copies once per primitive group inside one pass (D3D11, desktop GL). Those GPUs
	/// are immediate-mode, so the copy has no pass boundary or tile store. See
	/// GSFramebufferFetchPolicy.h's GLUsesPerPrimitiveFbCopy. Only meaningful on the copy road.
	bool multidraw_fb_copy = false;

	/// FeatureSupport::barrier_read_costs_per_draw: the barrier road costs like a copy on this
	/// device (Adreno under Turnip, Apple silicon under Honeykrisp). Only meaningful on
	/// InPassBarrier; false means the barrier is cheap.
	bool barrier_costs_per_draw = false;

	/// The database's copyRoadMaximumBlendingLevel for the running title, or -1 when unset.
	int title_cap = -1;

	/// EmuCore/GS AccurateBlendingUnit after the rest of the database applied. Only ever lowered.
	int configured_level = 0;
};

// Returns true when a draw that reads the destination pays for the read on this road -- a copy of
// the target, or a pipeline barrier -- rather than getting it from the driver for nothing.
constexpr bool DestinationReadCostsPerDraw(const GSCopyRoadBlendingInputs& in)
{
	switch (in.road)
	{
		case GSSelfReadRoad::InPassOrdered:
			// The driver orders the read (in-tile fetch, or a declared feedback loop the driver
			// orders). Free.
			return false;

		case GSSelfReadRoad::InPassBarrier:
			// A barrier per draw: cheap on immediate-mode GPUs, a pipeline drain per draw on a
			// tiler. The backend flags the devices where it is expensive.
			return in.barrier_costs_per_draw;

		case GSSelfReadRoad::Copy:
		default:
			// Copies, but on a GPU where a copy is just a copy.
			return !in.multidraw_fb_copy;
	}
}

// The blending accuracy the renderer should actually run at.
//
// Returns configured_level unless the title asked for a cap AND this device pays per draw for its
// destination read. Never raises the level.
constexpr int CopyRoadBlendingLevel(const GSCopyRoadBlendingInputs& in)
{
	if (in.title_cap < 0)
		return in.configured_level;

	if (!DestinationReadCostsPerDraw(in))
		return in.configured_level;

	return (in.configured_level < in.title_cap) ? in.configured_level : in.title_cap;
}

// Roads that must not move, with the cap present.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::InPassOrdered, .title_cap = 0, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel(
				  {.road = GSSelfReadRoad::Copy, .multidraw_fb_copy = true, .title_cap = 0, .configured_level = 1}) == 1);

// Roads where the cap applies.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::Copy, .title_cap = 0, .configured_level = 1}) == 0);
static_assert(CopyRoadBlendingLevel(
				  {.road = GSSelfReadRoad::InPassBarrier, .barrier_costs_per_draw = true, .title_cap = 0, .configured_level = 1}) == 0);

// Desktop Vulkan and GL on the barrier road: the entry is present and nothing moves.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::InPassBarrier, .title_cap = 0, .configured_level = 1}) == 1);

// No cap requested: untouched on every road.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::Copy, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::InPassBarrier, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::InPassOrdered, .configured_level = 1}) == 1);

// The cap lowers and never raises.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::Copy, .title_cap = 3, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::Copy, .title_cap = 3, .configured_level = 5}) == 3);

// The road predicate: the four destination reads above collapse to two answers.
static_assert(DestinationReadCostsPerDraw({.road = GSSelfReadRoad::Copy}));
static_assert(DestinationReadCostsPerDraw({.road = GSSelfReadRoad::InPassBarrier, .barrier_costs_per_draw = true}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoad::InPassBarrier}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoad::InPassOrdered}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoad::InPassOrdered, .barrier_costs_per_draw = true}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoad::Copy, .multidraw_fb_copy = true}));

// The same via GSSelfReadRoadFromPublishedBits, the path GS.cpp uses (FeatureSupport bit order).
static_assert(DestinationReadCostsPerDraw({.road = GSSelfReadRoadFromPublishedBits(false, false, false)}));
static_assert(DestinationReadCostsPerDraw(
	{.road = GSSelfReadRoadFromPublishedBits(false, true, false), .barrier_costs_per_draw = true}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoadFromPublishedBits(false, true, false)}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoadFromPublishedBits(true, true, false)}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoadFromPublishedBits(false, true, true)}));
