// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Where the IOP's JR and JALR jump: the value rs held before the delay slot
// and, for JALR, before the link. rpsxJR/rpsxJALR store rs to
// psxRegs.pcWriteback ahead of the slot and psxSetBranchReg jumps to that.
// The layout and the markers are those of ee_rec_jump_target_tests.cpp: pad A
// is right, pad B is a target read after the slot added 0xC, the trap is a
// jump that fell through. Run() compares every register with the interpreter.
//
// No divide feeds the target here: one IOP divide is charged more than the
// harness's 200-cycle budget, so the JIT stops at the block end after it while
// the interpreter, which counts per instruction, runs on. #918's constant
// folds are in the EE recompiler only.

#include "harness/JitTestHarness.h"

#include <gtest/gtest.h>

#include <iterator>
#include <string>
#include <vector>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;
constexpr u32 kPadA = kPc + 0x100;
constexpr u32 kPadB = kPadA + 0xC;
// I_MASK: a load from it goes through the load cold stub to the C handler.
constexpr u32 kMmio = 0x1F801074u;

constexpr u32 kMarkA = 0x111, kMarkB = 0x222, kMarkFell = 0x333;

constexpr u32 kMark = reg::s6;
constexpr u32 kParkReg = reg::s7;
constexpr u32 kSrc = reg::t5;
constexpr u32 kScratchReg = reg::t4;
constexpr u32 kMmioReg = reg::t3;
constexpr u32 kTmp1 = reg::t1;
constexpr u32 kTmp2 = reg::t2;

std::vector<u32> WithPads(std::vector<u32> code)
{
	code.push_back(ORI(kMark, reg::zero, kMarkFell));
	code.push_back(JR(kParkReg));
	code.push_back(NOP);
	EXPECT_LE(code.size(), (kPadA - kPc) / 4);
	code.resize((kPadA - kPc) / 4, NOP);
	code.push_back(ORI(kMark, reg::zero, kMarkA));
	code.push_back(JR(kParkReg));
	code.push_back(NOP);
	code.push_back(ORI(kMark, reg::zero, kMarkB));
	code.push_back(JR(kParkReg));
	code.push_back(NOP);
	return code;
}

enum class Source
{
	SetBeforeBlock,
	CopiedJustBefore,
	Constant,
	ShiftPair,
	MultPow2,
	ConstAcrossSites, // a constant rs across a RAM store, an MMIO load and a MULT
	Count
};

const char* SourceName(Source s)
{
	static const char* const names[] = {"SetBeforeBlock", "CopiedJustBefore", "Constant", "ShiftPair", "MultPow2",
		"ConstAcrossSites"};
	static_assert(std::size(names) == static_cast<size_t>(Source::Count));
	return names[static_cast<size_t>(s)];
}

enum class Slot
{
	Nop,
	AddToRs,
	LoadIntoRs,
	MmioLoad,
	RamStore,
	Count
};

const char* SlotName(Slot s)
{
	static const char* const names[] = {"Nop", "AddToRs", "LoadIntoRs", "MmioLoad", "RamStore"};
	static_assert(std::size(names) == static_cast<size_t>(Slot::Count));
	return names[static_cast<size_t>(s)];
}

