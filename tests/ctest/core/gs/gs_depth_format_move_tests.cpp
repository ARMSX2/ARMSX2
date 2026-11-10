// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A PSMZ24 local-to-local move of colour pages into a depth buffer (GSDepthFormatMove.h).
//
// Two layers. The first pins, on the CPU against the transfer engine (GSLocalMemory::Move), what such a
// move leaves in local memory when it moves whole pages: each depth pixel of the destination holds the
// low 24 bits of a colour pixel of the source, found by a fixed permutation of 8x8 blocks inside the
// page, and keeps its own top byte. A move that is not of whole pages does not have that shape, so it
// is refused. The second pins what the texture cache does with Elemental Gelade's move, a colour target
// at the source and a depth target at the destination: it makes the move on the GPU instead of
// declining it, which sent it to the CPU behind a synchronous readback of the colour target.

#include "gs_hw_draw_harness.h"

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSDepthFormatMove.h"
#include "GS/Renderers/HW/GSTextureCache.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <random>
#include <set>

using namespace GSHWDrawHarness;

namespace
{
	struct Geometry
	{
		u32 sbp, sbw;
		int sx, sy;
		u32 dbp, dbw;
		int dx, dy;
		int w, h;
	};

	class DepthFormatMoveMemory : public ::testing::Test
	{
	protected:
		static void SetUpTestSuite()
		{
			s_mem = new GSLocalMemory();
			s_before = new GSLocalMemory();
		}

		static void TearDownTestSuite()
		{
			delete s_before;
			delete s_mem;
			s_before = nullptr;
			s_mem = nullptr;
		}

		// Random memory, the PSMZ24 move, then every destination pixel read in the depth layout against
		// the colour pixel the block permutation names, read before the move. Returns the mismatches.
		static int MoveAndCheck(const Geometry& g, u32 seed)
		{
			std::mt19937 rng(seed);
			u32* const words = reinterpret_cast<u32*>(s_mem->m_vm8);
			for (size_t i = 0; i < GSLocalMemory::m_vmsize / sizeof(u32); i++)
				words[i] = rng();
			std::memcpy(s_before->m_vm8, s_mem->m_vm8, GSLocalMemory::m_vmsize);

			GIFRegBITBLTBUF b = {};
			b.SBP = g.sbp;
			b.SBW = g.sbw;
			b.SPSM = PSMZ24;
			b.DBP = g.dbp;
			b.DBW = g.dbw;
			b.DPSM = PSMZ24;
			GIFRegTRXPOS p = {};
			p.SSAX = g.sx;
			p.SSAY = g.sy;
			p.DSAX = g.dx;
			p.DSAY = g.dy;
			GIFRegTRXREG r = {};
			r.RRW = g.w;
			r.RRH = g.h;
			s_mem->Move(b, p, r);

			int bad = 0;
			for (int y = 0; y < g.h; y++)
			{
				for (int x = 0; x < g.w; x++)
				{
					const int px = x % GSDepthFormatMove::kPageW;
					const int py = y % GSDepthFormatMove::kPageH;
					const GSVector2i cb = GSDepthFormatMove::ColourBlockForDepthBlock(px / GSDepthFormatMove::kBlockW, py / GSDepthFormatMove::kBlockH);
					const int cx = (x - px) + cb.x * GSDepthFormatMove::kBlockW + px % GSDepthFormatMove::kBlockW;
					const int cy = (y - py) + cb.y * GSDepthFormatMove::kBlockH + py % GSDepthFormatMove::kBlockH;
					const u32 colour = s_before->ReadPixel32(g.sx + cx, g.sy + cy, g.sbp, g.sbw);
					const u32 kept = s_before->ReadPixel32Z(g.dx + x, g.dy + y, g.dbp, g.dbw) & 0xFF000000u;
					const u32 got = s_mem->ReadPixel32Z(g.dx + x, g.dy + y, g.dbp, g.dbw);
					bad += got != (kept | (colour & 0x00FFFFFFu));
				}
			}
			return bad;
		}

		static inline GSLocalMemory* s_mem = nullptr;
		static inline GSLocalMemory* s_before = nullptr;
	};

