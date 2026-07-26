// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// IOP loads, stores, branches and the three delay-slot captures, scored
// against a real-hardware oracle rather than against the other engine.
//
// The `.expected` files come from unknownbrackets/ps2autotests, captured on a
// real PS2. lsu.expected and branch.expected are tabular and are generated
// into autocases_iopmisc.h;
// branchdelay, hilodelay and lsudelay are nine, four and nine lines of
// free-form English and are transcribed below with the capture line quoted
// above each case.
//
// Three things about the capture harness that the numbers do not explain
// themselves:
//
//   * `ld` and `sd` are not R3000A instructions. The IOP is 32-bit MIPS-I.
//     GAS assembles them as macros into a pair of lw/sw on the register PAIR
//     (rt, rt+1), and this file re-expands them the same way. `ld +0`
//     printing 0x88888888 is 0x23456789 ^ 0xABCDEF01, not a load result.
//   * lsu.c loads into $t6 and prints $t6 ^ $t7, so its two register presets
//     are dead for every load. lwl/lwr are the exception: they merge into
//     $t6's leftover value and therefore chain, which is why each lwl/lwr
//     case is seeded with the previous case's printed value.
//   * branch.c reads its named constants with `*(u32 *)p` -- the first word
//     only. C_S64_MAX is therefore NEGATIVE on the IOP and C_S64_MIN is
//     ZERO, the opposite of what the same names mean in the EE captures.

#include <gtest/gtest.h>

#include "harness/JitTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"

#include <string>
#include <vector>

#include "autocases_iopmisc.h"

using namespace ps2auto_iopmisc;
using recompiler_tests::JitTestHarness;
using recompiler_tests::RecompilerTestEnvironment;

namespace
{
using namespace mips;

constexpr u32 kProgramPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;

// C_PATTERN in place, with the base register at C_PATTERN[1] (word 4).
constexpr u32 kPatternAddr = kScratch;
constexpr u32 kLoadBase = kPatternAddr + 16;

// The 6-word store buffer, based at buffer[1] like the capture.
constexpr u32 kStoreBuf = kScratch + 0x100;
constexpr u32 kStoreBase = kStoreBuf + 4;

// A single word of 0x13371337 for the load-delay capture.
constexpr u32 kDelayData = kScratch + 0x200;

constexpr u32 kBase = reg::t0;   // address operand
constexpr u32 kRes = reg::t1;    // the value the capture printed
constexpr u32 kRes2 = reg::t2;   // second printed value (ld delay cases)
constexpr u32 kAux = reg::t3;

inline u32 Move(u32 rd, u32 rs) { return OR(rd, rs, reg::zero); }
inline u32 B(s16 off) { return BEQ(reg::zero, reg::zero, off); }
inline u32 Word(int index) { return kProgramPc + 4u * static_cast<u32>(index); }

// `la rd, addr` -- the two-instruction GAS macro the capture uses.
void EmitLa(std::vector<u32>& p, u32 rd, u32 addr)
{
	p.push_back(LUI(rd, static_cast<u16>((addr + 0x8000) >> 16)));
	p.push_back(ADDIU(rd, rd, static_cast<s16>(addr & 0xFFFF)));
}

// One load, with `ld` expanded to the lw pair GAS emits for it.
bool EmitLoad(std::vector<u32>& p, const std::string& op, s16 off, u32 rt)
{
	if (op == "lb") { p.push_back(LB(rt, off, kBase)); return true; }
	if (op == "lbu") { p.push_back(LBU(rt, off, kBase)); return true; }
	if (op == "lh") { p.push_back(LH(rt, off, kBase)); return true; }
	if (op == "lhu") { p.push_back(LHU(rt, off, kBase)); return true; }
	if (op == "lw") { p.push_back(LW(rt, off, kBase)); return true; }
	if (op == "lwl") { p.push_back(LWL(rt, off, kBase)); return true; }
	if (op == "lwr") { p.push_back(LWR(rt, off, kBase)); return true; }
	if (op == "ld")
	{
		p.push_back(LW(rt, off, kBase));
		p.push_back(LW(rt + 1, static_cast<s16>(off + 4), kBase));
		return true;
	}
	return false;
}

// LOAD_OP: zero $t7, run the op into $t6, then print $t6 ^ $t7.
bool EmitLoadOp(std::vector<u32>& p, const std::string& op, s16 off)
{
	p.push_back(Move(reg::t7, reg::zero));
	if (!EmitLoad(p, op, off, reg::t6))
		return false;
	p.push_back(NOP);
	p.push_back(Move(kRes, reg::t6));
	p.push_back(XOR(kRes, kRes, reg::t7));
	return true;
}

// STORE_OP: reload $t6 with the value, zero $t7, run the op.
bool EmitStoreOp(std::vector<u32>& p, const std::string& op, s16 off)
{
	p.push_back(Move(reg::t6, kAux));
	p.push_back(Move(reg::t7, reg::zero));
	const u32 rt = reg::t6;
	if (op == "sb") p.push_back(SB(rt, off, kBase));
	else if (op == "sh") p.push_back(SH(rt, off, kBase));
	else if (op == "sw") p.push_back(SW(rt, off, kBase));
	else if (op == "swl") p.push_back(SWL(rt, off, kBase));
	else if (op == "swr") p.push_back(SWR(rt, off, kBase));
	else if (op == "sd")
	{
		// GAS macro: sw rt, off(base); sw rt+1, off+4(base), and rt+1 is
		// $t7, which the wrapper just zeroed. That is why every `sd` case
		// prints a zero second word.
		p.push_back(SW(rt, off, kBase));
		p.push_back(SW(rt + 1, static_cast<s16>(off + 4), kBase));
	}
	else
		return false;
	p.push_back(NOP);
	return true;
}

void SeedPattern(JitTestHarness& h)
{
	h.WriteBytes(kPatternAddr, kPattern, sizeof(kPattern));
}

void SeedStoreBuffer(JitTestHarness& h)
{
	// memcpy(buffer, C_PATTERN, sizeof(buffer)) -- 6 words, not 12.
	h.WriteBytes(kStoreBuf, kPattern, 6 * sizeof(u32));
}

bool IsLwlr(const char* op)
{
	const std::string s = op;
	return s == "lwl" || s == "lwr";
}
} // namespace

