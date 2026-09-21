// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cmath>

// Which texel a device pixel reads when a sprite MINIFIES a texture that lives in GS memory and
// the sampler is nearest.
//
// The console evaluates a sprite's texture coordinate once per pixel, so a sprite whose U spans
// two source texels per screen pixel reads every other texel and never displays the ones in
// between. Games rely on that. NASCAR Thunder 2002 draws its distance haze as two half-screen
// sprites reading a 640-texel-wide PSMT8 overlay across 320 pixels, packed so the even texels
// carry the haze alpha and the odd texels are alpha 0. At native the odd ones are unreachable.
//
// Upscaling breaks the arithmetic, not the placement. At 2x the sprite covers twice as many device
// columns and every one of them gets its own sample point, so the texels the console never showed
// land on every second column: the haze disappears there and the scene behind shows through as a
// one-device-pixel picket right across the grandstand. That is the defect the owner judged as
// "banding in background", and no shipped setting touches it -- twenty arms were run and fourteen
// are byte-identical to the base, because every shipped lever is about where the sprite lands or
// how a texel is filtered, and this draw is placed correctly and filtered correctly. The error is
// WHICH texel a device pixel reads. (Lane Q-nascar2x, 2026-09-20.)
//
// So the rule: A SPRITE THAT MINIFIES A GS-MEMORY TEXTURE UNDER A NEAREST SAMPLER READS THE TEXEL
// ITS NATIVE PIXEL WOULD HAVE READ. The 2x2 device pixels of one native pixel all take that native
// pixel's texel, which is exactly the picture native produces, enlarged.
//
// The scope is the point of the file, so each gate is a fact and not a taste:
//
//   * SPRITE, because only a sprite has the one-sample-per-pixel rule in this form. A triangle's
//     texture coordinate is interpolated and a device fragment's own sample point is the honest
//     answer for it.
//   * FROM GS MEMORY, because a render-target source is already at device resolution -- its texels
//     ARE device texels and there is no native grid to go back to.
//   * NEAREST, because a bilinear sprite is already averaging in the texels the console skipped.
//     That is a different picture and not this one's business.
//   * NO MIPMAP, because the level the console picked is a function of this same step, and the two
//     have to be settled together rather than one of them moved on its own.
//   * MINIFYING, at least two source texels per native pixel, because that is when the console
//     starts skipping texels. At a step of one nothing is skipped and there is nothing to restore.
//   * SCALE ABOVE 1, which is where a native pixel has more than one device pixel to disagree
//     about. At native the rule below is the identity, term for term, so 1x cannot move.
//   * THE DEVICE GRID IS THE NATIVE GRID SCALED -- a vertex at native coordinate n has to land at
//     device coordinate n * scale + 0.5, because that and nothing else makes floor(fragment)/scale
//     the fragment's OWN native pixel. Several half-pixel-offset modes deliberately put the
//     vertices somewhere else: GameDB `halfPixelOffset: 4` (Native) places them at
//     scale * (n + 0.5), so at 2x native 0 lands at device 1.0 rather than 0.5. Snapping on that
//     mapping hands the fragment a native pixel that is not its owner. Measured: Katamari Damacy's
//     two dumps are the corpus's Native-mode draws, they land at device 1.0 and 3.0 for native 0
//     and 1, and correcting them is a wash against the software renderer at native (exact
//     agreement 15.1% -> 15.0% on the pixels that move); with the mapping put back to
//     n * scale + 0.5 the same draws go 5.6% -> 19.8% exact and mean error 31.73 -> 17.25. So a
//     draw on another mapping is refused here rather than corrected on the wrong grid. Reading
//     the mapping the renderer actually used, and correcting for it, is a better rule and a
//     different change; this gate is what keeps the wrong answer off the screen until then.
//   * NOT A FIELD RENDER. In field mode (SMODE2.FFMD under an interlaced output) the game draws
//     half-height fields, and at an integer upscale of 2 or more the merge presents that field
//     render as the whole picture rather than weaving it -- because each field line now occupies
//     that many device rows, and those rows carry the display lines BETWEEN the field's own
//     (`field_render_is_whole_picture` in GSRenderer.cpp, `present_field_direct` in
//     GSInterlaceModePolicy.h). Those in-between lines are precisely what this rule removes: it
//     gives every device row of a native row the same texel. So in field mode the device picture
//     is deliberately not the native picture enlarged, and the derivation below does not hold.
//     Measured: Armored Core 3 and Shin Onimusha are the corpus's only FFMD dumps, and corrected
//     they move away from the software renderer at native (Armored Core 3, exact agreement
//     15.1% -> 4.9%) -- but with the field composition out of the way the same draws move hard
//     toward it (3.7% -> 38.6% exact, mean error 26.77 -> 6.31). The rule picks the right texel
//     for them and the composition discards it. That composition is another lane's; this gate
//     keeps this rule out of its way. Interlaced OUTPUT is not the condition and does not gate
//     anything -- NASCAR's output is interlaced too, and its targets are whole 640x448 frames.
//
// A note on what is NOT gated, and why. The step is read per AXIS: NASCAR's draws are 2:1 in U and
// 1:1 in V, so U snaps and V is left alone. And the step is read per SPRITE and the draw is refused
// unless every sprite in it walks the texture at the same rate, because the shader carries one step
// for the whole draw. Sprite PHASE is free -- the correction below is derived from the fragment's
// own position, not from an anchor, so sprites at different offsets each snap to their own grid.
//
// The arithmetic, once, because the shader repeats it and the two must not drift.
//
// The console evaluates a sprite's texture coordinate at the pixel's INTEGER coordinate, not at its
// centre, and the renderer carries that convention up the scale: device pixel j samples where
// native coordinate j/S samples. (Measured, not assumed -- NASCAR's right-half draw runs U from 0.5
// over 2 texels per native pixel from screen column 320, and device column 700 reads texel 60 while
// 701 reads 61, which is 0.5 + 2*(j/S - 320) floored and nothing else. Sampling at the centre would
// put column 700 half a texel lower, and the first build written that way lost the haze on every
// column, because half a texel is the whole difference between the texels the console shows and the
// ones it never does.)
//
// So the device pixel samples at j/S, its owner native pixel sampled at floor(j/S), and the
// correction in native pixels is
//
//     floor(j/S) - j/S
//
// which is the texture coordinate's own step times that. Two things fall out. At S = 1 it is
// floor(j) - j, zero for every pixel: the identity, by construction and not by measurement. And
// floor(j/S) is the ownership rule the dither and SCANMSK paths use -- native pixel k owns device
// pixels [ceil(kS), ceil((k+1)S)) -- so a fractional scale lands on the same owners those do, as
// long as j is the INTEGER pixel index and the divide is a real divide.
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
	// A right-to-left sprite is the same sprite read the other way round. Normalising the sign here
	// is what lets every comparison below assume a positive denominator.
	return (delta_pixels < 0) ? GSNativeTexelStep{-delta_texels, -delta_pixels} :
	                            GSNativeTexelStep{delta_texels, delta_pixels};
}

