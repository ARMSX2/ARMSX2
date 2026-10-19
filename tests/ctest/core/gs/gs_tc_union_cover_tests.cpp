// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A render or depth target whose dirty rectangles together cover its valid area is retired.
//
// The texture cache already retires a target when ONE dirty rectangle covers it: every pixel of the
// target is then newer in local memory than on the GPU, so the GPU copy will never be used. The same
// is true when several rectangles cover it between them (Call of Duty 2 and 3 fill two bloom targets
// in 28 to 47 fragments a frame), and GSTextureCache::FullRectDirtyByUnion says so.
//
// DirtyRectsCover is pinned against a pixel-by-pixel model on random rectangle lists. The target
// tests then go through InvalidateVideoMem, the call the renderer makes, and check that the target is
// retired exactly when the rectangles written so far cover its valid area, and not otherwise: not by a
// gap of one pixel, not by rectangles that leave out a channel of the target's format, not by
// rectangles in another format or width, not by rectangles outside the valid area.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/HW/GSTextureCache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace GSHWDrawHarness;

namespace
{
	GSDirtyRect Rect(int x, int y, int z, int w, u32 psm = PSMCT32, u32 bw = 10, u32 mask = 0xf)
	{
		GSVector4i r(x, y, z, w);
		RGBAMask m;
		m._u32 = mask;
		return GSDirtyRect(r, psm, bw, m, false);
	}

	/// The model: every pixel of `area`, marked by the rectangles that count.
	bool CoverModel(const std::vector<GSDirtyRect>& list, u32 psm, u32 bw, u32 mask, const GSVector4i& area)
	{
		if (area.rempty())
			return false;
		for (int y = area.y; y < area.w; y++)
		{
			for (int x = area.x; x < area.z; x++)
			{
				bool hit = false;
				for (const GSDirtyRect& d : list)
				{
					if (d.psm != psm || std::max(d.bw, 1u) != std::max(bw, 1u) || (d.rgba._u32 & mask) != mask)
						continue;
					if (x >= d.r.x && x < d.r.z && y >= d.r.y && y < d.r.w)
					{
						hit = true;
						break;
					}
				}
				if (!hit)
					return false;
			}
		}
		return true;
	}

	GSDirtyRectList AsList(const std::vector<GSDirtyRect>& v)
	{
		GSDirtyRectList l;
		for (const GSDirtyRect& d : v)
			l.push_back(d);
		return l;
	}

	bool Covers(const std::vector<GSDirtyRect>& v, u32 psm, u32 bw, u32 mask, const GSVector4i& area)
	{
		return GSTextureCache::DirtyRectsCover(AsList(v), psm, bw, mask, area);
	}

	/// A pinwheel of four rectangles round a fifth, tiling `area` with no two of them able to merge into one:
	/// no two share both edges on an axis.
	std::vector<GSDirtyRect> Pinwheel(int x0, int y0, int x1, int y1, u32 psm = PSMCT32, u32 bw = 10, u32 mask = 0xf)
	{
		const int w = x1 - x0, h = y1 - y0;
		const int xa = x0 + w * 3 / 8, xb = x0 + w * 5 / 8, ya = y0 + h * 4 / 9, yb = y0 + h * 2 / 3;
		return {
			Rect(x0, y0, xb, ya, psm, bw, mask), // top left, wide
			Rect(xb, y0, x1, yb, psm, bw, mask), // top right, tall
			Rect(xa, yb, x1, y1, psm, bw, mask), // bottom right, wide
			Rect(x0, ya, xa, y1, psm, bw, mask), // bottom left, tall
			Rect(xa, ya, xb, yb, psm, bw, mask), // the middle
		};
	}

