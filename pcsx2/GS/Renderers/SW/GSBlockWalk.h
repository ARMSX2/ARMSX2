// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// The GS interpolates a BLOCK of pixels at a time, and the block is not our vector
// register.  The horizontal span of one DDA step is EIGHT pixels only when
// texturing, fog and AA1 are all off, and FOUR if any one of them is on.
//
// Measured on real hardware, and it took three goes.  The first width sweep scored
// a curve over tens of thousands of gouraud readings and peaked at eight on both
// the textured and the untextured arm -- but it was taken with the colour gradient
// still wrong, and the two errors were partly cancelling on the textured arm. With
// the gradient measured (GSColourWalk.h) the same geometry separates the arms
// cleanly, and a later sweep of the rest of the pipeline found fog and AA1
// narrowing the block as well, on an untextured draw, while blending, depth
// testing and the target format do not.
//
// (The walk this file used to describe took its width from sizeof(VectorF), so the
// same draw walked in fours on a four-lane host and in eights on an eight-lane one,
// and the goldens depended on which machine produced them.  That is a fourth thing,
// and it is still wrong to do.)
//
// WHAT TAKES THE BLOCK.  Truncation is the only thing that makes a block observable
// -- with an exact step, W blocks of one and one block of W land on the same value --
// so an attribute the walk does not truncate has no block in it and keeps stepping
// one VECTOR at a time.  That is depth, carried in double, and the perspective
// texture coordinate, carried in float.  The affine texture coordinate IS truncated,
// so it could carry a block, but nothing has swept its width; it keeps its
// per-vector step until something does.
//
// COLOUR AND FOG ARE NOT HERE ANY MORE.  The colour interpolator is more than a
// wider step: the blocks are pinned to absolute screen x with a phase the
// primitive's own anchor decides, the value ramps inside a block on a gradient
// coarser than the one a block steps, and rows come in absolute pairs.
// GSColourWalk.h carries that model, and the tables the scanline reads are built
// from it once per primitive.  Fog rides the same DDA and takes the same width,
// which is why turning fog ON narrows the colour block.
//
// What is left here is the block WIDTH, which the walk takes as a parameter, and
// the reason a four-lane host walks an alternating pair of per-vector steps rather
// than one constant: at eight a block is two vectors, at four it is one and the
// pair comes out a pair of equal steps.

/// Horizontal span of one DDA step, in pixels. EIGHT only when texturing, fog and
/// AA1 are all off; any one of them narrows it to FOUR, and they do not compound.
/// Measured against an untextured and a textured base, one pipeline factor at a
/// time; two and sixteen are both refused outright.
__forceinline static constexpr int GSBlockWalkWidth(bool textured, bool fog, bool aa1)
{
	return (textured || fog || aa1) ? 4 : 8;
}

/// The widest block any draw takes, which is what the scanline's plumbing has to
/// be able to carry.
__forceinline static constexpr int GSBlockWalkWidth()
{
	return GSBlockWalkWidth(false, false, false);
}

/// Whether the block is wider than the host vector, so that one block takes two
/// vectors and the per-vector step alternates between two values.  True on a
/// four-lane ARM64 host, false on an eight-lane one -- and false on x86 SSE4, whose
/// generators have not been converted.
///
/// An eight-lane vector IS the block, so an AVX2 build needs no split and is right by
/// arithmetic.  On four lanes one block is two vectors and the per-vector step
/// alternates, which the ARM64 generators and the C++ reference implement and the x86
/// SSE4 generators do not.  So the split is gated on ARM64: that keeps an SSE4 build
/// self-consistent between its JIT and its own fallback, at the price of an x86 SSE4
/// software renderer whose colour and fog walk is not the ARM64 one.  Until those
/// generators are converted, the goldens are ARM64 goldens.
__forceinline static constexpr bool GSBlockWalkIsSplit([[maybe_unused]] int vlen)
{
#ifdef ARCH_ARM64
	return GSBlockWalkWidth() > vlen;
#else
	return false;
#endif
}
