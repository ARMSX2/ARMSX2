// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#include "ACUART_GLUE.h"
#include "ACUART.h"
#include "ACCORE.h"
#include "Config.h"
#include "common/Console.h"
#include <deque>

#define ACUART_LOG(fmt, ...) if (EmuConfig.Arcade.UARTVerbose) Console.WriteLn(Color_Gray, "ACUART:" fmt __VA_OPT__(,) __VA_ARGS__)
#define ACUART_WARN(fmt, ...) if (EmuConfig.Arcade.UARTVerbose) Console.Warning("ACUART:" fmt __VA_OPT__(,) __VA_ARGS__)

class Bg3HandleDevice : public ACUARTDevice
{
private:
    std::deque<u8> fifo;

    int txCnt = 0;
    u8 prevTx = 0;
    int handleCycles = 0;
    bool handleDone = false;

public:
// clear BG3 acuart HANDLE-handshake state on game boot (fixes HANDLE-ERROR-on-reset)
    void Reset() override
    {
        txCnt = 0;
        prevTx = 0;
        handleCycles = 0;
        handleDone = false;
        fifo.clear();
    }
    
// Battle Gear 3 / Tuned: answer the steering board's boot handshake (BGRLOAD FUN_00133e60) so the game
// doesn't stall on "HANDLE ERROR". With no emulated FFB board we reply ready and keep the drive-board flag
// (EE 0x2694b0) CLEAR, so steering stays on the JVS analog wheel. FFB GROUNDWORK: set s_bg3FfbEnabled=true
// when a real drive board is emulated to run the full calibration handshake.
// (Thanks to Hydreigon223 for the dump.)
    void TxByte(u8 value) override {
	    txCnt++;
	    if ((txCnt & 1) != 0) { prevTx = value; return; } // a {reg,val} command is 2 bytes; reply on the 2nd

	    if (prevTx == 0x20 && value == 0x00) // {0x20,0} starts an init -> re-arm so a re-init replies fresh
	    {
	    	handleDone = false;
	    	handleCycles = 0;
	    }
	    u8 hi = 0x00;
	    if (!handleDone)
	    {
	    	static constexpr bool s_bg3FfbEnabled = false;    // true once a real FFB drive board is emulated
	    	if (prevTx == 0x20 || prevTx == 0x1f)
	    		hi = s_bg3FfbEnabled ? 0x80 : 0x01;           // 0x01 = ready (skip calibrate while FFB off)
	    	else if (prevTx == 0x14 && value == 0x1a)
	    		hi = (++handleCycles > 4) ? 0x00 : 0x80; // calibrate busy-loop (FFB on only)
	    	if (prevTx == 0x11 && value == 0x03)
	    		handleDone = true;                       // last init command
	    }
	    fifo.push_back(hi); // byte0 = busy/ready status
	    fifo.push_back(0);  // byte1
	    ACUART_LOG("INTR");
	    ACCORE::intr(ACCORE::INTRN_UART);
	    prevTx = value;
    }

    bool RxByte(u8& value) {
		if (!fifo.empty())
		{
			value = fifo.front();
			fifo.pop_front();
            return true;
		}
        return false;
    }

    bool HasData() const override
    {
        return !fifo.empty();
    }
};


class RRVHandleDevice : public ACUARTDevice
{
private:
    std::deque<u8> fifo;

    int txCnt = 0;
    u8 prevTx = 0;
    int handleCycles = 0;
    bool handleDone = false;
    u32 s_v257Accum = 0;
    static constexpr u8 V257_STATUS[3] = {'C', '0', '1'}; // drive-board OK status; accepted by every RRV build

public:
    void Reset() override
    {
        txCnt = 0;
        prevTx = 0;
        handleCycles = 0;
        handleDone = false;
        fifo.clear();
    }
    //void TxByte(u8 value) override {}

    bool RxByte(u8& value) {
		if (!fifo.empty()) // RRV reading the serial port: hand it the next status byte we queued ("E00")
		{
			value = fifo.front();
			fifo.pop_front();
            return true;
		}
        return false;
    }

    bool HasData() const override
    {
        return !fifo.empty();
    }

// Ridge Racer V drive-board status streamer (called each DEV9 tick): refill the receive buffer with the
// board's OK status and raise the RX interrupt. Only RRV needs this.
    void Tick(u32 cycles) {
	    if (!(ACUART::IER & 0x01)) // host hasn't enabled the RX-data interrupt yet
	    	return;
	    s_v257Accum += cycles;
	    if (s_v257Accum < 240) // throttle (DEV9async ticks ~tens of kHz with cycles=1) -> a few hundred Hz
	    	return;
	    s_v257Accum = 0;
	    // Keep raising the RX IRQ every tick; reload the status only once the ISR has drained the previous copy.
	    if (fifo.empty())
	    	fifo.assign(V257_STATUS, V257_STATUS + 3);
	    ACUART_LOG("INTR");
	    ACCORE::intr(ACCORE::INTRN_UART); // raise the UART RX interrupt
    }
};



bool ACUART::SetupGameHandler(const std::string& S) {
    if (S == "NM00001")
        s_device = std::make_unique<RRVHandleDevice>();
    else if (S == "NM00010" || S == "NM00015")
        s_device = std::make_unique<Bg3HandleDevice>();
    else 
        return false;
    s_device->Reset();
    return true;
}
