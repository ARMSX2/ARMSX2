// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What happens when the process takes a fault nothing handles: the page fault
// handler gives up on it, and the process must die by that signal, not by an
// abort() of its own, so Android's tombstone and a Linux core dump show the
// fault itself.
//
// And what it writes to the log first (RecCrashReport-arm64.h): where in the
// code area the pc is, the recompiled block holding it, and the report's
// exact text, which the bug-report tooling parses.
//
// Each crash runs in a forked child, which writes its log to a file of its own
// and is waited for here. Linux only, as is the handler.

#if defined(__linux__)

#include "harness/EeRecTestHarness.h"
#include "harness/JitTestHarness.h"

#include "arm64/RecCrashReport-arm64.h"

#include "Memory.h"
#include "R5900.h"
#include "x86/BaseblockEx.h"

#include "common/Console.h"
#include "common/CrashHandler.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <vector>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace recompiler_tests;
using namespace mips;
using namespace RecCrashReport;

extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);
extern void recEeDispatcherAddrs(const void** dispatcher_event, const void** dispatcher_reg);
extern uptr psxRecLUT[];

namespace {

constexpr u32 kProgramPc = RecompilerTestEnvironment::kProgramPc;

std::vector<std::string> Lines(const std::string& text)
{
	std::vector<std::string> lines;
	std::istringstream in(text);
	for (std::string line; std::getline(in, line);)
		lines.push_back(line);
	return lines;
}

void AppendTo(void* context, const char* text, size_t length)
{
	static_cast<std::string*>(context)->append(text, length);
}

std::string Text(const Report& r)
{
	std::string text;
	Write(r, AppendTo, &text);
	return text;
}

// The code area the bug reports show: EE code at 0x111d00000.
constexpr uptr kBase = 0x111d00000;

Layout SampleLayout()
{
	Layout l;
	l.code_base = kBase;
	l.ee.blocks_start = kBase + 0x2000;
	l.ee.pool_start = kBase + HostMemoryMap::EErecSize - 0x40000;
	l.ee.cold_start = l.ee.pool_start - 0x800000;
	l.iop.blocks_start = kBase + HostMemoryMap::IOPrecOffset + 0x1000;
	l.iop.pool_start = kBase + HostMemoryMap::IOPrecOffset + HostMemoryMap::IOPrecSize - 0x10000;
	l.vu_blocks_start[0] = kBase + HostMemoryMap::mVU0recOffset + 0x3000;
	l.vu_blocks_start[1] = kBase + HostMemoryMap::mVU1recOffset + 0x3000;
	return l;
}

struct Crash
{
	int status = 0;
	std::string log;    // the child's emulog
	std::string err; // and what it wrote to stderr
};

std::string ReadFd(int fd)
{
	std::string text;
	char buf[4096];
	for (;;)
	{
		const ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0)
			text.append(buf, static_cast<size_t>(n));
		else if (n == 0 || errno != EINTR)
			break;
	}
	return text;
}

std::string ReadFile(const std::string& path)
{
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return {};
	std::string text = ReadFd(fd);
	close(fd);
	return text;
}

// Runs crash() in a child with a log file of its own and no core dump.
template <typename F>
Crash RunCrash(F&& crash)
{
	const std::string path = testing::TempDir() + "rec_crash_report_" + std::to_string(getpid()) + ".log";
	int err_pipe[2];
	if (pipe(err_pipe) != 0)
		return {};

	const pid_t pid = fork();
	if (pid == 0)
	{
		close(err_pipe[0]);
		dup2(err_pipe[1], STDERR_FILENO);
		close(err_pipe[1]);
		prctl(PR_SET_DUMPABLE, 0);
		Log::SetFileOutputLevel(LOGLEVEL_INFO, path);
		crash();
		_exit(99);
	}

	close(err_pipe[1]);
	Crash c;
	c.err = ReadFd(err_pipe[0]);
	close(err_pipe[0]);
	waitpid(pid, &c.status, 0);
	c.log = ReadFile(path);
	unlink(path.c_str());
	return c;
}

