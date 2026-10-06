// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the parts of the texture upscaler that have no GS dependencies
// (GS/Renderers/HW/GSTextureUpscaleSupport.h): the bounded newest-first job queue, the scale a
// texture is upscaled by, the count of guest mip levels an upscaled texture can take, the one or
// two 2x passes of a level's upscale, the pass that clamps an upscaled image to the range of its
// source, the pass that keeps hard alpha edges, the CPU box filtered mip chain, and the rule for which draws read texels as colours
// (GS/Renderers/HW/GSTexelAddressedDraw.h, header only like the rest).
//
// The upscale tests run the real engine with the real Smooth filters in bin/resources/upscale/raisr.

#include "GS/Renderers/HW/GSTexelAddressedDraw.h"
#include "GS/Renderers/HW/GSTextureUpscaleSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef GS_UPSCALER_RESOURCE_DIR
#error "GS_UPSCALER_RESOURCE_DIR must name bin/resources/upscale/raisr"
#endif

using namespace GSTextureUpscaleSupport;

namespace
{
	struct Mip
	{
		u32 width = 0;
		u32 height = 0;
		u32 pitch = 0;
		std::vector<u8> data;
	};

	// Pixel (x, y) of an RGBA8 image with the given pitch.
	std::vector<u8> MakeImage(u32 w, u32 h, u32 pitch, u8 (*fn)(u32 x, u32 y, u32 c))
	{
		std::vector<u8> img(static_cast<size_t>(pitch) * h, 0xEE); // padding is poisoned
		for (u32 y = 0; y < h; y++)
		{
			for (u32 x = 0; x < w; x++)
			{
				for (u32 c = 0; c < 4; c++)
					img[static_cast<size_t>(y) * pitch + x * 4 + c] = fn(x, y, c);
			}
		}
		return img;
	}

	u8 At(const Mip& m, u32 x, u32 y, u32 c)
	{
		return m.data[static_cast<size_t>(y) * m.pitch + x * 4 + c];
	}

	std::vector<u8> RandomBytes(size_t n, u32 seed)
	{
		std::vector<u8> v(n);
		u32 s = seed ? seed : 1;
		for (u8& b : v)
		{
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			b = static_cast<u8>(s >> 11);
		}
		return v;
	}
} // namespace

// ---------------------------------------------------------------------------------------------
//  BoundedLifoQueue
// ---------------------------------------------------------------------------------------------

TEST(GsTextureUpscaleQueue, PopsNewestFirst)
{
	BoundedLifoQueue<int> q(8, 1000);
	std::vector<int> dropped;
	for (int i = 1; i <= 4; i++)
		q.Push(i, 1, &dropped);

	EXPECT_TRUE(dropped.empty());
	EXPECT_EQ(q.Size(), 4u);
	for (int expect = 4; expect >= 1; expect--)
	{
		const std::optional<int> v = q.PopNewest();
		ASSERT_TRUE(v.has_value());
		EXPECT_EQ(*v, expect);
	}
	EXPECT_TRUE(q.Empty());
	EXPECT_FALSE(q.PopNewest().has_value());
	EXPECT_EQ(q.Cost(), 0u);
}

TEST(GsTextureUpscaleQueue, ItemCapDropsTheOldest)
{
	BoundedLifoQueue<int> q(3, 1000);
	std::vector<int> dropped;
	for (int i = 1; i <= 5; i++)
		q.Push(i, 1, &dropped);

	// 1 and 2 were pushed out, in the order they were the oldest.
	EXPECT_EQ(dropped, (std::vector<int>{1, 2}));
	EXPECT_EQ(q.Size(), 3u);
	EXPECT_EQ(*q.PopNewest(), 5);
	EXPECT_EQ(*q.PopNewest(), 4);
	EXPECT_EQ(*q.PopNewest(), 3);
}

TEST(GsTextureUpscaleQueue, CostCapDropsTheOldest)
{
	BoundedLifoQueue<int> q(100, 10);
	std::vector<int> dropped;
	q.Push(1, 4, &dropped);
	q.Push(2, 4, &dropped);
	EXPECT_TRUE(dropped.empty());
	EXPECT_EQ(q.Cost(), 8u);

	q.Push(3, 4, &dropped); // 12 > 10
	EXPECT_EQ(dropped, (std::vector<int>{1}));
	EXPECT_EQ(q.Cost(), 8u);

	q.Push(4, 9, &dropped); // 17: both older ones go
	EXPECT_EQ(dropped, (std::vector<int>{1, 2, 3}));
	EXPECT_EQ(q.Size(), 1u);
	EXPECT_EQ(q.Cost(), 9u);
}

TEST(GsTextureUpscaleQueue, TheNewestItemIsNeverDropped)
{
	// One item over the cost cap is kept, alone, until something newer arrives.
	BoundedLifoQueue<int> q(4, 10);
	std::vector<int> dropped;
	q.Push(1, 100, &dropped);
	EXPECT_TRUE(dropped.empty());
	EXPECT_EQ(q.Size(), 1u);

	q.Push(2, 1, &dropped);
	EXPECT_EQ(dropped, (std::vector<int>{1}));
	EXPECT_EQ(*q.PopNewest(), 2);

	// A cap of zero items still holds the newest.
	BoundedLifoQueue<int> one(0, 1000);
	one.Push(7, 1, nullptr);
	one.Push(8, 1, nullptr);
	EXPECT_EQ(one.Size(), 1u);
	EXPECT_EQ(*one.PopNewest(), 8);
}

TEST(GsTextureUpscaleQueue, ClearReturnsTheOldestFirst)
{
	BoundedLifoQueue<int> q(8, 1000);
	for (int i = 1; i <= 3; i++)
		q.Push(i, 5, nullptr);

	std::vector<int> removed;
	q.Clear(&removed);
	EXPECT_EQ(removed, (std::vector<int>{1, 2, 3}));
	EXPECT_TRUE(q.Empty());
	EXPECT_EQ(q.Cost(), 0u);
}

TEST(GsTextureUpscaleQueue, HoldsMoveOnlyItems)
{
	BoundedLifoQueue<std::unique_ptr<int>> q(2, 1000);
	std::vector<std::unique_ptr<int>> dropped;
	q.Push(std::make_unique<int>(1), 1, &dropped);
	q.Push(std::make_unique<int>(2), 1, &dropped);
	q.Push(std::make_unique<int>(3), 1, &dropped);

	ASSERT_EQ(dropped.size(), 1u);
	EXPECT_EQ(*dropped[0], 1);
	std::optional<std::unique_ptr<int>> newest = q.PopNewest();
	ASSERT_TRUE(newest.has_value());
	EXPECT_EQ(**newest, 3);
}

