// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the clear rule that lets GSTextureCacheSW::Texture keep its pixel buffer across draws
// (GS/Renderers/SW/GSSwTextureDirty.h).
//
// The property under test is the one the rasterizer depends on: after a draw's Update(), the
// buffer must read exactly what a freshly zeroed allocation plus that draw's unswizzle would
// have left there, everywhere -- inside the written rect, in the rest of the nominal buffer, and
// in the x4 guard band the scanline code is known to sample into. The model below is a small
// buffer, a "block write" that stamps a recognisable pattern, and a byte-for-byte comparison
// against a second buffer that is memset and stamped fresh every time.
//
// Rides gs_vertex_tests -- the rule is a header-only struct, so it needs no extra linkage.

#include "GS/Renderers/SW/GSSwTextureDirty.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace
{
	// One "draw": a rect of blocks unswizzled into a buffer of some pitch. rows/row_bytes are
	// the block's shape, exactly as GSTextureCacheSW::Texture::Update derives them from psm.bs.
	struct Draw
	{
		size_t pitch;
		size_t capacity; // the nominal allocation this draw would have taken, <= buffer size
		size_t rows; // block height
		size_t row_bytes; // block width, in bytes
		size_t top_row; // first pixel row written
		size_t block_cols; // how many block columns
		size_t block_rows; // how many block rows
		size_t left_bytes; // byte column of the first block
		uint8_t stamp;
	};

	// Stamp what one block write puts down, and hand back the byte range it covered.
	GSSwTextureDirty::Range StampBlock(std::vector<uint8_t>& buf, size_t off, const Draw& d)
	{
		for (size_t row = 0; row < d.rows; row++)
		{
			const size_t at = off + row * d.pitch;
			if (at + d.row_bytes > buf.size())
				break;
			std::memset(&buf[at], d.stamp, d.row_bytes);
		}

		GSSwTextureDirty::Range r;
		r.begin = off;
		r.end = off + GSSwTextureDirty::BlockExtent(d.rows, d.pitch, d.row_bytes);
		return r;
	}

	// Run one draw over `buf`, recording what it wrote into `dirty`.
	void RunDraw(std::vector<uint8_t>& buf, GSSwTextureDirty& dirty, const Draw& d)
	{
		for (size_t by = 0; by < d.block_rows; by++)
		{
			const size_t row_base = (d.top_row + by * d.rows) * d.pitch;
			for (size_t bx = 0; bx < d.block_cols; bx++)
			{
				const size_t off = row_base + d.left_bytes + bx * d.row_bytes;
				if (off >= buf.size())
					continue;
				const GSSwTextureDirty::Range w = StampBlock(buf, off, d);
				dirty.Add(w.begin, w.end);
			}
		}
	}

	// The persistent road: zero what the previous draws dirtied, then unswizzle.
	void UpdatePersistent(std::vector<uint8_t>& buf, GSSwTextureDirty& dirty, const Draw& d)
	{
		const GSSwTextureDirty::Range c = dirty.ClearRange(buf.size());
		if (c.Size() > 0)
			std::memset(&buf[c.begin], 0, c.Size());
		dirty.MakeEmpty();

		RunDraw(buf, dirty, d);
	}

	// The road it has to match: a fresh zeroed allocation, then the same unswizzle.
	std::vector<uint8_t> UpdateFresh(size_t capacity, const Draw& d)
	{
		std::vector<uint8_t> buf(capacity, 0);
		GSSwTextureDirty ignored;
		RunDraw(buf, ignored, d);
		return buf;
	}
} // namespace

TEST(GSSwTextureDirty, EmptyRangeClearsNothing)
{
	GSSwTextureDirty d;
	EXPECT_TRUE(d.Empty());
	EXPECT_EQ(d.ClearRange(4096).Size(), 0u);
}

TEST(GSSwTextureDirty, AddIsTheHullOfWhatWasWritten)
{
	GSSwTextureDirty d;
	d.Add(100, 200);
	d.Add(500, 600);
	d.Add(300, 400);
	EXPECT_EQ(d.lo, 100u);
	EXPECT_EQ(d.hi, 600u);

	const GSSwTextureDirty::Range r = d.ClearRange(4096);
	EXPECT_EQ(r.begin, 100u);
	EXPECT_EQ(r.end, 600u);
}

TEST(GSSwTextureDirty, EmptyWriteIsNotDirty)
{
	GSSwTextureDirty d;
	d.Add(100, 100);
	EXPECT_TRUE(d.Empty());
}

