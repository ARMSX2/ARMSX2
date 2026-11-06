// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A target's dirt (GSTargetDirty): one byte of channels per block, plus the rectangles that are not on block edges.
//
// The store is pinned against a pixel model on random marks: the rectangles it shows hold, pixel by pixel, the
// channels the marks put there; its totals, its "covers" and its one-rectangle answers agree with what it shows.
// Then through InvalidateVideoMem, the call the renderer makes: a write marks exactly the target blocks whose
// memory it touched, in every pairing of formats the cache meets, and a target is retired exactly when the
// writes cover its valid area.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/HW/GSTextureCache.h"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

using namespace GSHWDrawHarness;

namespace
{
	GSDirtyRect Rect(int x, int y, int z, int w, u32 psm = PSMCT32, u32 bw = 10, u32 mask = 0xf, bool linear = false)
	{
		RGBAMask m;
		m._u32 = mask;
		return GSDirtyRect(GSVector4i(x, y, z, w), psm, bw, m, linear);
	}

	using PixelMap = std::map<std::pair<int, int>, u32>;

	/// The channels (and 0x10 for bilinear) each pixel of the rectangles holds.
	PixelMap PixelsOf(const std::vector<GSDirtyRect>& rects)
	{
		PixelMap m;
		for (const GSDirtyRect& d : rects)
			for (int y = d.r.y; y < d.r.w; y++)
				for (int x = d.r.x; x < d.r.z; x++)
					m[{x, y}] |= (d.rgba._u32 & 0xf) | (d.req_linear ? 0x10u : 0u);
		return m;
	}

	bool CoverModel(const PixelMap& m, u32 mask, const GSVector4i& area)
	{
		if (area.rempty())
			return false;
		for (int y = area.y; y < area.w; y++)
			for (int x = area.x; x < area.z; x++)
			{
				const auto it = m.find({x, y});
				if (it == m.end() || (it->second & mask) != mask)
					return false;
			}
		return true;
	}

	/// A rectangle on the 8-pixel grid, or (sometimes) one that is not.
	GSDirtyRect RandomRect(std::mt19937& rng, int W, int H, bool aligned)
	{
		const int g = aligned ? 8 : 1;
		const int x0 = static_cast<int>(rng() % (W / g)) * g, y0 = static_cast<int>(rng() % (H / g)) * g;
		const int x1 = x0 + g * (1 + static_cast<int>(rng() % ((W - x0) / g)));
		const int y1 = y0 + g * (1 + static_cast<int>(rng() % ((H - y0) / g)));
		static constexpr u32 masks[] = {0xf, 0xf, 0xf, 0x7, 0x8};
		return Rect(x0, y0, x1, y1, PSMCT32, 10, masks[rng() % 5], rng() % 6 == 0);
	}

	/// The format table (GSLocalMemory::m_psm) is filled in by the first GSLocalMemory made.
	class GSTargetDirtyStore : public ::testing::Test
	{
	protected:
		static void SetUpTestSuite()
		{
			if (!s_mem)
				s_mem = std::make_unique<GSLocalMemory>();
		}
		static inline std::unique_ptr<GSLocalMemory> s_mem;
	};
} // namespace

