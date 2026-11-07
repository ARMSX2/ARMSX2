// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The post-mortem the recompilers log when they stop on an error of their own
// (RecErrorReport-arm64.h): its text from given facts, and how often it is
// logged. rec_error_exit_tests.cpp checks what the error paths put in it.

#include "arm64/RecErrorReport-arm64.h"

#include "Memory.h"
#include "R5900.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

using namespace RecErrorReport;

namespace {

std::vector<std::string> Lines(const std::string& text)
{
	std::vector<std::string> lines;
	std::istringstream in(text);
	for (std::string line; std::getline(in, line);)
		lines.push_back(line);
	return lines;
}

Facts EeUnalignedJalr()
{
	Facts f;
	f.cpu = Cpu::EE;
	f.kind = Kind::UnalignedJump;
	f.pc = 0x80464a53;
	f.jump = Jump{0x00123450, 2, 0x80464a53}; // jalr $ra, $v0
	f.gpr[2] = 1;                            // the delay slot wrote $v0
	f.gpr[31] = 0x00123458;
	f.hi = 0x11;
	f.lo = 0x22;
	f.status = 0x70030c13;
	f.cause = 0x8;
	f.epc = 0x100;
	f.error_epc = 0x200;
	f.cycle = 123456789;
	f.frame = 54826;
	f.code = {
		{0x0012344c, 0x24020001, true}, // addiu v0, zero, 1
		{0x00123450, 0x0040f809, true}, // jalr ra, v0
		{0x00123454, 0x00000000, true},
		{0x00123458, 0, false},
	};
	return f;
}

} // namespace

TEST(RecErrorReport, EeUnalignedJumpNamesTheJumpItsRegisterAndBothValues)
{
	const std::vector<std::string> lines = Lines(Format(EeUnalignedJalr()));
	const std::vector<std::string> expected = {
		"EE recompiler stopped: jump to unaligned address 0x80464a53",
		"  The jump at 0x00123450 (jalr ->v0) read v0 = 0x80464a53 before its delay slot; v0 now holds 0000000000000001",
		"  cycle 123456789, frame 54826",
		"  Status 0x70030c13  Cause 0x00000008  EPC 0x00000100  ErrorEPC 0x00000200",
		"  Registers:",
		"    zero 0000000000000000   at 0000000000000000   v0 0000000000000001   v1 0000000000000000",
		"      a0 0000000000000000   a1 0000000000000000   a2 0000000000000000   a3 0000000000000000",
		"      t0 0000000000000000   t1 0000000000000000   t2 0000000000000000   t3 0000000000000000",
		"      t4 0000000000000000   t5 0000000000000000   t6 0000000000000000   t7 0000000000000000",
		"      s0 0000000000000000   s1 0000000000000000   s2 0000000000000000   s3 0000000000000000",
		"      s4 0000000000000000   s5 0000000000000000   s6 0000000000000000   s7 0000000000000000",
		"      t8 0000000000000000   t9 0000000000000000   k0 0000000000000000   k1 0000000000000000",
		"      gp 0000000000000000   sp 0000000000000000   fp 0000000000000000   ra 0000000000123458",
		"     hi 0000000000000011   lo 0000000000000022",
		"  Code (> is the jump):",
		"    0012344c  24020001  addiu v0,zero, 0x1",
		"  > 00123450  0040f809  jalr ->v0",
		"    00123454  00000000  nop",
		"    00123458  ????????",
	};
	EXPECT_EQ(lines, expected);
}

