// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the render target size rule below 1x (GS/Renderers/Common/GSTargetSizePolicy.h): every
// target's unscaled size times the scale is a whole number of device pixels, the rounding adds less
// than one alignment step, and at 1x and above the size is untouched.

#include "GS/Renderers/Common/GSTargetSizePolicy.h"

#include <gtest/gtest.h>

#include <cmath>

namespace
{
	// The device size the texture cache gives a target (GSTextureCache::ScaleNativeToDevice).
	int DeviceSize(int unscaled, float scale)
	{
		return static_cast<int>(std::ceil(static_cast<float>(unscaled) * scale));
	}

	// The settings the UI offers, below and above 1x.
	constexpr float kBelowNative[] = {0.25f, 0.5f, 0.75f};
	constexpr float kNativeAndAbove[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f, 2.75f, 3.0f, 3.5f, 4.0f, 8.0f};
} // namespace

TEST(GSTargetSizePolicy, AlignmentIsTheScalesDenominatorBelowNative)
{
	EXPECT_EQ(GSTargetSizeAlignment(0.75f), 4);
	EXPECT_EQ(GSTargetSizeAlignment(0.5f), 2);
	EXPECT_EQ(GSTargetSizeAlignment(0.25f), 4);
	EXPECT_EQ(GSTargetSizeAlignment(0.125f), 8);
	EXPECT_EQ(GSTargetSizeAlignment(0.0625f), 16);
}

TEST(GSTargetSizePolicy, ScalesWithoutASmallDenominatorAreLeftAlone)
{
	// 0.6 and 0.1 have no power-of-two denominator up to 16 as floats.
	EXPECT_EQ(GSTargetSizeAlignment(0.6f), 1);
	EXPECT_EQ(GSTargetSizeAlignment(0.1f), 1);
	EXPECT_EQ(GSAlignTargetSize(597, 0.6f), 597);
}

TEST(GSTargetSizePolicy, BelowNativeEveryScaledSizeIsExact)
{
	for (const float scale : kBelowNative)
	{
		const int align = GSTargetSizeAlignment(scale);
		for (int size = 0; size <= 2048; size++)
		{
			const int aligned = GSAlignTargetSize(size, scale);
			ASSERT_GE(aligned, size) << "scale " << scale << " size " << size;
			ASSERT_LT(aligned - size, align) << "scale " << scale << " size " << size;
			ASSERT_EQ(aligned % align, 0) << "scale " << scale << " size " << size;
			// The texture is exactly scale times the unscaled size, so the draw's device scale
			// (texture size / unscaled size) is the nominal one.
			ASSERT_EQ(static_cast<float>(DeviceSize(aligned, scale)), static_cast<float>(aligned) * scale)
				<< "scale " << scale << " size " << size;
		}
	}
}

TEST(GSTargetSizePolicy, ExactSizesAreKept)
{
	// The common framebuffer sizes are already multiples of 4 and must not grow.
	for (const int size : {512, 640, 448, 480, 224, 256})
	{
		EXPECT_EQ(GSAlignTargetSize(size, 0.75f), size);
		EXPECT_EQ(GSAlignTargetSize(size, 0.5f), size);
		EXPECT_EQ(GSAlignTargetSize(size, 0.25f), size);
	}
}

TEST(GSTargetSizePolicy, InexactSizesAreRoundedUp)
{
	// 597 * 0.75 = 447.75, which the texture rounds to 448: a device scale of 0.7504 instead of 0.75.
	EXPECT_EQ(GSAlignTargetSize(597, 0.75f), 600);
	EXPECT_EQ(DeviceSize(600, 0.75f), 450);
	EXPECT_EQ(GSAlignTargetSize(449, 0.75f), 452);
	EXPECT_EQ(GSAlignTargetSize(449, 0.5f), 450);
	EXPECT_EQ(GSAlignTargetSize(1, 0.25f), 4);
}

TEST(GSTargetSizePolicy, NativeAndAboveAreUnchanged)
{
	for (const float scale : kNativeAndAbove)
	{
		EXPECT_EQ(GSTargetSizeAlignment(scale), 1) << "scale " << scale;
		for (int size = 0; size <= 2048; size++)
			ASSERT_EQ(GSAlignTargetSize(size, scale), size) << "scale " << scale << " size " << size;
	}
}