TEST_F(GSTargetDirtyStore, WhatItShowsIsWhatWasMarked)
{
	std::mt19937 rng(17);
	::GSTargetDirty dirty;
	for (int iter = 0; iter < 300; iter++)
	{
		const int W = 64 + static_cast<int>(rng() % 5) * 64, H = 32 + static_cast<int>(rng() % 5) * 32;
		std::vector<GSDirtyRect> marks;
		const int n = 1 + static_cast<int>(rng() % 14);
		for (int i = 0; i < n; i++)
		{
			marks.push_back(RandomRect(rng, W, H, rng() % 4 != 0));
			dirty.Mark(marks.back(), PSMCT32, 10);
		}

		const std::vector<GSDirtyRect> view = dirty.View();
		// Pixels on block-aligned marks are merged by OR; loose marks keep their own rectangle, so compare the
		// union of what was marked with the union of what is shown.
		ASSERT_EQ(PixelsOf(view), PixelsOf(marks)) << iter;
		for (const GSDirtyRect& d : view)
			ASSERT_TRUE(d.psm == PSMCT32 && d.bw == 10) << iter;

		ASSERT_EQ(dirty.size(), view.size());
		GSDirtyRect single;
		ASSERT_EQ(dirty.Single(&single), view.size() == 1) << iter;
		if (view.size() == 1)
			ASSERT_TRUE(single.r.eq(view[0].r) && single.rgba._u32 == view[0].rgba._u32) << iter;
		ASSERT_EQ(dirty.MoreThanOne(), view.size() > 1) << iter;
		ASSERT_EQ(dirty.FrontRGBA()._u32, view[0].rgba._u32) << iter;

		u32 channels = 0;
		for (const GSDirtyRect& d : view)
			channels |= d.rgba._u32;
		ASSERT_EQ(dirty.GetDirtyChannels(), channels) << iter;

		for (const u32 psm : {static_cast<u32>(PSMCT32), static_cast<u32>(PSMCT16), static_cast<u32>(PSMT8)})
		{
			GIFRegTEX0 tex0 = {};
			tex0.PSM = psm;
			GSVector4i want = GSVector4i::cxpr(INT_MAX, INT_MAX, 0, 0);
			for (const GSDirtyRect& d : view)
				want = want.runion(d.GetDirtyRect(tex0, true));
			want = want.ralign<Align_Outside>(GSLocalMemory::m_psm[psm].bs).rintersect(GSVector4i(0, 0, 2048, 2048));
			ASSERT_TRUE(dirty.GetTotalRect(tex0, GSVector2i(2048, 2048)).eq(want)) << iter << " psm " << psm;
		}

		const PixelMap model = PixelsOf(marks);
		for (int k = 0; k < 4; k++)
		{
			const GSVector4i area = (k == 0) ? GSVector4i(0, 0, W, H) : RandomRect(rng, W, H, k & 1).r;
			for (const u32 mask : {0xfu, 0x7u, 0x8u})
				ASSERT_EQ(dirty.Covers(area, PSMCT32, mask), CoverModel(model, mask, area)) << iter << " area " << k << " mask " << mask;
		}

		// Rewriting the rectangles in place keeps what they hold.
		dirty.EditRects([](std::vector<GSDirtyRect>&) {}, PSMCT32, 10);
		ASSERT_EQ(PixelsOf(dirty.View()), PixelsOf(marks)) << iter;

		dirty.clear();
		ASSERT_TRUE(dirty.empty());
		ASSERT_TRUE(dirty.View().empty());
	}
}

TEST_F(GSTargetDirtyStore, OneMarkOnBlockEdgesIsOneRectangle)
{
	::GSTargetDirty dirty;
	dirty.Mark(Rect(8, 16, 64, 48), PSMCT32, 10);
	GSDirtyRect single;
	ASSERT_TRUE(dirty.Single(&single));
	EXPECT_TRUE(single.r.eq(GSVector4i(8, 16, 64, 48)));
	// The same blocks written again change nothing.
	EXPECT_FALSE(dirty.Mark(Rect(16, 16, 32, 32), PSMCT32, 10));
	// A rectangle inside a block stays a rectangle of its own.
	EXPECT_TRUE(dirty.Mark(Rect(100, 100, 103, 101), PSMCT32, 10));
	ASSERT_EQ(dirty.size(), 2u);
	EXPECT_TRUE(dirty[1].r.eq(GSVector4i(100, 100, 103, 101)));
}

