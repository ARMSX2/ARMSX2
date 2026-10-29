// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP-thread ownership asserts: the predicates, the macros, and every entry point they are placed at.
//
// The real functions are called from the test thread (the EE thread) with the IOP thread running and no hold
// taken. The handler leaves each function at its first statement, so one that needs a machine never runs.

#include "CDVD/CDVD.h"
#include "DEV9/DEV9.h"
#include "Host.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "USB/USB.h"
#include "IopDma.h"
#include "IopThread/EeBridge.h"
#include "IopThread/IopBridge.h"
#include "IopThread/OwnerAsserts.h"
#include "IopThreadTestSupport.h"
#include "SIO/Sio.h"
#include "SPU2/spu2.h"
#include "Sif.h"

#include <csetjmp>
#include <string>

using namespace iopt;

namespace
{
	struct Violation
	{
		IopThread::Side side;
		std::string where;
	};
	std::mutex s_violation_mutex;
	std::vector<Violation>* s_violations = nullptr;

	// With a jump target set on the calling thread, a violation leaves the asserting function before its body
	// runs. This is a longjmp because the project builds without exceptions; there is nothing to unwind at the
	// first statement.
	thread_local std::jmp_buf* t_jump = nullptr;
	thread_local const char* t_last_where = nullptr;

	void RecordViolation(IopThread::Side side, const char* where)
	{
		{
			std::lock_guard lock(s_violation_mutex);
			if (s_violations)
				s_violations->push_back({side, where});
		}
		t_last_where = where;
		if (t_jump)
			std::longjmp(*t_jump, 1);
	}

	// Collects the violations. A test that wants the function stopped sets a jump target (CatchViolation).
	class ViolationRecorder
	{
	public:
		ViolationRecorder()
		{
			s_violations = &m_violations;
			m_previous = IopThread::SetViolationHandler(&RecordViolation);
		}
		~ViolationRecorder()
		{
			IopThread::SetViolationHandler(m_previous);
			s_violations = nullptr;
		}
		const std::vector<Violation>& Violations() const { return m_violations; }
		void Clear() { m_violations.clear(); }

	private:
		std::vector<Violation> m_violations;
		IopThread::ViolationHandler m_previous;
	};

	// Runs fn(); returns the name of the function whose assert fired and stopped it, or nullptr if none did.
	template <class F>
	const char* CatchViolation(F& fn)
	{
		std::jmp_buf buffer;
		t_last_where = nullptr;
		if (setjmp(buffer) == 0)
		{
			t_jump = &buffer;
			fn();
			t_jump = nullptr;
			return nullptr;
		}
		t_jump = nullptr;
		return t_last_where;
	}

	// Runs `fn` on the IOP thread (as a host command, which the Lockstep thread runs at the start of a
	// handoff) and waits for it. Returns the function whose assert fired there, if one did.
	template <class F>
	std::string OnIopThread(F&& fn)
	{
		std::string caught;
		const auto ticket = IopThread::g_thread.PostHostCommand([&caught, fn]() mutable {
			if (const char* where = CatchViolation(fn))
				caught = where;
		});
		IopThread::g_thread.LockstepApplyUpTo(0);
		EXPECT_TRUE(IopThread::g_thread.IsHostCommandDone(ticket));
		return caught;
	}
} // namespace

// The EE-started SIF pumps run IOP-side steps (they move data on both sides), so the assert on the IOP step is
// the observer: the bridge holds, a bare call does not.
TEST(IopThreadHolds, TheEeStartedSifPumpsHoldTheIop)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	ViolationRecorder violations;

	IopBridge::Sif0Pump();
	IopBridge::Sif1Pump();
	EXPECT_TRUE(violations.Violations().empty()) << "the bridge must hold the IOP for the pump";

	// Control: the same pump with no hold is what the assert exists to catch.
