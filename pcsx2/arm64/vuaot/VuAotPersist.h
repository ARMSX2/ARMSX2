// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The vu-aot sidecar of a persisted VU1 program: `<hash>.vuaot`, next to the program's
// `.vuprog` in the program cache's VU1 directory (so the directory's options sentinel covers it).
//
// It holds, for the current generation of the program's regions, one record per job published into
// it (the generation, then the roots, in publication order). Every record carries the job's PLAN:
// what the background compiler needs to build its region again (the root's PC and pipeline key, and
// for the generation the program's recorded JR/JALR targets), plus the micro-memory words the compile
// read, with their values. A record may also carry the job's CODE: the bytes the worker emitted, as it
// left them, with a fixup for every place that holds an address of the process that made it (a C
// helper, VU state, the program, a node's handle, a microVU block it links to, a placeholder for
// microVU code compiled at install). A later session that loads the program checks a record's words
// against live micro memory and either installs its code (persistence-design.md section 5, "install")
// or, when it cannot, sends its plan to the worker at once.
// Design: persistence-design.md (umbrella, campaigns/vu-aot-revival-eval).
//
// Layout (little-endian, packed):
//   header (FileHeader)
//   jump targets: u32 count of jumping blocks, then per block: u32 pc, u32 n, n x u32 target
//   records: u32 count, then per record: RecordHeader, the read bitmap (kReadBitmapBytes), then
//            one u32 value per set bit, in word order; with kHasCode, the code section (CodeHeader,
//            then each table in the order of its count)
//   declined roots: u32 count, then per root: u32 pc, the key (kKeyBytes)
//   u64 XXH3-64 of everything before it
//
// Nothing here knows microVU: the caller passes the identity fields in and resolves the fixups.

#include "common/Pcsx2Defs.h"

#include <array>
#include <map>
#include <vector>

namespace vuaot::persist
{
	constexpr u32 kMagic = 0x544F4156u; // "VAOT"
	constexpr u32 kFormatVersion = 3;
	// The meaning of a record: what a root (PC, key) and a read set stand for, and what region code
	// assumes of microVU (its boundary state, the bail record, the placeholders). Bumped with any change
	// to them. (While vu-aot changes daily the build id keys the files too: persistence-design.md,
	// decision 2.)
	constexpr u32 kVuAotAbiVersion = 2;

	constexpr u32 kMicroWords = 0x4000 / 4;
	constexpr u32 kReadBitmapBytes = kMicroWords / 8;
	constexpr u32 kKeyBytes = 96; // sizeof(microRegInfo), checked by the caller

	// Bounds the parser enforces before it allocates anything.
	constexpr u32 kMaxRecords = 4096;
	constexpr u32 kMaxJumpBlocks = kMicroWords / 2;
	constexpr u32 kMaxTargetsPerBlock = kMicroWords / 2;
	constexpr u32 kMaxCodeBytes = 16u << 20;
	constexpr u32 kMaxDeclinedRoots = 1u << 16;
	// Placeholder ids a record may name for microVU's stubs (the caller checks its own count).
	constexpr u32 kMaxStubs = 16;

	struct Identity
	{
		u64 contentHashLo = 0, contentHashHi = 0; // the program's (and the file name's)
		u64 sentinelLo = 0, sentinelHi = 0;       // the VU1 directory's options sentinel
		u64 buildLo = 0, buildHi = 0;             // the running build (the owner's default, 2026-10-15)
		u32 mvuAbi = 0;                           // kMvuCompilerAbiVersion
		u32 layoutHash = 0;                       // the struct offsets region code bakes in (the caller's hash)
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
		kHasCode = 16,      // the record carries the job's code (Record::code)
	};

	// Where a fixup's operand sits in the code.
	enum FixupForm : u8
	{
		kFormMovRun = 0, // MOVZ + MOVK x (width-1) over halfwords 0..width-1, one register
		kFormRel26,      // B / BL imm26
		kFormData64,     // an 8-byte data word
		kFormCount,
	};

	// What a fixup's operand stands for.
	enum FixupTarget : u8
	{
		kTargetIntra = 0,   // the record's own code + value
		kTargetPlaceholder, // placeholder `index` (RegionCode::ph): microVU code known only at install
		kTargetImage,       // the emulator module's base + value (C helpers, globals)
		kTargetData,        // the data arena's base + value
		kTargetBlockRef,    // RegionCode::blockRefs[index]: a microVU block the region links to and does not hold
		kTargetProgram,     // the microProgram the record is installed into
		kTargetNodeHandle,  // the handle of RegionCode::nodes[index] (a bail record's block pointer)
		kTargetCount,
	};

