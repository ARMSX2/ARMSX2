// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// The GS interpolates a BLOCK of pixels at a time, and the block is not our vector
// register.  The horizontal span of one DDA step is EIGHT pixels, on every draw,
// textured or not.
//
// Our software scanline already walks in that shape: it seeds at the span's first
// pixel, adds a truncated per-lane offset inside the vector, and adds one truncated
// step per vector, with the vectors aligned to absolute screen x -- a blocked
// truncating DDA.  What it got wrong was the WIDTH.  It took that from
// `sizeof(VectorF)`, so the same draw walked in fours on a four-lane host and in
// eights on an eight-lane one, and the goldens depended on which machine produced
// them.
//
// Eight is measured, not assumed.  The gs-interp and gs-walk2 captures (SCPH-30001)
// score a width curve over tens of thousands of gouraud readings; it peaks on powers
// of two and peaks globally at eight, on the textured arm as well as the untextured
// one.  Turning TME on changes what silicon draws but does not narrow the block.
//
// Two things from the same probe are NOT modelled here, deliberately.  Silicon also
// pairs rows two at a time (rows k and k+2 read identically at every k) and we have
// no vertical structure at all -- an open divergence, not a settled question.  And
// the affine texture coordinate is truncated, so it could carry a block, but no
// capture has swept its width; it keeps its per-vector step until one does.

// ⚠️ ARM64 and AVX2 walk eight.  x86 SSE4 walks four, in the JIT and in the C++
// reference alike.
//
// An eight-lane vector IS the block, so an AVX2 build needs no split and is right by
// arithmetic.  On four lanes one block is two vectors and the per-vector step
// alternates, which the ARM64 generators and the C++ reference implement and the x86
// SSE4 generators do not.  So the split below is gated on ARM64: that keeps an SSE4
// build self-consistent between its JIT and its own fallback, at the price of an
// x86 SSE4 software renderer whose colour and fog walk is not the ARM64 one.  Until
// those generators are converted, the goldens are ARM64 goldens.

/// Horizontal span of one DDA step, in pixels.
__forceinline static constexpr int GSBlockWalkWidth()
{
	return 8;
}

/// Whether the block is wider than the host vector, so that one block takes two
/// vectors and the per-vector step alternates between two values.  True on a
/// four-lane ARM64 host, false on an eight-lane one -- and false on x86 SSE4, whose
/// generators have not been converted (see above).
__forceinline static constexpr bool GSBlockWalkIsSplit([[maybe_unused]] int vlen)
{
#ifdef ARCH_ARM64
	return GSBlockWalkWidth() > vlen;
#else
	return false;
#endif
}

// The walk, written out once.  With W the block width, x0 the span's first pixel,
// s = x0 & (W-1) its position inside its block, and g the per-pixel gradient on the
// attribute's fixed-point grid:
//
//     value(x) = trunc(seed) + B*trunc(g*W) + trunc(g*(m - s))
//     B = (x - xb0) div W,  m = (x - xb0) mod W,  xb0 = x0 & ~(W-1)
//
// which for W == vlen is bit-for-bit what the scanline did before, and that is an
// eight-lane host.  On a FOUR-lane one, one block is two vectors: B advances every
// second vector and m jumps by vlen between them, so the difference between
// consecutive vectors alternates between A and trunc(g*W) - A.  Both are precomputed
// per (s, phase) into GSScanlineLocalData::dw and the scanline just walks the pair.
//
// WHAT TAKES THE BLOCK.  Truncation is the only thing that makes a block observable
// -- with an exact step, W blocks of one and one block of W land on the same value --
// so an attribute the walk does not truncate has no block in it and keeps stepping
// one VECTOR at a time.  That is depth, carried in double, and the perspective
// texture coordinate, carried in float.