void RunCase(u32 rs, Source source, Slot slot, bool link, u32 link_rd)
{
	SCOPED_TRACE(std::string(link ? "jalr rd=" + std::to_string(link_rd) : "jr") + " rs=" + std::to_string(rs) +
		" source=" + SourceName(source) + " slot=" + SlotName(slot));

	JitTestHarness h;
	h.SetGpr(kParkReg, RecompilerTestEnvironment::kParkingPc);
	h.SetGpr(kScratchReg, kScratch);
	h.SetGpr(kMmioReg, kMmio);
	h.SetGpr(kTmp2, 0x5A5A5A5Au);
	h.WriteU32(kScratch, kPadB);
	h.TrackMemWindow(kScratch, 16);

	std::vector<u32> code;
	switch (source)
	{
		case Source::SetBeforeBlock:
			h.SetGpr(rs, kPadA);
			break;
		case Source::CopiedJustBefore:
			h.SetGpr(kSrc, kPadA);
			code.push_back(ADDU(rs, kSrc, reg::zero));
			break;
		case Source::Constant:
			code.push_back(LUI(rs, static_cast<u16>(kPadA >> 16)));
			code.push_back(ORI(rs, rs, static_cast<u16>(kPadA)));
			break;
		case Source::ShiftPair:
			h.SetGpr(kSrc, 0x55010100u);
			code.push_back(SLL(rs, kSrc, 8));
			code.push_back(SRL(rs, rs, 8));
			break;
		case Source::MultPow2:
			h.SetGpr(kSrc, kPadA / 4);
			code.push_back(ADDIU(kTmp1, reg::zero, 4));
			code.push_back(MULTU(kSrc, kTmp1));
			code.push_back(MFLO(rs));
			break;
		case Source::ConstAcrossSites:
			code.push_back(LUI(rs, static_cast<u16>(kPadA >> 16)));
			code.push_back(ORI(rs, rs, static_cast<u16>(kPadA)));
			code.push_back(SW(kTmp2, 4, kScratchReg));
			code.push_back(LW(kTmp1, 0, kMmioReg));
			code.push_back(MULT(kTmp1, kTmp2));
			break;
		case Source::Count:
			break;
	}

	code.push_back(link ? JALR(link_rd, rs) : JR(rs));

	switch (slot)
	{
		case Slot::Nop: code.push_back(NOP); break;
		case Slot::AddToRs: code.push_back(ADDIU(rs, rs, 0xC)); break;
		case Slot::LoadIntoRs: code.push_back(LW(rs, 0, kScratchReg)); break;
		case Slot::MmioLoad: code.push_back(LW(kTmp1, 0, kMmioReg)); break;
		case Slot::RamStore: code.push_back(SW(kTmp2, 8, kScratchReg)); break;
		case Slot::Count: break;
	}

	const std::vector<u32> program = WithPads(std::move(code));
	h.LoadProgramAt(kPc, program.data(), program.size());
	h.Run();
	EXPECT_EQ(h.GetGprJit(kMark), kMarkA);
	EXPECT_EQ(h.GetGprInterp(kMark), kMarkA);
}

constexpr u32 kJumpRegs[] = {reg::a0, reg::v0, reg::t0, reg::t9, reg::ra, reg::sp, reg::s0, reg::k0, reg::at,
	reg::gp};

} // namespace

TEST(IopJumpTarget, JrJumpsToRsBeforeTheDelaySlot)
{
	u32 cases = 0;
	for (const u32 rs : kJumpRegs)
	{
		for (u32 s = 0; s < static_cast<u32>(Source::Count); s++)
		{
			for (u32 slot = 0; slot < static_cast<u32>(Slot::Count); slot++)
			{
				if (rs == reg::ra && static_cast<Source>(s) == Source::SetBeforeBlock)
					continue; // the harness sets $ra to the parking lot
				RunCase(rs, static_cast<Source>(s), static_cast<Slot>(slot), false, 0);
				cases++;
				if (::testing::Test::HasFailure())
					return;
			}
		}
	}
	EXPECT_EQ(cases, 295u);
}

TEST(IopJumpTarget, JalrJumpsToRsBeforeTheLinkAndTheDelaySlot)
{
	u32 cases = 0;
	for (const u32 rs : kJumpRegs)
	{
		for (const u32 rd : {static_cast<u32>(reg::ra), static_cast<u32>(reg::v1), rs})
		{
			for (u32 s = 0; s < static_cast<u32>(Source::Count); s++)
			{
				for (u32 slot = 0; slot < static_cast<u32>(Slot::Count); slot++)
				{
					if (rs == reg::ra && static_cast<Source>(s) == Source::SetBeforeBlock)
						continue;
					RunCase(rs, static_cast<Source>(s), static_cast<Slot>(slot), true, rd);
					cases++;
					if (::testing::Test::HasFailure())
						return;
				}
			}
		}
	}
	EXPECT_EQ(cases, 885u);
}