	/// The answer is the same with a pixel carried over from some other list, whether that pixel is in the area, outside
	/// it, or covered by this list, and a pixel it replaces is in the area and in none of the counting rectangles.
	void CheckWithHoles(const std::vector<GSDirtyRect>& list, u32 psm, u32 bw, u32 mask, const GSVector4i& area, bool want, int iter,
		std::mt19937& rng)
	{
		const GSDirtyRectList l = AsList(list);
		for (int k = 0; k < 3; k++)
		{
			GSVector2i hole(area.x - 2 + static_cast<int>(rng() % (area.z - area.x + 4)), area.y - 2 + static_cast<int>(rng() % (area.w - area.y + 4)));
			const GSVector2i seeded = hole;
			ASSERT_EQ(GSTextureCache::DirtyRectsCover(l, psm, bw, mask, area, &hole), want) << "iteration " << iter << " seeded hole";
			if (want)
				continue;
			// Asked again with what it left, the answer holds, and a pixel it replaced is truly uncovered.
			if (hole != seeded && hole.x >= area.x && hole.x < area.z && hole.y >= area.y && hole.y < area.w)
			{
				const GSVector4i one(hole.x, hole.y, hole.x + 1, hole.y + 1);
				for (const GSDirtyRect& d : list)
				{
					const bool counts = d.psm == psm && std::max(d.bw, 1u) == std::max(bw, 1u) && (d.rgba._u32 & mask) == mask;
					ASSERT_FALSE(counts && d.r.rintersect(one).eq(one)) << "iteration " << iter << " hole " << hole.x << "," << hole.y;
				}
			}
			ASSERT_FALSE(GSTextureCache::DirtyRectsCover(l, psm, bw, mask, area, &hole)) << "iteration " << iter;
		}
	}
} // namespace

TEST(GSTcUnionCover, OneRectangleThatCoversIsACover)
{
	EXPECT_TRUE(Covers({Rect(0, 0, 256, 192)}, PSMCT32, 10, 0xf, GSVector4i(0, 0, 256, 192)));
	EXPECT_TRUE(Covers({Rect(-8, -8, 300, 200)}, PSMCT32, 10, 0xf, GSVector4i(0, 0, 256, 192)));
	EXPECT_FALSE(Covers({Rect(0, 0, 256, 191)}, PSMCT32, 10, 0xf, GSVector4i(0, 0, 256, 192)));
	EXPECT_FALSE(Covers({}, PSMCT32, 10, 0xf, GSVector4i(0, 0, 256, 192)));
	EXPECT_FALSE(Covers({Rect(0, 0, 256, 192)}, PSMCT32, 10, 0xf, GSVector4i(0, 0, 0, 0)));
}

TEST(GSTcUnionCover, FragmentsThatTileTheAreaCover)
{
	const GSVector4i area(0, 0, 256, 192);
	EXPECT_TRUE(Covers(Pinwheel(0, 0, 256, 192), PSMCT32, 10, 0xf, area));

	// Strips that overlap each other, 28 of them, in an order that is not the order of their position.
	std::vector<GSDirtyRect> strips;
	for (int i = 0; i < 28; i++)
	{
		const int y0 = (i * 13) % 28 * 7;
		strips.push_back(Rect(0, y0, 256, std::min(y0 + 14, 192)));
	}
	EXPECT_TRUE(Covers(strips, PSMCT32, 10, 0xf, area));
}

TEST(GSTcUnionCover, AGapOfOnePixelIsNotACover)
{
	const GSVector4i area(0, 0, 256, 192);
	// The pinwheel with its middle one column narrower, and with it one row shorter.
	std::vector<GSDirtyRect> p = Pinwheel(0, 0, 256, 192);
	p[4].r.z -= 1;
	EXPECT_FALSE(Covers(p, PSMCT32, 10, 0xf, area));
	p = Pinwheel(0, 0, 256, 192);
	p[4].r.w -= 1;
	EXPECT_FALSE(Covers(p, PSMCT32, 10, 0xf, area));

	// One pixel missing from the corner of the area, from the middle of an edge, and from the middle.
	for (const GSVector2i hole : {GSVector2i(255, 191), GSVector2i(0, 0), GSVector2i(128, 0), GSVector2i(255, 96), GSVector2i(100, 100)})
	{
		std::vector<GSDirtyRect> l;
		// Four rectangles round a hole of one pixel.
		l.push_back(Rect(0, 0, 256, hole.y));
		l.push_back(Rect(0, hole.y + 1, 256, 192));
		l.push_back(Rect(0, hole.y, hole.x, hole.y + 1));
		l.push_back(Rect(hole.x + 1, hole.y, 256, hole.y + 1));
		EXPECT_FALSE(Covers(l, PSMCT32, 10, 0xf, area)) << hole.x << "," << hole.y;
		l.push_back(Rect(hole.x, hole.y, hole.x + 1, hole.y + 1));
		EXPECT_TRUE(Covers(l, PSMCT32, 10, 0xf, area)) << hole.x << "," << hole.y;
	}
}

