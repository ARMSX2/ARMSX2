// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Memory-op operands read from where the guest values live.
//
// On the fastmem path a load or store needs its address in a W register and
// a store needs its value in a register. When the base is a pin or an
// allocator slot and the offset is zero, that register IS the address; when
// the store value is a pin or a slot, that register IS the value. Neither is
// copied into w9/w10 first. A masked 128-bit address (LQ, SQ, LQC2, SQC2) is
// one AND off the base, and off a constant base it is the constant.
//
// The shape tests count W-register moves into w9 and w10 (ORR wd, wzr, wm) in
// the block; the matrix runs every family with the base and the value in each
// home (pin, slot, memory, constant, $zero), with and without an offset, and
// with the base and the destination or value the same register, against the
// interpreter.

#include "harness/EeRecTestHarness.h"

#include "Config.h"

#include <gtest/gtest.h>

#include <vector>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;

struct MoveCounts
{
	u32 into_w9 = 0;
	u32 into_w10 = 0;
	u32 and_after_mov_w9 = 0; // MOV w9, #imm then AND w9, w9, #imm
};

MoveCounts MovesInBlock(u32 guest_pc)
{
	MoveCounts m;
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
	{
		ADD_FAILURE() << "no compiled block at 0x" << std::hex << guest_pc;
		return m;
	}
	const u32* code = reinterpret_cast<const u32*>(fn);
	for (u32 i = 0; i < size / 4; i++)
	{
		const u32 w = code[i];
		// ORR Wd, WZR, Wm (MOV Wd, Wm): 0x2A0003E0 | Rm << 16 | Rd
		if ((w & 0xFFE0FFE0u) == 0x2A0003E0u)
		{
			const u32 rd = w & 0x1F;
			m.into_w9 += rd == 9;
			m.into_w10 += rd == 10;
		}
		// AND w9, w9, #imm right after a MOVZ or MOVK into w9
		if (i > 0 && (w & 0xFF8003FFu) == 0x12000129u &&
			((code[i - 1] & 0xFF80001Fu) == 0x52800009u || (code[i - 1] & 0xFF80001Fu) == 0x72800009u))
			m.and_after_mov_w9++;
	}
	return m;
}

void Seed(EeRecTestHarness& h)
{
	for (u32 i = 0; i < 32; i++)
		h.WriteU32(kScratch + i * 4, 0xA0B0C0D0u ^ (i * 0x01010101u));
	h.TrackMemWindow(kScratch, 160);
}

} // namespace

// t4 is written by an ALU op and read again later, so the allocator keeps it
// in a slot; the load addresses off it with no copy.
TEST(EeRecMemOperand, LoadOffAResidentBaseUsesTheSlotAsTheIndex)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	EeRecTestHarness h;
	Seed(h);
	h.SetGpr64(reg::t1, kScratch);
	h.LoadProgram({
		DADDU(reg::t4, reg::t1, reg::zero),
		LW(reg::v1, 0, reg::t4),
		LW(reg::t5, 0, reg::t4),
		DADDU(reg::t6, reg::t4, reg::t5),
	});
	h.Run();
	EXPECT_EQ(MovesInBlock(kProgPc).into_w9, 0u);
}

TEST(EeRecMemOperand, StoreOfAResidentValueUsesTheSlotAsTheValue)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	EeRecTestHarness h;
	Seed(h);
	h.SetGpr64(reg::t1, 0x1234567890ABCDEFull);
	h.SetGpr64(reg::a0, kScratch);
	h.LoadProgram({
		DADDU(reg::t4, reg::t1, reg::zero),
		SW(reg::t4, 4, reg::a0),
		SD(reg::t4, 16, reg::a0),
		DADDU(reg::t6, reg::t4, reg::t4),
	});
	h.Run();
	EXPECT_EQ(MovesInBlock(kProgPc).into_w10, 0u);
}

TEST(EeRecMemOperand, QuadAccessesOffAResidentBaseAreOneAnd)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	EeRecTestHarness h;
	Seed(h);
	h.SetGpr64(reg::t1, kScratch + 5); // unaligned: the AND matters
	h.SetGpr64(reg::t2, kScratch + 0x40);
	h.LoadProgram({
		DADDU(reg::t4, reg::t1, reg::zero),
		DADDU(reg::t3, reg::t2, reg::zero),
		LQ(reg::v1, 0, reg::t4),
		SQ(reg::v1, 0x20, reg::t4),
		// COP2 quads off an aligned base: the interpreter does not mask theirs.
		LQC2(4, reg::t3, 0),
		SQC2(4, reg::t3, 0x30),
		DADDU(reg::t6, reg::t4, reg::t3),
	});
	h.Run();
	EXPECT_EQ(MovesInBlock(kProgPc).into_w9, 0u);
}

