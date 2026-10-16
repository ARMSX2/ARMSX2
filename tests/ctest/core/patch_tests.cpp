// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Patch.h"

#include "MockMemoryInterface.h"

#include "fmt/format.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Create a test that makes sure applying a given list of patch commands results
// in a certain sequence of memory reads/writes.
#define PATCH_TEST(name, ...) \
	static void patch_test_setup_expected_calls_##name(MockMemoryInterface& ee, MockMemoryInterface& iop); \
	TEST(Patch, name) \
	{ \
		testing::StrictMock<MockMemoryInterface> ee; \
		testing::StrictMock<MockMemoryInterface> iop; \
		{ \
			testing::InSequence seq; \
			patch_test_setup_expected_calls_##name(ee, iop); \
		} \
		Patch::PatchCommand commands[]{__VA_ARGS__}; \
		std::vector<const Patch::PatchCommand*> pointers; \
		pointers.reserve(std::size(commands)); \
		for (Patch::PatchCommand& command : commands) \
			pointers.push_back(&command); \
		Patch::ApplyPatches(pointers, Patch::PPT_ONCE_ON_LOAD, ee, iop); \
		Patch::ApplyPatches(pointers, Patch::PPT_CONTINUOUSLY, ee, iop); \
		Patch::ApplyPatches(pointers, Patch::PPT_COMBINED_0_1, ee, iop); \
		Patch::ApplyPatches(pointers, Patch::PPT_ON_LOAD_OR_WHEN_ENABLED, ee, iop); \
	} \
	static void patch_test_setup_expected_calls_##name(MockMemoryInterface& ee, MockMemoryInterface& iop)

static Patch::PatchCommand BuildPatchCommand(
	Patch::patch_place_type place,
	Patch::patch_cpu_type cpu,
	u32 address,
	Patch::patch_data_type type,
	u64 data)
{
	Patch::PatchCommand command;
	command.placetopatch = place;
	command.cpu = cpu;
	command.addr = address;
	command.type = type;
	command.data = data;
	return command;
}

// *****************************************************************************
// Writes
// *****************************************************************************

PATCH_TEST(Byte,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::BYTE_T, 0x12))
{
	ee.ExpectIdempotentWrite8(0x00100000, 0, 0x12);
}

PATCH_TEST(Short,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::SHORT_T, 0x1234))
{
	ee.ExpectIdempotentWrite16(0x00100000, 0, 0x1234);
}

PATCH_TEST(Word,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::WORD_T, 0x12345678))
{
	ee.ExpectIdempotentWrite32(0x00100000, 0, 0x12345678);
}

PATCH_TEST(Double,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::DOUBLE_T, 0x123456789acdef12))
{
	ee.ExpectIdempotentWrite64(0x00100000, 0, 0x123456789acdef12);
}

PATCH_TEST(BigEndianShort,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::SHORT_BE_T, 0x1234))
{
	ee.ExpectIdempotentWrite16(0x00100000, 0, 0x3412);
}

PATCH_TEST(BigEndianWord,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::WORD_BE_T, 0x12345678))
{
	ee.ExpectIdempotentWrite32(0x00100000, 0, 0x78563412);
}

PATCH_TEST(BigEndianDouble,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::DOUBLE_BE_T, 0xabcdef0123456789))
{
	ee.ExpectIdempotentWrite64(0x00100000, 0, 0x8967452301efcdab);
}

PATCH_TEST(IOPByte,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_IOP, 0x00100000, Patch::BYTE_T, 0x12))
{
	iop.ExpectIdempotentWrite8(0x00100000, 0, 0x12);
}

PATCH_TEST(IOPShort,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_IOP, 0x00100000, Patch::SHORT_T, 0x1234))
{
	iop.ExpectIdempotentWrite16(0x00100000, 0, 0x1234);
}

PATCH_TEST(IOPWord,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_IOP, 0x00100000, Patch::WORD_T, 0x12345678))
{
	iop.ExpectIdempotentWrite32(0x00100000, 0, 0x12345678);
}

