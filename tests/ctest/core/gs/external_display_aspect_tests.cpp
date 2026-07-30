// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSRenderer.h"

#include <gtest/gtest.h>

TEST(ExternalDisplayAspect, PreventsStretchAndFitsCentered)
{
	EXPECT_EQ(GSResolveDisplayAspectRatio(AspectRatioType::Stretch, true),
		AspectRatioType::RAuto4_3_3_2);
	EXPECT_EQ(GSResolveDisplayAspectRatio(AspectRatioType::Stretch, false),
		AspectRatioType::Stretch);

	const GSDisplayFit four_three =
		GSCalculateDisplayFit(1920, 1080, 4.0f / 3.0f, 1.0f, 100.0f);
	EXPECT_FLOAT_EQ(four_three.width, 1440.0f);
	EXPECT_FLOAT_EQ(four_three.height, 1080.0f);
	EXPECT_FLOAT_EQ((1920.0f - four_three.width) * 0.5f, 240.0f);

	const GSDisplayFit sixteen_nine =
		GSCalculateDisplayFit(1920, 1080, 16.0f / 9.0f, 1.0f, 100.0f);
	EXPECT_FLOAT_EQ(sixteen_nine.width, 1920.0f);
	EXPECT_FLOAT_EQ(sixteen_nine.height, 1080.0f);
}