/// True when one native pixel covers at least two source texels on this axis -- the condition under
/// which the console skips texels, so the device has to skip the same ones.
constexpr bool GSAxisMinifiesToNativeTexels(const GSNativeTexelStep& step)
{
	if (step.pixels <= 0)
		return false;

	const int magnitude = (step.texels < 0) ? -step.texels : step.texels;
	return magnitude >= 2 * step.pixels;
}

/// True when two sprites of one draw walk the texture at the same rate. Cross-multiplied rather
/// than divided, so two steps that are the same ratio in different terms compare equal exactly.
constexpr bool GSNativeTexelStepsAgree(const GSNativeTexelStep& a, const GSNativeTexelStep& b)
{
	return static_cast<long long>(a.texels) * b.pixels == static_cast<long long>(b.texels) * a.pixels;
}

/// Two device-pixel quantities are the same to within a sixty-fourth of a device pixel. The
/// mappings that break the rule's premise miss by half a device pixel or more, and the vertex
/// offset is built out of a reciprocal, so a draw whose algebra says 0.5 can evaluate to 0.4999.
constexpr bool GSDeviceGridWithinSlack(float a, float b)
{
	constexpr float slack = 1.0f / 64.0f;
	return (a - b) <= slack && (b - a) <= slack;
}

/// Whether a vertex at native coordinate n lands at device coordinate n * scale + 0.5 -- the
/// mapping the correction is derived on, and the one gate that cannot be read off the draw itself.
/// `grid_scale` is device pixels per native pixel as the vertex transform actually spells it, and
/// `grid_offset` is where native coordinate 0 lands. Both are taken from the transform rather than
/// assumed, so a target whose texture is larger than its native size scaled fails here too.
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

	/// PRIM.FST: the draw addresses the texture in texels. The per-sprite step is then the exact
	/// ratio of two integers. An STQ draw's step is not -- the vertex trace carries s * TW without
	/// the q divide -- so those are left alone rather than snapped with a step that is only right
	/// when q is 1.
	bool texel_coordinates = false;

	/// The sampler is nearest in both directions.
	bool nearest = false;

	/// The draw selects a mip level, manually or automatically.
	bool mipmapped = false;

	/// The frame is a FIELD render -- SMODE2.FFMD under an interlaced output -- so the merge does
	/// not present the device picture as the native picture enlarged.
	bool field_render = false;

	/// Device pixels per native pixel for this draw's render target.
	float scale = 1.0f;

	/// The step every sprite in the draw agreed on, per axis.
	GSNativeTexelStep step_u;
	GSNativeTexelStep step_v;
};

