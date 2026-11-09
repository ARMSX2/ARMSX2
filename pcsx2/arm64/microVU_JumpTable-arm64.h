// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Storage for microVU's jump tables (microBlock::jumpCache): one per block that ends in a JR or JALR, an
// entry for every possible target. A block reaches a handful of targets, so nearly all of its table stays
// zeros. Each table sits on page boundaries in anonymous memory the host maps on first write, so the pages
// no target falls in cost nothing, and a table handed back gives its pages back to the host.
//
// A table reads as zeros until an entry is written, which the JR/JALR dispatch takes as a miss. Emitted code
// loads a block's table through the block and indexes it directly, so the table must stay mapped for as long
// as the block exists; Free is for when the block goes.
namespace mVUJumpTable
{
	constexpr size_t kBytes = 24 * (0x4000 / 8); // sizeof(microJumpCache) * entries; checked in microVU-arm64.h

	// A table of zeros. Any thread.
	void* Alloc();
	// Hands a table back: its contents are gone and its pages returned. Any thread; null is fine.
	void Free(void* table);
} // namespace mVUJumpTable
