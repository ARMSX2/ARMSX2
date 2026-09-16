// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Pins the two presentation/interlace policies extracted from GSRenderer so the decisions are
// checkable without a GS device. Ported from sashkinbro/EmuCoreX ("Fix GS interlace and Vulkan
// presentation policies"), which is also where the GT4 fade case below comes from.
//
// Both policies are constexpr and additionally static_assert their key cases at their definition,
// so a regression is a compile error there and a named failure here.

#include "GS/Renderers/Common/GSInterlaceModePolicy.h"
#include "GS/Renderers/Common/GSPresentationPolicy.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

TEST(GSInterlaceModePolicy, AutomaticFullFrameOutputRemainsPassThrough)
{
	const GSInterlaceModeSelection selection =
		SelectGSInterlaceMode(0, true, false, false, false);
	EXPECT_EQ(selection.field_offset, 0);
	// -1, NOT clamped to FastMAD. This is the progressive case: clamping it here is what makes a
	// deinterlace pass run over progressive output during a video-mode transition.
	EXPECT_EQ(selection.shader_mode, -1);
}

TEST(GSInterlaceModePolicy, AutomaticTemporalSourcesUseFastMAD)
{
	EXPECT_EQ(SelectGSInterlaceMode(0, true, true, false, false).shader_mode, 3);
	EXPECT_EQ(SelectGSInterlaceMode(0, true, false, true, false).shader_mode, 3);
	EXPECT_EQ(SelectGSInterlaceMode(0, true, false, false, true).shader_mode, 3);
}

TEST(GSInterlaceModePolicy, ExplicitModesMapToExpectedShadersAndFields)
{
	EXPECT_EQ(SelectGSInterlaceMode(1, false, false, false, false).shader_mode, -1);
	EXPECT_EQ(SelectGSInterlaceMode(2, false, false, false, false).shader_mode, 0);
	EXPECT_EQ(SelectGSInterlaceMode(3, false, false, false, false).field_offset, 1);
	EXPECT_EQ(SelectGSInterlaceMode(4, false, false, false, false).shader_mode, 1);
	EXPECT_EQ(SelectGSInterlaceMode(6, false, false, false, false).shader_mode, 2);
	EXPECT_EQ(SelectGSInterlaceMode(8, false, false, false, false).shader_mode, 3);
}

TEST(GSInterlaceModePolicy, FieldRenderAtIntegerUpscaleIsPresentedDirectly)
{
	// At an integer upscale of 2 or more the field render already holds every display line of the
	// screen at that field's moment, so there is nothing for a weave to reconstruct.
	const GSInterlaceModeSelection selection = SelectGSInterlaceMode(0, true, false, true, false, true);
	EXPECT_EQ(selection.shader_mode, -1);
	EXPECT_TRUE(selection.present_field_direct);
}

TEST(GSInterlaceModePolicy, FieldDirectNeedsAutomaticFieldModeAndNoScanmask)
{
	// 1x and every fractional scale: the caller says the render is not the whole picture.
	EXPECT_FALSE(SelectGSInterlaceMode(0, true, false, true, false, false).present_field_direct);
	EXPECT_EQ(SelectGSInterlaceMode(0, true, false, true, false, false).shader_mode, 3);
	// Frame mode, even where a game moves its framebuffer per field.
	EXPECT_FALSE(SelectGSInterlaceMode(0, true, true, false, false, true).present_field_direct);
	EXPECT_EQ(SelectGSInterlaceMode(0, true, true, false, false, true).shader_mode, 3);
	// SCANMSK keeps its pass.
	EXPECT_FALSE(SelectGSInterlaceMode(0, true, false, true, true, true).present_field_direct);
	// Every explicitly chosen mode is left exactly as it was.
	for (int mode = 1; mode < 10; mode++)
		EXPECT_FALSE(SelectGSInterlaceMode(mode, false, false, true, false, true).present_field_direct);
}

TEST(GSPresentationPolicy, SkipsOnlyBlankFramesBeforeFirstOutput)
{
	EXPECT_TRUE(ShouldSkipAndroidBlankFrame(true, false, true, 1));
	EXPECT_FALSE(ShouldSkipAndroidBlankFrame(true, true, true, 1));
	EXPECT_FALSE(ShouldSkipAndroidBlankFrame(false, false, true, 0));
	EXPECT_FALSE(ShouldSkipAndroidBlankFrame(false, true, true, 0));
}

TEST(GSPresentationPolicy, PreservesExistingOpenGLBlankSuppression)
{
	EXPECT_TRUE(ShouldSkipAndroidBlankFrame(true, false, false, 1));
	EXPECT_TRUE(ShouldSkipAndroidBlankFrame(true, true, false, 1));
	EXPECT_FALSE(ShouldSkipAndroidBlankFrame(true, true, false, 2));
	EXPECT_FALSE(ShouldSkipAndroidBlankFrame(false, true, false, 0));
}

TEST(GSPresentationPolicy, KeepsAlternatingMidGameFadeFramesOnSubmissionPath)
{
	// GT4 result transitions can alternate between output and blank frames while remaining in
	// SDTV 480p. Only the leading startup blank may bypass presentation.
	constexpr std::array<bool, 6> blank_frames = {true, false, true, false, true, false};
	bool has_current_output = false;
	std::array<bool, blank_frames.size()> skipped = {};

	for (size_t i = 0; i < blank_frames.size(); i++)
	{
		skipped[i] = ShouldSkipAndroidBlankFrame(
			blank_frames[i], has_current_output, true, blank_frames[i] ? 1 : 0);
		if (!blank_frames[i])
			has_current_output = true;
	}

	EXPECT_EQ(skipped, (std::array<bool, 6>{true, false, false, false, false, false}));
}
