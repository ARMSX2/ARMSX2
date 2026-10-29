// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"
#include "common/Pcsx2Defs.h"

// Snapping a pack texture's alpha back to 0x80 before an alpha test that compares against it.
//
// PS2 art stores "opaque" as alpha 0x80, and games alpha-test against it (EQUAL 0x80, GEQUAL 0x7E,
// GREATER 0x7F and so on). A texel that fails keeps its colour but loses its depth write, which
// shows as speckle.
//
// ASTC packs cannot store 0x80 exactly at the endpoint precision a 6x6 block can usually afford,
// so a pack's opaque texels decode as 119-142. Within the window below of 0x80 a pack texel is
// taken as 0x80, which catches nearly all of the drifted opaque texels. The few other texels it
// moves are mostly soft edges that were within the window in the source art too.
//
// Only for a pack texture, on a draw whose alpha test compares near 0x80; every other draw sees
// the texture's alpha as decoded.
namespace GSReplacementAlphaSnap
{
	/// Texture alpha within this distance of 0x80 becomes 0x80. The shaders carry the same number.
	static constexpr u32 WINDOW = 8;

	constexpr bool NearOpaque(u32 alpha)
	{
		return (alpha >= 0x80 - WINDOW) && (alpha <= 0x80 + WINDOW);
	}

	/// replacement: the draw samples a pack texture. tcc: the texture's alpha reaches the fragment.
	constexpr bool Wanted(bool replacement, bool tcc, bool ate, u32 atst, u32 aref)
	{
		return replacement && tcc && ate && atst != ATST_NEVER && atst != ATST_ALWAYS && NearOpaque(aref);
	}
} // namespace GSReplacementAlphaSnap