TEST(GSTcUnionCover, OnlyRectanglesThatNameEveryChannelCount)
{
	const GSVector4i area(0, 0, 256, 192);
	// An RGBA target: rectangles that dirty RGB only, or alpha only, leave a channel on the GPU newer than memory.
	std::vector<GSDirtyRect> p = Pinwheel(0, 0, 256, 192);
	EXPECT_TRUE(Covers(p, PSMCT32, 10, 0xf, area));
	p[2].rgba._u32 = 0x7;
	EXPECT_FALSE(Covers(p, PSMCT32, 10, 0xf, area));
	p[2].rgba._u32 = 0x8;
	EXPECT_FALSE(Covers(p, PSMCT32, 10, 0xf, area));
	// The same rectangles and the two halves of the channels do not add up either: the test is per rectangle.
	std::vector<GSDirtyRect> halves = {Rect(0, 0, 256, 192, PSMCT32, 10, 0x7), Rect(0, 0, 256, 192, PSMCT32, 10, 0x8)};
	EXPECT_FALSE(Covers(halves, PSMCT32, 10, 0xf, area));
	// An RGB target needs only RGB.
	EXPECT_TRUE(Covers(Pinwheel(0, 0, 256, 192, PSMCT24, 10, 0x7), PSMCT24, 10, 0x7, area));
	// A rectangle that names more than the mask is fine.
	EXPECT_TRUE(Covers(Pinwheel(0, 0, 256, 192, PSMCT24, 10, 0xf), PSMCT24, 10, 0x7, area));
}

TEST(GSTcUnionCover, OnlyRectanglesInTheTargetsFormatAndWidthCount)
{
	const GSVector4i area(0, 0, 256, 192);
	std::vector<GSDirtyRect> p = Pinwheel(0, 0, 256, 192);
	p[0].psm = PSMCT16;
	EXPECT_FALSE(Covers(p, PSMCT32, 10, 0xf, area));
	p = Pinwheel(0, 0, 256, 192);
	p[3].bw = 4;
	EXPECT_FALSE(Covers(p, PSMCT32, 10, 0xf, area));
	// A width of 0 and of 1 are the same layout.
	p = Pinwheel(0, 0, 256, 192, PSMCT32, 1);
	EXPECT_TRUE(Covers(p, PSMCT32, 0, 0xf, area));
	p = Pinwheel(0, 0, 256, 192, PSMCT32, 0);
	EXPECT_TRUE(Covers(p, PSMCT32, 1, 0xf, area));
}

// The area is the valid rectangle, not the target's size: rectangles that cover it and leave the rest of
// the texture alone are a cover, and what lies outside it is not needed.
TEST(GSTcUnionCover, TheAreaIsTheValidRectangleNotTheTexture)
{
	const GSVector4i valid(0, 0, 256, 192);
	// Pieces that reach outside, none of which lies wholly inside.
	std::vector<GSDirtyRect> l = {Rect(-64, -64, 100, 300), Rect(100, -10, 400, 100), Rect(90, 100, 300, 500)};
	EXPECT_TRUE(Covers(l, PSMCT32, 10, 0xf, valid));
	// The same pieces over the whole 256x256 texture leave its last 64 rows alone only if the list stops short.
	l = {Rect(0, 0, 256, 100), Rect(0, 100, 256, 192)};
	EXPECT_TRUE(Covers(l, PSMCT32, 10, 0xf, valid));
	EXPECT_FALSE(Covers(l, PSMCT32, 10, 0xf, GSVector4i(0, 0, 256, 256)));
	// Pieces wholly outside the valid area count for nothing.
	l = {Rect(0, 192, 256, 256), Rect(256, 0, 512, 256)};
	EXPECT_FALSE(Covers(l, PSMCT32, 10, 0xf, valid));
}

