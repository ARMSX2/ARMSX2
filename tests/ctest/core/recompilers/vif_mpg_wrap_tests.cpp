// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// An MPG upload that runs past the end of micro memory wraps to address 0.
// Both parts of it replace code, so both have to reach the VU recompiler's
// Clear, or a cached program over the wrapped part keeps running.

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "arm64/microVU_Persist-arm64.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

constexpr u32 kOld = VIADDIU_L(vi::vi1, vi::vi0, 0x11);
constexpr u32 kNew = VIADDIU_L(vi::vi1, vi::vi0, 0x22);

// An MPG of two pairs at the last pair, through the VIF command handler with
// the JIT as the VU: the second pair lands on pair 0 with `lower` in it.
void UploadWrapping(int idx, u32 lower)
{
	const u32 lastPair = (idx ? 0x4000u : 0x1000u) / 8 - 1;
	const VuOp nop = NopPair();
	const u32 data[4] = {nop.lower, nop.upper, lower, VNOP_U()};

	BaseVUmicroCPU*& cpu = idx ? CpuVU1 : CpuVU0;
	BaseVUmicroCPU* const saved = cpu;
	cpu = idx ? static_cast<BaseVUmicroCPU*>(&CpuMicroVU1) : static_cast<BaseVUmicroCPU*>(&CpuMicroVU0);

	vifStruct& vif = idx ? vif1 : vif0;
	VIFregisters& regs = idx ? vif1Regs : vif0Regs;
	regs.code = (0x4Au << 24) | (2u << 16) | lastPair; // MPG, 2 pairs, at the last pair
	const int pass1 = vifCmdHandler[idx][0x4A](0, nullptr);
	vif.vifpacketsize = 4;
	const int pass2 = vifCmdHandler[idx][0x4A](1, data);
	cpu = saved;

	ASSERT_EQ(pass1, 1) << "MPG waited for the VU";
	ASSERT_EQ(pass2, 4);
	u32 word0;
	std::memcpy(&word0, vuRegs[idx].Micro, sizeof(word0));
	ASSERT_EQ(word0, lower) << "the MPG did not wrap to pair 0";
}

class VifMpgWrap : public ::testing::TestWithParam<int>
{
};

} // namespace

// The program at pair 0 is compiled and warm across a wrapping upload of the
// same code; a wrapping upload of new code then has to replace it.
TEST_P(VifMpgWrap, WrappedPartReplacesCompiledCode)
{
	const int idx = GetParam();
	const bool savedThread = EmuConfig.Speedhacks.vuThread;
	EmuConfig.Speedhacks.vuThread = false; // the path without MTVU

	VuTestHarness h(idx);
	h.LoadProgram({LowerOnly(kOld), EBitNopPair()});
	h.Run();
	ASSERT_EQ(h.GetViJit(vi::vi1), 0x11u);

	// Any micro-memory write resets the entry state the next run searches
	// with, so the first run after one compiles a second variant of the block.
	UploadWrapping(idx, kOld);
	h.RunJitPreserveBlockCache();
	const u64 warm = mVUPersist::GetBlockCompileCount(idx);
	UploadWrapping(idx, kOld);
	h.RunJitPreserveBlockCache();
	ASSERT_EQ(mVUPersist::GetBlockCompileCount(idx), warm) << "block never went warm";
	ASSERT_EQ(h.GetViJit(vi::vi1), 0x11u);

	UploadWrapping(idx, kNew);
	h.RunJitPreserveBlockCache();
	EmuConfig.Speedhacks.vuThread = savedThread;
	EXPECT_EQ(h.GetViJit(vi::vi1), 0x22u) << "the program compiled before the upload ran";
}

INSTANTIATE_TEST_SUITE_P(Vu, VifMpgWrap, ::testing::Values(0, 1),
	[](const ::testing::TestParamInfo<int>& info) { return "VU" + std::to_string(info.param); });

} // namespace recompiler_tests
