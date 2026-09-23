// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the native-texel-grid rule (GS/Renderers/Common/GSNativeTexelGridPolicy.h).
//
// A sprite that minifies a GS-memory texture under a nearest sampler reads the texel its NATIVE
// pixel would have read. Two halves are pinned here and they fail differently, so they are tested
// separately:
//
//   * WHICH draws take the road. That is almost entirely a "nothing else moved" statement -- the
//     rule is narrow on purpose, and every draw that does not meet all six gates has to render
//     exactly as it did. Widening any one gate shows up as a failure here.
//   * The ARITHMETIC. The shaders repeat it, so a drift between the two is silent wrong output on
//     the one road the change exists for. The numbers below are NASCAR Thunder 2002's own, read
//     off the real 1x and 2x renders, so the test says "this expression puts
//     the device pixel on the texel the console displayed" rather than "this expression is what
//     was written".
//
// Rides gs_vertex_tests -- the policy is header-only, so it needs no extra linkage.

#include "GS/Renderers/Common/GSNativeTexelGridPolicy.h"

#include <gtest/gtest.h>

#include <cmath>

namespace
{
	// Sixteenths, which is how both UV and XY reach the vertex buffer.
	constexpr int kSub = 16;

	// NASCAR Thunder 2002's haze sprite, right half of the screen: U runs 0.5 .. 640.5 over X
	// 320 .. 640, so 640 texels across 320 native pixels, and V runs 0.5 .. 448.5 over Y 0 .. 448,
	// which is 1:1. PSMT8 out of GS memory, nearest, no mipmap, sprite class.
	GSNativeTexelGridInputs NascarHazeSprite(float scale)
	{
		GSNativeTexelGridInputs in;
		in.sprite = true;
		in.texture_from_memory = true;
		in.texel_coordinates = true;
		in.nearest = true;
		in.mipmapped = false;
		in.scale = scale;
		in.step_u = GSMakeNativeTexelStep(640 * kSub, 320 * kSub);
		in.step_v = GSMakeNativeTexelStep(448 * kSub, 448 * kSub);
		return in;
	}

} // namespace

TEST(GSNativeTexelGrid, NascarHazeSpriteTakesTheRoadAtTwoTimes)
{
	EXPECT_TRUE(GSSpriteSamplesOnTheNativeTexelGrid(NascarHazeSprite(2.0f)));

	// U minifies 2:1 and gets a step; V is 1:1 and is left entirely alone.
	EXPECT_FLOAT_EQ(GSNativeTexelGridStep(NascarHazeSprite(2.0f).step_u), 2.0f);
	EXPECT_FLOAT_EQ(GSNativeTexelGridStep(NascarHazeSprite(2.0f).step_v), 0.0f);
}

TEST(GSNativeTexelGrid, NativeScaleIsRefused)
{
	// Not merely harmless at 1x -- refused, so native keeps exactly the shader permutations it had
	// and the byte-identity gate at 1x holds by construction.
	EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(NascarHazeSprite(1.0f)));
}

TEST(GSNativeTexelGrid, EachGateRefusesOnItsOwn)
{
	{
		GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
		in.sprite = false;
		EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(in)) << "a triangle interpolates; its own sample point is right";
	}
	{
		GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
		in.texture_from_memory = false;
		EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(in)) << "a target source is already at device resolution";
	}
	{
		GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
		in.texel_coordinates = false;
		EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(in)) << "an STQ draw's step is not the integer ratio";
	}
	{
		GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
		in.nearest = false;
		EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(in)) << "a bilinear sprite already averages the skipped texels";
	}
	{
		GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
		in.mipmapped = true;
		EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(in)) << "the mip level is a function of the same step";
	}
	{
		GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
		in.field_render = true;
		EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(in)) << "a field render's extra device rows are display lines, not copies";
	}
}

TEST(GSNativeTexelGrid, AFieldRenderIsRefusedAndAnInterlacedFrameIsNot)
{
	// The two are different facts and the corpus contains both. Armored Core 3 and Shin Onimusha
	// draw half-height fields (SMODE2.FFMD), and at an integer upscale the merge presents the field
	// render as the whole picture because its extra device rows hold the display lines between the
	// field's own -- which is exactly what this rule takes back out. NASCAR's output is interlaced
	// too, with FFMD clear and whole 640x448 targets, so interlaced output gates nothing.
	GSNativeTexelGridInputs field = NascarHazeSprite(2.0f);
	field.field_render = true;
	EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(field));

	GSNativeTexelGridInputs whole = NascarHazeSprite(2.0f);
	whole.field_render = false;
	EXPECT_TRUE(GSSpriteSamplesOnTheNativeTexelGrid(whole));
}

