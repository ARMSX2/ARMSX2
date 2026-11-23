// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the EE charges for a register jump (JR, JALR) whose register was
// written by the instruction right before it.
//
// Measured on an SCPH-30001: a JR or JALR whose register comes from a load
// (LW, LD, LQ) or an MFC0 immediately before it costs about 3 cycles more than
// the table charges, in every form timed (JR, JALR, JR RA). With one
// instruction or more between them, or with the register written by an ALU op
// right before, the table's charge holds to within a cycle. Each program below
// reads COP0 Count, runs one block that ends in the jump, lands on a settling
// block and reads Count again; both engines must charge the same.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EECycleRate.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"

#include <gtest/gtest.h>

#include <vector>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;
constexpr u32 kCp0Count = 9;
constexpr u32 kCp0Status = 12;
constexpr u32 kCp0Epc = 14;
constexpr u32 kCp0Config = 16;
constexpr u32 kStatusCu0 = 1u << 28;
constexpr u32 kConfigDie = 1u << 18; // dual issue on, as the BIOS sets it

// The extra cost of a register jump right after the load or MFC0 that wrote
// its register, in cycles.
constexpr u64 kJumpAfterLoadCycles = 3;

u32 Eighths(u32 code) { return R5900::GetInstruction(code).cycles; }

struct ScopedClean
{
	bool wait_loop;
	s8 rate;
	ScopedClean()
		: wait_loop(EmuConfig.Speedhacks.WaitLoop)
		, rate(EmuConfig.Speedhacks.EECycleRate)
	{
		EmuConfig.Speedhacks.WaitLoop = false;
		EmuConfig.Speedhacks.EECycleRate = 0;
		EECycleRate::SyncToConfigured();
	}
	~ScopedClean()
	{
		EmuConfig.Speedhacks.WaitLoop = wait_loop;
		EmuConfig.Speedhacks.EECycleRate = rate;
		EECycleRate::SyncToConfigured();
	}
};

enum class Producer
{
	Lw,
	Ld,
	Lq,
	Mfc0,
	Alu,
};

struct Form
{
	const char* name;
	Producer producer;
	u32 between; // independent instructions between the producer and the jump
	bool jalr;
	u32 reg;     // the jump's register
	u64 extra;   // cycles beyond the table
};

const Form kForms[] = {
	{"lw then jr", Producer::Lw, 0, false, reg::t2, kJumpAfterLoadCycles},
	{"ld then jr", Producer::Ld, 0, false, reg::t2, kJumpAfterLoadCycles},
	{"lq then jr", Producer::Lq, 0, false, reg::t2, kJumpAfterLoadCycles},
	{"lw then jalr", Producer::Lw, 0, true, reg::t2, kJumpAfterLoadCycles},
	{"ld ra then jr ra", Producer::Ld, 0, false, reg::ra, kJumpAfterLoadCycles},
	{"mfc0 then jr", Producer::Mfc0, 0, false, reg::t2, kJumpAfterLoadCycles},
	{"lw, one between, jr", Producer::Lw, 1, false, reg::t2, 0},
	{"ld ra, two between, jr ra", Producer::Ld, 2, false, reg::ra, 0},
	{"lw, one between, jalr", Producer::Lw, 1, true, reg::t2, 0},
	{"addiu then jr", Producer::Alu, 0, false, reg::t2, 0},
};

// Prepends SYNCs (no-ops in both engines) until the block's charge, plus
// `extra_eighths`, is a whole number of cycles, and adds the charge to *cycles.
std::vector<u32> Whole(std::vector<u32> words, u32 extra_eighths, u64* cycles)
{
	u32 raw = extra_eighths;
	for (u32 w : words)
		raw += Eighths(w);
	u32 syncs = 0;
	while ((raw + syncs * Eighths(ee::SYNC)) % 8 != 0)
		syncs++;
	words.insert(words.begin(), syncs, ee::SYNC);
	*cycles += (raw + syncs * Eighths(ee::SYNC)) / 8;
	return words;
}

