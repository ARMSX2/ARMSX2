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
#include "GS/Renderers/HW/GSBurstMemo.h"
#include "GS/Renderers/HW/GSDepthFormatMove.h"
#include "GS/Renderers/HW/GSTextureCache.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>

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

			// The depth target now holds data local memory does not have: the moved words, held apart
			// until something reads or writes local memory there (DepthFormatMoveHeld below).
			EXPECT_EQ(DepthAt(kDepthBP), depth);
			const GSTextureCache::HeldWords* const held = g_texture_cache->HeldBy(depth);
			ASSERT_NE(held, nullptr);
			EXPECT_NE(held->words, nullptr);
			EXPECT_TRUE(held->rect.eq(GSVector4i(0, 0, kW, kH)));
			EXPECT_TRUE(g_texture_cache->HasHeldPages());
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

	// Held pages. After the GPU move local memory under the destination is out of date; the depth target holds
	// the words the CPU move would have written there and every path that reads or writes local memory over
	// those pages writes them in first. Each test below is one such path. The device tags what it downloads by
	// the texture it downloads from: the held words read kHeld, anything else (a readback of a target) kOther,
	// so local memory shows which one a reader got.
	constexpr u32 kHeld = 0x00A1B2C3u;
	constexpr u32 kOther = 0x00E0E0E0u;
	constexpr u32 kBefore = 0x5A111111u; // local memory under the destination before the move: top byte kept

	class TagDownload final : public GSDownloadTexture
	{
	public:
		TagDownload(u32 width, u32 height, GSTexture::Format format, const std::unordered_map<const GSTexture*, u32>* tags)
			: GSDownloadTexture(width, height, format)
			, m_tags(tags)
		{
			m_buffer.resize(GetBufferSize(width, height, format));
		}

		void DoCopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch) override
		{
			m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, 1);
			const auto it = m_tags->find(stex);
			const u32 v = (it != m_tags->end()) ? it->second : kOther;
			u32* const words = reinterpret_cast<u32*>(m_buffer.data());
			std::fill(words, words + m_buffer.size() / sizeof(u32), v);
			m_needs_flush = false;
		}
		bool Map(const GSVector4i& read_rc) override
		{
			m_map_pointer = m_buffer.data();
			return true;
		}
		void Unmap() override { m_map_pointer = nullptr; }
		void Flush() override {}
#ifdef PCSX2_DEVBUILD
		void SetDebugName(std::string_view name) override
		{
		}
