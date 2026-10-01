// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// An interpolated depth runs slightly short of its plane on the GS, so a pixel
// whose exact depth lands on an integer stores the integer below it.
//
// The bias is half a step of the 2^-10 grid the step sits on: enough to move a
// value exactly on an integer, too small to move one a step above it.
//
// The two axes differ:
//
//   X  applies from the span's first pixel (even at the vertex) and does NOT
//      follow the gradient's sign.
//
//   Y  applies only after the primitive's first scanline and follows the sign,
//      so a falling gradient runs high.
//
// "First scanline" is the primitive's, not the section's. A triangle without a
// flat edge is walked as two sections; exempting the second section's first row
// would put a one-unit depth seam along the middle vertex's row. (Unverified on
// hardware; only the exemption itself is.)
//
// A primitive with no depth gradient takes no bias. Sprites never come here:
// their depth is an integer and not interpolated.
//
// Applied to the scanline seed, so it costs nothing per pixel and both scanline
// backends inherit it.

static constexpr double kGSDepthWalkBias = 1.0 / 2048.0;

__forceinline static double GSDepthWalkBias(double dscan_z, double dedge_z, bool stepped)
{
	double bias = dscan_z != 0.0 ? kGSDepthWalkBias : 0.0;
	if (dedge_z != 0.0 && stepped)
		bias += dedge_z > 0.0 ? kGSDepthWalkBias : -kGSDepthWalkBias;
	return bias;
}