TEST(GSNativeTexelGrid, TheDeviceGridHasToBeTheNativeGridScaled)
{
	// Read off the real draws at 2x. The first three titles put native coordinate 0 at device 0.5
	// and native 1 at 2.5; Katamari Damacy runs under GameDB halfPixelOffset 4 (Native), which puts
	// them at 1.0 and 3.0, and that half-device-pixel shift is what makes floor(fragment)/scale the
	// wrong native pixel for it.
	EXPECT_TRUE(GSDeviceGridIsNativeGridScaled(2.0f, 0.5000f, 2.0f)) << "NASCAR Thunder 2002";
	EXPECT_TRUE(GSDeviceGridIsNativeGridScaled(2.0f, 0.4999f, 2.0f)) << "Armored Core 3";
	EXPECT_TRUE(GSDeviceGridIsNativeGridScaled(2.0f, 0.5000f, 2.0f)) << "Shin Onimusha";
	EXPECT_FALSE(GSDeviceGridIsNativeGridScaled(2.0f, 1.0000f, 2.0f)) << "Katamari Damacy";

	// The slack exists for the reciprocal in the vertex offset, and for nothing wider than that.
	EXPECT_TRUE(GSDeviceGridIsNativeGridScaled(2.0f, 0.5f + 1.0f / 128.0f, 2.0f));
	EXPECT_FALSE(GSDeviceGridIsNativeGridScaled(2.0f, 0.5f + 1.0f / 32.0f, 2.0f));

	// The rate has to be the draw's own scale as well as the offset being half a pixel: a target
	// whose texture is larger than its native size scaled walks the device grid at another rate,
	// and the fragment's native coordinate is then not its device coordinate over the scale.
	EXPECT_FALSE(GSDeviceGridIsNativeGridScaled(2.125f, 0.5f, 2.0f));
	EXPECT_FALSE(GSDeviceGridIsNativeGridScaled(1.0f, 0.5f, 2.0f));

	// Every scale the corpus runs, with the plain mapping.
	for (const float scale : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 8.0f})
		EXPECT_TRUE(GSDeviceGridIsNativeGridScaled(scale, 0.5f, scale)) << "scale " << scale;
}

TEST(GSNativeTexelGrid, OrdinaryUpscaledBlitsAreUntouched)
{
	// The shape of nearly every textured sprite in the corpus: one texel per pixel, or magnified.
	// These are what "nothing else moved" means.
	for (const int texels : {80, 160, 320, 480, 639})
	{
		GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
		in.step_u = GSMakeNativeTexelStep(texels * kSub, 320 * kSub);
		in.step_v = GSMakeNativeTexelStep(224 * kSub, 224 * kSub);
		EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(in)) << "step " << texels << "/320";
	}

	// Exactly two texels per pixel is where the console starts skipping, and is in.
	GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
	in.step_u = GSMakeNativeTexelStep(640 * kSub, 320 * kSub);
	EXPECT_TRUE(GSSpriteSamplesOnTheNativeTexelGrid(in));
}

TEST(GSNativeTexelGrid, MirroredSpriteMinifiesByMagnitudeAndKeepsItsSign)
{
	GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
	in.step_u = GSMakeNativeTexelStep(-640 * kSub, 320 * kSub);
	EXPECT_TRUE(GSSpriteSamplesOnTheNativeTexelGrid(in));
	EXPECT_FLOAT_EQ(GSNativeTexelGridStep(in.step_u), -2.0f);

	// The same sprite with its two corners the other way round is the same sprite.
	EXPECT_FLOAT_EQ(GSNativeTexelGridStep(GSMakeNativeTexelStep(640 * kSub, -320 * kSub)), -2.0f);
}

TEST(GSNativeTexelGrid, DegenerateSpriteHasNoStep)
{
	GSNativeTexelGridInputs in = NascarHazeSprite(2.0f);
	in.step_u = GSMakeNativeTexelStep(640 * kSub, 0);
	in.step_v = GSMakeNativeTexelStep(448 * kSub, 0);
	EXPECT_FALSE(GSSpriteSamplesOnTheNativeTexelGrid(in));
}

TEST(GSNativeTexelGrid, StepsAgreeAcrossSpritesOfOneDraw)
{
	// Same rate in different terms -- a half-width sprite over half the screen.
	EXPECT_TRUE(GSNativeTexelStepsAgree(
		GSMakeNativeTexelStep(640 * kSub, 320 * kSub), GSMakeNativeTexelStep(320 * kSub, 160 * kSub)));

	// One texel out over the whole span is a disagreement, because the shader carries one step.
	EXPECT_FALSE(GSNativeTexelStepsAgree(
		GSMakeNativeTexelStep(640 * kSub, 320 * kSub), GSMakeNativeTexelStep(639 * kSub, 320 * kSub)));

	// A sprite read mirrored does not agree with the same sprite read forwards.
	EXPECT_FALSE(GSNativeTexelStepsAgree(
		GSMakeNativeTexelStep(640 * kSub, 320 * kSub), GSMakeNativeTexelStep(-640 * kSub, 320 * kSub)));
}