// How the upscaler uses it: a pending mark per queued job, and the marks of jobs pushed out are
// removed, so a dropped texture can be queued again and none is left stale.
TEST(GsTextureUpscaleQueue, PendingMarksFollowTheQueue)
{
	BoundedLifoQueue<int> q(4, 1000);
	std::set<int> pending;

	for (int name = 0; name < 20; name++)
	{
		std::vector<int> dropped;
		ASSERT_TRUE(pending.insert(name).second);
		q.Push(name, 1, &dropped);
		for (int d : dropped)
			EXPECT_EQ(pending.erase(d), 1u);
	}

	// What is marked is exactly what is queued: the newest four.
	EXPECT_EQ(pending, (std::set<int>{16, 17, 18, 19}));
	std::set<int> queued;
	while (std::optional<int> v = q.PopNewest())
		queued.insert(*v);
	EXPECT_EQ(queued, pending);

	// A dropped name can be queued again.
	EXPECT_TRUE(pending.insert(3).second);
}

// ---------------------------------------------------------------------------------------------
//  UpscaledMipLevelCount
// ---------------------------------------------------------------------------------------------

TEST(GsTextureUpscaleLevels, PowerOfTwoSquareTakesEveryLevel)
{
	// The native texture never asks for more levels than its size has (log2 + 1), or than the
	// seven a guest texture can carry.
	for (u32 size : {8u, 16u, 64u, 256u, 1024u})
	{
		u32 full = 1;
		for (u32 s = size; s > 1; s >>= 1)
			full++;

		for (u32 requested = 1; requested <= std::min(full, 7u); requested++)
			EXPECT_EQ(UpscaledMipLevelCount(size, size, requested, 2), requested) << size << " x" << requested;
	}
}

TEST(GsTextureUpscaleLevels, StopsWhereOneSideHasReachedOnePixel)
{
	// Guest level 4 of a 256x16 texture is 16x1, doubled 32x2, which is the 512x32 texture's own
	// level 4. Guest level 5 is 8x1 (the height stays at one pixel), doubled 16x2, but the
	// 512x32 texture's level 5 is 16x1, so it does not fit.
	EXPECT_EQ(UpscaledMipLevelCount(256, 16, 9, 2), 5u);
	EXPECT_EQ(UpscaledMipLevelCount(256, 16, 5, 2), 5u);
	EXPECT_EQ(UpscaledMipLevelCount(256, 16, 3, 2), 3u);
	EXPECT_EQ(UpscaledMipLevelCount(16, 256, 9, 2), 5u);
}

TEST(GsTextureUpscaleLevels, StopsAtAnOddRegionSize)
{
	// A 100x60 region: 50x30 and 25x15 double to 100x60 and 50x30, which are the 200x120
	// texture's levels 1 and 2; guest level 3 is 12x7, doubled 24x14, against a slot of 25x15.
	EXPECT_EQ(UpscaledMipLevelCount(100, 60, 4, 2), 3u);
	EXPECT_EQ(UpscaledMipLevelCount(100, 60, 3, 2), 3u);
}

TEST(GsTextureUpscaleLevels, AlwaysAtLeastTheBase)
{
	EXPECT_EQ(UpscaledMipLevelCount(64, 64, 0, 2), 1u);
	EXPECT_EQ(UpscaledMipLevelCount(64, 64, 1, 2), 1u);
	EXPECT_EQ(UpscaledMipLevelCount(100, 60, 1, 2), 1u);
}

TEST(GsTextureUpscaleLevels, ScaleFourKnownSizes)
{
	// A 4x texture of a 64x64 guest texture is 256x256, with levels 256, 128, 64, 32, 16, 8, 4, 2, 1.
	// Guest level i is 64 >> i, and quadrupled it is the same size, down to the seventh level.
	for (u32 requested = 1; requested <= 7; requested++)
		EXPECT_EQ(UpscaledMipLevelCount(64, 64, requested, 4), requested);

	// A 256x16 guest level 4 is 16x1, quadrupled 64x4, which is the 1024x64 texture's level 4. Level 5
	// is 8x1, quadrupled 32x4, against a slot of 32x2.
	EXPECT_EQ(UpscaledMipLevelCount(256, 16, 9, 4), 5u);
	EXPECT_EQ(UpscaledMipLevelCount(16, 256, 9, 4), 5u);

	// A 100x60 region: 50x30 and 25x15 quadruple to 200x120 and 100x60, which are levels 1 and 2 of
	// the 400x240 texture. Guest level 3 is 12x7, quadrupled 48x28, against a slot of 50x30.
	EXPECT_EQ(UpscaledMipLevelCount(100, 60, 7, 4), 3u);
}

TEST(GsTextureUpscaleLevels, EveryReturnedLevelFitsItsSlotAndTheNextOneDoesNot)
{
	for (u32 scale : {2u, 4u})
	{
		for (u32 w = 1; w <= 130; w++)
		{
			for (u32 h : {1u, 2u, 3u, 8u, 17u, 64u, 100u})
			{
				const u32 requested = 7;
				const u32 count = UpscaledMipLevelCount(w, h, requested, scale);
				ASSERT_GE(count, 1u);
				ASSERT_LE(count, requested);

				for (u32 level = 0; level < std::min(count + 1, requested); level++)
				{
					const bool fits = std::max(w >> level, 1u) * scale == std::max((w * scale) >> level, 1u) &&
					                  std::max(h >> level, 1u) * scale == std::max((h * scale) >> level, 1u);
					if (level < count)
						EXPECT_TRUE(fits) << w << "x" << h << " scale " << scale << " level " << level;
					else
						EXPECT_FALSE(fits) << w << "x" << h << " scale " << scale << " level " << level;
				}
			}
		}
	}
}

TEST(GsTextureUpscaleLevels, ScaleFourTakesTheSameLevelsAsScaleTwo)
{
	// Multiplying by a power of two moves every level's size by the same shift, so a level that
	// fits at 2x fits at 4x. Pinned over a range of sizes so a change to either shows up.
	for (u32 w = 1; w <= 300; w++)
	{
		for (u32 h : {1u, 5u, 16u, 96u, 300u})
			EXPECT_EQ(UpscaledMipLevelCount(w, h, 7, 4), UpscaledMipLevelCount(w, h, 7, 2)) << w << "x" << h;
	}
}

// ---------------------------------------------------------------------------------------------
//  UpscaleScaleForSize
// ---------------------------------------------------------------------------------------------

TEST(GsTextureUpscaleScale, FourTimesUpToFiveHundredTwelve)
{
	EXPECT_EQ(UpscaleScaleForSize(true, 512, 512), 4u);
	EXPECT_EQ(UpscaleScaleForSize(true, 512, 64), 4u);
	EXPECT_EQ(UpscaleScaleForSize(true, 64, 512), 4u);
	EXPECT_EQ(UpscaleScaleForSize(true, 8, 8), 4u);
	EXPECT_EQ(UpscaleScaleForSize(true, 100, 60), 4u);
}

TEST(GsTextureUpscaleScale, LargerTexturesFallBackToTwoTimes)
{
	// Either side over 512 is enough.
	EXPECT_EQ(UpscaleScaleForSize(true, 513, 16), 2u);
	EXPECT_EQ(UpscaleScaleForSize(true, 16, 513), 2u);
	EXPECT_EQ(UpscaleScaleForSize(true, 600, 600), 2u);
	EXPECT_EQ(UpscaleScaleForSize(true, 1024, 1024), 2u);
	EXPECT_EQ(UpscaleScaleForSize(true, 1024, 8), 2u);
}

