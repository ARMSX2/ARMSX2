// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSSelfReadRoadPolicy.h"

// What blending accuracy a title renders at on a device where reading the destination costs
// something on every draw that does it.
//
// Accurate blending above Minimum makes a draw read the destination colour. How much that costs is
// not a property of the setting, it is a property of the device's destination read, and the four
// roads are a long way apart -- GSRenderer's device-loss report names them in these same words:
//
//   * in-tile framebuffer fetch -- the read is a subpassLoad out of tile memory. Free.
//   * texture barrier -- the read is a real sample of the live attachment, ordered by a pipeline
//     barrier the backend emits per draw. Cheap on an immediate-mode GPU, which is where the
//     barrier was designed; NOT cheap on a tiler, where the driver answers each barrier with a
//     cache flush and a wait for the pipeline to drain.
//   * render-target copy per primitive group -- the no-barrier fallback on an immediate-mode GPU
//     (D3D11, desktop GL). A blit is a blit there.
//   * render-target copy per DRAW -- the no-barrier fallback on a tiler. Each read ends the render
//     pass, stores the tile, copies the target and starts a new pass. This is the expensive one,
//     and it is where every Adreno under ARMSX2 #442 sits.
//
// On the copy road the price is not marginal. Measured on Splashdown (SLUS-20686) on an
// SD865 under Turnip at native scale: 1,169 of its 2,690 draws a frame read the render target,
// which is 1,192 render passes and 1,177 target copies per frame, and a 32.18 ms p95 against a
// 16.67 ms budget. Holding that one title's blending accuracy down to Minimum collapses it to 24
// passes and 8 copies and 23.70 ms -- 8.48 ms, 26% of the frame, off one setting. At 2x the same
// change is worth 1.20 ms, because the frame has become fill-bound and the saving is on the
// submission side.
//
// ⚠️ THE BARRIER ROAD ON A TILER IS THE SAME PROBLEM, and this file said the opposite until it
// was measured. It used to reason that a texture barrier means the read is cheap, so the cap should lift the
// moment a device has one. That is true of the GPUs the barrier came from and false of the ones we
// ship to. Measured on an Adreno 740 (Turnip): Splashdown's
// 4,678 destination-read draws per four frames stop being fixed-function blends and become software
// blends, each promoted to a full barrier, 59,023 barriers a run, and the frame goes from 24.76 ms
// to 34.80 ms -- +40% on the title that was already the slowest. With the cap applied on that same
// road the barriers go to zero, the draw calls, passes and copies come back to the copy road's
// exactly, and the frame is 25.06 ms: 1.012x the copy road. Pricing the barrier itself on the same device:
// dropping WAIT_FOR_IDLE from the driver's in-pass flush recovers 16-18% -- so what a barrier costs
// there is a wait for the whole pipeline, per draw.
//
// It costs picture. Blending accuracy is exactly what the level controls, so this is not a free
// win being withheld by caution: about 30% of Splashdown's frame moves, by up to 40 levels out of
// 255, across the water spray behind the boat. The Minimum picture was rendered on the device,
// compared against the full-accuracy one, and judged acceptable; that judgement is
// the authority for this file, since a speed change whose only cost is moved pixels is a call on
// the picture, not on byte identity. The road makes no difference to the picture that was judged:
// on the M2 at 1x the capped barrier road is byte-identical to the copy road, 3/3 frames, row for
// row over all 10,793 drawlog rows.
//
// So the behaviour is: ON A DEVICE WHERE READING THE DESTINATION COSTS SOMETHING PER DRAW,
// SPLASHDOWN RENDERS WITH MINIMUM BLENDING ACCURACY. Everywhere else it renders exactly as it did.
//
// The scope is the point of the file, and the scope is the ROAD, not one device fact. Where the
// driver orders the read for us -- Mali's in-tile fetch, and the declared feedback loop on a
// Turnip build measured to order one (the a6xx road) -- the destination read
// is free, there is nothing for the cap to buy, and the better picture stays. Where we pay for the
// read on every draw that takes one, by copying the target or by emitting a barrier, the cap
// applies. Those are the two answers, and DestinationReadCostsPerDraw below is where they are
// decided.
//
// Which device fact. NOT the driver workaround bit (DriverWorkaround::UseRenderTargetCopyForFeedback)
// that puts Adreno on the copy road, even though that bit is what motivated this work. The bit names
// one cause; the road has others -- an explicit OverrideTextureBarriers=0, a GLES part with no
// fetch extension, a future driver entry nobody has written yet -- and a title's picture should not
// depend on which cause landed the device on the road. It is also the difference between a rule
// that can be tested here and one that needs the device: the road is what the backend publishes, so
// the M2 reproduces every Adreno road with a single settings key and every no-change case is pinned
// at compile time.
//
// This still retires itself, on the road it was always meant to leave. The road is read from the
// live device every time the renderer opens or the settings change, so the day an Adreno reaches a
// driver-ordered destination read -- the a6xx fix shipping, or an a7xx driver that finally orders a
// declared loop -- the cap stops applying there with nothing in this file edited.
//
// The player still outranks all of it, one layer up. The cap arrives as a GameDB hardware fix
// (copyRoadMaximumBlendingLevel), so claiming accurate_blending_unit for the game -- or turning on
// manual hardware fixes -- makes applyGSHardwareFixes skip it and never set the field this policy
// reads. Nothing here needs to know about that; it sees a title that asked for nothing.
//
// And the cap ships in the mobile GameDB overlay only, which is the reason a desktop machine on the
// barrier road does not move: no overlay, no entry, title_cap stays -1. The one exception is a
// development build here that loads the overlay by hand -- the M2 does take the cap on Splashdown
// now, and that was measured rather than assumed.
//
// Levels are plain integers, 0 = Minimum through 5 = Maximum, which is the grammar the database
// key already speaks and what keeps this header free of Config.h. See
// gs_copy_road_blending_tests.cpp.