// ---------------------------------------------------------------------------
// lsu.expected -- loads
// ---------------------------------------------------------------------------

// 54 cases over 8 mnemonics. The `-> $0` rows load into $0 and read it back
// through `ori res, $0, 0`, which is the same shape that caught the EE
// interpreter writing $0 from LD; the IOP interpreter guards all six of its
// simple loads and both of LWL/LWR (R3000AOpcodeTables.cpp), and the capture
// agrees.
TEST(IopLsuBranchConsoleConformance, LoadsMatchConsole)
{
	int checked = 0;
	// $t6 as the lwl/lwr chain begins. Every later lwl/lwr case starts from
	// the previous case's printed value, because the printed value IS $t6.
	u32 t6_carry = kLwlrSeed;

	for (int i = 0; i < kIopLoadCaseCount; ++i)
	{
		const IopLoadCase& c = kIopLoadCases[i];
		const bool chained = IsLwlr(c.op1) && !c.into_zero;

		SCOPED_TRACE(::testing::Message()
		             << c.label << " -- ps2autotests hardware capture");

		std::vector<u32> prog;
		if (c.into_zero)
		{
			ASSERT_TRUE(EmitLoad(prog, c.op1, 0, reg::zero)) << c.op1;
			prog.push_back(NOP);
			prog.push_back(ORI(kRes, reg::zero, 0));
		}
		else
		{
			ASSERT_TRUE(EmitLoadOp(prog, c.op1, static_cast<s16>(c.off1)))
			    << c.op1;
			if (c.op2 != nullptr)
			{
				ASSERT_TRUE(
				    EmitLoadOp(prog, c.op2, static_cast<s16>(c.off2)))
				    << c.op2;
			}
		}
		prog.push_back(JR(reg::ra));
		prog.push_back(NOP);

		JitTestHarness h;
		SeedPattern(h);
		h.SetGpr(kBase, kLoadBase);
		h.SetGpr(reg::t6, chained ? t6_carry : 0x1337u);
		h.SetGpr(kRes, 0xBAADF00Du);
		h.LoadProgramAt(kProgramPc, prog.data(), prog.size());
		h.Run(); // also gtest-diffs JIT against interpreter

		EXPECT_EQ(h.GetGprJit(kRes), c.expect) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), c.expect) << "interp";
		if (chained)
			t6_carry = c.expect;
		++checked;
	}
	EXPECT_EQ(checked, kIopLoadCaseCount);
	EXPECT_GT(checked, 50);
}

// ---------------------------------------------------------------------------
// lsu.expected -- stores
// ---------------------------------------------------------------------------

TEST(IopLsuBranchConsoleConformance, StoresMatchConsole)
{
	int checked = 0;
	for (int i = 0; i < kIopStoreCaseCount; ++i)
	{
		const IopStoreCase& c = kIopStoreCases[i];
		SCOPED_TRACE(::testing::Message()
		             << c.label << " -- ps2autotests hardware capture");

		std::vector<u32> prog;
		ASSERT_TRUE(EmitStoreOp(prog, c.op1, static_cast<s16>(c.off1)))
		    << c.op1;
		if (c.op2 != nullptr)
		{
			ASSERT_TRUE(EmitStoreOp(prog, c.op2, static_cast<s16>(c.off2)))
			    << c.op2;
		}
		prog.push_back(JR(reg::ra));
		prog.push_back(NOP);

		JitTestHarness h;
		SeedStoreBuffer(h);
		h.SetGpr(kBase, kStoreBase);
		h.SetGpr(kAux, c.imm_preset ? kImmPreset : kMemPreset);
		h.LoadProgramAt(kProgramPc, prog.data(), prog.size());
		h.Run();

		const u32 at = kStoreBase + static_cast<u32>(c.block);
		EXPECT_EQ(h.ReadU32(at), c.mem[0]) << "word 0";
		EXPECT_EQ(h.ReadU32(at + 4), c.mem[1]) << "word 1";
		++checked;
	}
	EXPECT_EQ(checked, kIopStoreCaseCount);
	EXPECT_GT(checked, 30);
}