// More rectangles than the sweep holds: the target stays, which is what it did before this existed.
TEST(GSTcUnionCover, ALongerListThanTheSweepHoldsIsNotACover)
{
	std::vector<GSDirtyRect> l;
	for (int i = 0; i < 129; i++)
		l.push_back(Rect(i, 0, i + 1, 4));
	// 129 columns, each its own rectangle, covering 129x4.
	EXPECT_FALSE(Covers(l, PSMCT32, 10, 0xf, GSVector4i(0, 0, 129, 4)));
	l.pop_back();
	EXPECT_TRUE(Covers(l, PSMCT32, 10, 0xf, GSVector4i(0, 0, 128, 4)));
}

// Against the pixel model, on lists that sit close to the edge between covering and not.
TEST(GSTcUnionCover, MatchesThePixelModelOnRandomLists)
{
	std::mt19937 rng(2026);
	int covered = 0, uncovered = 0;
	for (int iter = 0; iter < 6000; iter++)
	{
		const int W = 6 + static_cast<int>(rng() % 26);
		const int H = 6 + static_cast<int>(rng() % 22);
		const GSVector4i area(static_cast<int>(rng() % 3), static_cast<int>(rng() % 3), W, H);
		const u32 mask = (rng() & 1) ? 0xf : 0x7;
		std::vector<GSDirtyRect> list;

		const int style = static_cast<int>(rng() % 3);
		if (style == 0)
		{
			// Rectangles at random.
			const int n = 1 + static_cast<int>(rng() % 14);
			for (int i = 0; i < n; i++)
			{
				const int x = static_cast<int>(rng() % W), y = static_cast<int>(rng() % H);
				list.push_back(Rect(x - static_cast<int>(rng() % 3), y - static_cast<int>(rng() % 3),
					x + 1 + static_cast<int>(rng() % (W / 2 + 1)), y + 1 + static_cast<int>(rng() % (H / 2 + 1))));
			}
		}
		else
		{
			// A tiling by a random guillotine cut, then each tile grown or shrunk by a pixel now and then, so
			// that a list is a cover or one pixel short of it.
			std::vector<GSVector4i> tiles = {area};
			const int cuts = 1 + static_cast<int>(rng() % 10);
			for (int c = 0; c < cuts; c++)
			{
				const size_t k = rng() % tiles.size();
				const GSVector4i t = tiles[k];
				if (rng() & 1)
				{
					if (t.z - t.x < 2)
						continue;
					const int cut = t.x + 1 + static_cast<int>(rng() % (t.z - t.x - 1));
					tiles[k] = GSVector4i(t.x, t.y, cut, t.w);
					tiles.push_back(GSVector4i(cut, t.y, t.z, t.w));
				}
				else
				{
					if (t.w - t.y < 2)
						continue;
					const int cut = t.y + 1 + static_cast<int>(rng() % (t.w - t.y - 1));
					tiles[k] = GSVector4i(t.x, t.y, t.z, cut);
					tiles.push_back(GSVector4i(t.x, cut, t.z, t.w));
				}
			}
			for (GSVector4i t : tiles)
			{
				if (style == 2 && rng() % 5 == 0)
				{
					switch (rng() % 4)
					{
						case 0: t.x++; break;
						case 1: t.y++; break;
						case 2: t.z--; break;
						default: t.w--; break;
					}
				}
				if (t.rempty())
					continue;
				list.push_back(Rect(t.x, t.y, t.z, t.w));
			}
			std::shuffle(list.begin(), list.end(), rng);
		}

		// Some rectangles that do not count.
		const int noise = static_cast<int>(rng() % 3);
		for (int i = 0; i < noise; i++)
		{
			GSDirtyRect d = Rect(0, 0, W + 4, H + 4);
			switch (rng() % 3)
			{
				case 0: d.psm = PSMCT16; break;
				case 1: d.bw = 3; break;
				default: d.rgba._u32 = 0x8; break;
			}
			list.insert(list.begin() + static_cast<long>(rng() % (list.size() + 1)), d);
		}

		const bool want = CoverModel(list, PSMCT32, 10, mask, area);
		ASSERT_EQ(Covers(list, PSMCT32, 10, mask, area), want) << "iteration " << iter;
		CheckWithHoles(list, PSMCT32, 10, mask, area, want, iter, rng);
		(want ? covered : uncovered)++;
	}
	// The generator must reach both sides, or the comparison proves nothing.
	EXPECT_GT(covered, 600);
	EXPECT_GT(uncovered, 600);
}

