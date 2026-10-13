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

		u32 Compiles() const { return compiles_; }
		u32 Regions() const { return regions_; }
		u32 Nodes() const { return nodes_; }
		u32 Exits() const { return exits_; }
		u64 Bytes() const { return bytes_; }
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
		u64 bytes_ = 0;
		std::map<std::string, u32> declined_;

		friend class RegionBuilder;
		bool Decline(const std::string& why, std::string* out);
	};
} // namespace vuopt
