// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// pcsx2-vurunner --vuopt-ir: validate vuopt's front end against microVU on
// captured programs. See VuOptMode.cpp.

#include "vu_capture.h"

#include <string>
#include <vector>

struct VuoptModeOptions
{
	int flagHack = 1;      // replay with vuFlagHack on (shipped default) or off
	bool dumpIr = false;   // print the SSA IR of each program
	bool verbose = false;  // print every mismatch line, not only the first few
	u32 cycleOverride = 0;
	bool noYield = false;
	u32 benchIters = 0;
	s32 lowerOnly = -1;    // --vuopt-only N: lower only compiled block N (bisecting); -1 = all
	s32 lowerLimit = -1;
	bool trace = false;    // --vuopt-trace: log block entries in both runs (debugging)   // --vuopt-limit N: lower only the first N compiled blocks    // --vuopt-run: steady-state PMU iterations per engine (0 = no bench)  // build the SSA without per-block yield checks (cost measurement)
};

// pcsx2-vurunner --vuopt-run: run each capture through microVU, then again
// with every block body vuopt can lower emitted from vuopt's IR, and compare
// the final state byte for byte; report retired host instructions for both.
int RunVuoptRun(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuoptModeOptions& opts,
	void (*applyConfig)(const vu_capture::CaptureRecord&));

int RunVuoptIr(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuoptModeOptions& opts,
	void (*applyConfig)(const vu_capture::CaptureRecord&));
