// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that the field-shift detector's decision is not permanent.
//
// The detector watches a dozen fields and decides whether the game moves its projection half a
// line between fields. A decision taken on a boot logo or a static menu -- two fields that are the
// same picture, which reads as "no shift" -- used to stand until the renderer was reset, so a game
// whose 3D scenes do shift kept the unshifted presentation for the whole session. It also ignored
// a video-mode change once decided, although the picture it measured was gone. Now a decision is
// re-examined on a video-mode change and again every GS_FIELD_SHIFT_RECHECK_FIELDS fields, and the
// old decision stays in force while the new one is measured.
//
// Driven on the deviceless None backend: its readbacks come back zero, which is a flat picture,
// so every pair is uninformative and each round ends on the probe budget with the default.

#include "GS/Renderers/Common/GSFieldShiftDetector.h"
#include "GS/Renderers/Common/GSFieldShiftPolicy.h"
#include "GS/Renderers/Null/GSDeviceNone.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{
	class GSFieldShiftDetectorLifetime : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			g_gs_device = std::make_unique<GSDeviceNone>();
			ASSERT_TRUE(g_gs_device->Create(GSVSyncMode::Disabled, false));
			m_merge = g_gs_device->CreateRenderTarget(1280, 896, GSTexture::Format::Color, false);
			ASSERT_NE(m_merge, nullptr);
		}

		void TearDown() override
		{
			m_detector.Reset();
			if (m_merge)
				g_gs_device->Recycle(m_merge);
			g_gs_device->Destroy();
			g_gs_device.reset();
		}

		void Feed(int fields, const GSVector2i& size = GSVector2i(1280, 896))
		{
			for (int i = 0; i < fields; i++, m_field++)
				m_detector.Update(m_merge, size, 2, 0, m_field & 1);
		}

		void FeedUntilDecided()
		{
			for (int i = 0; i < 64 && m_detector.IsProbing(); i++)
				Feed(1);
			ASSERT_FALSE(m_detector.IsProbing()) << "no decision within 64 fields";
		}

		GSFieldShiftDetector m_detector;
		GSTexture* m_merge = nullptr;
		int m_field = 0;
	};
} // namespace

TEST_F(GSFieldShiftDetectorLifetime, ADecisionIsReexaminedAfterTheRecheckInterval)
{
	FeedUntilDecided();

	Feed(GS_FIELD_SHIFT_RECHECK_FIELDS - 1);
	EXPECT_FALSE(m_detector.IsProbing());

	Feed(1);
	EXPECT_TRUE(m_detector.IsProbing());
	EXPECT_FALSE(m_detector.IsPending()) << "the old decision stays in force while re-measuring";

	FeedUntilDecided();
}

TEST_F(GSFieldShiftDetectorLifetime, AVideoModeChangeReexaminesADecision)
{
	FeedUntilDecided();

	Feed(1, GSVector2i(1280, 1024));
	EXPECT_TRUE(m_detector.IsProbing());
	EXPECT_FALSE(m_detector.IsPending());
}
