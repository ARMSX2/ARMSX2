// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The size of a READBACK transfer: how many bytes a download texture's map actually spans, as
// against how many bytes one row of it is.
//
// GSDownloadTexture::GetTransferSize hands a caller three numbers -- a byte offset, ONE row's byte
// width, and a row count -- and rows sit GetMapPitch() apart, so the row width is not the size of
// anything the host reads. Three Vulkan sites consumed it as if it were: the host-cache invalidate
// in Map() and the TRANSFER_WRITE -> HOST_READ barrier on both copy roads. On coherent readback
// memory that is inert. On memory that is genuinely non-coherent -- which is what the Mali r44p1
// driver profile deliberately keeps, being far faster there than the coherent alternative -- rows
// after the first are read through a cache nothing invalidated, and stale cache atoms surface as
// corruption at 64-byte granularity in the readback buffer's own linear space.
//
// So what is pinned here is the arithmetic those sites now use. A pitched region runs from the
// first byte of its first row to the last byte of its LAST row: (rows - 1) * pitch + row_bytes.
// The two wrong answers both have a shape worth naming -- rows * row_bytes forgets the padding
// between rows and lands short, rows * pitch counts padding past the final row and lands long, off
// the end of a buffer sized to hold exactly the transfer.

#include "GS/Renderers/Common/GSTexture.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace
{
	/// Independent model: walk the rows, take the furthest byte any of them reaches.
	u32 SpanByWalkingRows(u32 pitch, u32 row_bytes, u32 rows)
	{
		u32 furthest = 0;
		for (u32 r = 0; r < rows; r++)
			furthest = std::max(furthest, r * pitch + row_bytes);
		return furthest;
	}
} // namespace

TEST(GSReadbackTransferSize, EmptyTransferSpansNothing)
{
	EXPECT_EQ(GSDownloadTexture::GetTransferRegionSize(2560, 2560, 0), 0u);
}

TEST(GSReadbackTransferSize, OneRowIsTheRowItself)
{
	// The only case where the row width IS the region, and the reason the old computation looked
	// right wherever anybody checked it by hand.
	EXPECT_EQ(GSDownloadTexture::GetTransferRegionSize(2560, 2560, 1), 2560u);
	EXPECT_EQ(GSDownloadTexture::GetTransferRegionSize(4096, 400, 1), 400u);
}

TEST(GSReadbackTransferSize, ScreenshotSizedTransferIsNotOneRow)
{
	// 640x480 RGBA8 with a row pitch that needs no padding -- the shape the hash grid's PNGs come
	// out of. One row is 2560 bytes; the region is 480 of them.
	constexpr u32 pitch = 2560;
	constexpr u32 row_bytes = 2560;
	constexpr u32 rows = 480;

	const u32 region = GSDownloadTexture::GetTransferRegionSize(pitch, row_bytes, rows);
	EXPECT_EQ(region, 1228800u);
	EXPECT_EQ(region, SpanByWalkingRows(pitch, row_bytes, rows));

	// Red against the computation this replaced: the invalidate covered 1/480th of what it read.
	EXPECT_NE(region, row_bytes);
	EXPECT_EQ(region, row_bytes * rows);
}

TEST(GSReadbackTransferSize, PaddedPitchStopsAtTheLastRowsLastByte)
{
	// A pitch wider than the row -- 100 RGBA texels rounded up to a 256-byte copy alignment. The
	// region must not run into the padding that follows the final row, or an invalidate sized from
	// it walks off the end of a buffer allocated to hold exactly the transfer.
	constexpr u32 pitch = 512;
	constexpr u32 row_bytes = 400;
	constexpr u32 rows = 50;

	const u32 region = GSDownloadTexture::GetTransferRegionSize(pitch, row_bytes, rows);
	EXPECT_EQ(region, 49u * 512u + 400u);
	EXPECT_EQ(region, SpanByWalkingRows(pitch, row_bytes, rows));
	EXPECT_LT(region, pitch * rows); // not rows * pitch
	EXPECT_GT(region, row_bytes * rows); // not rows * row_bytes either
}

TEST(GSReadbackTransferSize, BlockCompressedRowsAreBlockRows)
{
	// GetTransferSize already reports block rows and a block row's byte width for a compressed
	// format, so the region arithmetic needs nothing extra -- but pin the case, because the road
	// that carries it (a readback of a BC/ASTC surface) has no other gate.
	const GSTexture::BlockInfo bi = GSTexture::GetBlockInfo(GSTexture::Format::BC1);
	ASSERT_EQ(bi.width, 4u);
	ASSERT_EQ(bi.height, 4u);

	constexpr u32 texels = 64;
	const u32 block_rows = (texels + bi.height - 1) / bi.height;
	const u32 row_bytes = ((texels + bi.width - 1) / bi.width) * bi.bytes;
	const u32 pitch = 256; // padded well past the 128-byte block row

	const u32 region = GSDownloadTexture::GetTransferRegionSize(pitch, row_bytes, block_rows);
	EXPECT_EQ(block_rows, 16u);
	EXPECT_EQ(region, 15u * pitch + row_bytes);
	EXPECT_EQ(region, SpanByWalkingRows(pitch, row_bytes, block_rows));
}

TEST(GSReadbackTransferSize, NeverRunsPastTheBufferItWasAllocatedFor)
{
	// The property that makes widening the invalidate safe: a full-surface transfer at the pitch
	// the buffer was sized with fits inside that buffer, so no consumer of the region size can
	// hand the allocator a range past the end.
	for (const u32 align : {1u, 4u, 64u, 256u, 512u})
	{
		for (const u32 w : {1u, 63u, 100u, 640u, 1024u})
		{
			for (const u32 h : {1u, 2u, 17u, 480u})
			{
				const GSTexture::BlockInfo bi = GSTexture::GetBlockInfo(GSTexture::Format::Color);
				const u32 pitch = GSDownloadTexture::GetBufferSize(w, 1, GSTexture::Format::Color, align);
				const u32 row_bytes = ((w + bi.width - 1) / bi.width) * bi.bytes;
				const u32 rows = (h + bi.height - 1) / bi.height;
				const u32 region = GSDownloadTexture::GetTransferRegionSize(pitch, row_bytes, rows);

				EXPECT_EQ(region, SpanByWalkingRows(pitch, row_bytes, rows))
					<< "align=" << align << " w=" << w << " h=" << h;
				EXPECT_LE(region, GSDownloadTexture::GetBufferSize(w, h, GSTexture::Format::Color, align))
					<< "align=" << align << " w=" << w << " h=" << h;
			}
		}
	}
}

TEST(GSReadbackTransferSize, SweepAgreesWithWalkingTheRows)
{
	for (const u32 pitch : {1u, 7u, 64u, 256u, 2560u, 4096u})
	{
		for (const u32 rows : {0u, 1u, 2u, 3u, 31u, 480u, 1080u})
		{
			for (const u32 row_bytes : {1u, 7u, 64u, 255u})
			{
				const u32 rb = std::min(row_bytes, pitch); // a row never exceeds its pitch
				EXPECT_EQ(GSDownloadTexture::GetTransferRegionSize(pitch, rb, rows), SpanByWalkingRows(pitch, rb, rows))
					<< "pitch=" << pitch << " rows=" << rows << " row_bytes=" << rb;
			}
		}
	}
}