// The `sd` block is the whole reason to say out loud that the IOP has no
// 64-bit store: every one of its second words is zero, because the pair's
// upper half is $t7 and the wrapper zeroes $t7 immediately before the store.
// A real SD would have written the upper half of a 64-bit value there.
TEST(IopLsuBranchConsoleConformance, SdIsAssembledAsTwoStores)
{
	int sd_cases = 0;
	for (int i = 0; i < kIopStoreCaseCount; ++i)
	{
		const IopStoreCase& c = kIopStoreCases[i];
		if (std::string(c.op1) != "sd")
			continue;
		EXPECT_EQ(c.mem[1], 0u) << c.label;
		++sd_cases;
	}
	EXPECT_EQ(sd_cases, 6);
}

// ---------------------------------------------------------------------------
// branch.expected
// ---------------------------------------------------------------------------

// 144 cases over all twelve R3000A branch and jump forms. Two rules this
// pins that a differential test cannot:
//
//   * BGEZAL / BLTZAL link UNCONDITIONALLY -- every one of their 34 rows
//     says "set ra", including the seventeen that say "skipped".
//   * The delay slot always runs. All 144 rows say "ran delay slot"; the
//     R3000A has no likely-branch form to skip it.
TEST(IopLsuBranchConsoleConformance, BranchesMatchConsole)
{
	int checked = 0;
	for (int i = 0; i < kIopBranchCaseCount; ++i)
	{
		const IopBranchCase& c = kIopBranchCases[i];
		SCOPED_TRACE(::testing::Message()
		             << c.label << " -- ps2autotests hardware capture");
		const std::string op = c.op;

		// The BRRO / BRO / BO / BRR / BR wrappers from branch.c, laid out
		// word by word. `res` accumulates 1 = followed, 2 = ra changed,
		// 4 = the delay slot ran.
		std::vector<u32> prog;
		if (c.form == BF_RSRT || c.form == BF_RS || c.form == BF_NONE)
		{
			//  0 lui res, 0            6 branch: ori res, res, 1
			//  1 or t9, $0, ra         7 skip:   beq t9, ra, done
			//  2 <branch>              8 nop
			//  3 ori res, res, 4       9 or ra, $0, t9
			//  4 b skip               10 ori res, res, 2
			//  5 nop                  11 done:   jr ra
			//                         12 nop
			prog.push_back(LUI(kRes, 0));
			prog.push_back(Move(reg::t9, reg::ra));
			if (op == "beq") prog.push_back(BEQ(kBase, kRes2, 3));
			else if (op == "bne") prog.push_back(BNE(kBase, kRes2, 3));
			else if (op == "bgez") prog.push_back(BGEZ(kBase, 3));
			else if (op == "bgezal") prog.push_back(BGEZAL(kBase, 3));
			else if (op == "bgtz") prog.push_back(BGTZ(kBase, 3));
			else if (op == "blez") prog.push_back(BLEZ(kBase, 3));
			else if (op == "bltz") prog.push_back(BLTZ(kBase, 3));
			else if (op == "bltzal") prog.push_back(BLTZAL(kBase, 3));
			else if (op == "j") prog.push_back(J(Word(6)));
			else if (op == "jal") prog.push_back(JAL(Word(6)));
			else FAIL() << "no encoder for " << op;
			prog.push_back(ORI(kRes, kRes, 4));
			prog.push_back(B(2));
			prog.push_back(NOP);
			prog.push_back(ORI(kRes, kRes, 1));
			prog.push_back(BEQ(reg::t9, reg::ra, 3));
			prog.push_back(NOP);
			prog.push_back(Move(reg::ra, reg::t9));
			prog.push_back(ORI(kRes, kRes, 2));
			prog.push_back(JR(reg::ra));
			prog.push_back(NOP);
		}
		else if (c.form == BF_JR)
		{
			//  0 lui res, 0            8 branch: ori res, res, 1
			//  1 la t8, branch         9 skip:   beq t9, ra, done
			//  3 or t9, $0, ra        10 nop
			//  4 jr t8                11 or ra, $0, t9
			//  5 ori res, res, 4      12 ori res, res, 2
			//  6 b skip               13 done:   jr ra
			//  7 nop                  14 nop
			prog.push_back(LUI(kRes, 0));
			EmitLa(prog, reg::t8, Word(8));
			prog.push_back(Move(reg::t9, reg::ra));
			prog.push_back(JR(reg::t8));
			prog.push_back(ORI(kRes, kRes, 4));
			prog.push_back(B(2));
			prog.push_back(NOP);
			prog.push_back(ORI(kRes, kRes, 1));
			prog.push_back(BEQ(reg::t9, reg::ra, 3));
			prog.push_back(NOP);
			prog.push_back(Move(reg::ra, reg::t9));
			prog.push_back(ORI(kRes, kRes, 2));
			prog.push_back(JR(reg::ra));
			prog.push_back(NOP);
		}
		else // BF_JALR -- links into t9, so ra is never touched at all
		{
			//  0 lui res, 0            8 branch: ori res, res, 1
			//  1 la t8, branch         9 skip:   beq t9, $0, done
			//  3 or t9, $0, $0        10 nop
			//  4 jalr t9, t8          11 ori res, res, 2
			//  5 ori res, res, 4      12 done:   jr ra
			//  6 b skip               13 nop
			//  7 nop
			prog.push_back(LUI(kRes, 0));
			EmitLa(prog, reg::t8, Word(8));
			prog.push_back(Move(reg::t9, reg::zero));
			prog.push_back(JALR(reg::t9, reg::t8));
			prog.push_back(ORI(kRes, kRes, 4));
			prog.push_back(B(2));
			prog.push_back(NOP);
			prog.push_back(ORI(kRes, kRes, 1));
			prog.push_back(BEQ(reg::t9, reg::zero, 2));
			prog.push_back(NOP);
			prog.push_back(ORI(kRes, kRes, 2));
			prog.push_back(JR(reg::ra));
			prog.push_back(NOP);
		}

		JitTestHarness h;
		h.SetGpr(kBase, c.rs);
		h.SetGpr(kRes2, c.rt);
		h.SetGpr(kRes, 0xBAADF00Du);
		h.LoadProgramAt(kProgramPc, prog.data(), prog.size());
		h.Run();

		EXPECT_EQ(h.GetGprJit(kRes), c.flags) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), c.flags) << "interp";
		++checked;
	}
	EXPECT_EQ(checked, kIopBranchCaseCount);
	EXPECT_GT(checked, 140);
}

