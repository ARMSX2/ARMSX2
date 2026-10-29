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
// What each crossing does in each mode:
//
//   Off        the call the call site always made. A function chosen by template (the blanking
//              calls, which run inside the event test and are templated on its mode) or one load
//              of the mode (the crossings that run at most a few thousand times a frame, none of them
//              inside the event test).
//   Lockstep   a queued item or a hold. An EE-to-IOP effect (blanking, the IRQ, the once-a-frame
//              jobs) becomes an item in the E2I queue, stamped with the EE's cycle, that the IOP thread
//              applies before the EE goes on. The EE's access to IOP state takes a hold, which in
//              Lockstep is a marker: the IOP thread is parked whenever the EE runs.
//   Threaded   the same items, but the EE posts them and goes on: the IOP applies each one when its own
//              clock reaches the stamp. The holds are real: the EE spins until the IOP has run up to the
//              EE's cycle and stopped.
//
// EeBridge.h has the other direction, IOP-side code reaching into the EE.

#pragma once

#include "Common.h"
#include "DEV9/DEV9.h"
#include "IopCounters.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "IopThread/IopThreadMode.h"
#include "IopThread/IopThreadVm.h"
#include "Patch.h"
#include "SIO/Sio.h"
#include "SPU2/spu2.h"
#include "Sif.h"

#include <optional>

namespace IopBridge
{
	/// The IOP held still for the length of a scope, so the EE thread may touch IOP-owned state inside
	/// it. Off has no IOP thread and nothing to hold: the cost is one load of the mode.
	/// In Lockstep the hold is a marker, because the IOP thread is parked whenever the EE runs; it is
	/// what IOP_SIDE_ASSERT looks for.
	class EmuHold
	{
	public:
		explicit EmuHold(IopThread::HoldKind kind = IopThread::HoldKind::Emulation)
		{
			if (IopThread::GetMode() != IopThreadMode::Off) [[unlikely]]
				m_hold.emplace(IopThread::g_thread, kind);
		}

	private:
		std::optional<IopThread::ScopedHold> m_hold;
	};

	// ---- Blanking ----------------------------------------------------------------------------
	// The EE's counters tell the IOP's counters that a blank started or ended. The calls are made
	// where the EE's own blank handling reaches that point.
	//
	// These run hundreds of times a frame, inside the event test, so the mode is a template parameter
	// (the mode of the event test instance that calls them) and Off is the direct call. Lockstep
	// queues the item and waits until the IOP thread has applied it. Threaded queues it and goes on.

	template <IopThreadMode M>
	__forceinline_odr void HBlankStart()
	{
		if constexpr (M == IopThreadMode::Off)
			psxHBlankStart();
		else
			IopThread::ApplyOnIop(IopThread::E2IKind::HBlankStart);
	}

	template <IopThreadMode M>
	__forceinline_odr void HBlankEnd()
	{
		if constexpr (M == IopThreadMode::Off)
			psxHBlankEnd();
		else
			IopThread::ApplyOnIop(IopThread::E2IKind::HBlankEnd);
	}

	template <IopThreadMode M>
	__forceinline_odr void VBlankStart()
	{
		if constexpr (M == IopThreadMode::Off)
			psxVBlankStart();
		else
			IopThread::ApplyOnIop(IopThread::E2IKind::VBlankStart);
	}

	template <IopThreadMode M>
	__forceinline_odr void VBlankEnd()
	{
		if constexpr (M == IopThreadMode::Off)
			psxVBlankEnd();
		else
			IopThread::ApplyOnIop(IopThread::E2IKind::VBlankEnd);
	}

	// ---- Once-a-frame jobs the EE runs for IOP devices ---------------------------------------
	// Run from the EE's vsync. They touch the memory card state the IOP's SIO2 code reads. Queued as
	// CALL items in Lockstep, like the blanking calls around them.

	template <IopThreadMode M>
	__forceinline_odr void AutoEjectTick()
	{
		if constexpr (M == IopThreadMode::Off)
			AutoEject::CountDownTicks();
		else
			IopThread::ApplyOnIop(IopThread::E2IKind::Call, IopThread::E2IJob::AutoEjectTick);
	}

	template <IopThreadMode M>
	__forceinline_odr void MemcardBusyTick()
	{
		if constexpr (M == IopThreadMode::Off)
			MemcardBusy::Decrement();
		else
			IopThread::ApplyOnIop(IopThread::E2IKind::Call, IopThread::E2IJob::MemcardBusyTick);
	}

	// The vsync patches that write IOP memory. Patch::ApplyVsyncPatches runs the EE ones itself, and is
	// called from VMManager (not from the event test), so the mode is read here: once a frame.
	__forceinline_odr void ApplyVsyncPatches()
	{
		if (IopThread::GetMode() == IopThreadMode::Off) [[likely]]
			Patch::ApplyVsyncPatchesIOP();
		else
			IopThread::ApplyOnIop(IopThread::E2IKind::Call, IopThread::E2IJob::VsyncPatchesIop);
	}

	// ---- The EE raises an IOP interrupt ------------------------------------------------------
	// A write to SBUS F240 with bit 18 set raises IOP interrupt line 1. Not hit during gameplay, so
	// the mode is read here.

