// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that a GS->GS move into an existing render target stays on the GPU when the moved block is
// wider than that target's texture but still inside the destination buffer width.
//
// A game that first moves a small block to some address gets a target sized for that block. When
// it later moves a wider block to the same address, the texture cache has to grow the target. It
// used to grow it only in height, so a wider block made GSTextureCache::Move refuse. The renderer
// then did the move on the CPU, which reads the source target back and waits for the GPU.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/HW/GSTextureCache.h"

using namespace GSHWDrawHarness;

namespace
{
	// A 1280x448 16-bit frame (buffer width 20) and a 128-wide scratch buffer after it.
	constexpr u32 kSceneBP = 0x8c0;
	constexpr u32 kSceneBW = 20;
	constexpr int kSceneW = 1280;
	constexpr int kSceneH = 448;
	constexpr u32 kScratchBP = 0x3fbd;
	constexpr u32 kScratchBW = 2;

	class GSTextureCacheMove : public Fixture
	{
	protected:
		// One untextured sprite over the whole scene, so a target exists at kSceneBP.
		void DrawScene()
		{
			Packet p;
			GIFReg r = {};

			r.U64 = 0;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);

			r.U64 = 0;
			r.SCISSOR.SCAX1 = kSceneW - 1;
			r.SCISSOR.SCAY1 = kSceneH - 1;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);

			r.U64 = 0;
			r.FRAME.FBP = kSceneBP / 32;
			r.FRAME.FBW = kSceneBW;
			r.FRAME.PSM = PSMCT16;
			p.Reg(GIF_A_D_REG_FRAME_1, r);

			r.U64 = 0;
			r.ZBUF.ZBP = 0x100;
			r.ZBUF.PSM = PSMZ16;
			r.ZBUF.ZMSK = 1;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);

			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			p.Reg(GIF_A_D_REG_TEST_1, r);

			r.U64 = 0;
			r.PRMODECONT.AC = 1;
			p.Reg(GIF_A_D_REG_PRMODECONT, r);

			r.U64 = 0;
			r.RGBAQ.R = 0x40;
			r.RGBAQ.G = 0x60;
			r.RGBAQ.B = 0x20;
			r.RGBAQ.A = 0x80;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);

			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(kSceneW << 4, kSceneH << 4, 0, 0, 0);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		bool MoveFromScene(int sx, int sy, int w, int h)
		{
			return g_texture_cache->Move(kSceneBP, kSceneBW, PSMCT16, sx, sy, kScratchBP, kScratchBW, PSMCT16, 0, 0, w, h);
		}

		GSTextureCache::Target* Scratch()
		{
			return g_texture_cache->GetExactTarget(kScratchBP, kScratchBW, GSTextureCache::RenderTarget, kScratchBP);
		}

		void WiderMoveStaysOnTheGPU()
		{
			BringUp();
			DrawScene();
			ASSERT_NE(g_texture_cache->GetExactTarget(kSceneBP, kSceneBW, GSTextureCache::RenderTarget, kSceneBP), nullptr);

			// The first move creates the scratch target, sized for a 40x20 block.
			ASSERT_TRUE(MoveFromScene(1170, 412, 40, 20));
			GSTextureCache::Target* const scratch = Scratch();
			ASSERT_NE(scratch, nullptr);
			const float scale = scratch->m_scale;
			ASSERT_LT(scratch->m_texture->GetWidth(), static_cast<int>(68 * scale));

			// 68 pixels is wider than that texture but inside the buffer width of 128.
			EXPECT_TRUE(MoveFromScene(1169, 412, 68, 34));

			GSTextureCache::Target* const grown = Scratch();
			ASSERT_NE(grown, nullptr);
			EXPECT_GE(grown->m_texture->GetWidth(), static_cast<int>(68 * scale));
			EXPECT_GE(grown->m_texture->GetHeight(), static_cast<int>(34 * scale));
		}
	};

	TEST_F(GSTextureCacheMove, WiderBlockGrowsTheTargetAtNativeScale)
	{
		GSConfig.UpscaleMultiplier = 1.0f;
		WiderMoveStaysOnTheGPU();
	}

	TEST_F(GSTextureCacheMove, WiderBlockGrowsTheTargetUpscaled)
	{
		GSConfig.UpscaleMultiplier = 2.0f;
		WiderMoveStaysOnTheGPU();
	}

	// The move must not grow a target past its buffer width: that block belongs to other memory.
	TEST_F(GSTextureCacheMove, BlockWiderThanTheBufferIsRefused)
	{
		BringUp();
		DrawScene();
		ASSERT_TRUE(MoveFromScene(1170, 412, 40, 20));
		GSTextureCache::Target* const scratch = Scratch();
		ASSERT_NE(scratch, nullptr);
		const int width_before = scratch->m_texture->GetWidth();

		EXPECT_FALSE(MoveFromScene(1000, 100, 160, 34));
		ASSERT_EQ(Scratch(), scratch);
		EXPECT_EQ(scratch->m_texture->GetWidth(), width_before);
	}
} // namespace
