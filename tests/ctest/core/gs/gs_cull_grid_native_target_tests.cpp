// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that the cull grid never drops a primitive a native-scale target would light.
//
// The grid is chosen once from the config, but the renderer does not draw every target at the
// upscale: native scaling and native palette draws render some targets at scale 1. A grid is safe
// for such a target only if it contains every scale-1 sample point, which sit at the whole-pixel
// sub-texels 16k. The plain upscaled grids (8k at 2x, 4k at 4x) contain them. The Native
// half-pixel-offset grid does not: it is shifted half a device step, 8k+4 at 2x, so a triangle
// spanning sub-texels [254, 258) holds the scale-1 point 256 and no grid point, and is culled
// although the scale-1 target lights a pixel for it. About 680 GameDB entries pair
// halfPixelOffset Native with nativeScaling.

#include "GS/GSState.h"

#include <gtest/gtest.h>

namespace
{
	// Whether sub-texel coordinate w is one of the grid's points on the triangle axis.
	bool OnGrid(const GSVertexKernels::CullGrid& grid, int w)
	{
		if (grid.shift == 0)
			return true; // no grid: every point is a sample point, nothing is culled
		const int step = 1 << grid.shift;
		const int phase = grid.phase.x;
		return (((w - phase) % step) + step) % step == 0;
	}

	// Every point `native` asks for is a point of `grid`, over a span wide enough to see a period.
	bool Contains(const GSVertexKernels::CullGrid& grid, const GSVertexKernels::CullGrid& native)
	{
		for (int w = 0; w < 256; w++)
			if (OnGrid(native, w) && !OnGrid(grid, w))
				return false;
		return true;
	}
} // namespace

TEST(GSCullGridNativeTargets, PlainUpscaledGridsContainTheNativePoints)
{
	const GSVertexKernels::CullGrid native = GSState::CullGridFor(1.0f, GSHalfPixelOffset::Off);
	for (const GSHalfPixelOffset hpo : {GSHalfPixelOffset::Off, GSHalfPixelOffset::Special, GSHalfPixelOffset::SpecialAggressive, GSHalfPixelOffset::NativeWTexOffset})
	{
		EXPECT_TRUE(Contains(GSState::CullGridFor(2.0f, hpo), native));
		EXPECT_TRUE(Contains(GSState::CullGridFor(4.0f, hpo), native));
	}
}

// With no target rendering at scale 1, Native keeps its own phased grid.
TEST(GSCullGridNativeTargets, NativeKeepsItsGridWhenEveryTargetIsUpscaled)
{
	const GSVertexKernels::CullGrid grid = GSState::CullGridFor(2.0f, GSHalfPixelOffset::Native, false);
	EXPECT_EQ(grid.shift, 3);
	EXPECT_EQ(grid.phase.x, 4);
}

TEST(GSCullGridNativeTargets, NativeContainsTheNativePointsWhenATargetCanBeNative)
{
	const GSVertexKernels::CullGrid native = GSState::CullGridFor(1.0f, GSHalfPixelOffset::Native);
	for (const float scale : {2.0f, 4.0f, 8.0f})
	{
		const GSVertexKernels::CullGrid upscaled = GSState::CullGridFor(scale, GSHalfPixelOffset::Native, false);
		const GSVertexKernels::CullGrid grid = GSState::CullGridFor(scale, GSHalfPixelOffset::Native, true);

		EXPECT_TRUE(Contains(grid, native)) << "scale " << scale;
		EXPECT_TRUE(Contains(grid, upscaled)) << "scale " << scale << ": the upscaled targets' points";
	}

	// The triangle from the header comment: [254, 258) holds 256, so it must survive.
	const GSVertexKernels::CullGrid grid = GSState::CullGridFor(2.0f, GSHalfPixelOffset::Native, true);
	bool any = false;
	for (int w = 254; w < 258; w++)
		any |= OnGrid(grid, w);
	EXPECT_TRUE(any);
}
