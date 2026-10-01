// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins how the texture cache places a native rectangle on an upscaled target.
//
// Native pixel n owns device pixels [ceil(n * scale), ceil((n + 1) * scale)). A native span
// [x, x + w) therefore owns [ceil(x * scale), ceil((x + w) * scale)), and its device length is the
// difference of those two -- not ceil(w * scale), which is the length of a span that starts at
// native 0. At an integer scale the two agree. At a fractional one they differ by up to a device
// pixel, and the difference decides whether a copy fits its texture and whether it writes into the
// native pixel next door.

#include "GS/Renderers/HW/GSTextureCache.h"

#include <gtest/gtest.h>

namespace
{
	// A 448-row target at 1.5x is 672 device rows.
	constexpr int kRows = 448;
	constexpr float kScale = 1.5f;
	constexpr int kDeviceRows = 672;
} // namespace

TEST(GSTextureCacheGrid, SpanIsTheDifferenceOfItsTwoEdges)
{
	ASSERT_EQ(GSTextureCache::ScaleNativeToDevice(kRows, kScale), kDeviceRows);

	// Native rows [1, 448) own device rows [2, 672): 670 of them. ceil(447 * 1.5) says 671.
	EXPECT_EQ(GSTextureCache::ScaleNativeSpanToDevice(1, 447, kScale), 670);
	// Native rows [0, 447) own device rows [0, 671).
	EXPECT_EQ(GSTextureCache::ScaleNativeSpanToDevice(0, 447, kScale), 671);

	// Spans that tile a range tile its device range: no device row is claimed twice or skipped.
	int total = 0;
	for (int y = 0; y < kRows; y += 7)
		total += GSTextureCache::ScaleNativeSpanToDevice(y, std::min(7, kRows - y), kScale);
	EXPECT_EQ(total, kDeviceRows);
}

TEST(GSTextureCacheGrid, SpanAtIntegerScaleIsTheProduct)
{
	for (int start = 0; start < 64; start++)
	{
		for (int len = 0; len < 64; len++)
		{
			EXPECT_EQ(GSTextureCache::ScaleNativeSpanToDevice(start, len, 2.0f), len * 2);
			EXPECT_EQ(GSTextureCache::ScaleNativeSpanToDevice(start, len, 3.0f), len * 3);
		}
	}
}

// A one-row scroll: native rows [1, 448) move up to [0, 447). Both rects must lie inside the
// 672-row texture, or the move is refused and falls to the CPU path.
TEST(GSTextureCacheGrid, OneRowScrollFitsItsTexture)
{
	const GSTextureCache::DeviceMove move = GSTextureCache::ScaleMoveToDevice(0, 1, 0, 0, 640, kRows - 1, kScale);

	EXPECT_LE(move.src.w, kDeviceRows);
	EXPECT_LE(move.dst.w, kDeviceRows);
	EXPECT_EQ(move.src.y, 2);
	EXPECT_EQ(move.dst.y, 0);
	EXPECT_EQ(move.src.height(), move.dst.height());
	EXPECT_EQ(move.src.width(), move.dst.width());
}

// Whatever the offsets, a move never reads a device pixel its source rectangle does not own and
// never writes one its destination rectangle does not own.
TEST(GSTextureCacheGrid, MoveStaysInsideBothOwnedRects)
{
	for (const float scale : {1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f})
	{
		for (int sy = 0; sy < 8; sy++)
		{
			for (int dy = 0; dy < 8; dy++)
			{
				for (int h = 1; h < 12; h++)
				{
					const GSTextureCache::DeviceMove move = GSTextureCache::ScaleMoveToDevice(3, sy, 5, dy, 17, h, scale);
					const int src_top = GSTextureCache::ScaleNativeToDevice(sy, scale);
					const int src_bottom = GSTextureCache::ScaleNativeToDevice(sy + h, scale);
					const int dst_top = GSTextureCache::ScaleNativeToDevice(dy, scale);
					const int dst_bottom = GSTextureCache::ScaleNativeToDevice(dy + h, scale);

					ASSERT_EQ(move.src.y, src_top);
					ASSERT_EQ(move.dst.y, dst_top);
					ASSERT_LE(move.src.w, src_bottom) << "scale " << scale << " sy " << sy << " dy " << dy << " h " << h;
					ASSERT_LE(move.dst.w, dst_bottom) << "scale " << scale << " sy " << sy << " dy " << dy << " h " << h;
					ASSERT_EQ(move.src.height(), move.dst.height());
					ASSERT_EQ(move.src.width(), move.dst.width());
					// At most one device row short of either owned span.
					ASSERT_GE(move.dst.height() + 1, dst_bottom - dst_top);
				}
			}
		}
	}
}
