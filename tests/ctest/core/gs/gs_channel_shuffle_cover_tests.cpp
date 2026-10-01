// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that the upscaled sprite coverage correction leaves a channel shuffle's rebuilt quad alone.
//
// A channel shuffle replaces the game's sprites with one quad of its own over the whole target
// (or the pages it touches), on whole native pixels and whole texels. The correction --
// AlignSpriteX, the pixel-grid snap and RoundSprite -- exists for sprites a game placed; it used to
// run before the rebuild, which then discarded it. Moved after the rebuilds, it met the rebuilt
// quad. The snap and AlignSpriteX find nothing to move on whole pixels, and RoundSprite leaves a
// nearest-sampled quad alone, but under bilinear sampling RoundSprite pulled the quad's far texel
// coordinate in by half a texel -- 16376 instead of 16384 -- so the whole shuffle read its source
// half a texel short. The texture-shuffle rebuild was already gated off; the channel-shuffle one
// was not.

#include "gs_hw_draw_harness.h"

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kSrcFBP = 0x00;
	constexpr u32 kDstFBP = 0x80;

	class GSChannelShuffleCover : public Fixture
	{
	protected:
		bool m_linear = false;
		void SeedTarget(u32 fbp)
		{
			Packet p;
			Environment(p, fbp, PSMCT32);
			p.Vertex(0, 0, 1, 0, 0);
			p.Vertex(640 * 16, 448 * 16, 1, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		// Metal Gear Solid 3's blue/alpha shape: an 8-bit read of a 32-bit target, region-repeat
		// clamped so every texel lands in one channel's byte column, drawn as one 16x16 sprite.
		void DrawChannelShuffle()
		{
			Packet p;
			Environment(p, kDstFBP, PSMCT32);

			GIFReg r = {};
			r.U64 = 0;
			r.TEX0.TBP0 = kSrcFBP * 32;
			r.TEX0.TBW = 20;
			r.TEX0.PSM = PSMT8;
			r.TEX0.TW = 10;
			r.TEX0.TH = 9;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
			p.Reg(GIF_A_D_REG_TEX0_1, r);

			r.U64 = 0;
			r.TEX1.MMAG = m_linear ? 1 : 0;
			r.TEX1.MMIN = m_linear ? 1 : 0;
			p.Reg(GIF_A_D_REG_TEX1_1, r);

			r.U64 = 0;
			r.CLAMP.WMS = 3;
			r.CLAMP.WMT = 3;
			r.CLAMP.MINU = 0x3F7;
			r.CLAMP.MAXU = 0x8;
			r.CLAMP.MINV = 0x3FD;
			r.CLAMP.MAXV = 0x0;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);

			p.Vertex(0, 0, 1, 8, 8);
			p.Vertex(16 * 16, 16 * 16, 1, 32 * 16 + 8, 16 * 16 + 8);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 1;
			p.Send(*m_gs, prim);
		}
	};
} // namespace

TEST_F(GSChannelShuffleCover, TheRebuiltQuadReachesTheBackendUnrounded)
{
	GSConfig.UserHacks_RoundSprite = 2; // on for every sprite, linear filtering or not
	GSConfig.UserHacks_AlignSpriteX = true;
	m_linear = true;
	GSConfig.UserHacks_TextureInsideRt = GSTextureInRtMode::Disabled; // the whole-target rebuild
	BringUp();
	SeedTarget(kSrcFBP);
	SeedTarget(kDstFBP);

	const u32 before = m_device->m_draws;
	DrawChannelShuffle();
	ASSERT_GT(m_device->m_draws, before) << "the draw never reached the backend";
	ASSERT_NE(m_device->m_ps.channel, 0u) << "not taken as a channel shuffle, so it tests nothing";
	ASSERT_EQ(m_device->m_verts.size(), 4u) << "the rebuild should leave one quad";

	// The rebuild's quad: native 0..1024 on both axes and the same in texels.
	const GSVertex& near_corner = m_device->m_verts[0];
	const GSVertex& far_corner = m_device->m_verts[3];
	EXPECT_EQ(near_corner.XYZ.X, 0);
	EXPECT_EQ(near_corner.XYZ.Y, 0);
	EXPECT_EQ(far_corner.XYZ.X, 16384);
	EXPECT_EQ(far_corner.XYZ.Y, 16384);
	EXPECT_EQ(near_corner.U, 0);
	EXPECT_EQ(near_corner.V, 0);
	EXPECT_EQ(far_corner.U, 16384) << "RoundSprite must not run on a channel shuffle's quad";
	EXPECT_EQ(far_corner.V, 16384) << "RoundSprite must not run on a channel shuffle's quad";
}

