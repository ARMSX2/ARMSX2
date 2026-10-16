// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "harness/RecompilerTestEnvironment.h"

#include "IopThread/IopThreadMode.h"

#include <gtest/gtest.h>

namespace
{

// gtest-side wrapper that delegates to the shared (gtest-free)
// RecompilerTestEnvironment::Initialize/Shutdown so pcsx2-vurunner can use
// the same setup logic without linking gtest.
class GtestEnvironment : public ::testing::Environment
{
public:
	void SetUp() override
	{
		// The DiffJitVsInterp harness is a single-thread EE and IOP interleave. This flag is how
		// IopThread::ResolveMode knows to keep the IOP thread out of it. It is set here, by the gtest
		// environment, and not from the compile-time define: eerunner is built with the define too.
		IopThread::SetRecompilerTestHarness(true);
		if (!recompiler_tests::RecompilerTestEnvironment::Initialize())
			ADD_FAILURE() << "RecompilerTestEnvironment::Initialize() failed (likely SysMemory::Allocate)";
	}
	void TearDown() override
	{
		recompiler_tests::RecompilerTestEnvironment::Shutdown();
		IopThread::SetRecompilerTestHarness(false);
	}
};

} // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	::testing::AddGlobalTestEnvironment(new GtestEnvironment());
	return RUN_ALL_TESTS();
}