TEST(RecErrorReport, EeUnmappedJumpSaysTheGuestRanOffTheEndOfRam)
{
	Facts f;
	f.cpu = Cpu::EE;
	f.kind = Kind::UnmappedPage;
	f.pc = 0x02000000;
	f.end_of_region = "RAM";
	f.zero_words_before = 300;
	f.gpr[29] = 0x01ff0000;
	f.gpr[31] = 0x00100020;
	f.stack = {
		{0x01ff0000, 1, true},
		{0x01ff0004, 2, true},
		{0x01ff0008, 3, true},
		{0x01ff000c, 4, true},
		{0x01ff0010, 0, false},
	};
	const std::vector<std::string> lines = Lines(Format(f));
	ASSERT_GE(lines.size(), 7u);
	EXPECT_EQ(lines[0], "EE recompiler stopped: jump to unmapped address 0x02000000");
	EXPECT_EQ(lines[1], "  0x02000000 is the end of RAM: the guest ran off the end of it.");
	EXPECT_EQ(lines[2], "  The 300 words before it are zero (NOPs), back to 0x01fffb50.");
	EXPECT_EQ(lines[3], "  ra 0000000000100020  sp 0000000001ff0000");
	EXPECT_EQ(lines[4], "  Stack at sp:");
	EXPECT_EQ(lines[5], "    01ff0000  00000001 00000002 00000003 00000004");
	EXPECT_EQ(lines[6], "    01ff0010  ????????");
	EXPECT_EQ(lines[7], "  cycle 0, frame 0");
	EXPECT_EQ(Format(f).find("Code"), std::string::npos);
}

TEST(RecErrorReport, UnmappedJumpWithNothingBeforeItSaysSo)
{
	Facts f;
	f.cpu = Cpu::EE;
	f.kind = Kind::UnmappedPage;
	f.pc = 0x1e800000;
	const std::vector<std::string> lines = Lines(Format(f));
	ASSERT_GE(lines.size(), 2u);
	EXPECT_EQ(lines[1], "  The word before it is not zero, or not in RAM or ROM.");
}

TEST(RecErrorReport, UnalignedPcOnEntrySaysTheJumpIsNotKnown)
{
	Facts f;
	f.cpu = Cpu::EE;
	f.kind = Kind::UnalignedJump;
	f.pc = 0x00010102;
	const std::vector<std::string> lines = Lines(Format(f));
	ASSERT_GE(lines.size(), 2u);
	EXPECT_EQ(lines[1], "  The pc was already unaligned when the recompiler was entered; the jump that set it is not known.");
}

TEST(RecErrorReport, IopRegistersAre32BitsAndThereIsNoErrorEpc)
{
	Facts f;
	f.cpu = Cpu::IOP;
	f.kind = Kind::UnalignedJump;
	f.pc = 0x050202d3;
	f.jump = Jump{0x00012340, 4, 0x050202d3}; // jr $a0
	f.gpr[4] = 0x050202d3;
	f.status = 0x401;
	f.code = {{0x00012340, 0x00800008, true}};
	const std::vector<std::string> lines = Lines(Format(f));
	ASSERT_GE(lines.size(), 7u);
	EXPECT_EQ(lines[0], "IOP recompiler stopped: jump to unaligned address 0x050202d3");
	EXPECT_EQ(lines[1], "  The jump at 0x00012340 (jr ->a0) read a0 = 0x050202d3 before its delay slot; a0 now holds 050202d3");
	EXPECT_EQ(lines[3], "  Status 0x00000401  Cause 0x00000000  EPC 0x00000000");
	EXPECT_EQ(lines[5], "    zero 00000000   at 00000000   v0 00000000   v1 00000000");
	EXPECT_EQ(lines[6], "      a0 050202d3   a1 00000000   a2 00000000   a3 00000000");
}

TEST(RecErrorReport, EretSaysWhichRegisterItReturnedToAndWhy)
{
	Facts f = EeUnalignedJalr();
	f.kind = Kind::UnalignedEret;
	f.jump.reset();
	f.eret = Eret{0x00123450, false};
	f.code[1].value = 0x42000018; // eret
	f.epc = f.pc;
	std::vector<std::string> lines = Lines(Format(f));
	ASSERT_GE(lines.size(), 17u);
	EXPECT_EQ(lines[0], "EE recompiler stopped: ERET to unaligned address 0x80464a53");
	EXPECT_EQ(lines[1], "  The ERET at 0x00123450 returned to EPC = 0x80464a53 (Status.ERL was clear)");
	EXPECT_EQ(lines[3], "  Status 0x70030c13  Cause 0x00000008  EPC 0x80464a53  ErrorEPC 0x00000200");
	EXPECT_EQ(lines[14], "  Code (> is the ERET):");
	EXPECT_EQ(lines[16], "  > 00123450  42000018  eret");

	f.eret->from_error_epc = true;
	lines = Lines(Format(f));
	EXPECT_EQ(lines[1], "  The ERET at 0x00123450 returned to ErrorEPC = 0x80464a53 (Status.ERL was set)");
}