TEST(EeRecMemOperand, QuadAddressOffAConstantBaseIsTheConstant)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	EeRecTestHarness h;
	Seed(h);
	h.LoadProgram({
		LUI(reg::t4, kScratch >> 16),
		ORI(reg::t4, reg::t4, 0x0007),
		LQ(reg::v1, 0x10, reg::t4),
		LQC2(5, reg::t4, 0x20),
	});
	h.Run();
	EXPECT_EQ(MovesInBlock(kProgPc).and_after_mov_w9, 0u);
}

// $v1 is pinned (x26): UYA's feeder addresses its SDL/SDR off it.
TEST(EeRecMemOperand, UnalignedStoreOffAPinnedBaseUsesThePin)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	for (bool sdl : {false, true})
	{
		SCOPED_TRACE(sdl ? "SDL" : "SDR");
		EeRecTestHarness h;
		Seed(h);
		h.SetGpr64(reg::v1, kScratch + 3);
		h.SetGpr64(reg::s4, 0x0102030405060708ull);
		h.LoadProgram({sdl ? SDL(reg::s4, 0, reg::v1) : SDR(reg::s4, 0, reg::v1)});
		h.Run();
		EXPECT_EQ(MovesInBlock(kProgPc).into_w9, 0u);
	}
}

// Every family, the base in each home, offset zero and not, against the
// interpreter. Each home has an aligned base (word and doubleword accesses)
// and an odd one (bytes, quads, unaligned): pins $v0/$v1, slots t4/t3 (written
// by an ALU op above, read again below), memory t1/t2, constants t7/t8. The
// store values are a pin ($a2) and a slot (t5).
TEST(EeRecMemOperand, EveryFamilyEveryBaseHome)
{
	struct Home
	{
		const char* name;
		u32 aligned, odd;
	};
	const Home homes[] = {
		{"pin", reg::v0, reg::v1}, {"slot", reg::t4, reg::t3}, {"memory", reg::t1, reg::t2}, {"constant", reg::t7, reg::t8}};
	for (const Home& b : homes)
	{
		for (s16 off : {0, 8, 0x10})
		{
			SCOPED_TRACE(testing::Message() << "base " << b.name << " offset " << off);
			EeRecTestHarness h;
			Seed(h);
			h.SetGpr64(reg::v0, kScratch);
			h.SetGpr64(reg::v1, kScratch + 1);
			h.SetGpr64(reg::t1, kScratch);
			h.SetGpr64(reg::t2, kScratch + 3);
			h.SetGpr64(reg::s4, 0x8877665544332211ull);
			h.SetGpr64(reg::a2, 0x0F1E2D3C4B5A6978ull);
			const u32 A = b.aligned, O = b.odd;
			std::vector<u32> prog = {
				DADDU(reg::t4, reg::t1, reg::zero),
				DADDU(reg::t3, reg::t2, reg::zero),
				LUI(reg::t7, kScratch >> 16),
				LUI(reg::t8, kScratch >> 16),
				ORI(reg::t8, reg::t8, 0x0005),
				DADDU(reg::t5, reg::s4, reg::zero),
				LB(reg::t9, off, O),
				SB(reg::t5, off + 1, O),
				LQ(reg::k1, off, O),
				SQ(reg::t5, off + 0x30, O),
				// LQC2/SQC2 off the aligned base: the interpreter hands an
				// unmasked address to memRead128/memWrite128, the JIT masks it.
				LQC2(6, A, off & ~15),
				SQC2(6, A, (off & ~15) + 0x40),
				SDR(reg::t5, off + 0x58, O),
				SDL(reg::a2, off + 0x58, O),
				LW(reg::t9, off, A),
				LD(reg::s7, off, A),
				SW(reg::a2, off + 4, A),
				SD(reg::t5, off + 0x20, A),
				LWC1(1, off, A),
				SWC1(1, off + 0x50, A),
				DADDU(reg::t6, reg::t4, reg::t5),
				DADDU(reg::t6, reg::t6, reg::t3),
			};
			h.LoadProgram(prog);
			h.Run();
		}
	}
}

// The base and the destination, or the base and the value, are one register.
TEST(EeRecMemOperand, BaseAliasesTheDestOrTheValue)
{
	EeRecTestHarness h;
	Seed(h);
	h.WriteU32(kScratch + 0x40, kScratch + 0x10);
	h.SetGpr64(reg::t1, kScratch + 0x40);
	h.SetGpr64(reg::v1, kScratch + 0x60);
	h.LoadProgram({
		DADDU(reg::t4, reg::t1, reg::zero),
		SW(reg::t4, 4, reg::t4),          // value = base, slot-resident
		LW(reg::t4, 0, reg::t4),          // dest = base: t4 = kScratch + 0x10
		LW(reg::t4, 0, reg::t4),          // and again through the new value
		SW(reg::v1, 0, reg::v1),          // pin as both
		DADDU(reg::t6, reg::t4, reg::zero),
	});
	h.Run();
}
