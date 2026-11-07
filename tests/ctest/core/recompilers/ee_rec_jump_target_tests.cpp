// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Where JR and JALR jump. MIPS jumps to the value rs held before the delay
// slot, which may overwrite rs (JALR with rd == rs overwrites it before the
// slot). The recompiler copies rs into a parked register first
// (recJR/recJALR) and SetBranchReg reads that copy back, so every way the
// target can reach rs and every way the slot can disturb it is a chance to
// jump somewhere else.
//
// Each case jumps to pad A. A target read after the slot added 0xC lands on pad
// B instead, and falling through the jump lands on the trap after it; each
// writes a different marker to $s6 and leaves for the parking lot through $s7.
// Run() also compares every register with the interpreter.
//
// The ways in: rs set before the block, copied in by the instruction before
// the jump, built from constants, and computed by the operations #918 changed
// (a fused shift pair, multiply and divide by a constant, the MADD fold, and a
// constant left unwritten across a fastmem or MULT site). The slots: nothing,
// an add to rs, a load into rs, a faulting MMIO load into another register,
// and a RAM store.

#include "harness/EeRecTestHarness.h"

#include "Memory.h"
#include "vtlb.h"

#include <gtest/gtest.h>

#include <iterator>
#include <string>
#include <vector>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kPark = RecompilerTestEnvironment::kParkingPc;
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;
constexpr u32 kPadA = kPc + 0x100;
constexpr u32 kPadB = kPadA + 0xC;
// INTC_MASK: a load from it takes the fastmem fault, and reading it changes
// nothing (see ee_rec_const_flush_tests.cpp).
constexpr u32 kMmio = 0x1000F010u;

constexpr u32 kMarkA = 0x111, kMarkB = 0x222, kMarkFell = 0x333;

// Registers the cases use for themselves.
constexpr u32 kMark = reg::s6;
constexpr u32 kParkReg = reg::s7;
constexpr u32 kSrc = reg::t5;     // a value the target is computed from
constexpr u32 kScratchReg = reg::t4;
constexpr u32 kMmioReg = reg::t3;
constexpr u32 kTmp1 = reg::t1;
constexpr u32 kTmp2 = reg::t2;

constexpr u32 MULT3(u32 rd, u32 rs, u32 rt) { return MULT(rs, rt) | (rd << 11); }
constexpr u32 MULTU3(u32 rd, u32 rs, u32 rt) { return MULTU(rs, rt) | (rd << 11); }

// The case's code, a trap if the jump falls through, then the two pads.
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

// How the target gets into rs.
enum class Source
{
	SetBeforeBlock,  // a value the compiler does not know
	CopiedJustBefore, // ADDU rs, src, zero as the instruction before the jump
	Constant,        // LUI + ORI
	ShiftPair32,     // SLL then SRL, fused into one UBFX
	ShiftPairSigned, // SLL then SRA, fused into one SBFX
	AndiShift,       // ANDI then SLL, fused into one UBFIZ
	ShiftPair64,     // DSLL32 then DSRL32
	ShiftThenJump,   // SLL just before the jump, its pair half in the slot
	MultPow2,        // MULTU by a constant 4, MFLO
	Mult3Pow2,       // three-operand MULTU by a constant 4
	DivuPow2,        // DIVU by a constant 2, MFLO
	DivConst3,       // DIV by a constant 3, MFLO
	MaddFold,        // MADD of two constants onto LO
	ConstAcrossSites, // a constant rs across a RAM store, an MMIO load and a MULT
	ConstIntoMult,   // a constant rs that is also a MULT operand
	Count
};

const char* SourceName(Source s)
{
	static const char* const names[] = {"SetBeforeBlock", "CopiedJustBefore", "Constant", "ShiftPair32",
		"ShiftPairSigned", "AndiShift", "ShiftPair64", "ShiftThenJump", "MultPow2", "Mult3Pow2", "DivuPow2",
		"DivConst3", "MaddFold", "ConstAcrossSites", "ConstIntoMult"};
	static_assert(std::size(names) == static_cast<size_t>(Source::Count));
	return names[static_cast<size_t>(s)];
}

