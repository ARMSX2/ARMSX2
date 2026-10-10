// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "gs_hw_draw_harness.h"
#include "GS/Renderers/HW/GSTextureCache.h"

#include <algorithm>
#include <cstring>

using namespace GSHWDrawHarness;

namespace
{
	class RebaseTexture final : public GSTexture
	{
	public:
		RebaseTexture(Usage usage, int width, int height, int levels, Format format)
		{
			m_usage = usage;
			m_size = GSVector2i(width, height);
			m_mipmap_levels = levels;
			m_format = format;
			m_pixels.resize(static_cast<size_t>(width) * height, 0xcdcdcdcd);
		}

		u32 Pixel(int x, int y) const { return m_pixels[static_cast<size_t>(y) * GetWidth() + x]; }
		void* GetNativeHandle() const override { return nullptr; }
		void Unmap() override {}
		void GenerateMipmap() override {}
#ifdef PCSX2_DEVBUILD
		void SetDebugName(std::string_view) override {}
#endif

	protected:
		bool DoUpdate(const GSVector4i& rect, const void* data, int pitch, int layer) override
		{
			for (int y = 0; y < rect.height(); y++)
				std::memcpy(&m_pixels[static_cast<size_t>(rect.top + y) * GetWidth() + rect.left],
					static_cast<const u8*>(data) + static_cast<size_t>(y) * pitch, rect.width() * sizeof(u32));
			return true;
		}

		bool DoMap(GSMap& map, const GSVector4i* rect, int layer) override
		{
			const GSVector4i area = rect ? *rect : GetRect();
			map.bits = reinterpret_cast<u8*>(&m_pixels[static_cast<size_t>(area.top) * GetWidth() + area.left]);
			map.pitch = GetWidth() * sizeof(u32);
			return true;
		}

	private:
		std::vector<u32> m_pixels;
	};

	class RebaseDevice final : public CaptureDevice
	{
	public:
		GSTexture* dispatched_color = nullptr;
		u32 watched_depth_bp = 16384;
		bool depth_present_at_dispatch = false;

		void DoRenderHW(GSHWDrawConfig& config) override
		{
			dispatched_color = config.rt;
			if (watched_depth_bp < 16384)
				depth_present_at_dispatch = g_texture_cache->GetExactTarget(watched_depth_bp, 4,
					GSTextureCache::DepthStencil, watched_depth_bp) != nullptr;
			CaptureDevice::DoRenderHW(config);
		}

		GSTextureCache::Target* watched_target = nullptr;
		GSTexture* old_texture = nullptr;
		u64 pool_before_copy = 0;
		u32 copies = 0;
		bool fail_suffix_allocation = false;
		u32 suffix_allocations = 0;

		void DoCopyRect(GSTexture* source, GSTexture* destination, const GSVector4i& rect, u32 x, u32 y) override
		{
			if (source == old_texture && destination->GetWidth() == 256 && destination->GetHeight() == 224)
			{
				copies++;
				EXPECT_EQ(suffix_allocations, 1u);
				EXPECT_NE(destination, old_texture);
				EXPECT_EQ(GetPoolMemoryUsage(), pool_before_copy);
				ASSERT_NE(watched_target, nullptr);
				EXPECT_EQ(watched_target->m_texture, old_texture);
				EXPECT_EQ(watched_target->m_TEX0.TBP0, 0x2f20u);
				EXPECT_TRUE(rect.eq(GSVector4i(0, 64, 256, 288)));
				EXPECT_EQ(x, 0u);
				EXPECT_EQ(y, 0u);
			}
			const auto* pixels = static_cast<const RebaseTexture*>(source);
			std::vector<u32> copy;
			for (int row = rect.top; row < rect.bottom; row++)
				for (int col = rect.left; col < rect.right; col++)
					copy.push_back(pixels->Pixel(col, row));
			destination->Update(GSVector4i(x, y, x + rect.width(), y + rect.height()),
				copy.data(), rect.width() * sizeof(u32));
		}

	protected:
		GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override
		{
			if (watched_target && width == 256 && height == 224)
			{
				suffix_allocations++;
				if (fail_suffix_allocation)
					return nullptr;
			}
			return new RebaseTexture(usage, width, height, levels, format);
		}
	};

	class GSTargetRebaseIntegration : public Fixture
	{
	protected:
		std::unique_ptr<CaptureDevice> NewDevice() override { return std::make_unique<RebaseDevice>(); }
		RebaseDevice& Device() { return *static_cast<RebaseDevice*>(m_device); }

