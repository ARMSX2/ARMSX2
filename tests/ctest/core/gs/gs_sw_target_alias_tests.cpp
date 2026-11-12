// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the test that keeps a draw whole when its frame buffer and its Z buffer share
// pages (GS/Renderers/SW/GSSwTargetAlias.h).
//
// Call of Duty 3 clears a 16-bit frame at 0x2a00 and a 24-bit Z buffer at 0x1c00 with
// one 512x512 sprite. The Z buffer's last two rows of pages are the frame's first two,
// so split by scanline between two workers the clear left different bytes in local
// memory from run to run. Three and four workers escaped only because that sprite's
// bounding box is one pixel wider than the buffer, which already makes it serial.
//
// Rides gs_vertex_tests.

#include "GS/Renderers/SW/GSSwTargetAlias.h"

#include <gtest/gtest.h>

#include <bitset>
#include <random>

namespace
{
	using PageSet = std::bitset<GS_MAX_PAGES>;

	// The pages the pixels of `r` really live on, one pixel at a time.
	PageSet PixelPages(const GSOffset& off, const GSVector4i& r, int pixels_per_page_shift)
	{
		PageSet pages;

		for (int y = r.top; y < r.bottom; y++)
		{
			for (int x = r.left; x < r.right; x++)
				pages.set((off.pa(x, y) >> pixels_per_page_shift) % GS_MAX_PAGES);
		}

		return pages;
	}

	// What the renderer's page bookkeeping says the rectangle covers.
	PageSet LooperPages(const GSOffset& off, const GSVector4i& r)
	{
		PageSet pages;

		off.pageLooperForRect(r).loopPages([&pages](u32 page) { pages.set(page); });

		return pages;
	}

	// A page holds 2048 32-bit or 24-bit pixels, or 4096 16-bit ones.
	int PixelsPerPageShift(GS_PSM psm)
	{
		return (psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S) ? 12 : 11;
	}
} // namespace

// The draw from the dump: every frame, the first sprite of the scene.
TEST(GSSwTargetAlias, CallOfDuty3ClearSharesPages)
{
	const GSOffset fb = GSOffset::fromKnownPSM(0x2a00, 8, PSMCT16S);
	const GSOffset zb = GSOffset::fromKnownPSM(0x1c00, 8, PSMZ24);

	EXPECT_TRUE(GSSwFrameAndDepthSharePages(fb, zb, GSVector4i(0, 0, 513, 513)));
	EXPECT_TRUE(GSSwFrameAndDepthSharePages(fb, zb, GSVector4i(0, 0, 512, 512)));

	// The Z rows above 448 stop short of the frame.
	EXPECT_FALSE(GSSwFrameAndDepthSharePages(fb, zb, GSVector4i(0, 0, 512, 448)));

	// And they do share bytes, not only pages: this is not the bookkeeping being cautious.
	const PageSet f = PixelPages(fb, GSVector4i(0, 0, 512, 512), 12);
	const PageSet z = PixelPages(zb, GSVector4i(0, 0, 512, 512), 11);
	EXPECT_EQ((f & z).count(), 16u);
}

// The layouts games normally use never share a page, whatever the rectangle.
TEST(GSSwTargetAlias, SeparateBuffersDoNot)
{
	// 640x448, frame at 0, Z right behind the two display buffers.
	EXPECT_FALSE(GSSwFrameAndDepthSharePages(GSOffset::fromKnownPSM(0x0000, 10, PSMCT32),
		GSOffset::fromKnownPSM(0x2300, 10, PSMZ24), GSVector4i(0, 0, 640, 448)));

	// Call of Duty 2: a 512x512 frame at 0x1000 that ends where the Z buffer starts.
	EXPECT_FALSE(GSSwFrameAndDepthSharePages(GSOffset::fromKnownPSM(0x1000, 8, PSMCT32),
		GSOffset::fromKnownPSM(0x2000, 8, PSMZ24), GSVector4i(0, 0, 512, 512)));

	// Buffers that touch end to end: 512x256 of 32-bit frame is exactly 64 pages.
	EXPECT_FALSE(GSSwFrameAndDepthSharePages(GSOffset::fromKnownPSM(0x2000, 8, PSMCT32),
		GSOffset::fromKnownPSM(0x2800, 8, PSMZ32), GSVector4i(0, 0, 512, 256)));

	// One row more and the frame is on the Z buffer's first pages.
	EXPECT_TRUE(GSSwFrameAndDepthSharePages(GSOffset::fromKnownPSM(0x2000, 8, PSMCT32),
		GSOffset::fromKnownPSM(0x2800, 8, PSMZ32), GSVector4i(0, 0, 512, 257)));
}