#if defined(PCSX2_DEBUG) || defined(PCSX2_DEVBUILD)
	SIF0Dma();
	SIF1Dma();
	EXPECT_GE(violations.Violations().size(), 2u) << "without the bridge the IOP step fires its assert";
#endif
}

TEST(IopThreadOwnerAsserts, OffAllowsEveryCaller)
{
	SettingsGuard guard;
	IopThread::ChooseMode();
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	EXPECT_TRUE(IopThread::IopSideAllowed());
	EXPECT_TRUE(IopThread::EeSideAllowed());
}

TEST(IopThreadOwnerAsserts, WithNoThreadRunningTheCallerIsAlone)
{
	SettingsGuard guard;
	// The mode says Lockstep, the thread has not been started (boot, between ChooseMode and SyncThread).
	EmuConfig.Speedhacks.iopThread = true;
	EmuConfig.Speedhacks.iopThreadLockstep = true;
	IopThread::ChooseMode();
	if (IopThread::GetMode() != IopThreadMode::Lockstep)
		GTEST_SKIP() << "Lockstep resolves to Off on this host";
	ASSERT_FALSE(IopThread::g_thread.IsRunning());
	EXPECT_TRUE(IopThread::IopSideAllowed());
}

TEST(IopThreadOwnerAsserts, TheEeThreadNeedsAHoldWhileTheIopThreadRuns)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	EXPECT_FALSE(IopThread::IopSideAllowed());
	{
		IopThread::VmHold hold;
		EXPECT_TRUE(IopThread::IopSideAllowed());
		{
			IopThread::VmHold nested;
			EXPECT_TRUE(IopThread::IopSideAllowed());
		}
		EXPECT_TRUE(IopThread::IopSideAllowed());
	}
	EXPECT_FALSE(IopThread::IopSideAllowed()) << "a released hold allows nothing";
}

TEST(IopThreadOwnerAsserts, TheIopThreadIsAlwaysAllowed)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	bool allowed = false;
	IopThread::g_thread.PostHostCommand([&allowed] { allowed = IopThread::IopSideAllowed(); });
	IopThread::g_thread.LockstepApplyUpTo(0);
	EXPECT_TRUE(allowed);
}

TEST(IopThreadOwnerAsserts, TheEeSideOnlyMattersInThreaded)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	// Lockstep: the IOP thread may call into the EE (the EE is blocked in the handoff).
	bool lockstep_from_iop = false;
	IopThread::g_thread.PostHostCommand([&] { lockstep_from_iop = IopThread::EeSideAllowed(); });
	IopThread::g_thread.LockstepApplyUpTo(0);
	EXPECT_TRUE(lockstep_from_iop);

	// Threaded: only the EE side may. The mode variable is set by hand because Threaded is not built; the
	// IOP thread reads it after the handoff.
	IopThread::s_mode = IopThreadMode::Threaded;
	bool threaded_from_iop = true;
	IopThread::g_thread.PostHostCommand([&] { threaded_from_iop = IopThread::EeSideAllowed(); });
	IopThread::g_thread.LockstepApplyUpTo(0);
	const bool threaded_from_ee = IopThread::EeSideAllowed();
	IopThread::s_mode = IopThreadMode::Lockstep;
	EXPECT_FALSE(threaded_from_iop);
	EXPECT_TRUE(threaded_from_ee);
}

#if defined(PCSX2_DEBUG) || defined(PCSX2_DEVBUILD)

namespace
{
	void CallsAnIopSideEntry()
	{
		IOP_SIDE_ASSERT();
	}
	void CallsAnEeSideEntry()
	{
		EE_SIDE_ASSERT();
	}
} // namespace