// *****************************************************************************
// Writes (Extended)
// *****************************************************************************

PATCH_TEST(Extended8BitWrite,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0);
	ee.ExpectWrite8(0x00100000, 0x12);
}

PATCH_TEST(Extended16BitWrite,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x10100000, Patch::EXTENDED_T, 0x00001234))
{
	ee.ExpectRead16(0x00100000, 0);
	ee.ExpectWrite16(0x00100000, 0x1234);
}

PATCH_TEST(Extended32BitWrite,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20100000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
}

// *****************************************************************************
// Increments/Decrements (Extended)
// *****************************************************************************

PATCH_TEST(Extended8BitIncrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30000012, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30000012, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead8(0x00100000, 0x00);
	ee.ExpectWrite8(0x00100000, 0x12);
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectWrite8(0x00100000, 0x24);
}

PATCH_TEST(Extended8BitIncrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30000012, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30000012, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead8(0x00100000, 0xee);
	ee.ExpectWrite8(0x00100000, 0x00);
	ee.ExpectRead8(0x00100000, 0x00);
	ee.ExpectWrite8(0x00100000, 0x12);
}

PATCH_TEST(Extended8BitDecrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30100012, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30100012, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead8(0x00100000, 0x24);
	ee.ExpectWrite8(0x00100000, 0x12);
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectWrite8(0x00100000, 0x00);
}

PATCH_TEST(Extended8BitDecrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30100012, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30100012, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectWrite8(0x00100000, 0x00);
	ee.ExpectRead8(0x00100000, 0x00);
	ee.ExpectWrite8(0x00100000, 0xee);
}

PATCH_TEST(Extended16BitIncrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30201234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30201234, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead16(0x00100000, 0x0000);
	ee.ExpectWrite16(0x00100000, 0x1234);
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectWrite16(0x00100000, 0x2468);
}

PATCH_TEST(Extended16BitIncrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30201234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30201234, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead16(0x00100000, 0xedcc);
	ee.ExpectWrite16(0x00100000, 0x0000);
	ee.ExpectRead16(0x00100000, 0x0000);
	ee.ExpectWrite16(0x00100000, 0x1234);
}

PATCH_TEST(Extended16BitDecrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30301234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30301234, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead16(0x00100000, 0x2468);
	ee.ExpectWrite16(0x00100000, 0x1234);
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectWrite16(0x00100000, 0x0000);
}

PATCH_TEST(Extended16BitDecrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30301234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30301234, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectWrite16(0x00100000, 0x0000);
	ee.ExpectRead16(0x00100000, 0x0000);
	ee.ExpectWrite16(0x00100000, 0xedcc);
}

PATCH_TEST(Extended32BitIncrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30400000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30400000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0x00000000);
	ee.ExpectWrite32(0x00100000, 0x12345678);
	ee.ExpectRead32(0x00100000, 0x12345678);
	ee.ExpectWrite32(0x00100000, 0x2468acf0);
}

PATCH_TEST(Extended32BitIncrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30400000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30400000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0xedcba988);
	ee.ExpectWrite32(0x00100000, 0x00000000);
	ee.ExpectRead32(0x00100000, 0x00000000);
	ee.ExpectWrite32(0x00100000, 0x12345678);
}

PATCH_TEST(Extended32BitDecrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30500000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30500000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0x2468acf0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
	ee.ExpectRead32(0x00100000, 0x12345678);
	ee.ExpectWrite32(0x00100000, 0x00000000);
}

PATCH_TEST(Extended32BitDecrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30500000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30500000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0x12345678);
	ee.ExpectWrite32(0x00100000, 0x00000000);
	ee.ExpectRead32(0x00100000, 0x00000000);
	ee.ExpectWrite32(0x00100000, 0xedcba988);
}

// *****************************************************************************
// Serial Write (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedSerialWriteZero,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x40100000, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000000, Patch::EXTENDED_T, 0x00000000))
{
}

