// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"

#include <algorithm>

// Does one draw's frame buffer share pages with its own Z buffer?
//
// The rasterizer list splits a draw between workers by scanline, which is a split of
// memory only while a worker's rows are that worker's bytes. A game may lay the two
// buffers over each other: Call of Duty 3 clears a 16-bit frame at 0x2a00 and a
// 512-row Z buffer at 0x1c00 in one sprite, and the Z buffer's last 64 rows are the
// frame's first 128. Frame row y and Z row y then share bytes for unrelated values of
// y, the two rows belong to different workers, and whichever stores last wins.
//
// A draw for which this returns true must not be split (GSRasterizerData::serial).
// The answer is by page, like the rest of the renderer's bookkeeping, so it can say
// yes for two rectangles that meet on a page without sharing a byte.
inline bool GSSwFrameAndDepthSharePages(const GSOffset& fb, const GSOffset& zb, const GSVector4i& r)
{
	// Nearly every draw has its buffers far apart. Neither rectangle reaches `reach`
	// pages past its base (pages are at least 64 pixels by 32 rows, a row of pages is
	// bw pages, and a base that is not page aligned spills into one page more).
	const int reach = ((r.bottom + 31) >> 5) * static_cast<int>(std::max(fb.bw(), zb.bw())) + ((r.right + 63) >> 6) + 1;
	const int apart = static_cast<int>(((zb.bp() >> 5) - (fb.bp() >> 5)) & (GS_MAX_PAGES - 1));

	if (apart >= reach && GS_MAX_PAGES - apart >= reach)
		return false;

	u32 pages[GS_MAX_PAGES / 32] = {};

	fb.pageLooperForRect(r).loopPages([&pages](u32 page) {
		pages[page >> 5] |= 1u << (page & 31);
	});

	bool shared = false;

	zb.pageLooperForRect(r).loopPagesWithBreak([&pages, &shared](u32 page) {
		shared = (pages[page >> 5] & (1u << (page & 31))) != 0;
		return !shared;
	});

	return shared;
}
