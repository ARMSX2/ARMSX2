// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The per-rectangle alpha known bits through the renderer on the None backend: which draws may
// establish a rectangle, which writes must reach the ones already there, and that a masked draw
// inside one drops its mask only when every pixel under it is known.
//
// Every case first leaves alpha bit 0 unknown for the target as a whole, so the only thing that
// can drop a 0x01000000 mask is a rectangle. The observable is the shader's masked-write flag on
// the masked draw: on means the target is read, off means the drop was taken. Every alpha is above
// 128, so no target is alpha-scaled, which would make it ineligible for the drop.

#include "gs_hw_draw_harness.h"

#include <initializer_list>

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kFBP = 0x40;
	constexpr u32 kHeldBit0 = 0x01000000;

	/// A sprite in 1/16 pixel units.
	struct Box
	{
		int x0, y0, x1, y1;
	};

	constexpr Box Px(int x0, int y0, int x1, int y1)
	{
		return {x0 * 16, y0 * 16, x1 * 16, y1 * 16};
	}

	class GSAlphaKnownRectsDraw : public Fixture
	{
	protected:
		/// Untextured sprites, one flat alpha, on a 32-bit frame at kFBP.
		void Sprites(std::initializer_list<Box> boxes, u8 alpha, u32 fbmsk = 0, u32 scanmsk = 0)
		{
			Packet p;
			Environment(p, kFBP, PSMCT32);

			GIFReg r = {};
			r.FRAME.FBP = kFBP;
			r.FRAME.FBW = 10;
			r.FRAME.PSM = PSMCT32;
			r.FRAME.FBMSK = fbmsk;
			p.Reg(GIF_A_D_REG_FRAME_1, r);

			r.U64 = 0;
			r.SCANMSK.MSK = scanmsk;
			p.Reg(GIF_A_D_REG_SCANMSK, r);

			r.U64 = 0;
			r.RGBAQ.R = 0x40;
			r.RGBAQ.A = alpha;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);

			for (const Box& b : boxes)
			{
				p.Vertex(b.x0, b.y0, 0, 0, 0);
				p.Vertex(b.x1, b.y1, 0, 0, 0);
			}

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		/// Leaves alpha bit 0 unknown for the target as a whole: two strips at the top that
		/// disagree on it, neither covering the valid rect.
		void Prime()
		{
			Sprites({Px(0, 0, 640, 8)}, 0xA1);
			Sprites({Px(0, 8, 640, 16)}, 0xA0);
		}

		/// Whether a masked draw over `b` (alpha 0xA0, bit 0 held back) still reads the target.
		bool MaskedDrawReads(const Box& b)
		{
			const u32 before = m_device->m_draws;
			Sprites({b}, 0xA0, kHeldBit0);
			EXPECT_EQ(m_device->m_draws, before + 1);
			return m_device->m_ps.fbmask != 0;
		}
	};
} // namespace

// The shape the rectangles are for: a clear at an offset, then masked draws inside it.
TEST_F(GSAlphaKnownRectsDraw, AnOffsetClearLetsAMaskedDrawInsideDrop)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	Prime();
	Sprites({Px(0, 32, 640, 415)}, 0xA0);
	EXPECT_FALSE(MaskedDrawReads(Px(100, 100, 200, 200)));
}

TEST_F(GSAlphaKnownRectsDraw, AnOffsetClearLetsAMaskedDrawInsideDropUpscaled)
{
	BringUp();
	Prime();
	Sprites({Px(0, 32, 640, 415)}, 0xA0);
	EXPECT_FALSE(MaskedDrawReads(Px(100, 100, 200, 200)));
}

// The tiling test passes these two sprites (the horizontal match checks only the first one's
// width), but the second covers eight columns of its row band. Nobody wrote (100..200, 252..258).
TEST_F(GSAlphaKnownRectsDraw, SpritesTheTilingTestPassesWithAGapEstablishNothing)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	Prime();
	Sprites({Px(0, 240, 640, 250), Px(0, 250, 8, 260)}, 0xA0);
	EXPECT_TRUE(MaskedDrawReads(Px(100, 252, 200, 258)));
}

// SCANMSK 2 discards every even row, so the clear writes half its rect.
TEST_F(GSAlphaKnownRectsDraw, AScanMaskedClearEstablishesNothing)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	Prime();
	Sprites({Px(0, 32, 640, 415)}, 0xA0, 0, 2);
	EXPECT_TRUE(MaskedDrawReads(Px(100, 100, 200, 200)));
}

// A far edge at 300.25: the draw rect takes row 300, the renderer's rounding does not write it.
TEST_F(GSAlphaKnownRectsDraw, AFractionalEdgeEstablishesNothing)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	Prime();
	Sprites({Box{0, 32 * 16, 640 * 16, 300 * 16 + 4}}, 0xA0);
	EXPECT_TRUE(MaskedDrawReads(Px(100, 290, 200, 301)));
}

// Upscaled, AlignSpriteX moves every far X in a batch out by half a native pixel when the first
// sprite ends on a half pixel. The second sprite here ends at 320, so it writes half of native
// column 320, outside its draw rect and inside the rectangle the first clear established. Its
// alpha disagrees on bit 0, so nothing may claim bit 0 there any more.
TEST_F(GSAlphaKnownRectsDraw, AlignSpriteXWritesPastTheDrawRect)
{
	GSConfig.UserHacks_AlignSpriteX = true;
	BringUp();
	Prime();
	Sprites({Px(320, 0, 640, 415)}, 0xA0);
	Sprites({Box{0, 32 * 16, 100 * 16 + 8, 100 * 16}, Px(0, 100, 320, 415)}, 0xA1);
	EXPECT_TRUE(MaskedDrawReads(Px(320, 200, 400, 300)));
}
