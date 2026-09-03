// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cstddef>

// The bookkeeping that lets GSTextureCacheSW::Texture keep its pixel buffer across draws
// instead of freeing it and allocating a zeroed one again.
//
// WHAT THE BUFFER OWES THE RASTERIZER. Every byte of the buffer that the unswizzle does not
// write has to read zero. That is not a nicety: the buffer is deliberately allocated four
// times larger than the texture needs, because the texture min/max is wrong somewhere and the
// scanline code does sample into the slack, so the guard band has to be zeroes rather than
// whatever the allocator last had there. Today that is established by allocating a fresh
// buffer and memsetting the whole thing.
//
// THE INVARIANT that replaces the memset:
//
//     every byte of [0, capacity) outside [lo, hi) is zero.
//
// Established by memsetting the whole capacity when the buffer is allocated or grown.
// Maintained by widening [lo, hi) over every byte the unswizzle writes. Re-established by
// memsetting [lo, hi), after which the buffer is zero everywhere -- byte for byte what a fresh
// zeroed allocation of any size up to the capacity would have handed back.
//
// [lo, hi) is ONE interval, not the exact set of written bytes. That is still correct:
// clearing a superset of what was written leaves bytes that were already zero at zero. It is
// also why pitch does not appear anywhere here. The interval is in raw bytes, so a draw that
// changes the pitch -- or the width, height or pixel format behind it -- still gets a buffer
// that reads zero everywhere it does not write, with no coordinate conversion to get wrong.
//
// The same rule tracks the m_valid bitmap, in units of words instead of bytes.
struct GSSwTextureDirty
{
	struct Range
	{
		size_t begin = 0;
		size_t end = 0;

		size_t Size() const { return end - begin; }
	};

	size_t lo = 0;
	size_t hi = 0;

	bool Empty() const { return hi <= lo; }

	void MakeEmpty()
	{
		lo = 0;
		hi = 0;
	}

	// Record that [begin, end) was written.
	void Add(size_t begin, size_t end)
	{
		if (end <= begin)
			return;

		if (Empty())
		{
			lo = begin;
			hi = end;
			return;
		}

		lo = std::min(lo, begin);
		hi = std::max(hi, end);
	}

	// The half-open range to memset so a buffer of `capacity` reads all zero again.
	//
	// Clamped to the capacity because the write loop walks forward one block row at a time
	// with no bound of its own: a wrong min/max can push `hi` past the end of the allocation.
	// Clamping keeps the clear inside the buffer and changes nothing any in-bounds byte reads,
	// since a byte outside the allocation is not one the invariant ever claimed.
	Range ClearRange(size_t capacity) const
	{
		Range r;
		r.begin = std::min(lo, capacity);
		r.end = std::max(r.begin, std::min(hi, capacity));
		return r;
	}

	// Bytes one block write covers, counted from the block's first byte: `rows` rows of
	// `row_bytes` each, at stride `pitch`. That is the shape of every rtxbP -- GSBlock::Read*
	// walks columns down the block at dstpitch -- and it is the same shape the caller's own
	// `block_pitch = pitch * bs.y` step already assumes.
	static size_t BlockExtent(size_t rows, size_t pitch, size_t row_bytes)
	{
		return (rows - 1) * pitch + row_bytes;
	}
};