TEST_F(GSTargetDirtyStore, RectsCoverMatchesThePixelModel)
{
	std::mt19937 rng(23);
	for (int iter = 0; iter < 2000; iter++)
	{
		const int W = 16 + static_cast<int>(rng() % 64), H = 16 + static_cast<int>(rng() % 48);
		std::vector<GSDirtyRect> list;
		const int n = 1 + static_cast<int>(rng() % 10);
		for (int i = 0; i < n; i++)
		{
			GSDirtyRect d = RandomRect(rng, W, H, false);
			if (rng() % 8 == 0)
				d.psm = PSMCT16;
			list.push_back(d);
		}
		std::vector<GSDirtyRect> counted;
		for (const GSDirtyRect& d : list)
			if (d.psm == PSMCT32)
				counted.push_back(d);
		const PixelMap model = PixelsOf(counted);
		for (const u32 mask : {0xfu, 0x8u})
			ASSERT_EQ(::GSTargetDirty::RectsCover(list, PSMCT32, mask, GSVector4i(0, 0, W, H)), CoverModel(model, mask, GSVector4i(0, 0, W, H))) << iter;
	}
}

namespace
{
	constexpr u32 kBP = 0;
	constexpr u32 kBW = 10;
	constexpr int kW = 640;
	constexpr int kH = 448;
	constexpr u32 kZBP = 0x100 << 5; // Environment() puts the depth buffer at page 0x100

	/// A pinwheel of four rectangles round a fifth, tiling `area` with no two of them able to merge into one.
	std::vector<GSDirtyRect> Pinwheel(int x0, int y0, int x1, int y1)
	{
		const int w = x1 - x0, h = y1 - y0;
		const int xa = x0 + w * 3 / 8, xb = x0 + w * 5 / 8, ya = y0 + h * 4 / 9, yb = y0 + h * 2 / 3;
		return {Rect(x0, y0, xb, ya), Rect(xb, y0, x1, yb), Rect(xa, yb, x1, y1), Rect(x0, ya, xa, y1), Rect(xa, ya, xb, yb)};
	}

	class GSTargetDirtyCache : public Fixture
	{
	protected:
		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.UpscaleMultiplier = 1.0f;
		}

		/// A render target of `psm` at block 0, 640 wide, valid over (0, 0, w, h), with nothing dirty.
		GSTextureCache::Target* Scene(u32 psm, int w = kW, int h = kH)
		{
			Packet p;
			Environment(p, kBP / 32, psm);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(w << 4, h << 4, 0, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
			GSTextureCache::Target* const t = Find();
			if (t)
				t->m_dirty.clear();
			return t;
		}

		GSTextureCache::Target* Find() { return g_texture_cache->GetExactTarget(kBP, kBW, GSTextureCache::RenderTarget, kBP); }

		/// What the renderer calls when the EE or a software draw writes `r` of the frame at block 0.
		void Write(const GSVector4i& r, u32 psm = PSMCT32)
		{
			g_texture_cache->InvalidateVideoMem(g_gs_renderer->m_mem.GetOffset(kBP, kBW, psm), r);
		}
	};

	/// The target blocks inside `valid` whose memory a write of `write` in (sbp, sbw, spsm) touches.
	std::set<std::pair<int, int>> TouchedBlocks(u32 tbp, u32 tbw, u32 tpsm, const GSVector4i& valid, u32 sbp, u32 sbw, u32 spsm, const GSVector4i& write)
	{
		const GSOffset src = GSOffset(GSLocalMemory::m_psm[spsm].info, sbp, sbw, spsm);
		std::set<u32> blocks;
		for (int y = write.y; y < write.w; y++)
			for (int x = write.x; x < write.z; x++)
				blocks.insert(src.bn(x, y));
		const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[tpsm];
		const GSOffset dst = GSOffset(p.info, tbp, tbw, tpsm);
		const GSVector4i cover = valid.ralign<Align_Outside>(p.bs);
		std::set<std::pair<int, int>> out;
		for (int y = cover.y; y < cover.w; y += p.bs.y)
			for (int x = cover.x; x < cover.z; x += p.bs.x)
				if (blocks.contains(dst.bn(x, y)))
					out.emplace(x, y);
		return out;
	}

