// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/SW/GSScanlineEnvironment.h"

// The GS interpolates a BLOCK of pixels at a time, and the block is not our
// vector register.
//
// Sony's own internals documentation describes the drawing hardware as sixteen
// pixel engines that the DDA feeds in parallel -- two rows by eight columns
// without texture mapping, two rows by four columns with it.  So the horizontal
// span of one DDA step is a property of the DRAW: eight pixels when TME is off,
// four when it is on.
//
// Our software scanline already walks in exactly that shape.  It seeds at the
// span's first pixel, adds a truncated per-lane offset inside the vector, and
// adds one truncated step per vector, with the vectors aligned to absolute
// screen x -- a blocked truncating DDA.  What it got wrong is the WIDTH: it took
// it from `sizeof(VectorF)`, so the same draw walked in fours on a NEON or SSE4
// build and in eights on an AVX2 one.  Each host was therefore right for half
// the draws and wrong for the other half, and the goldens depended on which
// machine produced them.
//
// The width curve measured against an SCPH-30001 (gs-interp sections 4/5/6,
// 66,912 gouraud readings, every case TME=0 and so predicted 8) peaks on powers
// of two and peaks globally at eight -- 1:42.31 2:69.38 3:80.70 4:88.80 5:85.65
// 6:85.04 7:87.50 8:90.20 9:85.55 10:86.39 12:85.66 16:83.94 32:82.63 64:81.90.
// A structure that prefers 4 and 8 over 5, 6, 7, 9 and 10 is a block, not a
// fitting artefact.  The section with steep gradients over 500 pixels -- the one
// with room for a block boundary to show -- prefers 8 by three points.
//
// The two independent readings of "ten fractional bits" are the same reading: a
// block of width W stepping on our 2^-7 colour grid has an effective per-pixel
// gradient resolution of 2^-(7+log2 W), which at W=8 is the 2^-10 gs-interp
// fitted for depth.  At W=4 it is 2^-9, one bit coarse, and the missing bit was
// the block width.
//
// What the console has NOT been asked, and what is therefore taken on the
// documentation alone: the 2x4 textured block (no gouraud case in any capture we
// own runs with TME on), the vertical half of the block, and whether a span
// starting part-way into a block re-seeds at the block base or at its own first
// pixel.  We keep our own answer to the last one -- offsets are measured from
// the seed pixel -- because that is what the renderer already did and no
// measurement distinguishes them.  Note the consequence: the four-wide textured
// case is unchanged on a four-lane host, so this makes x86 agree with ARM rather
// than moving the reference.

// ⚠️ HALF DONE. The ARM64 generators and the C++ reference take the width from
// here; the x86 generators still take it from the register file, so an SSE4
// build's reference path and its generated path now disagree on an untextured
// gouraud draw, and an AVX2 build still walks eights through a textured one.
// Neither is measurable on this box. Until they are converted, x86 output is not
// the ARM output and the goldens are ARM goldens.

/// Horizontal span of one DDA step, in pixels.
__forceinline static constexpr int GSBlockWalkWidth(GSScanlineSelector sel)
{
	return sel.tfx == TFX_NONE ? 8 : 4;
}

/// Whether the block is wider than the host vector, so that one block takes two
/// vectors and the per-vector step alternates between two values.  Only ever
/// true for an untextured draw on a four-lane host.
__forceinline static constexpr bool GSBlockWalkIsSplit(GSScanlineSelector sel, int vlen)
{
	return GSBlockWalkWidth(sel) > vlen;
}

// The walk, written out once.  With W the block width, x0 the span's first
// pixel, s = x0 & (W-1) its position inside its block, and g the per-pixel
// gradient on the attribute's fixed-point grid:
//
//     value(x) = trunc(seed) + B*trunc(g*W) + trunc(g*(m - s))
//     B = (x - xb0) div W,  m = (x - xb0) mod W,  xb0 = x0 & ~(W-1)
//
// which for W == vlen is bit-for-bit what the scanline did before.  The two
// cases that move:
//
//   W > vlen (untextured, four-lane host).  One block is two vectors, B advances
//   every second vector, and m jumps by vlen between them, so the difference
//   between consecutive vectors alternates between A and trunc(g*W) - A.  Both
//   are precomputed per (s, phase) into GSScanlineLocalData::dw and the scanline
//   just walks the pair.
//
//   W < vlen (textured, eight-lane host).  One vector is two blocks: the upper
//   half carries an extra trunc(g*W) and the per-vector step is two of them.
