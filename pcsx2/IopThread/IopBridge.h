// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The places where the EE's side of the emulator reaches into state the IOP owns.
//
// The EE and the IOP run on one thread today, so each of these is a plain call. When the IOP gets
// its own thread, every one of them needs something the plain call cannot give: the call has to wait
// for the IOP to be stopped, or become a message the IOP applies at the right cycle. Naming them
// here, one function per crossing, puts that change in one place per crossing instead of in the
// middle of Counters.cpp, Memory.cpp and the SIF code.
//
// Each function is inline and calls exactly the function its call site called before, with the same
// arguments in the same order. There is no mode test and no atomic in here. The generated code at
// every call site is the code it was before.
//
// The crossing numbers (C8, C14, ...) are the rows of section 7.1 of the IOP thread design.
// EeBridge.h has the other direction, IOP-side code reaching into the EE.

#pragma once

#include "Common.h"
#include "DEV9/DEV9.h"
#include "IopCounters.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "Patch.h"
#include "SIO/Sio.h"
#include "SPU2/spu2.h"
#include "Sif.h"

namespace IopBridge
{
	// ---- Blanking (C14) ----------------------------------------------------------------------
	// The EE's counters tell the IOP's counters that a blank started or ended. The calls are made
	// where the EE's own blank handling reaches that point, in the same order as before.

	__forceinline_odr void HBlankStart() { psxHBlankStart(); }
	__forceinline_odr void HBlankEnd() { psxHBlankEnd(); }
	__forceinline_odr void VBlankStart() { psxVBlankStart(); }
	__forceinline_odr void VBlankEnd() { psxVBlankEnd(); }

	// ---- Once-a-frame jobs the EE runs for IOP devices ---------------------------------------
	// Run from the EE's vsync. They touch the memory card state the IOP's SIO2 code reads.

	__forceinline_odr void AutoEjectTick() { AutoEject::CountDownTicks(); }
	__forceinline_odr void MemcardBusyTick() { MemcardBusy::Decrement(); }

	// The vsync patches that write IOP memory. Patch::ApplyVsyncPatches runs the EE ones itself.
	__forceinline_odr void ApplyVsyncPatches() { Patch::ApplyVsyncPatchesIOP(); }

	// ---- The EE raises an IOP interrupt (C8) -------------------------------------------------
	// A write to SBUS F240 with bit 18 set raises IOP interrupt line 1.

	__forceinline_odr void RaiseIopIrq(uint line) { iopIntcIrq(line); }

	// ---- SIF DMA, started from the EE (C10, C11, C12) ----------------------------------------
	// dmaSIF0/1/2 start a transfer and then run the pump, which moves data on both sides and
	// schedules both sides' completion interrupts. The pump is the call that touches the IOP.

	__forceinline_odr void Sif0Pump() { SIF0Dma(); }
	__forceinline_odr void Sif1Pump() { SIF1Dma(); }
	__forceinline_odr void Sif2Pump() { SIF2Dma(); }

	// ---- The EE reads and writes IOP address space (C16) -------------------------------------
	// The EE's memory map sends these ranges to IOP-owned state: IOP RAM (0x1c000000), DEV9
	// (0x14000000), the CDVD register page (0x1f400000) and SPU2 (0x1f900000). One function per
	// access size.

	__forceinline_odr u8 MemRead8(u32 mem) { return iopMemRead8(mem); }
	__forceinline_odr u16 MemRead16(u32 mem) { return iopMemRead16(mem); }
	__forceinline_odr u32 MemRead32(u32 mem) { return iopMemRead32(mem); }
	__forceinline_odr void MemWrite8(u32 mem, u8 value) { iopMemWrite8(mem, value); }
	__forceinline_odr void MemWrite16(u32 mem, u16 value) { iopMemWrite16(mem, value); }
	__forceinline_odr void MemWrite32(u32 mem, u32 value) { iopMemWrite32(mem, value); }

	__forceinline_odr u8 Hw4Read8(u32 mem) { return psxHw4Read8(mem); }
	__forceinline_odr void Hw4Write8(u32 mem, u8 value) { psxHw4Write8(mem, value); }

	__forceinline_odr u8 Dev9Read8(u32 addr) { return DEV9read8(addr); }
	__forceinline_odr u16 Dev9Read16(u32 addr) { return DEV9read16(addr); }
	__forceinline_odr u32 Dev9Read32(u32 addr) { return DEV9read32(addr); }
	__forceinline_odr void Dev9Write8(u32 addr, u8 value) { DEV9write8(addr, value); }
	__forceinline_odr void Dev9Write16(u32 addr, u16 value) { DEV9write16(addr, value); }
	__forceinline_odr void Dev9Write32(u32 addr, u32 value) { DEV9write32(addr, value); }

	__forceinline_odr u16 Spu2Read(u32 mem) { return SPU2read(mem); }
	__forceinline_odr void Spu2Write(u32 mem, u16 value) { SPU2write(mem, value); }

	// The IOP hardware registers the EE sees at 0x1f801000, 0x1f803000, 0x1f808000 and the rest of
	// 0x1f800000 are reached through handler functions registered in the EE's memory map, not
	// through a call. A handler is passed through here when it is registered, so a mode that needs
	// a different handler for these pages can return one without touching the registration sites.
	// The handler comes back unchanged.

	template <typename Handler>
	__forceinline_odr Handler HwHandler(Handler handler)
	{
		return handler;
	}
} // namespace IopBridge
