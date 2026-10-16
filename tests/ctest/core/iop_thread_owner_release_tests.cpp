// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The ownership asserts compile to nothing in a Release build (IOP thread design 6.3). This file is built
// into the Devel test binary like the rest, so it undefines the two build-type macros before the only
// header it includes that reads them, and checks what the assert macros expand to. A Release build of the
// emulator defines neither macro.
//
// Only OwnerAsserts.h (and the two small headers it includes) is included after the undefs: any header
// with an inline function that reads PCSX2_DEVBUILD would be compiled two ways in one program.

#undef PCSX2_DEVBUILD
#undef PCSX2_DEBUG

#include "IopThread/OwnerAsserts.h"

#include <gtest/gtest.h>

#define IOP_TEST_STR2(x) #x
#define IOP_TEST_STR(x) IOP_TEST_STR2(x)

TEST(IopThreadOwnerAssertsRelease, BothMacrosExpandToNothing)
{
	EXPECT_STREQ(IOP_TEST_STR(IOP_SIDE_ASSERT()), "((void)0)");
	EXPECT_STREQ(IOP_TEST_STR(EE_SIDE_ASSERT()), "((void)0)");
}

TEST(IopThreadOwnerAssertsRelease, ACallingFunctionReadsNoModeAndCallsNothing)
{
	// If the macros expanded to a check, this would read the mode and call CheckIopSide on a violation.
	// Run with the mode forced to something that would fail it: nothing happens.
	const IopThreadMode saved = IopThread::s_mode;
	IopThread::s_mode = IopThreadMode::Threaded;
	IopThread::ResetViolationCount();
	IOP_SIDE_ASSERT();
	EE_SIDE_ASSERT();
	IopThread::s_mode = saved;
	EXPECT_EQ(IopThread::ViolationCount(), 0u);
}
