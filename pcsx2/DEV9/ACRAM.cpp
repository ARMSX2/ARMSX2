// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).


#include "common/Console.h"
#include "ACRAM.h"
#include "MemoryTypes.h"
#include "IopMem.h"
#include <cstdlib>
#include <cstring>

#include "Config.h"
#define ACRAM_LOG(fmt, ...) if (EmuConfig.Arcade.RAMVerboseReads) Console.WriteLn(Color_Gray, "ACRAM:" fmt __VA_OPT__(,) __VA_ARGS__)

#define OOB_REPORT(T) Console.Error("%s: out of bound index: %08X", __FUNCTION__, T)
#define GET_RAM_OFF(addr) (((addr) - ACRAM_ADDR_BASE) / 2) // u8 buffer on u16 MMIO, halve the address to get real offset

ACRAM::BankState ACRAM::banks[ACRAM_NUM_BANKS] = {};

// In PCSX2x6 the board's RAM is part of the IOP's memory block. Here a console never has it: it is
// allocated when an arcade session starts and freed when it ends (calloc'd, so pages nobody touches
// cost nothing), and cleared with the IOP's memory on every reset.
static u8* s_ram = nullptr;

bool ACRAM::Allocate()
{
    if (!s_ram)
        s_ram = static_cast<u8*>(std::calloc(ACRAM_MAX_SIZE, 1));
    std::memset(banks, 0, sizeof(banks));
    return s_ram != nullptr;
}

void ACRAM::Release()
{
    std::free(s_ram);
    s_ram = nullptr;
    std::memset(banks, 0, sizeof(banks));
}

void ACRAM::Clear()
{
    // A fresh allocation instead of a memset: zeroed pages that stay uncommitted until the game uses them,
    // where a memset would make the whole 128MB resident on every reset.
    if (s_ram)
    {
        std::free(s_ram);
        s_ram = static_cast<u8*>(std::calloc(ACRAM_MAX_SIZE, 1));
        if (!s_ram)
            Console.Error("ACRAM: could not reallocate the arcade board's RAM on reset.");
    }
    std::memset(banks, 0, sizeof(banks));
}

u8* ACRAM::GetBuffer()
{
    return s_ram;
}

u16 ACRAM::Read16(u32 addr) {
    u32 offset = addr - ACRAM_ADDR_BASE;
    u32 reg = offset & ACRAM_REG_MASK;
    // FPGA status registers: TK5DR polls reg 0x00-0x1F during boot, expecting 0x50 (ready).
    if (reg < 0x20)
        return 0x50;
    u32 off = GET_RAM_OFF(addr);
    if (off < ACRAM_MAX_SIZE)
        return s_ram ? s_ram[off] : 0;
    OOB_REPORT(addr);
    return 0;
}

// Track DMA pointers per bank (ACRAM_NUM_BANKS): without this, one bank's streaming writes clobber
// another bank's data/pointers and the game hangs at load. Ref: ps2sdk acram/src/ram.c
void ACRAM::Write16(u32 addr, u16 val) {
    u32 offset = addr - ACRAM_ADDR_BASE;
    int bank = (offset >> 21) & (ACRAM_NUM_BANKS - 1); // address bits 21+ pick the bank
    u32 reg = offset & ACRAM_REG_MASK;
    u32 bank_base = bank * ACRAM_BANK_SIZE;

    if (reg >= ACRAM_REG_READ && reg < ACRAM_REG_WRITE) {
        // One register write carries a full address, split in two:
        //   address = (value << 11) + (register offset & 0x7FC)
        // The value picks the 2KB page, the register offset picks the spot
        // inside that page (that's how the driver works — ps2sdk acram ram.c).
        banks[bank].read_addr = bank_base + ((u32)val << 11) + (reg & 0x7FC);
        return;
    } else if (reg >= ACRAM_REG_WRITE && reg < 0x80000) {
        banks[bank].write_addr = bank_base + ((u32)val << 11) + (reg & 0x7FC);
        return;
    } else if (reg >= 0x20000 && reg < ACRAM_REG_READ) {
        // Size/config registers: control only, nothing to store — the actual
        // transfer size comes from the DMA8 BCR.
        return;
    }

    u32 off = GET_RAM_OFF(addr);
    if (off < ACRAM_MAX_SIZE)
    {
        if (s_ram)
            s_ram[off] = (u8)(val & 0xFF);
    }
    else
        OOB_REPORT(addr);
}

int ACRAM::BankFromDmaTarget(u32 dma_target) { // same bank select as Write16
    return ((dma_target - ACRAM_ADDR_BASE) >> 21) & (ACRAM_NUM_BANKS - 1);
}

void ACRAM::DmaRead(u32* iop_buf, u32 size_bytes, int bank) {
    u32& addr = banks[bank].read_addr;
    addr &= (ACRAM_MAX_SIZE - 1);
    ACRAM_LOG("DMARead  addr:%8X size:%8X bank:%d", addr, size_bytes, bank);
    if (!s_ram) {
        std::memset(iop_buf, 0, size_bytes);
    } else if (addr + size_bytes <= ACRAM_MAX_SIZE) {
        std::memcpy(iop_buf, &s_ram[addr], size_bytes);
    } else {
        u32 first = ACRAM_MAX_SIZE - addr;
        std::memcpy(iop_buf, &s_ram[addr], first);
        std::memcpy((u8*)iop_buf + first, &s_ram[0], size_bytes - first);
    }
    addr = (addr + size_bytes) & (ACRAM_MAX_SIZE - 1);
}

void ACRAM::DmaWrite(u32* iop_buf, u32 size_bytes, int bank) {
    u32& addr = banks[bank].write_addr;
    addr &= (ACRAM_MAX_SIZE - 1);
    ACRAM_LOG("DMAWrite addr:%8X size:%8X bank:%d", addr, size_bytes, bank);
    if (!s_ram) {
        // nowhere to put it outside an arcade session
    } else if (addr + size_bytes <= ACRAM_MAX_SIZE) {
        std::memcpy(&s_ram[addr], iop_buf, size_bytes);
    } else {
        u32 first = ACRAM_MAX_SIZE - addr;
        std::memcpy(&s_ram[addr], iop_buf, first);
        std::memcpy(&s_ram[0], (u8*)iop_buf + first, size_bytes - first);
    }
    addr = (addr + size_bytes) & (ACRAM_MAX_SIZE - 1);
}
