// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Which draws read a texture one texel at a time, as a colour.
//
// Katamari Damacy colours most of its world from a 128x128 atlas of flat swatches. A wall is one
// triangle fan whose texture coordinates run from (65.0, 67.7) to (70.5, 71.3): 5 by 4 texels
// inside a 5x5 swatch of one colour, stretched over 180 by 225 pixels under a nearest sampler. The
// game relies on every texel it reads being that colour and on the neighbouring swatch never
// leaking in. A generated upscale cannot promise either at the swatch border. It writes new, in
// between values there, one per 2x texel, and at 32 pixels per texel each of them is a visible
// block. (A pack that anti-aliases the swatch edges would show the same.)
//
// The rest of that game is better upscaled, so the draws have to be told apart, not the textures.
// Rule: a draw reads texels as colours, and uses the guest's own texels in place of a generated
// upscale, when
//
//   * its sampler is NEAREST. A bilinear draw interpolates between texel centres and a 2x texture
//     only refines that.
//   * it is NOT MIPMAPPED, so the texture it falls back to is one level.
//   * half or more of its area, in native pixels, magnifies the texture by at least 8 pixels per
//     texel on both axes. At that size a 2x texel is a 4x4 pixel block of its own, and any
//     difference between neighbouring ones shows. Below about 2 pixels per texel a 2x texel is one
//     pixel and the extra detail is the point of upscaling.
//
// A draw can mix both uses, and takes one texture, so the vote is by area. A flat colour primitive,
// whose texture coordinates are all equal, magnifies without limit and counts. A patch stretched
// that far shows the native texels either way, so the fallback loses no sharpness.
//
// See gs_texture_upscale_support_tests.cpp.

/// Native pixels per texel at which a nearest sampled primitive counts as reading texels as
/// colours, on each axis.
constexpr float GSTexelAddressedMagnification = 8.0f;

/// Whether a primitive's bounding box covers at least GSTexelAddressedMagnification native pixels
/// per texel on both axes. `du` and `dv` are the extent of its texture coordinates in texels, `dx`
/// and `dy` the extent of its vertices in native pixels. A texture coordinate extent of zero (every
/// vertex on the same texel position) passes at any size. NaN, or a negative extent, does not pass.
constexpr bool GSPrimitiveMagnifiesTexels(float du, float dv, float dx, float dy)
{
	return du >= 0.0f && dv >= 0.0f && dx >= 0.0f && dy >= 0.0f && dx >= du * GSTexelAddressedMagnification &&
	       dy >= dv * GSTexelAddressedMagnification;
}

/// The area vote over the primitives of one draw, in native pixels squared.
struct GSTexelAddressedVote
{
	double magnified_area = 0.0;
	double total_area = 0.0;

	/// One primitive: its area, and whether it magnifies texels. Empty and NaN areas are ignored.
	constexpr void Add(double area, bool magnified)
	{
		if (!(area > 0.0))
			return;

		total_area += area;
		if (magnified)
			magnified_area += area;
	}

	/// True when half or more of the draw's area magnifies texels. An empty draw does not.
	constexpr bool Passes() const { return total_area > 0.0 && magnified_area * 2.0 >= total_area; }
};
