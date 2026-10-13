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
};

int RunVuoptIr(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuoptModeOptions& opts,
	void (*applyConfig)(const vu_capture::CaptureRecord&));