void LoadFromAddress8()
{
	*reinterpret_cast<volatile int*>(static_cast<uintptr_t>(8));
}

// A host block for the EE program at kProgramPc, compiled.
bool CompileEeBlock(uptr* fnptr, u32* host_size)
{
	EeRecTestHarness h;
	h.LoadProgram({
		ADDIU(reg::v0, reg::zero, 5),
		ADDU(reg::v1, reg::v0, reg::a0),
	});
	h.RunJitNoDiff();
	uptr lut = 0;
	return recEeBlockHostInfo(kProgramPc, fnptr, host_size, &lut);
}

} // namespace

TEST(RecCrashReport, PcsAreSortedIntoTheRecompilersAndTheirParts)
{
	const Layout l = SampleLayout();
	const auto area = [&l](uptr pc) { return Classify(l, pc).area; };

	EXPECT_EQ(area(kBase - 4), Area::None);
	EXPECT_EQ(area(kBase + HostMemoryMap::CodeSize), Area::None);
	EXPECT_EQ(area(kBase), Area::EeShared);
	EXPECT_EQ(area(kBase + 0x1ffc), Area::EeShared);
	EXPECT_EQ(area(kBase + 0x2000), Area::EeBlocks);
	EXPECT_EQ(area(l.ee.cold_start - 4), Area::EeBlocks);
	EXPECT_EQ(area(l.ee.cold_start), Area::EeColdExits);
	EXPECT_EQ(area(l.ee.pool_start), Area::EeConstants);
	EXPECT_EQ(area(kBase + HostMemoryMap::IOPrecOffset), Area::IopShared);
	EXPECT_EQ(area(l.iop.blocks_start), Area::IopBlocks);
	EXPECT_EQ(area(l.iop.pool_start), Area::IopConstants);
	EXPECT_EQ(area(kBase + HostMemoryMap::VIF0recOffset), Area::Vif0Dynarec);
	EXPECT_EQ(area(kBase + HostMemoryMap::VIF1recOffset + 8), Area::Vif1Dynarec);
	EXPECT_EQ(area(kBase + HostMemoryMap::mVU0recOffset + 0xcc4), Area::Vu0Shared);
	EXPECT_EQ(area(l.vu_blocks_start[0]), Area::Vu0Blocks);
	EXPECT_EQ(area(kBase + HostMemoryMap::mVU1recOffset), Area::Vu1Shared);
	EXPECT_EQ(area(l.vu_blocks_start[1] + 0x100), Area::Vu1Blocks);
	EXPECT_EQ(area(kBase + HostMemoryMap::VIFUnpackRecOffset), Area::VifUnpack);
	EXPECT_EQ(area(kBase + HostMemoryMap::SWrecOffset + 0x10), Area::SwRenderer);

	// The address in the bug reports: the second huge-page mapping
	// (microVU0 and microVU1) starts at 0x118d00000, and +0xcc4 is in
	// microVU0's dispatchers.
	const Where w = Classify(l, 0x118d00cc4);
	EXPECT_EQ(w.area, Area::Vu0Shared);
	EXPECT_EQ(w.region_offset, 0xcc4u);
	EXPECT_EQ(w.code_offset, HostMemoryMap::mVU0recOffset + 0xcc4u);
	EXPECT_STREQ(RegionName(w.area), "microVU0");
	EXPECT_STREQ(PartName(w.area), "shared code");

	// Boundaries not recorded yet: everything is blocks.
	Layout bare;
	bare.code_base = kBase;
	EXPECT_EQ(Classify(bare, kBase).area, Area::EeBlocks);
	EXPECT_EQ(Classify(bare, kBase + HostMemoryMap::mVU1recOffset).area, Area::Vu1Blocks);
	// No code area at all.
	EXPECT_EQ(Classify(Layout{}, kBase).area, Area::None);
}

