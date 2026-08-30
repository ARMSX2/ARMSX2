// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "R3000A.h"
#include "R5900.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

namespace recompiler_tests {

// Fixed-size memory window captured alongside register state.
struct MemWindow
{
	u32 addr = 0;
	std::vector<u8> bytes;
};

// Captures everything a diff between JIT and interp needs to compare for the
// IOP.
struct IopSnapshot
{
	psxRegisters regs{};
	std::vector<MemWindow> mem_windows;

	// Fills `regs` from the global psxRegs and copies the given windows of
	// IOP RAM into `mem_windows`.
	static IopSnapshot Capture(const std::vector<MemWindow>& windows_to_capture);

	// Writes this snapshot's `regs` back to global psxRegs and restores the
	// memory windows.
	void Restore() const;

	// Zeroes every field and clears captured memory.
	static void ZeroGlobals();
};

// Produces a human-readable list of field-level differences between two IOP
// snapshots. Empty when the snapshots match across every field the harness
// considers architecturally significant (GPRs, HI/LO, CP0, PC, memory windows).
// Specifically ignores: `cycle`, `interrupt`, `pcWriteback`,
// `iopNextEventCycle`, `iopBreak`, `iopCycleEE`, `iopCycleEECarry`, `sCycle`,
// `eCycle` — these are dispatcher bookkeeping that differs between interp and
// JIT for reasons unrelated to ISA semantics.
std::vector<std::string> DiffIop(const IopSnapshot& a, const IopSnapshot& b);

void PrintIop(std::ostream& os, const IopSnapshot& s);

// ---------------------------------------------------------------------------
//  EE / R5900 snapshot
// ---------------------------------------------------------------------------
// Captures cpuRegs (128-bit GPRs, HI/LO, 32-bit CP0, PC, SA) and fpuRegs
// (32 FPR + 32 FPR control). Does NOT capture dispatcher bookkeeping
// (cycle, nextEventCycle, sCycle, eCycle, interrupt, dmastall) — those
// differ between JIT and interp for reasons unrelated to ISA semantics.
struct EeSnapshot
{
	cpuRegisters regs{};
	fpuRegisters fprs{};
	// g_eeFprSlotsRelocated as it stood when `fprs` was copied: a snapshot taken
	// under one clamp mode and read under another decodes to garbage without it.
	// Read the FPRs through FprWord / AccWord, never fprs.fpr[i].Word().
	bool fprs_relocated = false;
	std::vector<MemWindow> mem_windows;

	u32 FprWord(u32 reg_idx) const;
	u32 AccWord() const;

	static EeSnapshot Capture(const std::vector<MemWindow>& windows_to_capture);
	// Puts the file back, converting the FPRs if the format has moved since.
	void Restore() const;
	static void ZeroGlobals();
};

std::vector<std::string> DiffEe(const EeSnapshot& a, const EeSnapshot& b);
void PrintEe(std::ostream& os, const EeSnapshot& s);

} // namespace recompiler_tests
