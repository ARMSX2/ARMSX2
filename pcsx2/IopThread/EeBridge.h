// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The places where IOP-side code, or EE-side code that the IOP's side calls, reaches into the EE's
// scheduling or state.
//
// The EE and the IOP run on one thread today, so each of these is a plain call or a plain write.
// When the IOP gets its own thread, the IOP must not write EE state: a crossing here becomes a message
// the EE picks up at its next event test, or nothing at all where the IOP schedules for itself.
// Naming them here, one function per crossing, puts that change in one place per crossing.
//
// Each function is inline and does exactly what the code at its call site did before, in the same
// order. There is no mode test and no atomic in here. The generated code at every call site is the
// code it was before.
//
// The crossing numbers (C3, C4, ...) are the rows of section 7.1 of the IOP thread design.
// IopBridge.h has the other direction, the EE reaching into IOP state.

#pragma once

#include "Common.h"
#include "IopHw.h"
#include "IopThread/IopThreadMode.h"
#include "IopThread/OwnerAsserts.h"
#include "R3000A.h"
#include "Sif.h"
#include "VMManager.h"

namespace EeBridge
{
	// ---- The IOP asks the EE to look sooner (C3, C4) -----------------------------------------

	// PSX_INT scheduled an IOP event that falls due before the EE's next event test would look at it.
	// The EE runs its event test `delta` EE cycles from now instead.
	__forceinline_odr void ScheduleEeEvent(s32 delta)
	{
		EE_SIDE_ASSERT();
		cpuSetNextEventDelta(delta);
	}

	// iopTestIntc found an unmasked IOP interrupt pending. The IOP has to get to run its exception
	// code, which happens in its event test. If the EE is running code, ask it to branch soon so the
	// IOP can handle it promptly, and mark the IOP as having work. If the EE is already inside its
	// event test, the IOP's own next branch will do.
	__forceinline_odr void IopInterruptPending()
	{
		EE_SIDE_ASSERT();
		if (!eeEventTestIsActive)
		{
			// An iop exception has occurred while the EE is running code.
			// Inform the EE to branch so the IOP can handle it promptly:

			cpuSetNextEventDelta(16);
			iopEventAction = true;
			//Console.Error( "** IOP Needs an EE EventText, kthx **  %d", iopCycleEE );

			// Note: No need to set the iop's branch delta here, since the EE
			// will run an IOP branch test regardless.
		}
		else if (!iopEventTestIsActive)
			psxSetNextBranchDelta(2);
	}

	// ---- An EE event is scheduled while an IOP slice is running (C5) -------------------------
	// Called from CPU_INT, cpuTestINTCInts and cpuTestDMACInts after they have found that an EE
	// event is due soon and that IOP cycles are left in the slice. The IOP stops running now and
	// the EE gets back to its event test; the cycles the IOP did not run are recorded.

	__forceinline_odr void BreakIopSlice()
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}

	// ---- The IOP interrupts the EE (C13) -----------------------------------------------------
	// A write to the IOP's SBUS register 0x1f801450 with bit 1 set raises the SBUS interrupt on the EE.

	__forceinline_odr void RaiseSbusIrq()
	{
		EE_SIDE_ASSERT();
		hwIntcIrq(INTC_SBUS);
	}

	// ---- SIF DMA, started from the IOP (C10, C11) --------------------------------------------
	// psxDma9/psxDma10 start a transfer and then run the pump, which moves data on both sides and
	// schedules both sides' completion interrupts. The pump is the call that touches the EE.

	__forceinline_odr void Sif0Pump() { EE_SIDE_ASSERT(); SIF0Dma(); }
	__forceinline_odr void Sif1Pump() { EE_SIDE_ASSERT(); SIF1Dma(); }

	// ---- The IOP stops the VM ----------------------------------------------------------------
	// The IOP JIT's error path and the IOP interpreter's breakpoint pause the VM and leave the
	// EE's execution loop. Both are calls the CPU thread owns: when the IOP is on its own thread
	// (Lockstep) the request is kept and the EE makes it when its handoff returns. Error paths only,
	// so the call is not inline.

	__forceinline_odr void RequestPause()
	{
		IopThread::RequestPause();
	}
} // namespace EeBridge
