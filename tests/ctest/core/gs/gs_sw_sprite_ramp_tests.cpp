// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The UV-route sprite ramp bias is a hair, not a sixteenth of a texel.
//
// Spider-Man 3's blur passes draw sprites one texel per pixel down a 448, 224,
// 112 or 56 row extent, seeding V at 1.53125 texels (a thirty-second above 1.5).
// The linear filter takes half a texel off, so the sample point sits at 1.03125
// texels, which is 16.5 sixteenths: half a sixteenth above the boundary between
// sixteenth 15 and 16. A bias of a whole sixteenth crosses that boundary and
// reads the row above; a hair does not. The console's frame agrees with the hair
// (98.03% of the words of the replayed frame identical against 69.80% for a
// sixteenth). Every console capture behind the old rule seeded on a whole
// sixteenth, where the two are the same.
//
// GSCoordinateWalk.h carries the reading. Coordinates below are the 16.16 units
// the software renderer holds, with the linear filter's half texel already off.

#include "GS/Renderers/SW/GSCoordinateWalk.h"

#include <gtest/gtest.h>

namespace
{
constexpr float kTexel = 65536.0f;

// V of the blur passes' first row after the filter's half texel: 1.03125 texels.
constexpr float kBlurSeed = 1.53125f * kTexel - 0.5f * kTexel;

// The sixteenth the scanline reads a coordinate in: floored onto the accumulator
// grid, then the bits above the sixteenth.
s32 SixteenthOf(float coord)
{
	return GSAffineCoordinateOnGrid(coord) >> GS_COORD_SIXTEENTH_SHIFT;
}
} // namespace

// Bounds the size to what the frame measured: 1 to 2048 units give one picture and
// 2049 up give the sixteenth's.
TEST(GSSwSpriteRamp, BiasIsInsideTheHairWindow)
{
	EXPECT_GT(GS_UV_RAMP_BIAS, 0.0f);
	EXPECT_LE(GS_UV_RAMP_BIAS, GS_UV_SIXTEENTH / 2.0f);
}

TEST(GSSwSpriteRamp, AppliesOnlyToAscendingWholeSixteenthStepsOnAnExtentThatIsNotAPowerOfTwo)
{
	EXPECT_EQ(GSSpriteRampBias(kTexel, 448), GS_UV_RAMP_BIAS);
	EXPECT_EQ(GSSpriteRampBias(kTexel, 96), GS_UV_RAMP_BIAS);
	EXPECT_EQ(GSSpriteRampBias(2.0f * kTexel, 192), GS_UV_RAMP_BIAS);
	EXPECT_EQ(GSSpriteRampBias(GS_UV_SIXTEENTH, 448), GS_UV_RAMP_BIAS);

	// A power-of-two extent is exact.
	EXPECT_EQ(GSSpriteRampBias(kTexel, 64), 0.0f);
	EXPECT_EQ(GSSpriteRampBias(kTexel, 512), 0.0f);

	// A step that is not a whole number of sixteenths is exact: the test for that
	// is the sixteenth, not the size of the bias.
	EXPECT_EQ(GSSpriteRampBias(kTexel + GS_UV_SIXTEENTH / 2.0f, 448), 0.0f);
	EXPECT_EQ(GSSpriteRampBias(kTexel + 2.0f, 448), 0.0f);

	// Descending and still ramps are exact.
	EXPECT_EQ(GSSpriteRampBias(-kTexel, 448), 0.0f);
	EXPECT_EQ(GSSpriteRampBias(0.0f, 448), 0.0f);
}

// The seed that separates a hair from a sixteenth.
TEST(GSSwSpriteRamp, HairKeepsASeedHalfASixteenthAboveABoundaryOnItsRow)
{
	// 1.03125 texels is 16.5 sixteenths.
	EXPECT_EQ(SixteenthOf(kBlurSeed), 16);

	EXPECT_EQ(SixteenthOf(GSCoordinateLowered(kBlurSeed, GS_UV_RAMP_BIAS)), 16);

	// What a whole sixteenth did: the row above.
	EXPECT_EQ(SixteenthOf(GSCoordinateLowered(kBlurSeed, GS_UV_SIXTEENTH)), 15);
}

// On a whole sixteenth a hair and a sixteenth agree, which is why the captures that
// measured the term (gs-tex1..7) do not tell them apart.
TEST(GSSwSpriteRamp, HairDropsASeedOnABoundaryToTheSixteenthBelow)
{
	for (int n = -40; n <= 4000; n += 37)
	{
		const float coord = static_cast<float>(n) * GS_UV_SIXTEENTH;

		EXPECT_EQ(SixteenthOf(GSCoordinateLowered(coord, GS_UV_RAMP_BIAS)), n - 1) << n;
		EXPECT_EQ(SixteenthOf(GSCoordinateLowered(coord, GS_UV_SIXTEENTH)), n - 1) << n;
	}
}

// Above 2^24 units (256 texels) a float cannot hold a change of one unit, and the
// plain subtract rounds to even and returns the coordinate. The blur pass over 448
// rows crosses 256 texels at row 255, so 193 of its rows lost the bias that way and
// the frame scored 88.90% instead of 98.03%.
TEST(GSSwSpriteRamp, LoweringSurvivesTheFloatGrain)
{
	// Row 300 of the 448-row pass: 19,761,152 units, past 2^24.
	const volatile float coord = kBlurSeed + 300.0f * kTexel;

	EXPECT_GT(coord, 16777216.0f);
	EXPECT_EQ(coord - GS_UV_RAMP_BIAS, coord) << "the plain subtract is the hazard";
	EXPECT_LT(GSCoordinateLowered(coord, GS_UV_RAMP_BIAS), coord);
	EXPECT_LT(GSAffineCoordinateOnGrid(GSCoordinateLowered(coord, GS_UV_RAMP_BIAS)),
		GSAffineCoordinateOnGrid(coord));

	for (int row = 0; row < 448; row++)
	{
		const float c = kBlurSeed + static_cast<float>(row) * kTexel;
		const float lowered = GSCoordinateLowered(c, GS_UV_RAMP_BIAS);

		EXPECT_LT(lowered, c) << row;
		EXPECT_LT(GSAffineCoordinateOnGrid(lowered), GSAffineCoordinateOnGrid(c)) << row;
	}
}

// The bias stays a hair across the whole 12.4 field (+-2048 texels, 2^27 units): a
// float step there is at most 16 units, far inside half a sixteenth.
TEST(GSSwSpriteRamp, LoweringStaysInsideTheWindowAcrossTheField)
{
	for (int k = 0; k <= 27; k++)
	{
		const float c = 1.5f * static_cast<float>(1 << k);
		const float lowered = GSCoordinateLowered(c, GS_UV_RAMP_BIAS);

		EXPECT_LT(lowered, c) << k;
		EXPECT_LE(c - lowered, GS_UV_SIXTEENTH / 2.0f) << k;

		EXPECT_LT(GSCoordinateLowered(-c, GS_UV_RAMP_BIAS), -c) << k;
	}
}
