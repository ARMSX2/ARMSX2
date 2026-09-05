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
// The three things this comment used to list as unasked have now been put to the
// console (gs-walk2, SCPH-30001, 2026-09-05, two byte-identical run pairs).  None
// of the answers changes the code; two of them change what the code is known to
// be, and the third is a defect we do not model.
//
//   THE TEXTURED BLOCK.  Turning TME on does change silicon's picture -- 1,664 of
//   7,136 readings on one gouraud gradient, 23.32%, identically on two gradient
//   sets and two console pairs, with a MODULATE pass-through control exact.  So
//   the width really is a property of the draw.  It is NOT evidence that the
//   textured width is four: scored against this walk, silicon's textured arm fits
//   eight better than four, and the difference has somewhere else to come from
//   (the texture function's own truncation of the vertex colour).  Keeping four
//   here is the documentation's answer, not a measured one.
//
//   THE VERTICAL HALF.  The pairing is exactly two rows.  Eight vertical
//   translations of one primitive: every odd one differs and every even one is
//   identical, so rows k and k+2 agree at every k, which a four-row or eight-row
//   block could not do.  WE HAVE NO VERTICAL STRUCTURE AT ALL and return zero on
//   the same sweep -- a real absence, with the readout proved live independently.
//   That is an open divergence, not a settled question.
//
//   ANCHORING.  Four left edges at one absolute plane give four distinct pictures
//   on silicon, so the value at a pixel carries where the span began, which is
//   what we already do.  Our answer to this one stands measured rather than
//   assumed.
//
// A fourth thing came out of the same probe and is larger than any of them: the
// GRADIENT ITSELF is not a divide.  Swept over twenty-four baselines, silicon's
// colour gradient is the channel delta times a reciprocal truncated to EIGHT
// SIGNIFICANT BITS -- 93.19% of 23,568 readings against 81.97% for the exact
// quotient, improving 136 rows and worsening none, with a sharp peak (78.39% at
// seven bits, 88.76% at nine).  Under that gradient the width curve peaks at
// eight for both arms; under the exact quotient it peaks at two.  It is not
// implemented here because every triangle in that probe has a flat top and a
// vertical left edge, which is the one shape where the gradient IS delta over
// baseline, while SetupTriangle divides by the cross product -- so the capture
// says a reciprocal is truncated to eight bits and does not say which one's.
// See hardware-oracle/captures/gs-walk2/RESULT.md.
//
// Note the consequence of the width as it stands: the four-wide textured case is
// unchanged on a four-lane host, so this makes x86 agree with ARM rather than
// moving the reference.

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
