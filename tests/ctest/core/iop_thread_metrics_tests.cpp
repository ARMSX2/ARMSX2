// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP thread's CPU time, next to the EE, GS and VU1 threads' in the OSD, the performance log and the PINE statistics.
// PerformanceMetrics keeps a handle to the thread while it runs, and none otherwise.

#include <gtest/gtest.h>

#include "IopThreadTestSupport.h"
#include "PerformanceMetrics.h"

TEST(IopThreadMetrics, TheThreadsCpuTimeIsReportedWhileItRuns)
{
	iopt::SettingsGuard guard;
	EXPECT_FALSE(PerformanceMetrics::HasIOPThread());

	iopt::StartLockstep();
	EXPECT_TRUE(PerformanceMetrics::HasIOPThread());

	IopThread::StopThread();
	EXPECT_FALSE(PerformanceMetrics::HasIOPThread());
	// With no thread both figures read zero, like the GS back thread's.
	EXPECT_EQ(PerformanceMetrics::GetIOPThreadUsage(), 0.0f);
	EXPECT_EQ(PerformanceMetrics::GetIOPThreadAverageTime(), 0.0f);
}
