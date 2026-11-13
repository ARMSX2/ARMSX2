// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// A second VM boot in the same process, with MTVU on.
//
// mVUreset finishes a running VU1 program before it regenerates the
// dispatchers, when MTVU is on and VPU_STAT bit 0x100 says VU1 is busy. That
// flush is for MTVU being switched on while a game runs. VPU_STAT lives in
// vuState, which outlives the VM: the next boot's vuMemReset (SysMemory::Reset)
// zeroes it, but VM init runs ClearCPUExecutionCaches before SysMemory::Reset.
// So a VM stopped while VU1 was busy leaves the bit set for the next boot.
//
// On Android every game runs in its own CPU thread session: CPUThreadShutdown
// shuts the recompilers down and releases the code memory, and the next boot
// maps it again, zero-filled, at the same address. VU0's reset then sees the
// stale bit and calls microVU1's dispatcher, which VU1's own reset (after
// VU0's) has not regenerated yet. The zero bytes decode as UDF: SIGILL at the
// start of the microVU1 code region (field reports, 2026-11).
//
// The first two tests run that sequence in a forked child: session one leaves
// VU1 busy, the test environment is shut down the way CPUThreadShutdown does it
// (recompilers, then SysMemory::Release), and session two is brought up the way
// CPUThreadInitialize and VMManager::Initialize do it, calling the production
// VMManager::Internal::ClearCPUExecutionCaches before SysMemory::Reset. A
// signal guard turns a fault into a test failure and reports where in the
// microVU1 region it landed. The third test pins the flush the reset exists
// for: MTVU switched on with a VU1 program genuinely mid-run still finishes it.

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"

#include "Config.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "VU.h"
#include "VUmicro.h"

#include <cstring>
#include <gtest/gtest.h>

#if defined(__linux__)
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mvu_test_hooks
{
	void* GetResumeEntry(int vu_index);
} // namespace mvu_test_hooks

namespace mVUPersist
{
	void SetRecordingEnabled(bool enabled);
	bool IsRecordingEnabled();
} // namespace mVUPersist

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

// Multi-block loop, so a starved dispatch breaks mid-program (same program as
// mvu_resume_dispatch_tests.cpp). vi5 counts iterations (ends at 4), vi2 gets
// 0x55 at the E-bit end.
void LoadLoopProgram(VuTestHarness& h)
{
	h.SetVfBits(1, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);
	h.SetVi(vi::vi5, 0);
	h.LoadProgram({
		LowerOnly(VIADDIU_L(vi::vi1, vi::vi0, 4)),
		VuOp{VIADDIU_L(vi::vi5, vi::vi5, 1),
			VADD_U(mask::xyzw, vf::vf1, vf::vf1, vf::vf0)},
		LowerOnly(VIADDI_L(vi::vi1, vi::vi1, -1)),
		LowerOnly(0),
		LowerOnly(0),
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, -5)),
		LowerOnly(VIADDIU_L(vi::vi4, vi::vi0, 0xAA)),
		EBit(LowerOnly(VIADDIU_L(vi::vi2, vi::vi0, 0x55))),
	});
}

// What vu1ExecMicro / VU_Thread::ExecuteVU leave behind: TPC set, VU1 busy.
void SeedVu1Dispatch(u32 start_pc_bytes)
{
	vuRegs[1].VI[REG_TPC].UL = start_pc_bytes / 8u;
	CpuMicroVU1.SetStartPC(start_pc_bytes);
	vuRegs[0].VI[REG_VPU_STAT].UL |= 0x100u;
	vuRegs[1].VI[REG_VPU_STAT].UL = vuRegs[0].VI[REG_VPU_STAT].UL;
}

bool Vu1Busy()
{
	return (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100u) != 0;
}

class RecordingOffScope
{
	bool m_prev;

public:
	RecordingOffScope()
		: m_prev(mVUPersist::IsRecordingEnabled())
	{
		mVUPersist::SetRecordingEnabled(false);
	}
	~RecordingOffScope() { mVUPersist::SetRecordingEnabled(m_prev); }
};

#if defined(__linux__)
// Turns SIGILL / SIGSEGV / SIGBUS raised inside Run() into a return value, so
// the unfixed code fails the test instead of killing the test binary. The
// previous handlers (the fastmem fault handler among them) are put back.
class FaultGuard
{
public:
	struct Result
	{
		int signal = 0;
		uptr address = 0;
	};

	template <typename F>
	static Result Run(F&& fn)
	{
		struct sigaction sa = {};
		sa.sa_sigaction = &Handler;
		sa.sa_flags = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);
		struct sigaction old_ill, old_segv, old_bus;
		sigaction(SIGILL, &sa, &old_ill);
		sigaction(SIGSEGV, &sa, &old_segv);
		sigaction(SIGBUS, &sa, &old_bus);

		s_signal = 0;
		s_address = 0;
		if (sigsetjmp(s_jmp, 1) == 0)
			fn();