PATCH_TEST(ExtendedSerialWriteOnce,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x40100000, Patch::EXTENDED_T, 0x00010000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x11111111))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
}

PATCH_TEST(ExtendedSerialWriteContiguous,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x40100000, Patch::EXTENDED_T, 0x00020001),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x11111111))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
	ee.ExpectRead32(0x00100004, 0);
	ee.ExpectWrite32(0x00100004, 0x23456789);
}

PATCH_TEST(ExtendedSerialWriteStrided,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x40100000, Patch::EXTENDED_T, 0x00020002),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x11111111))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
	ee.ExpectRead32(0x00100008, 0);
	ee.ExpectWrite32(0x00100008, 0x23456789);
}

// *****************************************************************************
// Copy bytes (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedCopyBytes0,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x50100000, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000000))
{
}

PATCH_TEST(ExtendedCopyBytes2,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x50100000, Patch::EXTENDED_T, 0x00000002),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
	ee.ExpectRead8(0x00100001, 0x12);
	ee.ExpectRead8(0x00200001, 0);
	ee.ExpectWrite8(0x00200001, 0x12);
}

// *****************************************************************************
// Pointer write (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedPointerWrite8,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000001, Patch::EXTENDED_T, 0x00000004))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead8(0x00200004, 0);
	ee.ExpectWrite8(0x00200004, 0x12);
}

PATCH_TEST(ExtendedPointerWrite16,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x00001234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00010001, Patch::EXTENDED_T, 0x00000004))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead16(0x00200004, 0);
	ee.ExpectWrite16(0x00200004, 0x1234);
}

PATCH_TEST(ExtendedPointerWrite32,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020001, Patch::EXTENDED_T, 0x00000004))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead32(0x00200004, 0);
	ee.ExpectWrite32(0x00200004, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteMultiEven,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020002, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead32(0x00200004, 0x00300000);
	ee.ExpectRead32(0x00300008, 0);
	ee.ExpectWrite32(0x00300008, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteMultiOdd,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020003, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0000000c))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead32(0x00200004, 0x00300000);
	ee.ExpectRead32(0x00300008, 0x00400000);
	ee.ExpectRead32(0x0040000c, 0);
	ee.ExpectWrite32(0x0040000c, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsNullSingle,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020001, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

// There was previously a bug where if the pointer write command was split over
// three lines or more, if the first pointer was null it would interpret the
// middle of the pointer write command as the start of a new command.
PATCH_TEST(ExtendedPointerWriteSkipsFirstNullEven,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020002, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x0fffffff, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsFirstNullOdd,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020003, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsMiddleNullEven,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020004, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0000000c),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x0fffffff, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0x00110000);
	ee.ExpectRead32(0x00110004, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsMiddleNullOdd,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020005, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0000000c),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000010, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0x00110000);
	ee.ExpectRead32(0x00110004, 0x00120000);
	ee.ExpectRead32(0x00120008, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsLastNullEven,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020002, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x0fffffff, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0x00110000);
	ee.ExpectRead32(0x00110004, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsLastNullOdd,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020003, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0x00110000);
	ee.ExpectRead32(0x00110004, 0x00120000);
	ee.ExpectRead32(0x00120008, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

// *****************************************************************************
// Boolean operation (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedBooleanOr8,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0x78);
	ee.ExpectWrite8(0x00100000, 0x7a);
}

PATCH_TEST(ExtendedBooleanOr16,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00101234))
{
	ee.ExpectRead16(0x00100000, 0x89ab);
	ee.ExpectWrite16(0x00100000, 0x9bbf);
}

PATCH_TEST(ExtendedBooleanAnd8,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00200012))
{
	ee.ExpectRead8(0x00100000, 0x34);
	ee.ExpectWrite8(0x00100000, 0x10);
}

