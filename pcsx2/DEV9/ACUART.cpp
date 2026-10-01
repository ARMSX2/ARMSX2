// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#include "Config.h"
#include "ACUART.h"
#include "ACJV.h"
#include "ACCORE.h"
#include "common/Console.h"
#include <deque>
#include <string>

#define ACUART_LOG(fmt, ...) if (EmuConfig.Arcade.UARTVerbose) Console.WriteLn(Color_Gray, "ACUART:" fmt __VA_OPT__(,) __VA_ARGS__)
#define ACUART_WARN(fmt, ...) if (EmuConfig.Arcade.UARTVerbose) Console.Warning("ACUART:" fmt __VA_OPT__(,) __VA_ARGS__)

// 16550 UART register emulation for Namco System 246 arcade I/O
// Ref: ps2sdk iop/arcade/acuart/src/uart.c
// Register map (16-bit access, 2-byte stride from base 0x12418000):
//   +0x00  THR/RBR/DLL  (TX/RX data or divisor latch low when DLAB=1)
//   +0x02  IER/DLH      (interrupt enable or divisor latch high when DLAB=1)
//   +0x04  IIR/FCR      (interrupt ID read / FIFO control write)
//   +0x06  LCR          (line control, bit 7 = DLAB)
//   +0x08  MCR          (modem control)
//   +0x0A  LSR          (line status)
//   +0x0C  MSR          (modem status)
//   +0x0E  SCR          (scratch)

u16 ACUART::IER = 0;
u16 ACUART::LCR = 0;
u16 ACUART::MCR = 0;
u16 ACUART::SCR = 0;
u16 ACUART::DLL = 0;
u16 ACUART::DLH = 0;
u16 ACUART::FCR_SHADOW = 0;
std::unique_ptr<ACUARTDevice> ACUART::s_device;

u16 ACUART::Read16(u32 addr) {
	u16 r = 0;
	const u32 reg = addr & 0xFFF;
	ACUART_LOG("Read16  %03X", reg);
	switch (reg) {
	case 0x000: // RBR or DLL
		if (ACUART::LCR & 0x80) {
			r = ACUART::DLL;
			break;
		}
    	u8 b;
		r = 0;
    	if (s_device && s_device->RxByte(b)) {
    	    r = b;
		}
		ACUART_LOG("RX:%02X", r);
		break;
	case 0x002: // IER or DLH
		if (ACUART::LCR & 0x80)
			r = ACUART::DLH;
		else
			r = ACUART::IER;
		break;
	case 0x004: // IIR (read-only)
		r = 0x01; // no interrupt pending, 16550 mode
		break;
	case 0x006: // LCR
		r = ACUART::LCR;
		break;
	case 0x008: // MCR
		r = ACUART::MCR;
		break;
	case 0x00A: // LSR
		// bit 5 = THRE (TX holding register empty)
		// bit 6 = TEMT (TX shift register empty)
		// both set = transmitter idle, ready to accept data
		// bit 0 = DR (RX data ready) — set while the V257 status FIFO has bytes (RRV)
		r = 0x60 | ((s_device && s_device->HasData()) ? 0x01 : 0x00);
		//r = 0x60 | ((s_v257RxFifo.empty()) ? 0x00 : 0x01);
		break;
	case 0x00C: // MSR
		r = 0;
		break;
	case 0x00E: // SCR
		r = ACUART::SCR;
		break;
	default:
		ACUART_WARN("Unhandled read: %03X", reg);
		r = 0;
	}
	return r;
}

void ACUART::Write16(u32 addr, u16 val) {
	const u32 reg = addr & 0xFFF;
	
	switch (reg) {
	case 0x000: // THR or DLL
		if (ACUART::LCR & 0x80)
			ACUART::DLL = val;
		else {
			ACUART_LOG("TX:%02X", val);
			if (s_device)
				s_device->TxByte((u8)val);
		}
		break;
	case 0x002: // IER or DLH — set to 0 on module stop, bits toggled during xmit (acUartModuleStop, uart_xmit)
		if (ACUART::LCR & 0x80)
			ACUART::DLH = val;
		else
			ACUART::IER = val;
		break;
	case 0x004: // FCR (write-only) — val=7 on module stop: enable FIFO + reset RX/TX (acUartModuleStop)
		ACUART::FCR_SHADOW = val & 0xC9; // preserve trigger level + enable bits
		break;
	case 0x006: // LCR
		ACUART::LCR = val;
		break;
	case 0x008: // MCR
		ACUART::MCR = val;
		break;
	case 0x00E: // SCR
		ACUART::SCR = val;
		break;
	default:
		ACUART_WARN("Unhandled write:%03X:%04X", reg, val);
		break;
	}
}

