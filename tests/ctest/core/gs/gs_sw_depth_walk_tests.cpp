// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for the SW rasterizer's depth walk.
//
// The gs-zgrad probe, run on an SCPH-30001 against the software arm, measured
// that an interpolated depth never reaches its plane: on 13.64% of gradient
// pixels our stored depth was one unit away from silicon's, and the differing
// pixels were exactly the ones whose exact depth lands on an integer. Applying
// the shortfall took that to 3.80%, and the same change took the gs-interp
// capture's depth sections from 2450 differing readings to 1898.
//
// What is pinned here is the SHAPE of the shortfall, because it is asymmetric
// between the axes in a way that looks like a bug and is not. Each half was
// established by rebuilding and re-running the probe against the console, and
// the alternative is recorded with the number it scored:
//
//   * a flat triangle takes NO bias -- silicon is exact on all 896 flat
//     readings, so this belongs to the walk and not to the seed;
//   * the X half does not follow the gradient's sign (signing it: gs-zgrad's
//     gridx section 192 wrong pixels -> 384);
//   * the Y half does, and only once the walk has stepped off the vertex
//     scanline (not gating and not signing it: ygrad 0 -> 448 wrong pixels).
//
// One unit of depth is the entire margin a depth test between consecutive Z
// levels decides on, so none of this is cosmetic: it is what a Z-laddered scene
// resolves on. The capture, not this file, is the gate -- these are pins that
// make a silent "simplification" of the rule fail loudly.

#include "GS/Renderers/SW/GSDepthWalk.h"

#include <gtest/gtest.h>

namespace
{
constexpr double kStep = 1.0 / 1024.0; // the grid the depth step is truncated onto
}

TEST(GSDepthWalk, FlatPrimitiveTakesNoBias)
{
	// Silicon stores a flat triangle's depth exactly: 896 of 896 readings.
	EXPECT_EQ(GSDepthWalkBias(0.0, 0.0, 0.0), 0.0);
	EXPECT_EQ(GSDepthWalkBias(0.0, 0.0, 17.0), 0.0);
}

TEST(GSDepthWalk, XGradientBiasesFromTheFirstScanline)
{
	// The X shortfall is there at the vertex itself, so it does not wait for dy.
	EXPECT_EQ(GSDepthWalkBias(0.25, 0.0, 0.0), kGSDepthWalkBias);
	EXPECT_EQ(GSDepthWalkBias(0.25, 0.0, 9.0), kGSDepthWalkBias);
}

TEST(GSDepthWalk, XGradientBiasDoesNotFollowItsSign)
{
	// A falling X gradient runs short in the same direction as a rising one.
	// Signing this half doubled gs-zgrad's gridx miss count.
	EXPECT_EQ(GSDepthWalkBias(-0.5, 0.0, 0.0), kGSDepthWalkBias);
	EXPECT_EQ(GSDepthWalkBias(-8192.0, 0.0, 3.0), kGSDepthWalkBias);
}

TEST(GSDepthWalk, YGradientIsExactOnTheVertexScanline)
{
	// The Y shortfall accumulates, so the seed row carries none of it.
	EXPECT_EQ(GSDepthWalkBias(0.0, 0.5, 0.0), 0.0);
	EXPECT_EQ(GSDepthWalkBias(0.0, -0.5, 0.0), 0.0);
}

TEST(GSDepthWalk, YGradientBiasPointsBackTowardTheSeed)
{
	// Rising runs low, falling runs HIGH -- the shortfall is in the walk's
	// own direction, which is what took the ygrad section to zero.
	EXPECT_EQ(GSDepthWalkBias(0.0, 0.5, 1.0), kGSDepthWalkBias);
	EXPECT_EQ(GSDepthWalkBias(0.0, -0.5, 1.0), -kGSDepthWalkBias);
}

TEST(GSDepthWalk, AxesCompose)
{
	EXPECT_EQ(GSDepthWalkBias(0.25, 0.5, 4.0), 2.0 * kGSDepthWalkBias);
	// A falling Y under a rising X cancels rather than compounding.
	EXPECT_EQ(GSDepthWalkBias(0.25, -0.5, 4.0), 0.0);
}

TEST(GSDepthWalk, BiasMovesIntegerLandingsAndNothingElse)
{
	// The property the magnitude exists for: a value that lands exactly on an
	// integer drops below it, and a value even one step of the 2^-10 grid above
	// that integer does not. This is what makes the bias safe to apply to the
	// seed instead of testing every pixel at the store.
	const double bias = GSDepthWalkBias(0.25, 0.0, 0.0);
	for (const double z : {1.0, 2.0, 1024.0, 8388608.0})
	{
		EXPECT_LT(z - bias, z) << "an exact landing must fall below the integer";
		EXPECT_GE(z - bias, z - 1.0) << "and must never fall a whole unit";
		EXPECT_GT((z + kStep) - bias, z) << "one step above must stay put";
	}
}