PATCH_TEST(ExtendedBooleanAnd16,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00301234))
{
	ee.ExpectRead16(0x00100000, 0x5678);
	ee.ExpectWrite16(0x00100000, 0x1230);
}

PATCH_TEST(ExtendedBooleanXor8,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00400012))
{
	ee.ExpectRead8(0x00100000, 0x89);
	ee.ExpectWrite8(0x00100000, 0x9b);
}

PATCH_TEST(ExtendedBooleanXor16,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00501234))
{
	ee.ExpectRead16(0x00100000, 0x89ab);
	ee.ExpectWrite16(0x00100000, 0x9b9f);
}

// *****************************************************************************
// Do multi-lines if conditional (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedConditional8BitEqualTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01010012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditional8BitEqualFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01010012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0x21);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalEqualTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01001234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalEqualFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01001234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x4321);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalNotEqualTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01101234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x4321);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
}

PATCH_TEST(ExtendedConditionalNotEqualFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01101234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x1234);
}

PATCH_TEST(ExtendedConditionalLessThanTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01101234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x4321);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
}

PATCH_TEST(ExtendedConditionalLessThanFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01101234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x1234);
}

PATCH_TEST(ExtendedConditionalECodeEqualTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xe0011234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalECodeEqualFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xe0011234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x4321);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalResetSkipCount,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0xff010012),
	BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0xab);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
}

// *****************************************************************************
// The vsync patches are applied in two passes, EE lines and then IOP lines
// *****************************************************************************
//
// Patch::ApplyVsyncPatches used to make one pass over the patch list with both memories. The IOP
// thread needs the IOP lines as a pass of their own (they have to run where IOP memory is owned),
// so the list is now walked once per CPU. The two memories are independent, so each must see the
// same accesses in the same order as before; only the interleaving between the two memories may
// change. These tests pin that, including the state that carries from one line to the next.

namespace
{
	// A guest memory that keeps its bytes and logs every access it gets, so that two ways of applying
	// the same patch list can be compared access for access.
	class LoggingMemory final : public MemoryInterface
	{
	public:
		std::vector<std::string> log;

		u8 Read8(u32 address, bool* valid = nullptr) override { return static_cast<u8>(Read("r8", address, 1, valid)); }
		u16 Read16(u32 address, bool* valid = nullptr) override { return static_cast<u16>(Read("r16", address, 2, valid)); }
		u32 Read32(u32 address, bool* valid = nullptr) override { return static_cast<u32>(Read("r32", address, 4, valid)); }
		u64 Read64(u32 address, bool* valid = nullptr) override { return Read("r64", address, 8, valid); }
		u128 Read128(u32 address, bool* valid = nullptr) override
		{
			Note("r128", address, 0);
			if (valid)
				*valid = true;
			return {};
		}
		bool ReadBytes(u32 address, void* dest, u32 size) override
		{
			Note("rbytes", address, size);
			std::memset(dest, 0, size);
			return true;
		}

		bool Write8(u32 address, u8 value) override { return Write("w8", address, 1, value); }
		bool Write16(u32 address, u16 value) override { return Write("w16", address, 2, value); }
		bool Write32(u32 address, u32 value) override { return Write("w32", address, 4, value); }
		bool Write64(u32 address, u64 value) override { return Write("w64", address, 8, value); }
		bool Write128(u32 address, u128 value) override
		{
			Note("w128", address, value._u64[0] ^ value._u64[1]);
			return true;
		}
		bool WriteBytes(u32 address, const void* src, u32 size) override
		{
			const u8* bytes = static_cast<const u8*>(src);
			u64 sum = 0;
			for (u32 i = 0; i < size; i++)
			{
				sum = sum * 31 + bytes[i];
				m_bytes[address + i] = bytes[i];
			}
			Note("wbytes", address, sum);
			return true;
		}
		bool CompareBytes(u32 address, const void* src, u32 size) override
		{
			const u8* bytes = static_cast<const u8*>(src);
			for (u32 i = 0; i < size; i++)
			{
				if (ByteAt(address + i) != bytes[i])
					return false;
			}
			return true;
		}