// ---------------------------------------------------------------------------
// branchdelay.expected -- transcribed by hand, capture line quoted
// ---------------------------------------------------------------------------

// Every one of these asks the same question in a different shape: when a
// linking branch and its delay slot both write a register, who wins? The
// console says the DELAY SLOT does, i.e. the link is written before the
// delay slot executes. The one case the capture refuses to pin -- a branch
// in a branch's delay slot -- prints only "did not crash" and is not
// reproduced here.
TEST(IopLsuBranchConsoleConformance, BranchDelaySlotOrderingMatchesConsole)
{
	// "jal: ra order: 00000002"
	//   jal target2 / li $ra, 2 -- target2 reads $ra and sees the delay
	//   slot's 2, not the link address.
	{
		SCOPED_TRACE("jal: ra order");
		JitTestHarness h;
		h.SetGpr(kRes, 0xFFFFFFFFu);
		h.LoadProgramNoTerm({
		    /* 0 */ Move(reg::t2, reg::ra),
		    /* 1 */ ORI(kBase, reg::zero, 0),
		    /* 2 */ JAL(Word(7)),
		    /* 3 */ ORI(reg::ra, reg::zero, 2), // delay slot
		    /* 4 */ ORI(kRes, reg::zero, 1),    // target1
		    /* 5 */ J(Word(8)),
		    /* 6 */ NOP,
		    /* 7 */ Move(kRes, reg::ra),        // target2
		    /* 8 */ Move(reg::ra, reg::t2),     // skip
		    /* 9 */ JR(reg::ra),
		    /* 10 */ NOP,
		});
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), 2u) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), 2u) << "interp";
	}

	// "jalr: ra order: 00000002" -- the same for the register form.
	{
		SCOPED_TRACE("jalr: ra order");
		JitTestHarness h;
		h.SetGpr(kRes, 0xFFFFFFFFu);
		std::vector<u32> p;
		p.push_back(Move(reg::t2, reg::ra));    // 0
		EmitLa(p, kBase, Word(8));              // 1,2
		p.push_back(JALR(reg::ra, kBase));      // 3
		p.push_back(ORI(reg::ra, reg::zero, 2));// 4  delay slot
		p.push_back(ORI(kRes, reg::zero, 1));   // 5  target1
		p.push_back(J(Word(9)));                // 6
		p.push_back(NOP);                       // 7
		p.push_back(Move(kRes, reg::ra));       // 8  target2
		p.push_back(Move(reg::ra, reg::t2));    // 9  skip
		p.push_back(JR(reg::ra));               // 10
		p.push_back(NOP);                       // 11
		h.LoadProgramAt(kProgramPc, p.data(), p.size());
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), 2u) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), 2u) << "interp";
	}

	// "jalr: non-ra: OK" -- the capture only range-checks the link value
	// (0x100 < rd <= 0x7ffffffc). We can be exact: rd is the address of the
	// jalr plus 8, i.e. the instruction after the delay slot.
	{
		SCOPED_TRACE("jalr: non-ra value");
		JitTestHarness h;
		h.SetGpr(kRes, 0xFFFFFFFFu);
		std::vector<u32> p;
		p.push_back(Move(reg::t2, reg::ra));    // 0
		EmitLa(p, kAux, Word(8));               // 1,2
		p.push_back(ORI(kRes, reg::zero, 0));   // 3
		p.push_back(JALR(kRes, kAux));          // 4
		p.push_back(NOP);                       // 5  delay slot
		p.push_back(J(Word(9)));                // 6  target1
		p.push_back(NOP);                       // 7
		p.push_back(NOP);                       // 8  target2
		p.push_back(Move(reg::ra, reg::t2));    // 9  skip
		p.push_back(JR(reg::ra));               // 10
		p.push_back(NOP);                       // 11
		h.LoadProgramAt(kProgramPc, p.data(), p.size());
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), Word(6)) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), Word(6)) << "interp";
		// and the capture's own weaker claim, unchanged
		EXPECT_GT(h.GetGprJit(kRes), 0x100u);
		EXPECT_LE(h.GetGprJit(kRes), 0x7FFFFFFCu);
	}

	// "jalr: non-ra order: 00000001" -- delay slot beats the link write even
	// when the link target is an ordinary register.
	{
		SCOPED_TRACE("jalr: non-ra order");
		JitTestHarness h;
		h.SetGpr(kRes, 0xFFFFFFFFu);
		std::vector<u32> p;
		p.push_back(Move(reg::t2, reg::ra));    // 0
		EmitLa(p, kAux, Word(7));               // 1,2
		p.push_back(JALR(kRes, kAux));          // 3
		p.push_back(ORI(kRes, reg::zero, 1));   // 4  delay slot
		p.push_back(J(Word(8)));                // 5  target1
		p.push_back(NOP);                       // 6
		p.push_back(NOP);                       // 7  target2
		p.push_back(Move(reg::ra, reg::t2));    // 8  skip
		p.push_back(JR(reg::ra));               // 9
		p.push_back(NOP);                       // 10
		h.LoadProgramAt(kProgramPc, p.data(), p.size());
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), 1u) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), 1u) << "interp";
	}

	// "jalr: rs/rd match: 00000002" -- `jalr $t0, $t0`. The link write to
	// rd must not disturb the jump, which reads the OLD rs. psxJALR used to
	// link first and then read Rs, which sent this to target1.
	{
		SCOPED_TRACE("jalr: rs/rd match");
		JitTestHarness h;
		h.SetGpr(kRes, 0xFFFFFFFFu);
		std::vector<u32> p;
		p.push_back(Move(reg::t2, reg::ra));    // 0
		EmitLa(p, kBase, Word(8));              // 1,2
		p.push_back(JALR(kBase, kBase));        // 3
		p.push_back(NOP);                       // 4  delay slot
		p.push_back(ORI(kRes, reg::zero, 1));   // 5  target1
		p.push_back(J(Word(9)));                // 6
		p.push_back(NOP);                       // 7
		p.push_back(ORI(kRes, reg::zero, 2));   // 8  target2
		p.push_back(Move(reg::ra, reg::t2));    // 9  skip
		p.push_back(JR(reg::ra));               // 10
		p.push_back(NOP);                       // 11
		h.LoadProgramAt(kProgramPc, p.data(), p.size());
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), 2u) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), 2u) << "interp";
	}

	// "jalr: clobber rs: 00000002" -- the delay slot overwrites rs. The
	// target was already latched, so the jump still lands on target2.
	{
		SCOPED_TRACE("jalr: clobber rs");
		JitTestHarness h;
		h.SetGpr(kRes, 0xFFFFFFFFu);
		std::vector<u32> p;
		p.push_back(Move(reg::t2, reg::ra));    // 0
		EmitLa(p, kBase, Word(9));              // 1,2
		p.push_back(JALR(reg::ra, kBase));      // 3
		p.push_back(ORI(kBase, reg::zero, 5));  // 4  delay slot clobbers rs
		p.push_back(NOP);                       // 5
		p.push_back(ORI(kRes, reg::zero, 1));   // 6  target1
		p.push_back(J(Word(10)));               // 7
		p.push_back(NOP);                       // 8
		p.push_back(ORI(kRes, reg::zero, 2));   // 9  target2
		p.push_back(Move(reg::ra, reg::t2));    // 10 skip
		p.push_back(JR(reg::ra));               // 11
		p.push_back(NOP);                       // 12
		h.LoadProgramAt(kProgramPc, p.data(), p.size());
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), 2u) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), 2u) << "interp";
	}

	// "bltzal: ra order: 00000002" and "bgezal: ra order: 00000002" -- the
	// same ordering rule for the two linking conditional branches.
	// (The capture builds its -1 with `subu $t0, $0, 1`, a GAS macro; the
	// value is what matters, so this uses addiu.)
	struct LinkBranch { const char* name; u32 rs; u32 word; };
	const LinkBranch kLinkBranches[] = {
	    {"bltzal: ra order", 0xFFFFFFFFu, BLTZAL(kBase, 4)},
	    {"bgezal: ra order", 0x00000000u, BGEZAL(kBase, 4)},
	};
	for (const LinkBranch& lb : kLinkBranches)
	{
		SCOPED_TRACE(lb.name);
		JitTestHarness h;
		h.SetGpr(kRes, 0xFFFFFFFFu);
		h.SetGpr(kBase, lb.rs);
		h.LoadProgramNoTerm({
		    /* 0 */ Move(reg::t2, reg::ra),
		    /* 1 */ NOP,                        // stands in for the setup
		    /* 2 */ lb.word,                    // -> target2 at word 7
		    /* 3 */ ORI(reg::ra, reg::zero, 2), // delay slot
		    /* 4 */ ORI(kRes, reg::zero, 1),    // target1
		    /* 5 */ J(Word(8)),
		    /* 6 */ NOP,
		    /* 7 */ Move(kRes, reg::ra),        // target2
		    /* 8 */ Move(reg::ra, reg::t2),     // skip
		    /* 9 */ JR(reg::ra),
		    /* 10 */ NOP,
		});
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), 2u) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), 2u) << "interp";
	}
}