TEST(GsTextureUpscaleScale, TwoTimesWhenTheFourTimesModeIsOff)
{
	EXPECT_EQ(UpscaleScaleForSize(false, 8, 8), 2u);
	EXPECT_EQ(UpscaleScaleForSize(false, 512, 512), 2u);
	EXPECT_EQ(UpscaleScaleForSize(false, 1024, 1024), 2u);
}

// ---------------------------------------------------------------------------------------------
//  UpscaleRGBA8: one or two 2x passes
// ---------------------------------------------------------------------------------------------

namespace
{
	std::shared_ptr<const GSTextureUpscaler::FilterSet> LoadFilters(const char* set)
	{
		std::string error;
		auto filters = GSTextureUpscaler::FilterSet::Load(std::string(GS_UPSCALER_RESOURCE_DIR) + "/" + set, &error);
		EXPECT_TRUE(filters) << error;
		return filters;
	}

	std::shared_ptr<const GSTextureUpscaler::FilterSet> LoadSmooth()
	{
		return LoadFilters("smooth");
	}

	// A busy image, so the filter has something to do and a chaining mistake shows in the bytes.
	std::vector<u8> BusyImage(u32 w, u32 h, u32 pitch)
	{
		return MakeImage(w, h, pitch, [](u32 x, u32 y, u32 c) -> u8 {
			switch (c)
			{
				case 0: return static_cast<u8>(((x / 3) ^ (y / 2)) & 1 ? 230 : 25);
				case 1: return static_cast<u8>((x * 11 + y * 5) & 0xFF);
				case 2: return static_cast<u8>(60 + ((x + y) % 7) * 20);
				default: return static_cast<u8>(40 + ((x * 37 + y * 101) % 161));
			}
		});
	}

	// A pitch-w*4 image of one colour.
	std::vector<u8> ConstantImage(u32 w, u32 h, u8 r, u8 g, u8 b, u8 a)
	{
		std::vector<u8> img(static_cast<size_t>(w) * h * 4);
		for (size_t i = 0; i < static_cast<size_t>(w) * h; i++)
		{
			img[i * 4 + 0] = r;
			img[i * 4 + 1] = g;
			img[i * 4 + 2] = b;
			img[i * 4 + 3] = a;
		}
		return img;
	}

	// What a 2x image should be after KeepHardAlphaEdges2x, worked out from the sample position: where
	// the four source texels an output pixel is interpolated from (x / 2 - 0.25, clamped to the image)
	// differ in alpha by HARD_ALPHA_STEP or more, the pixel is the texel it lies inside, colour and
	// alpha; everywhere else out keeps the pixel it has.
	void ApplyHardAlphaEdgesReference(const u8* in, u32 iw, u32 ih, u32 ipitch, u8* out, u32 opitch)
	{
		for (u32 y = 0; y < ih * 2; y++)
		{
			for (u32 x = 0; x < iw * 2; x++)
			{
				const int x0 = static_cast<int>(std::floor((x + 0.5) / 2.0 - 0.5));
				const int y0 = static_cast<int>(std::floor((y + 0.5) / 2.0 - 0.5));
				int lo = 255, hi = 0;
				for (int dy = 0; dy <= 1; dy++)
				{
					for (int dx = 0; dx <= 1; dx++)
					{
						const u32 tx = static_cast<u32>(std::clamp(x0 + dx, 0, static_cast<int>(iw) - 1));
						const u32 ty = static_cast<u32>(std::clamp(y0 + dy, 0, static_cast<int>(ih) - 1));
						const int a = in[static_cast<size_t>(ty) * ipitch + tx * 4 + 3];
						lo = std::min(lo, a);
						hi = std::max(hi, a);
					}
				}

				if (hi - lo >= static_cast<int>(HARD_ALPHA_STEP))
					std::memcpy(out + static_cast<size_t>(y) * opitch + x * 4, in + static_cast<size_t>(y / 2) * ipitch + (x / 2) * 4, 4);
			}
		}
	}

	// The reference for a 4x level: the engine's own 2x entry points, each RAISR pass clamped to the
	// range of its source and with its hard alpha edges kept, chained by hand with the same size
	// rule, into a tightly packed result.
	std::vector<u8> ChainByHand(const GSTextureUpscaler::FilterSet& f, const std::vector<u8>& src, u32 w, u32 h, u32 pitch)
	{
		const auto pass = [&f](const u8* in, u32 iw, u32 ih, u32 ipitch, std::vector<u8>* out) {
			out->assign(static_cast<size_t>(iw) * 2 * ih * 2 * 4, 0);
			if (std::min(iw, ih) >= 8)
			{
				GSTextureUpscaler::UpscaleRGBA8x2(f, in, iw, ih, ipitch, out->data(), iw * 2 * 4);
				ClampUpscaledToSourceRange(in, iw, ih, ipitch, out->data(), iw * 2 * 4);
			}
			else
				GSTextureUpscaler::BilinearRGBA8x2(in, iw, ih, ipitch, out->data(), iw * 2 * 4);
			ApplyHardAlphaEdgesReference(in, iw, ih, ipitch, out->data(), iw * 2 * 4);
		};

		std::vector<u8> mid, out;
		pass(src.data(), w, h, pitch, &mid);
		pass(mid.data(), w * 2, h * 2, w * 2 * 4, &out);
		return out;
	}
} // namespace

TEST(GsTextureUpscaleChain, ScaleTwoIsOneClampedEnginePassWithItsHardAlphaEdgesKept)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);
	const u32 w = 24, h = 16;
	const std::vector<u8> src = BusyImage(w, h, w * 4);

	std::vector<u8> expect(static_cast<size_t>(w) * 2 * h * 2 * 4);
	GSTextureUpscaler::UpscaleRGBA8x2(*f, src.data(), w, h, w * 4, expect.data(), w * 2 * 4);
	ClampUpscaledToSourceRange(src.data(), w, h, w * 4, expect.data(), w * 2 * 4);
	ApplyHardAlphaEdgesReference(src.data(), w, h, w * 4, expect.data(), w * 2 * 4);

	std::vector<u8> got(expect.size(), 0xEE);
	UpscaleRGBA8(*f, src.data(), w, h, w * 4, 2, got.data(), w * 2 * 4);
	EXPECT_EQ(got, expect);
}

TEST(GsTextureUpscaleChain, ScaleFourIsTwoEnginePassesOnTheFirstResult)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);
	const std::pair<u32, u32> sizes[] = {{16, 16}, {24, 10}, {9, 33}, {64, 8}};
	for (const auto& [w, h] : sizes)
	{
		const std::vector<u8> src = BusyImage(w, h, w * 4);
		const std::vector<u8> expect = ChainByHand(*f, src, w, h, w * 4);

		std::vector<u8> got(static_cast<size_t>(w) * 4 * h * 4 * 4, 0xEE);
		UpscaleRGBA8(*f, src.data(), w, h, w * 4, 4, got.data(), w * 4 * 4);
		EXPECT_EQ(got, expect) << w << "x" << h;
	}
}

