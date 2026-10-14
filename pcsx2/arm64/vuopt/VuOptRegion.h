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
		// Test only: break the rule above on purpose, to show the tests notice.
		enum class HandoffMutation : u8
		{
			None,
			IgnoreReads,    // an instance the block reads counts as dead if the block also writes it
			NoArrivalGuard, // the budget test does not keep one cycle in hand for the block's entry test
			AllDead,        // every ring instance is dead at a hand-off
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
		u64 bytes_ = 0;
		std::map<std::string, u32> declined_;

		friend class RegionBuilder;
		bool Decline(const std::string& why, std::string* out);
	};
} // namespace vuopt