// CHARACTERIZATION, NOT CONFORMANCE -- and the console has since disagreed.
// psxBGEZAL and psxBLTZAL have the same shape psxJALR used to have --
// `_SetLink(31)` and only then a test of Rs -- as do BGEZAL / BLTZAL /
// BGEZALL / BLTZALL in the EE interpreter (pcsx2/Interpreter.cpp). The
// ordering is only observable when Rs IS 31, which ps2autotests never
// captured on either CPU.
//
// What this pins is that both IOP engines agree with each other today: both
// link first and then judge the LINKED value, so `bltzal $ra` with a
// negative $ra falls through and `bgezal $ra` with a negative $ra branches.
// The EE half of the same question HAS been measured since -- silicon judges
// these on the PRE-link value
// (EeSaPerfConsoleConformance.LinkBranchesWithRaAsSourceMatchConsole), and
// `jalr rd,rd` already agreed across both CPUs. So the two negative-$ra rows
// below are very likely WRONG for the IOP too; whoever fixes
// psxBGEZAL/psxBLTZAL should expect this test to resist and should invert
// them rather than assume a regression. The IOP itself was never measured
// (that needs an IRX, not an EE ELF).
TEST(IopLsuBranchConsoleConformance, LinkBranchesWithRaAsSourceArePinned)
{
	struct Probe { const char* name; u32 word; u32 ra; u32 expect; };
	const Probe kProbes[] = {
	    // Rs == 31. Both engines judge the condition on the POST-link value,
	    // which is a small positive code address, so the answers invert.
	    {"bltzal $ra (ra negative)", BLTZAL(reg::ra, 4), 0xFFFFFFFFu, 1u},
	    {"bgezal $ra (ra negative)", BGEZAL(reg::ra, 4), 0xFFFFFFFFu, 2u},
	    // Controls: ra >= 0 already agrees with the post-link value, so
	    // these two are ordering-independent and must hold either way.
	    {"bltzal $ra (ra zero)", BLTZAL(reg::ra, 4), 0x00000000u, 1u},
	    {"bgezal $ra (ra zero)", BGEZAL(reg::ra, 4), 0x00000000u, 2u},
	};
	for (const Probe& p : kProbes)
	{
		SCOPED_TRACE(p.name);
		JitTestHarness h;
		h.SetGpr(kBase, p.ra);
		h.SetGpr(kRes, 0xBAADF00Du);
		h.LoadProgramNoTerm({
		    /* 0 */ Move(reg::t2, reg::ra),  // stash the harness return
		    /* 1 */ Move(reg::ra, kBase),    // ra = the value under test
		    /* 2 */ p.word,                  // rs == rd == 31
		    /* 3 */ NOP,                     // delay slot
		    /* 4 */ ORI(kRes, reg::zero, 1), // not taken
		    /* 5 */ J(Word(8)),
		    /* 6 */ NOP,
		    /* 7 */ ORI(kRes, reg::zero, 2), // taken
		    /* 8 */ Move(reg::ra, reg::t2),
		    /* 9 */ JR(reg::ra),
		    /* 10 */ NOP,
		});
		h.Run(); // JIT-vs-interp diff: the two must at least agree
		EXPECT_EQ(h.GetGprJit(kRes), p.expect) << "jit";
		EXPECT_EQ(h.GetGprInterp(kRes), p.expect) << "interp";
	}
}

