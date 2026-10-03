// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#pragma once

#include "common/Pcsx2Types.h"
#include "common/Pcsx2Defs.h"

/**
 * @file ACRAM.h
 * Additional RAM memory living on the namco board
 * The RAM can be 32, 64, 96 or 128mb sized, or directly, not be there
 * System246 Rack A (DRIVING/GUN) : ACRAM is an external PCB of either 32 or 64mb, namco expansion board has two slots for them
 * System246 Rack B : Builtin on the namco board, 64mb version
 * System246 Rack C : Builtin on the board, 64mb version
 * System256 : ACRAM is no longer part of the system
 *
 * 64MB = 2 x 32MB banks. Each bank has independent DMA read/write pointers.
 * Ref: ps2sdk/iop/arcade/acram/src/ram.c, ps2sdk/iop/arcade/accore/src/dma.c
 *
 * Address map (base = 0x14000000, bank select = bit 21):
 *   [base + (bank<<21) + 0x60000]  Read pointer register
 *   [base + (bank<<21) + 0x70000]  Write pointer register
 *   [base + (bank<<21) + 0x100000] DMA IO port (written to IOP 0x1F801410)
 *
 * Register value = addr >> 11. Reconstructed: bank_base + (val << 11).
 */

#define ACRAM_ADDR_BASE   0x14000000
#define ACRAM_RANGE       0x1400
#define ACRAM_MAX_SIZE    (_64mb * 2)
#define ACRAM_BANK_SIZE   0x2000000  // 32MB per bank
#define ACRAM_NUM_BANKS   4          // most games use 1 bank; TK4 needs 2, Wangan 4 (its RAM expansion). Use the max.
#define ACRAM_REG_READ    0x60000
#define ACRAM_REG_WRITE   0x70000
#define ACRAM_REG_MASK    0x1FFFFF   // isolate register offset within bank

namespace ACRAM
{
    struct BankState {
        u32 read_addr;
        u32 write_addr;
    };

    u16 Read16(u32 addr);
    void Write16(u32 addr, u16 val);
    void DmaRead(u32* iop_buf, u32 size_bytes, int bank);
    void DmaWrite(u32* iop_buf, u32 size_bytes, int bank);
    int BankFromDmaTarget(u32 dma_target);
    extern BankState banks[ACRAM_NUM_BANKS];

    // The board's RAM exists only during an arcade session (see ACRAM.cpp).
    bool Allocate();  // false when the 128MB could not be had
    void Release();
    void Clear();
    u8* GetBuffer();  // null outside an arcade session
}
