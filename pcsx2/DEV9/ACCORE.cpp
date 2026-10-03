// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#include "DEV9.h"
#include "IopDma.h"
#include "ACCORE.h"
#include "ACJV.h"
#include "ACMACROS.h"
#include "common/Console.h"
#include "R3000A.h"

#define ANS(addr, what) case addr: Console.Error("%-16s %08X: %04X", __FUNCTION__, addr, what); return what

enum ACCORE::DMA::TT ACCORE::DMA::PendTrasnfType;

u16 INTR_REG = 0;

u16 ACCORE::Read16(u32 mem) {
    switch (mem) {
	case 0x1241C000:
		// Console.Error("%-16s %08X: %04X", __FUNCTION__, mem, INTR_REG);
    	return INTR_REG; // ACRAM will wait 0xFFFFF times for this to not have 0x1000 bitmask set. also used inside intr_intr (interrup handler 13 declared on accore)
    
    break;
    default: Console.Error("%-16s %08X:  %04X", "ACUNK::Read16", mem, 0); return 0;
    }
    return 0;
}

void ACCORE::Write16(u32 mem, u16 value) {
		switch (mem) {
		case ACJV_CTR_START: Console.Warning("ACJV::START"); ACJV::enabled = true; ACJV::OnBoardStart(); break;
		case ACJV_CTR_STOP:  Console.Warning("ACJV::STOP");  ACJV::enabled = false;  break;
		case 0x1241510C: Console.Warning("ACCORE::INTR  DISABLE_ACATA_INTR"); break;
		case 0x1241511C: Console.Warning("ACCORE::INTR  DISABLE_ACUART_INTR"); break;
		case 0x1241511E: break;
		// ACFPGA UPLOAD MMIO ///TODOx6: move this handling to ACFPGA.cpp
		// unknown addresses set to 0 on ACCORE. most likely stopping other stuff. games do write to these
		case 0x1241600A:
		case 0x12416004:
		case 0x12416006:
		case 0x12416014:
		case 0x12416016:
		case 0x12416018:
		case 0x1241601A:
		case 0x1241601E:
		case 0x12416032:
		case 0x12416036:
		case 0x1241603A:
		case 0x12417000:
			break;
		case ACCORE_FPGA_BEGIN_PROGRAM:
			INTR_REG |= (0x1000|0x2000);
			break;
		case ACCPRE_FPGA_FINISH_PROGRAM:
			CLRB(INTR_REG, (0x1000|0x2000)); // we clear it to speed up boot times: ACRAM waits 0xFFFFF times for 0x1000 flags to be cleared
			break;

		default: Console.Error("%-16s %08X = %04X", "ACUNK::write16", mem, value); break;
		}
}

void ACCORE::Reset() {
	INTR_REG = 0;
	DMA::PendTrasnfType = DMA::NONE;
}

bool ACCORE::hasPendingInterrupt() {
	return INTR_REG != 0;
}

void ACCORE::intr(int INTRN) {
	switch (INTRN)
	{
	case INTRN_ATA:
		INTR_REG |= CAUS_ATA;
		break;

	case INTRN_UART: // V257 drive-board UART RX (Ridge Racer V status stream, see ACUART::StreamV257)
		INTR_REG |= CAUS_UART;
		break;

	default:

		return;
	}
	dev9Irq(1);
}

void ACCORE::Interrupt(u32 mem, u16 v) {
	switch (mem)
	{
	case ACCORE_INTR_ATA:
		CLRB(INTR_REG, CAUS_ATA);
		break;
	case ACCORE_INTR_UART:
		CLRB(INTR_REG, CAUS_UART); // acknowledge the UART RX interrupt (clear its cause bit)
		break;
	default:
		Console.Warning("ACCORE: unknown INTR write to: %08X", mem);
		break;
	}
}