TEST(RecCrashReport, TheLiveLayoutPutsTheDispatchersAndBlocksWhereTheyAre)
{
	uptr fnptr = 0;
	u32 host_size = 0;
	ASSERT_TRUE(CompileEeBlock(&fnptr, &host_size));
	const void* event = nullptr;
	const void* reg = nullptr;
	recEeDispatcherAddrs(&event, &reg);

	const Layout l = CurrentLayout();
	ASSERT_NE(l.code_base, 0u);
	EXPECT_EQ(Classify(l, reinterpret_cast<uptr>(reg)).area, Area::EeShared);
	EXPECT_EQ(Classify(l, fnptr).area, Area::EeBlocks);
	EXPECT_EQ(Classify(l, l.vu_blocks_start[1] - 4).area, Area::Vu1Shared);
	EXPECT_EQ(Classify(l, l.vu_blocks_start[1]).area, Area::Vu1Blocks);
	EXPECT_EQ(Classify(l, l.iop.blocks_start - 4).area, Area::IopShared);
}

TEST(RecCrashReport, AnEeHostPcLeadsToItsBlock)
{
	uptr fnptr = 0;
	u32 host_size = 0;
	ASSERT_TRUE(CompileEeBlock(&fnptr, &host_size));
	ASSERT_GT(host_size, 8u);

	BlockHit hit;
	ASSERT_TRUE(FindEeBlock(fnptr + 4, &hit));
	EXPECT_EQ(hit.guest_pc, kProgramPc);
	EXPECT_GE(hit.guest_words, 2u);
	EXPECT_EQ(hit.host_start, fnptr);
	EXPECT_EQ(hit.host_size, host_size);
	ASSERT_TRUE(FindEeBlock(fnptr + host_size - 4, &hit));
	EXPECT_EQ(hit.host_start, fnptr);

	const void* event = nullptr;
	const void* reg = nullptr;
	recEeDispatcherAddrs(&event, &reg);
	EXPECT_FALSE(FindEeBlock(reinterpret_cast<uptr>(reg), &hit)) << "a dispatcher is in no block";
}

TEST(RecCrashReport, AnIopHostPcLeadsToItsBlock)
{
	JitTestHarness h;
	h.LoadProgram({ADDIU(reg::v0, reg::a0, 1)});
	h.Run();
	const uptr fnptr = PC_GETBLOCK_(kProgramPc, psxRecLUT)->GetFnptr();

	BlockHit hit;
	ASSERT_TRUE(FindIopBlock(fnptr + 4, &hit));
	EXPECT_EQ(hit.guest_pc, kProgramPc);
	EXPECT_EQ(hit.host_start, fnptr);
	EXPECT_EQ(Classify(CurrentLayout(), fnptr).area, Area::IopBlocks);
}

Report SampleEeReport()
{
	Report r;
	r.signal = SIGSEGV;
	r.si_code = 1;
	r.fault_address = 8;
	r.tid = 4242;
	std::strcpy(r.thread_name, "CPU Thread");
	r.pc = 0x111d01234;
	r.where = {Area::EeBlocks, 0x1234, 0x1234};
	r.have_instruction = true;
	r.instruction = 0xf9400021;
	r.block_cpu = Cpu::EE;
	r.block_found = true;
	r.block = {0x00123450, 12, 0x111d01200, 0x180};
	r.have_host_registers = true;
	for (u32 i = 0; i < 31; i++)
		r.x[i] = i;
	r.sp = 0x7ffff000;
	r.pstate = 0x60000000;
	r.guest = Cpu::EE;
	r.gpr[2] = 1;
	r.gpr[31] = 0x00123458;
	r.hi = 0x11;
	r.lo = 0x22;
	r.guest_pc = 0x00123450;
	r.frame = 1234;
	r.code[0] = {0x00123450, 0x27bdfff0, true};
	r.code[1] = {0x00123454, 0, false};
	r.code_count = 2;
	return r;
}

