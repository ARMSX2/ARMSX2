// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <algorithm>

// How far a stream ring grows when it would otherwise block the GS thread on the GPU.
//
// A full ring has to wait for a submitted command buffer to retire before it can reuse that
// buffer's bytes. Every fence that has not retired is the command buffer being recorded or one of
// the two submitted before it, because activating a command buffer already waited for the one three
// submits back. None of those waits is free:
//
//  - on the command buffer being recorded (the frame's own data does not fit), it has to be
//    submitted first and the GPU drained, so CPU and GPU run one after the other;
//  - on the previous submit, the CPU waits for the GPU to finish the work it was just handed;
//  - on the one before that, the command-buffer rotation would wait for the same fence anyway, but
//    only after the buffer being recorded is submitted. A ring that waits mid-buffer takes that
//    wait up to a whole buffer's worth of CPU time early.
//
// So a ring that has headroom grows instead of waiting at all. A title whose frames fit the ring
// never waits on it and never grows; a title whose frames outgrow it pays the memory once. Each
// growth doubles, and at `max_size` the ring waits as it always did.
//
// Pure so the decision can be tested without a device. See gs_stream_ring_growth_tests.cpp.

namespace GSStreamRingGrowth
{
	/// The size to grow to so that `required` bytes can be placed without waiting, or 0 to wait
	/// instead: when the ring has no headroom, or not even `max_size` holds the request.
	constexpr u32 SizeInsteadOfWait(u32 size, u32 max_size, u32 required)
	{
		if (size == 0 || size >= max_size)
			return 0;

		u64 new_size = static_cast<u64>(size) * 2;
		while (new_size < required)
			new_size *= 2;
		new_size = std::min<u64>(new_size, max_size);
		return (new_size >= required) ? static_cast<u32>(new_size) : 0;
	}
} // namespace GSStreamRingGrowth
