// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopThreadMode.h"

#include "Config.h"

#include <algorithm>

s32 IopThread::ClampLag(s32 requested)
{
	return std::clamp(requested, kMinLag, kMaxLag);
}

IopThreadMode IopThread::ResolveMode(const Pcsx2Config& config, const HostCpuInfo& host, const VmConditions& vm)
{
	if (!config.Speedhacks.iopThread)
		return IopThreadMode::Off;

	if (!host.is_arm64)
		return IopThreadMode::Off;

	if (vm.ps1_mode || vm.arcade || vm.gs_dump_replay || vm.iop_breakpoints || vm.recompiler_test_harness)
		return IopThreadMode::Off;

	// EE and GS threads, the IOP thread, and the VU1 thread when MTVU is on.
	const u32 threads = 2 + (config.Speedhacks.vuThread ? 1 : 0) + 1;
	const u32 spare_cores_for_the_rest = 2;
	if (host.performance_cores < threads || host.total_cores < threads + spare_cores_for_the_rest)
		return IopThreadMode::Off;

	return config.Speedhacks.iopThreadLockstep ? IopThreadMode::Lockstep : IopThreadMode::Threaded;
}
