// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The program graph vu-aot compiles: every microVU block variant reachable
// from the program's entries, linked the way microVU links them.
//
// A node is a microVU block: a start PC plus the pipeline state it was entered with
// (its 96-byte key), running straight to a branch, an E-bit or a jump. Two entries into
// the same PC with different pipeline states are two nodes, as microVU compiles two
// copies. So every pipeline-time decision inside a node is a compile-time constant
// (validated against microVU, see VuAotPipe), joins whose incoming pipeline states differ
// are duplicated the way microVU duplicates them, and every node boundary is a point where
// microVU can take over with an identical state.
//
// Block lookup follows microBlockManager::search: an exact-match key compares
// all 96 bytes, any other compares the first 8 (minus the MAC bits under the
// flag hack) and returns the first block registered with that key. Which block
// that is depends on registration order, so the closure registers blocks in
// microVU's compile order (depth-first, not-taken before taken).

#include "VuAotPipe.h"

#include <utility>
#include <vector>

namespace vuaot
{
	struct GraphNode
	{
		u32 id = 0;
		u32 pc = 0;
		PipeState entry;
		BlockResult block;
		int taken = -1;                 // Branch / CondBranch taken successor
		int fall = -1;                  // CondBranch fall-through successor
		std::vector<int> jumpTargets;   // resolved Jump successors
		bool jumpUnresolved = false;    // a Jump target may be outside jumpTargets
		std::vector<int> preds;
		bool isRoot = false;
	};

	enum class JumpResolution : u8
	{
		Static,    // every value the jump register can hold is known
		Recorded,  // targets observed at run time (may be incomplete)
		Unresolved,
	};

	struct GraphOptions
	{
		PipeConfig cfg;
		// Program entries: PC + the pipeline state microVU enters it with
		// (mVU.prog.lpState, all zero after a normal E-bit end).
		std::vector<std::pair<u32, PipeState>> entries;
		// PCs observed as JR/JALR targets at run time.
		std::vector<u32> recordedJumpTargets;
		bool staticJumpResolution = true;
		u32 maxNodes = 50000;
	};

	struct GraphStats
	{
		u32 nodes = 0;               // microVU block variants
		u32 distinctBlockPCs = 0;    // distinct block start PCs
		u32 instructions = 0;        // sum of node lengths (pairs, with duplicates)
		u32 reachedPairs = 0;        // distinct pairs covered
		u32 pcBasicBlocks = 0;       // PC-level basic blocks (leaders in covered code)
		u32 edges = 0;
		u32 backEdges = 0;           // retreating edges whose target dominates the source
		u32 irreducibleEdges = 0;    // retreating edges whose target does not
		u32 pcBackEdges = 0;         // the same two on the PC-level CFG
		u32 pcIrreducibleEdges = 0;
		u32 jumpSites = 0;           // distinct JR/JALR PCs
		u32 jumpSitesStatic = 0;
		u32 jumpSitesRecorded = 0;
		u32 jumpSitesUnresolved = 0;
		u32 condBranches = 0, branches = 0, ebitExits = 0, tbitExits = 0, evilJumps = 0;
		bool truncated = false;      // hit maxNodes
	};

	struct ProgramGraph
	{
		std::vector<GraphNode> nodes;
		std::vector<int> roots;
		std::vector<JumpResolution> jumpResolution; // per node (meaningful for Jump nodes)
		GraphStats stats;

		int Find(u32 pc, const PipeState& state, bool flagHack) const;
	};

	ProgramGraph BuildProgramGraph(const u8* micro, const GraphOptions& opts);
} // namespace vuaot