	private:
		std::map<u32, u8> m_bytes;

		u8 ByteAt(u32 address) const
		{
			const auto it = m_bytes.find(address);
			return it == m_bytes.end() ? 0 : it->second;
		}
		void Note(const char* what, u32 address, u64 value) { log.push_back(fmt::format("{} {:08x} {:x}", what, address, value)); }
		u64 Read(const char* what, u32 address, u32 size, bool* valid)
		{
			u64 value = 0;
			for (u32 i = 0; i < size; i++)
				value |= static_cast<u64>(ByteAt(address + i)) << (8 * i);
			Note(what, address, value);
			if (valid)
				*valid = true;
			return value;
		}
		bool Write(const char* what, u32 address, u32 size, u64 value)
		{
			for (u32 i = 0; i < size; i++)
				m_bytes[address + i] = static_cast<u8>(value >> (8 * i));
			Note(what, address, value);
			return true;
		}
	};

	constexpr Patch::patch_place_type kVsyncPlaces[] = {Patch::PPT_CONTINUOUSLY, Patch::PPT_COMBINED_0_1};

	// Every kind of line that matters to the split: EE and IOP lines interleaved, in both vsync
	// places and in the other two, a separator that resets the carried state, and an EE
	// conditional that skips the extended line after it with an IOP line in between.
	std::vector<Patch::PatchCommand> BuildMixedPatchList()
	{
		std::vector<Patch::PatchCommand> list;
		list.push_back(BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_EE, 0x00100000, Patch::WORD_T, 0x11111111));
		list.push_back(BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_IOP, 0x00001000, Patch::BYTE_T, 0xaa));
		list.push_back(BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01001234));
		list.push_back(BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_IOP, 0x00001004, Patch::SHORT_T, 0xbbbb));
		list.push_back(BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012)); // skipped
		list.push_back(BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000034));
		list.push_back(BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_IOP, 0x00001008, Patch::WORD_T, 0xdeadbeef)); // not a vsync place
		list.push_back(BuildPatchCommand(Patch::PPT_COMBINED_0_1, Patch::CPU_IOP, 0x0000100c, Patch::WORD_T, 0xcccccccc));
		list.push_back(BuildPatchCommand(Patch::PPT_COMBINED_0_1, Patch::CPU_EE, 0x00400000, Patch::SHORT_BE_T, 0x1234));
		list.push_back(BuildPatchCommand(Patch::PPT_COMBINED_0_1, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01001234));
		list.push_back(BuildPatchCommand(Patch::PPT_COMBINED_0_1, Patch::CPU_IOP, 0x00001010, Patch::WORD_T, 0xdddddddd));
		list.push_back(BuildPatchCommand(Patch::PPT_COMBINED_0_1, Patch::CPU_EE, 0x00500000, Patch::EXTENDED_T, 0x00000056)); // skipped
		list.push_back(BuildPatchCommand(Patch::PPT_ON_LOAD_OR_WHEN_ENABLED, Patch::CPU_EE, 0x00600000, Patch::WORD_T, 0x66666666));
		Patch::PatchCommand bytes = BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_IOP, 0x00001100, Patch::BYTES_T, 4);
		bytes.data_ptr = static_cast<u8*>(std::malloc(4));
		std::memcpy(bytes.data_ptr, "\x01\x02\x03\x04", 4);
		list.push_back(std::move(bytes));
		return list;
	}

	// The pointer list ApplyPatches takes. A null entry separates groups of lines and resets the
	// carried state, so the pass over one CPU has to honour it too.
	std::vector<const Patch::PatchCommand*> PointersWithSeparator(const std::vector<Patch::PatchCommand>& list)
	{
		std::vector<const Patch::PatchCommand*> pointers;
		for (size_t i = 0; i < list.size(); i++)
		{
			pointers.push_back(&list[i]);
			if (i == 5)
				pointers.push_back(nullptr);
		}
		return pointers;
	}

	void ApplyVsyncPlaces(const std::vector<const Patch::PatchCommand*>& pointers, LoggingMemory& ee, LoggingMemory& iop, Patch::patch_cpu_filter filter)
	{
		for (const Patch::patch_place_type place : kVsyncPlaces)
			Patch::ApplyPatches(pointers, place, ee, iop, filter);
	}
} // namespace

