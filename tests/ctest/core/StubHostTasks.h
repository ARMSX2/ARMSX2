// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The tasks the code under test posted with Host::RunOnCPUThread. StubHost.cpp keeps them for a test to look at and
// run.

#pragma once

#include <functional>
#include <vector>

namespace StubHost
{
	struct CpuThreadTask
	{
		std::function<void()> function;
		bool block;
	};

	// The tasks posted since the last call, oldest first.
	std::vector<CpuThreadTask> TakeCpuThreadTasks();
} // namespace StubHost