TEST(GsTextureUpscaleChain, SmallLevelsUseBilinearAndTheIntermediateCanStillUseTheFilter)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);

	// 4x4 is under 8, so its first pass is bilinear; the 8x8 result is not, so the second pass is the
	// filter. 3x3 stays under 8 after one pass (6x6), so both passes are bilinear. 6x20 is under 8 on
	// one side only, which is enough.
	const std::pair<u32, u32> sizes[] = {{4, 4}, {3, 3}, {6, 20}, {2, 2}, {1, 9}};
	for (const auto& [w, h] : sizes)
	{
		const std::vector<u8> src = BusyImage(w, h, w * 4);
		const std::vector<u8> expect = ChainByHand(*f, src, w, h, w * 4);

		std::vector<u8> got(static_cast<size_t>(w) * 4 * h * 4 * 4, 0xEE);
		UpscaleRGBA8(*f, src.data(), w, h, w * 4, 4, got.data(), w * 4 * 4);
		EXPECT_EQ(got, expect) << w << "x" << h;
	}

	// The filter pass must actually differ from a second bilinear pass on 4x4, or the test above
	// would not tell the two apart.
	const std::vector<u8> src = BusyImage(4, 4, 16);
	std::vector<u8> mid(8 * 8 * 4), bilinear_twice(16 * 16 * 4), filtered(16 * 16 * 4);
	GSTextureUpscaler::BilinearRGBA8x2(src.data(), 4, 4, 16, mid.data(), 32);
	GSTextureUpscaler::BilinearRGBA8x2(mid.data(), 8, 8, 32, bilinear_twice.data(), 64);
	UpscaleRGBA8(*f, src.data(), 4, 4, 16, 4, filtered.data(), 64);
	EXPECT_NE(filtered, bilinear_twice);
}

TEST(GsTextureUpscaleChain, ConstantImageStaysConstantAtFourTimes)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);

	const u32 w = 20, h = 12, scale = 4;
	const u32 dst_pitch = w * scale * 4 + 32; // padding after each row, poisoned
	const std::vector<u8> src = ConstantImage(w, h, 200, 100, 50, 77);
	std::vector<u8> dst(static_cast<size_t>(dst_pitch) * h * scale + 64, 0xEE);

	UpscaleRGBA8(*f, src.data(), w, h, w * 4, scale, dst.data(), dst_pitch);

	for (u32 y = 0; y < h * scale; y++)
	{
		for (u32 x = 0; x < w * scale; x++)
		{
			const u8* px = &dst[static_cast<size_t>(y) * dst_pitch + x * 4];
			EXPECT_EQ(px[0], 200) << x << "," << y;
			EXPECT_EQ(px[1], 100) << x << "," << y;
			EXPECT_EQ(px[2], 50) << x << "," << y;
			EXPECT_EQ(px[3], 77) << x << "," << y;
		}

		// The pitch padding and nothing past the last row are touched.
		for (u32 i = w * scale * 4; i < dst_pitch; i++)
			EXPECT_EQ(dst[static_cast<size_t>(y) * dst_pitch + i], 0xEE) << "row " << y << " pad " << i;
	}
	for (size_t i = static_cast<size_t>(dst_pitch) * h * scale; i < dst.size(); i++)
		ASSERT_EQ(dst[i], 0xEE) << "past the end at " << i;
}

TEST(GsTextureUpscaleChain, ReadsASourceWithRowPadding)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);

	const u32 w = 17, h = 11;
	const std::vector<u8> tight = BusyImage(w, h, w * 4);
	const std::vector<u8> padded = BusyImage(w, h, w * 4 + 20); // padding is poisoned by MakeImage

	std::vector<u8> a(static_cast<size_t>(w) * 16 * h * 4), b(a.size());
	UpscaleRGBA8(*f, tight.data(), w, h, w * 4, 4, a.data(), w * 4 * 4);
	UpscaleRGBA8(*f, padded.data(), w, h, w * 4 + 20, 4, b.data(), w * 4 * 4);
	EXPECT_EQ(a, b);
}

TEST(GsTextureUpscaleChain, AlphaStaysInTheSourceRangeAtFourTimes)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);

	// Alpha anywhere in 40..200 (BusyImage). Each pass interpolates it or takes a source texel's, so
	// the result cannot leave that range, and the range taken from the source is valid for the
	// renderer.
	const u32 w = 32, h = 32;
	const std::vector<u8> src = BusyImage(w, h, w * 4);
	std::vector<u8> dst(static_cast<size_t>(w) * 16 * h * 4);
	UpscaleRGBA8(*f, src.data(), w, h, w * 4, 4, dst.data(), w * 4 * 4);
	for (size_t i = 3; i < dst.size(); i += 4)
	{
		ASSERT_GE(dst[i], 40);
		ASSERT_LE(dst[i], 200);
	}
}

namespace
{
	// Every output pixel's alpha must be the alpha of the source texel that pixel lies inside:
	// source (x / scale, y / scale). Worked out from the position, not from the code under test.
	void ExpectAlphaIsTheContainingTexelsAlpha(const std::vector<u8>& src, u32 w, u32 h, u32 src_pitch,
		const std::vector<u8>& dst, u32 dst_pitch, u32 scale)
	{
		for (u32 y = 0; y < h * scale; y++)
		{
			for (u32 x = 0; x < w * scale; x++)
			{
				const u8 want = src[static_cast<size_t>(y / scale) * src_pitch + (x / scale) * 4 + 3];
				ASSERT_EQ(dst[static_cast<size_t>(y) * dst_pitch + x * 4 + 3], want) << x << "," << y;
			}
		}
	}
} // namespace

TEST(GsTextureUpscaleAlpha, KeepHardAlphaEdges2xChangesOnlyTheHardEdgePixels)
{
	// Random alpha over its whole range gives mostly hard edges; alpha confined to a narrow band gives
	// none, and a mix of the two gives both. A pixel off a hard edge, and the padding, must come out
	// as they went in; one on a hard edge is the whole texel it lies inside.
	const std::pair<u32, u32> sizes[] = {{1, 1}, {1, 6}, {7, 1}, {3, 5}, {16, 9}};
	for (const u32 band : {256u, 40u, 100u})
	{
		for (const auto& [w, h] : sizes)
		{
			SCOPED_TRACE("band " + std::to_string(band) + " " + std::to_string(w) + "x" + std::to_string(h));
			const u32 src_pitch = w * 4 + 8;
			const u32 dst_pitch = w * 8 + 12;
			std::vector<u8> src = RandomBytes(static_cast<size_t>(src_pitch) * h, w * 131 + h + band);
			for (u32 y = 0; y < h; y++)
			{
				for (u32 x = 0; x < w; x++)
				{
					u8& a = src[static_cast<size_t>(y) * src_pitch + x * 4 + 3];
					a = static_cast<u8>(100 + a % band);
				}
			}

			const std::vector<u8> before = RandomBytes(static_cast<size_t>(dst_pitch) * h * 2, w * 17 + h * 3 + 1);
			std::vector<u8> expect = before;
			ApplyHardAlphaEdgesReference(src.data(), w, h, src_pitch, expect.data(), dst_pitch);

			std::vector<u8> dst = before;
			KeepHardAlphaEdges2x(src.data(), w, h, src_pitch, dst.data(), dst_pitch);
			ASSERT_EQ(dst, expect);
		}
	}
}