	std::set<std::pair<int, int>> DirtyBlocks(const GSTextureCache::Target* t)
	{
		const GSVector2i& bs = GSLocalMemory::m_psm[t->m_TEX0.PSM].bs;
		std::set<std::pair<int, int>> out;
		for (const GSDirtyRect& d : t->m_dirty)
		{
			const GSVector4i r = d.GetDirtyRect(t->m_TEX0, false).ralign<Align_Outside>(bs);
			for (int y = r.y; y < r.w; y += bs.y)
				for (int x = r.x; x < r.z; x += bs.x)
					out.emplace(x, y);
		}
		return out;
	}
} // namespace

// A write that overlaps a target's memory marks the blocks of the target whose memory it touched, and only those:
// every target format against every write format the cache matches up, bases on and off pages, widths that fold.
TEST_F(GSTargetDirtyCache, AWriteMarksTheBlocksWhoseMemoryItTouched)
{
	BringUp();
	std::mt19937 rng(29);
	const u32 targets[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
	const u32 writes[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMZ32, PSMZ16};
	int checked = 0, nonempty = 0;
	for (int iter = 0; iter < 400; iter++)
	{
		const u32 tpsm = targets[rng() % std::size(targets)];
		const u32 spsm = writes[rng() % std::size(writes)];
		if (!GSUtil::HasSharedBits(spsm, tpsm))
			continue;

		GIFRegTEX0 tex0 = {};
		tex0.TBP0 = 0x1000 + static_cast<u32>(rng() % 64) * 32 + ((rng() % 3 == 0) ? static_cast<u32>(rng() % 32) : 0);
		tex0.TBW = 2 + static_cast<u32>(rng() % 6);
		tex0.PSM = tpsm;
		const int type = GSLocalMemory::m_psm[tpsm].depth ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget;
		GSTextureCache::Target* const t = g_texture_cache->CreateTarget(tex0, GSVector2i(512, 256), GSVector2i(512, 256), 1.0f, type, true, 0, false, false, false);
		ASSERT_NE(t, nullptr);
		// Wider than the buffer now and then, so rows fold onto the ones below.
		const int vw = static_cast<int>(tex0.TBW) * 64 + ((rng() % 4 == 0) ? 64 : 0);
		t->m_valid = GSVector4i(static_cast<int>(rng() % 16), static_cast<int>(rng() % 16), std::min(vw, 512), 128 + static_cast<int>(rng() % 128));
		t->m_dirty.clear();

		// Several writes, often over the same blocks again (the case the address mirror skips).
		std::set<std::pair<int, int>> want;
		const u32 sbw = 1 + static_cast<u32>(rng() % 8);
		const u32 sbp = (tex0.TBP0 & ~31u) + static_cast<u32>(rng() % 8) * 32 + 1 + static_cast<u32>(rng() % 31);
		const int writes_n = 1 + static_cast<int>(rng() % 4);
		GSVector4i write = GSVector4i::zero();
		for (int k = 0; k < writes_n; k++)
		{
			if (k == 0 || rng() % 2)
			{
				const int x = static_cast<int>(rng() % 128), y = static_cast<int>(rng() % 64);
				// Now and then wide and tall enough to hold whole pages of the write's format.
				const bool big = rng() % 3 == 0;
				write = GSVector4i(x, y, x + 1 + static_cast<int>(rng() % (big ? 384 : 64)), y + 1 + static_cast<int>(rng() % (big ? 192 : 32)));
			}
			g_texture_cache->InvalidateVideoMem(g_gs_renderer->m_mem.GetOffset(sbp, sbw, spsm), write);
			const auto touched = TouchedBlocks(tex0.TBP0, tex0.TBW, tpsm, t->m_valid, sbp, sbw, spsm, write);
			want.insert(touched.begin(), touched.end());
			if (g_texture_cache->GetExactTarget(tex0.TBP0, tex0.TBW, type, tex0.TBP0) != t)
				break;
		}
		// A write that dirties all of the target retires it.
		const bool kept = g_texture_cache->GetExactTarget(tex0.TBP0, tex0.TBW, type, tex0.TBP0) == t;
		if (kept)
		{
			ASSERT_EQ(DirtyBlocks(t), want) << "iteration " << iter << " target " << GSUtil::GetPSMName(tpsm) << " write " << GSUtil::GetPSMName(spsm);
			if (!t->m_dirty.empty())
				ASSERT_EQ(t->m_dirty.GetDirtyChannels(), GSUtil::GetChannelMask(spsm)) << iter;
			checked++;
			nonempty += !want.empty();
		}
		g_texture_cache->RemoveAll(true, true, true);
	}
	EXPECT_GT(checked, 100);
	EXPECT_GT(nonempty, 50);
}

// Fragments that tile the valid area retire the target at the write that completes them, and not before.
TEST_F(GSTargetDirtyCache, FragmentsThatTileTheTargetRetireIt)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);
	ASSERT_TRUE(t->m_valid.eq(GSVector4i(0, 0, kW, kH)));

	const std::vector<GSDirtyRect> p = Pinwheel(0, 0, kW, kH);
	for (size_t i = 0; i + 1 < p.size(); i++)
	{
		Write(p[i].r);
		ASSERT_EQ(Find(), t) << "retired early, after " << i + 1 << " of " << p.size();
	}
	Write(p.back().r);
	EXPECT_EQ(Find(), nullptr);
}