/// Everything the rule asks of the draw except the step, which costs a walk over the draw's
/// sprites to find out. Split out so that walk only happens where it could matter.
constexpr bool GSDrawCouldSampleOnTheNativeTexelGrid(const GSNativeTexelGridInputs& in)
{
	if (!in.sprite || !in.texture_from_memory || !in.texel_coordinates || !in.nearest ||
		in.mipmapped || in.field_render)
		return false;

	// Not `>= 1.0f`: at native every term of the correction cancels anyway, and refusing here keeps
	// 1x on exactly the shader permutations it had.
	return in.scale > 1.0f;
}

/// Whether this draw reads its texture on the native pixel grid instead of the device one.
constexpr bool GSSpriteSamplesOnTheNativeTexelGrid(const GSNativeTexelGridInputs& in)
{
	if (!GSDrawCouldSampleOnTheNativeTexelGrid(in))
		return false;

	return GSAxisMinifiesToNativeTexels(in.step_u) || GSAxisMinifiesToNativeTexels(in.step_v);
}

/// The step the shader is handed for one axis, in texels per native pixel. Zero on an axis that
/// does not minify, which makes the correction vanish there -- a per-axis off switch that costs no
/// second selector bit and no branch in the shader.
constexpr float GSNativeTexelGridStep(const GSNativeTexelStep& step)
{
	return GSAxisMinifiesToNativeTexels(step) ?
	           (static_cast<float>(step.texels) / static_cast<float>(step.pixels)) :
	           0.0f;
}

/// The correction added to a texture coordinate on one axis, in the same units as `step`.
/// `device_coord` is the fragment coordinate, pixel + 0.5; the floor recovers the integer pixel
/// index, which is what the sampling convention is written in. The shaders run this same
/// expression; keeping one definition is what lets the tests speak for all four of them.
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

