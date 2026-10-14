// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The live tier: vuopt running inside the emulator, behind
// EmuCore/CPU/Recompiler/EnableVUOpt (default off). The machinery lives in
// microVU's translation unit (microVU_VuOpt-arm64.inl), because it works on
// microVU's programs and block managers; this header is the policy and what
// the rest of the tree (and the test runners) may see of it.
//
// What it does, in one paragraph: microVU keeps compiling and running every VU1
// program as it always has. With the tier on, the dispatcher counts the runs of
// each program. A program that reaches kTierUpRuns is tiered up on the VU1
// thread at its next dispatch: the JR/JALR targets microVU has recorded for it
// (its jump caches) are collected, its microVU blocks are set aside (not freed:
// older code and stale jump caches can still reach them), and the program is
// compiled again with vuopt's compile hooks handed to microVU for that program,
// so its blocks come out as vuopt regions and framed bodies. microVU remains
// the fallback at every block boundary. A tiered program that meets new jump
// targets is compiled again, a bounded number of times.

#include "common/Pcsx2Defs.h"

#include <string>

namespace vuopt
{
	namespace live
	{
		// Runs of one program (dispatches that resolve to it) before it is tiered
		// up. 9n of compiler-design.md: 150-300 runs, a fraction of a second of
		// play, put 85-93% of GoW2's runs in vuopt and compile 140-430 programs
		// per scene.
		constexpr u32 kTierUpRuns = 200;

		// A tiered program looks for new jump targets every this many runs
		// (when any JR/JALR missed the jump cache since), and is compiled again
		// when it finds some, at most kMaxRetiers times and not within
		// kRetierMinRuns runs of its last compile. After kSettleChecks looks in a
		// row that found nothing it stops looking: the jump cache misses whenever
		// a game rewrites micro memory, so the miss count alone says little.
		constexpr u32 kRetierCheckMask = 0x3ff;
		constexpr u32 kMaxRetiers = 2;
		constexpr u32 kRetierMinRuns = 1024;
		constexpr u32 kSettleChecks = 3;

		// Recorded targets kept per jumping block and per program: a data-driven
		// jump (GTA, Burnout 3) can reach hundreds, and every one becomes a node.
		constexpr u32 kMaxTargetsPerJump = 48;
		constexpr u32 kMaxTargetsPerProgram = 512;

		struct Stats
		{
			u64 tierUps = 0;          // programs tiered up
			u64 retiers = 0;          // compiles of a tiered program for new jump targets
			u64 retierChecks = 0;     // times a tiered program looked for new targets
			u64 blocksSetAside = 0;   // microVU blocks moved out of the way by tier-ups
			u64 targetsRecorded = 0;  // jump targets handed to regions
			u64 jumpMisses = 0;       // JR/JALR the jump cache did not know (VU1)
			u64 rootsCompiled = 0;    // blocks offered to the region tier
			u64 regions = 0;
			u64 regionNodes = 0;
			u64 regionExits = 0;
			u64 regionBytes = 0;
			u64 bodyBlocks = 0;       // blocks offered to the body tier (the region declined them)
			u64 bodiesLowered = 0;
			u64 bodyBytes = 0;
			u64 bailCompiles = 0;     // regions' blocks microVU compiled at run time (budget shortfalls)
			// pcsx2-eerunner --count-vu1-ins only: user-mode instructions VU1's compile
			// sessions retired, for programs the tier took over and for the rest.
			u64 compileInsTiered = 0;
			u64 compileInsPlain = 0;
		};

		// Whether the tier runs this session (EnableVUOpt, VU1 recompiled, no
		// gamefix vuopt declines). Settled at init and at every reset.
		bool IsEnabled();

		Stats GetStats();
		// Human-readable counters, including every decline reason.
		std::string FormatStats();

#ifdef PCSX2_RECOMPILER_TESTS
		// Test runners only: tier programs up after `runs` dispatches instead of
		// kTierUpRuns (a low number puts almost every program through vuopt).
		void SetTierUpRuns(u32 runs);
#endif
	} // namespace live
} // namespace vuopt

#ifdef PCSX2_RECOMPILER_TESTS
namespace mvu_test_hooks
{
	// Test runners only (Linux): count the user-mode instructions the thread that
	// runs VU1 retires inside recMicroVU1::Execute; read the total and the number
	// of dispatches back after the run.
	void EnableVu1InstructionCount(bool on);
	void GetVu1InstructionCount(u64& instructions, u64& dispatches);
} // namespace mvu_test_hooks
#endif