		sigaction(SIGILL, &old_ill, nullptr);
		sigaction(SIGSEGV, &old_segv, nullptr);
		sigaction(SIGBUS, &old_bus, nullptr);
		return Result{s_signal, s_address};
	}

private:
	static void Handler(int sig, siginfo_t* info, void*)
	{
		s_signal = sig;
		s_address = reinterpret_cast<uptr>(info->si_addr);
		siglongjmp(s_jmp, 1);
	}

	static inline sigjmp_buf s_jmp;
	static inline volatile sig_atomic_t s_signal = 0;
	static inline volatile uptr s_address = 0;
};

// What the child reports back to the test.
struct SecondBootResult
{
	bool forked = false;
	bool session_one_ok = false; // VU1 left busy (and a resume parked, if asked)
	bool bit_survived_release = false;
	bool allocated = false;
	int signal = 0;
	uptr fault_address = 0;
	uptr vu1_rec = 0;
	bool vu1_busy_after_cache_clear = false;
	bool program_ran = false;
};

// Session one: VU1's dispatcher generated under the session's MTVU setting, the
// loop program loaded and marked running. With parked_resume, a starved
// dispatch also breaks it at its first block, which parks a resume entry; the
// next Execute then enters through startFunctResume instead of startFunct.
bool LeaveVu1Busy(bool mtvu, bool parked_resume)
{
	EmuConfig.Speedhacks.vuThread = mtvu;
	CpuMicroVU1.Reset();

	VuTestHarness h(1);
	LoadLoopProgram(h);
	SeedVu1Dispatch(0);
	if (parked_resume)
	{
		CpuMicroVU1.Execute(0);
		if (!mvu_test_hooks::GetResumeEntry(1))
			return false;
	}
	return Vu1Busy();
}

// Session one, then the end of the CPU thread session the way
// CPUThreadShutdown does it, then a second session the way
// CPUThreadInitialize + VMManager::Initialize bring one up.
SecondBootResult SecondBootInThisProcess(bool mtvu_session_one, bool parked_resume)
{
	SecondBootResult r;
	r.forked = true;
	r.session_one_ok = LeaveVu1Busy(mtvu_session_one, parked_resume);
	if (!r.session_one_ok)
		return r;

	// The loop program as session one loaded it, for session two to run.
	u8 program[0x100];
	std::memcpy(program, vuRegs[1].Micro, sizeof(program));

	// CPUThreadShutdown: ShutdownCPUProviders, then SysMemory::Release.
	RecompilerTestEnvironment::Shutdown();
	r.bit_survived_release = Vu1Busy();

	// CPUThreadInitialize: SysMemory::Allocate, then InitializeCPUProviders.
	r.allocated = SysMemory::Allocate();
	if (!r.allocated)
		return r;
	recCpu.Reserve();
	psxRec.Reserve();
	CpuMicroVU0.Reserve();
	CpuMicroVU1.Reserve();
	r.vu1_rec = reinterpret_cast<uptr>(SysMemory::GetVU1Rec());

	// VMManager::Initialize with every recompiler and MTVU on:
	// UpdateCPUImplementations, ClearCPUExecutionCaches, SysMemory::Reset.
	EmuConfig.Speedhacks.vuThread = true;
	Cpu = &recCpu;
	psxCpu = &psxRec;
	CpuVU0 = &CpuMicroVU0;
	CpuVU1 = &CpuMicroVU1;
	const FaultGuard::Result fault = FaultGuard::Run([] { VMManager::Internal::ClearCPUExecutionCaches(); });
	r.signal = fault.signal;
	r.fault_address = fault.address;
	if (r.signal != 0)
		return r;

	r.vu1_busy_after_cache_clear = Vu1Busy();
	SysMemory::Reset();

	// The new session's VU1 runs a program to its end. (On MTVU the end does
	// not clear VU0's busy bit; the VU_MTVU_BUSY event does, so read the
	// program's own results.)
	std::memcpy(vuRegs[1].Micro, program, sizeof(program));
	for (int i = 0; i < 4; i++)
		vuRegs[1].VF[1].UL[i] = 0x3F800000u;
	SeedVu1Dispatch(0);
	CpuMicroVU1.Execute(4096);
	r.program_ran = (vuRegs[1].VI[vi::vi5].UL & 0xFFFFu) == 4u &&
		(vuRegs[1].VI[vi::vi2].UL & 0xFFFFu) == 0x55u;
	return r;
}

