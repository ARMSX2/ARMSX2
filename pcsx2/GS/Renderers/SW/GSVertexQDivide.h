// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/GSRegs.h"

// Whether the vertex conversion divides S and T by Q on the way past, leaving
// the scanline a coordinate that needs no divide.
//
// Not on a triangle, even with constant Q: the GS multiplies by a reciprocal
// truncated to fourteen bits below its leading mantissa bit, while a vertex
// divide gives the exact quotient. Near a texel boundary that samples one texel
// high.
//
// Sprites keep it. A sprite's Q comes from the second vertex (the q(n) / q(n+1)
// rule), which the scanline cannot express, so the conversion resolves it.
__forceinline static bool GSUseAffineRoute(u32 primclass, bool eq_q, float min_q);

__forceinline static bool GSUseVertexQDivide(u32 primclass, bool mipmap, bool eq_q, float min_q)
{
	if (mipmap)
		return false;

	// Divide at the vertex exactly when the scanline will read the coordinate as
	// affine (a bit-cast 16.16 integer that never touches Q) and Q is not one.
	//
	// This includes a sprite with constant Q != 1, not only a sprite whose Q
	// varies: without the divide the scanline reads undivided ST as texels.
	return GSUseAffineRoute(primclass, eq_q, min_q) && !(eq_q && min_q == 1.0f);
}

// Whether the scanline is told the coordinate is affine -- the bit-cast 16.16
// path -- rather than perspective.
//
// Sprites take it, and so does a draw whose Q is identically one. The
// perspective path would round some boundary pixels differently from the
// bit-cast even with a reciprocal of exactly one.
//
// Any other Q, constant or not, takes the perspective route and the truncated
// reciprocal.
__forceinline static bool GSUseAffineRoute(u32 primclass, bool eq_q, float min_q)
{
	if (primclass == GS_SPRITE_CLASS)
		return true;

	return eq_q && min_q == 1.0f;
}