	struct Fixup
	{
		u32 offset = 0; // of the first instruction (or the data word) in the record's code
		u8 form = 0;
		u8 target = 0;
		u8 width = 0; // kFormMovRun: instruction count (1..4)
		u8 reserved = 0;
		u32 index = 0;
		u32 reserved2 = 0;
		s64 value = 0;
	};
	static_assert(sizeof(Fixup) == 24, "Fixup layout");

	// What a placeholder stands for: one of microVU's stubs (index = the caller's stub id), an exit
	// (a microVU block the region leaves to, exits[index]), a jump tail (microVU's dispatch for a JR/JALR
	// node, tails[index]) or a node an earlier record of the program built (pending[index]).
	enum PlaceholderKind : u8
	{
		kPhStub = 0,
		kPhExit,
		kPhTail,
		kPhPending,
		kPhCount,
	};
	struct Placeholder
	{
		u8 kind = 0;
		u8 reserved[3] = {};
		u32 index = 0;
	};
	static_assert(sizeof(Placeholder) == 8, "Placeholder layout");

	using Key = std::array<u8, kKeyBytes>;
	// A block by its start PC and pipeline key (an index into RegionCode::keys).
	struct BlockAt
	{
		u32 pc = 0;
		u32 key = 0;
	};
	struct CodeNode
	{
		u32 pc = 0;
		u32 key = 0;
		u32 entryOff = 0; // the node's entry, as an offset into the code
	};
	struct CodeTail
	{
		u32 node = 0; // the JR/JALR node whose jump tail it is
		u32 key = 0;  // its end state
	};
	struct CodeRange
	{
		u32 start = 0, last = 0; // a block from `start` through its last pair (mVUsetupRange's calls)
	};

	// A job's code as the worker left it, and what installing it needs.
	struct RegionCode
	{
		std::vector<u8> code;
		std::vector<Fixup> fixups;
		std::vector<Key> keys;              // the distinct pipeline keys the tables below name
		std::vector<CodeNode> nodes;        // registration order, the root first
		std::vector<BlockAt> exits;         // microVU blocks the region leaves to (compiled at install)
		std::vector<BlockAt> pending;       // nodes of earlier records the region links to
		std::vector<BlockAt> blockRefs;     // blocks of the program's graph the region links to
		std::vector<CodeTail> tails;
		std::vector<CodeRange> ranges;
		std::vector<Placeholder> ph;        // per placeholder id
		u32 regions = 0, regionNodes = 0, regionMerged = 0, regionExits = 0; // the job's counters (stats only)
		u64 regionBytes = 0;

		// The index of `k` in `keys`, adding it.
		u32 KeyIndex(const u8* k);
		// Instructions (or words) the fixup's patch rewrites.
		static u32 Span(const Fixup& f) { return f.form == kFormMovRun ? f.width : (f.form == kFormData64 ? 2 : 1); }
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
		// With kHasCode in flags.
		RegionCode code;

		// Whether every word in the read set holds the saved value in `micro` (16 KB).
		bool MatchesMicro(const u8* micro) const;
		// Builds the read set from a per-word mask (non-zero = read) and the image the compile read.
		void SetReadSet(const u8* perWordMask, const u8* micro);
		u32 ReadCount() const { return static_cast<u32>(readValues.size()); }
		bool HasCode() const { return (flags & kHasCode) != 0; }
		// Keeps the plan, drops the code.
		void DropCode()
		{
			flags &= static_cast<u8>(~kHasCode);
			code = RegionCode{};
		}
	};

	// A root the region builder declined in the current generation (a block microVU compiled in the tiered
	// program, whose job came back without a region): a later session does not ask for it again.
	struct DeclinedRoot
	{
		u32 pc = 0;
		Key key = {};
	};

	struct File
	{
		Identity id;
		u32 tiers = 0;
		bool settled = false;
		std::map<u32, std::vector<u32>> jumpTargets;
		std::vector<Record> records; // publication order; the generation first when there is one
		std::vector<DeclinedRoot> declinedRoots;
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
		OtherLayout,
		OtherSentinel,
		OtherProgram,
		BadChecksum,
		Malformed,
		kCount,
	};
	const char* ParseResultName(ParseResult r);

	// Parses and checks a file against the identity it must have. On anything but Ok, `out` is
	// unspecified. Never reads past `size`, and allocates only what the (bounded) counts declare. A
	// record's code section is checked for shape: every fixup and entry inside the code, every index
	// inside its table, forms and targets that go together. Whether its targets exist in this process
	// is the caller's to check.
	ParseResult Parse(const u8* data, size_t size, const Identity& expect, File& out);
} // namespace vuaot::persist
