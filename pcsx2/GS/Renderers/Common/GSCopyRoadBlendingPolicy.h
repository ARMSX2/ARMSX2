// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// What blending accuracy a title renders at on a device that has to COPY the render target in
// order to read it.
//
// Accurate blending above Minimum makes a draw read the destination colour. How much that costs is
// not a property of the setting, it is a property of the device's destination read, and the four
// roads are a long way apart -- GSRenderer's device-loss report names them in these same words:
//
//   * in-tile framebuffer fetch -- the read is a subpassLoad out of tile memory. Free.
//   * texture barrier -- the read is a real sample of the live attachment, ordered by a barrier.
//     Cheap on an immediate-mode GPU, which is where the barrier exists.
//   * render-target copy per primitive group -- the no-barrier fallback on an immediate-mode GPU
//     (D3D11, desktop GL). A blit is a blit there.
//   * render-target copy per DRAW -- the no-barrier fallback on a tiler. Each read ends the render
//     pass, stores the tile, copies the target and starts a new pass. This is the expensive one,
//     and it is where every Adreno under ARMSX2 #442 sits.
//
// On that last road the price is not marginal. Lane Q-native measured Splashdown (SLUS-20686) on
// an SD865 under Turnip at native scale: 1,169 of its 2,690 draws a frame read the render target,
// which is 1,192 render passes and 1,177 target copies per frame, and a 32.18 ms p95 against a
// 16.67 ms budget. Holding that one title's blending accuracy down to Minimum collapses it to 24
// passes and 8 copies and 23.70 ms -- 8.48 ms, 26% of the frame, off one setting. At 2x the same
// change is worth 1.20 ms, because the frame has become fill-bound and the saving is on the
// submission side (lane F1).
//
// It costs picture. Blending accuracy is exactly what the level controls, so this is not a free
// win being withheld by caution: about 30% of Splashdown's frame moves, by up to 40 levels out of
// 255, across the water spray behind the boat. Lane F1 rendered the flips on the device and the
// owner judged the Minimum picture fine on 2026-09-24. That is the authority for this file; the
// standing rule is that a speed change whose only cost is moved pixels is the owner's call on a
// flip corpus, and he made it.
//
// So the behaviour is: ON A DEVICE THAT MUST COPY THE RENDER TARGET TO READ IT, SPLASHDOWN RENDERS
// WITH MINIMUM BLENDING ACCURACY. Everywhere else it renders exactly as it did.
//
// The scope is the point of the file. The owner passed a picture as rendered on the copy road, and
// the cost the picture is buying exists only there. On a Mali with in-tile fetch the read is free;
// on the M2, and on every desktop GPU, the barrier is cheap; and the Adreno campaign's declared-
// read road (lanes E2/E4) removes the copies outright. Applying the cap on any of those would
// spend the picture and buy nothing. A device fact is therefore part of the decision, not just the
// game.
//
// Which device fact. NOT the driver workaround bit (DriverWorkaround::UseRenderTargetCopyForFeedback)
// that puts Adreno on this road, even though that bit is what motivated the lane. The bit names one
// cause; the road has others -- an explicit OverrideTextureBarriers=0, a GLES part with no fetch
// extension, a future driver entry nobody has written yet -- and a title's picture should not
// depend on which cause landed the device on the road. It is also the difference between a rule
// that can be tested here and one that needs the device: the three facts below are what the backend
// publishes, so the M2 reproduces the Adreno road with a single settings key and every no-change
// case is pinned at compile time.
//
// This retires itself. The three facts are read from the live device every time the renderer opens
// or the settings change, so the day the declared-read road ships and an Adreno keeps its texture
// barriers, the cap stops applying on that device with nothing in this file edited. That is the
// intended end state: the cap exists to pay for a driver workaround, and it should leave when the
// workaround does.
//
// The player still outranks all of it, one layer up. The cap arrives as a GameDB hardware fix
// (copyRoadMaximumBlendingLevel), so claiming accurate_blending_unit for the game -- or turning on
// manual hardware fixes -- makes applyGSHardwareFixes skip it and never set the field this policy
// reads. Nothing here needs to know about that; it sees a title that asked for nothing.
//
// Levels are plain integers, 0 = Minimum through 5 = Maximum, which is the grammar the database
// key already speaks and what keeps this header free of Config.h. See
// gs_copy_road_blending_tests.cpp.