	__forceinline_odr void RaiseIopIrq(uint line)
	{
		if (IopThread::GetMode() == IopThreadMode::Off) [[likely]]
			iopIntcIrq(line);
		else
			IopThread::ApplyOnIop(IopThread::E2IKind::IopIrq, line);
	}

	// ---- The PS1 mode switch -----------------------------------------------------------------
	// An EE write of F240 bit 19 resets the IOP into PS1 mode. PS1 mode forces Off, so the switch
	// holds the IOP, stops the IOP thread if there is one and asks the EE core to regenerate its
	// dispatcher for Off. The hold stays valid across the stop. Once per PS1 boot.

	class Ps1Switch
	{
	public:
		Ps1Switch()
			: m_hold(IopThread::g_thread, IopThread::HoldKind::Emulation)
		{
			IopThread::DemoteForPs1Switch();
		}

	private:
		IopThread::ScopedHold m_hold;
	};

	// ---- SIF DMA, started from the EE --------------------------------------------------------
	// dmaSIF0/1/2 start a transfer and then run the pump, which moves data on both sides and
	// schedules both sides' completion interrupts. The pump is the call that touches the IOP.

	// The pump runs on the EE thread and moves data on both sides, so the IOP is held while it does. In
	// Threaded this is a rendezvous: the EE spins until the IOP has run up to the EE's cycle and
	// stopped, then runs the combined pump. The pumps the EE starts are counted.

	__forceinline_odr void CountSifPump()
	{
		if (IopThread::GetMode() == IopThreadMode::Threaded) [[unlikely]]
			IopThread::g_thread.CountSifPump();
	}

	__forceinline_odr void Sif0Pump() { EmuHold hold; CountSifPump(); SIF0Dma(); }
	__forceinline_odr void Sif1Pump() { EmuHold hold; CountSifPump(); SIF1Dma(); }
	__forceinline_odr void Sif2Pump() { EmuHold hold; CountSifPump(); SIF2Dma(); }

	// ---- The EE reads and writes IOP address space -------------------------------------------
	// The EE's memory map sends these ranges to IOP-owned state: IOP RAM (0x1c000000), DEV9
	// (0x14000000), the CDVD register page (0x1f400000) and SPU2 (0x1f900000). One function per
	// access size. Each holds the IOP for the access (Lockstep: a marker; Off: one load of the mode).
	// Not hit after boot in the titles checked, so the mode is read at run time.

	__forceinline_odr u8 MemRead8(u32 mem) { EmuHold hold; return iopMemRead8(mem); }
	__forceinline_odr u16 MemRead16(u32 mem) { EmuHold hold; return iopMemRead16(mem); }
	__forceinline_odr u32 MemRead32(u32 mem) { EmuHold hold; return iopMemRead32(mem); }
	__forceinline_odr void MemWrite8(u32 mem, u8 value) { EmuHold hold; iopMemWrite8(mem, value); }
	__forceinline_odr void MemWrite16(u32 mem, u16 value) { EmuHold hold; iopMemWrite16(mem, value); }
	__forceinline_odr void MemWrite32(u32 mem, u32 value) { EmuHold hold; iopMemWrite32(mem, value); }

	__forceinline_odr u8 Hw4Read8(u32 mem) { EmuHold hold; return psxHw4Read8(mem); }
	__forceinline_odr void Hw4Write8(u32 mem, u8 value) { EmuHold hold; psxHw4Write8(mem, value); }

	__forceinline_odr u8 Dev9Read8(u32 addr) { EmuHold hold; return DEV9read8(addr); }
	__forceinline_odr u16 Dev9Read16(u32 addr) { EmuHold hold; return DEV9read16(addr); }
	__forceinline_odr u32 Dev9Read32(u32 addr) { EmuHold hold; return DEV9read32(addr); }
	__forceinline_odr void Dev9Write8(u32 addr, u8 value) { EmuHold hold; DEV9write8(addr, value); }
	__forceinline_odr void Dev9Write16(u32 addr, u16 value) { EmuHold hold; DEV9write16(addr, value); }
	__forceinline_odr void Dev9Write32(u32 addr, u32 value) { EmuHold hold; DEV9write32(addr, value); }

	__forceinline_odr u16 Spu2Read(u32 mem) { EmuHold hold; return SPU2read(mem); }
	__forceinline_odr void Spu2Write(u32 mem, u16 value) { EmuHold hold; SPU2write(mem, value); }

	// The IOP hardware registers the EE sees at 0x1f801000, 0x1f803000, 0x1f808000 and the rest of
	// 0x1f800000 are reached through handler functions registered in the EE's memory map, not
	// through a call. The handler is passed through here when it is registered; what comes back holds
	// the IOP around the call to it, like the functions above. The handler is a template argument so the
	// wrapper is a plain function the memory map can store.

	template <auto Handler, typename Signature = decltype(Handler)>
	struct HeldHandler;

	template <auto Handler, typename R, typename... Args>
	struct HeldHandler<Handler, R (*)(Args...)>
	{
		static R Call(Args... args)
		{
			EmuHold hold;
			return Handler(args...);
		}
	};

	template <auto Handler>
	constexpr auto HwHandler()
	{
		return &HeldHandler<Handler>::Call;
	}
} // namespace IopBridge
