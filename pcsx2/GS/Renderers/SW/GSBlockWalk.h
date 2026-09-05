// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/SW/GSScanlineEnvironment.h"

// The GS interpolates a BLOCK of pixels at a time, and the block is not our
// vector register.  The horizontal span of one DDA step is EIGHT pixels, on every
// draw, textured or not.
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
// console (gs-walk2, SCPH-30001, 2026-09-05, two byte-identical run pairs).  One
// of the answers is the width above, one changes what the code is known to be,
// and the third is a defect we do not model.
//
//   THE TEXTURED BLOCK.  Turning TME on does change silicon's picture -- 1,664 of
//   7,136 readings on one gouraud gradient, 23.32%, identically on two gradient
//   sets and two console pairs, with a MODULATE pass-through control exact.  What
//   it does NOT do is narrow the block.  This walk shipped a textured width of
//   four on the strength of a document rather than a measurement, and the console
//   refuses it: scoring gs-walk2's width section under the corrected gradient
//   splits by TME into an untextured arm at 93.0% and a textured arm at 92.6%,
//   and the same model scored at eight puts the textured arm at 96.9%.  Two
//   errors were partly cancelling -- the gradient was wrong in the direction that
//   hid a block half the width it should be -- so correcting the gradient is what
//   made the width measurable, and correcting the width is what this is.
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
// See hardware-oracle/captures/gs-walk2/RESULT.md.  It has since been fitted on a
// shape sweep and landed; that is the gradient the width above is measured under.

// ⚠️ HALF DONE. The ARM64 generators and the C++ reference take the width from
// here; the x86 SSE4 generators still take it from the register file, so an SSE4
// build's reference path and its generated path disagree on any gouraud draw.
// That is not measurable on this box. Until they are converted, x86 output is not
// the ARM output and the goldens are ARM goldens.  An AVX2 build's vector IS the
// block, so it needs no split and it is now right by arithmetic on both arms.

/// Horizontal span of one DDA step, in pixels.
__forceinline static constexpr int GSBlockWalkWidth([[maybe_unused]] GSScanlineSelector sel)
{
	return 8;
}

/// Whether the block is wider than the host vector, so that one block takes two
/// vectors and the per-vector step alternates between two values.  True for every
/// draw on a four-lane host, false on an eight-lane one.
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
// which for W == vlen is bit-for-bit what the scanline did before, and that is an
// eight-lane host.  On a FOUR-lane one, one block is two vectors: B advances every
// second vector and m jumps by vlen between them, so the difference between
// consecutive vectors alternates between A and trunc(g*W) - A.  Both are
// precomputed per (s, phase) into GSScanlineLocalData::dw and the scanline just
// walks the pair.
//
// WHAT TAKES THE BLOCK, and what does not.  Truncation is the only thing that
// makes a block observable -- with an exact step, W blocks of one and one block of
// W land on the same value -- so an attribute the walk does not truncate has no
// block in it and keeps stepping one VECTOR at a time.  That is depth, carried in
// double, and the perspective texture coordinate, carried in float; neither moves
// with the width.
//
// The AFFINE texture coordinate is truncated and could carry one, and it does not:
// it keeps its per-vector step.  The console evidence above is the colour walk's,
// on colour readings, and no capture we own has swept the coordinate's own width
// -- while gs-shade has measured the coordinate itself to a sixteenth of a texel
// and our answer agrees with silicon on 13,324 of 13,336 readings.  Widening a
// walk that is already right, on no measurement, is how the four above got here.
