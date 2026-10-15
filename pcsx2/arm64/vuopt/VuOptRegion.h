// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Milestone 4: vuopt regions. A region replaces microVU's per-block framing
// for a set of microVU blocks (graph nodes) that link to each other, so state
// can stay in host registers across their links.
//
// When microVU starts compiling a block, the test-only region hook
// (VuOptMvuTrace.h) asks this tier to take it. The tier follows microVU's own
// compile order (depth first, not-taken before taken) from that block,
// collecting every block that ends in a plain branch or conditional branch and
// that the lowering supports; those are the region's nodes, and each is
// registered in microVU's block manager under its own key, exactly where
// microVU would have registered it. Everything else the walk reaches is an
// exit: a block that ends the program, jumps through a register, carries a
// T/D/M bit, or is already compiled.
//
// Inside the region:
//   - every variable has one home for the whole region (HomeMap): the
//     busiest VF/ACC registers in NEON registers, the busiest VI and flag
//     words in general registers, the rest in microVU's boundary locations;
//     the cycle counter and the VU1 memory base sit in registers too;
//   - each node runs microVU's cycle test against the counter register, then
//     its body (VuOptLower, region mode), whose final values go straight to
//     the homes with microVU's link-time ring reorder and Q/P swap folded in;
//   - links are direct branches; a conditional link compares the condition
//     the body left in w8.
//
// Every boundary keeps microVU's contract, so microVU can take over at any of
// them: an entry stub per node loads the register homes from microVU's
// boundary locations (it is the node's hostEntry, so microVU's links, jump
// cache and the dispatcher enter there); a cycle-budget break stores them and
// leaves through microVU's cycleBreak exactly as microVU's own test would; a
// link to an exit stores them and jumps to microVU's block for that exit,
// compiled by microVU after the region.

