// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that "Disable Safe Features" turns the upscaled point correction off, as it does the line
// correction.
//
// Above native a point is snapped to the pixel the GS lights and drawn as that pixel's whole device
// block, with an offset written for that figure (GSPointPlace.h). The line path runs its pixel-run
// correction only with safe features on, or at native, and leaves the draw to DetermineVSConfig's
// offset otherwise. Points did not: they were snapped and given the corrected offset in every case.
// With safe features off above native, a point is now drawn where the game put it, with the
// half-pixel-offset mode's offset, the way lines are.

#include "gs_hw_draw_harness.h"

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kDstFBP = 0x40;

	// 10.75 native pixels: not on a pixel boundary, so the snap moves it.
	constexpr int kPointX = 10 * 16 + 12;
	constexpr int kPointY = 20 * 16 + 12;

	class GSPointSafeFeatures : public Fixture
	{
	protected:
		void DrawPoint()
		{
			Packet p;
			Environment(p, kDstFBP, PSMCT32);
			p.Vertex(kPointX, kPointY, 1, 0, 0);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_POINTLIST;
			p.Send(*m_gs, prim);
		}
	};
} // namespace

TEST_F(GSPointSafeFeatures, SafeFeaturesOnSnapsTheUpscaledPoint)
{
	BringUp();
	const u32 before = m_device->m_draws;
	DrawPoint();
	ASSERT_EQ(m_device->m_draws, before + 1);
	ASSERT_FALSE(m_device->m_verts.empty());
	EXPECT_NE(m_device->m_verts[0].XYZ.X, kPointX);
}

TEST_F(GSPointSafeFeatures, SafeFeaturesOffLeavesTheUpscaledPointWhereTheGamePutIt)
{
	GSConfig.UserHacks_DisableSafeFeatures = true;
	BringUp();
	const u32 before = m_device->m_draws;
	DrawPoint();
	ASSERT_EQ(m_device->m_draws, before + 1);
	ASSERT_FALSE(m_device->m_verts.empty());
	EXPECT_EQ(m_device->m_verts[0].XYZ.X, kPointX);
	EXPECT_EQ(m_device->m_verts[0].XYZ.Y, kPointY);
}

// Half-pixel-offset mode Off at 2x: DetermineVSConfig's offset for any draw is the plain half
// device pixel. With safe features off the point keeps it, where the corrected figure would take
// a different one.
TEST_F(GSPointSafeFeatures, SafeFeaturesOffKeepsTheModesOffset)
{
	GSConfig.UserHacks_DisableSafeFeatures = true;
	BringUp();

	// A sprite first, to read the offset DetermineVSConfig gives this target.
	{
		Packet p;
		Environment(p, kDstFBP, PSMCT32);
		p.Vertex(0, 0, 1, 0, 0);
		p.Vertex(64 * 16, 64 * 16, 1, 0, 0);
		GIFRegPRIM prim = {};
		prim.PRIM = GS_SPRITE;
		p.Send(*m_gs, prim);
	}
	const GSVector2 sprite_offset = m_device->m_cb_vs.vertex_offset;

	DrawPoint();
	EXPECT_EQ(m_device->m_cb_vs.vertex_offset.x, sprite_offset.x);
	EXPECT_EQ(m_device->m_cb_vs.vertex_offset.y, sprite_offset.y);
}

// At native the correction is the GS's own rule and runs whatever the setting says, as the line
// path's does.
TEST_F(GSPointSafeFeatures, NativeSnapsWhateverTheSetting)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	GSConfig.UserHacks_DisableSafeFeatures = true;
	BringUp();
	DrawPoint();
	ASSERT_FALSE(m_device->m_verts.empty());
	EXPECT_NE(m_device->m_verts[0].XYZ.X, kPointX);
}