// The same, on the 8 pixel grid the texture cache's rectangles usually sit on, where the bitmap answers: rows of
// one word and of several (areas wider than 512 pixels), exact covers and covers one cell short, with an edge
// now and then moved off the grid so that the sweep answers instead.
TEST(GSTcUnionCover, MatchesThePixelModelOnTheEightPixelGrid)
{
	std::mt19937 rng(8);
	int covered = 0, uncovered = 0;
	for (int iter = 0; iter < 700; iter++)
	{
		const int cells_x = 1 + static_cast<int>(rng() % ((iter % 3 == 0) ? 150 : 40));
		const int cells_y = 1 + static_cast<int>(rng() % 6);
		const GSVector4i area(8 * static_cast<int>(rng() % 3), 8 * static_cast<int>(rng() % 3), 0, 0);
		const GSVector4i full(area.x, area.y, area.x + 8 * cells_x, area.y + 8 * cells_y);
		std::vector<GSDirtyRect> list;

		// A guillotine tiling in cells, each tile dropped or shrunk by a cell now and then.
		std::vector<GSVector4i> tiles = {full};
		const int cuts = 1 + static_cast<int>(rng() % 12);
		for (int c = 0; c < cuts; c++)
		{
			const size_t k = rng() % tiles.size();
			const GSVector4i t = tiles[k];
			if (rng() & 1)
			{
				if (t.z - t.x < 16)
					continue;
				const int cut = t.x + 8 * (1 + static_cast<int>(rng() % ((t.z - t.x) / 8 - 1)));
				tiles[k] = GSVector4i(t.x, t.y, cut, t.w);
				tiles.push_back(GSVector4i(cut, t.y, t.z, t.w));
			}
			else
			{
				if (t.w - t.y < 16)
					continue;
				const int cut = t.y + 8 * (1 + static_cast<int>(rng() % ((t.w - t.y) / 8 - 1)));
				tiles[k] = GSVector4i(t.x, t.y, t.z, cut);
				tiles.push_back(GSVector4i(t.x, cut, t.z, t.w));
			}
		}
		for (GSVector4i t : tiles)
		{
			const int what = static_cast<int>(rng() % 12);
			if (what == 0)
				t.z = std::max(t.z - 8, t.x);
			else if (what == 1)
				t.y += 8;
			else if (what == 2)
				t.x += 1; // off the grid
			else if (what == 3)
				t.w -= 1;
			if (!t.rempty())
				list.push_back(Rect(t.x, t.y, t.z, t.w));
		}
		std::shuffle(list.begin(), list.end(), rng);

		const bool want = CoverModel(list, PSMCT32, 10, 0xf, full);
		ASSERT_EQ(Covers(list, PSMCT32, 10, 0xf, full), want) << "iteration " << iter;
		CheckWithHoles(list, PSMCT32, 10, 0xf, full, want, iter, rng);
		(want ? covered : uncovered)++;
	}
	EXPECT_GT(covered, 100);
	EXPECT_GT(uncovered, 100);
}

