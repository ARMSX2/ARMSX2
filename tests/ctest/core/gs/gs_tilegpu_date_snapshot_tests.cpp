// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu DATE SNAPSHOT RECT: the rectangle a pass copies out of its colour target so its
// destination-alpha draws can read a destination they are not allowed to read in-pass. It used to
// be the whole target -- 140 pages, 1.147 MB, once per pass with a DATE draw in it -- and is now
// the union of what those draws actually fetch.
//
// The change moves no pixel, so what is pinned here is the one claim it rests on: CONTAINMENT. A
// snapshot has exactly one consumer, `texelFetch(u_snapshot, ivec2(gl_FragCoord.xy), 0).a` in
// tilegpu.glsl under the draw's own `date != 0` -- an unfiltered fetch at the fragment's OWN
// coordinate, so a DATE draw reads exactly the pixels it rasterizes and the union of the pass's
// DATE draws' scissor-clipped boxes covers every read the snapshot can serve.
//
// ⚠️ AND THE FAILURE IS NOT A STALE PIXEL. Outside the copied rect the scratch surface holds its
// previous tenant, straight out of the texture pool -- so a rect that is one pixel short does not
// read an old version of the target, it reads some other frame's image and discards fragments on
// it. That is why the sweep below checks containment PER PIXEL over the read rectangles rather
// than comparing two rectangles' corners: a corner comparison and the shader's fetch are the same
// arithmetic only while the arithmetic is right, which is the thing under test.
//
// The round-out to the 64x32 page grid is not required by anything -- the copy is a vkCmdCopyImage
// over an uncompressed colour format, block 1x1, and the reader is a texelFetch -- so it is pinned
// as a property of the census's units rather than of correctness: a copy costs a whole number of
// the 8 KB pages every other TileGpu counter is expressed in.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
	GSVector4i Snap(const std::vector<GSVector4i>& reads, int w, int h)
	{
		return GSDevice::gsTileGpuSnapshotRect(reads, w, h);
	}

	/// The rect the planner pushes, as the push site chooses it: the union under
	/// EmuCore/GS/TileGpuNarrowDateSnapshot, the whole target without it. Modelled here so the OFF
	/// arm is pinned by the same tests as the ON one rather than by reading the renderer.
	GSVector4i Pushed(const std::vector<GSVector4i>& reads, int w, int h, bool narrow)
	{
		return narrow ? Snap(reads, w, h) : GSVector4i(0, 0, w, h);
	}

	/// Whether every pixel of `read` that the target actually has is inside `copied`. Walked as
	/// pixels, not as corners -- see the header note.
	bool CoversEveryPixel(const GSVector4i& copied, const GSVector4i& read, int w, int h)
	{
		const int x0 = std::max(read.x, 0), y0 = std::max(read.y, 0);
		const int x1 = std::min(read.z, w), y1 = std::min(read.w, h);
		for (int y = y0; y < y1; y++)
		{
			for (int x = x0; x < x1; x++)
			{
				if (x < copied.x || x >= copied.z || y < copied.y || y >= copied.w)
					return false;
			}
		}
		return true;
	}

	/// A 640x448 target is 10 x 14 pages of 64x32 -- the 140 the stuntman decomposition counted, and
	/// 140 * 8192 = 1,146,880 = 640 * 448 * 4.
	constexpr int kW = 640;
	constexpr int kH = 448;
} // namespace

// -- containment: the whole claim ---------------------------------------------------------------

TEST(GSTileGpuDateSnapshot, ACopyCoversEveryPixelItsDateDrawsFetch)
{
	// One sprite, two disjoint sprites, a stack of overlapping ones, a full-width bar, a single
	// pixel, and the pathological pair at opposite corners that forces the union back to the target.
	const std::vector<std::vector<GSVector4i>> cases = {
		{GSVector4i(0, 0, 1, 1)},
		{GSVector4i(100, 100, 164, 132)},
		{GSVector4i(63, 31, 65, 33)},                                  // straddles a page corner
		{GSVector4i(10, 10, 40, 40), GSVector4i(600, 400, 640, 448)},  // opposite corners
		{GSVector4i(0, 0, 640, 32)},                                   // full-width bar
		{GSVector4i(320, 0, 321, 448)},                                // full-height sliver
		{GSVector4i(5, 5, 300, 300), GSVector4i(200, 200, 400, 400), GSVector4i(1, 1, 2, 2)},
		{GSVector4i(639, 447, 640, 448)},                              // the last pixel
	};

	for (const auto& reads : cases)
	{
		const GSVector4i copied = Snap(reads, kW, kH);
		for (const GSVector4i& r : reads)
			EXPECT_TRUE(CoversEveryPixel(copied, r, kW, kH))
				<< "copy " << copied.x << "," << copied.y << ".." << copied.z << "," << copied.w
				<< " misses read " << r.x << "," << r.y << ".." << r.z << "," << r.w;
	}
}