TEST(IopThreadOwnerAsserts, TheMacrosReportToTheHandlerWithTheFunctionName)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	ViolationRecorder violations;

	CallsAnIopSideEntry();
	ASSERT_EQ(violations.Violations().size(), 1u);
	EXPECT_EQ(violations.Violations()[0].side, IopThread::Side::Iop);
	EXPECT_EQ(violations.Violations()[0].where, "CallsAnIopSideEntry");
	EXPECT_EQ(IopThread::ViolationCount() > 0, true);

	{
		IopThread::VmHold hold;
		CallsAnIopSideEntry();
	}
	EXPECT_EQ(violations.Violations().size(), 1u) << "a hold makes the call legal";

	// The EE-side macro is silent unless the mode is Threaded.
	CallsAnEeSideEntry();
	EXPECT_EQ(violations.Violations().size(), 1u);
}

TEST(IopThreadOwnerAsserts, OffPaysNoViolationEvenWithNoHold)
{
	SettingsGuard guard;
	IopThread::ChooseMode();
	ASSERT_EQ(IopThread::GetMode(), IopThreadMode::Off);
	ViolationRecorder violations;
	CallsAnIopSideEntry();
	CallsAnEeSideEntry();
	EXPECT_TRUE(violations.Violations().empty());
}

TEST(IopThreadOwnerAsserts, TheLoggingHandlerCountsAndGoesOn)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	const IopThread::ViolationHandler previous = IopThread::SetViolationHandler(&IopThread::LogViolation);
	IopThread::ResetViolationCount();
	CallsAnIopSideEntry();
	CallsAnIopSideEntry();
	CallsAnIopSideEntry();
	IopThread::SetViolationHandler(previous);
	EXPECT_EQ(IopThread::ViolationCount(), 3u);
	IopThread::ResetViolationCount();
}

namespace
{
	// An IOP-side entry point and a way to call it bare. The handler throws as soon as the assert fires,
	// before the body, so the body never needs a machine.
	struct IopEntry
	{
		const char* name;
		void (*call)();
		bool safe; // the body can run on a bare machine with zeroed IOP state
	};

	const IopEntry kIopEntries[] = {
		{"PSX_INT", [] { PSX_INT(IopEvt_Cdvd, 100); }, false},
		{"iopIntcIrq", [] { iopIntcIrq(3); }, true},
		{"psxHBlankStart", [] { psxHBlankStart(); }, true},
		{"psxHBlankEnd", [] { psxHBlankEnd(); }, true},
		{"psxVBlankStart", [] { psxVBlankStart(); }, false},
		{"psxVBlankEnd", [] { psxVBlankEnd(); }, false},
		{"SPU2read", [] { (void)SPU2read(0x1f900000); }, false},
		{"SPU2write", [] { SPU2write(0x1f900000, 0); }, false},
		{"SPU2async", [] { SPU2async(); }, false},
		{"SPU2readDMA4Mem", [] { SPU2readDMA4Mem(nullptr, 0); }, false},
		{"SPU2writeDMA4Mem", [] { SPU2writeDMA4Mem(nullptr, 0); }, false},
		{"SPU2readDMA7Mem", [] { SPU2readDMA7Mem(nullptr, 0); }, false},
		{"SPU2writeDMA7Mem", [] { SPU2writeDMA7Mem(nullptr, 0); }, false},
		{"cdvdVsync", [] { cdvdVsync(); }, false},
		{"cdvdNewDiskCB", [] { cdvdNewDiskCB(); }, false},
		// Calls the CPU thread makes into devices the IOP thread owns; each is held by its caller.
		{"SetOutputPaused", [] { SPU2::SetOutputPaused(true); }, false},
		{"OnTargetSpeedChanged", [] { SPU2::OnTargetSpeedChanged(); }, false},
		{"CheckForConfigChanges", [] { SPU2::CheckForConfigChanges(EmuConfig); }, false},
		{"SetOutputVolume", [] { SPU2::SetOutputVolume(50); }, false},
		{"SetOutputMuted", [] { (void)SPU2::SetOutputMuted(true); }, false},
		{"DEV9CheckChanges", [] { DEV9CheckChanges(EmuConfig); }, false},
		{"CheckForConfigChanges", [] { USB::CheckForConfigChanges(EmuConfig); }, false},
		{"FileMcd_Reopen", [] { FileMcd_Reopen(std::string()); }, false},
		{"FileMcd_Swap", [] { FileMcd_Swap(); }, false},
		{"FileMcd_CancelEject", [] { FileMcd_CancelEject(); }, false},
		{"psxDma2", [] { psxDma2(0, 0, 0); }, false},
		{"psxDma4", [] { psxDma4(0, 0, 0); }, false},
		{"psxDma6", [] { psxDma6(0, 0, 0); }, false},
		{"psxDma7", [] { psxDma7(0, 0, 0); }, false},
		{"psxDma8", [] { psxDma8(0, 0, 0); }, false},
		{"psxDma9", [] { psxDma9(0, 0, 0); }, false},
		{"psxDma10", [] { psxDma10(0, 0, 0); }, false},
		{"psxDma11", [] { psxDma11(0, 0, 0); }, false},
		{"psxDma12", [] { psxDma12(0, 0, 0); }, false},
		{"iopRunSliceOnIopThread", [] { iopRunSliceOnIopThread(); }, false},
		{"Write8", [] { IOPMemoryInterface().Write8(0, 0); }, false},
		{"Write16", [] { IOPMemoryInterface().Write16(0, 0); }, false},
		{"Write32", [] { IOPMemoryInterface().Write32(0, 0); }, false},
		{"WriteBytes", [] { IOPMemoryInterface().WriteBytes(0, nullptr, 0); }, false},
	};
} // namespace