#include "VuOptLower.h"
#include "VuOptMvuTrace.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vuopt
{
	class RegionTier
	{
	public:
		RegionTier(u32 vuIndex, const u8* micro)
			: vu_(vuIndex)
			, micro_(micro)
		{
		}

		// The region hook: true when a region rooted at `block` was emitted.
		bool OnCompile(const MvuBlockTrace& block, const MvuEmitEnv& env, std::string* why);

		// Drops the first-pass port (and its per-slot history, about 240 KB).
		// The live tier calls it when a compile session ends, so a tier that
		// lives as long as the emulator holds no analysis state between
		// sessions; the next compile starts a port with no history.
		void ResetAnalyzer() { an_.reset(); }

		u32 Compiles() const { return compiles_; }
		u32 Regions() const { return regions_; }
		u32 Nodes() const { return nodes_; }
		u32 Exits() const { return exits_; }
		u64 Bytes() const { return bytes_; }
		// Sticky STATUS accumulation: regions that use it, regions that could have but
		// whose status chains the lowering cannot carry (FSSET kills), and the status
		// updates accumulated instead of packed.
		u32 AccRegions() const { return accRegions_; }
		u32 AccNonLinear() const { return accNonLinear_; }
		u32 AccAccumulated() const { return accAccumulated_; }
		// Regions with candidates where the accumulators would cost more than they save.
		u32 AccUnprofitable() const { return accUnprofitable_; }
		// Hand-off points (a link into a loop head, whose bail runs microVU's block) and the
		// ring instances (of twelve) they leave out, summed.
		// Static census of the regions' code: status readers (FSAND/FSEQ/FSOR), and those whose
		// mask selects a sticky bit (bits 6-11).
		// Compiles that dropped the sticky byte (relaxation 3), and those that could not because the micro
		// memory has a sticky reader.
		u32 StickyDropped() const { return stickyDropped_; }
		u32 StickyReaderImages() const { return stickyReaderImages_; }
		u32 StatusReaders() const { return statusReaders_; }
		u32 StickyReaders() const { return stickyReaders_; }
		// ... of which read the sticky I / D bits (STATUS bits 10-11), the ones a divide sets.
		u32 StickyDivReaders() const { return stickyDivReaders_; }
		// Budget bails the regions' code has taken, summed over the programs it saw: at an
		// entry from microVU, and at a loop head (read from the programs; only valid while
		// they are alive).
		void BailCounts(u64& entry, u64& loopHead) const;
		// Flag sinking: entries from microVU that built flag sources (counted by the regions' code, as the
		// bails are).
		u64 FsinkSynthRuns() const;
		// Test only: forget the programs counted so far (they have been freed).
		void ForgetPrograms() { progs_.clear(); }
		// Flag sinking: producers whose MAC / STATUS word a loop no longer builds (a FlagSrc each); the
		// places a word is built from a source instead ([0] a bail at a loop head, [1] a link out of a
		// loop, to microVU or to a node outside it, [2] a JR fallback); entry routines and edges that build
		// sources from words; links inside a loop whose two ends hold a variable differently.
		u32 FsinkPending() const { return fsinkPending_; }
		u32 FsinkMaterialise(int kind) const { return fsinkMat_[kind]; }
		u32 FsinkSynth() const { return fsinkSynth_; }
		u32 FsinkConflicts() const { return fsinkConf_; }
		u32 HandoffEdges() const { return handoffEdges_; }
		u32 HandoffRingsDead() const { return handoffRingsDead_; }
		const std::map<std::string, u32>& Declined() const { return declined_; }

		// Build only the first `maxRegions` regions (bisecting); -1 = all.
		s32 maxRegions = -1;
		// Print each region's nodes, homes and code.
		bool dump = false;
		// Homes: 0 = every variable in microVU's boundary location (links
		// only); otherwise at most this many NEON homes.
		s32 maxVHomes = 14;
		s32 maxGHomes = 6;
		// Milestone 9: list-schedule node bodies for in-order cores.
		bool schedule = true;
		// Milestone 10: each loop keeps its own homes (live ranges split at
		// loop boundaries).
		bool splitHomes = true;
		// Sticky STATUS accumulation (LowerEnv::stickyAcc) in regions that have
		// status updates it can take. Costs two vector homes and one general home.
		bool stickyAcc = true;
		// ... even where the static estimate says they would not pay (tests, A/B).
		bool accAlways = false;
		// Flag-ring instances at a hand-off to microVU (a bail at a loop head):
		// only the instances microVU's code for the block can still see are
		// handed over exactly. Off (A/B): all twelve, as before.
		bool handoffLiveness = true;
		// Relaxation 3 of the exactness contract: with no instruction in the micro memory reading a
		// sticky STATUS bit (FSAND/FSEQ/FSOR with a mask that selects bits 6-11), whatever the flag
		// hack (owner decision j, 2026-10-14), STATUS updates drop the sticky byte
		// (BuildOptions::dropSticky) and a divide's flags drop the sticky I / D bits. The STATUS a
		// program leaves can then differ from microVU's in bits 6-7 (the sticky zero and sign
		// bits) and 10-11 (the sticky invalid and divide-by-zero bits), which nothing in the corpora
		// reads. The live tier switches this off for the rest of the session when something does
		// (the tripwire, microVU_VuOpt-arm64.inl).
		bool dropSticky = true;
		int dropStickyMutation = 0; // test only: LowerEnv::dropStickyMutation
		// Flag sinking (compiler-design.md 9ae): inside a loop, a MAC word and the low part of a STATUS
		// word that nothing in the loop reads are not built; the FMAC result they come from is kept
		// instead (a flag source), and the word is built only where something outside the loop can see
		// it: a link out of the loop, a JR fallback, a bail at a loop head. An entry from microVU builds
		// the sources from microVU's words. Every word anything observes is the one microVU builds.
		// `flagSinkStatus` off keeps STATUS words (MAC only). STATUS sinking needs `dropSticky`.
		static constexpr bool kFlagSinkDefault = false;
		bool flagSink = kFlagSinkDefault;
		static constexpr bool kFlagSinkStatusDefault = false;
		bool flagSinkStatus = kFlagSinkStatusDefault;
		// Test only: break flag sinking on purpose (1 bails skip materialising, 2 links out of a loop skip
		// it, 3 sources are not normalised, 4 entries and edges skip synthesising, 5 a STATUS source is
		// packed with the other layout, 7 the in-loop readers are ignored).
		int flagSinkMutation = 0;
		// Test only: break the rule above on purpose, to show the tests notice.
		enum class HandoffMutation : u8
		{
			None,
			IgnoreReads, // an instance the block reads counts as dead if the block also writes it
			AllDead,     // every ring instance is dead at a hand-off
		};
		HandoffMutation handoffMutation = HandoffMutation::None;
		// Recorded JR/JALR targets: block start PC of the jump -> target PCs
		// seen at run time. A region dispatches these directly; any other
		// target goes through microVU's jump cache.
		std::map<u32, std::vector<u32>> jumpTargets;

		// Analysis only: count each node's executions and keep its code split
		// by the IR operation that emitted it; PrintProfile() writes both.
		bool profile = false;
		void PrintProfile(FILE* out) const;

	private:
		struct ProfSeg
		{
			const u8* start;
			const u8* end;
			std::string tag; // IR op name, "head", "tail"
		};
		struct ProfNode
		{
			u32 pc = 0;
			std::unique_ptr<u64> count;
			std::vector<ProfSeg> segs;
		};
		std::vector<ProfNode> prof_;
		u32 vu_;
		const u8* micro_;
		std::unique_ptr<PipeAnalyzer> an_;
		u32 compiles_ = 0, regions_ = 0, nodes_ = 0, exits_ = 0;
		u32 accRegions_ = 0, accNonLinear_ = 0, accAccumulated_ = 0, accUnprofitable_ = 0;
		u32 handoffEdges_ = 0, handoffRingsDead_ = 0;
		u32 statusReaders_ = 0, stickyReaders_ = 0, stickyDivReaders_ = 0;
		u32 stickyDropped_ = 0, stickyReaderImages_ = 0;
		u32 fsinkPending_ = 0, fsinkMat_[3] = {}, fsinkSynth_ = 0, fsinkConf_ = 0;
		std::vector<std::pair<const void*, s32>> progs_;
		void NoteProgram(const void* program, s32 off);
		bool ImageReadsSticky() const { return MicroReadsSticky(micro_, vu_); }

	public:
		// Whether a micro image (VU `vu`'s, 4 or 16 KB) has a sticky STATUS reader: the scan
		// relaxation 3 decides by. The live tier's background compile runs it on the job's copy
		// of micro memory, and again on live micro memory when it publishes a region that
		// dropped the sticky byte.
		static bool MicroReadsSticky(const u8* micro, u32 vu);

	private:
		u64 bytes_ = 0;
		std::map<std::string, u32> declined_;

		friend class RegionBuilder;
		bool Decline(const std::string& why, std::string* out);
	};
} // namespace vuopt
