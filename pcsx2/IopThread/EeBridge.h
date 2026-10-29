// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The places where IOP-side code, or EE-side code that the IOP calls, reaches into the EE's
// scheduling or state. IopBridge.h has the other direction, the EE reaching into IOP state.
//
// In Off and Lockstep each function does what its call site always did, in the same order. In
// Threaded the IOP runs concurrently with the EE and must not write EE state, so a crossing that
// wrote it is suppressed or becomes an item the EE applies at its next event test. One function per
// crossing keeps that change in one place.
//
// Each bridge reads the mode once, on a path that runs at most a few hundred times a frame. The read
// is not in the event test, in any IOP memory access, or at the entry of CPU_INT: the slice break is
// reached from CPU_INT only behind CPU_INT's two existing conditions.

#pragma once

#include "Common.h"
#include "IopHw.h"
#include "Input/InputManager.h"
#include "IopThread/IopThreadMode.h"
#include "IopThread/IopThreadVm.h"
#include "IopThread/OwnerAsserts.h"
#include "R3000A.h"
#include "Sif.h"
#include "VMManager.h"

namespace EeBridge
{
	// PSX_INT scheduled an IOP event that falls due before the EE's next event test would look at it.
	// The EE runs its event test `delta` EE cycles from now instead.
	//
	// Threaded: suppressed. The EE does not look at the IOP's events: the IOP runs whenever the
	// watermark gives it budget, and schedules its own events.
	__forceinline_odr void ScheduleEeEvent(s32 delta)
	{
		if (IopThread::GetMode() == IopThreadMode::Threaded) [[unlikely]]
			return;
		EE_SIDE_ASSERT();
		cpuSetNextEventDelta(delta);
	}

	// iopTestIntc found an unmasked IOP interrupt pending. The IOP has to get to run its exception
	// code, which happens in its event test. If the EE is running code, ask it to branch soon so the
	// IOP can handle it promptly, and mark the IOP as having work. If the EE is already inside its
	// event test, the IOP's own next branch will do.
	//
	// Threaded: the IOP has work, and brings its own next branch forward. It does not look at the
	// EE's flag, which belongs to the other thread.
	__forceinline_odr void IopInterruptPending()
	{
		if (IopThread::GetMode() == IopThreadMode::Threaded) [[unlikely]]
		{
			iopEventAction = true;
			if (!iopEventTestIsActive)
				psxSetNextBranchDelta(2);
			return;
		}
		EE_SIDE_ASSERT();
		if (!eeEventTestIsActive)
		{
			cpuSetNextEventDelta(16);
			iopEventAction = true;

			// Note: No need to set the iop's branch delta here, since the EE
			// will run an IOP branch test regardless.
		}
		else if (!iopEventTestIsActive)
			psxSetNextBranchDelta(2);
	}

	// Called from CPU_INT, cpuTestINTCInts and cpuTestDMACInts after they have found that an EE
	// event is due soon and that IOP cycles are left in the slice. The IOP stops running now and
	// the EE gets back to its event test; the cycles the IOP did not run are recorded.
	//
	// Threaded: skipped. No EE event happens inside an IOP slice: the IOP runs beside the EE, and
	// the registers this would write belong to the IOP thread, which is running. (The condition the
	// caller tested before getting here read iopCycleEE from the EE thread; whatever it saw is
	// discarded.)
	__forceinline_odr void BreakIopSlice()
	{
		if (IopThread::GetMode() == IopThreadMode::Threaded) [[unlikely]]
			return;
		psxRegs.iopBreak += psxRegs.iopCycleEE;
		psxRegs.iopCycleEE = 0;
	}

	// A write to the IOP's SBUS register 0x1f801450 with bit 1 set raises the SBUS interrupt on the EE.
	//
	// Threaded: INTC_STAT is written on the EE thread only, because the EE JIT loads it inline when the
	// IntcStat speedhack is off. The IOP queues the interrupt, in order with whatever else it queued, and the EE
	// raises it at its next event test.
	__forceinline_odr void RaiseSbusIrq()
	{
		if (IopThread::GetMode() == IopThreadMode::Threaded) [[unlikely]]
		{
			IopThread::g_thread.PostI2E(IopThread::I2EKind::SbusIrq);
			return;
		}
		EE_SIDE_ASSERT();
		hwIntcIrq(INTC_SBUS);
	}

	// psxDma9/psxDma10 start a transfer and then run the pump, which moves data on both sides and
	// schedules both sides' completion interrupts. The pump is the call that touches the EE.
	//
	// Threaded: the pump runs on the EE thread with the IOP stopped. The IOP blocks in CallOnEE until
	// the EE has run it, which it does at its next event test or sooner inside any EE wait.
	__forceinline_odr void Sif0Pump()
	{
		if (IopThread::GetMode() == IopThreadMode::Threaded) [[unlikely]]
		{
			IopThread::RunSifPumpOnEe(&SIF0Dma);
			return;
		}
		EE_SIDE_ASSERT();
		SIF0Dma();
	}

	__forceinline_odr void Sif1Pump()
	{
		if (IopThread::GetMode() == IopThreadMode::Threaded) [[unlikely]]
		{
			IopThread::RunSifPumpOnEe(&SIF1Dma);
			return;
		}
		EE_SIDE_ASSERT();
		SIF1Dma();
	}

	// The IOP JIT's error path and the IOP interpreter's breakpoint pause the VM and leave the
	// EE's execution loop. Both are calls the CPU thread owns: when the IOP is on its own thread
	// (Lockstep) the request is kept and the EE makes it when its handoff returns, in Threaded it is an
	// item the EE takes at its next event test. Error paths only, so the call is not inline.

	__forceinline_odr void RequestPause()
	{
		IopThread::RequestPause();
	}

	// The pad commands (DualShock 2, Negcon, Jogcon) and the USB trance vibrator set a motor's intensity from
	// the IOP's code. The input manager's bindings, and the sources behind them, are the CPU thread's, so in
	// Threaded mode the call is posted to the CPU thread and does not wait. Once per pad poll, at most a few
	// times a frame.

	__forceinline_odr void SetPadVibration(u32 pad_index, float large_or_single_motor_intensity, float small_motor_intensity)
	{
		IopThread::CallOnCpuThread([=] { InputManager::SetPadVibrationIntensity(pad_index, large_or_single_motor_intensity, small_motor_intensity); });
	}

	__forceinline_odr void SetUsbVibration(u32 port, float large_or_single_motor_intensity, float small_motor_intensity)
	{
		IopThread::CallOnCpuThread([=] { InputManager::SetUSBVibrationIntensity(port, large_or_single_motor_intensity, small_motor_intensity); });
	}
} // namespace EeBridge