TEST(GsTextureUpscaleAlpha, ABinaryMaskKeepsItsExactEdges)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);

	// Katamari's King of All Cosmos draws his eyes as a 64x32 texture of two rectangles: alpha 0x80
	// inside each and 0 around them. Draws against it alpha test NOTEQUAL 0 with depth writes, and
	// the face is drawn after, behind them. Interpolated alpha gave each rectangle a ring of small
	// non-zero alpha: it passed the test and kept the face out, but blended almost none of the eye
	// over the dark cloud behind, a dark frame like glasses. A mask has to come out with the
	// original's edges, whatever the scale and whichever pass made the colours (the filter, or
	// bilinear for a level too small for it).
	const std::pair<u32, u32> sizes[] = {{64, 32}, {24, 16}, {8, 8}, {7, 9}, {4, 4}, {3, 5}, {1, 8}};
	const std::pair<u8, u8> levels[] = {{0, 0x80}, {0x10, 0xFF}};
	for (const auto& [lo, hi] : levels)
	{
		for (const auto& [w, h] : sizes)
		{
			std::vector<u8> src(static_cast<size_t>(w) * 4 + 8, 0xEE);
			src.resize((static_cast<size_t>(w) * 4 + 8) * h, 0xEE); // padding is poisoned
			for (u32 y = 0; y < h; y++)
			{
				for (u32 x = 0; x < w; x++)
				{
					u8* px = &src[static_cast<size_t>(y) * (w * 4 + 8) + x * 4];
					for (u32 c = 0; c < 3; c++) // colour varies, including under the transparent texels, as in the game
						px[c] = static_cast<u8>(30 + ((x * 29 + y * 17 + c * 53) & 0x7F));
					px[3] = ((x >= 2 && x < 9 && y >= 2 && y < 6) || (x >= 12 && y >= 1 && y < 3)) ? hi : lo;
				}
			}

			for (const u32 scale : {2u, 4u})
			{
				SCOPED_TRACE(std::to_string(w) + "x" + std::to_string(h) + " at " + std::to_string(scale) + "x, alpha " +
							 std::to_string(lo) + "/" + std::to_string(hi));
				const u32 dst_pitch = w * scale * 4;
				std::vector<u8> dst(static_cast<size_t>(dst_pitch) * h * scale, 0xEE);
				UpscaleRGBA8(*f, src.data(), w, h, w * 4 + 8, scale, dst.data(), dst_pitch);
				ExpectAlphaIsTheContainingTexelsAlpha(src, w, h, w * 4 + 8, dst, dst_pitch, scale);
			}
		}
	}
}

TEST(GsTextureUpscaleAlpha, DarkTransparentTexelsDoNotDarkenTheOpaqueSide)
{
	// Katamari's King of All Cosmos eye mask: the texels around the two rectangles are black and
	// alpha 0, the rectangles dark grey and alpha 0x80. The colour of a transparent texel is the
	// game's to choose and nothing on screen is meant to show it, but interpolating across the edge
	// blends it into the opaque pixel next to the edge, and the clamp to the range of the four source
	// texels allows that because the black texel is one of them. With the alpha edge kept exact that
	// pixel is drawn, a dark outline round each rectangle. No pixel on the opaque side may come out
	// darker than the opaque colour, which is the darkest thing in the opaque texels.
	const u8 opaque[3] = {62, 60, 56};
	const u32 w = 64, h = 32;
	std::vector<u8> src(static_cast<size_t>(w) * h * 4, 0);
	for (u32 y = 0; y < h; y++)
	{
		for (u32 x = 0; x < w; x++)
		{
			if ((x >= 16 && x < 28 && y >= 6 && y < 13) || (x >= 37 && x < 48 && y >= 6 && y < 13))
			{
				u8* px = &src[(static_cast<size_t>(y) * w + x) * 4];
				std::memcpy(px, opaque, 3);
				px[3] = 0x80;
			}
		}
	}

	for (const char* set : {"smooth", "sharp"})
	{
		const auto f = LoadFilters(set);
		ASSERT_TRUE(f);
		for (const u32 scale : {2u, 4u})
		{
			SCOPED_TRACE(std::string(set) + " at " + std::to_string(scale) + "x");
			const u32 dst_pitch = w * scale * 4;
			std::vector<u8> dst(static_cast<size_t>(dst_pitch) * h * scale, 0xEE);
			UpscaleRGBA8(*f, src.data(), w, h, w * 4, scale, dst.data(), dst_pitch);

			u32 checked = 0;
			for (u32 y = 0; y < h * scale; y++)
			{
				for (u32 x = 0; x < w * scale; x++)
				{
					if (src[(static_cast<size_t>(y / scale) * w + x / scale) * 4 + 3] != 0x80)
						continue;

					checked++;
					const u8* px = &dst[static_cast<size_t>(y) * dst_pitch + x * 4];
					for (u32 c = 0; c < 3; c++)
						ASSERT_GE(px[c], opaque[c]) << "channel " << c << " at " << x << "," << y;
				}
			}
			ASSERT_GT(checked, 0u);
		}
	}
}

TEST(GsTextureUpscaleAlpha, AGradientKeepsItsInterpolatedAlpha)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);

	// Black bakes its lighting into the alpha of 256x256 lightmaps and reads them with a bilinear
	// sampler and a blend that scales the framebuffer by that alpha. Replacing a gradient's
	// interpolated alpha with each texel's own made it a staircase, which the blend turned into
	// broad dark bands. Where neighbouring texels differ by less than a hard edge, alpha has to come
	// out as the plain bilinear upscale of the source.
	const std::pair<u32, u32> sizes[] = {{32, 16}, {6, 5}};
	for (const auto& [w, h] : sizes)
	{
		SCOPED_TRACE(std::to_string(w) + "x" + std::to_string(h));
		const std::vector<u8> src = MakeImage(w, h, w * 4, [](u32 x, u32 y, u32 c) -> u8 {
			return c == 3 ? static_cast<u8>(10 + x * 5 + y * 3) : static_cast<u8>(40 + ((x + y) & 0x3F));
		});

		std::vector<u8> bilinear(static_cast<size_t>(w) * 2 * h * 2 * 4);
		GSTextureUpscaler::BilinearRGBA8x2(src.data(), w, h, w * 4, bilinear.data(), w * 2 * 4);

		std::vector<u8> dst(bilinear.size(), 0xEE);
		UpscaleRGBA8(*f, src.data(), w, h, w * 4, 2, dst.data(), w * 2 * 4);
		for (size_t i = 3; i < dst.size(); i += 4)
			ASSERT_EQ(dst[i], bilinear[i]) << "byte " << i;
	}
}

