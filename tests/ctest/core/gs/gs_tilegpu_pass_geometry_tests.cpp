// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A TileGpu pass answers two geometry questions, and they are not the same question.
//
//  * WHAT MAY IT WRITE. A Vulkan framebuffer may not be larger than the smallest attachment it
//    carries, so a pass pairing a tall colour target with a shorter depth one renders into the
//    intersection. That is the render area, and the scissor matches it.
//  * THROUGH WHAT MAPPING DO ITS VERTICES ARRIVE. The renderer writes each draw's transform
//    against the COLOUR target's own size -- guest row y is colour-image row y, because a
//    surface's texture row 0 is its base pointer's row 0 -- and the viewport is the other half
//    of that transform. It has to span exactly the height the transform divided by.
//
// Reading one number for both is a silent picture-mover. FlatOut 2 renders its world into a
// 640x448 PSMCT32 buffer at block 0x8c0 and then composites it down to the PSMCT16 display buffer
// at block 0; a texture read of that work buffer declares TW=1024/TH=512, so the colour surface
// grows to 640x512 while its depth partner at block 0x1a40 stays 640x448. Serving the viewport
// from min(colour, depth) then squeezed every world row by 448/512: the horizon sat 11 rows high,
// the ground ran out at row 392 of 448 with a black band under it, and the HUD -- whose sprites
// need no depth and so land in unclamped, colour-only passes -- stayed exactly where it belonged,
// which is what made the defect read as "the frame is shifted" instead of "the transform is
// wrong".
//
// The pairs below are the shapes the planner actually produces. Nothing here needs a device: the
// derivation is constexpr, which is the point of it being a function.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

namespace
{
	using Geom = GSDevice::GSTileGpuPassGeometry;

	Geom Pair(int cw, int ch, int dw, int dh)
	{
		return GSDevice::gsTileGpuPassGeometry(true, cw, ch, true, dw, dh);
	}
	Geom ColorOnly(int cw, int ch)
	{
		return GSDevice::gsTileGpuPassGeometry(true, cw, ch, false, 0, 0);
	}
	Geom DepthOnly(int dw, int dh)
	{
		return GSDevice::gsTileGpuPassGeometry(false, 0, 0, true, dw, dh);
	}
} // namespace

TEST(TileGpuPassGeometry, MatchedPairIsItsOwnSizeBothWays)
{
	const Geom g = Pair(640, 448, 640, 448);
	EXPECT_EQ(g.viewport_width, 640);
	EXPECT_EQ(g.viewport_height, 448);
	EXPECT_EQ(g.area_width, 640);
	EXPECT_EQ(g.area_height, 448);
}

// FlatOut 2's shape: the colour target grew past its depth partner because a texture read of it
// declared a 512-row texture. The framebuffer must clamp; the transform must not.
TEST(TileGpuPassGeometry, TallColourKeepsItsOwnViewportAndClampsTheArea)
{
	const Geom g = Pair(640, 512, 640, 448);
	EXPECT_EQ(g.viewport_height, 512) << "the transform divided guest y by the colour height";
	EXPECT_EQ(g.area_height, 448) << "the framebuffer cannot exceed the depth attachment";
	EXPECT_EQ(g.viewport_width, 640);
	EXPECT_EQ(g.area_width, 640);
}

// The other way round costs nothing: the area is already the colour target's own size, so both
// answers agree and the depth attachment's spare rows are simply not rendered into.
TEST(TileGpuPassGeometry, TallDepthLeavesTheColourViewportAlone)
{
	const Geom g = Pair(640, 448, 640, 512);
	EXPECT_EQ(g.viewport_height, 448);
	EXPECT_EQ(g.area_height, 448);
}

// Width clamps on the same rule, and independently of height -- a pair may differ in one and not
// the other, and a clamp that took the smaller pair wholesale would move pixels sideways.
TEST(TileGpuPassGeometry, WidthAndHeightClampIndependently)
{
	const Geom g = Pair(1024, 448, 640, 512);
	EXPECT_EQ(g.viewport_width, 1024);
	EXPECT_EQ(g.viewport_height, 448);
	EXPECT_EQ(g.area_width, 640);
	EXPECT_EQ(g.area_height, 448);
}

// A colour-only pass has nothing to clamp against: this is where the HUD lands, and it is why the
// HUD kept its position while the world moved.
TEST(TileGpuPassGeometry, ColourOnlyPassIsUnclamped)
{
	const Geom g = ColorOnly(640, 512);
	EXPECT_EQ(g.viewport_height, 512);
	EXPECT_EQ(g.area_height, 512);
	EXPECT_EQ(g.viewport_width, 640);
	EXPECT_EQ(g.area_width, 640);
}

// A depth-only pass has no colour transform to honour, so the depth attachment answers both.
TEST(TileGpuPassGeometry, DepthOnlyPassTakesTheDepthSize)
{
	const Geom g = DepthOnly(640, 448);
	EXPECT_EQ(g.viewport_height, 448);
	EXPECT_EQ(g.area_height, 448);
	EXPECT_EQ(g.viewport_width, 640);
	EXPECT_EQ(g.area_width, 640);
}

// The area is never larger than either attachment, and the viewport is never smaller than the
// area -- the first is a Vulkan requirement, the second is what keeps the scissor from cutting
// rows the pass is entitled to write.
TEST(TileGpuPassGeometry, AreaFitsBothAttachmentsAndViewportCoversTheArea)
{
	for (const int ch : {64, 448, 512, 1024})
	{
		for (const int dh : {64, 448, 512, 1024})
		{
			for (const int cw : {64, 640, 1024})
			{
				for (const int dw : {64, 640, 1024})
				{
					const Geom g = Pair(cw, ch, dw, dh);
					EXPECT_LE(g.area_width, cw);
					EXPECT_LE(g.area_width, dw);
					EXPECT_LE(g.area_height, ch);
					EXPECT_LE(g.area_height, dh);
					EXPECT_GE(g.viewport_width, g.area_width);
					EXPECT_GE(g.viewport_height, g.area_height);
					// The viewport is the colour target, whatever the depth partner does.
					EXPECT_EQ(g.viewport_width, cw);
					EXPECT_EQ(g.viewport_height, ch);
				}
			}
		}
	}
}