TEST(IopThreadOwnerAsserts, EveryIopSideEntryRefusesTheEeThreadWithNoHold)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	for (const IopEntry& entry : kIopEntries)
	{
		ViolationRecorder violations;
		auto call = [&entry] { entry.call(); };
		const char* caught = CatchViolation(call);
		ASSERT_EQ(violations.Violations().size(), 1u) << entry.name << " did not assert";
		EXPECT_EQ(violations.Violations()[0].side, IopThread::Side::Iop) << entry.name;
		ASSERT_NE(caught, nullptr) << entry.name;
		EXPECT_STREQ(caught, entry.name);
	}
}

TEST(IopThreadOwnerAsserts, EveryIopSideEntryAcceptsTheEeThreadInsideAHold)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	ViolationRecorder violations;
	IopStateGuard iop_state;

	IopThread::VmHold hold;
	// The entries whose bodies are harmless on a bare machine run to completion. The rest are covered by
	// the predicate: the assert is the same check for all of them (EveryIopSideEntryRefuses... shows each
	// one calls it).
	size_t ran = 0;
	for (const IopEntry& entry : kIopEntries)
	{
		if (!entry.safe)
			continue;
		entry.call();
		ran++;
	}
	EXPECT_GE(ran, 3u);
	EXPECT_TRUE(violations.Violations().empty());
}

TEST(IopThreadOwnerAsserts, EveryIopSideEntryAcceptsTheIopThread)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	ViolationRecorder violations;
	IopStateGuard iop_state;

	size_t ran = 0;
	for (const IopEntry& entry : kIopEntries)
	{
		if (!entry.safe)
			continue;
		const std::string caught = OnIopThread([&entry] { entry.call(); });
		EXPECT_EQ(caught, "") << entry.name;
		ran++;
	}
	EXPECT_GE(ran, 3u);
	EXPECT_TRUE(violations.Violations().empty());
}

namespace
{
	struct EeEntry
	{
		const char* name;
		void (*call)();
	};

	const EeEntry kEeEntries[] = {
		{"ScheduleEeEvent", [] { EeBridge::ScheduleEeEvent(10); }},
		{"IopInterruptPending", [] { EeBridge::IopInterruptPending(); }},
		{"RaiseSbusIrq", [] { EeBridge::RaiseSbusIrq(); }},
		{"Sif0Pump", [] { EeBridge::Sif0Pump(); }},
		{"Sif1Pump", [] { EeBridge::Sif1Pump(); }},
	};
} // namespace

