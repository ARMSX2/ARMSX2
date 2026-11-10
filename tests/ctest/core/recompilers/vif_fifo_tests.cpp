// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Reads of the VIF1 FIFO (ReadFIFO_VIF1, FiFo.cpp). The guest decides when it
// reads, so a read with nothing in the FIFO is something a game can do; it
// must not stop the emulator.

#include "harness/EeRecTestHarness.h"

#include "Vif.h"
#include "ps2/HwInternal.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

// A bug report: a game read the VIF1 FIFO while its count (FQC) was zero, and
// a release-build assertion aborted the process. The read gives zeroes.
TEST(VifFifo, ReadingAnEmptyVif1FifoGivesZeroes)
{
	recompiler_tests::EeRecTestHarness h;
	const tVIF_STAT saved = vif1Regs.stat;
	vif1Regs.stat._u32 = 0;

	mem128_t out;
	std::memset(&out, 0xa5, sizeof(out));
	ReadFIFO_VIF1(&out);

	EXPECT_EQ(out._u64[0], 0u);
	EXPECT_EQ(out._u64[1], 0u);
	EXPECT_EQ(vif1Regs.stat.FQC, 0u);
	vif1Regs.stat = saved;
}

} // namespace
