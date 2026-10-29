// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade SRAM. Only reached while an arcade game runs (Arcade::IsActive(),
// common/ARCADE.h).

#pragma once

#include "MemoryTypes.h"
#include "Config.h"
#include "common/Pcsx2Types.h"
#include "common/Pcsx2Defs.h"
#include <string>

#define ACSRAM_ADDR_BASE 0x12500000
#define ACSRAM_RANGE     0x1250
#define ACSRAM_MAX_SIZE  _32kb

#define ACSRAM_LOG(fmt, ...) if (EmuConfig.Arcade.SRAMVerboseReads) Console.WriteLn(Color_Gray, "SRAM:" fmt __VA_OPT__(,) __VA_ARGS__)

namespace ACSRAM
{
    // ACSRAM reads may be detected as 8-bit MMIO, yet the address increments as if it was 16-bit.
    // Homebrew ACSRAM does not do this: it goes over 16-bit MMIO like the rest of the IRXes.
    u8 Read8(u32 addr);
    u16 Read16(u32 addr);
    void Write16(u32 addr, u16 val);

    extern u8 buffer[];
    extern std::string filepath;

    int ReadFile();
    int WriteFile();
    void Clear(u8 fillerbyte = 0x0);
}
