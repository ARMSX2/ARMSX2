// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the code under test handed the host: the tasks it posted with Host::RunOnCPUThread and the errors it
// reported with Host::ReportErrorAsync. StubHost.cpp keeps them for a test to look at.

#pragma once

#include <functional>
#include <string>
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

	struct ErrorReport
	{
		std::string title;
		std::string message;
	};

	// The errors reported since the last call, oldest first.
	std::vector<ErrorReport> TakeErrorReports();
} // namespace StubHost