		static GIFRegTEX0 Layout(u32 bp, u32 psm)
		{
			GIFRegTEX0 layout = {};
			layout.TBP0 = bp;
			layout.TBW = 4;
			layout.PSM = psm;
			layout.TW = 8;
			layout.TH = 9;
			return layout;
		}

		GSTextureCache::Target* ExistingTarget()
		{
			GSConfig.UpscaleMultiplier = 1.0f;
			GSConfig.UserHacks_TextureInsideRt = GSTextureInRtMode::InsideTargets;
			BringUp();
			m_gs->s_n = 1;
			auto* target = g_texture_cache->CreateTarget(Layout(0x2f20, PSMCT32), GSVector2i(256, 288),
				GSVector2i(256, 288), 1.0f, GSTextureCache::RenderTarget, true, 0, false, false, false);
			if (!target)
				return nullptr;
			target->m_valid = GSVector4i(0, 0, 256, 288);
			target->m_drawn_since_read = GSVector4i(0, 64, 256, 288);
			target->m_end_block = GSLocalMemory::GetEndBlockAddress(0x2f20, 4, PSMCT32, target->m_valid);
			target->m_dirty.clear();
			target->m_valid_rgb = true;
			target->m_valid_alpha_low = true;
			target->m_valid_alpha_high = true;
			target->m_alpha_min = 0x91;
			target->m_alpha_max = 0xe7;
			target->m_alpha_known = GSAlphaKnownBits::Known::Nothing();
			target->m_alpha_known_via_union = false;
			target->m_rt_alpha_scale = false;
			target->m_last_draw = m_gs->s_n - 1;
			std::vector<u32> pixels;
			for (int y = 0; y < 288; y++)
				for (int x = 0; x < 256; x++)
					pixels.push_back(0x91000000u | static_cast<u32>((y << 8) | x));
			target->m_texture->Update(target->m_valid, pixels.data(), 256 * sizeof(u32));
			Device().watched_target = target;
			Device().old_texture = target->m_texture;
			Device().pool_before_copy = Device().GetPoolMemoryUsage();
			return target;
		}

		GSTextureCache::Target* Overwrite(GSTextureCache::Source* source = nullptr)
		{
			const GIFRegTEX0 incoming = Layout(0x2c20, PSMZ32);
			const GSVector2i size(256, 224);
			const GSVector4i draw(0, 0, 256, 224);
			auto* target = g_texture_cache->LookupDrawTarget(incoming, size, 1.0f,
				GSTextureCache::RenderTarget, true, 0, false, false, false, draw,
				false, false, false, source);
			// Draw() creates a new target when lookup preserves an old target elsewhere.
			return target ? target : g_texture_cache->CreateTarget(incoming, size, size, 1.0f,
				GSTextureCache::RenderTarget, true, 0, false, false, false, draw, source);
		}

		GSTextureCache::Source* SourceAtOldBase()
		{
			GIFRegTEXA alpha = {};
			GIFRegCLAMP clamp = {};
			GIFRegFRAME frame = {};
			return g_texture_cache->LookupSource(true, Layout(0x2f20, PSMCT32), alpha, clamp,
				GSVector4i(0, 64, 256, 128), nullptr, false, false, frame);
		}
	};

	TEST_F(GSTargetRebaseIntegration, LookupCopiesSuffixBeforeRecycleAndRefreshesMetadataAndMemoryUsage)
	{
		auto* target = ExistingTarget();
		ASSERT_NE(target, nullptr);
		GSTexture* const old_texture = target->m_texture;
		const u64 memory_before = g_texture_cache->GetTargetMemoryUsage();
		const u64 old_bytes = old_texture->GetMemUsage();
		auto* incoming = Overwrite();
		ASSERT_NE(incoming, nullptr);
		auto* rebased = g_texture_cache->GetExactTarget(0x3020, 4, GSTextureCache::RenderTarget, 0x3020);
		ASSERT_EQ(rebased, target);
		EXPECT_EQ(Device().copies, 1u);
		EXPECT_NE(rebased->m_texture, old_texture);
		EXPECT_EQ(rebased->m_TEX0.PSM, PSMCT32);
		EXPECT_EQ(rebased->m_unscaled_size.x, 256);
		EXPECT_EQ(rebased->m_unscaled_size.y, 224);
		EXPECT_TRUE(rebased->m_valid.eq(GSVector4i(0, 0, 256, 224)));
		EXPECT_TRUE(rebased->m_drawn_since_read.eq(rebased->m_valid));
		EXPECT_EQ(rebased->m_end_block, GSLocalMemory::GetEndBlockAddress(0x3020, 4, PSMCT32, rebased->m_valid));
		EXPECT_TRUE(rebased->m_valid_rgb);
		EXPECT_TRUE(rebased->m_valid_alpha_low);
		EXPECT_TRUE(rebased->m_valid_alpha_high);
		EXPECT_EQ(rebased->m_alpha_min, 0x91);
		EXPECT_EQ(rebased->m_alpha_max, 0xe7);
		EXPECT_EQ(g_texture_cache->GetTargetMemoryUsage(), memory_before - old_bytes +
			rebased->m_texture->GetMemUsage() + incoming->m_texture->GetMemUsage());
		EXPECT_GE(Device().GetPoolMemoryUsage(), Device().pool_before_copy + old_bytes);
		const auto* pixels = static_cast<const RebaseTexture*>(rebased->m_texture);
		for (int y = 0; y < 224; y++)
			for (int x = 0; x < 256; x++)
				ASSERT_EQ(pixels->Pixel(x, y), 0x91000000u | static_cast<u32>(((y + 64) << 8) | x));
	}