// t9 keeps the harness's return address across a form that loads ra.
std::vector<u32> Program(const Form& f, u32* target, u64* charged)
{
	const u32 read0 = MFC0(reg::t0, kCp0Count);
	std::vector<u32> body = {
		OR(reg::t9, reg::ra, reg::zero),
		LUI(reg::a0, static_cast<u16>(kScratch >> 16)),
		ORI(reg::a0, reg::a0, static_cast<u16>(kScratch & 0xFFFF)),
	};
	switch (f.producer)
	{
		case Producer::Lw: body.push_back(LW(f.reg, 0, reg::a0)); break;
		case Producer::Ld: body.push_back(ee::LD(f.reg, 0, reg::a0)); break;
		case Producer::Lq: body.push_back(ee::LQ(f.reg, 0, reg::a0)); break;
		case Producer::Mfc0: body.push_back(MFC0(f.reg, kCp0Epc)); break;
		case Producer::Alu: body.push_back(ADDIU(f.reg, reg::t3, 0)); break;
	}
	for (u32 i = 0; i < f.between; i++)
		body.push_back(ADDIU(reg::t5, reg::t5, 1));
	body.push_back(f.jalr ? JALR(reg::t4, f.reg) : JR(f.reg));
	body.push_back(NOP);
	std::vector<u32> prog = Whole(body, Eighths(read0), charged);
	prog.insert(prog.begin(), read0);
	*target = kProgPc + 4 * static_cast<u32>(prog.size());
	const std::vector<u32> settle = Whole({BEQ(reg::zero, reg::zero, 1), NOP}, 0, charged);
	prog.insert(prog.end(), settle.begin(), settle.end());
	prog.push_back(MFC0(reg::t1, kCp0Count));
	prog.push_back(OR(reg::ra, reg::t9, reg::zero));
	*charged += f.extra;
	return prog;
}

void ExpectForm(const Form& f)
{
	SCOPED_TRACE(f.name);
	const ScopedClean clean;
	for (int pass = 0; pass < 2; pass++)
	{
		u64 charged = 0;
		u32 target = 0;
		const std::vector<u32> prog = Program(f, &target, &charged);
		EeRecTestHarness h;
		h.SetCp0(kCp0Status, kStatusCu0);
		h.SetCp0(kCp0Config, kConfigDie);
		h.SetCp0(kCp0Epc, target);
		h.WriteU32(kScratch, target);
		h.WriteU32(kScratch + 4, 0);
		h.SetGpr(reg::t3, target);
		h.LoadProgram(prog);
		h.Run();
		if (pass == 1)
		{
			EXPECT_EQ(h.GetGprJit(reg::t1) - h.GetGprJit(reg::t0), charged) << "jit";
			EXPECT_EQ(h.GetGprInterp(reg::t1) - h.GetGprInterp(reg::t0), charged) << "interp";
		}
	}
}

} // namespace

TEST(EeRecJumpLoadCharge, RegisterJumpRightAfterItsLoadPaysTheStall)
{
	for (const Form& f : kForms)
		if (f.extra)
			ExpectForm(f);
}

TEST(EeRecJumpLoadCharge, RegisterJumpWithItsRegisterReadyEarlierPaysTheTable)
{
	for (const Form& f : kForms)
		if (!f.extra)
			ExpectForm(f);
}

// The charge is decided from the jump and the two words before it in memory,
// the same way in both engines: the word before must write the jump's register
// as a GPR load or an MFC0, and must not sit in a branch's delay slot (a slot
// of a branch-likely that was not taken never ran).
TEST(EeRecJumpLoadCharge, ProducerIsTheWordBeforeAndNotADelaySlot)
{
	constexpr u32 kM = 1; // 2 - Config.DIE, DIE set
	const u32 lw = LW(reg::t2, 0, reg::a0);
	const u32 add = ADDIU(reg::t5, reg::t5, 1);
	const u32 e = static_cast<u32>(kJumpAfterLoadCycles * 8);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), lw, add, kM), e);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JALR(reg::ra, reg::t2), lw, add, kM), e);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), MFC0(reg::t2, kCp0Epc), add, kM), e);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), LBU(reg::t2, 0, reg::a0), add, kM), e);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), ee::LD(reg::t2, 0, reg::a0), add, kM), e);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), ee::LQ(reg::t2, 0, reg::a0), add, kM), e);
	// Dual issue off (multiplier 2) doubles every charge, this one with them.
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), lw, add, 2), e * 2);
	// The word before is in a delay slot.
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), lw, BNE(reg::zero, reg::zero, 2), kM), 0u);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), lw, JAL(0x1000), kM), 0u);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), lw, JR(reg::ra), kM), 0u);
	// Another register, or not a GPR load.
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), LW(reg::t3, 0, reg::a0), add, kM), 0u);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), IType(0x31, reg::a0, reg::t2, 0) /* LWC1 $f10 */, add, kM), 0u);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), MTC0(reg::t2, kCp0Epc), add, kM), 0u);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::t2), ADDIU(reg::t2, reg::t3, 0), add, kM), 0u);
	// Not a register jump.
	EXPECT_EQ(R5900::JumpAfterLoadEighths(BEQ(reg::t2, reg::zero, 1), lw, add, kM), 0u);
	EXPECT_EQ(R5900::JumpAfterLoadEighths(JR(reg::zero), LW(reg::zero, 0, reg::a0), add, kM), 0u);
}
