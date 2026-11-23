// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <vector>

// The VU1 chunk store: compiled VU1 code kept by what the compiler read to
// produce it, so a routine a game uploads again, at the same address or at
// another, runs the code compiled the first time.
//
// A chunk is the code one outermost mVUcompile emits: a root block and the
// blocks compiled inside it. The store keeps one entry per distinct chunk:
//
//   key     the root block's entry state, and every micro-memory word the
//           compile read, as an offset from the root, with a mask of the bits
//           it read at compile time (under the IbitHack gamefix some
//           immediates are read when the block runs instead);
//   body    where the chunk's code is, its blocks (offset, entry and end
//           state, code), its branches to blocks outside the chunk (offset,
//           state searched with), the ranges it added, its slot count.
//
// An outermost mVUcompile first looks for an entry whose key matches micro
// memory at the address it was asked for. A match is installed as a new
// placement of the entry's code: an entry stub and a block object per block
// in the program being compiled, and a run of placement slots (see
// microVU::placeSlots). Nothing is compiled. An entry is not used where one of
// its blocks already exists, where a block it branches to does not exist yet,
// or where it would not fit inside micro memory. Otherwise the JIT compiles,
// and what it compiled becomes an entry. One root pair and entry state keeps
// its 16 entries used last.
//
// An entry also holds the VU AOT tier's bodies for the routine: its regions
// and the blocks microVU compiled around them, recorded from one build and
// placed the same way, in preference to the plain body
// (vuaot/microVU_VuAot-arm64.inl). The store is not asked while that build
// runs.
//
// The store is emptied by every recompiler reset. With the VU program cache
// on, each body is also a record of the cache's pack on disk, and the first
// lookup of a routine after a reset, in this session or a later one, makes
// its bodies again from the records (microVU_ChunkPack-arm64.inl).
namespace mVUChunkStore
{
	struct Stats
	{
		// Outermost VU1 compiles asked for, and how many of them were not
		// looked up: the build of a tiered body, the compile of the block a
		// region bails to, or any compile while a test hook is installed.
		u64 episodes = 0;
		u64 skipped = 0;
		// Served by an install.
		u64 hits = 0;
		// Lookups that fell through to the JIT, by how far the best entry got.
		u64 missNoEntry = 0;     // no entry with this root pair and entry state
		u64 missBounds = 0;      // the entry would leave micro memory here
		u64 missReadSet = 0;     // a word the compile read differs
		u64 missBlockExists = 0; // the program already has one of its blocks
		u64 missLinkTarget = 0;  // the program lacks a block it branches to
		u64 missNoRoom = 0;      // out of placement slots or code cache
		// Compiled chunks offered to the store.
		u64 fed = 0;             // added as a new entry
		u64 feedRepeats = 0;     // already held
		u64 feedWrapped = 0;     // not kept: ran across the end of micro memory
		u64 feedOther = 0;       // not kept: VU AOT's, or code no slot describes
		u64 feedEvicted = 0;     // plain bodies dropped to make room for a new one (mVUchunkMaxPlainPerHead)
		// Host code the JIT emitted for VU1, the entries' code the installs
		// reused, and the stubs the installs emitted.
		u64 codeBytesCompiled = 0;
		u64 codeBytesInstalled = 0;
		u64 stubBytes = 0;
		// Plain bodies held now, and the bytes of code they describe.
		u64 entries = 0;
		u64 entryCodeBytes = 0;
		// VU AOT tiered bodies: build episodes run (each is one outermost
		// compile, counted in `skipped`), bodies held and their code, lookups
		// served by one, placements of one (lookups and re-aims), and copies
		// that were on microVU's code and were moved onto one.
		u64 tieredBuilds = 0;
		u64 tieredBodies = 0;
		u64 tieredCodeBytes = 0;
		u64 tieredHits = 0;
		u64 tieredPlaced = 0;
		u64 tieredReAimed = 0;
		// The disk pack. Bodies written to it and their record bytes; bodies not
		// written because a copy could not be patched to run in another
		// process; bodies made from its records, their code, and the time
		// making them took; records refused.
		u64 packSaved = 0;
		u64 packSavedBytes = 0;
		u64 packUnpackable = 0;
		u64 packLoaded = 0;
		u64 packLoadedCodeBytes = 0;
		u64 packLoadNs = 0;
		u64 packRejected = 0;
		// The first placement of a tiered body in this process: how many
		// outermost compiles had been asked for by then, and how many of them
		// the JIT compiled. 0 when none was placed.
		u64 firstTieredEpisode = 0;
		u64 firstTieredCompiles = 0;
	};
	Stats GetStats();

#ifdef PCSX2_RECOMPILER_TESTS
	// Whether `data` passes the checks a pack record has to pass before a body
	// is made of it.
	bool TestParseRecord(const u8* data, size_t size);
	// The JR/JALR targets (byte addresses, ascending) the blocks at `blockPC`
	// of the program VU1 ran last recorded as their own (microBlock::jumpTargets).
	std::vector<u32> TestJumpTargets(u32 blockPC);
	// The routines (heads) the store holds.
	size_t TestHeadCount();
#endif
} // namespace mVUChunkStore