// What one test of a 47-rectangle list costs, for the record. Run with --gtest_also_run_disabled_tests.
TEST(GSTcUnionCover, DISABLED_CostOfOneSweep)
{
	// 47 fragments that overlap and miss 128 pixels of 256x192: the shape of CoD2's target 279. Once with
	// edges on the 8 pixel grid (the bitmap), once with the first rectangle's edge moved off it (the sweep).
	for (const bool on_grid : {true, false})
	{
		std::vector<GSDirtyRect> l;
		std::mt19937 rng(7);
		for (int i = 0; i < 46; i++)
		{
			const int y = static_cast<int>(rng() % 24) * 8, x = static_cast<int>(rng() % 8) * 8;
			l.push_back(Rect(x, y, std::min(256, x + 64 + static_cast<int>(rng() % 24) * 8), std::min(192, y + 16 + static_cast<int>(rng() % 8) * 8)));
		}
		l.push_back(Rect(0, 0, 256, 192));
		l.back().r.z = 255;
		if (!on_grid)
			l[0].r.x += 1;
		const GSDirtyRectList list = AsList(l);
		const GSVector4i area(0, 0, 256, 192);
		constexpr int N = 200000;
		for (const bool with_hole : {false, true})
		{
			GSVector2i hole(-1, -1);
			int n_true = 0;
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < N; i++)
				n_true += GSTextureCache::DirtyRectsCover(list, PSMCT32, 10, 0xf, area, with_hole ? &hole : nullptr) ? 1 : 0;
			const auto t1 = std::chrono::steady_clock::now();
			std::printf("%s%s of %zu rectangles: %.1f ns (covers %d of %d)\n", on_grid ? "bitmap" : "sweep ",
				with_hole ? " + hole" : "       ", l.size(), std::chrono::duration<double, std::nano>(t1 - t0).count() / N, n_true, N);
		}
	}
}

// ---- the targets ----

namespace
{
	constexpr u32 kBP = 0;
	constexpr u32 kBW = 10;
	constexpr int kW = 640;
	constexpr int kH = 448;
	constexpr u32 kZBP = 0x100 << 5; // Environment() puts the depth buffer at page 0x100

	class GSTcUnionRetire : public Fixture
	{
	protected:
		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.UpscaleMultiplier = 1.0f;
			GSTextureCache::SetUnionRetire(true);
		}

		void TearDown() override
		{
			GSTextureCache::SetUnionRetire(true);
			Fixture::TearDown();
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
			GSTextureCache::Target* const t = g_texture_cache->GetExactTarget(kBP, kBW, GSTextureCache::RenderTarget, kBP);
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

		void Write(const GSDirtyRect& d) { Write(d.r); }
	};
} // namespace

// Fragments that tile the valid area retire the target at the write that completes them, and not before.
TEST_F(GSTcUnionRetire, FragmentsThatTileTheTargetRetireIt)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);
	ASSERT_TRUE(t->m_valid.eq(GSVector4i(0, 0, kW, kH)));

	const std::vector<GSDirtyRect> p = Pinwheel(0, 0, kW, kH);
	const GSTextureCache::UnionRetireStats before = GSTextureCache::GetUnionRetireStats();
	for (size_t i = 0; i + 1 < p.size(); i++)
	{
		Write(p[i]);
		ASSERT_EQ(Find(), t) << "retired early, after " << i + 1 << " of " << p.size();
	}
	ASSERT_EQ(t->m_dirty.size(), p.size() - 1) << "the fragments merged, so this does not test a union";
	Write(p.back());
	EXPECT_EQ(Find(), nullptr);
	EXPECT_EQ(GSTextureCache::GetUnionRetireStats().retired, before.retired + 1);
}

// The same writes with the retirement off leave the target, fully dirty, as it was before the change.
TEST_F(GSTcUnionRetire, OffLeavesTheFullyDirtyTargetAlone)
{
	BringUp();
	GSTextureCache::SetUnionRetire(false);
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);

	for (const GSDirtyRect& d : Pinwheel(0, 0, kW, kH))
		Write(d);
	ASSERT_EQ(Find(), t);
	EXPECT_EQ(t->m_dirty.size(), 5u);
	// One rectangle that covers still retires it, with the union off.
	Write(GSVector4i(0, 0, kW, kH));
	EXPECT_EQ(Find(), nullptr);
}

