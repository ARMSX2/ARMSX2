// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The vu-aot sidecar of a persisted VU1 program: `<hash>.vuaot`, next to the program's
// `.vuprog` in the program cache's VU1 directory (so the directory's options sentinel covers it).
//
// This version holds the program's PLAN only: for the current generation of its regions, what
// the background compiler needs to build each one again (the root's PC and pipeline key, and for
// the generation the program's recorded JR/JALR targets), plus the micro-memory words the compile
// read, with their values. A later session that loads the program checks those words against
// live micro memory and sends the jobs to the worker at once, instead of waiting for the program
// to run kTierUpRuns times. No code is saved, so a stale file can cost speed, never correctness:
// every job is compiled from live micro memory and published under the live tier's own checks.
// Design: persistence-design.md (umbrella, campaigns/vu-aot-revival-eval), the "plan" half.
//
// Layout (little-endian, packed):
//   header (FileHeader)
//   jump targets: u32 count of jumping blocks, then per block: u32 pc, u32 n, n x u32 target
//   records: u32 count, then per record: RecordHeader, the read bitmap (kReadBitmapBytes), then
//            one u32 value per set bit, in word order
//   u64 XXH3-64 of everything before it
//
// Nothing here knows microVU: the caller passes the identity fields and the key size in.

#include "common/Pcsx2Defs.h"

#include <map>
#include <vector>

namespace vuaot::persist
{
	constexpr u32 kMagic = 0x544F4156u; // "VAOT"
	constexpr u32 kFormatVersion = 1;
	// The meaning of a plan: what a root (PC, key) and a read set stand for. Bumped when the region
	// builder's notion of a root or of the words a compile reads changes.
	constexpr u32 kVuAotAbiVersion = 1;

	constexpr u32 kMicroWords = 0x4000 / 4;
	constexpr u32 kReadBitmapBytes = kMicroWords / 8;
	constexpr u32 kKeyBytes = 96; // sizeof(microRegInfo), checked by the caller

	// Bounds the parser enforces before it allocates anything.
	constexpr u32 kMaxRecords = 4096;
	constexpr u32 kMaxJumpBlocks = kMicroWords / 2;
	constexpr u32 kMaxTargetsPerBlock = kMicroWords / 2;

	struct Identity
	{
		u64 contentHashLo = 0, contentHashHi = 0; // the program's (and the file name's)
		u64 sentinelLo = 0, sentinelHi = 0;       // the VU1 directory's options sentinel
		u64 buildLo = 0, buildHi = 0;             // the running build (the owner's default, 2026-10-15)
		u32 mvuAbi = 0;                           // kMvuCompilerAbiVersion
	};

	enum RecordKind : u8
	{
		kGeneration = 0,
		kRoot = 1,
	};

	enum RecordFlags : u8
	{
		kViaJump = 1,       // the generation was asked for by the JR/JALR slow path
		kDropSticky = 2,    // the job was made with the sticky drop allowed
		kStickyDropped = 4, // a region in it dropped the sticky STATUS byte
		kAccepted = 8,      // the region builder accepted the root
	};

	struct Record
	{
		u8 kind = kGeneration;
		u8 flags = 0;
		u32 pc = 0;
		u8 key[kKeyBytes] = {};
		// The words the compile read: bit w of `readBits` for micro-memory word w, then their values in
		// word order.
		std::vector<u8> readBits; // kReadBitmapBytes
		std::vector<u32> readValues;

		// Whether every word in the read set holds the saved value in `micro` (16 KB).
		bool MatchesMicro(const u8* micro) const;
		// Builds the read set from a per-word mask (non-zero = read) and the image the compile read.
		void SetReadSet(const u8* perWordMask, const u8* micro);
		u32 ReadCount() const { return static_cast<u32>(readValues.size()); }
	};

	struct File
	{
		Identity id;
		u32 tiers = 0;
		bool settled = false;
		std::map<u32, std::vector<u32>> jumpTargets;
		std::vector<Record> records; // publication order; the generation first when there is one
	};

	std::vector<u8> Serialize(const File& f);

	enum class ParseResult : u8
	{
		Ok,
		Truncated,
		BadMagic,
		BadFormatVersion,
		BadAbiVersion,
		BadMvuAbi,
		BadKeySize,
		OtherBuild,
		OtherSentinel,
		OtherProgram,
		BadChecksum,
		Malformed,
		kCount,
	};
	const char* ParseResultName(ParseResult r);

	// Parses and checks a file against the identity it must have. On anything but Ok, `out` is
	// unspecified. Never reads past `size`, and allocates only what the (bounded) counts declare.
	ParseResult Parse(const u8* data, size_t size, const Identity& expect, File& out);
} // namespace vuaot::persist