struct GSCopyRoadBlendingInputs
{
	/// Which of the three self-read roads the backend is on, from GSSelfReadRoadPolicy.h. Vulkan
	/// decides it in DecideSelfReadRoad; every backend publishes enough in FeatureSupport for
	/// GSSelfReadRoadFromPublishedBits to name it, and those two are pinned equal at compile time.
	///
	/// ⚠️ NOT `texture_barrier`. That bit is true on the barrier road and on BOTH driver-ordered
	/// roads, so it cannot tell a per-draw barrier from a free read -- which is the whole question
	/// this file asks.
	GSSelfReadRoad road = GSSelfReadRoad::Copy;

	/// The no-barrier fallback copies the target once per PRIMITIVE GROUP inside one pass rather
	/// than once per draw, which is the shape D3D11 and desktop GL take. It is a worse shape on
	/// paper and a cheap one in practice, because the GPUs that take it are immediate-mode and a
	/// blit costs them a blit -- no pass boundary, no tile store. See GSFramebufferFetchPolicy.h's
	/// GLUsesPerPrimitiveFbCopy for the measurement that separates the two. Only ever set on the
	/// copy road, and only meaningful there.
	bool multidraw_fb_copy = false;

	/// The database's copyRoadMaximumBlendingLevel for the running title, or -1 when it asks for
	/// nothing -- which is every title but one, and every title on a build without the mobile
	/// overlay.
	int title_cap = -1;

	/// EmuCore/GS AccurateBlendingUnit as configured, after the rest of the database has had its
	/// say. The cap only ever lowers this.
	int configured_level = 0;
};

// Returns true when a draw that reads the destination pays for the read on this road -- a copy of
// the target, or a pipeline barrier -- rather than getting it from the driver for nothing.
constexpr bool DestinationReadCostsPerDraw(const GSCopyRoadBlendingInputs& in)
{
	switch (in.road)
	{
		case GSSelfReadRoad::InPassOrdered:
			// The driver orders the read and emits nothing: the in-tile fetch, and the declared
			// feedback loop on a driver build measured to order one. Free, and the better picture
			// stays.
			return false;

		case GSSelfReadRoad::InPassBarrier:
			// A barrier per draw. Cheap on the immediate-mode GPUs the barrier was designed for,
			// and on a tiler a per-draw wait for the pipeline to drain -- +40% on Splashdown on an
			// Adreno 740. The cap is in the mobile overlay, so which of those two a device is
			// is already decided by whether it has the entry at all.
			return true;

		case GSSelfReadRoad::Copy:
		default:
			// Copies, but on a GPU where a copy is just a copy.
			return !in.multidraw_fb_copy;
	}
}

// The blending accuracy the renderer should actually run at.
//
// Returns configured_level unchanged unless BOTH halves hold: the title asked for a cap, and this
// device pays for its destination read per draw. It never raises the level -- a title asking for a
// cap higher than the player's setting gets nothing, which is what "maximum" means.
constexpr int CopyRoadBlendingLevel(const GSCopyRoadBlendingInputs& in)
{
	if (in.title_cap < 0)
		return in.configured_level;

	if (!DestinationReadCostsPerDraw(in))
		return in.configured_level;

	return (in.configured_level < in.title_cap) ? in.configured_level : in.title_cap;
}

// The roads that must not move, by name. Splashdown's entry is present in all of them.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::InPassOrdered, .title_cap = 0, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel(
				  {.road = GSSelfReadRoad::Copy, .multidraw_fb_copy = true, .title_cap = 0, .configured_level = 1}) == 1);

// The road the picture was judged on, and the road measured byte-identical to it.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::Copy, .title_cap = 0, .configured_level = 1}) == 0);
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::InPassBarrier, .title_cap = 0, .configured_level = 1}) == 0);

// A title that asked for nothing is untouched on every road, which is every title but one.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::Copy, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::InPassBarrier, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::InPassOrdered, .configured_level = 1}) == 1);

// The cap lowers and never raises.
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::Copy, .title_cap = 3, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.road = GSSelfReadRoad::Copy, .title_cap = 3, .configured_level = 5}) == 3);

// The road predicate itself, so the four destination reads named in the comment above are two
// distinct answers here: the two that cost something per draw, and the two that do not.
static_assert(DestinationReadCostsPerDraw({.road = GSSelfReadRoad::Copy}));
static_assert(DestinationReadCostsPerDraw({.road = GSSelfReadRoad::InPassBarrier}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoad::InPassOrdered}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoad::Copy, .multidraw_fb_copy = true}));

// And the same statement made through the road read-back, which is how GS.cpp reaches it: the bits
// a backend publishes, in the order GSDevice::FeatureSupport carries them.
static_assert(DestinationReadCostsPerDraw({.road = GSSelfReadRoadFromPublishedBits(false, false, false)}));
static_assert(DestinationReadCostsPerDraw({.road = GSSelfReadRoadFromPublishedBits(false, true, false)}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoadFromPublishedBits(true, true, false)}));
static_assert(!DestinationReadCostsPerDraw({.road = GSSelfReadRoadFromPublishedBits(false, true, true)}));