// ERET with ERL set clears ERL and keeps EXL; without it, clears EXL.
TEST(RecErrorReport, EretSourceIsWorkedOutFromTheStateItLeft)
{
	constexpr u32 kExl = 1u << 1;
	// EXL still set: only the ErrorEPC path leaves it.
	EXPECT_TRUE(EretUsedErrorEpc(kExl, 0x101, 0x101, 0x101));
	// EXL clear: the register that holds the pc.
	EXPECT_FALSE(EretUsedErrorEpc(0, 0x101, 0x101, 0x200));
	EXPECT_TRUE(EretUsedErrorEpc(0, 0x101, 0x200, 0x101));
	// Both hold it: EPC, the value is the same either way.
	EXPECT_FALSE(EretUsedErrorEpc(0, 0x101, 0x101, 0x101));
}

// A Resume repeats the error at the same pc: logged the first time only. And
// at most kMaxLogged errors are logged at all.
TEST(RecErrorReport, EachErrorIsLoggedOnceAndOnlyAFewInAll)
{
	ResetForTests();
	const u32 saved_pc = cpuRegs.pc;

	cpuRegs.pc = 0x1e800000;
	LogEe(Kind::UnmappedPage, std::nullopt);
	EXPECT_FALSE(TakeLoggedForTests().empty());
	LogEe(Kind::UnmappedPage, std::nullopt);
	EXPECT_TRUE(TakeLoggedForTests().empty()) << "the same error twice";

	// A Resume after an unaligned jump or ERET meets the pc through the check
	// on entry, which reports it as an unaligned pc of unknown origin: the
	// same error, not logged again.
	LogEe(Kind::UnalignedJump, std::nullopt);
	EXPECT_TRUE(TakeLoggedForTests().empty()) << "the same pc reached another way";

	for (u32 i = 1; i < kMaxLogged; i++)
	{
		cpuRegs.pc = 0x1e800000 + i * 0x10000;
		LogEe(Kind::UnmappedPage, std::nullopt);
		EXPECT_FALSE(TakeLoggedForTests().empty()) << i;
	}
	cpuRegs.pc = 0x1f000000;
	LogEe(Kind::UnmappedPage, std::nullopt);
	EXPECT_TRUE(TakeLoggedForTests().empty()) << "past the cap";

	cpuRegs.pc = saved_pc;
	ResetForTests();
}

// The word readers the crash post-mortem uses: RAM through its segments and
// mirrors, ROM, and nothing for an address with no memory behind it.
TEST(RecErrorReport, WordReadersSeeRamRomAndNothingElse)
{
	ASSERT_NE(eeMem, nullptr);
	ASSERT_NE(iopMem, nullptr);
	const u32 ee_saved = *reinterpret_cast<u32*>(eeMem->Main + 0x1230);
	const u32 iop_saved = *reinterpret_cast<u32*>(iopMem->Main + 0x4560);
	*reinterpret_cast<u32*>(eeMem->Main + 0x1230) = 0x11223344;
	*reinterpret_cast<u32*>(iopMem->Main + 0x4560) = 0x55667788;

	u32 value = 0;
	EXPECT_TRUE(ReadEeWord(0x00001230, &value));
	EXPECT_EQ(value, 0x11223344u);
	value = 0;
	EXPECT_TRUE(ReadEeWord(0x80001232, &value)) << "kseg0, and the low bits are ignored";
	EXPECT_EQ(value, 0x11223344u);
	EXPECT_TRUE(ReadEeWord(0xbfc00000, &value)) << "the BIOS ROM";
	EXPECT_FALSE(ReadEeWord(0x1e800000, &value));

	value = 0;
	EXPECT_TRUE(ReadIopWord(0x80004560, &value));
	EXPECT_EQ(value, 0x55667788u);
	value = 0;
	EXPECT_TRUE(ReadIopWord(0x00204560, &value)) << "IOP RAM mirrors through the first 8 MB";
	EXPECT_EQ(value, 0x55667788u);
	EXPECT_FALSE(ReadIopWord(0x01000000, &value));

	*reinterpret_cast<u32*>(eeMem->Main + 0x1230) = ee_saved;
	*reinterpret_cast<u32*>(iopMem->Main + 0x4560) = iop_saved;
}
