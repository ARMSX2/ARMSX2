// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The unscaled size of a render target below 1x.
//
// A target's texture is ceil(unscaled * scale) device pixels per axis. Below 1x the vertex transform
// maps the target's unscaled size onto the whole texture (the native-aligned branch of
// GSRendererHW::DetermineVSConfig), so a draw runs at texture size / unscaled size, not at the
// scale. When unscaled * scale is not a whole number the ceil makes the two differ, and the picture
// drifts by up to half a device pixel across the target.
//
// Rule: below 1x a target's unscaled size is rounded up to a multiple of the scale's denominator
// (4 at 0.75x and 0.25x, 2 at 0.5x). unscaled * scale is then whole, the texture is exactly that
// size, and every draw to the target runs at the nominal scale. At 1x and above the size is left as
// it is: 1x is exact, and above 1x the transform uses the nominal scale rather than the ratio.
//
// A scale that is not a multiple of 1/16 gets no rounding.
//
// See gs_target_size_policy_tests.cpp.

/// The multiple a target's unscaled size is rounded up to at this scale: the smallest power of two
/// d up to 16 with scale * d whole, below 1x; 1 otherwise. Multiplying a float by a power of two is
/// exact, so the test is exact.
constexpr int GSTargetSizeAlignment(float scale)
{
	if (!(scale < 1.0f))
		return 1;

	for (int d = 2; d <= 16; d *= 2)
	{
		const float scaled = scale * static_cast<float>(d);
		if (scaled == static_cast<float>(static_cast<int>(scaled)))
			return d;
	}

	return 1;
}

/// A target's unscaled size on one axis, rounded up so that its scaled size is exact.
constexpr int GSAlignTargetSize(int size, float scale)
{
	const int align = GSTargetSizeAlignment(scale);
	return (size + align - 1) & -align;
}

static_assert(GSTargetSizeAlignment(0.75f) == 4);
static_assert(GSTargetSizeAlignment(0.5f) == 2);
static_assert(GSTargetSizeAlignment(0.25f) == 4);
static_assert(GSTargetSizeAlignment(1.0f) == 1);
static_assert(GSTargetSizeAlignment(1.5f) == 1);
static_assert(GSAlignTargetSize(597, 0.75f) == 600);
static_assert(GSAlignTargetSize(640, 0.75f) == 640);
static_assert(GSAlignTargetSize(597, 1.0f) == 597);
static_assert(GSAlignTargetSize(597, 2.0f) == 597);