#endif

	private:
		const std::unordered_map<const GSTexture*, u32>* m_tags;
		std::vector<u8> m_buffer;
	};

	class TagDevice final : public CaptureDevice
	{
	public:
		std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override
		{
			return std::make_unique<TagDownload>(width, height, format, &m_tags);
		}
		std::unordered_map<const GSTexture*, u32> m_tags;
	};

	class DepthFormatMoveHeld : public DepthFormatMoveCache, public ::testing::WithParamInterface<float>
	{
	protected:
		std::unique_ptr<CaptureDevice> NewDevice() override { return std::make_unique<TagDevice>(); }

		void TearDown() override
		{
			GSBurstMemo::SetForcedOff(false); // the copy-road tests switch it off
			DepthFormatMoveCache::TearDown();
		}

		TagDevice* Tags() { return static_cast<TagDevice*>(m_device); }

		/// Whether InvalidateVideoMem takes its early return for a write of `rect` at `bp`: no source under it.
		bool NoSourceUnder(u32 bp, u32 psm, const GSVector4i& rect)
		{
			const GSOffset off = m_gs->m_mem.GetOffset(bp, kBW, psm);
			return g_texture_cache->NoSourceUnderWrite(off, rect, GSLocalMemory::GetStartBlockAddress(bp, kBW, psm, rect),
				GSLocalMemory::GetUnwrappedEndBlockAddress(bp, kBW, psm, rect));
		}

		// The game move, at the scale the test runs at, with local memory under the destination set to
		// kBefore first. Returns the depth target, which holds the move.
		GSTextureCache::Target* Hold()
		{
			GSConfig.UpscaleMultiplier = GetParam();
			GSConfig.HWDownloadMode = GSHardwareDownloadMode::Enabled;
			BringUp();
			Draw(kOtherFrameBP, kDepthBP, true);
			Draw(kFrameBP, 0x3000, false);
			GSTextureCache::Target* const depth = DepthAt(kDepthBP);
			if (!depth)
				return nullptr;
			depth->m_drawn_since_read = GSVector4i::zero();
			for (int y = 0; y < kH; y++)
			{
				for (int x = 0; x < kW; x++)
					m_gs->m_mem.WritePixel32Z(x, y, kBefore, kDepthBP, kBW);
			}
			if (!GameMove() || !g_texture_cache->HeldBy(depth))
				return nullptr;
			Tags()->m_tags[g_texture_cache->HeldBy(depth)->words] = kHeld;
			return depth;
		}

		// Pixels of the destination, in the depth layout, whose low 24 bits are not `want` inside `inside`
		// and not `outside_want` elsewhere; the top byte must be kBefore's throughout.
		int Mismatches(u32 want, const GSVector4i& inside = GSVector4i(0, 0, kW, kH), u32 outside_want = kHeld)
		{
			int bad = 0;
			for (int y = 0; y < kH; y++)
			{
				for (int x = 0; x < kW; x++)
				{
					const u32 got = m_gs->m_mem.ReadPixel32Z(x, y, kDepthBP, kBW);
					const bool in = x >= inside.x && x < inside.z && y >= inside.y && y < inside.w;
					const u32 expect = (kBefore & 0xFF000000u) | (in ? want : outside_want);
					bad += got != expect;
				}
			}
			return bad;
		}

		bool Untouched() { return Mismatches(kBefore & 0x00FFFFFFu, GSVector4i(0, 0, kW, kH)) == 0; }

		u64 Materialised() const { return g_texture_cache->GetHeldStats().materialised; }

		// A point-sampled textured sprite copying (0,0)-(w,h) of a texture at `tbp` into a frame at `fbp`
		// (width 10, PSMCT32, no depth). A paletted texture loads its palette from `cbp`.
		void DrawTextured(u32 tbp, u32 tpsm, u32 fbp, int w, int h, u32 cbp = 0x3800)
		{
			Packet p;
			Environment(p, fbp / 32, PSMCT32);
			GIFReg r = {};
			r.U64 = 0;
			r.TEX0.TBP0 = tbp;
			r.TEX0.TBW = kBW;
			r.TEX0.PSM = tpsm;
			r.TEX0.TW = 10;
			r.TEX0.TH = 8;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
			if (GSLocalMemory::m_psm[tpsm].pal > 0)
			{
				r.TEX0.CBP = cbp;
				r.TEX0.CPSM = PSMCT32;
				r.TEX0.CLD = 1;
			}
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			r.U64 = 0;
			p.Reg(GIF_A_D_REG_TEX1_1, r);
			r.U64 = 0;
			r.CLAMP.WMS = CLAMP_CLAMP;
			r.CLAMP.WMT = CLAMP_CLAMP;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(w << 4, h << 4, 0, w << 4, h << 4);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 1;
			p.Send(*m_gs, prim);
		}

		// An untextured sprite over (x0,y0)-(x1,y1) writing depth into the held depth target.
		void DrawDepth(int x0, int y0, int x1, int y1)
		{
			Packet p;
			GIFReg r = {};
			r.U64 = 0;
			r.FRAME.FBP = kOtherFrameBP / 32;
			r.FRAME.FBW = kBW;
			r.FRAME.PSM = PSMCT32;
			p.Reg(GIF_A_D_REG_FRAME_1, r);
			r.U64 = 0;
			r.ZBUF.ZBP = kDepthBP / 32;
			r.ZBUF.PSM = PSMZ24 & 0xf;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);
			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			p.Reg(GIF_A_D_REG_TEST_1, r);
			p.Vertex(x0 << 4, y0 << 4, 0x4321, 0, 0);
			p.Vertex(x1 << 4, y1 << 4, 0x4321, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		// An untextured sprite into a colour frame at `fbp` in `psm`, no depth.
		void DrawColour(u32 fbp, u32 psm, int w, int h)
		{
			Packet p;
			Environment(p, fbp / 32, psm);
			GIFReg r = {};
			r.U64 = 0;
			r.FRAME.FBP = fbp / 32;
			r.FRAME.FBW = kBW;
			r.FRAME.PSM = psm;
			p.Reg(GIF_A_D_REG_FRAME_1, r);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(w << 4, h << 4, 0, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		void SetTransfer(u32 dir, u32 bp, u32 psm, int x, int y, int w, int h)
		{
			Packet p;
			GIFReg r = {};
			r.U64 = 0;
			if (dir == 1)
			{
				r.BITBLTBUF.SBP = bp;
				r.BITBLTBUF.SBW = kBW;
				r.BITBLTBUF.SPSM = psm;
			}
			else
			{
				r.BITBLTBUF.DBP = bp;
				r.BITBLTBUF.DBW = kBW;
				r.BITBLTBUF.DPSM = psm;
			}
			p.Reg(GIF_A_D_REG_BITBLTBUF, r);
			r.U64 = 0;
			if (dir == 1)
			{
				r.TRXPOS.SSAX = x;
				r.TRXPOS.SSAY = y;
			}
			else
			{
				r.TRXPOS.DSAX = x;
				r.TRXPOS.DSAY = y;
			}
			p.Reg(GIF_A_D_REG_TRXPOS, r);
			r.U64 = 0;
			r.TRXREG.RRW = w;
			r.TRXREG.RRH = h;
			p.Reg(GIF_A_D_REG_TRXREG, r);
			r.U64 = 0;
			r.TRXDIR.XDIR = dir;
			p.Reg(GIF_A_D_REG_TRXDIR, r);
			p.Send(*m_gs, GIFRegPRIM{});
		}
	};

	// The move leaves local memory as it was and holds the destination's pages.
	TEST_P(DepthFormatMoveHeld, TheMoveHoldsThePages)
	{
		GSTextureCache::Target* const depth = Hold();
		ASSERT_NE(depth, nullptr);
		EXPECT_TRUE(Untouched());
		EXPECT_TRUE(depth->m_drawn_since_read.rempty());
		EXPECT_EQ(g_texture_cache->GetHeldStats().held, 1u);
		EXPECT_EQ(Materialised(), 0u);
	}

	// An EE read (local-to-host transfer) of part of the pages: the bytes handed to the EE are the moved words.
	TEST_P(DepthFormatMoveHeld, AnEEReadGetsTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		SetTransfer(1, kDepthBP, PSMZ32, 64, 32, 64, 32);
		std::vector<u32> fifo(64 * 32);
		const int qwc = static_cast<int>(fifo.size() * sizeof(u32) / 16);
		m_gs->InitReadFIFO(reinterpret_cast<u8*>(fifo.data()), qwc);
		m_gs->ReadFIFO(reinterpret_cast<u8*>(fifo.data()), qwc);
		int bad = 0;
		for (const u32 w : fifo)
			bad += w != ((kBefore & 0xFF000000u) | kHeld);
		EXPECT_EQ(bad, 0);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_FALSE(g_texture_cache->HasHeldPages());
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// A savestate, which a GS dump and a renderer switch (GSreopen) also take: local memory as serialised
	// holds the moved words, whatever UserHacks_ReadTCOnClose says.
	TEST_P(DepthFormatMoveHeld, ASavestateGetsTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		GSConfig.UserHacks_ReadTCOnClose = false;
		freezeData fd = {};
		ASSERT_EQ(m_gs->Freeze(&fd, true), 0);
		std::vector<u8> data(fd.size);
		fd.data = data.data();
		ASSERT_EQ(m_gs->Freeze(&fd, false), 0);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// ReadbackTextureCache: a GS dump's readback, a reset, a renderer switch under UserHacks_ReadTCOnClose.
	TEST_P(DepthFormatMoveHeld, ReadingBackTheCacheGetsTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		m_gs->ReadbackTextureCache();
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// A draw into the depth target after the move, then a reader: the words under everything the draw did not
	// reach, the target's own pixels (a readback, kOther) where it drew. The order the CPU move gives too.
	TEST_P(DepthFormatMoveHeld, ADrawAfterTheMoveReadsBackOverTheWords)
	{
		GSTextureCache::Target* const depth = Hold();
		ASSERT_NE(depth, nullptr);
		DrawDepth(0, 0, 128, 64);
		ASSERT_EQ(DepthAt(kDepthBP), depth);
		ASSERT_FALSE(depth->m_drawn_since_read.rempty());

		m_gs->ReadbackTextureCache();
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kOther, GSVector4i(0, 0, 128, 64)), 0);
	}

	// An EE write into part of the pages: the words go in first, the write lands over them.
	TEST_P(DepthFormatMoveHeld, AnEEWriteLandsOverTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		constexpr u32 kWritten = 0x00123456u;
		SetTransfer(0, kDepthBP, PSMZ24, 64, 32, 64, 32);
		std::vector<u8> bytes(64 * 32 * 3);
		for (size_t i = 0; i < bytes.size(); i += 3)
		{
			bytes[i] = kWritten & 0xFF;
			bytes[i + 1] = (kWritten >> 8) & 0xFF;
			bytes[i + 2] = (kWritten >> 16) & 0xFF;
		}
		m_gs->Write(bytes.data(), static_cast<int>(bytes.size()));
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kWritten, GSVector4i(64, 32, 128, 64)), 0);
	}

	// A texture over the pages in a colour format that the cache serves from the depth target (a young
	// target, 32- or 16-bit): the depth target's texture is what is sampled, as after the CPU move, which
	// leaves the same depth target. Nothing is read from local memory, so nothing is materialised.
	TEST_P(DepthFormatMoveHeld, AColourTextureOfTheDepthTargetTakesTheTarget)
	{
		GSTextureCache::Target* const depth = Hold();
		ASSERT_NE(depth, nullptr);
		DrawTextured(kDepthBP, PSMCT32, 0x3000, 128, 64);
		DrawTextured(kDepthBP, PSMCT16, 0x3000, 128, 64);
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_EQ(DepthAt(kDepthBP), depth);
		EXPECT_NE(g_texture_cache->HeldBy(depth), nullptr);
	}

	// A texture the cache builds from local memory instead: a paletted one, and a colour one over a depth
	// target too old to be taken. The source is read after the words go in.
	TEST_P(DepthFormatMoveHeld, APalettedTextureOverThePagesReadsTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		DrawTextured(kDepthBP, PSMT8, 0x3000, 128, 64);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	TEST_P(DepthFormatMoveHeld, AColourTextureOverAnOldDepthTargetReadsTheWords)
	{
		GSTextureCache::Target* const depth = Hold();
		ASSERT_NE(depth, nullptr);
		depth->m_age = 5;
		depth->m_used = false;
		DrawTextured(kDepthBP, PSMCT32, 0x3000, 128, 64);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// A colour target drawn over the pages replaces the depth target there. The words stay held without it,
	// and a reader then finds them under everything the colour target does not cover, and the colour
	// target's own pixels (read back after them) where it drew: its first page.
	TEST_P(DepthFormatMoveHeld, AColourTargetOverThePagesKeepsTheWordsHeld)
	{
		ASSERT_NE(Hold(), nullptr);
		DrawColour(kDepthBP, PSMCT32, 64, 32);
		EXPECT_EQ(DepthAt(kDepthBP), nullptr);
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_TRUE(g_texture_cache->HasHeldPages());

		m_gs->ReadbackTextureCache();
		EXPECT_EQ(Materialised(), 1u);
		int bad = 0;
		for (int y = 0; y < kH; y++)
		{
			for (int x = 0; x < kW; x++)
			{
				const u32 got = m_gs->m_mem.ReadPixel32Z(x, y, kDepthBP, kBW);
				if (x < 64 && y < 32)
					bad += (got & 0x00FFFFFFu) != kOther;
				else
					bad += got != ((kBefore & 0xFF000000u) | kHeld);
			}
		}
		EXPECT_EQ(bad, 0);
	}

	// A depth buffer of another format over some of the pages, preloaded from local memory: it reads the
	// words (Target::Update).
	TEST_P(DepthFormatMoveHeld, AnotherDepthBufferPreloadedOverThePagesReadsTheWords)
	{
		// Upscaled, Target::Update's draw of a 16-bit depth target passes the bilinear filter with a shader
		// that filters itself, which GSDevice::DoStretchRectWithAssertions rejects; that is Target::Update's,
		// not the hold's, and the hold is materialised before the draw at either scale.
		if (GetParam() != 1.0f)
			GTEST_SKIP();
		GSConfig.PreloadFrameWithGSData = true;
		ASSERT_NE(Hold(), nullptr);
		Packet p;
		Environment(p, 0x3000 / 32, PSMCT32);
		GIFReg r = {};
		r.U64 = 0;
		r.ZBUF.ZBP = (kDepthBP + 32 * kBW * 3) / 32;
		r.ZBUF.PSM = PSMZ16 & 0xf;
		p.Reg(GIF_A_D_REG_ZBUF_1, r);
		r.U64 = 0;
		r.TEST.ZTE = 1;
		r.TEST.ZTST = ZTST_GEQUAL;
		p.Reg(GIF_A_D_REG_TEST_1, r);
		p.Vertex(0, 0, 5, 0, 0);
		p.Vertex(kW << 4, 64 << 4, 5, 0, 0);
		GIFRegPRIM prim = {};
		prim.PRIM = GS_SPRITE;
		p.Send(*m_gs, prim);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// A constant clear the renderer writes straight into local memory: over part of the pages the words go
	// in first and the clear lands over them; over all of them the hold is dropped unread.
	TEST_P(DepthFormatMoveHeld, AClearOfPartOfThePagesLandsOverTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		DrawColour(kDepthBP + 32 * kBW * 3, PSMCT32, kW, 64); // page rows 3 and 4
		EXPECT_EQ(Materialised(), 1u);
		int bad = 0;
		for (int y = 0; y < kH; y++)
		{
			for (int x = 0; x < kW; x++)
			{
				const u32 got = m_gs->m_mem.ReadPixel32Z(x, y, kDepthBP, kBW);
				const bool cleared = y >= 96 && y < 160;
				bad += cleared ? (got != 0x80206040u) : (got != ((kBefore & 0xFF000000u) | kHeld));
			}
		}
		EXPECT_EQ(bad, 0);
	}

	TEST_P(DepthFormatMoveHeld, AClearOfAllThePagesDropsTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		DrawColour(kDepthBP, PSMCT32, kW, kH);
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_EQ(g_texture_cache->GetHeldStats().dropped, 1u);
		EXPECT_FALSE(g_texture_cache->HasHeldPages());
		int bad = 0;
		for (int y = 0; y < kH; y++)
		{
			for (int x = 0; x < kW; x++)
				bad += m_gs->m_mem.ReadPixel32Z(x, y, kDepthBP, kBW) != 0x80206040u;
		}
		EXPECT_EQ(bad, 0);
	}

	// The copy road (the CPU sprite renderer the hardware renderer runs texture copies on) reads a texture over
	// the pages from local memory: the frame it writes holds the words. The burst memo is off here, since its
	// block hashes would materialise the words on their own (TheBurstMemoHashesTheWords).
	TEST_P(DepthFormatMoveHeld, TheCopyRoadReadsTheWords)
	{
		GSConfig.UserHacks_CPUSpriteRenderBW = 10;
		GSConfig.UserHacks_CPUSpriteRenderLevel = 0;
		GSBurstMemo::SetForcedOff(true);
		GSVertexSW::InitStatic();
		ASSERT_NE(Hold(), nullptr);
		DrawTextured(kDepthBP, PSMCT32, 0x3000, 64, 32);
		EXPECT_EQ(Materialised(), 1u);
		int bad = 0;
		for (int y = 0; y < 32; y++)
		{
			for (int x = 0; x < 64; x++)
				bad += m_gs->m_mem.ReadPixel32(x, y, 0x3000, kBW) != ((kBefore & 0xFF000000u) | kHeld);
		}
		EXPECT_EQ(bad, 0);
	}

	// The copy road after a draw into the depth target: the road looks for colour targets only, so it reads
	// local memory, which does not have that draw (neither does it after the CPU move); it has the words.
	TEST_P(DepthFormatMoveHeld, TheCopyRoadAfterADrawIntoTheDepthTargetReadsTheWords)
	{
		GSConfig.UserHacks_CPUSpriteRenderBW = 10;
		GSConfig.UserHacks_CPUSpriteRenderLevel = 0;
		GSBurstMemo::SetForcedOff(true);
		GSVertexSW::InitStatic();
		ASSERT_NE(Hold(), nullptr);
		DrawDepth(0, 0, 128, 64);
		DrawTextured(kDepthBP, PSMCT32, 0x3000, 64, 32);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
		int bad = 0;
		for (int y = 0; y < 32; y++)
		{
			for (int x = 0; x < 64; x++)
				bad += m_gs->m_mem.ReadPixel32(x, y, 0x3000, kBW) != ((kBefore & 0xFF000000u) | kHeld);
		}
		EXPECT_EQ(bad, 0);
	}

	// The burst memo keys a draw on the hashes of the blocks it reads: a block in a held page hashes the words.
	TEST_P(DepthFormatMoveHeld, TheBurstMemoHashesTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		const u32 block = kDepthBP + 5;
		const u64 h = g_texture_cache->BlockHash(m_gs->m_mem, block);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
		EXPECT_EQ(h, g_texture_cache->BlockHash(m_gs->m_mem, block));
	}

	// And the check of a recorded draw's keys against local memory.
	TEST_P(DepthFormatMoveHeld, TheBurstMemoKeyCheckHashesTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		const u32 block = kDepthBP + 5;
		const u64 none = 0;
		g_texture_cache->BlockHashesMatch(m_gs->m_mem, &block, &none, 1);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// The texture key of a lookup that names nothing is made from block identities kept per block (GSBlockId,
	// GSTextureCache::BlockHash's cache). Identities kept from before the words went in must not survive their
	// download: the download marks its pages, so the next key over them is the key of the words. The key is the
	// same with the per-block cache and without it, so the one without is the reference.
	TEST_P(DepthFormatMoveHeld, TheBlockIdentitiesOverThePagesAreTakenAfterTheWords)
	{
		GSTextureCache::Target* const depth = Hold();
		ASSERT_NE(depth, nullptr);
		m_gs->m_mem.MarkAllPagesWritten(); // Hold() wrote kBefore with no mark

		GIFRegTEX0 tex0 = {};
		tex0.TBP0 = kDepthBP;
		tex0.TBW = kBW;
		tex0.PSM = PSMCT32;
		tex0.TW = 10;
		tex0.TH = 8;
		const auto key = [&](bool cache) {
			GSTextureCache::SetBlockHashCacheEnabled(cache);
			const GSTextureCache::HashCacheKey k = GSTextureCache::HashCacheKey::Create(tex0, GIFRegTEXA{}, nullptr, nullptr, GSTextureCache::SourceRegion{}, true);
			GSTextureCache::SetBlockHashCacheEnabled(true);
			return k;
		};

		// Identities of the bytes under the hold (kBefore), kept in the cache: what a lookup made before the
		// move would have left there. Making a key does not materialise the words; reading a texture does.
		const GSTextureCache::HashCacheKey before = key(true);
		EXPECT_EQ(Materialised(), 0u);

		GSTextureCache::SetBlockHashVerify(true);
		depth->m_age = 5;
		depth->m_used = false;
		DrawTextured(kDepthBP, PSMCT32, 0x3000, 128, 64);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);

		const GSTextureCache::HashCacheKey after = key(true);
		EXPECT_TRUE(after == key(false)) << "a block identity kept from before the words went in";
		EXPECT_FALSE(after == before);
		const GSTextureCache::BlockHashVerifyReport report = GSTextureCache::GetBlockHashVerifyReport();
		EXPECT_EQ(report.stale_blocks, 0u);
		GSTextureCache::SetBlockHashVerify(false);
	}

	// A palette load from the pages reads local memory on the CPU: it gets the words.
	TEST_P(DepthFormatMoveHeld, APaletteLoadGetsTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		Packet p;
		GIFReg r = {};
		r.U64 = 0;
		r.TEX0.TBP0 = 0x3000;
		r.TEX0.TBW = 4;
		r.TEX0.PSM = PSMT8;
		r.TEX0.TW = 8;
		r.TEX0.TH = 8;
		r.TEX0.CBP = kDepthBP;
		r.TEX0.CPSM = PSMCT32;
		r.TEX0.CLD = 1;
		p.Reg(GIF_A_D_REG_TEX0_1, r);
		p.Send(*m_gs, GIFRegPRIM{});
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// The target going away (here a texture cache purge, as on a settings change) leaves the words held, since
	// local memory, where the CPU move would have put them, outlives it; the next reader gets them.
	TEST_P(DepthFormatMoveHeld, DroppingTheTargetKeepsTheWordsHeld)
	{
		ASSERT_NE(Hold(), nullptr);
		m_gs->PurgeTextureCache(true, true, true);
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_TRUE(g_texture_cache->HasHeldPages());
		EXPECT_TRUE(Untouched());

		SetTransfer(1, kDepthBP, PSMZ32, 0, 0, 64, 32);
		std::vector<u32> fifo(64 * 32);
		const int qwc = static_cast<int>(fifo.size() * sizeof(u32) / 16);
		m_gs->InitReadFIFO(reinterpret_cast<u8*>(fifo.data()), qwc);
		m_gs->ReadFIFO(reinterpret_cast<u8*>(fifo.data()), qwc);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// A hardware reset or a savestate load replaces local memory: the words are void, nothing is written.
	TEST_P(DepthFormatMoveHeld, AResetDropsTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		m_gs->Reset(true);
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_EQ(g_texture_cache->GetHeldStats().dropped, 1u);
		EXPECT_FALSE(g_texture_cache->HasHeldPages());
	}

	// The game moves into the same pages again: the earlier hold is replaced, not downloaded.
	TEST_P(DepthFormatMoveHeld, AnotherMoveOverTheHoldReplacesIt)
	{
		GSTextureCache::Target* const depth = Hold();
		ASSERT_NE(depth, nullptr);
		depth->m_drawn_since_read = GSVector4i::zero();
		EXPECT_TRUE(GameMove());
		EXPECT_EQ(g_texture_cache->GetHeldStats().dropped, 1u);
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_NE(g_texture_cache->HeldBy(depth), nullptr);
		EXPECT_TRUE(Untouched());
	}

	// A write of local memory: one that replaces every held word drops the hold, one that does not
	// materialises it first (the hook ClearGSLocalMemory calls).
	TEST_P(DepthFormatMoveHeld, AWriteThatReplacesTheWordsDropsThem)
	{
		ASSERT_NE(Hold(), nullptr);
		const GSOffset whole = m_gs->m_mem.GetOffset(kDepthBP, kBW, PSMZ32);
		g_texture_cache->BeforeLocalMemoryWrite(whole, GSVector4i(0, 0, kW, kH), true);
		EXPECT_EQ(g_texture_cache->GetHeldStats().dropped, 1u);
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_FALSE(g_texture_cache->HasHeldPages());
	}

	TEST_P(DepthFormatMoveHeld, AWriteOfPartOfTheWordsMaterialisesThem)
	{
		ASSERT_NE(Hold(), nullptr);
		const GSOffset whole = m_gs->m_mem.GetOffset(kDepthBP, kBW, PSMZ32);
		g_texture_cache->BeforeLocalMemoryWrite(whole, GSVector4i(0, 0, 64, 32), true);
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	TEST_P(DepthFormatMoveHeld, AMaskedWriteOverTheWordsMaterialisesThem)
	{
		ASSERT_NE(Hold(), nullptr);
		const GSOffset whole = m_gs->m_mem.GetOffset(kDepthBP, kBW, PSMZ32);
		g_texture_cache->BeforeLocalMemoryWrite(whole, GSVector4i(0, 0, kW, kH), false); // keeps some of the low 24 bits
		EXPECT_EQ(Materialised(), 1u);
		EXPECT_EQ(Mismatches(kHeld), 0);
	}

	// Readers elsewhere in memory leave the hold alone: nothing is downloaded that nobody reads.
	TEST_P(DepthFormatMoveHeld, ReadersElsewhereLeaveTheHold)
	{
		ASSERT_NE(Hold(), nullptr);
		SetTransfer(1, 0x3000, PSMCT32, 0, 0, 64, 32);
		std::vector<u32> fifo(64 * 32);
		const int qwc = static_cast<int>(fifo.size() * sizeof(u32) / 16);
		m_gs->InitReadFIFO(reinterpret_cast<u8*>(fifo.data()), qwc);
		m_gs->ReadFIFO(reinterpret_cast<u8*>(fifo.data()), qwc);
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_TRUE(g_texture_cache->HasHeldPages());
		EXPECT_TRUE(Untouched());
	}





	// The lean path's early return in GSTextureCache::InvalidateVideoMem (no source under the write) cannot skip a held
	// word: a hold is not a source, and every writer of local memory writes the words in before it reaches that
	// call. Here a constant clear over part of the pages: the clear's post-draw invalidation finds no source under
	// it and returns early, and the words went in before the clear all the same.
	TEST_P(DepthFormatMoveHeld, TheLeanEarlyReturnDoesNotSkipTheWords)
	{
		ASSERT_NE(Hold(), nullptr);
		const u32 clear_bp = kDepthBP + 32 * kBW * 3; // page rows 3 and 4
		EXPECT_TRUE(NoSourceUnder(clear_bp, PSMCT32, GSVector4i(0, 0, kW, 64))) << "the early return must be the road taken";
		DrawColour(clear_bp, PSMCT32, kW, 64);
		EXPECT_EQ(Materialised(), 1u);
		int bad = 0;
		for (int y = 0; y < kH; y++)
		{
			for (int x = 0; x < kW; x++)
			{
				const u32 got = m_gs->m_mem.ReadPixel32Z(x, y, kDepthBP, kBW);
				const bool cleared = y >= 96 && y < 160;
				bad += cleared ? (got != 0x80206040u) : (got != ((kBefore & 0xFF000000u) | kHeld));
			}
		}
		EXPECT_EQ(bad, 0);
	}

	TEST_P(DepthFormatMoveHeld, AnInvalidationWithNoSourceUnderItLeavesTheHold)
	{
		ASSERT_NE(Hold(), nullptr);
		EXPECT_TRUE(NoSourceUnder(kDepthBP, PSMZ24, GSVector4i(0, 0, kW, kH))) << "the early return must be the road taken";
		g_texture_cache->InvalidateVideoMem(m_gs->m_mem.GetOffset(kDepthBP, kBW, PSMZ24), GSVector4i(0, 0, kW, kH), false);
		EXPECT_TRUE(g_texture_cache->HasHeldPages());
		EXPECT_EQ(Materialised(), 0u);
		EXPECT_TRUE(Untouched());
	}

	// A draw into the held depth target takes the front-of-list answer (the target is at the front, same layout and
	// scale), and the readback that follows lands over the words.
	TEST_P(DepthFormatMoveHeld, ADrawIntoTheHeldTargetTakesTheFrontAnswer)
	{
		GSTextureCache::Target* const depth = Hold();
		ASSERT_NE(depth, nullptr);
		GIFRegTEX0 layout = {};
		layout.TBP0 = kDepthBP;
		layout.TBW = kBW;
		layout.PSM = PSMZ24;
		EXPECT_EQ(g_texture_cache->FrontDrawTarget(layout, GetParam(), GSTextureCache::DepthStencil, true, GSVector4i(0, 0, 128, 64)), depth);
		DrawDepth(0, 0, 128, 64);
		ASSERT_EQ(DepthAt(kDepthBP), depth);
		m_gs->ReadbackTextureCache();
		EXPECT_EQ(Mismatches(kOther, GSVector4i(0, 0, 128, 64)), 0);
	}

	INSTANTIATE_TEST_SUITE_P(Scale, DepthFormatMoveHeld, ::testing::Values(1.0f, 2.0f),
		[](const ::testing::TestParamInfo<float>& info) { return info.param == 1.0f ? std::string("Native") : std::string("Upscaled"); });
} // namespace
