// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"

#include <algorithm>

/// Does the union of a draw's sprites cover a rectangle?
///
/// GSState::SpriteDrawWithoutGaps() asks a different question -- do these sprites *tile*, in one of
/// three patterns it recognises -- and answers no for a set that overlaps. Two sprites drawn one on
/// top of the other, each spanning the whole target, are not a tiling and are refused, although
/// either one of them covers everything on its own.
///
/// The rule is a header of its own because it is pure rectangle arithmetic, so it can be tested
/// without a GS device, and because the answer is deliberately not the same fact as
/// m_primitive_covers_without_gaps: widening that value moves pixels through the render-target
/// alpha scale decision.
namespace GSSpriteCover
{
	/// Above this many sprites the answer is no, whatever the geometry. The class that pays is a
	/// handful of coincident screen-sized sprites; larger sets exist in the corpus and change no
	/// verdict on any title, so refusing them costs nothing and keeps the sweep trivially bounded.
	inline constexpr u32 MaxSprites = 8;

	/// Whether the union of `count` pixel rectangles covers every pixel of `r`.
	///
	/// Rectangles are half-open, in the same pixel space as `r`, and may overlap in any way. An
	/// empty rectangle covers nothing. The sweep walks the horizontal bands the rectangle edges cut
	/// `r` into and asks each band to be covered from left edge to right edge; a band no rectangle
	/// spans fails on its own.
	inline bool UnionCoversRect(const GSVector4i* rects, u32 count, const GSVector4i& r)
	{
		if (count == 0 || count > MaxSprites || r.rempty())
			return false;

		// One sprite that contains the rectangle answers on its own, which is the whole of the
		// class this exists for.
		for (u32 i = 0; i < count; i++)
		{
			if (rects[i].x <= r.x && rects[i].y <= r.y && rects[i].z >= r.z && rects[i].w >= r.w)
				return true;
		}

		int ys[MaxSprites * 2 + 2];
		u32 ny = 0;
		ys[ny++] = r.y;
		ys[ny++] = r.w;
		for (u32 i = 0; i < count; i++)
		{
			if (rects[i].y > r.y && rects[i].y < r.w)
				ys[ny++] = rects[i].y;
			if (rects[i].w > r.y && rects[i].w < r.w)
				ys[ny++] = rects[i].w;
		}
		std::sort(ys, ys + ny);
		ny = static_cast<u32>(std::unique(ys, ys + ny) - ys);

		for (u32 b = 0; (b + 1) < ny; b++)
		{
			const int y0 = ys[b];
			const int y1 = ys[b + 1];

			// How far to the right the band is covered without a hole. A rectangle extends the
			// reach when it spans the band and starts at or before where the reach stands.
			int reach = r.x;
			bool progress = true;
			while (reach < r.z && progress)
			{
				progress = false;
				for (u32 i = 0; i < count; i++)
				{
					if (rects[i].y > y0 || rects[i].w < y1)
						continue;
					if (rects[i].x <= reach && rects[i].z > reach)
					{
						reach = rects[i].z;
						progress = true;
					}
				}
			}

			if (reach < r.z)
				return false;
		}

		return true;
	}
} // namespace GSSpriteCover