TEST(GsTextureUpscaleChain, ASixHundredSquareTextureIsUpscaledByTwoNotFour)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);

	// What the job does with a texture over 512 when the 4x mode is on: the scale comes back as 2,
	// and the output is 1200x1200, not 2400x2400.
	const u32 w = 600, h = 600;
	const u32 scale = UpscaleScaleForSize(true, w, h);
	ASSERT_EQ(scale, 2u);

	const std::vector<u8> src = ConstantImage(w, h, 10, 20, 30, 255);
	const size_t out_bytes = static_cast<size_t>(w) * scale * h * scale * 4;
	std::vector<u8> dst(out_bytes + 64, 0xEE);
	UpscaleRGBA8(*f, src.data(), w, h, w * 4, scale, dst.data(), w * scale * 4);

	EXPECT_EQ(dst[0], 10);
	EXPECT_EQ(dst[out_bytes - 4], 10);
	EXPECT_EQ(dst[out_bytes - 1], 255);
	for (size_t i = out_bytes; i < dst.size(); i++)
		ASSERT_EQ(dst[i], 0xEE) << "past the end at " << i;
}

// ---------------------------------------------------------------------------------------------
//  BuildBoxMipChain
// ---------------------------------------------------------------------------------------------

TEST(GsTextureUpscaleMips, ChainSizesMatchAFullTexture)
{
	const u32 w = 512, h = 256;
	const std::vector<u8> base = MakeImage(w, h, w * 4, [](u32, u32, u32 c) -> u8 { return static_cast<u8>(c * 10); });

	// floor(log2(512)) + 1 levels, as GSDevice::GetMipmapLevelsForSize gives.
	std::vector<Mip> mips;
	BuildBoxMipChain(base.data(), w, h, w * 4, 10, &mips);
	ASSERT_EQ(mips.size(), 9u);
	for (u32 i = 0; i < mips.size(); i++)
	{
		const u32 level = i + 1;
		EXPECT_EQ(mips[i].width, std::max(w >> level, 1u)) << level;
		EXPECT_EQ(mips[i].height, std::max(h >> level, 1u)) << level;
		EXPECT_EQ(mips[i].pitch, mips[i].width * 4);
		EXPECT_EQ(mips[i].data.size(), static_cast<size_t>(mips[i].pitch) * mips[i].height);
	}
	EXPECT_EQ(mips.back().width, 1u);
	EXPECT_EQ(mips.back().height, 1u);
}

TEST(GsTextureUpscaleMips, ConstantImageStaysConstant)
{
	const u32 w = 64, h = 32;
	const std::vector<u8> base = MakeImage(w, h, w * 4, [](u32, u32, u32 c) -> u8 {
		static const u8 value[4] = {200, 17, 0, 255};
		return value[c];
	});

	std::vector<Mip> mips;
	BuildBoxMipChain(base.data(), w, h, w * 4, 7, &mips);
	ASSERT_EQ(mips.size(), 6u);
	for (const Mip& m : mips)
	{
		for (u32 y = 0; y < m.height; y++)
		{
			for (u32 x = 0; x < m.width; x++)
			{
				EXPECT_EQ(At(m, x, y, 0), 200);
				EXPECT_EQ(At(m, x, y, 1), 17);
				EXPECT_EQ(At(m, x, y, 2), 0);
				EXPECT_EQ(At(m, x, y, 3), 255);
			}
		}
	}
}

TEST(GsTextureUpscaleMips, AveragesTwoByTwoBlocksPerChannel)
{
	// Channel 0 holds x + 4y over a 4x4 image, channel 3 is 255 - that, channels 1 and 2 are 0.
	const std::vector<u8> base = MakeImage(4, 4, 16, [](u32 x, u32 y, u32 c) -> u8 {
		const u32 v = x + 4 * y;
		return c == 0 ? static_cast<u8>(v) : c == 3 ? static_cast<u8>(255 - v) : 0;
	});

	std::vector<Mip> mips;
	BuildBoxMipChain(base.data(), 4, 4, 16, 3, &mips);
	ASSERT_EQ(mips.size(), 2u);

	// Level 1 is 2x2. Block (0,0) holds 0, 1, 4, 5: the mean 2.5 rounds to 3. Block (1,0) holds
	// 2, 3, 6, 7: 4.5 rounds to 5. Block (0,1): 8, 9, 12, 13: 10.5 -> 11. Block (1,1): 12.5 -> 13.
	ASSERT_EQ(mips[0].width, 2u);
	ASSERT_EQ(mips[0].height, 2u);
	EXPECT_EQ(At(mips[0], 0, 0, 0), 3);
	EXPECT_EQ(At(mips[0], 1, 0, 0), 5);
	EXPECT_EQ(At(mips[0], 0, 1, 0), 11);
	EXPECT_EQ(At(mips[0], 1, 1, 0), 13);
	// Alpha is averaged on its own: 255 - 2.5 = 252.5 rounds to 253.
	EXPECT_EQ(At(mips[0], 0, 0, 3), 253);
	EXPECT_EQ(At(mips[0], 1, 1, 3), 243);

	// Level 2 is one pixel: (3 + 5 + 11 + 13) / 4 = 8.
	ASSERT_EQ(mips[1].width, 1u);
	ASSERT_EQ(mips[1].height, 1u);
	EXPECT_EQ(At(mips[1], 0, 0, 0), 8);
	EXPECT_EQ(At(mips[1], 0, 0, 1), 0);
}

TEST(GsTextureUpscaleMips, RoundsToNearest)
{
	// Four pixels in one block, one channel each: sums 1, 2, 3, 1023 over four.
	const auto block = [](u8 a, u8 b, u8 c, u8 d) {
		const u8 px[4] = {a, b, c, d};
		const std::vector<u8> base = MakeImage(2, 2, 8, [](u32, u32, u32) -> u8 { return 0; });
		std::vector<u8> img = base;
		for (u32 i = 0; i < 4; i++)
			img[(i / 2) * 8 + (i % 2) * 4] = px[i];
		std::vector<Mip> mips;
		BuildBoxMipChain(img.data(), 2, 2, 8, 2, &mips);
		return mips.at(0).data[0];
	};

	EXPECT_EQ(block(0, 0, 0, 1), 0); // 0.25
	EXPECT_EQ(block(0, 0, 1, 1), 1); // 0.5 rounds up
	EXPECT_EQ(block(0, 1, 1, 1), 1); // 0.75
	EXPECT_EQ(block(255, 255, 255, 255), 255);
	EXPECT_EQ(block(255, 255, 255, 254), 255); // 254.75
	EXPECT_EQ(block(255, 255, 254, 254), 255); // 254.5 rounds up
}

