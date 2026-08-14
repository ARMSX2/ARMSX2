// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The FPR slot is 64 bits (R5900.h); the architectural register and the
// savestate block are 32. This pins that boundary.

#include "R5900.h"

#include "common/Pcsx2Defs.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

namespace
{
	// Zero and negative zero, the denormal boundary, an ordinary value, the top
	// binade, the EE maximum, and a raw CVT.W.S integer payload.
	constexpr u32 kWords[] = {
		0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x00800000, 0x80800000,
		0x3F800000, 0x7F7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7FFFFFFF,
		0xFFFFFFFF, 0x0000000A, 0x80000001, 0x4E800000,
	};
} // namespace

// The 264-byte block every savestate carries, field for field.
TEST(EeFpuRegFile, TheWireBlockIsTheSavestateLayout)
{
	EXPECT_EQ(sizeof(fpuRegistersWire), 264u);
	EXPECT_EQ(offsetof(fpuRegistersWire, fpr), 0u);
	EXPECT_EQ(offsetof(fpuRegistersWire, fprc), 128u);
	EXPECT_EQ(offsetof(fpuRegistersWire, ACC), 256u);
	EXPECT_EQ(offsetof(fpuRegistersWire, ACCflag), 260u);
}

// The stride the emitters compute from &fpuRegs.fpr[n] is the slot's size.
TEST(EeFpuRegFile, TheWordIsTheSlotsLowHalf)
{
	EXPECT_EQ(sizeof(FPRreg), 8u);

	for (u32 word : kWords)
	{
		FPRreg slot;
		slot.UD = UINT64_C(0xA5A5A5A5) << 32 | 0x5A5A5A5A;
		slot.UL = word;
		EXPECT_EQ(slot.UD & 0xFFFFFFFFu, word) << std::hex << word;
		EXPECT_EQ(slot.UD >> 32, 0xA5A5A5A5u) << std::hex << word;
	}
}

TEST(EeFpuRegFile, TheWireRoundTripIsExact)
{
	fpuRegisters saved;
	std::memcpy(&saved, &fpuRegs, sizeof(fpuRegisters));

	for (int i = 0; i < 32; i++)
	{
		fpuRegs.fpr[i].UL = kWords[i % std::size(kWords)] ^ static_cast<u32>(i);
		fpuRegs.fprc[i] = 0xC0DE0000u | static_cast<u32>(i);
	}
	fpuRegs.ACC.UL = 0x7FFFFFFF;
	fpuRegs.ACCflag = 1;

	fpuRegistersWire wire;
	fpuRegsToWire(wire);

	for (int i = 0; i < 32; i++)
	{
		EXPECT_EQ(wire.fpr[i], fpuRegs.fpr[i].UL) << "fpr" << i;
		EXPECT_EQ(wire.fprc[i], fpuRegs.fprc[i]) << "fprc" << i;
	}
	EXPECT_EQ(wire.ACC, 0x7FFFFFFFu);
	EXPECT_EQ(wire.ACCflag, 1u);

	// 0xA5 everywhere, so a load that writes only the word leaves the upper
	// half set.
	std::memset(&fpuRegs, 0xA5, sizeof(fpuRegisters));
	fpuRegsFromWire(wire);

	for (int i = 0; i < 32; i++)
	{
		EXPECT_EQ(fpuRegs.fpr[i].UD, static_cast<u64>(wire.fpr[i])) << "fpr" << i;
		EXPECT_EQ(fpuRegs.fprc[i], wire.fprc[i]) << "fprc" << i;
	}
	EXPECT_EQ(fpuRegs.ACC.UD, UINT64_C(0x7FFFFFFF));
	EXPECT_EQ(fpuRegs.ACCflag, 1u);

	std::memcpy(&fpuRegs, &saved, sizeof(fpuRegisters));
}
