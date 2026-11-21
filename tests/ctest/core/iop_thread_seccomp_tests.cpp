// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP thread under a seccomp filter that traps membarrier, the way the app filter of Android 8
// and 9 answers every syscall it does not list: the kernel skips the call and raises SIGSYS
// (SECCOMP_RET_TRAP). With the signal handlers the emulator installs at startup
// (PageFaultHandler::Install), the thread has to start and fall back to the fence instead of the
// process dying. The filter is installed in a death-test child, so it never reaches the test binary.

#include <gtest/gtest.h>

#include "IopThread/IopThread.h"
#include "common/HostSys.h"

#if defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))

#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

namespace
{
#if defined(__aarch64__)
	constexpr u32 kAuditArch = AUDIT_ARCH_AARCH64;
#else
	constexpr u32 kAuditArch = AUDIT_ARCH_X86_64;
#endif

	// Makes syscall `nr` raise SIGSYS in this thread and every thread it starts afterwards. Everything else is
	// allowed.
	bool TrapSyscall(u32 nr)
	{
		sock_filter code[] = {
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		};
		sock_fprog prog = {static_cast<unsigned short>(std::size(code)), code};
		return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
	}

	// The child's exit code says which step failed.
	enum ChildExit : int
	{
		kPassed = 0,
		kNoHandlers = 2,
		kNoFilter = 3,
		kNotStarted = 4,
		kUsedMembarrier = 5,
	};

	int StartTheIopThreadWithMembarrierTrapped()
	{
		if (!PageFaultHandler::Install())
			return kNoHandlers;
		if (!TrapSyscall(__NR_membarrier))
			return kNoFilter;

		IopThread::Thread thread;
		IopThread::Params p;
		p.mode = IopThreadMode::Threaded;
		p.lag = 1 << 30;
		p.sleep_barrier = IopThread::SleepBarrier::Auto;
		p.run_slice = [](u64 from, u64 target) { return target > from ? target : from; };
		p.apply_item = [](const IopThread::E2IItem&) {};
		p.apply_kicks = [](u32) {};
		p.ee_cycle = [] { return u64{0}; };
		p.should_stop = [] { return false; };
		if (!thread.Start(p))
			return kNotStarted;
		const bool used_membarrier = thread.GetStats().sleep_barrier_membarrier != 0;
		thread.Stop();
		return used_membarrier ? kUsedMembarrier : kPassed;
	}
} // namespace

TEST(IopThreadSeccomp, AMembarrierTheFilterTrapsFallsBackToTheFence)
{
	// A fresh process: in a forked copy of this one, an earlier test may already have registered membarrier.
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(std::_Exit(StartTheIopThreadWithMembarrierTrapped()), ::testing::ExitedWithCode(kPassed), "");
}

#endif
