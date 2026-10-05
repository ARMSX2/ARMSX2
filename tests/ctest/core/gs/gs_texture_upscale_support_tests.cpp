// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the parts of the texture upscaler that have no GS dependencies
// (GS/Renderers/HW/GSTextureUpscaleSupport.h): the bounded newest-first job queue, the count of
// guest mip levels an upscaled texture can take, and the CPU box filtered mip chain.

#include "GS/Renderers/HW/GSTextureUpscaleSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

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
			EXPECT_EQ(UpscaledMipLevelCount(size, size, requested), requested) << size << " x" << requested;
	}
}

TEST(GsTextureUpscaleLevels, StopsWhereOneSideHasReachedOnePixel)
{
	// Guest level 4 of a 256x16 texture is 16x1, doubled 32x2, which is the 512x32 texture's own
	// level 4. Guest level 5 is 8x1 (the height stays at one pixel), doubled 16x2, but the
	// 512x32 texture's level 5 is 16x1, so it does not fit.
	EXPECT_EQ(UpscaledMipLevelCount(256, 16, 9), 5u);
	EXPECT_EQ(UpscaledMipLevelCount(256, 16, 5), 5u);
	EXPECT_EQ(UpscaledMipLevelCount(256, 16, 3), 3u);
	EXPECT_EQ(UpscaledMipLevelCount(16, 256, 9), 5u);
}

TEST(GsTextureUpscaleLevels, StopsAtAnOddRegionSize)
{
	// A 100x60 region: 50x30 and 25x15 double to 100x60 and 50x30, which are the 200x120
	// texture's levels 1 and 2; guest level 3 is 12x7, doubled 24x14, against a slot of 25x15.
	EXPECT_EQ(UpscaledMipLevelCount(100, 60, 4), 3u);
	EXPECT_EQ(UpscaledMipLevelCount(100, 60, 3), 3u);
}

TEST(GsTextureUpscaleLevels, AlwaysAtLeastTheBase)
{
	EXPECT_EQ(UpscaledMipLevelCount(64, 64, 0), 1u);
	EXPECT_EQ(UpscaledMipLevelCount(64, 64, 1), 1u);
	EXPECT_EQ(UpscaledMipLevelCount(100, 60, 1), 1u);
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