TEST(GSSwTextureDirty, ClearRangeIsClampedToTheAllocation)
{
	// A wrong texture min/max can walk the write loop past the end of the buffer. The clear
	// must stay inside it.
	GSSwTextureDirty d;
	d.Add(3000, 9000);

	const GSSwTextureDirty::Range r = d.ClearRange(4096);
	EXPECT_EQ(r.begin, 3000u);
	EXPECT_EQ(r.end, 4096u);

	// Entirely past the end: nothing in the buffer was claimed, so nothing is cleared.
	GSSwTextureDirty past;
	past.Add(8000, 9000);
	EXPECT_EQ(past.ClearRange(4096).Size(), 0u);
}

TEST(GSSwTextureDirty, BlockExtentIsRowsAtStridePlusTheLastRow)
{
	// PSMCT32: 8 rows of 32 bytes.
	EXPECT_EQ(GSSwTextureDirty::BlockExtent(8, 512, 32), 7u * 512u + 32u);
	// PSMT8: 16 rows of 16 bytes.
	EXPECT_EQ(GSSwTextureDirty::BlockExtent(16, 128, 16), 15u * 128u + 16u);
	// A single-row block is just its own width.
	EXPECT_EQ(GSSwTextureDirty::BlockExtent(1, 512, 32), 32u);
}

TEST(GSSwTextureDirty, PersistentBufferEqualsFreshZeroPlusUnswizzle)
{
	// A 128x64 PSMCT32 texture: pitch 512, th 64, the x4 guard band on top.
	const Draw a{512, 512 * 64 * 4, 8, 32, 0, 4, 8, 0, 0xA5};
	// Then a smaller one into the same allocation, offset so it cannot cover the first.
	const Draw b{512, 512 * 32 * 4, 8, 32, 16, 2, 2, 128, 0x3C};

	std::vector<uint8_t> persistent(a.capacity, 0);
	GSSwTextureDirty dirty;

	UpdatePersistent(persistent, dirty, a);
	EXPECT_EQ(persistent, UpdateFresh(a.capacity, a));

	UpdatePersistent(persistent, dirty, b);
	const std::vector<uint8_t> fresh_b = UpdateFresh(a.capacity, b);
	EXPECT_EQ(persistent, fresh_b);

	// And the point of the whole thing: the guard band past the second draw's nominal size is
	// still zero, which is what the rasterizer samples when the min/max is wrong.
	for (size_t i = b.capacity; i < persistent.size(); i++)
		ASSERT_EQ(persistent[i], 0) << "guard band dirty at " << i;
}

TEST(GSSwTextureDirty, PitchChangeStillPresentsZeroesEverywhere)
{
	// 32-bit source (pitch 512), then an 8-bit one at the same TW (pitch 128), then back. The
	// clear is in raw bytes, so it does not care that the second draw reads the buffer in
	// different coordinates from the first.
	const size_t capacity = 512 * 64 * 4;

	const Draw wide{512, capacity, 8, 32, 0, 16, 8, 0, 0x11};
	const Draw narrow{128, 128 * 64 * 4, 16, 16, 0, 8, 4, 0, 0x22};

	std::vector<uint8_t> persistent(capacity, 0);
	GSSwTextureDirty dirty;

	UpdatePersistent(persistent, dirty, wide);
	EXPECT_EQ(persistent, UpdateFresh(capacity, wide));

	UpdatePersistent(persistent, dirty, narrow);
	EXPECT_EQ(persistent, UpdateFresh(capacity, narrow));

	UpdatePersistent(persistent, dirty, wide);
	EXPECT_EQ(persistent, UpdateFresh(capacity, wide));
}

TEST(GSSwTextureDirty, RandomisedDrawSequenceMatchesFreshZeroEveryTime)
{
	std::mt19937 rng(0x5741524d);
	const size_t capacity = 1024 * 32 * 4;

	std::vector<uint8_t> persistent(capacity, 0);
	GSSwTextureDirty dirty;

	for (int i = 0; i < 400; i++)
	{
		const size_t pitch = 64u << (rng() % 5); // 64..1024
		Draw d{};
		d.pitch = pitch;
		d.capacity = capacity;
		d.rows = (rng() % 2) ? 8 : 16;
		d.row_bytes = (d.rows == 8) ? 32 : 16;
		d.block_cols = 1 + rng() % (pitch / d.row_bytes);
		d.block_rows = 1 + rng() % 8;
		d.top_row = (rng() % 8) * d.rows;
		d.left_bytes = (rng() % (pitch / d.row_bytes)) * d.row_bytes;
		d.stamp = static_cast<uint8_t>(1 + (rng() % 255));

		UpdatePersistent(persistent, dirty, d);
		ASSERT_EQ(persistent, UpdateFresh(capacity, d)) << "draw " << i;
	}
}