// ---------------------------------------------------------------------------
// hilodelay.expected -- transcribed by hand, capture line quoted
// ---------------------------------------------------------------------------

// HI and LO are preset to 0x10 / 0x20 before each case so a missing write is
// visible, and the destination register is preset so a missing read is too.
// Console says every one of these reads back the finished value at the very
// next instruction: mfhi/mflo have no visible result delay, and mult/div
// interlock rather than exposing a partial HI/LO.
TEST(IopLsuBranchConsoleConformance, HiLoDelayMatchesConsole)
{
	// mthi 0x10 / mtlo 0x20, then read each back one instruction later.
	const auto prologue = [](std::vector<u32>& p) {
		p.push_back(ORI(reg::t8, reg::zero, 0x10));
		p.push_back(MTHI(reg::t8));
		p.push_back(ORI(reg::t8, reg::zero, 0x20));
		p.push_back(MTLO(reg::t8));
		p.push_back(NOP);
	};

	// "mfhi delay: 10" / "mflo delay: 20"
	{
		SCOPED_TRACE("mfhi/mflo delay");
		JitTestHarness h;
		std::vector<u32> p;
		prologue(p);
		p.push_back(ORI(reg::t8, reg::zero, 0x01));
		p.push_back(MFHI(reg::t8));
		p.push_back(Move(kRes, reg::t8));
		p.push_back(ORI(reg::t8, reg::zero, 0x02));
		p.push_back(MFLO(reg::t8));
		p.push_back(Move(kRes2, reg::t8));
		p.push_back(JR(reg::ra));
		p.push_back(NOP);
		h.SetGpr(kRes, 0x1337);
		h.SetGpr(kRes2, 0x1337);
		h.LoadProgramAt(kProgramPc, p.data(), p.size());
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), 0x10u) << "mfhi jit";
		EXPECT_EQ(h.GetGprInterp(kRes), 0x10u) << "mfhi interp";
		EXPECT_EQ(h.GetGprJit(kRes2), 0x20u) << "mflo jit";
		EXPECT_EQ(h.GetGprInterp(kRes2), 0x20u) << "mflo interp";
	}

	// "div delay: 1 1" -- 3 / 2 is quotient 1 remainder 1, and both are
	// readable by the instruction immediately after the div.
	// (The capture emits the div as a .word to dodge GAS's divide-by-zero
	// check macro; we encode DIV directly, which is the same instruction.)
	// "mult delay: 0 6" -- 3 * 2 = 6, HI zero.
	struct MulDiv { const char* name; u32 word; u32 hi; u32 lo; };
	const MulDiv kCases[] = {
	    {"div delay", DIV(reg::t8, reg::t9), 1u, 1u},
	    {"mult delay", MULT(reg::t8, reg::t9), 0u, 6u},
	};
	for (const MulDiv& c : kCases)
	{
		SCOPED_TRACE(c.name);
		JitTestHarness h;
		std::vector<u32> p;
		prologue(p);
		p.push_back(ORI(reg::t8, reg::zero, 0x03));
		p.push_back(ORI(reg::t9, reg::zero, 0x02));
		p.push_back(c.word);
		p.push_back(MFHI(kRes));
		p.push_back(c.word);
		p.push_back(MFLO(kRes2));
		p.push_back(JR(reg::ra));
		p.push_back(NOP);
		h.SetGpr(kRes, 0x1337);
		h.SetGpr(kRes2, 0x1337);
		h.LoadProgramAt(kProgramPc, p.data(), p.size());
		h.Run();
		EXPECT_EQ(h.GetGprJit(kRes), c.hi) << "hi jit";
		EXPECT_EQ(h.GetGprInterp(kRes), c.hi) << "hi interp";
		EXPECT_EQ(h.GetGprJit(kRes2), c.lo) << "lo jit";
		EXPECT_EQ(h.GetGprInterp(kRes2), c.lo) << "lo interp";
	}
}

