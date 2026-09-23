// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cmath>

// Which texel a device pixel reads when a sprite MINIFIES a GS-memory texture under a nearest
// sampler.
//
// The console evaluates a sprite's texture coordinate once per pixel, so a sprite stepping two
// texels per pixel never displays the texels in between, and games rely on that. NASCAR Thunder
// 2002 draws its distance haze from a 640-texel PSMT8 overlay across 320 pixels, with haze alpha in
// the even texels and alpha 0 in the odd ones. Upscaled, every device column gets its own sample
// point, the odd texels land on every second column, and the haze shows as a one-pixel picket.
// No placement or filtering setting affects this; the error is which texel is read.
//
// Rule: such a sprite reads the texel its native pixel would have read, so all device pixels of one
// native pixel take that native pixel's texel -- the native picture, enlarged.
//
// Gates:
//
//   * SPRITE: a triangle's coordinate is interpolated, and the fragment's own sample point is
//     correct for it.
//   * FROM GS MEMORY: a render-target source is already at device resolution; there is no native
//     grid to return to.
//   * NEAREST: a bilinear sprite already averages in the skipped texels.
//   * NO MIPMAP: the console's level depends on the same step, so the two would have to change
//     together.
//   * MINIFYING, at least two texels per native pixel, which is when texels get skipped.
//   * SCALE ABOVE 1. At native the correction is the identity term for term.
//   * THE DEVICE GRID IS THE NATIVE GRID SCALED: native n must land at device n * scale + 0.5, or
//     floor(fragment)/scale is not the fragment's own native pixel. Some half-pixel-offset modes
//     place vertices elsewhere (GameDB `halfPixelOffset: 4` puts native 0 at device 1.0 at 2x), and
//     correcting on that grid picks the wrong owner. Those draws are refused rather than corrected
//     on the wrong grid.
//   * NOT A FIELD RENDER (SMODE2.FFMD under interlaced output). At integer scale >= 2 the merge
//     presents a field render as the whole picture (`field_render_is_whole_picture` in
//     GSRenderer.cpp, `present_field_direct` in GSInterlaceModePolicy.h), so the extra device rows
//     carry the display lines between the field's own. This rule would flatten exactly those rows.
//     Interlaced output alone does not gate anything.
//
// Not gated: the step is read per AXIS (NASCAR is 2:1 in U and 1:1 in V, so only U snaps). Every
// sprite in the draw must share one step, because the shader carries one step per draw. Sprite
// phase is free: the correction comes from the fragment's own position, not an anchor.
//
// The arithmetic, which the shaders repeat and must not drift from:
//
// The console samples at the pixel's INTEGER coordinate, not its centre, and the renderer keeps
// that convention: device pixel j samples where native coordinate j/S samples. Sampling at the
// centre is half a texel off, which on NASCAR selects the skipped texels.
//
// The owner native pixel sampled at floor(j/S), so the correction in native pixels is
//
//     floor(j/S) - j/S
//
// times the coordinate's step. At S = 1 it is zero for every pixel. floor(j/S) is the same
// ownership rule the dither and SCANMSK paths use -- native pixel k owns device pixels
// [ceil(kS), ceil((k+1)S)) -- so fractional scales agree with them, provided j is the INTEGER pixel
// index and the divide is a real divide.
//
// See gs_native_texel_grid_tests.cpp.

/// One axis of a sprite's texture step, held as the exact integer ratio the GS hands us: UV and XY
/// both arrive in sixteenths, so the ratio of the two deltas is texels per native pixel with no
/// divide and no tolerance. `pixels` is normalised positive; `texels` keeps its sign, because a
/// sprite may read its source mirrored.
struct GSNativeTexelStep
{
	int texels = 0;
	int pixels = 0;
};

/// A sprite's step on one axis, from the two corner vertices' raw deltas.
constexpr GSNativeTexelStep GSMakeNativeTexelStep(int delta_texels, int delta_pixels)
{
	// Normalise to a positive denominator; a right-to-left sprite is the same sprite reversed.
	return (delta_pixels < 0) ? GSNativeTexelStep{-delta_texels, -delta_pixels} :
	                            GSNativeTexelStep{delta_texels, delta_pixels};
}

