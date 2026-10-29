// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Shared by the IOP thread tests that start a Lockstep thread. The recording actions stand in for
// the IOP's functions when the IOP thread applies an EE-to-IOP item.

#pragma once

#include "Common.h"
#include "Config.h"
#include "IopCounters.h"
#include "IopHw.h"
#include "IopMem.h"
#include "IopThread/IopThreadMode.h"
#include "IopThread/IopThreadVm.h"
#include "R3000A.h"
#include "R5900.h"
#include "StubHostTasks.h"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

extern bool hBlanking;
extern bool vBlanking;
extern s32 psxNextDeltaCounter;
extern u64 psxNextStartCounter;

namespace iopt
{
	// Puts the settings that choose the mode back when a test is done, and the mode with them.
	class SettingsGuard
	{
	public:
		SettingsGuard()
			: m_iop_thread(EmuConfig.Speedhacks.iopThread)
			, m_lockstep(EmuConfig.Speedhacks.iopThreadLockstep)
			, m_cpu(Cpu)
		{
		}
		~SettingsGuard()
		{
			IopThread::StopThread();
			EmuConfig.Speedhacks.iopThread = m_iop_thread;
			EmuConfig.Speedhacks.iopThreadLockstep = m_lockstep;
			Cpu = m_cpu;
			IopThread::ChooseMode();
		}

	private:
		bool m_iop_thread, m_lockstep;
		R5900cpu* m_cpu;
	};

	inline void StartLockstep()
	{
		EmuConfig.Speedhacks.iopThread = true;
		EmuConfig.Speedhacks.iopThreadLockstep = true;
		IopThread::ChooseMode();
		IopThread::SyncThread();
	}

	struct Applied
	{
		const char* what;
		u32 arg;
		std::thread::id thread;
	};
	inline std::mutex s_seen_mutex;
	inline std::vector<Applied>* s_seen = nullptr;

	inline void Record(const char* what, u32 arg = 0)
	{
		std::lock_guard lock(s_seen_mutex);
		if (s_seen)
			s_seen->push_back({what, arg, std::this_thread::get_id()});
	}

	inline const IopThread::E2IActions kRecordingActions = {
		[] { Record("hblank_start"); },
		[] { Record("hblank_end"); },
		[] { Record("vblank_start"); },
		[] { Record("vblank_end"); },
		[](u32 line) { Record("iop_irq", line); },
		[](u32 job) { Record("job", job); },
	};

	class Recorder
	{
	public:
		Recorder()
		{
			s_seen = &m_seen;
			m_previous = IopThread::SetE2IActionsForTests(&kRecordingActions);
		}
		~Recorder()
		{
			IopThread::SetE2IActionsForTests(m_previous);
			s_seen = nullptr;
		}
		const std::vector<Applied>& Items() const { return m_seen; }

	private:
		std::vector<Applied> m_seen;
		const IopThread::E2IActions* m_previous;
	};

	// The IOP state the safe entry points touch, saved and put back: ISTAT and ICTRL (iopIntcIrq sets
	// a bit and, with ICTRL zero, returns at once), the blanking flags and the IOP counters (the blank
	// calls update them and the next-counter bookkeeping).
	class IopStateGuard
	{
	public:
		IopStateGuard()
			: m_istat(psxHu32(HW_ISTAT))
			, m_ictrl(psxHu32(HW_ICTRL))
			, m_h(hBlanking)
			, m_v(vBlanking)
			, m_next_delta(psxNextDeltaCounter)
			, m_next_start(psxNextStartCounter)
			, m_iop_next_event(psxRegs.iopNextEventCycle)
		{
			std::memcpy(m_counters, psxCounters, sizeof(m_counters));
			psxHu32(HW_ISTAT) = 0;
			psxHu32(HW_ICTRL) = 0;
		}
		~IopStateGuard()
		{
			psxHu32(HW_ISTAT) = m_istat;
			psxHu32(HW_ICTRL) = m_ictrl;
			hBlanking = m_h;
			vBlanking = m_v;
			psxNextDeltaCounter = m_next_delta;
			psxNextStartCounter = m_next_start;
			psxRegs.iopNextEventCycle = m_iop_next_event;
			std::memcpy(psxCounters, m_counters, sizeof(m_counters));
		}

	private:
		u32 m_istat, m_ictrl;
		bool m_h, m_v;
		s32 m_next_delta;
		u64 m_next_start;
		u32 m_iop_next_event;
		psxCounter m_counters[NUM_COUNTERS];
	};
} // namespace iopt

#define REQUIRE_ARM64_HOST() \
	do \
	{ \
		if (!IopThread::CurrentHostInfo().is_arm64) \
			GTEST_SKIP() << "Lockstep resolves to Off on a host that is not ARM64"; \
	} while (0)