// What the delay slot does.
enum class Slot
{
	Nop,
	AddToRs,   // ADDIU rs, rs, 0xC
	LoadIntoRs, // LW rs from memory holding pad B
	MmioLoad,  // LW t1 from MMIO: the fastmem fault and its backpatch
	RamStore,  // SW t2 to scratch
	Count
};

const char* SlotName(Slot s)
{
	static const char* const names[] = {"Nop", "AddToRs", "LoadIntoRs", "MmioLoad", "RamStore"};
	static_assert(std::size(names) == static_cast<size_t>(Slot::Count));
	return names[static_cast<size_t>(s)];
}

struct Case
{
	u32 rs;
	Source source;
	Slot slot;
	// JR when link is false; JALR link_rd, rs otherwise.
	bool link;
	u32 link_rd;
};

std::string Describe(const Case& c)
{
	return std::string(c.link ? "jalr rd=" + std::to_string(c.link_rd) : "jr") + " rs=" + std::to_string(c.rs) +
		" source=" + SourceName(c.source) + " slot=" + SlotName(c.slot);
}

void RunCase(const Case& c)
{
	SCOPED_TRACE(Describe(c));

	EeRecTestHarness h;
	h.SetGpr64(kParkReg, kPark);
	h.SetGpr64(kScratchReg, kScratch);
	h.SetGpr64(kMmioReg, static_cast<s64>(static_cast<s32>(kMmio)));
	h.SetGpr64(kTmp2, 0x5A5A5A5Au);
	h.WriteU32(kScratch, kPadB);
	h.TrackMemWindow(kScratch, 16);

	std::vector<u32> code;
	const u32 rs = c.rs;
	switch (c.source)
	{
		case Source::SetBeforeBlock:
			h.SetGpr64(rs, kPadA);
			break;
		case Source::CopiedJustBefore:
			h.SetGpr64(kSrc, kPadA);
			code.push_back(ADDU(rs, kSrc, reg::zero));
			break;
		case Source::Constant:
			code.push_back(LUI(rs, static_cast<u16>(kPadA >> 16)));
			code.push_back(ORI(rs, rs, static_cast<u16>(kPadA)));
			break;
		case Source::ShiftPair32:
			h.SetGpr64(kSrc, 0x55010100u);
			code.push_back(SLL(rs, kSrc, 8));
			code.push_back(SRL(rs, rs, 8));
			break;
		case Source::ShiftPairSigned:
			h.SetGpr64(kSrc, 0x55010100u);
			code.push_back(SLL(rs, kSrc, 8));
			code.push_back(SRA(rs, rs, 8));
			break;
		case Source::AndiShift:
			h.SetGpr64(kSrc, 0xFFFF1010u);
			code.push_back(ANDI(rs, kSrc, 0xFFFF));
			code.push_back(SLL(rs, rs, 4));
			break;
		case Source::ShiftPair64:
			h.SetGpr64(kSrc, 0xDEADBEEF00000000ull | kPadA);
			code.push_back(ee::DSLL32(rs, kSrc, 0));
			code.push_back(ee::DSRL32(rs, rs, 0));
			break;
		case Source::ShiftThenJump:
			// The slot's SRL would complete a pair with the SLL, but the jump
			// sits between them and the target is the SLL's result.
			h.SetGpr64(kSrc, kPadA >> 4);
			code.push_back(SLL(rs, kSrc, 4));
			break;
		case Source::MultPow2:
			h.SetGpr64(kSrc, kPadA / 4);
			code.push_back(ADDIU(kTmp1, reg::zero, 4));
			code.push_back(MULTU(kSrc, kTmp1));
			code.push_back(MFLO(rs));
			break;
		case Source::Mult3Pow2:
			h.SetGpr64(kSrc, kPadA / 4);
			code.push_back(ADDIU(kTmp1, reg::zero, 4));
			code.push_back(MULTU3(rs, kSrc, kTmp1));
			break;
		case Source::DivuPow2:
			h.SetGpr64(kSrc, kPadA * 2);
			code.push_back(ADDIU(kTmp1, reg::zero, 2));
			code.push_back(DIVU(kSrc, kTmp1));
			code.push_back(MFLO(rs));
			break;
		case Source::DivConst3:
			h.SetGpr64(kSrc, kPadA * 3 + 2);
			code.push_back(ADDIU(kTmp1, reg::zero, 3));
			code.push_back(DIV(kSrc, kTmp1));
			code.push_back(MFLO(rs));
			break;
		case Source::MaddFold:
			h.SetGpr64(kSrc, kPadA - 8);
			code.push_back(MTLO(kSrc));
			code.push_back(MTHI(reg::zero));
			code.push_back(ADDIU(kTmp1, reg::zero, 4));
			code.push_back(ADDIU(kTmp2, reg::zero, 2));
			code.push_back(ee::MADD(rs, kTmp1, kTmp2));
			break;
		case Source::ConstAcrossSites:
			code.push_back(LUI(rs, static_cast<u16>(kPadA >> 16)));
			code.push_back(ORI(rs, rs, static_cast<u16>(kPadA)));
			code.push_back(SW(kTmp2, 4, kScratchReg));
			code.push_back(LW(kTmp1, 0, kMmioReg));
			code.push_back(MULT(kTmp1, kTmp2));
			break;
		case Source::ConstIntoMult:
			code.push_back(LUI(rs, static_cast<u16>(kPadA >> 16)));
			code.push_back(ORI(rs, rs, static_cast<u16>(kPadA)));
			code.push_back(ADDIU(kTmp1, reg::zero, 3));
			code.push_back(MULT(rs, kTmp1));
			break;
		case Source::Count:
			break;
	}

	code.push_back(c.link ? JALR(c.link_rd, rs) : JR(rs));

	switch (c.slot)
	{
		case Slot::Nop: code.push_back(NOP); break;
		case Slot::AddToRs: code.push_back(ADDIU(rs, rs, 0xC)); break;
		case Slot::LoadIntoRs: code.push_back(LW(rs, 0, kScratchReg)); break;
		case Slot::MmioLoad: code.push_back(LW(kTmp1, 0, kMmioReg)); break;
		case Slot::RamStore: code.push_back(SW(kTmp2, 8, kScratchReg)); break;
		case Slot::Count: break;
	}
	if (c.source == Source::ShiftThenJump && c.slot == Slot::Nop)
		code.back() = SRL(rs, rs, 4);

	vtlb_ClearLoadStoreInfo(); // the faulting-PC set is process-global
	h.LoadProgramNoTerm(WithPads(std::move(code)));
	h.Run();
	EXPECT_EQ(h.GetGpr64Jit(kMark), kMarkA);
	EXPECT_EQ(h.GetGpr64Interp(kMark), kMarkA);
}