	TEST_F(DepthFormatMoveMemory, TheBlockPermutationCoversThePage)
	{
		std::set<std::pair<int, int>> seen;
		for (int by = 0; by < GSDepthFormatMove::kBlocksY; by++)
		{
			for (int bx = 0; bx < GSDepthFormatMove::kBlocksX; bx++)
			{
				const GSVector2i c = GSDepthFormatMove::ColourBlockForDepthBlock(bx, by);
				ASSERT_GE(c.x, 0);
				ASSERT_GE(c.y, 0);
				seen.insert({c.x, c.y});
			}
		}
		EXPECT_EQ(seen.size(), static_cast<size_t>(GSDepthFormatMove::kBlocksX * GSDepthFormatMove::kBlocksY));
	}

	// Elemental Gelade's move: the 640x224 frame at 0x0 to 0x8c0, and on the next frame to 0x1180.
	TEST_F(DepthFormatMoveMemory, TheGameMoveIsABlockPermutation)
	{
		for (const u32 dbp : {0x8c0u, 0x1180u})
		{
			const Geometry g = {0x0, 10, 0, 0, dbp, 10, 0, 0, 640, 224};
			EXPECT_TRUE(GSDepthFormatMove::WholePages(g.sbp, g.sbw, g.sx, g.sy, g.dbp, g.dbw, g.dx, g.dy, g.w, g.h));
			EXPECT_EQ(MoveAndCheck(g, dbp), 0);
		}
	}

	// Whole pages anywhere: bases, page offsets and buffer widths drawn at random.
	TEST_F(DepthFormatMoveMemory, EveryWholePageMoveIsABlockPermutation)
	{
		std::mt19937 rng(20261109);
		int checked = 0;
		for (int i = 0; i < 400 && checked < 24; i++)
		{
			Geometry g;
			g.sbw = 1 + rng() % 12;
			g.dbw = 1 + rng() % 12;
			g.sbp = (rng() % 256) * 32;
			g.dbp = (rng() % 256) * 32;
			g.sx = static_cast<int>(rng() % 4) * 64;
			g.sy = static_cast<int>(rng() % 4) * 32;
			g.dx = static_cast<int>(rng() % 4) * 64;
			g.dy = static_cast<int>(rng() % 4) * 32;
			g.w = static_cast<int>(1 + rng() % 4) * 64;
			g.h = static_cast<int>(1 + rng() % 3) * 32;
			if (!GSDepthFormatMove::WholePages(g.sbp, g.sbw, g.sx, g.sy, g.dbp, g.dbw, g.dx, g.dy, g.w, g.h))
				continue; // the two regions share a page
			EXPECT_EQ(MoveAndCheck(g, rng()), 0) << "sbp " << g.sbp << " sbw " << g.sbw << " s " << g.sx << "," << g.sy << " dbp " << g.dbp
												 << " dbw " << g.dbw << " d " << g.dx << "," << g.dy << " " << g.w << "x" << g.h;
			checked++;
		}
		EXPECT_EQ(checked, 24);
	}

	// A source that does not start on a page edge does not have that shape, and is refused. (The shape
	// does not depend on where the destination starts, since both ends are read in the depth layout;
	// the texture cache asks for whole pages at both ends all the same.)
	TEST_F(DepthFormatMoveMemory, APartPageMoveIsNotABlockPermutation)
	{
		const Geometry shifted = {0x0, 10, 8, 0, 0x8c0, 10, 0, 0, 576, 224};
		EXPECT_FALSE(GSDepthFormatMove::WholePages(shifted.sbp, shifted.sbw, shifted.sx, shifted.sy, shifted.dbp, shifted.dbw, shifted.dx,
			shifted.dy, shifted.w, shifted.h));
		EXPECT_GT(MoveAndCheck(shifted, 2), 0);

		EXPECT_FALSE(GSDepthFormatMove::WholePages(0x0, 10, 0, 0, 0x8c0, 10, 8, 0, 576, 224));
		EXPECT_FALSE(GSDepthFormatMove::WholePages(0x0, 10, 0, 0, 0x8c0, 10, 0, 0, 640, 200));
		EXPECT_FALSE(GSDepthFormatMove::WholePages(0x10, 10, 0, 0, 0x8c0, 10, 0, 0, 640, 224));
		// The destination's first page row is the source's last.
		EXPECT_FALSE(GSDepthFormatMove::WholePages(0x0, 10, 0, 0, 0x780, 10, 0, 0, 640, 224));
	}