TEST_F(GSTargetDirtyCache, AGapOfOnePixelKeepsTheTargetUntilItIsClosed)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);

	std::vector<GSDirtyRect> p = Pinwheel(0, 0, kW, kH);
	const GSVector4i middle = p[4].r;
	p[4].r.z -= 1;
	for (const GSDirtyRect& d : p)
		Write(d.r);
	ASSERT_EQ(Find(), t);
	EXPECT_FALSE(t->m_dirty.Covers(t->m_valid, PSMCT32, 0xf));

	Write(GSVector4i(middle.z - 1, middle.y, middle.z, middle.w));
	EXPECT_EQ(Find(), nullptr);
}

// A rectangle that leaves out the alpha channel of an RGBA target does not make it stale.
TEST_F(GSTargetDirtyCache, RectanglesThatLeaveOutAChannelDoNotCount)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);

	const std::vector<GSDirtyRect> p = Pinwheel(0, 0, kW, kH);
	for (size_t i = 0; i < p.size(); i++)
		Write(p[i].r, i == 2 ? PSMCT24 : PSMCT32);
	ASSERT_EQ(Find(), t);
	bool saw_rgb_only = false;
	for (const GSDirtyRect& d : t->m_dirty)
		saw_rgb_only |= d.rgba._u32 == 0x7;
	EXPECT_TRUE(saw_rgb_only) << "the 24-bit write did not leave an RGB-only rectangle, so this tests nothing";

	Write(p[2].r);
	EXPECT_EQ(Find(), nullptr);
}

// A 24-bit target has three channels, and rectangles of three channels cover it.
TEST_F(GSTargetDirtyCache, ARgbTargetIsCoveredByRgbRectangles)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT24);
	ASSERT_NE(t, nullptr);
	ASSERT_EQ(t->m_TEX0.PSM, static_cast<u32>(PSMCT24));

	const std::vector<GSDirtyRect> p = Pinwheel(0, 0, kW, kH);
	for (size_t i = 0; i + 1 < p.size(); i++)
		Write(p[i].r, PSMCT24);
	ASSERT_EQ(Find(), t);
	Write(p.back().r, PSMCT24);
	EXPECT_EQ(Find(), nullptr);
}