TEST(RecCrashReport, TheReportForEeCodeReadsExactlySo)
{
	const std::vector<std::string> expected = {
		"Host fault: SIGSEGV (SEGV_MAPERR) at 0x0000000000000008",
		"  thread 4242 \"CPU Thread\", signal 11, si_code 1",
		"  pc 0x0000000111d01234: EE recompiler +0x00001234 (blocks), code area +0x00001234, instruction f9400021",
		"  block: EE 0x00123450, 12 instructions, host 0x0000000111d01200 +0x00000034 of 0x00000180",
		"  host registers:",
		"      x0 0000000000000000   x1 0000000000000001   x2 0000000000000002   x3 0000000000000003",
		"      x4 0000000000000004   x5 0000000000000005   x6 0000000000000006   x7 0000000000000007",
		"      x8 0000000000000008   x9 0000000000000009  x10 000000000000000a  x11 000000000000000b",
		"     x12 000000000000000c  x13 000000000000000d  x14 000000000000000e  x15 000000000000000f",
		"     x16 0000000000000010  x17 0000000000000011  x18 0000000000000012  x19 0000000000000013",
		"     x20 0000000000000014  x21 0000000000000015  x22 0000000000000016  x23 0000000000000017",
		"     x24 0000000000000018  x25 0000000000000019  x26 000000000000001a  x27 000000000000001b",
		"     x28 000000000000001c  x29 000000000000001d  x30 000000000000001e   sp 000000007ffff000",
		"    pstate 0000000060000000",
		"  EE registers (pinned ones from the host registers):",
		"    zero 0000000000000000   at 0000000000000000   v0 0000000000000001   v1 0000000000000000",
		"      a0 0000000000000000   a1 0000000000000000   a2 0000000000000000   a3 0000000000000000",
		"      t0 0000000000000000   t1 0000000000000000   t2 0000000000000000   t3 0000000000000000",
		"      t4 0000000000000000   t5 0000000000000000   t6 0000000000000000   t7 0000000000000000",
		"      s0 0000000000000000   s1 0000000000000000   s2 0000000000000000   s3 0000000000000000",
		"      s4 0000000000000000   s5 0000000000000000   s6 0000000000000000   s7 0000000000000000",
		"      t8 0000000000000000   t9 0000000000000000   k0 0000000000000000   k1 0000000000000000",
		"      gp 0000000000000000   sp 0000000000000000   fp 0000000000000000   ra 0000000000123458",
		"     hi 0000000000000011   lo 0000000000000022",
		"  EE pc 0x00123450 as last stored, frame 1234",
		"  EE code at the block:",
		"    00123450  27bdfff0",
		"    00123454  ????????",
		"Aborting application.",
	};
	EXPECT_EQ(Lines(Text(SampleEeReport())), expected);
}

TEST(RecCrashReport, IopRegistersAre32Bits)
{
	Report r = SampleEeReport();
	r.where = {Area::IopBlocks, 0x40, HostMemoryMap::IOPrecOffset + 0x40};
	r.block_cpu = Cpu::IOP;
	r.guest = Cpu::IOP;
	const std::vector<std::string> lines = Lines(Text(r));
	ASSERT_EQ(lines.size(), 29u);
	EXPECT_EQ(lines[2], "  pc 0x0000000111d01234: IOP recompiler +0x00000040 (blocks), code area +0x04000040, instruction f9400021");
	EXPECT_EQ(lines[3], "  block: IOP 0x00123450, 12 instructions, host 0x0000000111d01200 +0x00000034 of 0x00000180");
	EXPECT_EQ(lines[14], "  IOP registers:");
	EXPECT_EQ(lines[15], "    zero 00000000   at 00000000   v0 00000001   v1 00000000");
	EXPECT_EQ(lines[23], "     hi 00000011   lo 00000022");
	EXPECT_EQ(lines[24], "  IOP pc 0x00123450 as last stored, frame 1234");
	EXPECT_EQ(lines[25], "  IOP code at the block:");
}