// Releasing and re-mapping the code memory would leave every other suite's
// generated code (the VIF unpackers, cached thunks) pointing at zeroes, so the
// two sessions run in a forked child and the result comes back over a pipe.
SecondBootResult SecondBoot(bool mtvu_session_one, bool parked_resume)
{
	SecondBootResult r;
	int fds[2];
	if (pipe(fds) != 0)
		return r;

	const pid_t pid = fork();
	if (pid == 0)
	{
		close(fds[0]);
		const SecondBootResult child = SecondBootInThisProcess(mtvu_session_one, parked_resume);
		const ssize_t written = write(fds[1], &child, sizeof(child));
		_exit(written == static_cast<ssize_t>(sizeof(child)) ? 0 : 1);
	}

	close(fds[1]);
	if (pid > 0)
	{
		ssize_t got = 0;
		while (got < static_cast<ssize_t>(sizeof(r)))
		{
			const ssize_t n = read(fds[0], reinterpret_cast<u8*>(&r) + got, sizeof(r) - got);
			if (n > 0)
				got += n;
			else if (n == 0 || errno != EINTR)
				break;
		}
		if (got != static_cast<ssize_t>(sizeof(r)))
			r = {};
		int status = 0;
		waitpid(pid, &status, 0);
	}
	close(fds[0]);
	return r;
}

void ExpectCleanSecondBoot(const SecondBootResult& r)
{
	ASSERT_TRUE(r.forked) << "the second-boot child did not report back";
	ASSERT_TRUE(r.session_one_ok) << "precondition: session one must end with VU1 busy";
	ASSERT_TRUE(r.bit_survived_release) << "precondition: VPU_STAT outlives the released VM memory";
	ASSERT_TRUE(r.allocated) << "SysMemory::Allocate failed for the second session";
	EXPECT_EQ(r.signal, 0)
		<< "the second boot's ClearCPUExecutionCaches faulted with signal " << r.signal
		<< " at microVU1 code region +0x" << std::hex << (r.fault_address - r.vu1_rec)
		<< " (address 0x" << r.fault_address << ")";
	if (r.signal != 0)
		return;
	EXPECT_FALSE(r.vu1_busy_after_cache_clear)
		<< "the stale VU1-busy bit must not survive the cache clear";
	EXPECT_TRUE(r.program_ran) << "the second session's VU1 did not run a program to its end";
}

#endif // __linux__

} // namespace

#if defined(__linux__)
// Session one on MTVU, VU1 marked busy (the MTVU busy window, Instant VU1 off),
// no resume parked: a dispatch enters at startFunct, the region's first byte.
// Unfixed, this faults at +0x0, the field reports' first address.
TEST(MvuSecondBoot, StaleMtvuBusyBitDoesNotCallIntoReleasedCode)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	ASSERT_TRUE(EmuConfig.Cpu.Recompiler.EnableVU0 && EmuConfig.Cpu.Recompiler.EnableVU1);
	RecordingOffScope recording_off;
	ExpectCleanSecondBoot(SecondBoot(/*mtvu_session_one=*/true, /*parked_resume=*/false));
}

// Session one without MTVU, VU1 stopped mid-program on its cycle budget with a
// resume parked: a dispatch enters at startFunctResume. With MTVU off and the
// EE and VU1 FPCRs equal (the defaults) the dispatcher emits no FPCR switches,
// which puts startFunctResume at +0x1b4, the field reports' second address
// (+0x1cc when the dispatcher was built with MTVU on).
TEST(MvuSecondBoot, StaleParkedVu1ResumeDoesNotCallIntoReleasedCode)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	ASSERT_TRUE(EmuConfig.Cpu.Recompiler.EnableVU0 && EmuConfig.Cpu.Recompiler.EnableVU1);
	RecordingOffScope recording_off;
	ExpectCleanSecondBoot(SecondBoot(/*mtvu_session_one=*/false, /*parked_resume=*/true));
}

#endif // __linux__

// What the flush is for: a VU1 program mid-run without MTVU, then MTVU
// switched on (settings changes reach mVUreset through
// ClearCPUExecutionCaches). VU0's reset must still run it to its end.
TEST(MvuSecondBoot, MtvuSwitchedOnMidProgramStillFinishesVu1)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	RecordingOffScope recording_off;
	BaseVUmicroCPU* const prev_vu1 = CpuVU1;
	CpuVU1 = &CpuMicroVU1;

	EmuConfig.Speedhacks.vuThread = false;
	CpuMicroVU1.Reset();
	VuTestHarness h(1);
	LoadLoopProgram(h);
	SeedVu1Dispatch(0);
	CpuMicroVU1.Execute(1);
	ASSERT_TRUE(Vu1Busy()) << "precondition: the starved dispatch must break mid-program";
	ASSERT_NE(vuRegs[1].VI[vi::vi5].UL & 0xFFFFu, 4u) << "precondition: the loop has not finished";

	EmuConfig.Speedhacks.vuThread = true;
	CpuMicroVU0.Reset();
	EXPECT_FALSE(Vu1Busy());
	EXPECT_EQ(vuRegs[1].VI[vi::vi5].UL & 0xFFFFu, 4u) << "VU0's reset did not finish the running VU1 program";
	EXPECT_EQ(vuRegs[1].VI[vi::vi2].UL & 0xFFFFu, 0x55u);

	EmuConfig.Speedhacks.vuThread = false;
	CpuMicroVU0.Reset();
	CpuMicroVU1.Reset();
	CpuVU1 = prev_vu1;
}

} // namespace recompiler_tests