// The valid area is what is covered. A target drawn only in its upper left has a texture larger than that.
TEST_F(GSTargetDirtyCache, TheValidAreaIsWhatMustBeCovered)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);
	t->m_valid = GSVector4i(0, 0, 320, 224);
	ASSERT_GT(t->m_unscaled_size.x * t->m_unscaled_size.y, 320 * 224);

	for (const GSDirtyRect& d : Pinwheel(0, 0, 320, 224))
		Write(d.r);
	EXPECT_EQ(Find(), nullptr);
}

// Fragments that cover only part of the valid area, however many, do not retire it.
TEST_F(GSTargetDirtyCache, FragmentsOfPartOfTheTargetKeepIt)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);

	for (const GSDirtyRect& d : Pinwheel(0, 0, kW, kH - 8))
		Write(d.r);
	EXPECT_EQ(Find(), t);
}

// A depth target is retired the same way.
TEST_F(GSTargetDirtyCache, ADepthTargetIsRetiredToo)
{
	BringUp();
	GIFRegTEX0 tex0 = {};
	tex0.TBP0 = kZBP;
	tex0.TBW = kBW;
	tex0.PSM = PSMZ32;
	GSTextureCache::Target* const z = g_texture_cache->CreateTarget(tex0, GSVector2i(kW, kH), GSVector2i(kW, kH), 1.0f,
		GSTextureCache::DepthStencil, true, 0, false, false);
	ASSERT_NE(z, nullptr);
	ASSERT_EQ(g_texture_cache->GetExactTarget(kZBP, kBW, GSTextureCache::DepthStencil, kZBP), z);
	z->m_dirty.clear();

	const std::vector<GSDirtyRect> pw = Pinwheel(0, 0, kW, kH);
	for (size_t i = 0; i < pw.size(); i++)
	{
		g_texture_cache->InvalidateVideoMem(g_gs_renderer->m_mem.GetOffset(kZBP, kBW, PSMZ32), pw[i].r);
		if (i + 1 < pw.size())
			ASSERT_EQ(g_texture_cache->GetExactTarget(kZBP, kBW, GSTextureCache::DepthStencil, kZBP), z) << i;
	}
	EXPECT_EQ(g_texture_cache->GetExactTarget(kZBP, kBW, GSTextureCache::DepthStencil, kZBP), nullptr);
}

// The target is retired when, and only when, the rectangles written cover its valid area; against the pixel model,
// on random writes, each case a fresh dirt on one target.
TEST_F(GSTargetDirtyCache, RetiresExactlyWhenTheWritesCoverIt)
{
	BringUp();
	std::mt19937 rng(11);
	int retired_cases = 0, kept_cases = 0;
	for (int iter = 0; iter < 160; iter++)
	{
		const int W = 64 + static_cast<int>(rng() % 5) * 64;
		const int H = 32 + static_cast<int>(rng() % 5) * 32;
		GSTextureCache::Target* t = Scene(PSMCT32);
		ASSERT_NE(t, nullptr) << iter;
		t->m_valid = GSVector4i(0, 0, W, H);

		std::vector<u8> model(static_cast<size_t>(W) * H, 0);
		const int steps = 2 + static_cast<int>(rng() % 40);
		const int grain = (rng() % 4 == 0) ? 1 : 8;
		bool gone = false;
		for (int s = 0; s < steps && !gone; s++)
		{
			const auto span = [&](int n) {
				const int cells = n / grain;
				int lo = (rng() % 3 == 0) ? 0 : static_cast<int>(rng() % cells);
				int hi = (rng() % 3 == 0) ? cells : lo + 1 + static_cast<int>(rng() % (cells - lo));
				return std::pair{lo * grain, hi * grain};
			};
			const auto xs = span(W);
			const auto ys = span(H);
			const GSVector4i r(xs.first, ys.first, xs.second, ys.second);
			for (int yy = r.y; yy < r.w; yy++)
				for (int xx = r.x; xx < r.z; xx++)
					model[static_cast<size_t>(yy) * W + xx] = 1;
			bool all = true;
			for (const u8 v : model)
				all &= (v != 0);

			Write(r);
			gone = (Find() == nullptr);
			ASSERT_EQ(gone, all) << "iteration " << iter << " step " << s;
		}
		(gone ? retired_cases : kept_cases)++;
		if (!gone)
			Find()->m_dirty.clear();
	}
	EXPECT_GT(retired_cases, 20);
	EXPECT_GT(kept_cases, 20);
}

