// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cstddef>

// Lets GSTextureCacheSW::Texture reuse its pixel buffer across draws instead of
// reallocating a zeroed one.
//
// Every byte the unswizzle does not write must read zero. The buffer is allocated
// four times larger than needed because the scanline samples past the texture's
// min/max, so the slack must be zeroes.
//
// Invariant: every byte of [0, capacity) outside [lo, hi) is zero.
//
// Established by zeroing the whole capacity on allocate or grow, maintained by
// widening [lo, hi) over every byte written, re-established by zeroing [lo, hi).
//
// One interval over-covers the written bytes, which is fine: clearing extra
// zero bytes is harmless. Being in raw bytes, it stays correct when pitch, size
// or format change between draws.
//
// The m_valid bitmap is tracked the same way, in words.
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
	// Clamped to the capacity: the write loop has no bound of its own, so a wrong
	// min/max can push `hi` past the allocation.
	Range ClearRange(size_t capacity) const
	{
		Range r;
		r.begin = std::min(lo, capacity);
		r.end = std::max(r.begin, std::min(hi, capacity));
		return r;
	}

	// Bytes one block write covers from its first byte: `rows` rows of `row_bytes`
	// at stride `pitch`, matching how GSBlock::Read* writes.
	static size_t BlockExtent(size_t rows, size_t pitch, size_t row_bytes)
	{
		return (rows - 1) * pitch + row_bytes;
	}
};
