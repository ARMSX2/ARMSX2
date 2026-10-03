// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#pragma once
#include "MemoryTypes.h"

/**
 * @brief source related to ACCORE.IRX MMIO and stuff
 * 
 */


namespace ACCORE {
    u16 Read16(u32 addr);
    void Write16(u32 addr, u16 val);
	void intr(int INTRN);
	void Interrupt(u32 mem, u16 val);
	bool hasPendingInterrupt();
	void Reset(); // ARMSX2: no interrupt or DMA pending, as a freshly started emulator has it
	namespace DMA {
        enum TT {
            NONE = 0,
            ATA,
            ATAPI,
            ATA_WRITE,
        };
		extern enum TT PendTrasnfType;
	}
	enum {
		INTRN_ATA = 0x0,
		INTRN_JV = 0x1,
		INTRN_UART = 0x2,
		INTRN_LAST = 0x2,
	};
	enum INTC_CAUS {
		CAUS_ATA = 0x8000,
		CAUS_UART = 0x4000 // UART receive interrupt (drive-board serial link)
	};
}

#define ACCORE_INTR_ATA  			0x13000000
#define ACCORE_INTR_UART 			0x13100000
#define ACCORE_FPGA_BEGIN_PROGRAM 	0x12416008
#define ACCPRE_FPGA_FINISH_PROGRAM 	0x12416012
