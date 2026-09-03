// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// What a non-coherent stream ring still owes the GPU, as at most two byte ranges.
//
// On the cached non-coherent road (GSStreamRingMemoryPolicy.h) every committed region has to be
// cleaned out of the CPU's caches before the GPU reads it. That clean used to be issued once per
// commit, inside VKStreamBuffer::CommitMemory. Cache maintenance is priced per byte and the call
// is priced per call, so a title with many small commits pays the call price many times over the
// same bytes: on the MQ65 the per-commit road won 20-30% of the GS thread on the upload-heavy
// titles and lost 10.1% on ac3 (1,626 draws a frame) and 5.8% on outrun-a (650 draws), entirely
// CPU-side.
//
// The GPU cannot read any of it until the queue submission that consumes it, so one clean per ring
// per submit covers exactly the same bytes. This tracks which bytes those are.
//
// Two ranges, not one, because the ring can wrap between two submits: the writes since the last
// flush are then a high range running to the end of the buffer and a low range starting at zero.
// A second wrap would need a third range and cannot happen -- reusing the high range's bytes means
// the GPU has consumed them, which means a fence completed, which means a submit, which means a
// flush -- but Add reports the overflow rather than assuming it, and the caller flushes early.
//
// Gaps inside a range are absorbed rather than split out. The gaps are alignment padding between
// one commit and the next, cleaning them writes back whatever the CPU last put there and never
// invalidates anything, and nothing but the CPU ever writes a ring. Splitting them out would trade
// the byte price this is trying to keep for the call price it is trying to lose.
//
// Pure and backend-neutral so the wrap and coalescing cases can be pinned on a host whose own
// rings are coherent and never take this road. See gs_stream_ring_flush_tests.cpp.

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

	/// How many committed regions the pending ranges cover. The number of flushes the per-commit
	/// road would have issued for the same bytes, which is the whole point of the comparison.
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
			// Forward of the newest range's start: extend it, swallowing any alignment gap. The
			// max is for a commit that lands entirely inside what is already pending, which the
			// ring does not do but which must not shrink the range if it ever does.
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