TEST(GsTextureUpscaleMips, ReadsOddSizesWithoutReadingPastTheEdge)
{
	// 5x3: level 1 is 2x1, from columns 0-3 and rows 0-1. Column 4 and row 2 fall off the smaller
	// level, as they do for a GPU generated one.
	const u32 w = 5, h = 3, pitch = 5 * 4 + 12; // extra poisoned bytes at the end of each row
	const std::vector<u8> base = MakeImage(w, h, pitch, [](u32 x, u32 y, u32 c) -> u8 {
		return c == 0 ? static_cast<u8>(10 * (x + 1) + 100 * y) : 0;
	});

	std::vector<Mip> mips;
	BuildBoxMipChain(base.data(), w, h, pitch, 3, &mips);
	ASSERT_EQ(mips.size(), 2u);
	ASSERT_EQ(mips[0].width, 2u);
	ASSERT_EQ(mips[0].height, 1u);
	// Block (0,0): x 0,1 and y 0,1: 10, 20, 110, 120 -> 65. Block (1,0): 30, 40, 130, 140 -> 85.
	EXPECT_EQ(At(mips[0], 0, 0, 0), 65);
	EXPECT_EQ(At(mips[0], 1, 0, 0), 85);
	// Level 2 is 1x1: (65 + 85) / 2 horizontally, the single row read twice: 75.
	ASSERT_EQ(mips[1].width, 1u);
	ASSERT_EQ(mips[1].height, 1u);
	EXPECT_EQ(At(mips[1], 0, 0, 0), 75);
	// The poisoned row padding never reaches the output.
	for (const Mip& m : mips)
	{
		for (u32 y = 0; y < m.height; y++)
		{
			for (u32 x = 0; x < m.width; x++)
			{
				EXPECT_NE(At(m, x, y, 1), 0xEE);
				EXPECT_NE(At(m, x, y, 3), 0xEE);
			}
		}
	}
}

TEST(GsTextureUpscaleMips, StaysWithinTheBaseAlphaRange)
{
	// Alpha anywhere in 40..200. An average cannot leave that range, so a range taken from the base
	// level is valid for the whole chain.
	const u32 w = 32, h = 32;
	const std::vector<u8> base = MakeImage(w, h, w * 4, [](u32 x, u32 y, u32 c) -> u8 {
		if (c == 3)
			return static_cast<u8>(40 + ((x * 37 + y * 101) % 161));
		return static_cast<u8>(x * 8);
	});

	std::vector<Mip> mips;
	BuildBoxMipChain(base.data(), w, h, w * 4, 6, &mips);
	for (const Mip& m : mips)
	{
		for (u32 y = 0; y < m.height; y++)
		{
			for (u32 x = 0; x < m.width; x++)
			{
				EXPECT_GE(At(m, x, y, 3), 40);
				EXPECT_LE(At(m, x, y, 3), 200);
			}
		}
	}
}

TEST(GsTextureUpscaleMips, NoLevelsRequested)
{
	const std::vector<u8> base(4 * 4 * 4, 7);
	std::vector<Mip> mips(3);
	BuildBoxMipChain(base.data(), 4, 4, 16, 1, &mips);
	EXPECT_TRUE(mips.empty());
	BuildBoxMipChain(base.data(), 4, 4, 16, 0, &mips);
	EXPECT_TRUE(mips.empty());
}

// ---------------------------------------------------------------------------------------------
//  ClampUpscaledToSourceRange
// ---------------------------------------------------------------------------------------------

namespace
{
	// Smallest and largest value of channel c over the four source texels output pixel (x, y) is
	// interpolated from, worked out from the sample position rather than from the index formulas the
	// code under test uses: x / 2 - 0.25 lies between floor() and floor() + 1, clamped to the image.
	void SupportRange(const std::vector<u8>& src, u32 w, u32 h, u32 pitch, u32 x, u32 y, u32 c, u8* lo, u8* hi)
	{
		const double sx = (x + 0.5) / 2.0 - 0.5;
		const double sy = (y + 0.5) / 2.0 - 0.5;
		const int x0 = static_cast<int>(std::floor(sx));
		const int y0 = static_cast<int>(std::floor(sy));
		*lo = 255;
		*hi = 0;
		for (int dy = 0; dy <= 1; dy++)
		{
			for (int dx = 0; dx <= 1; dx++)
			{
				const u32 tx = static_cast<u32>(std::clamp(x0 + dx, 0, static_cast<int>(w) - 1));
				const u32 ty = static_cast<u32>(std::clamp(y0 + dy, 0, static_cast<int>(h) - 1));
				const u8 v = src[static_cast<size_t>(ty) * pitch + tx * 4 + c];
				*lo = std::min(*lo, v);
				*hi = std::max(*hi, v);
			}
		}
	}
} // namespace

TEST(GsTextureUpscaleClamp, MatchesABruteForceReferenceOnRandomData)
{
	// Odd sizes, a single row and column, and padded pitches, with random "upscaled" output that
	// is out of range almost everywhere.
	const std::pair<u32, u32> sizes[] = {{1, 1}, {1, 6}, {7, 1}, {2, 2}, {3, 5}, {8, 8}, {13, 9}, {32, 17}};
	for (const auto& s : sizes)
	{
		const u32 w = s.first, h = s.second;
		SCOPED_TRACE(std::to_string(w) + "x" + std::to_string(h));
		const u32 src_pitch = w * 4 + 8;
		const u32 dst_pitch = w * 8 + 12;
		const std::vector<u8> src = RandomBytes(static_cast<size_t>(src_pitch) * h, w * 977 + h);
		const std::vector<u8> before = RandomBytes(static_cast<size_t>(dst_pitch) * h * 2, w * 31 + h * 7 + 5);
		std::vector<u8> dst = before;

		ClampUpscaledToSourceRange(src.data(), w, h, src_pitch, dst.data(), dst_pitch);

		for (u32 y = 0; y < h * 2; y++)
		{
			for (u32 x = 0; x < w * 2; x++)
			{
				for (u32 c = 0; c < 4; c++)
				{
					u8 lo, hi;
					SupportRange(src, w, h, src_pitch, x, y, c, &lo, &hi);
					const u8 in = before[static_cast<size_t>(y) * dst_pitch + x * 4 + c];
					const u8 want = std::min(std::max(in, lo), hi);
					ASSERT_EQ(dst[static_cast<size_t>(y) * dst_pitch + x * 4 + c], want) << x << "," << y << " ch " << c;
				}
			}
			// The bytes past the end of the row are not ours to touch.
			for (u32 i = w * 8; i < dst_pitch; i++)
				ASSERT_EQ(dst[static_cast<size_t>(y) * dst_pitch + i], before[static_cast<size_t>(y) * dst_pitch + i]) << "padding";
		}
	}
}

TEST(GsTextureUpscaleClamp, OutputInsideTheRangeIsNotTouched)
{
	// A nearest 2x copy takes each value from one of the four texels it is interpolated from, so
	// it is always inside the range and must come out identical.
	const u32 w = 11, h = 6;
	const std::vector<u8> src = RandomBytes(static_cast<size_t>(w) * 4 * h, 4242);
	std::vector<u8> dst(static_cast<size_t>(w) * 8 * h * 2);
	for (u32 y = 0; y < h * 2; y++)
	{
		for (u32 x = 0; x < w * 2; x++)
		{
			for (u32 c = 0; c < 4; c++)
				dst[(static_cast<size_t>(y) * w * 2 + x) * 4 + c] = src[(static_cast<size_t>(y / 2) * w + x / 2) * 4 + c];
		}
	}

	const std::vector<u8> copy = dst;
	ClampUpscaledToSourceRange(src.data(), w, h, w * 4, dst.data(), w * 8);
	EXPECT_EQ(dst, copy);
}