TEST(GSTileGpuDateSnapshot, ASweepOfReadSetsIsContainedPixelForPixel)
{
	// A deterministic sweep rather than a handful of cases: the rule is arithmetic and the way
	// arithmetic goes wrong is at a boundary nobody wrote a case for. Small targets too, because a
	// target that is not a whole number of pages is where a round-out overruns.
	u32 seed = 0x5eed1234u;
	const auto next = [&seed](u32 n) {
		seed = seed * 1664525u + 1013904223u;
		return (seed >> 8) % n;
	};

	for (int iter = 0; iter < 4000; iter++)
	{
		const int w = 1 + static_cast<int>(next(700));
		const int h = 1 + static_cast<int>(next(500));
		std::vector<GSVector4i> reads;
		const u32 n = 1 + next(4);
		for (u32 k = 0; k < n; k++)
		{
			const int x0 = static_cast<int>(next(static_cast<u32>(w) + 40u)) - 20;
			const int y0 = static_cast<int>(next(static_cast<u32>(h) + 40u)) - 20;
			const int x1 = x0 + static_cast<int>(next(120));
			const int y1 = y0 + static_cast<int>(next(120));
			reads.push_back(GSVector4i(x0, y0, x1, y1));
		}

		const GSVector4i copied = Snap(reads, w, h);

		// Never outside the target. A copy that names a pixel the image does not have is a Vulkan
		// validation error before it is a wrong pixel.
		EXPECT_GE(copied.x, 0);
		EXPECT_GE(copied.y, 0);
		EXPECT_LE(copied.z, w);
		EXPECT_LE(copied.w, h);

		for (const GSVector4i& r : reads)
			EXPECT_TRUE(CoversEveryPixel(copied, r, w, h)) << "iter " << iter;
	}
}

// -- the fail-safe -------------------------------------------------------------------------------

TEST(GSTileGpuDateSnapshot, AnEmptyUnionIsTheWholeTargetAndNotNothing)
{
	// A pass whose DATE draws all scissored away, and a caller that named no reads at all. Both come
	// back as the whole target: outside the copied rect the scratch holds its previous tenant, so
	// the direction to fail in is a slow pass, never an uninitialized one.
	EXPECT_TRUE(Snap({}, kW, kH).eq(GSVector4i(0, 0, kW, kH)));
	EXPECT_TRUE(Snap({GSVector4i(10, 10, 10, 10)}, kW, kH).eq(GSVector4i(0, 0, kW, kH)));

	// A GS scissor whose high edge sits below its low one is legal and means "nothing"; it must not
	// arrive here as a negative extent, and it must not narrow anything either.
	EXPECT_TRUE(Snap({GSVector4i(300, 300, 100, 100)}, kW, kH).eq(GSVector4i(0, 0, kW, kH)));

	// Wholly off the target in each direction: clipped to nothing, so the fail-safe stands.
	EXPECT_TRUE(Snap({GSVector4i(-50, -50, -10, -10)}, kW, kH).eq(GSVector4i(0, 0, kW, kH)));
	EXPECT_TRUE(Snap({GSVector4i(700, 500, 800, 600)}, kW, kH).eq(GSVector4i(0, 0, kW, kH)));

	// A degenerate target keeps the same answer rather than dividing by its own size.
	EXPECT_TRUE(Snap({GSVector4i(0, 0, 4, 4)}, 0, 0).eq(GSVector4i(0, 0, 0, 0)));
}

// -- the grid ------------------------------------------------------------------------------------

TEST(GSTileGpuDateSnapshot, TheLowEdgeSitsOnThePageGridAndNeverAboveTheRead)
{
	// Rounding the low edge the wrong way -- to the NEAREST page line rather than DOWN to one -- is
	// the misalignment that costs pixels, and it costs them only for reads that start just after a
	// line. Swept across a whole page in each axis so no offset is unrepresented.
	for (int ox = 0; ox < 64; ox++)
	{
		for (int oy = 0; oy < 32; oy++)
		{
			const GSVector4i read(128 + ox, 96 + oy, 128 + ox + 10, 96 + oy + 10);
			const GSVector4i c = Snap({read}, kW, kH);
			EXPECT_EQ(c.x % 64, 0);
			EXPECT_EQ(c.y % 32, 0);
			EXPECT_LE(c.x, read.x);
			EXPECT_LE(c.y, read.y);
			EXPECT_TRUE(CoversEveryPixel(c, read, kW, kH));
		}
	}
}

