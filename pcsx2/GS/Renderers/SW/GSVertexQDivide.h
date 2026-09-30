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

// Whether a triangle draws the console's own S, T and Q planes (GSPerspectivePlane.h):
// a triangle whose coordinate is STQ and takes the perspective route, so not the UV
// register, not the affine route a constant Q of one takes (a mipmapped draw never
// takes it, GSRendererSW), and not AA1, whose edge pass walks its own float vertices.
//
// One answer for two callers. The renderer sets the scanline's plane bit from it, and
// the front end (GSState) asks it before rounding the vertices: the console's plane
// does not depend on Z, and the front end's texel rounding only fires under a constant
// one, so it must leave a draw that builds the plane alone.
__forceinline static bool GSUseConsolePlane(u32 primclass, bool fst, bool aa1, bool mipmap, bool eq_q, float min_q)
{
	return primclass == GS_TRIANGLE_CLASS && !fst && !aa1 && (mipmap || !GSUseAffineRoute(primclass, eq_q, min_q));
}
