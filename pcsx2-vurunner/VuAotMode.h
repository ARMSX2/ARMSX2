// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// pcsx2-vurunner --vuaot-ir: validate vu-aot's front end against microVU on
// captured programs. See VuAotMode.cpp.

#include "vu_capture.h"

#include <string>
#include <vector>

struct VuAotModeOptions
{
	int flagHack = 1;      // replay with vuFlagHack on (shipped default) or off
	bool dumpIr = false;   // print the SSA IR of each program
	bool verbose = false;  // print every mismatch line, not only the first few
	u32 cycleOverride = 0;
	bool noYield = false;  // --vuaot-no-yield: build the SSA without per-block yield checks (cost measurement)
	u32 benchIters = 0;    // --vuaot-bench N: steady-state PMU iterations per engine (0 = no bench)
	long traceDump = -1;   // --vuaot-trace-dump N: with --vuaot-trace, print the state at block entry N in both runs
	std::string lowerSkip; // --vuaot-skip a,b,...: leave these compiled blocks to microVU (bisecting)
	bool region = false;   // --vuaot-region: vu-aot regions, framed bodies for the rest
	bool regionBg = false; // --vuaot-region-bg: the same regions, each built on the live tier's worker thread and copied in
	s32 regionLimit = -1;  // --vuaot-region-limit N: build only the first N regions
	s32 vHomes = 14;       // --vuaot-vhomes N: at most N NEON homes per region
	bool schedule = true;  // --vuaot-no-sched: regions keep each node's operations in program order
	bool splitHomes = true; // --vuaot-no-split: one home map per region (A/B)
	bool wMemHomes = true;  // --vuaot-no-wmem: every vector home keeps its w lane in the register (A/B)
	bool stickyAcc = true;  // --vuaot-no-acc: every status update builds its instance (no sticky accumulators; A/B)
		bool accAlways = false; // --vuaot-acc-always: the accumulators even where the estimate says they would not pay (A/B)
	bool handoff = true;    // --vuaot-no-handoff: every flag-ring instance exact at a bail to microVU (A/B)
	bool dropSticky = true; // --vuaot-no-dropsticky: relaxation 3 off (the sticky bits of STATUS kept, flag hack on or off)
	bool merge = true;      // --vuaot-no-merge: region variant merging off (RegionTier::merge, A/B)
	// --vuaot-mutation NAME: break one relaxation on purpose, to show the observable gate catches it
	// (handoff-ignore-reads, handoff-all-dead, sticky-hi, sticky-pack, sticky-div).
	int handoffMutation = 0;
	int dropStickyMutation = 0;
	bool observe = false;   // --vuaot-observe: compare observable state (the exactness contract), classify the byte differences,
	                        // and continue each run through its budget stops with --vuaot-resumes re-entries
	u32 resumes = 6;
	bool profile = false;  // --vuaot-profile: per region node, its execution count and its code by IR op ("N"/"I" lines)
	s32 gHomes = 6;        // --vuaot-ghomes N: at most N general-register homes per region
	s32 lowerOnly = -1;    // --vuaot-only N: lower only compiled block N (bisecting); -1 = all
	s32 lowerLimit = -1;   // --vuaot-limit N: lower only the first N compiled blocks
	bool trace = false;    // --vuaot-trace: log block entries in both runs (debugging)
};

// pcsx2-vurunner --vuaot-run: run each capture through microVU, then again
// with every block body vu-aot can lower emitted from vu-aot's IR, and compare
// the final state byte for byte; report retired host instructions for both.
int RunVuAotRun(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuAotModeOptions& opts,
	void (*applyConfig)(const vu_capture::CaptureRecord&));

int RunVuAotIr(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuAotModeOptions& opts,
	void (*applyConfig)(const vu_capture::CaptureRecord&));