TEST(GSTileGpuDateSnapshot, TheHighEdgeCoversThePartialPageItLandsIn)
{
	// One pixel past a page line takes the whole next page, not the pixel. Rounding the high edge
	// IN is the "too small" failure that a corner comparison of two rectangles would not notice,
	// because the rectangle still looks sensible.
	EXPECT_TRUE(Snap({GSVector4i(0, 0, 65, 33)}, kW, kH).eq(GSVector4i(0, 0, 128, 64)));
	EXPECT_TRUE(Snap({GSVector4i(0, 0, 64, 32)}, kW, kH).eq(GSVector4i(0, 0, 64, 32)));

	// ...and where the round-out would overrun a target whose size is not a whole number of pages
	// in either axis, it stops at the target instead. 129 rounds to 192 and 104 to 128; the image
	// has 130 by 105.
	const GSVector4i read(100, 100, 129, 104);
	const GSVector4i c = Snap({read}, 130, 105);
	EXPECT_EQ(c.z, 130);
	EXPECT_EQ(c.w, 105);
	EXPECT_TRUE(CoversEveryPixel(c, read, 130, 105));

	// Where it would NOT overrun, the margin is taken and the clamp never engages.
	EXPECT_TRUE(Snap({GSVector4i(100, 100, 110, 110)}, 130, 105).eq(GSVector4i(64, 96, 128, 105)));
}

TEST(GSTileGpuDateSnapshot, ReadsReachingPastTheTargetAreClippedNotCarried)
{
	// A draw whose bbox overhangs the target -- the scissor allows it, the image does not. The copy
	// must stay inside the image, and must still cover the part of the read the image has.
	const GSVector4i read(600, 430, 900, 700);
	const GSVector4i c = Snap({read}, kW, kH);
	EXPECT_LE(c.z, kW);
	EXPECT_LE(c.w, kH);
	EXPECT_EQ(c.z, kW);
	EXPECT_EQ(c.w, kH);
	EXPECT_TRUE(CoversEveryPixel(c, read, kW, kH));
}

// -- the union, as against anything else ---------------------------------------------------------

TEST(GSTileGpuDateSnapshot, TwoDisjointReadsTakeBothAndNotJustOne)
{
	// The DATE road's own case: SotC's depth-of-field composite is disjoint column sprites sharing
	// one snapshot, so a rule that kept the FIRST read, or the LAST, or the two intersected, would
	// pass every single-rect test above and break exactly this one.
	const GSVector4i a(0, 0, 64, 32);
	const GSVector4i b(576, 416, 640, 448);
	const GSVector4i c = Snap({a, b}, kW, kH);
	EXPECT_TRUE(CoversEveryPixel(c, a, kW, kH));
	EXPECT_TRUE(CoversEveryPixel(c, b, kW, kH));
	EXPECT_TRUE(c.eq(GSVector4i(0, 0, kW, kH)));

	// Two adjacent ones do NOT take the target, which is what says the union is a union and not a
	// "more than one read means give up".
	const GSVector4i d = Snap({GSVector4i(0, 0, 64, 32), GSVector4i(64, 0, 128, 32)}, kW, kH);
	EXPECT_TRUE(d.eq(GSVector4i(0, 0, 128, 32)));
}

TEST(GSTileGpuDateSnapshot, AReadCoveringTheTargetComesBackAsTheTarget)
{
	// The narrow arm degenerates to the whole-target arm exactly where the draws want the whole
	// target, so the lever can never cost a title anything it was not already paying.
	EXPECT_TRUE(Snap({GSVector4i(0, 0, kW, kH)}, kW, kH).eq(GSVector4i(0, 0, kW, kH)));
}

// -- the key -------------------------------------------------------------------------------------

