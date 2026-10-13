// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopThreadMode.h"

#include "Config.h"

#include <algorithm>

IopThreadMode IopThread::s_mode = IopThreadMode::Off;

namespace
{
	// Set once by the gtest environment before any test runs and cleared after the last, so a plain
	// variable does.
	bool s_recompiler_test_harness = false;
} // namespace

void IopThread::SetRecompilerTestHarness(bool active)
{
	s_recompiler_test_harness = active;
}

bool IopThread::IsRecompilerTestHarness()
{
	return s_recompiler_test_harness;
}

s32 IopThread::ClampLag(s32 requested)
{
	return std::clamp(requested, kMinLag, kMaxLag);
}

IopThreadMode IopThread::ResolveMode(const Pcsx2Config& config, const HostCpuInfo& host, const VmConditions& vm)
{
	if (!config.Speedhacks.iopThread)
		return IopThreadMode::Off;

	// Conditions about the VM apply to both modes: with any of them the IOP cannot run on a
	// thread of its own, whatever the host.
	if (vm.ps1_mode || vm.arcade || vm.gs_dump_replay || vm.iop_breakpoints || vm.recompiler_test_harness)
		return IopThreadMode::Off;

	// The x86 IOP JIT has not been audited for running on another thread, and x86 is not a target
	// for this feature (manager decision, 2026-10-13), so neither mode resolves there.
	if (!host.is_arm64)
		return IopThreadMode::Off;

	// Lockstep is a reference for tests, eerunner and debugging. The core rule below keeps a
	// player from being given a thread the machine cannot afford. Lockstep is not meant for play
	// and runs the two threads in strict turns, so it needs no spare core and the rule has
	// nothing to protect.
	if (config.Speedhacks.iopThreadLockstep)
		return IopThreadMode::Lockstep;

	// EE and GS threads, the IOP thread, and the VU1 thread when MTVU is on.
	const u32 threads = 2 + (config.Speedhacks.vuThread ? 1 : 0) + 1;
	const u32 spare_cores_for_the_rest = 2;
	if (host.performance_cores < threads || host.total_cores < threads + spare_cores_for_the_rest)
		return IopThreadMode::Off;

	return IopThreadMode::Threaded;
}
