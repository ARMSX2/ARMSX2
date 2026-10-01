// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// The GS interpolates a block of pixels per DDA step, independent of our vector
// width. The block is eight pixels wide only when texturing, fog and AA1 are all
// off, else four. Blending, depth testing and target format do not affect it.
// The width must not come from the host vector size, or output depends on the
// machine.
//
// Only truncation makes a block observable: with an exact step, W steps of one
// and one step of W agree. So untruncated attributes (depth in double, the
// perspective coordinate in float) step per vector. The affine coordinate is
// truncated but its block width is unknown, so it also steps per vector.
//
// Colour and fog use the block model in GSColourWalk.h (blocks pinned to absolute
// x, coarser in-block ramp, row pairs). Fog shares the colour DDA, which is why
// enabling fog narrows the colour block.
//
// This file supplies the width, and whether a four-lane host must alternate two
// per-vector steps (W = 8, two vectors per block) or use one (W = 4).

/// Horizontal span of one DDA step, in pixels. Eight only when texturing, fog and
/// AA1 are all off; any one of them makes it four. They do not compound.
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

/// Whether one block spans two host vectors, so the per-vector step alternates.
/// True on four-lane ARM64. An eight-lane (AVX2) vector is the block and needs no
/// split. The x86 SSE4 generators do not implement the split, so it is gated on
/// ARM64 to keep SSE4's JIT and C++ fallback consistent with each other; their
/// colour and fog walk therefore differs from ARM64's.
__forceinline static constexpr bool GSBlockWalkIsSplit([[maybe_unused]] int vlen)
{
#ifdef ARCH_ARM64
	return GSBlockWalkWidth() > vlen;
#else
	return false;
#endif
}