/// True when one native pixel covers at least two source texels on this axis, i.e. the console
/// skips texels.
constexpr bool GSAxisMinifiesToNativeTexels(const GSNativeTexelStep& step)
{
	if (step.pixels <= 0)
		return false;

	const int magnitude = (step.texels < 0) ? -step.texels : step.texels;
	return magnitude >= 2 * step.pixels;
}

/// True when two sprites walk the texture at the same rate. Cross-multiplied so equal ratios in
/// different terms compare exactly.
constexpr bool GSNativeTexelStepsAgree(const GSNativeTexelStep& a, const GSNativeTexelStep& b)
{
	return static_cast<long long>(a.texels) * b.pixels == static_cast<long long>(b.texels) * a.pixels;
}

/// Equal to within 1/64 device pixel. Wrong mappings miss by half a pixel or more; the slack only
/// absorbs reciprocal rounding (0.5 evaluating to 0.4999).
constexpr bool GSDeviceGridWithinSlack(float a, float b)
{
	constexpr float slack = 1.0f / 64.0f;
	return (a - b) <= slack && (b - a) <= slack;
}

/// Whether native coordinate n lands at device n * scale + 0.5, the mapping the correction assumes.
/// `grid_scale` and `grid_offset` (where native 0 lands) come from the actual vertex transform, so
/// a target larger than its native size scaled also fails.
constexpr bool GSDeviceGridIsNativeGridScaled(float grid_scale, float grid_offset, float scale)
{
	return GSDeviceGridWithinSlack(grid_scale, scale) && GSDeviceGridWithinSlack(grid_offset, 0.5f);
}

struct GSNativeTexelGridInputs
{
	/// The draw is sprite class.
	bool sprite = false;

	/// The source is GS memory rather than a render target, so its texels are the console's texels.
	bool texture_from_memory = false;

	/// PRIM.FST: texel addressing, so the step is an exact integer ratio. STQ draws are excluded
	/// because the vertex trace carries s * TW without the q divide.
	bool texel_coordinates = false;

	/// The sampler is nearest in both directions.
	bool nearest = false;

	/// The draw selects a mip level, manually or automatically.
	bool mipmapped = false;

	/// SMODE2.FFMD under interlaced output; the merge does not present the native picture enlarged.
	bool field_render = false;

	/// Device pixels per native pixel for this draw's render target.
	float scale = 1.0f;

	/// The step every sprite in the draw agreed on, per axis.
	GSNativeTexelStep step_u;
	GSNativeTexelStep step_v;
};

/// Every gate except the step, which needs a walk over the draw's sprites. Split out so that walk
/// only happens where it could matter.
constexpr bool GSDrawCouldSampleOnTheNativeTexelGrid(const GSNativeTexelGridInputs& in)
{
	if (!in.sprite || !in.texture_from_memory || !in.texel_coordinates || !in.nearest ||
		in.mipmapped || in.field_render)
		return false;

	// Refuse 1x so it keeps its existing shader permutations; the correction is zero there anyway.
	return in.scale > 1.0f;
}

/// Whether this draw reads its texture on the native pixel grid instead of the device one.
constexpr bool GSSpriteSamplesOnTheNativeTexelGrid(const GSNativeTexelGridInputs& in)
{
	if (!GSDrawCouldSampleOnTheNativeTexelGrid(in))
		return false;

	return GSAxisMinifiesToNativeTexels(in.step_u) || GSAxisMinifiesToNativeTexels(in.step_v);
}

/// The shader's step for one axis, in texels per native pixel. Zero on a non-minifying axis, which
/// disables the correction there without a selector bit or branch.
constexpr float GSNativeTexelGridStep(const GSNativeTexelStep& step)
{
	return GSAxisMinifiesToNativeTexels(step) ?
	           (static_cast<float>(step.texels) / static_cast<float>(step.pixels)) :
	           0.0f;
}

