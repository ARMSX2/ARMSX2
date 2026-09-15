// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/GSRegs.h"

// Whether the vertex conversion divides S and T by Q on the way past, leaving
// the scanline a coordinate that needs no divide.
//
// It must not do that on a triangle, however constant Q is, because the divide
// is not a divide on silicon. The GS multiplies by a reciprocal truncated to
// fourteen fractional bits below its leading mantissa bit, and dividing at the
// vertex computes the exact quotient instead -- a different number wherever the
// quotient lands near a texel boundary, which on a constant-quotient band is
// every pixel at once.
//
// That shape is measured: S ramping across a span with Q constant and not one
// arrived at the scanline with q = 1.0 and t = exactly sixteen texels, so it
// sampled texel 16 where the console's truncated reciprocal samples texel 15.
// One texel high, on every pixel.
//
// Sprites keep it. A sprite's Q comes from the second vertex rather than its own
// (the q(n) / q(n+1) rule), which the scanline has no way to express, so the
// conversion has to resolve it there.
__forceinline static bool GSUseAffineRoute(u32 primclass, bool eq_q, float min_q);

__forceinline static bool GSUseVertexQDivide(u32 primclass, bool mipmap, bool eq_q, float min_q)
{
	if (mipmap)
		return false;

	// The two rules are one rule: divide at the vertex exactly when the scanline
	// will read the coordinate as AFFINE and there is something to divide. An
	// affine route means the scanline bit-casts a 16.16 integer and never touches
	// Q, so if Q is not one it has to have been divided out here or the coordinate
	// is simply wrong.
	//
	// ⚠️ The sprite case is NOT "a sprite whose Q varies". That is true of the
	// q(n)/q(n+1) rule and false of the question being asked here: a sprite with Q
	// constant and not one takes the affine route either way, so dropping its
	// vertex divide leaves the scanline reading undivided ST as though it were a
	// texel coordinate, and every later draw that samples what it wrote inherits
	// the damage.
	return GSUseAffineRoute(primclass, eq_q, min_q) && !(eq_q && min_q == 1.0f);
}

// Whether the scanline is told the coordinate is affine -- the bit-cast 16.16
// path -- rather than perspective.
//
// A sprite takes it, as it always has. So does a draw whose Q is identically one
// across its vertices: there is nothing for a divide to do, and the console was
// measured on exactly that geometry. Routing those through the perspective path
// instead costs a boundary pixel here and there, where the float-to-integer
// conversion rounds the other way from the bit-cast even though the reciprocal of
// one is exactly one.
//
// Everything else -- any Q that is not identically one, constant or not -- takes
// the perspective route and the console's truncated reciprocal.
__forceinline static bool GSUseAffineRoute(u32 primclass, bool eq_q, float min_q)
{
	if (primclass == GS_SPRITE_CLASS)
		return true;

	return eq_q && min_q == 1.0f;
}