	TEST_F(GSTargetRebaseIntegration, CachedSourceAtOldLayoutIsInvalidated)
	{
		auto* target = ExistingTarget();
		ASSERT_NE(target, nullptr);
		auto* old_source = SourceAtOldBase();
		ASSERT_NE(old_source, nullptr);
		ASSERT_EQ(old_source->m_from_target, target);
		ASSERT_EQ(old_source->m_from_target_TEX0.TBP0, 0x2f20u);
		ASSERT_NE(Overwrite(), nullptr);
		auto* fresh_source = SourceAtOldBase();
		ASSERT_NE(fresh_source, nullptr);
		// These coordinates still physically alias the suffix. A fresh source can use
		// that target, but must bind its new layout instead of the cached old layout.
		EXPECT_EQ(fresh_source->m_from_target, target);
		EXPECT_EQ(fresh_source->m_from_target_TEX0.TBP0, 0x3020u);
		EXPECT_EQ(fresh_source->m_from_target_TEX0.TBW, 4u);
		EXPECT_EQ(fresh_source->m_from_target_TEX0.PSM, PSMCT32);
	}

	TEST_F(GSTargetRebaseIntegration, CurrentSourceReferenceDeclinesRebase)
	{
		auto* target = ExistingTarget();
		ASSERT_NE(target, nullptr);
		auto* source = SourceAtOldBase();
		ASSERT_NE(source, nullptr);
		ASSERT_EQ(source->m_from_target, target);
		Overwrite(source);
		EXPECT_EQ(Device().copies, 0u);
		EXPECT_EQ(g_texture_cache->GetExactTarget(0x3020, 4, GSTextureCache::RenderTarget, 0x3020), nullptr);
	}

	TEST_F(GSTargetRebaseIntegration, FailedSuffixAllocationFallsBackWithoutCopyOrRebasedTarget)
	{
		ASSERT_NE(ExistingTarget(), nullptr);
		Device().fail_suffix_allocation = true;
		Overwrite();
		EXPECT_GT(Device().suffix_allocations, 0u);
		EXPECT_EQ(Device().copies, 0u);
		EXPECT_EQ(g_texture_cache->GetExactTarget(0x3020, 4, GSTextureCache::RenderTarget, 0x3020), nullptr);
		EXPECT_EQ(g_texture_cache->GetExactTarget(0x2f20, 4, GSTextureCache::RenderTarget, 0x2f20), nullptr);
	}
	class GSSelectedColorInvalidation : public Fixture
	{
	protected:
		std::unique_ptr<CaptureDevice> NewDevice() override { return std::make_unique<RebaseDevice>(); }

		GSTextureCache::Target* CreateValidTarget(int type)
		{
			GIFRegTEX0 layout = {};
			layout.TBP0 = 0x1000;
			layout.TBW = 4;
			layout.PSM = type == GSTextureCache::RenderTarget ? PSMCT32 : PSMZ32;
			auto* target = g_texture_cache->CreateTarget(layout, GSVector2i(256, 128), GSVector2i(256, 128),
				1.0f, type, true, 0, false, false, false);
			if (!target)
				return nullptr;
			target->m_valid = GSVector4i(0, 0, 256, 128);
			target->m_drawn_since_read = target->m_valid;
			target->m_end_block = GSLocalMemory::GetEndBlockAddress(0x1000, 4, layout.PSM, target->m_valid);
			target->m_dirty.clear();
			target->m_valid_rgb = true;
			target->m_valid_alpha_low = true;
			target->m_valid_alpha_high = true;
			target->m_alpha_min = 0x91;
			target->m_alpha_max = 0xe7;
			target->m_alpha_known = GSAlphaKnownBits::Known::Nothing();
			target->m_alpha_known_via_union = false;
			target->m_rt_alpha_scale = false;
			target->m_last_draw = m_gs->s_n;
			std::vector<u32> pixels(256 * 128, 0x91000000);
			target->m_texture->Update(target->m_valid, pixels.data(), 256 * sizeof(u32));
			return target;
		}

