// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu SCISSOR ROAD: which of the two ways of applying the per-draw GS scissor a device
// takes, and -- the part that has to be exact -- that the two reject the same fragments.
//
// Why there are two. The scissor rides as four vertex-shader clip planes written out of the draw's
// state row, which is the cheap road: one indirect call then carries any number of scissors, so the
// scissor is part of no cut. It needs shaderClipDistance, and the Mali blob on the RG477V does not
// offer it -- and a vertex module that merely DECLARES the SPIR-V ClipDistance capability is one
// such a driver may refuse, so "write zeros into it" is not an escape. The road that needs no
// feature sets a Vulkan scissor per indirect call and cuts the call wherever the rectangle changes.
//
// What is pinned here:
//
//  - The three-state road selection, the same shape as the pass cap and the specialization budget.
//    Including the one asymmetry: a force value can turn the clip planes OFF anywhere, and cannot
//    turn them ON where the device has no feature to turn on.
//
//  - The RECTANGLE, against the clip planes' own arithmetic rather than against itself. The clip
//    road's inclusion test is a float compare on an interpolated distance and the scissor road's is
//    an integer range test, and the whole substitution rests on them selecting the same pixel
//    columns. So the reference here is the shader's expression, transcribed: the vertex stage writes
//    `pix.x - sc_x0` and `sc_x1 - pix.x` where `pix` is the vertex in target pixels, while
//    gl_Position puts that same vertex at framebuffer x = pix.x + 0.5 - 0.05/16 (the 12.4 nudge the
//    transform applies and the clip planes do not). So a fragment sampled at framebuffer x = i + 0.5
//    carries pix.x = i + 0.05/16, and THAT is what the planes compare. A ULP here is a column of
//    pixels, which is why it is a test and not a comment.
//
//  - The clamping, which is not cosmetic: a Vulkan scissor offset must be non-negative and inside
//    the area the pass declared, and a GS scissor whose high edge is below its low one is legal and
//    means "nothing" -- which has to arrive as a zero extent, never a negative one.
//
// ⚠️ What this file CANNOT pin, and the reason the two roads are not byte-identical: clipping is
// geometric. A clipped primitive is re-interpolated from its clipped vertices, so a fragment inside
// the scissor can take an attribute value one ULP from the one the unclipped primitive's plane
// gives it. Coverage is identical -- measured, by running both mechanisms at once over the corpus --
// but the shaded value on a clipped primitive is not, and no scissor rectangle can make it so.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

namespace
{
	using Road = GSDevice::GSTileGpuScissorRoad;

	/// The vertex shader's clip-plane test for one pixel column, transcribed. `pix` is the target
	/// pixel coordinate the shader computes for a fragment sampled at framebuffer x = i + 0.5; the
	/// 1/320 is the 0.05-of-a-16th nudge gl_Position applies to the vertex and the clip distance does
	/// not. A clip distance is out only when it is NEGATIVE, so both bounds are >= 0.
	bool ClipPlanesAdmitColumn(int i, int lo, int hi)
	{
		const float pix = static_cast<float>(i) + (0.05f / 16.0f);
		return (pix - static_cast<float>(lo)) >= 0.0f && (static_cast<float>(hi) - pix) >= 0.0f;
	}

	/// The scissor road's test for the same column: Vulkan keeps the pixel whose integer coordinate
	/// is in [offset, offset + extent).
	bool ScissorAdmitsColumn(int i, int offset, int extent)
	{
		return i >= offset && i < (offset + extent);
	}
} // namespace

TEST(GSTileGpuScissorRoad, ZeroAsksTheDevice)
{
	EXPECT_EQ(GSDevice::gsTileGpuScissorRoad(0, true), Road::ClipPlanes);
	EXPECT_EQ(GSDevice::gsTileGpuScissorRoad(0, false), Road::PerCall);
}

TEST(GSTileGpuScissorRoad, PositiveForcesThePerCallRoadAnywhere)
{
	// The arm a byte-comparison needs on a device that would never take it otherwise.
	EXPECT_EQ(GSDevice::gsTileGpuScissorRoad(1, true), Road::PerCall);
	EXPECT_EQ(GSDevice::gsTileGpuScissorRoad(4096, true), Road::PerCall);
	EXPECT_EQ(GSDevice::gsTileGpuScissorRoad(1, false), Road::PerCall);
}

TEST(GSTileGpuScissorRoad, NegativeAsksForClipPlanesAndCannotConjureTheFeature)
{
	EXPECT_EQ(GSDevice::gsTileGpuScissorRoad(-1, true), Road::ClipPlanes);
	// The asymmetry, and the whole of "fail closed" here: there is no third road to fall to, so a
	// device without shaderClipDistance keeps the per-call road whatever the setting asks for.
	EXPECT_EQ(GSDevice::gsTileGpuScissorRoad(-1, false), Road::PerCall);
	EXPECT_EQ(GSDevice::gsTileGpuScissorRoad(-4096, false), Road::PerCall);
}

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

TEST(GSTileGpuScissorRect, SelectsExactlyTheColumnsTheClipPlanesDo)
{
	// The substitution argument, executed. Every scissor the corpus's shapes cover, against every
	// pixel column of a 640-wide target, both tests -- the shader's float compare on the
	// interpolated distance and Vulkan's integer range test. A disagreement anywhere is a column of
	// wrong pixels on a device that has no other road.
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
				EXPECT_EQ(ScissorAdmitsColumn(i, r.x, r.width), ClipPlanesAdmitColumn(i, lo, hi))
					<< "column " << i << " of scissor [" << lo << ", " << hi << ")";
			}
		}
	}
}

TEST(GSTileGpuScissorRect, TheSameHoldsOnTheVerticalAxis)
{
	// Same arithmetic, second axis, because the shader writes four planes and a transposed
	// off-by-one would be invisible in the horizontal test above.
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
				EXPECT_EQ(ScissorAdmitsColumn(i, r.y, r.height), ClipPlanesAdmitColumn(i, lo, hi))
					<< "row " << i << " of scissor [" << lo << ", " << hi << ")";
			}
		}
	}
}