// ---------------------------------------------------------------------------
// lsudelay.expected -- transcribed by hand, capture line quoted
// ---------------------------------------------------------------------------
//
// The R3000A load delay slot: the instruction immediately after a load still
// sees the register's OLD value, and if it WRITES that register its write
// wins over the arriving load result. PCSX2's IOP -- both engines -- retires
// loads immediately, so the cases that can tell the difference are recorded
// here as divergences rather than asserted away.

namespace
{
constexpr u32 kDelayPreset = 0x11223344u;
constexpr u32 kDelayValue = 0x13371337u;

struct LsuDelayCase
{
	const char* label;
	u32 expect;     // what the console printed
	u32 expect2;    // second printed word, for the `ld` cases
	bool two;       // does this case print two words
	bool bad_interp;
	bool bad_jit;
};

// bad_interp / bad_jit are recorded from a real run of this file, per engine,
// never derived from a rule.
constexpr LsuDelayCase kLsuDelayCases[] = {
    // "lb 0: 00000000" / "lw 0: 00000000"
    //   The delay-slot instruction writes the load's own destination. The
    //   load result never lands. Both engines agree by accident: they retire
    //   the load first and then the delay slot overwrites it anyway.
    {"lb 0", 0x00000000u, 0, false, false, false},
    {"lw 0", 0x00000000u, 0, false, false, false},
    // "lb 1: 11223344" / "lw 1: 11223344"
    //   The delay-slot instruction READS the destination and gets the old
    //   value. This is the load delay slot proper.
    {"lb 1", kDelayPreset, 0, false, true, true},
    {"lw 1", kDelayPreset, 0, false, true, true},
    // "lb 2: 00000037" / "lw 2: 13371337"
    //   One nop later the load has landed.
    {"lb 2", 0x00000037u, 0, false, false, false},
    {"lw 2", kDelayValue, 0, false, false, false},
    // "ld 1: 13371337 00000000" -- `ld` is the lw pair again, so the
    //   delay-slot `ori $t1, $0, 0` lands on the SECOND lw's destination.
    {"ld 1", kDelayValue, 0x00000000u, true, false, false},
    // "ld 2: 13371337 13371337"
    {"ld 2", kDelayValue, kDelayValue, true, false, false},
    // "lw then branch: 2" -- the branch in the load delay slot compares the
    //   OLD $t0 against a copy of it, so on console the branch is TAKEN.
    {"lw then branch", 2u, 0, false, true, true},
};
constexpr int kLsuDelayCaseCount =
    static_cast<int>(sizeof(kLsuDelayCases) / sizeof(kLsuDelayCases[0]));

// Build the program for one lsudelay case. Returns false for an unknown
// label so a typo cannot silently pass.
bool BuildLsuDelay(const std::string& label, std::vector<u32>& p)
{
	// lui/ori $t0 = 0x11223344, the value the delay slot may still see.
	p.push_back(LUI(kAux, static_cast<u16>(kDelayPreset >> 16)));
	p.push_back(ORI(kAux, kAux, static_cast<u16>(kDelayPreset & 0xFFFF)));

	const bool is_lb = label.rfind("lb ", 0) == 0;
	if (label == "lb 0" || label == "lw 0")
	{
		p.push_back(is_lb ? LB(kAux, 0, kBase) : LW(kAux, 0, kBase));
		p.push_back(ORI(kAux, reg::zero, 0)); // delay slot
		p.push_back(Move(kRes, kAux));
		p.push_back(NOP);
	}
	else if (label == "lb 1" || label == "lw 1")
	{
		p.push_back(is_lb ? LB(kAux, 0, kBase) : LW(kAux, 0, kBase));
		p.push_back(Move(kRes, kAux)); // delay slot reads the old value
		p.push_back(NOP);
	}
	else if (label == "lb 2" || label == "lw 2")
	{
		p.push_back(is_lb ? LB(kAux, 0, kBase) : LW(kAux, 0, kBase));
		p.push_back(NOP);
		p.push_back(Move(kRes, kAux));
		p.push_back(NOP);
	}
	else if (label == "ld 1" || label == "ld 2")
	{
		p.push_back(ORI(reg::t4, kAux, 0)); // ori $t1, $t0, 0
		p.push_back(LW(kAux, 0, kBase));    // ld $t0 -> the lw pair
		p.push_back(LW(reg::t4, 4, kBase));
		if (label == "ld 1")
			p.push_back(ORI(reg::t4, reg::zero, 0)); // delay slot
		else
			p.push_back(NOP);
		p.push_back(Move(kRes, kAux));
		p.push_back(Move(kRes2, reg::t4));
		p.push_back(NOP);
	}
	else if (label == "lw then branch")
	{
		//  0,1 lui/ori t3 = 0x11223344   (already emitted)
		//  2 ori t4, t3, 0
		//  3 lw t3, 0(base)
		//  4 beq t3, t4, equal -> word 9
		//  5 nop
		//  6 ori res, $0, 1
		//  7 b done -> word 10
		//  8 nop
		//  9 ori res, $0, 2   (equal)
		// 10 jr ra            (done, appended by the caller)
		// 11 nop
		p.push_back(ORI(reg::t4, kAux, 0));
		p.push_back(LW(kAux, 0, kBase));
		p.push_back(BEQ(kAux, reg::t4, 4));
		p.push_back(NOP);
		p.push_back(ORI(kRes, reg::zero, 1));
		p.push_back(B(2));
		p.push_back(NOP);
		p.push_back(ORI(kRes, reg::zero, 2));
		return true;
	}
	else
	{
		return false;
	}
	return true;
}

int RunLsuDelay(const LsuDelayCase& c, u32& got, u32& got2, bool jit)
{
	std::vector<u32> p;
	if (!BuildLsuDelay(c.label, p))
		return -1;
	p.push_back(JR(reg::ra));
	p.push_back(NOP);

	JitTestHarness h(JitTestHarness::Mode::DiffJitVsInterp);
	h.WriteU32(kDelayData, kDelayValue);
	h.WriteU32(kDelayData + 4, kDelayValue);
	h.SetGpr(kBase, kDelayData);
	h.SetGpr(kRes, 0);
	h.SetGpr(kRes2, 0);
	h.LoadProgramAt(kProgramPc, p.data(), p.size());
	h.Run();
	got = jit ? h.GetGprJit(kRes) : h.GetGprInterp(kRes);
	got2 = jit ? h.GetGprJit(kRes2) : h.GetGprInterp(kRes2);
	return 0;
}
} // namespace

