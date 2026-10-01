// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Model-call stub guard.
//
// mVUGenerateModelStubs emits the EE FPU / EFU model seam once per target. Two
// things about that are per-VU and invisible to a JIT-vs-interp diff: a build
// that loses the generation call leaves the table null, and the mul-band
// target is chosen by mVU.index, the two functions reading different slots of
// g_vuMulBand. Both only bite at vuClampMode 4.

#include "harness/RecompilerTestEnvironment.h"

#include "common/Pcsx2Defs.h"

#include <gtest/gtest.h>

#include <set>

// Defined in pcsx2/arm64/microVU-arm64.cpp (PCSX2_RECOMPILER_TESTS builds only).
// Global scope — must be declared outside namespace recompiler_tests.
int mVUTestProbe_ModelStubCount();
const u8* mVUTestProbe_ModelStub(int index, int stub);
const void* mVUTestProbe_ModelStubTarget(int index, int stub);

namespace recompiler_tests {

// Generated during environment init, so no VU program has to run.
TEST(MvuModelStubs, BothVusGetOneStubPerTarget)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());

	const int count = mVUTestProbe_ModelStubCount();
	for (int index = 0; index < 2; index++)
	{
		std::set<const u8*> seen;
		for (int stub = 0; stub < count; stub++)
		{
			const u8* p = mVUTestProbe_ModelStub(index, stub);
			EXPECT_NE(p, nullptr) << "VU" << index << " stub " << stub << " never generated";
			EXPECT_TRUE(seen.insert(p).second)
				<< "VU" << index << " stub " << stub << " shares an entry with an earlier target";
		}
	}
}

TEST(MvuModelStubs, OnlyTheMulBandTargetIsPerVu)
{
	const int count = mVUTestProbe_ModelStubCount();
	int differ = 0;
	for (int stub = 0; stub < count; stub++)
	{
		const void* vu0 = mVUTestProbe_ModelStubTarget(0, stub);
		const void* vu1 = mVUTestProbe_ModelStubTarget(1, stub);
		ASSERT_NE(vu0, nullptr) << "stub " << stub;
		ASSERT_NE(vu1, nullptr) << "stub " << stub;
		if (vu0 != vu1)
			differ++;
	}
	EXPECT_EQ(differ, 1) << "exactly the mul-band target takes the VU index";
}

} // namespace recompiler_tests