TEST(Patch, VsyncPassPerCpuGivesEachMemoryTheSameAccessesAsOnePassOverAllLines)
{
	const std::vector<Patch::PatchCommand> list = BuildMixedPatchList();
	const std::vector<const Patch::PatchCommand*> pointers = PointersWithSeparator(list);

	LoggingMemory ee_one, iop_one;
	ApplyVsyncPlaces(pointers, ee_one, iop_one, Patch::patch_cpu_filter::Both);

	LoggingMemory ee_two, iop_two;
	ApplyVsyncPlaces(pointers, ee_two, iop_two, Patch::patch_cpu_filter::EEOnly);
	ApplyVsyncPlaces(pointers, ee_two, iop_two, Patch::patch_cpu_filter::IOPOnly);

	EXPECT_EQ(ee_one.log, ee_two.log);
	EXPECT_EQ(iop_one.log, iop_two.log);

	// The comparison means nothing if neither memory was written.
	const auto wrote = [](const std::vector<std::string>& log, const char* what) {
		return std::count_if(log.begin(), log.end(), [&](const std::string& s) { return s.rfind(what, 0) == 0; });
	};
	EXPECT_GE(wrote(ee_one.log, "w32"), 1);
	EXPECT_GE(wrote(iop_one.log, "w32"), 2);
	EXPECT_GE(wrote(iop_one.log, "wbytes"), 1);
	// The two lines the conditionals skip must be skipped in both ways of applying the list.
	const auto touched = [](const std::vector<std::string>& log, const char* address) {
		return std::count_if(log.begin(), log.end(), [&](const std::string& s) { return s.find(address) != std::string::npos; });
	};
	EXPECT_EQ(touched(ee_two.log, "00200000"), 0);
	EXPECT_EQ(touched(ee_two.log, "00500000"), 0);
	EXPECT_GE(touched(ee_two.log, "00300000"), 1);
}

TEST(Patch, EEOnlyPassNeverTouchesIopMemoryAndIOPOnlyPassNeverTouchesEeMemory)
{
	const std::vector<Patch::PatchCommand> list = BuildMixedPatchList();
	const std::vector<const Patch::PatchCommand*> pointers = PointersWithSeparator(list);

	LoggingMemory ee_a, iop_a;
	ApplyVsyncPlaces(pointers, ee_a, iop_a, Patch::patch_cpu_filter::EEOnly);
	EXPECT_FALSE(ee_a.log.empty());
	EXPECT_TRUE(iop_a.log.empty());

	LoggingMemory ee_b, iop_b;
	ApplyVsyncPlaces(pointers, ee_b, iop_b, Patch::patch_cpu_filter::IOPOnly);
	EXPECT_TRUE(ee_b.log.empty());
	EXPECT_FALSE(iop_b.log.empty());
}

TEST(Patch, ApplyPatchesAppliesBothCpusWhenNoFilterIsGiven)
{
	const std::vector<Patch::PatchCommand> list = BuildMixedPatchList();
	const std::vector<const Patch::PatchCommand*> pointers = PointersWithSeparator(list);

	LoggingMemory ee_default, iop_default;
	for (const Patch::patch_place_type place : kVsyncPlaces)
		Patch::ApplyPatches(pointers, place, ee_default, iop_default);

	LoggingMemory ee_both, iop_both;
	ApplyVsyncPlaces(pointers, ee_both, iop_both, Patch::patch_cpu_filter::Both);

	EXPECT_EQ(ee_default.log, ee_both.log);
	EXPECT_EQ(iop_default.log, iop_both.log);
}
