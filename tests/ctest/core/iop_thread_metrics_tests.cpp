// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP thread's CPU time, next to the EE, GS and VU1 threads' in the OSD, the performance log and the PINE statistics.
// PerformanceMetrics keeps a handle to the thread while it runs, and none otherwise.

#include <gtest/gtest.h>

#include "IopThreadTestSupport.h"
#include "PerformanceMetrics.h"

#include <string>

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

// The stats line the OSD and the eerunner report print: the deferred SIF pumps and the SBUS near-bound waits are on it,
// per frame, apart from the pumps that held the IOP and the pumps the IOP started.
TEST(IopThreadStats, TheLineShowsDeferredSifPumpsAndNearBoundWaits)
{
	IopThread::Stats t = {};
	t.sif_ee_pumps = 4;
	t.sif_deferred = 20;
	t.sif_deferred_applied = 18;
	t.calls_posted = 30; // 18 deferred pumps and 12 the IOP started
	t.near_waits = 6;
	t.near_wait_ns = 3'000'000;

	const std::string line = IopThread::FormatStats(t, 2.0);
	EXPECT_NE(line.find("SIF pumps ee 2.00/f deferred 10.00/f (applied 9.00/f) iop 6.00/f"), std::string::npos) << line;
	EXPECT_NE(line.find("SBUS near-bound waits 3.00/f 1500.0 us/f"), std::string::npos) << line;
}

TEST(IopThreadStats, ACountOfIopStartedPumpsBelowTheAppliedItemsDoesNotGoNegative)
{
	// The counters are read while they move, so calls_posted can trail the applied items for a moment.
	IopThread::Stats t = {};
	t.sif_deferred_applied = 5;
	t.calls_posted = 3;
	const std::string line = IopThread::FormatStats(t, 1.0);
	EXPECT_NE(line.find("iop 0.00/f"), std::string::npos) << line;
}
