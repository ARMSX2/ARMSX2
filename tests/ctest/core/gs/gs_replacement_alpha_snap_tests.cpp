// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the pack-texture alpha snap (GSReplacementAlphaSnap.h): a draw that samples a pack texture
// and alpha-tests near 0x80 takes texture alpha within the window of 0x80 as 0x80, because ASTC
// packs cannot store 0x80 exactly. The rule is tested on its own, then through the renderer on the
// None backend: a pack texture under such a test gets the snap, and nothing else does.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/Common/GSReplacementAlphaSnap.h"
#include "GS/Renderers/HW/GSTextureCache.h"

using namespace GSHWDrawHarness;

namespace
{
	TEST(GSReplacementAlphaSnapRule, EveryComparisonNearOpaqueOnAPackTexture)
	{
		for (u32 atst = ATST_LESS; atst <= ATST_NOTEQUAL; atst++)
			EXPECT_TRUE(GSReplacementAlphaSnap::Wanted(true, true, true, atst, 0x80)) << "ATST " << atst;
	}

	TEST(GSReplacementAlphaSnapRule, NotWhenTheTestDecidesNothing)
	{
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(true, true, true, ATST_NEVER, 0x80));
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(true, true, true, ATST_ALWAYS, 0x80));
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(true, true, false, ATST_EQUAL, 0x80));
	}

	TEST(GSReplacementAlphaSnapRule, NotWithoutAPackTextureWhoseAlphaIsUsed)
	{
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(false, true, true, ATST_EQUAL, 0x80));
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(true, false, true, ATST_EQUAL, 0x80));
	}

	// The references the corpus uses for "is it opaque" (0x7E Sly, 0x7F Shin Onimusha and GT4,
	// 0x80 River King and Armored Core 3) are inside; the common low cut-offs are not.
	TEST(GSReplacementAlphaSnapRule, ReferenceWindow)
	{
		using GSReplacementAlphaSnap::WINDOW;
		EXPECT_TRUE(GSReplacementAlphaSnap::Wanted(true, true, true, ATST_GEQUAL, 0x80 - WINDOW));
		EXPECT_TRUE(GSReplacementAlphaSnap::Wanted(true, true, true, ATST_GEQUAL, 0x80 + WINDOW));
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(true, true, true, ATST_GEQUAL, 0x80 - WINDOW - 1));
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(true, true, true, ATST_GEQUAL, 0x80 + WINDOW + 1));
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(true, true, true, ATST_GEQUAL, 0x40));
		EXPECT_FALSE(GSReplacementAlphaSnap::Wanted(true, true, true, ATST_GREATER, 0));
	}

	constexpr u32 kDstFBP = 0x40;
	// Well clear of the frame and the Z buffer, so the texture cache reads GS memory.
	constexpr u32 kMemoryTBP = 0x3000;
	constexpr u32 kTBW = 8;

	class GSReplacementAlphaSnapDraw : public Fixture
	{
	protected:
		void SetUp() override
		{
			Fixture::SetUp();
			// Full preloading puts every direct texture in the hash cache, which is where a pack
			// texture is found too.
			GSConfig.TexturePreloading = TexturePreloadingLevel::Full;
		}

		/// Texels alternate 0x80 and 0x00 alpha, so neither outcome of an EQUAL 0x80 test is
		/// certain and the renderer keeps the test.
		void FillTexture()
		{
			for (int y = 0; y < 512; y++)
				for (int x = 0; x < 512; x++)
					m_gs->m_mem.WritePixel32(x, y, ((x & 1) ? 0x80u : 0u) << 24 | 0x404040u, kMemoryTBP, kTBW);
		}

		static GIFRegTEX0 TextureTEX0()
		{
			GIFRegTEX0 tex0 = {};
			tex0.TBP0 = kMemoryTBP;
			tex0.TBW = kTBW;
			tex0.PSM = PSMCT32;
			tex0.TW = 9;
			tex0.TH = 9;
			tex0.TCC = 1;
			tex0.TFX = TFX_DECAL;
			return tex0;
		}

		void Draw(bool alpha_test)
		{
			Packet p;
			Environment(p, kDstFBP, PSMCT32);
			Texture(p, kMemoryTBP, PSMCT32);

			GIFReg r = {};
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			if (alpha_test)
			{
				r.TEST.ATE = 1;
				r.TEST.ATST = ATST_EQUAL;
				r.TEST.AREF = 0x80;
				r.TEST.AFAIL = AFAIL_KEEP;
			}
			p.Reg(GIF_A_D_REG_TEST_1, r);

			p.Vertex(0, 0, 1, 0, 0);
			p.Vertex(256 * 16, 224 * 16, 1, 256 * 16, 224 * 16);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 1;
			p.Send(*m_gs, prim);
		}

		/// Swaps a "pack" texture into the hash-cache entry the draws use, as the pack loader does
		/// when a file finishes loading. Alpha range 0-255, as the loader reports for ASTC.
		void LoadPackTexture()
		{
			GSTexture* const tex = g_gs_device->CreateTexture(512, 512, 1, GSTexture::Format::Color);
			ASSERT_NE(tex, nullptr);
			const GIFRegTEXA texa = {};
			const GSTextureCache::HashCacheKey key =
				GSTextureCache::HashCacheKey::Create(TextureTEX0(), texa, nullptr, nullptr, GSTextureCache::SourceRegion());
			g_texture_cache->InjectHashCacheTexture(key, tex, {0, 255});
		}
	};

	TEST_F(GSReplacementAlphaSnapDraw, AGameTextureIsNotSnapped)
	{
		BringUp();
		FillTexture();

		const u32 before = m_device->m_draws;
		Draw(true);
		ASSERT_EQ(m_device->m_draws, before + 1) << "the draw never reached the backend";
		ASSERT_NE(m_device->m_ps.atst, GSHWDrawConfig::PS_ATST::NONE) << "the alpha test was dropped, so this tests nothing";
		EXPECT_EQ(m_device->m_ps.replacement_alpha_snap, 0u);
	}

	TEST_F(GSReplacementAlphaSnapDraw, APackTextureUnderAnOpaqueTestIsSnapped)
	{
		BringUp();
		FillTexture();
		Draw(true);
		LoadPackTexture();

		const u32 before = m_device->m_draws;
		Draw(true);
		ASSERT_EQ(m_device->m_draws, before + 1) << "the draw never reached the backend";
		ASSERT_NE(m_device->m_ps.atst, GSHWDrawConfig::PS_ATST::NONE) << "the alpha test was dropped, so this tests nothing";
		EXPECT_EQ(m_device->m_ps.replacement_alpha_snap, 1u);
	}

	TEST_F(GSReplacementAlphaSnapDraw, APackTextureWithoutAnAlphaTestIsNotSnapped)
	{
		BringUp();
		FillTexture();
		Draw(true);
		LoadPackTexture();

		const u32 before = m_device->m_draws;
		Draw(false);
		ASSERT_EQ(m_device->m_draws, before + 1) << "the draw never reached the backend";
		EXPECT_EQ(m_device->m_ps.replacement_alpha_snap, 0u);
	}
} // namespace
