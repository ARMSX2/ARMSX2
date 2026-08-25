// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu SCISSOR: the GS scissor register turned into the Vulkan scissor rectangle the
// executor sets before each indirect call.
//
// There is one road. TileGpu used to have two -- four vertex-shader clip planes written out of the
// draw's state row, and this one -- and it took the clip planes wherever the device offered
// shaderClipDistance, because one indirect call then carried any number of scissors and cut
// nothing. That road is gone. Clipping is GEOMETRIC: a primitive the planes cut is re-interpolated
// from its clipped vertices, so a fragment well inside the rectangle can take an attribute value a
// ULP off the one the unclipped primitive's plane gives it, by float rules that differ per GPU
// vendor. The GS scissor is not that. It is an exact integer rectangle test applied to rasterized
// pixels, and it says nothing about interpolation -- which is what a Vulkan scissor is, on every
// device, identically. The device-dependent road was the reason two GPUs could not be expected to
// produce the same frame; deleting it is what buys that back.
//
// What is pinned here:
//
//  - The rectangle is the register's own half-open interval. scissor.in is
//    [SCAX0, SCAX1 + 1) x [SCAY0, SCAY1 + 1), and Vulkan keeps the pixel whose integer coordinate
//    is in [offset, offset + extent). The columns kept must be exactly x0 .. x1-1: an off-by-one
//    here is a column of pixels.
//
//  - The clamping, which is not cosmetic: a Vulkan scissor offset must be non-negative and inside
//    the area the pass declared, and a GS scissor whose high edge is below its low one is legal and
//    means "nothing" -- which has to arrive as a zero extent, never a negative one.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

namespace
{
	/// The GS's own test for one pixel column: the register names [lo, hi), so column i is in when
	/// lo <= i < hi. This is the reference the rectangle is checked against -- deliberately the
	/// register's semantic rather than the rectangle restated, so a bug in gsTileGpuScissorRect
	/// cannot agree with itself.
	bool RegisterAdmitsColumn(int i, int lo, int hi)
	{
		return i >= lo && i < hi;
	}

	/// The rectangle's test for the same column: Vulkan keeps the pixel whose integer coordinate is
	/// in [offset, offset + extent).
	bool ScissorAdmitsColumn(int i, int offset, int extent)
	{
		return i >= offset && i < (offset + extent);
	}
} // namespace

TEST(GSTileGpuScissorRect, IsTheHalfOpenRectangleTheRegistersName)
{
	// scissor.in is [SCAX0, SCAX1 + 1) x [SCAY0, SCAY1 + 1), so a 640x448 full-frame scissor arrives
	// as (0, 0, 640, 448) and comes out as the whole area with nothing clamped.
	const auto r = GSDevice::gsTileGpuScissorRect(0, 0, 640, 448, 640, 448);
	EXPECT_EQ(r.x, 0);
	EXPECT_EQ(r.y, 0);
	EXPECT_EQ(r.width, 640);
	EXPECT_EQ(r.height, 448);

	// One of Shadow of the Colossus's post-process column scissors.
	const auto c = GSDevice::gsTileGpuScissorRect(320, 0, 400, 448, 640, 448);
	EXPECT_EQ(c.x, 320);
	EXPECT_EQ(c.y, 0);
	EXPECT_EQ(c.width, 80);
	EXPECT_EQ(c.height, 448);
}

TEST(GSTileGpuScissorRect, ClampsIntoTheRenderArea)
{
	// A pass pairing a tall colour target with a shorter depth one renders into the intersection
	// (gsTileGpuPassGeometry), and a scissor reaching past that has to be cut to it -- an offset
	// outside the framebuffer is not a legal scissor.
	const auto r = GSDevice::gsTileGpuScissorRect(0, 0, 1024, 512, 640, 448);
	EXPECT_EQ(r.width, 640);
	EXPECT_EQ(r.height, 448);

	// ...and one that starts past the area keeps a legal offset and no extent, rather than a
	// negative width.
	const auto past = GSDevice::gsTileGpuScissorRect(700, 500, 800, 600, 640, 448);
	EXPECT_EQ(past.x, 640);
	EXPECT_EQ(past.y, 448);
	EXPECT_EQ(past.width, 0);
	EXPECT_EQ(past.height, 0);
}

TEST(GSTileGpuScissorRect, AnEmptyGsScissorIsAZeroExtentNotANegativeOne)
{
	// SCAX1 < SCAX0 is legal and draws nothing. Vulkan has no way to say a negative extent.
	const auto r = GSDevice::gsTileGpuScissorRect(300, 200, 100, 50, 640, 448);
	EXPECT_EQ(r.x, 300);
	EXPECT_EQ(r.y, 200);
	EXPECT_EQ(r.width, 0);
	EXPECT_EQ(r.height, 0);

	// And a degenerate one where the edges meet.
	const auto z = GSDevice::gsTileGpuScissorRect(100, 100, 100, 100, 640, 448);
	EXPECT_EQ(z.width, 0);
	EXPECT_EQ(z.height, 0);
}

TEST(GSTileGpuScissorRect, SelectsExactlyTheColumnsTheRegisterNames)
{
	// Every scissor the corpus's shapes cover, against every pixel column of a 640-wide target.
	// A disagreement anywhere is a column of wrong pixels on every device, since there is no
	// second road to fall to.
	constexpr int kArea = 640;
	const int bounds[] = {0, 1, 2, 7, 63, 64, 80, 159, 160, 319, 320, 321, 480, 638, 639, 640};
	for (const int lo : bounds)
	{
		for (const int hi : bounds)
		{
			if (hi < lo)
				continue;
			const auto r = GSDevice::gsTileGpuScissorRect(lo, 0, hi, 1, kArea, 1);
			for (int i = 0; i < kArea; i++)
			{
				EXPECT_EQ(ScissorAdmitsColumn(i, r.x, r.width), RegisterAdmitsColumn(i, lo, hi))
					<< "column " << i << " of scissor [" << lo << ", " << hi << ")";
			}
		}
	}
}

TEST(GSTileGpuScissorRect, TheSameHoldsOnTheVerticalAxis)
{
	// Same arithmetic, second axis, because a transposed off-by-one would be invisible in the
	// horizontal test above.
	constexpr int kArea = 448;
	const int bounds[] = {0, 1, 15, 16, 111, 112, 223, 224, 446, 447, 448};
	for (const int lo : bounds)
	{
		for (const int hi : bounds)
		{
			if (hi < lo)
				continue;
			const auto r = GSDevice::gsTileGpuScissorRect(0, lo, 1, hi, 1, kArea);
			for (int i = 0; i < kArea; i++)
			{
				EXPECT_EQ(ScissorAdmitsColumn(i, r.y, r.height), RegisterAdmitsColumn(i, lo, hi))
					<< "row " << i << " of scissor [" << lo << ", " << hi << ")";
			}
		}
	}
}