// The target page index answers "may a target be in reach of this write" with no false no: on random targets
// moved and resized through Surface's setters and deleted, against the scan InvalidateVideoMem makes.
TEST_F(GSTargetDirtyCache, TheTargetIndexNeverMissesATargetInReach)
{
	BringUp();
	std::mt19937 rng(31);
	std::vector<GSTextureCache::Target*> live;
	int checks = 0, misses_wanted = 0;
	// Mostly short ranges, some wrapping past the top of memory, some anywhere.
	const auto end_for = [&](u32 tbp) {
		const u32 r = rng() % 8;
		return r == 0 ? static_cast<u32>(rng() % 16384) : (tbp + static_cast<u32>(rng() % 600)) % 16384;
	};
	for (int iter = 0; iter < 3000; iter++)
	{
		const u32 op = rng() % 10;
		if ((op < 3 && live.size() < 12) || live.empty())
		{
			GIFRegTEX0 tex0 = {};
			tex0.TBP0 = static_cast<u32>(rng() % 16384);
			tex0.TBW = 1 + static_cast<u32>(rng() % 10);
			tex0.PSM = PSMCT32;
			live.push_back(new GSTextureCache::Target(tex0, GSTextureCache::RenderTarget, GSVector2i(64, 64), 1.0f, nullptr));
			live.back()->SetEndBlock(end_for(tex0.TBP0));
		}
		else if (op < 5)
		{
			live[rng() % live.size()]->SetTBP0(static_cast<u32>(rng() % 16384));
		}
		else if (op < 7)
		{
			GSTextureCache::Target* t = live[rng() % live.size()];
			t->SetEndBlock(end_for(t->m_TEX0.TBP0));
		}
		else if (op < 8)
		{
			GIFRegTEX0 tex0 = {};
			tex0.TBP0 = static_cast<u32>(rng() % 16384);
			tex0.PSM = PSMCT32;
			live[rng() % live.size()]->SetTEX0(tex0);
		}
		else
		{
			const size_t k = rng() % live.size();
			delete live[k];
			live.erase(live.begin() + k);
		}

		for (int q = 0; q < 4; q++)
		{
			const u32 bp = static_cast<u32>(rng() % 16384);
			const u32 end_bp = (rng() % 4 == 0) ? bp : bp + static_cast<u32>(rng() % 4096);
			bool scan = false;
			for (const GSTextureCache::Target* t : live)
				scan |= !((bp < t->m_TEX0.TBP0 && end_bp < t->m_TEX0.TBP0) || bp > t->UnwrappedEndBlock());
			const bool index = GSTextureCache::TargetsMayReach(bp, end_bp);
			ASSERT_TRUE(index || !scan) << "iteration " << iter << ": a target is in reach of " << bp << ".." << end_bp << " and the index said none";
			checks++;
			misses_wanted += !scan;
		}
	}
	for (GSTextureCache::Target* t : live)
		delete t;
	// With nothing live the index is empty again.
	EXPECT_FALSE(GSTextureCache::TargetsMayReach(0, 2 * GS_MAX_BLOCKS - 1));
	EXPECT_GT(checks, 10000);
	EXPECT_GT(misses_wanted, 200) << "too few writes reached no target for the test to mean anything";
}
