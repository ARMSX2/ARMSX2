// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

static const u32 BIAS = 2;				// Bus is half of the actual ps2 speed
// EE bus clock. A console and a System 246 run at 294.912 MHz; an arcade System 256 runs at 4/3 of
// that and a Super System 256 at 3/2 (PCSX2x6). Only an arcade session ever changes it (VMManager), and
// the IOP clock follows at the same 8:1 ratio (psxReset), so every PS2CLK/PSXCLK ratio stays 8.
static constexpr u32 PS2CLK_DEFAULT = 294912000; //Hz	/* 294.912 MHz: PS2 console, System 246 */
static constexpr u32 PS2CLK_S256 = 393216000;    //Hz	/* 393.216 MHz: System 256 */
static constexpr u32 PS2CLK_SS256 = 442368000;   //Hz	/* 442.368 MHz: Super System 256 */
extern u32 PS2CLK;
extern u32 PSXCLK;	/* 36.864 MHz (49.152 on a System 256, 55.296 on a Super System 256) */


#include "Memory.h"
#include "R5900.h"
#include "Hw.h"
#include "Dmac.h"

#include "SaveState.h"
#include "DebugTools/Debug.h"

#include <string>

extern std::string ShiftJIS_ConvertString( const char* src );
extern std::string ShiftJIS_ConvertString( const char* src, int maxlen );