TEST(GsTextureUpscaleClamp, ARingingEdgeComesOutFlatOnBothSides)
{
	// A vertical step from 113 to 223 between source columns 7 and 8, with the values from the
	// Katamari swatch that showed this. The "upscaler output" overshoots the way the sharp RAISR
	// filters do: 82 and 87 below the dark side, 252 and 248 above the bright side.
	const u32 w = 16, h = 4;
	const std::vector<u8> src = MakeImage(w, h, w * 4, [](u32 x, u32, u32 c) -> u8 {
		return c == 3 ? 128 : (x < 8 ? 113 : 223);
	});

	std::vector<u8> dst(static_cast<size_t>(w) * 8 * h * 2);
	for (u32 y = 0; y < h * 2; y++)
	{
		for (u32 x = 0; x < w * 2; x++)
		{
			static const u8 shape[] = {82, 87, 94, 113, 150, 190, 223, 252, 248, 232, 223, 218};
			u8 v = (x < 8 ? 113 : 223);
			if (x >= 10 && x < 22)
				v = shape[x - 10];
			for (u32 c = 0; c < 3; c++)
				dst[(static_cast<size_t>(y) * w * 2 + x) * 4 + c] = v;
			dst[(static_cast<size_t>(y) * w * 2 + x) * 4 + 3] = 128;
		}
	}

	ClampUpscaledToSourceRange(src.data(), w, h, w * 4, dst.data(), w * 8);

	for (u32 y = 0; y < h * 2; y++)
	{
		for (u32 x = 0; x < w * 2; x++)
		{
			const u8 v = dst[(static_cast<size_t>(y) * w * 2 + x) * 4];
			ASSERT_GE(v, 113) << x << "," << y;
			ASSERT_LE(v, 223) << x << "," << y;
			// Output columns 15 and 16 are the only ones whose four source texels include both
			// sides of the step. Everything left of them reads only 113 and everything right of
			// them only 223, so those come out exactly flat.
			if (x <= 14)
				ASSERT_EQ(v, 113) << x << "," << y;
			if (x >= 17)
				ASSERT_EQ(v, 223) << x << "," << y;
		}
	}
}

TEST(GsTextureUpscaleClamp, EmptyImageIsANoOp)
{
	std::vector<u8> dst(16, 0x55);
	ClampUpscaledToSourceRange(nullptr, 0, 0, 0, dst.data(), 0);
	ClampUpscaledToSourceRange(dst.data(), 4, 0, 16, dst.data(), 32);
	ClampUpscaledToSourceRange(dst.data(), 0, 4, 0, dst.data(), 0);
	for (u8 b : dst)
		EXPECT_EQ(b, 0x55);
}

// ---------------------------------------------------------------------------------------------
//  GSTexelAddressedDraw.h
// ---------------------------------------------------------------------------------------------

TEST(GsTexelAddressedDraw, KatamariWallMagnifiesItsTexels)
{
	// The wall triangle of Katamari Damacy's room: texture coordinates over 5.6 x 3.6 texels of the
	// swatch atlas, spread over 180 x 225 native pixels, so 32 and 62 pixels to a texel.
	EXPECT_TRUE(GSPrimitiveMagnifiesTexels(5.6f, 3.6f, 180.0f, 225.0f));
}

TEST(GsTexelAddressedDraw, AStretchedTextureDoesNot)
{
	// The stain texture drawn over the same wall: 65 x 11 texels over 70 x 18 pixels.
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(65.0f, 11.0f, 70.0f, 18.0f));
	// A sprite copied 1:1, and one stretched 2:1.
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(64.0f, 64.0f, 64.0f, 64.0f));
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(32.0f, 32.0f, 64.0f, 64.0f));
}

TEST(GsTexelAddressedDraw, TheThresholdIsEightPixelsPerTexelOnBothAxes)
{
	EXPECT_TRUE(GSPrimitiveMagnifiesTexels(10.0f, 10.0f, 80.0f, 80.0f));
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(10.0f, 10.0f, 79.0f, 80.0f));
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(10.0f, 10.0f, 80.0f, 79.0f));
	// One axis magnified a long way does not make up for the other.
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(1.0f, 40.0f, 1000.0f, 40.0f));
}

TEST(GsTexelAddressedDraw, AFlatColourPrimitiveAlwaysMagnifies)
{
	// Every vertex on the same texel position: one texel is read for the whole primitive.
	EXPECT_TRUE(GSPrimitiveMagnifiesTexels(0.0f, 0.0f, 3.0f, 3.0f));
	EXPECT_TRUE(GSPrimitiveMagnifiesTexels(0.0f, 0.0f, 0.0f, 0.0f));
	EXPECT_TRUE(GSPrimitiveMagnifiesTexels(0.0f, 0.0f, 512.0f, 448.0f));
}

TEST(GsTexelAddressedDraw, NotANumberAndNegativeExtentsDoNotPass)
{
	const float nan = std::nanf("");
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(nan, 1.0f, 100.0f, 100.0f));
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(1.0f, nan, 100.0f, 100.0f));
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(1.0f, 1.0f, nan, 100.0f));
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(1.0f, 1.0f, 100.0f, nan));
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(-1.0f, 1.0f, 100.0f, 100.0f));
	EXPECT_FALSE(GSPrimitiveMagnifiesTexels(1.0f, 1.0f, -100.0f, 100.0f));
}

TEST(GsTexelAddressedDraw, TheVoteIsByArea)
{
	GSTexelAddressedVote empty;
	EXPECT_FALSE(empty.Passes());

	// Half and more passes, under half does not.
	GSTexelAddressedVote half;
	half.Add(50.0, true);
	half.Add(50.0, false);
	EXPECT_TRUE(half.Passes());

	GSTexelAddressedVote under;
	under.Add(49.0, true);
	under.Add(51.0, false);
	EXPECT_FALSE(under.Passes());

	// The Katamari wall call: two big magnified fans and a few slivers, against the small
	// stretched stain triangles that share the call.
	GSTexelAddressedVote wall;
	wall.Add(20250.0, true);
	wall.Add(15000.0, true);
	wall.Add(300.0, true);
	wall.Add(40.0, false);
	wall.Add(35.0, false);
	wall.Add(60.0, false);
	EXPECT_TRUE(wall.Passes());

	// A character: hundreds of small triangles over a detailed texture, and one flat colour
	// triangle for the eyes. The eyes must not take the whole draw off the upscaled texture.
	GSTexelAddressedVote character;
	for (int i = 0; i < 300; i++)
		character.Add(40.0, false);
	character.Add(12.0, true);
	EXPECT_FALSE(character.Passes());
}

TEST(GsTexelAddressedDraw, EmptyAndNotANumberAreasAreIgnored)
{
	const double nan = std::nan("");
	GSTexelAddressedVote vote;
	vote.Add(0.0, true);
	vote.Add(-5.0, true);
	vote.Add(nan, true);
	EXPECT_EQ(vote.total_area, 0.0);
	EXPECT_FALSE(vote.Passes());

	vote.Add(10.0, true);
	EXPECT_TRUE(vote.Passes());
}