/// The correction added to a texture coordinate on one axis, in units of `step`. `device_coord` is
/// the fragment coordinate (pixel + 0.5); the floor recovers the integer pixel index. The shaders
/// run the same expression and must match it.
inline float GSNativeTexelGridOffset(float step, float scale, float device_coord)
{
	const float here = std::floor(device_coord) / scale;
	return step * (std::floor(here) - here);
}

// NASCAR Thunder 2002's haze sprites: 640 texels across 320 pixels in U, 448 across 448 in V, at
// 2x, nearest, PSMT8 out of GS memory.
static_assert(GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.scale = 2.0f,
	.step_u = {640 * 16, 320 * 16},
	.step_v = {448 * 16, 448 * 16}}));

// The same draw at native scale.
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.scale = 1.0f,
	.step_u = {640 * 16, 320 * 16},
	.step_v = {448 * 16, 448 * 16}}));

// An ordinary 1:1 upscaled blit is untouched.
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.scale = 2.0f,
	.step_u = {320 * 16, 320 * 16},
	.step_v = {224 * 16, 224 * 16}}));

// Each gate on its own, against the NASCAR draw.
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = false,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.scale = 2.0f,
	.step_u = {640 * 16, 320 * 16}}));
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = false,
	.texel_coordinates = true,
	.nearest = true,
	.scale = 2.0f,
	.step_u = {640 * 16, 320 * 16}}));
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = false,
	.nearest = true,
	.scale = 2.0f,
	.step_u = {640 * 16, 320 * 16}}));
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = false,
	.scale = 2.0f,
	.step_u = {640 * 16, 320 * 16}}));
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.mipmapped = true,
	.scale = 2.0f,
	.step_u = {640 * 16, 320 * 16}}));

// Axes are independent: NASCAR's 1:1 V gets a zero step.
static_assert(GSNativeTexelGridStep({640 * 16, 320 * 16}) == 2.0f);
static_assert(GSNativeTexelGridStep({448 * 16, 448 * 16}) == 0.0f);

// The threshold is two texels per pixel, deliberately not "step > 1".
static_assert(!GSAxisMinifiesToNativeTexels({319, 160}));
static_assert(GSAxisMinifiesToNativeTexels({320, 160}));

// A mirrored sprite minifies by the magnitude and keeps its sign in the step.
static_assert(GSAxisMinifiesToNativeTexels(GSMakeNativeTexelStep(-640 * 16, 320 * 16)));
static_assert(GSNativeTexelGridStep(GSMakeNativeTexelStep(-640 * 16, 320 * 16)) == -2.0f);
static_assert(GSNativeTexelGridStep(GSMakeNativeTexelStep(640 * 16, -320 * 16)) == -2.0f);

// A zero-width sprite has no step.
static_assert(!GSAxisMinifiesToNativeTexels({640 * 16, 0}));

// Two sprites of one draw at the same rate in different terms, and two that disagree.
static_assert(GSNativeTexelStepsAgree({640 * 16, 320 * 16}, {320 * 16, 160 * 16}));
static_assert(!GSNativeTexelStepsAgree({640 * 16, 320 * 16}, {639 * 16, 320 * 16}));

// A field render is refused regardless of the rest of the draw.
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.field_render = true,
	.scale = 2.0f,
	.step_u = {640 * 16, 320 * 16},
	.step_v = {448 * 16, 448 * 16}}));

// At 2x: native 0 at device 0.5 passes; the Native half-pixel-offset mode's 1.0 is refused.
static_assert(GSDeviceGridIsNativeGridScaled(2.0f, 0.5f, 2.0f));
static_assert(!GSDeviceGridIsNativeGridScaled(2.0f, 1.0f, 2.0f));

// The slack covers reciprocal rounding, not a quarter pixel.
static_assert(GSDeviceGridIsNativeGridScaled(2.0f, 0.4999f, 2.0f));
static_assert(!GSDeviceGridIsNativeGridScaled(2.0f, 0.75f, 2.0f));

// Native scale is the identity mapping (and refused earlier by the scale gate).
static_assert(GSDeviceGridIsNativeGridScaled(1.0f, 0.5f, 1.0f));

// A target larger than its native size scaled walks the device grid at its own rate.
static_assert(!GSDeviceGridIsNativeGridScaled(2.125f, 0.5f, 2.0f));