TEST(IopThreadOwnerAsserts, TheEeSideEntriesAreTheIopsToCallInLockstep)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	// In Lockstep the IOP thread calls EE entries freely: the EE is blocked in the handoff.
	for (const EeEntry& entry : kEeEntries)
	{
		ViolationRecorder none;
		const std::string caught = OnIopThread([&] { IopThread::CheckEeSide(entry.name); });
		EXPECT_EQ(caught, "") << entry.name;
		EXPECT_TRUE(none.Violations().empty()) << entry.name;
	}
}

// In Threaded the EE-side entries are the EE's alone: the check refuses the IOP thread. The bridges the IOP code
// calls (kEeEntries) are suppressed, queued for the EE, or run by the EE (iop_thread_threaded_tests.cpp), so none
// of them fires the assert in Threaded.
TEST(IopThreadOwnerAsserts, TheEeSideCheckRefusesTheIopThreadInThreaded)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());

	ViolationRecorder violations;
	IopThread::s_mode = IopThreadMode::Threaded;
	const std::string caught = OnIopThread([&] { IopThread::CheckEeSide("AnEeEntry"); });
	IopThread::s_mode = IopThreadMode::Lockstep;
	ASSERT_EQ(violations.Violations().size(), 1u);
	EXPECT_EQ(violations.Violations()[0].side, IopThread::Side::Ee);
	EXPECT_EQ(caught, "AnEeEntry");
}

namespace
{
	// What each frontend's Host::RunOnCPUThread starts with.
	void FakeRunOnCpuThread(bool block)
	{
		BLOCKING_HOST_CALL_ASSERT(block);
	}
} // namespace

TEST(IopThreadOwnerAsserts, TheIopThreadMayNotMakeABlockingCallToTheCpuThread)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	ViolationRecorder violations;

	// The EE is blocked in the handoff (Lockstep) or may be spinning on the IOP (Threaded): either way it is not
	// running the queue a blocking call waits on.
	const std::string caught = OnIopThread([] { FakeRunOnCpuThread(true); });
	ASSERT_EQ(violations.Violations().size(), 1u);
	EXPECT_EQ(violations.Violations()[0].side, IopThread::Side::BlockingHostCall);
	EXPECT_EQ(caught, "FakeRunOnCpuThread");
}

TEST(IopThreadOwnerAsserts, TheIopThreadMayPostWithoutWaiting)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	ViolationRecorder violations;
	EXPECT_EQ(OnIopThread([] { FakeRunOnCpuThread(false); }), "");
	EXPECT_TRUE(violations.Violations().empty());
}

TEST(IopThreadOwnerAsserts, TheCpuThreadMayBlockWhateverTheModeIs)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	ViolationRecorder violations;

	IopThread::ChooseMode();
	FakeRunOnCpuThread(true);
	EXPECT_TRUE(IopThread::BlockingHostCallAllowed());

	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	FakeRunOnCpuThread(true);
	EXPECT_TRUE(IopThread::BlockingHostCallAllowed());
	EXPECT_TRUE(violations.Violations().empty());
}

TEST(IopThreadOwnerAsserts, TheStubHostsRunOnCpuThreadHasTheAssert)
{
	REQUIRE_ARM64_HOST();
	SettingsGuard guard;
	StartLockstep();
	ASSERT_TRUE(IopThread::LockstepActive());
	ViolationRecorder violations;
	StubHost::TakeCpuThreadTasks();

	const std::string caught = OnIopThread([] { Host::RunOnCPUThread([] {}, true); });
	EXPECT_EQ(caught, "RunOnCPUThread");
	ASSERT_EQ(violations.Violations().size(), 1u);
	EXPECT_EQ(violations.Violations()[0].side, IopThread::Side::BlockingHostCall);
	StubHost::TakeCpuThreadTasks();
}

#endif // PCSX2_DEBUG || PCSX2_DEVBUILD