		void DrawInsideTarget(bool masked_clear)
		{
			Packet packet;
			Environment(packet, 0x1080 / 32, PSMCT32);
			GIFReg reg = {};
			reg.FRAME.FBP = 0x1080 / 32;
			reg.FRAME.FBW = 4;
			reg.FRAME.PSM = PSMCT32;
			reg.FRAME.FBMSK = masked_clear ? 0xff000000 : 0;
			packet.Reg(GIF_A_D_REG_FRAME_1, reg);
			reg.U64 = 0;
			reg.SCISSOR.SCAX1 = 255;
			reg.SCISSOR.SCAY1 = 127;
			packet.Reg(GIF_A_D_REG_SCISSOR_1, reg);
			GIFRegPRIM prim = {};
			if (masked_clear)
			{
				// Constant sprite plus a preserved alpha channel is ClearWithDraw.
				// TryTargetClear cannot clear the texture while preserving that channel.
				packet.Vertex(0, 0, 1, 0, 0);
				packet.Vertex(256 << 4, 32 << 4, 1, 0, 0);
				prim.PRIM = GS_SPRITE;
			}
			else
			{
				packet.VertexRGBA(0, 0, 1, 0, 0, 0x20, 0x40, 0x60, 0x91);
				packet.VertexRGBA(256 << 4, 0, 1, 0, 0, 0x30, 0x50, 0x70, 0xa1);
				packet.VertexRGBA(0, 32 << 4, 1, 0, 0, 0x40, 0x60, 0x80, 0xb1);
				prim.PRIM = GS_TRIANGLELIST;
				prim.IIP = 1;
			}
			packet.Send(*m_gs, prim);
		}

		void ExpectSelectedLayoutInvalidation(bool masked_clear)
		{
			GSConfig.UpscaleMultiplier = 1.0f;
			GSConfig.UserHacks_TextureInsideRt = GSTextureInRtMode::InsideTargets;
			GSConfig.UserHacks_DisableSafeFeatures = false;
			BringUp();
			auto* color = CreateValidTarget(GSTextureCache::RenderTarget);
			ASSERT_NE(color, nullptr);
			ASSERT_NE(CreateValidTarget(GSTextureCache::DepthStencil), nullptr);
			static_cast<RebaseDevice*>(m_device)->watched_depth_bp = 0x1000;
			DrawInsideTarget(masked_clear);
			auto& device = *static_cast<RebaseDevice*>(m_device);
			ASSERT_EQ(device.m_draws, 1u) << "the primitive must reach the backend";
			ASSERT_EQ(g_texture_cache->GetExactTarget(0x1000, 4, GSTextureCache::RenderTarget, 0x1000), color);
			ASSERT_EQ(device.dispatched_color, color->m_texture);
			ASSERT_TRUE(device.depth_present_at_dispatch) << "depth must survive until post-dispatch invalidation";
			ASSERT_NE(device.m_colormask.wrgba, 0);
			EXPECT_EQ(m_gs->GetCachedCtx()->FRAME.Block(), 0x1080u);
			EXPECT_EQ(color->m_TEX0.TBP0, 0x1000u);
			EXPECT_EQ(g_texture_cache->GetExactTarget(0x1000, 4, GSTextureCache::RenderTarget, 0x1000), color);
			EXPECT_EQ(g_texture_cache->GetExactTarget(0x1000, 4, GSTextureCache::DepthStencil, 0x1000), nullptr);
			if (masked_clear)
			{
				EXPECT_EQ(device.m_colormask.wrgba, 7);
				EXPECT_EQ(m_gs->GetCachedCtx()->FRAME.FBMSK, 0xff000000u);
			}
		}
	};

	TEST_F(GSSelectedColorInvalidation, DispatchedInsideTargetDrawInvalidatesDepthAtSelectedBase)
	{
		ExpectSelectedLayoutInvalidation(false);
	}

	TEST_F(GSSelectedColorInvalidation, MaskedPossibleClearThatDeclinesShortcutStillInvalidatesSelectedBase)
	{
		ExpectSelectedLayoutInvalidation(true);
	}

} // namespace