// The registers the cases may jump through: every pinned one, and unpinned
// ones in and out of the caller-saved range. None of the registers the cases
// use for themselves.
constexpr u32 kJumpRegs[] = {reg::sp, reg::ra, reg::v0, reg::v1, reg::a0, reg::a1, reg::a2, reg::s0, reg::s1,
	reg::at, reg::t0, reg::t9, reg::k0, reg::s5, 30};

} // namespace

TEST(EeRecJumpTarget, JrJumpsToRsBeforeTheDelaySlot)
{
	u32 cases = 0;
	for (const u32 rs : kJumpRegs)
	{
		for (u32 s = 0; s < static_cast<u32>(Source::Count); s++)
		{
			for (u32 slot = 0; slot < static_cast<u32>(Slot::Count); slot++)
			{
				const Case c{rs, static_cast<Source>(s), static_cast<Slot>(slot), false, 0};
				if (rs == reg::ra && c.source == Source::SetBeforeBlock)
					continue; // the harness sets $ra to the parking lot
				RunCase(c);
				cases++;
				if (::testing::Test::HasFailure())
					return;
			}
		}
	}
	EXPECT_EQ(cases, 1120u);
}

// JALR's link register: $ra (the call idiom), another register, and rs itself,
// whose link the jump must not see.
TEST(EeRecJumpTarget, JalrJumpsToRsBeforeTheLinkAndTheDelaySlot)
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
					const Case c{rs, static_cast<Source>(s), static_cast<Slot>(slot), true, rd};
					if (rs == reg::ra && c.source == Source::SetBeforeBlock)
						continue;
					RunCase(c);
					cases++;
					if (::testing::Test::HasFailure())
						return;
				}
			}
		}
	}
	EXPECT_EQ(cases, 3360u);
}