TEST(RecCrashReport, MicroVuCodeGetsTheNearestBlockEntryAndTpc)
{
	Report r = SampleEeReport();
	r.pc = 0x118d03100;
	r.where = {Area::Vu0Blocks, 0x3100, HostMemoryMap::mVU0recOffset + 0x3100};
	r.block_cpu = Cpu::VU0;
	r.block_found = false;
	r.vu = {true, 3, 0x0000, 0x0120, 0x118d030c0, 0x01a8};
	r.guest = Cpu::None;
	r.code_count = 0;
	std::vector<std::string> lines = Lines(Text(r));
	ASSERT_EQ(lines.size(), 16u);
	EXPECT_EQ(lines[2], "  pc 0x0000000118d03100: microVU0 +0x00003100 (blocks), code area +0x07003100, instruction f9400021");
	EXPECT_EQ(lines[3], "  block: VU0 0x0120, nearest entry below the pc in program 3 (start 0x0000), host 0x0000000118d030c0 +0x00000040");
	EXPECT_EQ(lines[4], "  VU0 TPC 0x01a8 as last stored");
	EXPECT_EQ(lines[5], "  host registers:");
	EXPECT_EQ(lines[15], "Aborting application.");

	r.vu.found = false;
	lines = Lines(Text(r));
	ASSERT_GE(lines.size(), 5u);
	EXPECT_EQ(lines[3], "  block: none found");
	EXPECT_EQ(lines[4], "  VU0 TPC 0x01a8 as last stored");
}

TEST(RecCrashReport, APcOutsideTheCodeAreaSaysSo)
{
	Report r;
	r.signal = SIGBUS;
	r.si_code = 1;
	r.fault_address = 0x1001;
	r.tid = 7;
	std::strcpy(r.thread_name, "GS Thread");
	r.pc = 0x5555deadbeef;
	r.have_host_registers = true;
	const std::vector<std::string> lines = Lines(Text(r));
	ASSERT_EQ(lines.size(), 14u);
	EXPECT_EQ(lines[0], "Host fault: SIGBUS (BUS_ADRALN) at 0x0000000000001001");
	EXPECT_EQ(lines[1], "  thread 7 \"GS Thread\", signal 7, si_code 1");
	EXPECT_EQ(lines[2], "  pc 0x00005555deadbeef: not in recompiled code");
	EXPECT_EQ(lines[3], "  host registers:");
	EXPECT_EQ(lines[13], "Aborting application.");

	r.signal = SIGSEGV;
	r.si_code = 77;
	EXPECT_EQ(Lines(Text(r))[0], "Host fault: SIGSEGV (si_code 77) at 0x0000000000001001");
	r.si_code = SI_TKILL;
	EXPECT_EQ(Lines(Text(r))[0], "Host fault: SIGSEGV (SI_TKILL) at 0x0000000000001001");
}

// A fault in C++ code, outside the recompilers: still the real signal.
TEST(RecCrashReport, AnUnhandledFaultKillsTheProcessWithItsOwnSignal)
{
	const Crash c = RunCrash(LoadFromAddress8);
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGSEGV);
	EXPECT_NE(c.err.find("Aborting application."), std::string::npos) << c.err;

	const std::vector<std::string> lines = Lines(c.log);
	ASSERT_GE(lines.size(), 3u) << c.log;
	EXPECT_EQ(lines[0], "Host fault: SIGSEGV (SEGV_MAPERR) at 0x0000000000000008");
	EXPECT_NE(lines[2].find(": not in recompiled code"), std::string::npos) << lines[2];
	EXPECT_EQ(lines.back(), "Aborting application.");
}

// A jump to an unmapped address: the pc itself is the bad address. The handler
// must not read the instruction at the pc; doing so faulted again inside the
// handler, which re-entered itself until the stack was gone, and the report
// never got written.
TEST(RecCrashReport, AJumpToAnUnmappedPcIsReportedAsItself)
{
	const Crash c = RunCrash([] {
		reinterpret_cast<void (*)()>(static_cast<uintptr_t>(0x10))();
	});
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGSEGV);

	const std::vector<std::string> lines = Lines(c.log);
	ASSERT_GE(lines.size(), 3u) << c.log;
	EXPECT_EQ(lines[0], "Host fault: SIGSEGV (SEGV_MAPERR) at 0x0000000000000010");
	EXPECT_EQ(lines[2], "  pc 0x0000000000000010: not in recompiled code");
	EXPECT_EQ(lines.back(), "Aborting application.");
}