TEST(GSTileGpuDateSnapshotKeyOff, TheOffArmIsTheWholeTargetForEveryCase)
{
	// EmuCore/GS/TileGpuNarrowDateSnapshot=false must reproduce the road as it shipped on
	// 2026-08-30, which pushed GSVector4i(0, 0, tsz.x, tsz.y) with no read list consulted at all.
	const std::vector<std::vector<GSVector4i>> cases = {
		{}, {GSVector4i(0, 0, 1, 1)}, {GSVector4i(100, 100, 164, 132)},
		{GSVector4i(10, 10, 40, 40), GSVector4i(600, 400, 640, 448)}};
	for (const auto& reads : cases)
		EXPECT_TRUE(Pushed(reads, kW, kH, false).eq(GSVector4i(0, 0, kW, kH)));
}

TEST(GSTileGpuDateSnapshotKeyOff, TheOnArmNeverReachesPastTheOffArm)
{
	// Whatever the union comes to, it is a sub-rectangle of what the off arm copies -- so the off
	// arm's soundness is the on arm's ceiling and the lever cannot introduce a read the old road
	// did not already serve.
	u32 seed = 0xc0ffee11u;
	const auto next = [&seed](u32 n) {
		seed = seed * 1664525u + 1013904223u;
		return (seed >> 8) % n;
	};
	for (int iter = 0; iter < 2000; iter++)
	{
		std::vector<GSVector4i> reads;
		const u32 n = 1 + next(3);
		for (u32 k = 0; k < n; k++)
		{
			const int x0 = static_cast<int>(next(kW));
			const int y0 = static_cast<int>(next(kH));
			reads.push_back(GSVector4i(x0, y0, x0 + static_cast<int>(next(200)), y0 + static_cast<int>(next(200))));
		}
		const GSVector4i on = Pushed(reads, kW, kH, true);
		const GSVector4i off = Pushed(reads, kW, kH, false);
		EXPECT_GE(on.x, off.x);
		EXPECT_GE(on.y, off.y);
		EXPECT_LE(on.z, off.z);
		EXPECT_LE(on.w, off.w);
	}
}

// -- what a copy costs ---------------------------------------------------------------------------

TEST(GSTileGpuDateSnapshotPages, TheWholeTargetIsTheHundredAndFortyPagesTheDecompositionCounted)
{
	// 640x448 = 10 x 14 pages of 64x32, and 140 * 8192 = 1,146,880 = 640 * 448 * 4. The census's
	// megabytes are read against the decomposition's, so this arithmetic is the one that has to
	// agree with it.
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPages(GSVector4i(0, 0, 640, 448)), 140u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPages(GSVector4i(0, 0, 64, 32)), 1u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPages(GSVector4i(0, 0, 128, 32)), 2u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPages(GSVector4i(0, 0, 0, 0)), 0u);

	// A target that is not a whole number of page rows is charged for the partial row it copies,
	// rounded up -- 226 is seven page rows and two pixels, and the copy moves all eight.
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPages(GSVector4i(0, 0, 640, 226)), 80u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPages(GSVector4i(0, 0, 640, 224)), 70u);
}

TEST(GSTileGpuDateSnapshotPages, TheNarrowCopyNeverCostsMoreThanTheWholeTarget)
{
	u32 seed = 0x1badd00du;
	const auto next = [&seed](u32 n) {
		seed = seed * 1664525u + 1013904223u;
		return (seed >> 8) % n;
	};
	const u32 full = GSDevice::gsTileGpuSnapshotPages(GSVector4i(0, 0, kW, kH));
	for (int iter = 0; iter < 2000; iter++)
	{
		const int x0 = static_cast<int>(next(kW));
		const int y0 = static_cast<int>(next(kH));
		const GSVector4i read(x0, y0, x0 + static_cast<int>(next(300)), y0 + static_cast<int>(next(300)));
		EXPECT_LE(GSDevice::gsTileGpuSnapshotPages(Snap({read}, kW, kH)), full);
	}
}

TEST(GSTileGpuDateSnapshotPages, TheCensusBucketsAreDoublingBandsWithTheLastOneOpen)
{
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(0), 0u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(1), 0u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(2), 1u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(3), 2u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(4), 2u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(8), 3u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(16), 4u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(32), 5u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(64), 6u);
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(65), 7u);
	// The whole target on the corpus's dumps lands in the open band, which is what makes the
	// histogram readable: everything below it is a copy the lever made.
	EXPECT_EQ(GSDevice::gsTileGpuSnapshotPageBucket(140), 7u);
	// Monotone, or the histogram's bands cross.
	for (u32 p = 1; p < 400; p++)
		EXPECT_LE(GSDevice::gsTileGpuSnapshotPageBucket(p - 1), GSDevice::gsTileGpuSnapshotPageBucket(p));
}
