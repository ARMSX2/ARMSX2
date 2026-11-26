// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Deferred plain scratch copies (GSRendererHW::TryDeferScratchCopy / TryResolveDeferredScratchAlias).
// Exercises the pattern used by games such as The Sims 2: Castaway, which unpack paletted textures into
// a fixed scratch PSMCT32 target and build a mip chain before sampling it on the main framebuffer.

#include "gs_hw_draw_harness.h"

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <cstring>

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 MAIN_FBP_PAGE = 0x50; // 0x0a00 blocks
	constexpr u32 SCRATCH_L0_PAGE = 0x168; // 0x2d00 blocks (128x128 C32)
	constexpr u32 SCRATCH_L1_PAGE = 0x188; // 0x3100 blocks (64x64 C32)
	constexpr u32 SCRATCH_L2_PAGE = 0x190; // 0x3200 blocks (32x32 C32)
	constexpr u32 SRC_TEX_A_BP = 0x3400; // 128x128 P8
	constexpr u32 SRC_TEX_B_BP = 0x3600; // 128x128 P8
	constexpr u32 SRC_TEX_C_BP = 0x3800; // 64x64 P8
	constexpr u32 CLUT_BP = 0x3A00;

	class CountingDevice final : public CaptureDevice
	{
	public:
		void DoRenderHW(GSHWDrawConfig& config) override
		{
			CaptureDevice::DoRenderHW(config);
			m_last_tex = config.tex;
			m_last_tex_size = config.tex ? config.tex->GetSize() : GSVector2i(0, 0);
		}

		using CaptureDevice::DoStretchRect;
		void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
			ShaderConvertSelector shader, Filter filter) override
		{
			m_stretch_rects++;
			CaptureDevice::DoStretchRect(sTex, sRect, dTex, dRect, shader, filter);
		}

		void DoCopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override
		{
			m_copy_rects++;
			CaptureDevice::DoCopyRect(sTex, dTex, r, destX, destY);
		}

		void ResetCounts()
		{
			m_draws = 0;
			m_stretch_rects = 0;
			m_copy_rects = 0;
			m_last_tex = nullptr;
			m_last_tex_size = GSVector2i(0, 0);
		}

		u32 m_stretch_rects = 0;
		u32 m_copy_rects = 0;
		GSTexture* m_last_tex = nullptr;
		GSVector2i m_last_tex_size{};
	};

	class GSDeferredScratchCopy : public Fixture
	{
	protected:
		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.UpscaleMultiplier = 1.0f;
			GSConfig.HWMipmap = true;
			GSConfig.TexturePreloading = TexturePreloadingLevel::Full;
			GSVertexSW::InitStatic();
			BringUp();
		}

		std::unique_ptr<CaptureDevice> NewDevice() override
		{
			auto dev = std::make_unique<CountingDevice>();
			m_counting = dev.get();
			return dev;
		}

		void SeedTextures()
		{
			GSLocalMemory& mem = m_gs->m_mem;
			std::memset(mem.m_vm8, 0, GSLocalMemory::m_vmsize);
			for (u32 i = 0; i < 256; i++)
			{
				const u32 rgba = 0x80000000u | (i << 16) | ((i ^ 0x55u) << 8) | (255u - i);
				mem.WritePixel32(static_cast<int>(i & 15), static_cast<int>(i >> 4), rgba, CLUT_BP, 1);
			}
			auto fill_p8 = [&](u32 bp, u32 bw, int w, int h, u32 seed) {
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++)
						mem.WritePixel8(x, y, static_cast<u8>((x + y * 3 + seed) & 0xFF), bp, bw);
			};
			fill_p8(SRC_TEX_A_BP, 2, 128, 128, 1);
			fill_p8(SRC_TEX_B_BP, 2, 128, 128, 77);
			fill_p8(SRC_TEX_C_BP, 1, 64, 64, 155);
			mem.MarkAllPagesWritten();
		}

		static void SendScratchSprite(GSState& gs, u32 dst_fbp_page, u32 dst_fbw, int dst_w, int dst_h,
			u32 src_tbp, u32 src_tbw, u32 src_psm, u32 src_tw, u32 src_th,
			bool abe = false, u32 fbmsk = 0, int dst_x0 = 0, int dst_y0 = 0)
		{
			Packet p;
			GIFReg r;
			r.U64 = 0;
			r.PRMODECONT.AC = 1;
			p.Reg(GIF_A_D_REG_PRMODECONT, r);
			r.U64 = 0;
			r.XYOFFSET.OFX = 8; // 0.5
			r.XYOFFSET.OFY = 8;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);
			r.U64 = 0;
			r.SCISSOR.SCAX1 = 255;
			r.SCISSOR.SCAY1 = 255;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);
			r.U64 = 0;
			r.FRAME.FBP = dst_fbp_page;
			r.FRAME.FBW = dst_fbw;
			r.FRAME.PSM = PSMCT32;
			r.FRAME.FBMSK = fbmsk;
			p.Reg(GIF_A_D_REG_FRAME_1, r);
			r.U64 = 0;
			r.ZBUF.ZBP = 0x100;
			r.ZBUF.PSM = PSMZ32;
			r.ZBUF.ZMSK = 1;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);
			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			p.Reg(GIF_A_D_REG_TEST_1, r);
			r.U64 = 0;
			r.TEXA.TA0 = 0;
			r.TEXA.TA1 = 128;
			p.Reg(GIF_A_D_REG_TEXA, r);
			r.U64 = 0;
			r.TEX1.MMAG = 1;
			r.TEX1.MMIN = 1;
			p.Reg(GIF_A_D_REG_TEX1_1, r);
			r.U64 = 0;
			r.CLAMP.WMS = CLAMP_CLAMP;
			r.CLAMP.WMT = CLAMP_CLAMP;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);
			r.U64 = 0;
			r.TEX0.TBP0 = src_tbp;
			r.TEX0.TBW = src_tbw;
			r.TEX0.PSM = src_psm;
			r.TEX0.TW = src_tw;
			r.TEX0.TH = src_th;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
			r.TEX0.CBP = CLUT_BP;
			r.TEX0.CPSM = PSMCT32;
			r.TEX0.CLD = (src_psm == PSMT8 || src_psm == PSMT4) ? 1 : 0;
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			r.U64 = 0;
			r.RGBAQ.R = r.RGBAQ.G = r.RGBAQ.B = 128;
			r.RGBAQ.A = 128;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);
			// (-0.5, -0.5) .. (dst_w - 0.5, dst_h - 0.5) after subtracting OFX=0.5 -> raw 0 .. dst_w*16
			p.VertexST(dst_x0 * 16, dst_y0 * 16, 0, 0.0f, 0.0f, 1.0f);
			p.VertexST((dst_x0 + dst_w) * 16, (dst_y0 + dst_h) * 16, 0, 1.0f, 1.0f, 1.0f);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 0;
			prim.ABE = abe ? 1 : 0;
			p.Send(gs, prim);
		}

		static void SendMainSample(GSState& gs, u32 tbp, u32 tbw, u32 tw, u32 th)
		{
			Packet p;
			Environment(p, MAIN_FBP_PAGE, PSMCT32);
			GIFReg r;
			r.U64 = 0;
			r.TEX1.MMAG = 1;
			r.TEX1.MMIN = 1;
			p.Reg(GIF_A_D_REG_TEX1_1, r);
			r.U64 = 0;
			r.CLAMP.WMS = CLAMP_REPEAT;
			r.CLAMP.WMT = CLAMP_REPEAT;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);
			r.U64 = 0;
			r.TEX0.TBP0 = tbp;
			r.TEX0.TBW = tbw;
			r.TEX0.PSM = PSMCT32;
			r.TEX0.TW = tw;
			r.TEX0.TH = th;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_MODULATE;
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			r.U64 = 0;
			r.RGBAQ.R = r.RGBAQ.G = r.RGBAQ.B = r.RGBAQ.A = 128;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(64 * 16, 64 * 16, 0, (1 << tw) * 16, (1 << th) * 16);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 1;
			p.Send(gs, prim);
		}

		CountingDevice* m_counting = nullptr;
	};

	TEST_F(GSDeferredScratchCopy, UnpackAndMipChainDeferredUntilRead)
	{
		SeedTextures();
		m_counting->ResetCounts();

		// Unread chain A -> 0x2d00 -> 0x3100 -> 0x3200, then immediately overwritten by chain B.
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 64, 64, SCRATCH_L0_PAGE << 5, 2, PSMCT32, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L2_PAGE, 1, 32, 32, SCRATCH_L1_PAGE << 5, 1, PSMCT32, 6, 6);

		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_B_BP, 2, PSMT8, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 64, 64, SCRATCH_L0_PAGE << 5, 2, PSMCT32, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L2_PAGE, 1, 32, 32, SCRATCH_L1_PAGE << 5, 1, PSMCT32, 6, 6);

		EXPECT_EQ(m_counting->m_draws, 0u);
		EXPECT_EQ(m_counting->m_stretch_rects, 0u);

		// Reading L0 (1:1 unpack) aliases directly to the VRAM source with zero StretchRects.
		SendMainSample(*m_gs, SCRATCH_L0_PAGE << 5, 2, 7, 7);
		EXPECT_EQ(m_counting->m_draws, 1u);
		EXPECT_EQ(m_counting->m_stretch_rects, 0u);
		EXPECT_EQ(m_counting->m_last_tex_size, GSVector2i(128, 128));

		// First read of L1 builds and caches the 64x64 mip via 1 StretchRect.
		SendMainSample(*m_gs, SCRATCH_L1_PAGE << 5, 1, 6, 6);
		EXPECT_EQ(m_counting->m_draws, 2u);
		EXPECT_EQ(m_counting->m_stretch_rects, 1u);
		EXPECT_EQ(m_counting->m_last_tex_size, GSVector2i(64, 64));

		// Second read of L1 hits the scratch mip cache with zero extra StretchRects.
		SendMainSample(*m_gs, SCRATCH_L1_PAGE << 5, 1, 6, 6);
		EXPECT_EQ(m_counting->m_draws, 3u);
		EXPECT_EQ(m_counting->m_stretch_rects, 1u);
	}

	TEST_F(GSDeferredScratchCopy, RootUploadPinsHashCacheWithZeroCopiesAndKeepsMipCache)
	{
		SeedTextures();
		m_counting->ResetCounts();

		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 64, 64, SCRATCH_L0_PAGE << 5, 2, PSMCT32, 7, 7);
		// Build L1 mip once (1 StretchRect).
		SendMainSample(*m_gs, SCRATCH_L1_PAGE << 5, 1, 6, 6);
		EXPECT_EQ(m_counting->m_draws, 1u);
		EXPECT_EQ(m_counting->m_stretch_rects, 1u);
		EXPECT_EQ(m_counting->m_copy_rects, 0u);

		// Overwrite a block on the root texture's page: hash-cached root is immutable & pinned,
		// so zero CopyRects occur and neither L0 nor L1 breaks the render pass.
		GIFRegBITBLTBUF blit = {};
		blit.DBP = SRC_TEX_A_BP + 8;
		blit.DBW = 1;
		blit.DPSM = PSMCT32;
		m_gs->InvalidateVideoMem(blit, GSVector4i(0, 0, 16, 16));

		EXPECT_EQ(m_counting->m_copy_rects, 0u);
		EXPECT_EQ(m_counting->m_stretch_rects, 1u);

		// Reading L0 still aliases directly to the pinned HashCacheEntry with zero copies.
		SendMainSample(*m_gs, SCRATCH_L0_PAGE << 5, 2, 7, 7);
		EXPECT_EQ(m_counting->m_draws, 2u);
		EXPECT_EQ(m_counting->m_copy_rects, 0u);
		EXPECT_EQ(m_counting->m_stretch_rects, 1u);

		// Re-submitting the same texture A chain hits the retained mip cache with zero extra StretchRects.
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 64, 64, SCRATCH_L0_PAGE << 5, 2, PSMCT32, 7, 7);
		SendMainSample(*m_gs, SCRATCH_L1_PAGE << 5, 1, 6, 6);
		EXPECT_EQ(m_counting->m_draws, 3u);
		EXPECT_EQ(m_counting->m_stretch_rects, 1u);
		EXPECT_EQ(m_counting->m_copy_rects, 0u);
	}

	TEST_F(GSDeferredScratchCopy, StaleTargetAtScratchAddressSelfHeals)
	{
		SeedTextures();
		m_counting->ResetCounts();

		// Create a real non-scratch RenderTarget at SCRATCH_L0_PAGE (e.g., from a loading screen).
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7, true, 0);
		EXPECT_EQ(m_counting->m_draws, 1u);

		// Two consecutive full scratch copies advance streak (1, 2); the third retires the stale target and defers.
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_B_BP, 2, PSMT8, 7, 7);
		EXPECT_EQ(m_counting->m_draws, 3u);

		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 1, 64, 64, SRC_TEX_C_BP, 1, PSMT8, 6, 6);
		EXPECT_EQ(m_counting->m_draws, 3u);
	}

	TEST_F(GSDeferredScratchCopy, PartialWriteToScratchMaterializesFirst)
	{
		SeedTextures();
		m_counting->ResetCounts();

		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		EXPECT_EQ(m_counting->m_draws, 0u);
		EXPECT_EQ(m_counting->m_stretch_rects, 0u);

		// Partial sprite at (32, 0) cannot be deferred and must materialize 0x2d00 first
		// (1:1 ShaderConvert::COPY may fast-path through CopyRect instead of StretchRect).
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 32, 32, SRC_TEX_C_BP, 1, PSMT8, 6, 6, false, 0, 32, 0);
		EXPECT_EQ(m_counting->m_stretch_rects + m_counting->m_copy_rects, 1u);
		EXPECT_EQ(m_counting->m_draws, 1u);
	}

	TEST_F(GSDeferredScratchCopy, NonEligibleDrawsNotDeferred)
	{
		SeedTextures();
		m_counting->ResetCounts();

		// Blended copy (ABE=1) must execute immediately.
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7, true, 0);
		EXPECT_EQ(m_counting->m_draws, 1u);

		// Masked copy (FBMSK!=0) must execute immediately.
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 64, 64, SRC_TEX_C_BP, 1, PSMT8, 6, 6, false, 0xFF000000u);
		EXPECT_EQ(m_counting->m_draws, 2u);
	}

	TEST_F(GSDeferredScratchCopy, SwPrimRenderMaterializesDeferredScratchSourceAndFrame)
	{
		SeedTextures();
		m_counting->ResetCounts();

		// Defer a 128x128 scratch unpack at SCRATCH_L0_PAGE.
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		EXPECT_EQ(m_counting->m_draws, 0u);
		EXPECT_EQ(m_counting->m_stretch_rects + m_counting->m_copy_rects, 0u);

		// Enable CPU sprite rendering (copy road): a sprite reading from SCRATCH_L0_PAGE must
		// materialize the deferred target first so CanUseSwPrimRender sees the GPU target
		// instead of reading stale local memory on the CPU.
		GSConfig.UserHacks_CPUSpriteRenderBW = 4;
		GSConfig.UserHacks_CPUSpriteRenderLevel = 0;
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 64, 64, SCRATCH_L0_PAGE << 5, 2, PSMCT32, 7, 7);
		EXPECT_EQ(m_counting->m_stretch_rects + m_counting->m_copy_rects, 1u);
		// Materializing SCRATCH_L0_PAGE makes it a real GPU target, so the copy to L1 draws on GPU.
		EXPECT_EQ(m_counting->m_draws, 1u);

		// Now defer a scratch copy at SCRATCH_L2_PAGE while CPUSpriteRenderBW is off, then re-enable
		// CPUSpriteRenderBW and write to SCRATCH_L2_PAGE from a plain VRAM texture via SwPrimRender:
		// MaterialiseHeldUnderCopy must materialize/invalidate the deferred entry first.
		GSConfig.UserHacks_CPUSpriteRenderBW = 0;
		m_counting->ResetCounts();
		SendScratchSprite(*m_gs, SCRATCH_L2_PAGE, 1, 32, 32, SRC_TEX_C_BP, 1, PSMT8, 6, 6);
		EXPECT_EQ(m_counting->m_draws, 0u);
		EXPECT_EQ(m_counting->m_stretch_rects + m_counting->m_copy_rects, 0u);

		GSConfig.UserHacks_CPUSpriteRenderBW = 4;
		SendScratchSprite(*m_gs, SCRATCH_L2_PAGE, 1, 32, 32, SRC_TEX_C_BP, 1, PSMT8, 6, 6);
		// Materialized before SwPrimRender wrote over SCRATCH_L2_PAGE; SwPrimRender itself did 0 GPU draws.
		EXPECT_EQ(m_counting->m_stretch_rects + m_counting->m_copy_rects, 1u);
		EXPECT_EQ(m_counting->m_draws, 0u);
		GSConfig.UserHacks_CPUSpriteRenderBW = 0;
	}

	TEST_F(GSDeferredScratchCopy, HashCacheLimitsUnpinScratchMipCacheOnVSync)
	{
		SeedTextures();
		m_counting->ResetCounts();

		const GSTextureCache::HashCacheLimits saved_limits = GSTextureCache::GetHashCacheLimits();
		auto invalidate_bp = [&](u32 bp) {
			GIFRegBITBLTBUF blit = {};
			blit.DBP = bp;
			blit.DBW = 2;
			blit.DPSM = PSMCT32;
			m_gs->InvalidateVideoMem(blit, GSVector4i(0, 0, 16, 16));
		};

		// Populate and sample mip chains for textures A, B, and C so all 3 are pinned in m_scratch_mip_cache.
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 64, 64, SCRATCH_L0_PAGE << 5, 2, PSMCT32, 7, 7);
		SendMainSample(*m_gs, SCRATCH_L1_PAGE << 5, 1, 6, 6);

		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_B_BP, 2, PSMT8, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 64, 64, SCRATCH_L0_PAGE << 5, 2, PSMCT32, 7, 7);
		SendMainSample(*m_gs, SCRATCH_L1_PAGE << 5, 1, 6, 6);

		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 1, 64, 64, SRC_TEX_C_BP, 1, PSMT8, 6, 6);
		SendScratchSprite(*m_gs, SCRATCH_L1_PAGE, 1, 32, 32, SCRATCH_L0_PAGE << 5, 1, PSMCT32, 6, 6);
		SendMainSample(*m_gs, SCRATCH_L1_PAGE << 5, 1, 5, 5);

		// Drop m_src entries for A and B via VRAM writes so ONLY m_scratch_mip_cache pins their HashCacheEntries.
		invalidate_bp(SRC_TEX_A_BP);
		invalidate_bp(SRC_TEX_B_BP);
		ASSERT_GE(g_texture_cache->GetHashCacheEntryCount(), 3u);

		// Enforce tight hash cache limit (max_entries = 1): VSync must unpin A and B in AgeScratchMipCache()
		// before IncAge() so the hash cache can actually evict them.
		GSTextureCache::HashCacheLimits tight{};
		tight.max_entries = 1;
		tight.floor_entries = 0;
		tight.max_bytes = 1;
		GSTextureCache::SetHashCacheLimits(tight);

		m_gs->AgeScratchMipCache();
		g_texture_cache->IncAge();
		const size_t after_count = g_texture_cache->GetHashCacheEntryCount();
		GSTextureCache::SetHashCacheLimits(saved_limits);

		// C is still held by m_deferred_scratch_copies and m_src; A and B were unpinned and evicted.
		EXPECT_EQ(after_count, 1u);
	}

	TEST_F(GSDeferredScratchCopy, RetiringTargetAfterDateDrawRemainsValid)
	{
		SeedTextures();
		m_counting->ResetCounts();

		// Issue a DATE draw into SCRATCH_L0_PAGE to create a real target that may populate m_date_chain_rt.
		{
			Packet p;
			Environment(p, SCRATCH_L0_PAGE, PSMCT32);
			GIFReg r;
			r.U64 = 0;
			r.FRAME.FBP = SCRATCH_L0_PAGE;
			r.FRAME.FBW = 2;
			r.FRAME.PSM = PSMCT32;
			p.Reg(GIF_A_D_REG_FRAME_1, r);
			r.U64 = 0;
			r.TEST.DATE = 1;
			r.TEST.DATM = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			p.Reg(GIF_A_D_REG_TEST_1, r);
			r.U64 = 0;
			r.RGBAQ.R = r.RGBAQ.G = r.RGBAQ.B = r.RGBAQ.A = 128;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(128 * 16, 128 * 16, 0, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.ABE = 1;
			p.Send(*m_gs, prim);
		}
		EXPECT_EQ(m_counting->m_draws, 1u);

		// Three consecutive plain full-target scratch overwrites retire the target cleanly via RemoveTarget.
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_B_BP, 2, PSMT8, 7, 7);
		EXPECT_EQ(m_counting->m_draws, 3u);

		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_A_BP, 2, PSMT8, 7, 7);
		EXPECT_EQ(m_counting->m_draws, 3u);

		// A subsequent blended draw at SCRATCH_L0_PAGE materializes a fresh target without stale DATE state.
		SendScratchSprite(*m_gs, SCRATCH_L0_PAGE, 2, 128, 128, SRC_TEX_B_BP, 2, PSMT8, 7, 7, true, 0);
		EXPECT_EQ(m_counting->m_draws, 4u);
	}
} // namespace
