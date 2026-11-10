// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"
#include "GS/GSRegs.h"
#include "common/Pcsx2Types.h"

// A local-to-local move in PSMZ24 that copies colour pages into a depth buffer.
//
// Elemental Gelade does this once a frame: it moves its 640x224 PSMCT32 frame at 0x0 into the depth
// buffer of the next frame (0x8c0 and 0x1180 in turn) with a PSMZ24 transfer. The texture cache found
// no depth target at the source and declined, so the move ran on the CPU behind a synchronous readback
// of the colour target.
//
// When both ends are whole pages (page-aligned bases, rectangles on page edges, the two regions sharing
// no page) the transfer copies each page whole into the matching page: Z24 pixel (u, v) of a source
// page lands at Z24 pixel (u, v) of the destination page, so every word goes to the same offset, with
// its top byte left as the destination had it. PSMCT32 and PSMZ32 share the 64x32 page and the layout
// of the words inside an 8x8 block; they differ only in where each of the page's 32 blocks sits. So the
// depth pixel (u, v) of a destination page holds the low 24 bits of the colour pixel of the source page
// that sits at the same offset inside the block whose number the depth layout gives (u, v): a fixed
// permutation of whole 8x8 blocks, the same for every page (ColourBlockForDepthBlock).
namespace GSDepthFormatMove
{
	inline constexpr int kPageW = 64;
	inline constexpr int kPageH = 32;
	inline constexpr int kBlockW = 8;
	inline constexpr int kBlockH = 8;
	inline constexpr int kBlocksX = kPageW / kBlockW;
	inline constexpr int kBlocksY = kPageH / kBlockH;

	/// True when a move between these rectangles copies whole pages into whole pages and the two
	/// regions share no page. Bases are in blocks; the rectangles are in pixels of a 32-bit format.
	inline bool WholePages(u32 sbp, u32 sbw, int sx, int sy, u32 dbp, u32 dbw, int dx, int dy, int w, int h)
	{
		constexpr u32 kBlocksPerPage = 32;
		if (w <= 0 || h <= 0 || sbw == 0 || dbw == 0)
			return false;
		if ((sbp % kBlocksPerPage) != 0 || (dbp % kBlocksPerPage) != 0)
			return false;
		if ((sx % kPageW) != 0 || (dx % kPageW) != 0 || (w % kPageW) != 0)
			return false;
		if ((sy % kPageH) != 0 || (dy % kPageH) != 0 || (h % kPageH) != 0)
			return false;

		// Page spans in page numbers from the start of memory. A row of the rectangle may run past the
		// buffer width into the next row of pages; the span covers it.
		const u32 s_first = sbp / kBlocksPerPage + static_cast<u32>(sy / kPageH) * sbw + static_cast<u32>(sx / kPageW);
		const u32 s_last = sbp / kBlocksPerPage + static_cast<u32>((sy + h) / kPageH - 1) * sbw + static_cast<u32>((sx + w) / kPageW - 1);
		const u32 d_first = dbp / kBlocksPerPage + static_cast<u32>(dy / kPageH) * dbw + static_cast<u32>(dx / kPageW);
		const u32 d_last = dbp / kBlocksPerPage + static_cast<u32>((dy + h) / kPageH - 1) * dbw + static_cast<u32>((dx + w) / kPageW - 1);
		constexpr u32 kPages = 512; // 4 MB of local memory
		if (s_last >= kPages || d_last >= kPages)
			return false;
		return s_last < d_first || d_last < s_first;
	}

	/// The block position (in blocks, inside a page) of the PSMCT32 block that holds the words of the
	/// PSMZ32 block at (bx, by).
	inline GSVector2i ColourBlockForDepthBlock(int bx, int by)
	{
		const u32 n = GSLocalMemory::BlockNumber32Z(bx * kBlockW, by * kBlockH, 0, 1);
		for (int cy = 0; cy < kBlocksY; cy++)
		{
			for (int cx = 0; cx < kBlocksX; cx++)
			{
				if (GSLocalMemory::BlockNumber32(cx * kBlockW, cy * kBlockH, 0, 1) == n)
					return GSVector2i(cx, cy);
			}
		}
		return GSVector2i(-1, -1);
	}
} // namespace GSDepthFormatMove