// Frame and Z at one address share everything.
TEST(GSSwTargetAlias, SameAddressShares)
{
	EXPECT_TRUE(GSSwFrameAndDepthSharePages(GSOffset::fromKnownPSM(0x2300, 10, PSMCT32),
		GSOffset::fromKnownPSM(0x2300, 10, PSMZ32), GSVector4i(100, 100, 108, 108)));
}

// Local memory wraps at 4 MiB, and so do the page numbers.
TEST(GSSwTargetAlias, WrapsAroundTheEndOfMemory)
{
	// The frame's second row of pages is 0x3f00 + 8 pages = page 0 again.
	EXPECT_TRUE(GSSwFrameAndDepthSharePages(GSOffset::fromKnownPSM(0x3f00, 8, PSMCT32),
		GSOffset::fromKnownPSM(0x0000, 8, PSMZ24), GSVector4i(0, 0, 64, 64)));
	EXPECT_FALSE(GSSwFrameAndDepthSharePages(GSOffset::fromKnownPSM(0x3f00, 8, PSMCT32),
		GSOffset::fromKnownPSM(0x0000, 8, PSMZ24), GSVector4i(0, 0, 64, 32)));
}

// Against the bookkeeping it summarises and against the pixels themselves, over random
// buffers and rectangles: the quick reject never hides a shared page, and a draw whose
// frame and Z pixels really share a page is always caught.
TEST(GSSwTargetAlias, MatchesThePageSetsOnRandomDraws)
{
	static constexpr GS_PSM fpsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S};
	static constexpr GS_PSM zpsms[] = {PSMZ32, PSMZ24, PSMZ16, PSMZ16S};

	std::mt19937 rng(20261110);
	int shared = 0;

	for (int i = 0; i < 4000; i++)
	{
		const GS_PSM fpsm = fpsms[rng() % 4];
		const GS_PSM zpsm = zpsms[rng() % 4];
		const u32 bw = 1 + rng() % 16;
		const u32 fbp = (rng() % 0x4000) & ((rng() % 4) ? ~0x1fu : ~0u); // mostly page aligned
		// Half the draws put the Z buffer near the frame, where the interesting cases are.
		const u32 zbp = (rng() % 2) ? ((fbp + rng() % 0x600) & 0x3fe0) : ((rng() % 0x4000) & ~0x1fu);
		const int x = rng() % 600;
		const int y = rng() % 500;
		const GSVector4i r(x, y, x + 1 + static_cast<int>(rng() % 160), y + 1 + static_cast<int>(rng() % 160));

		const GSOffset fb = GSOffset::fromKnownPSM(fbp, bw, fpsm);
		const GSOffset zb = GSOffset::fromKnownPSM(zbp, bw, zpsm);

		const bool got = GSSwFrameAndDepthSharePages(fb, zb, r);
		const bool by_bookkeeping = (LooperPages(fb, r) & LooperPages(zb, r)).any();
		const bool by_pixels = (PixelPages(fb, r, PixelsPerPageShift(fpsm)) & PixelPages(zb, r, PixelsPerPageShift(zpsm))).any();

		ASSERT_EQ(got, by_bookkeeping) << "draw " << i << " fbp " << fbp << " zbp " << zbp << " bw " << bw
									   << " r " << r.left << "," << r.top << "," << r.right << "," << r.bottom;
		ASSERT_TRUE(got || !by_pixels) << "draw " << i << ": pixels share a page the test missed";

		shared += got ? 1 : 0;
	}

	// The sweep has to exercise both answers to mean anything.
	EXPECT_GT(shared, 200);
	EXPECT_LT(shared, 3800);
}