struct GSCopyRoadBlendingInputs
{
	/// The backend reads the destination in tile memory: Vulkan rasterization-order attachment
	/// access, GL_ARM_shader_framebuffer_fetch, Metal programmable blending. The read is free, so
	/// there is nothing here to buy.
	bool framebuffer_fetch = false;

	/// The backend has a texture barrier, so a self-reading draw samples the live attachment and
	/// the pass stays open. The M2 at its defaults and every desktop GPU are here.
	bool texture_barrier = false;

	/// The no-barrier fallback copies the target once per PRIMITIVE GROUP inside one pass rather
	/// than once per draw, which is the shape D3D11 and desktop GL take. It is a worse shape on
	/// paper and a cheap one in practice, because the GPUs that take it are immediate-mode and a
	/// blit costs them a blit -- no pass boundary, no tile store. See GSFramebufferFetchPolicy.h's
	/// GLUsesPerPrimitiveFbCopy for the measurement that separates the two.
	bool multidraw_fb_copy = false;

	/// The database's copyRoadMaximumBlendingLevel for the running title, or -1 when it asks for
	/// nothing -- which is every title but one, and every title on a build without the mobile
	/// overlay.
	int title_cap = -1;

	/// EmuCore/GS AccurateBlendingUnit as configured, after the rest of the database has had its
	/// say. The cap only ever lowers this.
	int configured_level = 0;
};

// Returns true when a draw that reads the render target has to be served from a copy of it, and
// that copy costs a render-pass boundary -- the tiler road, and the only road this policy acts on.
constexpr bool FeedbackReadTakesAPerDrawCopy(const GSCopyRoadBlendingInputs& in)
{
	// The in-tile read. Free, and the road step 2.0's speed win lives on.
	if (in.framebuffer_fetch)
		return false;

	// A real barrier: the read samples the live attachment and the pass survives it.
	if (in.texture_barrier)
		return false;

	// Copies, but on a GPU where a copy is just a copy.
	if (in.multidraw_fb_copy)
		return false;

	return true;
}

// The blending accuracy the renderer should actually run at.
//
// Returns configured_level unchanged unless BOTH halves hold: the title asked for a cap, and this
// device is on the per-draw copy road. It never raises the level -- a title asking for a cap higher
// than the player's setting gets nothing, which is what "maximum" means.
constexpr int CopyRoadBlendingLevel(const GSCopyRoadBlendingInputs& in)
{
	if (in.title_cap < 0)
		return in.configured_level;

	if (!FeedbackReadTakesAPerDrawCopy(in))
		return in.configured_level;

	return (in.configured_level < in.title_cap) ? in.configured_level : in.title_cap;
}

// The three roads that must not move, by name. Splashdown's entry is present in all of them.
static_assert(CopyRoadBlendingLevel(
				  {.framebuffer_fetch = true, .texture_barrier = true, .title_cap = 0, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.texture_barrier = true, .title_cap = 0, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.multidraw_fb_copy = true, .title_cap = 0, .configured_level = 1}) == 1);

// The road the owner judged the picture on.
static_assert(CopyRoadBlendingLevel({.title_cap = 0, .configured_level = 1}) == 0);

// A title that asked for nothing is untouched on that same road, which is every title but one.
static_assert(CopyRoadBlendingLevel({.configured_level = 1}) == 1);

// The cap lowers and never raises.
static_assert(CopyRoadBlendingLevel({.title_cap = 3, .configured_level = 1}) == 1);
static_assert(CopyRoadBlendingLevel({.title_cap = 3, .configured_level = 5}) == 3);

// The road predicate itself, so the four names in the comment above are four distinct answers.
static_assert(FeedbackReadTakesAPerDrawCopy({}));
static_assert(!FeedbackReadTakesAPerDrawCopy({.framebuffer_fetch = true, .texture_barrier = true}));
static_assert(!FeedbackReadTakesAPerDrawCopy({.texture_barrier = true}));
static_assert(!FeedbackReadTakesAPerDrawCopy({.multidraw_fb_copy = true}));