	// The texture cache: a colour target at the source and a depth target at the destination.
	class DepthFormatMoveCache : public Fixture
	{
	protected:
		static constexpr u32 kFrameBP = 0x0;
		static constexpr u32 kDepthBP = 0x8c0;
		static constexpr u32 kOtherFrameBP = 0x2300;
		static constexpr u32 kBW = 10;
		static constexpr int kW = 640;
		static constexpr int kH = 224;

		// One untextured sprite over a 640x224 PSMCT32 buffer at `frame_bp`; with `depth_bp` set it also
		// writes Z24 there, so a depth target exists at that address.
		void Draw(u32 frame_bp, u32 depth_bp, bool write_depth)
		{
			Packet p;
			GIFReg r = {};

			r.U64 = 0;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);

			r.U64 = 0;
			r.SCISSOR.SCAX1 = kW - 1;
			r.SCISSOR.SCAY1 = kH - 1;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);

			r.U64 = 0;
			r.FRAME.FBP = frame_bp / 32;
			r.FRAME.FBW = kBW;
			r.FRAME.PSM = PSMCT32;
			p.Reg(GIF_A_D_REG_FRAME_1, r);

			r.U64 = 0;
			r.ZBUF.ZBP = depth_bp / 32;
			r.ZBUF.PSM = PSMZ24 & 0xf;
			r.ZBUF.ZMSK = write_depth ? 0 : 1;
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

			p.Vertex(0, 0, 0x1234, 0, 0);
			p.Vertex(kW << 4, kH << 4, 0x1234, 0, 0);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		bool GameMove(int w = kW, int h = kH)
		{
			return g_texture_cache->Move(kFrameBP, kBW, PSMZ24, 0, 0, kDepthBP, kBW, PSMZ24, 0, 0, w, h);
		}

		GSTextureCache::Target* DepthAt(u32 bp)
		{
			return g_texture_cache->GetExactTarget(bp, kBW, GSTextureCache::DepthStencil, bp);
		}

		void TheGameMoveStaysOnTheGPU()
		{
			BringUp();
			Draw(kOtherFrameBP, kDepthBP, true);
			Draw(kFrameBP, 0x3000, false);
			ASSERT_NE(g_texture_cache->GetExactTarget(kFrameBP, kBW, GSTextureCache::RenderTarget, kFrameBP), nullptr);
			GSTextureCache::Target* const depth = DepthAt(kDepthBP);
			ASSERT_NE(depth, nullptr);
			depth->m_drawn_since_read = GSVector4i::zero();

			EXPECT_TRUE(GameMove());

			// The depth target now holds data local memory does not have: a later read of it has to come
			// from the target.
			EXPECT_EQ(DepthAt(kDepthBP), depth);
			EXPECT_TRUE(depth->m_drawn_since_read.rintersect(GSVector4i(0, 0, kW, kH)).eq(GSVector4i(0, 0, kW, kH)));
		}
	};

	TEST_F(DepthFormatMoveCache, TheGameMoveStaysOnTheGPUAtNativeScale)
	{
		GSConfig.UpscaleMultiplier = 1.0f;
		TheGameMoveStaysOnTheGPU();
	}

	TEST_F(DepthFormatMoveCache, TheGameMoveStaysOnTheGPUUpscaled)
	{
		GSConfig.UpscaleMultiplier = 2.0f;
		TheGameMoveStaysOnTheGPU();
	}

	// Without a depth target at the destination there is nothing on the GPU the CPU move would update:
	// the move is left to the CPU.
	TEST_F(DepthFormatMoveCache, NoDestinationTargetIsLeftToTheCPU)
	{
		BringUp();
		Draw(kFrameBP, 0x3000, false);
		EXPECT_FALSE(GameMove());
	}

	TEST_F(DepthFormatMoveCache, APartPageMoveIsLeftToTheCPU)
	{
		BringUp();
		Draw(kOtherFrameBP, kDepthBP, true);
		Draw(kFrameBP, 0x3000, false);
		EXPECT_FALSE(GameMove(kW, 200));
	}
} // namespace
