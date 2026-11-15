// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Extended RAM (EmuCore/CPU/ExtraMemory: 128MB EE map) on the EE recompiler.
//
// The recompiler sizes its block LUT, recRAM, recRAMCopy and the
// manual_page/manual_counter arrays from Ps2MemSize::ExposedRam and rebuilds
// them in recResetRaw when it changes. Arcade sessions have used the 128MB map
// since the System 246/256 support landed, but a System 256 game only needs
// 64MB, so nothing exercised code above 64MB. These tests run a block at
// 127MB, rewrite it and run it again, and load/store at the top of the map.
//
// Each test switches the map to 128MB and back through
// RecompilerTestEnvironment::SetExtraMemory, so the rest of the suite still
// sees a 32MB VM.

#include "harness/EeRecTestHarness.h"

#include "Memory.h"
#include "R5900.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

extern u32 recEeBlockGuestSize(u32 pc_query);

namespace {

constexpr u32 kPark = RecompilerTestEnvironment::kParkingPc;
// 127MB: above both the 32MB retail map and the 64MB a System 256 game uses.
constexpr u32 kHighCode = 0x07F00000;
constexpr u32 kHighData = Ps2MemSize::TotalRam - 0x10;

class EeRecExtendedRam : public ::testing::Test
{
protected:
	void SetUp() override { RecompilerTestEnvironment::SetExtraMemory(true); }
	void TearDown() override { RecompilerTestEnvironment::SetExtraMemory(false); }

	static void WriteHighCode(u32 v0_value)
	{
		memWrite32(kHighCode + 0x0, ADDIU(reg::v0, reg::zero, static_cast<s16>(v0_value)));
		memWrite32(kHighCode + 0x4, J(kPark));
		memWrite32(kHighCode + 0x8, NOP);
	}
};

} // namespace

TEST_F(EeRecExtendedRam, MapIs128MB)
{
	EXPECT_EQ(Ps2MemSize::ExposedRam, Ps2MemSize::TotalRam);
}

TEST_F(EeRecExtendedRam, JitRunsBlockAbove64MB)
{
	WriteHighCode(0x1234);

	EeRecTestHarness h;
	h.LoadProgramNoTerm({
		J(kHighCode),
		NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 0x1234ull);
	EXPECT_GT(recEeBlockGuestSize(kHighCode), 0u)
		<< "no EE JIT block was compiled at 0x" << std::hex << kHighCode;
}

TEST_F(EeRecExtendedRam, RewrittenBlockAbove64MBRecompiles)
{
	WriteHighCode(0x1234);

	EeRecTestHarness h;
	h.LoadProgramNoTerm({
		J(kHighCode),
		NOP,
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 0x1234ull);

	// Rewrite the compiled block's first word and invalidate it the way the
	// fastmem fault handler does. The second dispatch must recompile from the
	// new word, not reuse the block compiled from the old one.
	memWrite32(kHighCode, ADDIU(reg::v0, reg::zero, 0x5678));
	h.SimulateFastmemFault(kHighCode);
	h.Run(EeRecTestHarness::RunMode::PreserveCache);
	h.ExpectGpr64(reg::v0, 0x5678ull);
}

TEST_F(EeRecExtendedRam, LoadStoreAtTopOfMap)
{
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, kHighData);
	h.SetGpr64(reg::a1, 0x00000000CAFEF00Dull);
	h.TrackMemWindow(kHighData, 16);
	h.LoadProgram({
		SW(reg::a1, 0xC, reg::a0),
		LW(reg::v0, 0xC, reg::a0),
	});
	h.Run();
	h.ExpectGpr64(reg::v0, 0xFFFFFFFFCAFEF00Dull);
	EXPECT_EQ(h.ReadU32(kHighData + 0xC), 0xCAFEF00Du);
}