// The same draw at native scale. Nothing to disagree about, so it never reaches the shader.
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.scale = 1.0f,
	.step_u = {640 * 16, 320 * 16},
	.step_v = {448 * 16, 448 * 16}}));

// The ordinary upscaled blit: one texel per pixel, magnified or 1:1. Untouched, which is most of
// every frame in the corpus.
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.scale = 2.0f,
	.step_u = {320 * 16, 320 * 16},
	.step_v = {224 * 16, 224 * 16}}));

// Each gate on its own, against the NASCAR draw, so a widening shows up here first.
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

// The axes are independent: NASCAR's V is 1:1 and gets a zero step, so the shader's V term is zero
// and the row a fragment reads is the one it reads today.
static_assert(GSNativeTexelGridStep({640 * 16, 320 * 16}) == 2.0f);
static_assert(GSNativeTexelGridStep({448 * 16, 448 * 16}) == 0.0f);

// Just under two texels per pixel is not minifying enough to skip a texel every pixel, and is left
// alone. The rule is deliberately not "step > 1".
static_assert(!GSAxisMinifiesToNativeTexels({319, 160}));
static_assert(GSAxisMinifiesToNativeTexels({320, 160}));

// A mirrored sprite minifies by the magnitude and keeps its sign in the step.
static_assert(GSAxisMinifiesToNativeTexels(GSMakeNativeTexelStep(-640 * 16, 320 * 16)));
static_assert(GSNativeTexelGridStep(GSMakeNativeTexelStep(-640 * 16, 320 * 16)) == -2.0f);
static_assert(GSNativeTexelGridStep(GSMakeNativeTexelStep(640 * 16, -320 * 16)) == -2.0f);

// A degenerate sprite -- zero pixels wide -- has no step to speak of.
static_assert(!GSAxisMinifiesToNativeTexels({640 * 16, 0}));

// Two sprites of one draw at the same rate in different terms, and two that disagree.
static_assert(GSNativeTexelStepsAgree({640 * 16, 320 * 16}, {320 * 16, 160 * 16}));
static_assert(!GSNativeTexelStepsAgree({640 * 16, 320 * 16}, {639 * 16, 320 * 16}));

// A field render is refused whatever else the draw looks like: NASCAR's own sprite, drawn in field
// mode, would have the merge present the field as the whole picture and this rule would take the
// in-between display lines back out again.
static_assert(!GSSpriteSamplesOnTheNativeTexelGrid({.sprite = true,
	.texture_from_memory = true,
	.texel_coordinates = true,
	.nearest = true,
	.field_render = true,
	.scale = 2.0f,
	.step_u = {640 * 16, 320 * 16},
	.step_v = {448 * 16, 448 * 16}}));

// The measured mappings, at 2x. NASCAR, Armored Core 3 and Shin Onimusha put native 0 at device
// 0.5; Katamari Damacy's Native half-pixel-offset mode puts it at 1.0, which is the refusal.
static_assert(GSDeviceGridIsNativeGridScaled(2.0f, 0.5f, 2.0f));
static_assert(!GSDeviceGridIsNativeGridScaled(2.0f, 1.0f, 2.0f));

// The slack is there for the reciprocal, not for half a pixel: an Armored Core 3 draw evaluates to
// 0.4999 and passes, and nothing a quarter of a pixel out does.
static_assert(GSDeviceGridIsNativeGridScaled(2.0f, 0.4999f, 2.0f));
static_assert(!GSDeviceGridIsNativeGridScaled(2.0f, 0.75f, 2.0f));

// Native scale is the identity mapping, and is refused earlier anyway by the scale gate.
static_assert(GSDeviceGridIsNativeGridScaled(1.0f, 0.5f, 1.0f));

// A target whose texture is bigger than its native size scaled walks the device grid at its own
// rate, so floor(fragment)/scale is not the fragment's native pixel there either.
static_assert(!GSDeviceGridIsNativeGridScaled(2.125f, 0.5f, 2.0f));
