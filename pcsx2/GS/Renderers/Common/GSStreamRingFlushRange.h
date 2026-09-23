// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// What a non-coherent stream ring still owes the GPU, as at most two byte ranges.
//
// On the cached non-coherent road (GSStreamRingMemoryPolicy.h) every committed region must be
// cleaned from the CPU caches before the GPU reads it. Cache maintenance costs per byte, the call
// costs per call, and a title with many small commits pays the call cost many times; so the clean
// is issued once per ring per submit rather than per commit. The GPU cannot read the data before
// that submit, so the same bytes are covered.
//
// Two ranges because the ring can wrap between submits: a high range to the end of the buffer and
// a low range from zero. A second wrap cannot happen (reusing the high range's bytes requires a
// completed fence, hence a submit, hence a flush), but Add reports overflow instead of assuming it,
// and the caller flushes early.
//
// Gaps inside a range (alignment padding between commits) are absorbed. Cleaning them only writes
// back what the CPU last put there and invalidates nothing, and only the CPU writes a ring.
//
// Pure and backend-neutral so wrap and coalescing can be tested on hosts with coherent rings. See
// gs_stream_ring_flush_tests.cpp.

struct GSStreamRingFlushRanges
{
	struct Range
	{
		/// Half-open, [begin, end), in bytes from the start of the ring.
		u32 begin = 0;
		u32 end = 0;

		__fi u32 size() const { return end - begin; }
	};

	static constexpr u32 MAX_RANGES = 2;

	Range ranges[MAX_RANGES] = {};
	u32 count = 0;

	/// How many committed regions the pending ranges cover (the flushes a per-commit clean would
	/// have issued).
	u32 commits = 0;

	__fi bool IsEmpty() const { return count == 0; }

	__fi void Reset()
	{
		count = 0;
		commits = 0;
	}

	/// Records a committed region. Returns false when it cannot be represented alongside what is
	/// already pending -- the caller must flush, Reset(), and call again, which then always fits.
	__fi bool Add(u32 offset, u32 bytes)
	{
		if (bytes == 0)
			return true;

		const u32 end = offset + bytes;
		if (count == 0)
		{
			ranges[0] = {offset, end};
			count = 1;
			commits = 1;
			return true;
		}

		Range& last = ranges[count - 1];
		if (offset >= last.begin)
		{
			// At or past the newest range's start: extend it, absorbing any alignment gap. The max
			// keeps a commit inside the pending range from shrinking it.
			last.end = (end > last.end) ? end : last.end;
			commits++;
			return true;
		}

		// Behind it, so the ring wrapped since that range was written.
		if (count == MAX_RANGES)
			return false;

		ranges[count++] = {offset, end};
		commits++;
		return true;
	}
};
