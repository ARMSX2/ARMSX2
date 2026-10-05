// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the parts of the texture upscaler that have no GS dependencies
// (GS/Renderers/HW/GSTextureUpscaleSupport.h): the bounded newest-first job queue, the scale a
// texture is upscaled by, the count of guest mip levels an upscaled texture can take, the one or
// two 2x passes of a level's upscale, and the CPU box filtered mip chain.
//
// The upscale tests run the real engine with the real Smooth filters in bin/resources/upscale/raisr.

#include "GS/Renderers/HW/GSTextureUpscaleSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
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
	std::shared_ptr<const GSTextureUpscaler::FilterSet> LoadSmooth()
	{
		std::string error;
		auto filters = GSTextureUpscaler::FilterSet::Load(std::string(GS_UPSCALER_RESOURCE_DIR) + "/smooth", &error);
		EXPECT_TRUE(filters) << error;
		return filters;
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

	// The reference for a 4x level: the engine's own 2x entry points, chained by hand with the same
	// size rule, into a tightly packed result.
	std::vector<u8> ChainByHand(const GSTextureUpscaler::FilterSet& f, const std::vector<u8>& src, u32 w, u32 h, u32 pitch)
	{
		const auto pass = [&f](const u8* in, u32 iw, u32 ih, u32 ipitch, std::vector<u8>* out) {
			out->assign(static_cast<size_t>(iw) * 2 * ih * 2 * 4, 0);
			if (std::min(iw, ih) >= 8)
				GSTextureUpscaler::UpscaleRGBA8x2(f, in, iw, ih, ipitch, out->data(), iw * 2 * 4);
			else
				GSTextureUpscaler::BilinearRGBA8x2(in, iw, ih, ipitch, out->data(), iw * 2 * 4);
		};

		std::vector<u8> mid, out;
		pass(src.data(), w, h, pitch, &mid);
		pass(mid.data(), w * 2, h * 2, w * 2 * 4, &out);
		return out;
	}
} // namespace

TEST(GsTextureUpscaleChain, ScaleTwoIsOneEnginePass)
{
	const auto f = LoadSmooth();
	ASSERT_TRUE(f);
	const u32 w = 24, h = 16;
	const std::vector<u8> src = BusyImage(w, h, w * 4);

	std::vector<u8> expect(static_cast<size_t>(w) * 2 * h * 2 * 4);
	GSTextureUpscaler::UpscaleRGBA8x2(*f, src.data(), w, h, w * 4, expect.data(), w * 2 * 4);

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

	// Alpha anywhere in 40..200 (BusyImage). Both passes upscale it bilinearly, so the result cannot
	// leave that range, and the range taken from the result is valid for the renderer.
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
