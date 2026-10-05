// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins what GSTextureCache::Target::Update uploads when the EE has written into a render target's
// memory: for every dirty rectangle the texels reaching the update texture are that rectangle's
// local memory, unswizzled to RGBA8, and the target's alpha range follows the alpha bytes of the
// rectangles that were read.
//
// Update used to unswizzle straight into a mapped upload buffer and now unswizzles into a scratch
// buffer and uploads from there. The test does not care which: its textures take a mapping or an
// Update() call, and what it checks is the texels the update texture holds when it is copied into
// the target. A rectangle the dirty list names must arrive complete and at the right place, a
// rectangle that is not block aligned must still read its whole blocks for the alpha range, and a
// colour format that has no alpha must not be scanned for it.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/HW/GSTextureCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kFrameBP = 0;
	constexpr int kFrameW = 640;
	constexpr int kFrameH = 448;

	/// A texture in RAM that keeps what is uploaded into it, through Update() or through a mapping.
	/// Rows are padded the way a device pads its upload pitch, so a caller that assumes the pitch
	/// is width * 4 lands in the wrong place. Contents start as 0xCD so a texel nobody wrote reads
	/// as a mismatch.
	class RecordingTexture final : public GSTexture
	{
	public:
		RecordingTexture(Usage usage, int width, int height, int levels, Format format)
		{
			m_usage = usage;
			m_size = GSVector2i(width, height);
			m_mipmap_levels = levels;
			m_format = format;
			// Only Color is read back; the other formats get the widest texel so a write cannot overrun.
			m_texel_size = (format == Format::Color) ? 4 : 8;
			m_stride = ((static_cast<size_t>(width) * m_texel_size + 31) & ~static_cast<size_t>(31)) + 32;
			m_bytes.assign(m_stride * static_cast<size_t>(height), 0xCD);
		}

		static bool Is(const GSTexture* tex) { return tex->GetNativeHandle() == &s_tag; }

		u32 Texel(int x, int y) const
		{
			u32 v;
			std::memcpy(&v, &m_bytes[static_cast<size_t>(y) * m_stride + static_cast<size_t>(x) * 4], sizeof(v));
			return v;
		}

		void* GetNativeHandle() const override { return &s_tag; }
		void Unmap() override {}
		void GenerateMipmap() override {}
#ifdef PCSX2_DEVBUILD
		void SetDebugName(std::string_view) override {}
#endif

	protected:
		bool DoUpdate(const GSVector4i& r, const void* data, int pitch, int layer) override
		{
			for (int y = 0; y < r.height(); y++)
			{
				std::memcpy(&m_bytes[static_cast<size_t>(r.top + y) * m_stride + static_cast<size_t>(r.left) * m_texel_size],
					static_cast<const u8*>(data) + static_cast<size_t>(y) * static_cast<size_t>(pitch),
					static_cast<size_t>(r.width()) * m_texel_size);
			}
			return true;
		}

		bool DoMap(GSMap& m, const GSVector4i* r, int layer) override
		{
			const GSVector4i rc = r ? *r : GetRect();
			m.bits = &m_bytes[static_cast<size_t>(rc.top) * m_stride + static_cast<size_t>(rc.left) * m_texel_size];
			m.pitch = static_cast<int>(m_stride);
			return true;
		}

	private:
		static inline char s_tag = 0;
		size_t m_texel_size = 4;
		size_t m_stride = 0;
		std::vector<u8> m_bytes;
	};

	/// Keeps the texels of every update texture at the moment the cache copies it into a target.
	class RecordingDevice final : public CaptureDevice
	{
	public:
		struct Upload
		{
			GSVector4i dst; // where in the target, in target pixels (scaled)
			std::vector<u32> texels; // the source rectangle, row by row
			int width = 0;
		};

		std::vector<Upload> m_uploads;

		// At native scale the copy is 1:1 and the device serves it as a CopyRect.
		void DoCopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override
		{
			Record(sTex, r,
				GSVector4i(static_cast<int>(destX), static_cast<int>(destY), static_cast<int>(destX) + r.width(),
					static_cast<int>(destY) + r.height()));
		}

	protected:
		using CaptureDevice::DoStretchRect;

		GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override
		{
			return new RecordingTexture(usage, width, height, levels, format);
		}

		void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
			ShaderConvertSelector shader, Filter filter) override
		{
			const float w = static_cast<float>(sTex->GetWidth());
			const float h = static_cast<float>(sTex->GetHeight());
			Record(sTex,
				GSVector4i(static_cast<int>(std::lround(sRect.x * w)), static_cast<int>(std::lround(sRect.y * h)),
					static_cast<int>(std::lround(sRect.z * w)), static_cast<int>(std::lround(sRect.w * h))),
				GSVector4i(static_cast<int>(std::lround(dRect.x)), static_cast<int>(std::lround(dRect.y)),
					static_cast<int>(std::lround(dRect.z)), static_cast<int>(std::lround(dRect.w))));
		}

	private:
		void Record(GSTexture* sTex, const GSVector4i& src_rect, const GSVector4i& dst_rect)
		{
			// The update texture is the one plain colour texture that is the source of a copy.
			if (!RecordingTexture::Is(sTex) || sTex->GetUsage() != GSTexture::Texture || sTex->GetFormat() != GSTexture::Format::Color)
				return;

			const RecordingTexture* src = static_cast<const RecordingTexture*>(sTex);
			Upload up;
			up.dst = dst_rect;
			up.width = src_rect.width();
			for (int y = src_rect.top; y < src_rect.bottom; y++)
			{
				for (int x = src_rect.left; x < src_rect.right; x++)
					up.texels.push_back(src->Texel(x, y));
			}
			m_uploads.push_back(std::move(up));
		}
	};

	/// The colour the test puts at (x, y) of the target's memory. Alpha runs between `alpha_lo`
	/// and `alpha_hi` over the picture; one pixel can be given its own alpha.
	struct Picture
	{
		u32 alpha_lo = 0x10;
		u32 alpha_hi = 0x70;
		int spike_x = -1;
		int spike_y = -1;
		u32 spike_alpha = 0;

		u32 At(int x, int y) const
		{
			const u32 r = static_cast<u32>(x * 5 + y * 3) & 0xff;
			const u32 g = static_cast<u32>(x * 7 + y * 11 + 1) & 0xff;
			const u32 b = static_cast<u32>(x ^ (y * 13)) & 0xff;
			u32 a = alpha_lo + static_cast<u32>(x * 3 + y * 5) % (alpha_hi - alpha_lo + 1);
			if (x == spike_x && y == spike_y)
				a = spike_alpha;
			return r | (g << 8) | (b << 16) | (a << 24);
		}
	};

	class GSTargetUpdate : public Fixture, public ::testing::WithParamInterface<float>
	{
	protected:
		std::unique_ptr<CaptureDevice> NewDevice() override { return std::make_unique<RecordingDevice>(); }

		RecordingDevice& Device() { return *static_cast<RecordingDevice*>(m_device); }

		/// A frame target of `psm` covering the whole 640x448 frame, with the picture in local memory
		/// and nothing dirty.
		GSTextureCache::Target* Scene(u32 psm, const Picture& pic)
		{
			GSConfig.UpscaleMultiplier = GetParam();
			BringUp();

			Packet p;
			Environment(p, kFrameBP / 32, psm);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(kFrameW << 4, kFrameH << 4, 0, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);

			GSTextureCache::Target* const t =
				g_texture_cache->GetExactTarget(kFrameBP, 10, GSTextureCache::RenderTarget, kFrameBP);
			if (!t)
				return nullptr;

			for (int y = 0; y < kFrameH; y++)
			{
				for (int x = 0; x < kFrameW; x++)
					g_gs_renderer->m_mem.WritePixel32(x, y, pic.At(x, y), t->m_TEX0.TBP0, t->m_TEX0.TBW);
			}

			t->m_dirty.clear();
			return t;
		}

		static void MarkDirty(GSTextureCache::Target* t, GSVector4i rect, bool alpha)
		{
			RGBAMask mask = {};
			mask._u32 = alpha ? 0xf : 0x7;
			t->m_dirty.push_back(GSDirtyRect(rect, t->m_TEX0.PSM, t->m_TEX0.TBW, mask, false));
		}

		/// Every texel the update texture carried for `rects` is the picture's pixel at that place.
		void ExpectPicture(const GSTextureCache::Target* t, std::vector<GSVector4i> rects, const Picture& pic)
		{
			auto& ups = Device().m_uploads;
			ASSERT_EQ(ups.size(), rects.size());

			// The order the dirty list is walked in is not what is being pinned.
			auto by_position = [](const GSVector4i& a, const GSVector4i& b) {
				return a.top != b.top ? a.top < b.top : a.left < b.left;
			};
			std::sort(rects.begin(), rects.end(), by_position);
			std::vector<RecordingDevice::Upload*> sorted;
			for (auto& up : ups)
				sorted.push_back(&up);
			const float scale = t->m_scale;
			auto target_rect = [scale](const RecordingDevice::Upload* up) {
				return GSVector4i(static_cast<int>(std::lround(up->dst.x / scale)), static_cast<int>(std::lround(up->dst.y / scale)),
					static_cast<int>(std::lround(up->dst.z / scale)), static_cast<int>(std::lround(up->dst.w / scale)));
			};
			std::sort(sorted.begin(), sorted.end(), [&](const RecordingDevice::Upload* a, const RecordingDevice::Upload* b) {
				return by_position(target_rect(a), target_rect(b));
			});

			for (size_t i = 0; i < rects.size(); i++)
			{
				const GSVector4i got = target_rect(sorted[i]);
				ASSERT_TRUE(got.eq(rects[i])) << "rect " << i << " went to (" << got.x << "," << got.y << "," << got.z << ","
											  << got.w << ") instead of (" << rects[i].x << "," << rects[i].y << "," << rects[i].z
											  << "," << rects[i].w << ")";
				ASSERT_EQ(sorted[i]->width, rects[i].width());

				int bad = 0;
				for (int y = 0; y < rects[i].height() && bad < 3; y++)
				{
					for (int x = 0; x < rects[i].width() && bad < 3; x++)
					{
						const u32 want = pic.At(rects[i].left + x, rects[i].top + y);
						const u32 have = sorted[i]->texels[static_cast<size_t>(y) * static_cast<size_t>(sorted[i]->width) + static_cast<size_t>(x)];
						if (have != want)
						{
							ADD_FAILURE() << "rect " << i << " texel (" << rects[i].left + x << "," << rects[i].top + y << ") is "
										  << std::hex << have << ", memory holds " << want;
							bad++;
						}
					}
				}
			}
		}

		/// The alpha range of the whole blocks the rectangles read.
		static std::pair<u32, u32> AlphaOfBlocks(u32 psm, const std::vector<GSVector4i>& rects, const Picture& pic)
		{
			const GSVector2i bs = GSLocalMemory::m_psm[psm].bs;
			u32 lo = 255, hi = 0;
			for (const GSVector4i& r : rects)
			{
				const GSVector4i blocks = r.ralign<Align_Outside>(bs);
				for (int y = blocks.top; y < blocks.bottom; y++)
				{
					for (int x = blocks.left; x < blocks.right; x++)
					{
						const u32 a = pic.At(x, y) >> 24;
						lo = std::min(lo, a);
						hi = std::max(hi, a);
					}
				}
			}
			return {lo, hi};
		}
	};

	// One block-aligned rectangle in the middle of the target.
	TEST_P(GSTargetUpdate, OneRectangleArrivesWhole)
	{
		const Picture pic;
		GSTextureCache::Target* const t = Scene(PSMCT32, pic);
		ASSERT_NE(t, nullptr);

		const GSVector4i rect(64, 32, 192, 96);
		MarkDirty(t, rect, true);
		const int prev_min = t->m_alpha_min;
		const int prev_max = t->m_alpha_max;
		Device().m_uploads.clear();
		t->Update();

		ExpectPicture(t, {rect}, pic);
		const auto alpha = AlphaOfBlocks(PSMCT32, {rect}, pic);
		EXPECT_EQ(static_cast<u32>(t->m_alpha_min), std::min(static_cast<u32>(prev_min), alpha.first));
		EXPECT_EQ(static_cast<u32>(t->m_alpha_max), std::max(static_cast<u32>(prev_max), alpha.second));
		EXPECT_TRUE(t->m_dirty.empty());
	}

	// A rectangle that starts and ends inside blocks: the texels that arrive are the rectangle, and
	// the alpha range is over the whole blocks that were read to produce them.
	TEST_P(GSTargetUpdate, RectangleInsideBlocksReadsWholeBlocksForAlpha)
	{
		Picture pic;
		pic.alpha_lo = 0x20;
		pic.alpha_hi = 0x60;
		// Inside the block that the rectangle clips: the alpha range has to include it.
		pic.spike_x = 65;
		pic.spike_y = 33;
		pic.spike_alpha = 0x04;
		GSTextureCache::Target* const t = Scene(PSMCT32, pic);
		ASSERT_NE(t, nullptr);

		const GSVector4i rect(67, 35, 133, 93);
		MarkDirty(t, rect, true);
		const int prev_min = t->m_alpha_min;
		const int prev_max = t->m_alpha_max;
		Device().m_uploads.clear();
		t->Update();

		ExpectPicture(t, {rect}, pic);
		const auto alpha = AlphaOfBlocks(PSMCT32, {rect}, pic);
		EXPECT_EQ(alpha.first, 0x04u);
		EXPECT_EQ(static_cast<u32>(t->m_alpha_min), std::min(static_cast<u32>(prev_min), alpha.first));
		EXPECT_EQ(static_cast<u32>(t->m_alpha_max), std::max(static_cast<u32>(prev_max), alpha.second));
	}

	// Several rectangles of different shapes, one of them with alpha above 0x80: each arrives whole
	// at its own place, the alpha range covers all of them, and the target does not keep the
	// alpha-halving shortcut.
	TEST_P(GSTargetUpdate, SeveralRectanglesEachArriveWhole)
	{
		Picture pic;
		pic.spike_x = 300;
		pic.spike_y = 200;
		pic.spike_alpha = 0xf0;
		GSTextureCache::Target* const t = Scene(PSMCT32, pic);
		ASSERT_NE(t, nullptr);

		const std::vector<GSVector4i> rects = {
			GSVector4i(0, 0, 64, 32),
			GSVector4i(256, 192, 328, 232),
			GSVector4i(8, 400, 616, 408),
			GSVector4i(400, 40, 416, 360),
		};
		for (const GSVector4i& r : rects)
			MarkDirty(t, r, true);
		ASSERT_EQ(t->m_dirty.size(), rects.size());
		const int prev_min = t->m_alpha_min;
		const int prev_max = t->m_alpha_max;
		Device().m_uploads.clear();
		t->Update();

		ExpectPicture(t, rects, pic);
		const auto alpha = AlphaOfBlocks(PSMCT32, rects, pic);
		EXPECT_EQ(alpha.second, 0xf0u);
		EXPECT_EQ(static_cast<u32>(t->m_alpha_min), std::min(static_cast<u32>(prev_min), alpha.first));
		EXPECT_EQ(static_cast<u32>(t->m_alpha_max), std::max(static_cast<u32>(prev_max), alpha.second));
		EXPECT_FALSE(t->m_rt_alpha_scale);
	}

	// The whole target as one rectangle is the largest read the function does, and with every alpha
	// at or below 0x80 the target takes the alpha-halving shortcut.
	TEST_P(GSTargetUpdate, WholeTargetWithLowAlphaTakesTheShortcut)
	{
		const Picture pic;
		GSTextureCache::Target* const t = Scene(PSMCT32, pic);
		ASSERT_NE(t, nullptr);
		const GSVector4i all(0, 0, kFrameW, kFrameH);
		ASSERT_TRUE(t->m_valid.eq(all)) << "the scene's target is not the whole frame";
		// The scene's sprite has alpha 0x80, which already left the shortcut on. Clear it so that what
		// Update decides is what the test sees.
		t->m_rt_alpha_scale = false;

		MarkDirty(t, all, true);
		Device().m_uploads.clear();
		t->Update();

		ExpectPicture(t, {all}, pic);
		// One rectangle over the whole valid area replaces the alpha range instead of widening it.
		const auto alpha = AlphaOfBlocks(PSMCT32, {all}, pic);
		EXPECT_EQ(static_cast<u32>(t->m_alpha_min), alpha.first);
		EXPECT_EQ(static_cast<u32>(t->m_alpha_max), alpha.second);
		EXPECT_TRUE(t->m_rt_alpha_scale);
	}

	TEST_P(GSTargetUpdate, WholeTargetWithHighAlphaDoesNot)
	{
		Picture pic;
		pic.spike_x = 5;
		pic.spike_y = 7;
		pic.spike_alpha = 0xc0;
		GSTextureCache::Target* const t = Scene(PSMCT32, pic);
		ASSERT_NE(t, nullptr);
		const GSVector4i all(0, 0, kFrameW, kFrameH);
		ASSERT_TRUE(t->m_valid.eq(all));

		MarkDirty(t, all, true);
		Device().m_uploads.clear();
		t->Update();

		ExpectPicture(t, {all}, pic);
		EXPECT_EQ(t->m_alpha_max, 0xc0);
		EXPECT_FALSE(t->m_rt_alpha_scale);
	}

	// A rectangle that dirtied only colour channels does not touch the alpha range.
	TEST_P(GSTargetUpdate, ColourOnlyRectangleLeavesTheAlphaRangeAlone)
	{
		Picture pic;
		pic.spike_x = 70;
		pic.spike_y = 40;
		pic.spike_alpha = 0xff;
		GSTextureCache::Target* const t = Scene(PSMCT32, pic);
		ASSERT_NE(t, nullptr);

		const GSVector4i rect(64, 32, 192, 96);
		MarkDirty(t, rect, false);
		const int prev_min = t->m_alpha_min;
		const int prev_max = t->m_alpha_max;
		Device().m_uploads.clear();
		t->Update();

		ExpectPicture(t, {rect}, pic);
		EXPECT_EQ(t->m_alpha_min, prev_min);
		EXPECT_EQ(t->m_alpha_max, prev_max);
	}

	// A 24-bit target has no alpha to scan: the texels still arrive (the fourth byte is whatever
	// memory holds), and the alpha range stays where the format fixed it.
	TEST_P(GSTargetUpdate, TwentyFourBitTargetIsNotScannedForAlpha)
	{
		Picture pic;
		pic.alpha_lo = 0x01;
		pic.alpha_hi = 0xfe;
		GSTextureCache::Target* const t = Scene(PSMCT24, pic);
		ASSERT_NE(t, nullptr);
		ASSERT_FALSE(t->HasValidAlpha());

		const GSVector4i rect(64, 32, 192, 96);
		MarkDirty(t, rect, true);
		const int prev_min = t->m_alpha_min;
		const int prev_max = t->m_alpha_max;
		Device().m_uploads.clear();
		t->Update();

		ExpectPicture(t, {rect}, pic);
		EXPECT_EQ(t->m_alpha_min, prev_min);
		EXPECT_EQ(t->m_alpha_max, prev_max);
	}

	INSTANTIATE_TEST_SUITE_P(Scales, GSTargetUpdate, ::testing::Values(1.0f, 2.0f));
} // namespace