TEST_F(GSTcUnionRetire, AGapOfOnePixelKeepsTheTargetUntilItIsClosed)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);

	std::vector<GSDirtyRect> p = Pinwheel(0, 0, kW, kH);
	const GSVector4i middle = p[4].r;
	p[4].r.z -= 1;
	for (const GSDirtyRect& d : p)
		Write(d);
	ASSERT_EQ(Find(), t);
	EXPECT_FALSE(GSTextureCache::DirtyRectsCover(t->m_dirty, t->m_TEX0.PSM, t->m_TEX0.TBW, 0xf, t->m_valid));

	// The column that was left out.
	Write(GSVector4i(middle.z - 1, middle.y, middle.z, middle.w));
	EXPECT_EQ(Find(), nullptr);
}

// A rectangle that leaves out the alpha channel of an RGBA target does not make it stale.
TEST_F(GSTcUnionRetire, RectanglesThatLeaveOutAChannelDoNotCount)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);

	const std::vector<GSDirtyRect> p = Pinwheel(0, 0, kW, kH);
	for (size_t i = 0; i < p.size(); i++)
	{
		if (i == 2)
		{
			// The EE writes this one as 24-bit pixels: RGB only. (A 24-bit write of the frame at block 0.)
			Write(p[i].r, PSMCT24);
		}
		else
		{
			Write(p[i]);
		}
	}
	ASSERT_EQ(Find(), t);
	bool saw_rgb_only = false;
	for (const GSDirtyRect& d : t->m_dirty)
		saw_rgb_only |= d.rgba._u32 == 0x7;
	EXPECT_TRUE(saw_rgb_only) << "the 24-bit write did not leave a RGB-only rectangle, so this tests nothing";

	// The same rectangle written with all four channels completes the cover.
	Write(p[2]);
	EXPECT_EQ(Find(), nullptr);
}

// A 24-bit target has three channels, and rectangles of three channels cover it.
TEST_F(GSTcUnionRetire, ARgbTargetIsCoveredByRgbRectangles)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT24);
	ASSERT_NE(t, nullptr);
	ASSERT_EQ(t->m_TEX0.PSM, static_cast<u32>(PSMCT24));

	const std::vector<GSDirtyRect> p = Pinwheel(0, 0, kW, kH);
	for (size_t i = 0; i + 1 < p.size(); i++)
		Write(p[i].r, PSMCT24);
	ASSERT_EQ(Find(), t);
	ASSERT_GE(t->m_dirty.size(), 2u);
	Write(p.back().r, PSMCT24);
	EXPECT_EQ(Find(), nullptr);
}

// The valid area is what is covered. A target drawn only in its upper left has a texture larger than that.
TEST_F(GSTcUnionRetire, TheValidAreaIsWhatMustBeCovered)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);
	t->m_valid = GSVector4i(0, 0, 320, 224);
	ASSERT_GT(t->m_unscaled_size.x * t->m_unscaled_size.y, 320 * 224);

	for (const GSDirtyRect& d : Pinwheel(0, 0, 320, 224))
		Write(d);
	EXPECT_EQ(Find(), nullptr);
}

// Fragments that cover only part of the valid area, however many, do not retire it.
TEST_F(GSTcUnionRetire, FragmentsOfPartOfTheTargetKeepIt)
{
	BringUp();
	GSTextureCache::Target* const t = Scene(PSMCT32);
	ASSERT_NE(t, nullptr);

	for (const GSDirtyRect& d : Pinwheel(0, 0, kW, kH - 8))
		Write(d);
	EXPECT_EQ(Find(), t);
	EXPECT_EQ(t->m_dirty.size(), 5u);
}

// A depth target is retired the same way.
TEST_F(GSTcUnionRetire, ADepthTargetIsRetiredToo)
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
// on random writes, each case a fresh list on one target.
TEST_F(GSTcUnionRetire, RetiresExactlyWhenTheWritesCoverIt)
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
			// Edges of the area are taken often, so that covers happen at about the rate that gaps do.
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
		{
			// Back to a target nothing is dirty on, for the next case.
			Find()->m_dirty.clear();
		}
	}
	EXPECT_GT(retired_cases, 20);
	EXPECT_GT(kept_cases, 20);
}