// The fault the bug reports could not tell apart: a load through a bad
// pointer inside a recompiled EE block. The child overwrites the block's code
// with one (its copy of the code area; the parent's is untouched) and calls it.
TEST(RecCrashReport, AFaultInAnEeBlockIsReportedAndKillsWithItsOwnSignal)
{
	uptr fnptr = 0;
	u32 host_size = 0;
	ASSERT_TRUE(CompileEeBlock(&fnptr, &host_size));
	ASSERT_GE(host_size, 12u);
	u32 first_word = 0;
	ASSERT_TRUE(RecErrorReport::ReadEeWord(kProgramPc, &first_word));

	const Crash c = RunCrash([fnptr] {
		static constexpr u32 kCode[] = {
			0xd2800100, // mov x0, #8
			0xf9400001, // ldr x1, [x0]
			0xd65f03c0, // ret
		};
		std::memcpy(reinterpret_cast<void*>(fnptr), kCode, sizeof(kCode));
		__builtin___clear_cache(reinterpret_cast<char*>(fnptr), reinterpret_cast<char*>(fnptr + sizeof(kCode)));
		reinterpret_cast<void (*)()>(fnptr)();
	});
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGSEGV);

	const std::vector<std::string> lines = Lines(c.log);
	ASSERT_GE(lines.size(), 30u) << c.log;
	char expect[256];
	EXPECT_EQ(lines[0], "Host fault: SIGSEGV (SEGV_MAPERR) at 0x0000000000000008");
	EXPECT_EQ(lines[1].rfind("  thread ", 0), 0u) << lines[1];
	const Where w = Classify(CurrentLayout(), fnptr + 4);
	std::snprintf(expect, sizeof(expect),
		"  pc 0x%016llx: EE recompiler +0x%08llx (blocks), code area +0x%08llx, instruction f9400001",
		static_cast<unsigned long long>(fnptr + 4), static_cast<unsigned long long>(w.region_offset),
		static_cast<unsigned long long>(w.code_offset));
	EXPECT_EQ(lines[2], expect);
	std::snprintf(expect, sizeof(expect), "  block: EE 0x%08x, ", kProgramPc);
	EXPECT_EQ(lines[3].rfind(expect, 0), 0u) << lines[3];
	std::snprintf(expect, sizeof(expect), "host 0x%016llx +0x00000004 of 0x%08x",
		static_cast<unsigned long long>(fnptr), host_size);
	EXPECT_NE(lines[3].find(expect), std::string::npos) << lines[3];
	EXPECT_EQ(lines[4], "  host registers:");
	EXPECT_EQ(lines[5].rfind("      x0 0000000000000008", 0), 0u) << lines[5];
	EXPECT_EQ(lines[14], "  EE registers (pinned ones from the host registers):");
	EXPECT_EQ(lines[25], "  EE code at the block:");
	std::snprintf(expect, sizeof(expect), "    %08x  %08x", kProgramPc, first_word);
	EXPECT_EQ(lines[26], expect);
	EXPECT_EQ(lines.back(), "Aborting application.");
}

// A SIGSEGV someone sent is not raised again by retrying an instruction, so the
// handler sends it again.
TEST(RecCrashReport, ASentSignalIsSentAgain)
{
	const Crash c = RunCrash([] { raise(SIGSEGV); });
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGSEGV);
}

// The crash handler installed over the page fault handler (the wrong order):
// it hands the fault back to the page fault handler, which hands it back to
// the crash handler, which must then give up to the default action rather
// than go round again.
TEST(RecCrashReport, OurHandlersHandingTheFaultToEachOtherDoNotLoop)
{
	const Crash c = RunCrash([] {
		CrashHandler::Install();
		LoadFromAddress8();
	});
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGSEGV);
}

#endif // defined(__linux__)
