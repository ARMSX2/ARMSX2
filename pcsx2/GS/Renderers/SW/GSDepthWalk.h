// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// An interpolated depth never quite reaches its plane on silicon.
//
// Measured on an SCPH-30001 with the gs-zgrad probe, which drives integer
// landings under a raw depth readback densely and on purpose. On a gradient of
// 1/4 -- where the 2^-10 step truncation is the identity and our walk is
// otherwise exact -- every fourth pixel of every row read one unit HIGHER than
// the console and nothing between them differed at all. The walk runs short of
// the plane, so a pixel whose exact depth lands on an integer stores the
// integer below it.
//
// The bias is half a step of the 2^-10 grid the step already sits on: large
// enough to move a value that lands exactly on an integer, small enough to
// leave a value even one step above it alone.
//
// The two axes carry the shortfall DIFFERENTLY, which the capture separates
// rather than assumes. Both halves were measured by rebuilding and re-running
// the probe, not reasoned about:
//
//   X  present from the span's first pixel -- a primitive with an X gradient
//      reads one low even at the vertex itself -- and NOT following the
//      gradient's sign. Signing it moved gs-zgrad's gridx section from 192
//      wrong pixels to 384.
//
//   Y  accumulates instead. The vertex scanline is exact on silicon (the pure-Y
//      cases differ only on their first row otherwise), and the shortfall
//      points back toward the seed, so a FALLING gradient runs high. Signing
//      this half took the ygrad section from 448 wrong pixels to zero.
//
// A flat triangle is exact on silicon -- 896 of 896 readings -- so the bias
// belongs to the walk and not to the seed, and a primitive with no depth
// gradient takes none of it. Sprites never come here: their depth is carried as
// an integer and never interpolated.
//
// Applied to the scanline seed rather than tested at the store, so it costs
// nothing per pixel and both scanline backends inherit it from one place.

static constexpr double kGSDepthWalkBias = 1.0 / 2048.0;

__forceinline static double GSDepthWalkBias(double dscan_z, double dedge_z, double dy)
{
	double bias = dscan_z != 0.0 ? kGSDepthWalkBias : 0.0;
	if (dedge_z != 0.0 && dy != 0.0)
		bias += dedge_z > 0.0 ? kGSDepthWalkBias : -kGSDepthWalkBias;
	return bias;
}