// Always-on: the clean cases must match silicon, and the recorded
// divergences must still diverge. If one starts matching, this fails and
// tells you to move it -- and to run DISABLED_AllLsuDelayMatchesConsole.
TEST(IopLsuBranchConsoleConformance, LsuDelaySlotMatchesConsole)
{
	int diverged = 0;
	for (int i = 0; i < kLsuDelayCaseCount; ++i)
	{
		const LsuDelayCase& c = kLsuDelayCases[i];
		for (int engine = 0; engine < 2; ++engine)
		{
			const bool jit = engine == 1;
			const bool known_bad = jit ? c.bad_jit : c.bad_interp;
			SCOPED_TRACE(::testing::Message()
			             << c.label << (jit ? " [jit]" : " [interp]")
			             << " -- ps2autotests hardware capture");
			u32 got = 0, got2 = 0;
			ASSERT_EQ(RunLsuDelay(c, got, got2, jit), 0)
			    << "no program for " << c.label;

			const bool matches =
			    got == c.expect && (!c.two || got2 == c.expect2);
			if (known_bad)
			{
				EXPECT_FALSE(matches)
				    << c.label << " now MATCHES silicon -- PCSX2 grew a "
				    << "load delay slot? Clear its bad_ flag and re-run "
				    << "DISABLED_AllLsuDelayMatchesConsole.";
				++diverged;
			}
			else
			{
				EXPECT_EQ(got, c.expect);
				if (c.two)
					EXPECT_EQ(got2, c.expect2);
			}
		}
	}
	// Three cases, both engines: PCSX2 does not model the IOP load delay.
	EXPECT_EQ(diverged, 6);
}

// Tripwire. Enable to see every lsudelay case scored against silicon with no
// known-bad list at all:
//   ./recompiler_tests --gtest_also_run_disabled_tests \
//       --gtest_filter='*AllLsuDelayMatchesConsole*'
TEST(IopLsuBranchConsoleConformance, DISABLED_AllLsuDelayMatchesConsole)
{
	for (int i = 0; i < kLsuDelayCaseCount; ++i)
	{
		const LsuDelayCase& c = kLsuDelayCases[i];
		for (int engine = 0; engine < 2; ++engine)
		{
			const bool jit = engine == 1;
			SCOPED_TRACE(::testing::Message()
			             << c.label << (jit ? " [jit]" : " [interp]"));
			u32 got = 0, got2 = 0;
			ASSERT_EQ(RunLsuDelay(c, got, got2, jit), 0);
			EXPECT_EQ(got, c.expect);
			if (c.two)
				EXPECT_EQ(got2, c.expect2);
		}
	}
}
