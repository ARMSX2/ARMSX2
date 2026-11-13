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
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"

#include "arm64/RecCrashReport-arm64.h"

#include "Memory.h"
#include "R5900.h"
#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_Persist-arm64.h"
#include "x86/BaseblockEx.h"

#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/DynamicLibrary.h"
#include "common/ModuleMap.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <dlfcn.h>
#include <elf.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <vector>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <link.h>

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

[[gnu::noinline]] void ExecuteUdf()
{
	asm volatile(".inst 0x00000000"); // UDF #0
}

// Writes code at `at` (the child's copy of the code area) and calls it.
template <size_t N>
void CallCode(uptr at, const u32 (&code)[N])
{
	std::memcpy(reinterpret_cast<void*>(at), code, sizeof(code));
	__builtin___clear_cache(reinterpret_cast<char*>(at), reinterpret_cast<char*>(at + sizeof(code)));
	reinterpret_cast<void (*)()>(at)();
}

constexpr u32 kNop = 0xd503201f;
constexpr u32 kUdf = 0x00000000;

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

// The GNU build id of an ELF file, read from the file rather than from memory,
// as hex; empty when it has none.
std::string FileBuildId(const char* path)
{
	const int fd = open(path, O_RDONLY);
	if (fd < 0)
		return {};
	std::string hex;
	ElfW(Ehdr) eh;
	if (pread(fd, &eh, sizeof(eh), 0) == static_cast<ssize_t>(sizeof(eh)))
	{
		for (u32 i = 0; i < eh.e_phnum && hex.empty(); i++)
		{
			ElfW(Phdr) ph;
			if (pread(fd, &ph, sizeof(ph), eh.e_phoff + i * eh.e_phentsize) != static_cast<ssize_t>(sizeof(ph)) ||
				ph.p_type != PT_NOTE)
				continue;
			std::vector<u8> notes(ph.p_filesz);
			if (pread(fd, notes.data(), notes.size(), ph.p_offset) != static_cast<ssize_t>(notes.size()))
				continue;
			for (size_t at = 0; at + sizeof(ElfW(Nhdr)) <= notes.size();)
			{
				ElfW(Nhdr) nh;
				std::memcpy(&nh, &notes[at], sizeof(nh));
				const size_t name_at = at + sizeof(nh);
				const size_t desc_at = name_at + ((nh.n_namesz + 3) & ~3u);
				if (desc_at + nh.n_descsz > notes.size())
					break;
				if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 && std::memcmp(&notes[name_at], "GNU", 4) == 0)
				{
					for (u32 k = 0; k < nh.n_descsz; k++)
					{
						char b[3];
						std::snprintf(b, sizeof(b), "%02x", notes[desc_at + k]);
						hex += b;
					}
					break;
				}
				at = desc_at + ((nh.n_descsz + 3) & ~3u);
			}
		}
	}
	close(fd);
	return hex;
}

std::string HitBuildId(const ModuleMap::Hit& hit)
{
	std::string hex;
	for (u32 i = 0; i < hit.build_id_size; i++)
	{
		char b[3];
		std::snprintf(b, sizeof(b), "%02x", hit.build_id[i]);
		hex += b;
	}
	return hex;
}

std::string BaseName(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string ExePath()
{
	char path[1024];
	const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	return n > 0 ? std::string(path, static_cast<size_t>(n)) : std::string();
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

VuBlock Vb(u16 guest_pc, uptr host_start, u32 host_size, u16 guest_pairs)
{
	VuBlock b;
	b.guest_pc = guest_pc;
	b.host_start = host_start;
	b.host_size = host_size;
	b.guest_pairs = guest_pairs;
	return b;
}

// microVU code at 0x118d03100: inside block 0x0120 (host 0x118d030c0, 0x180
// bytes), which was compiled inside block 0x0000's compile.
Report SampleVuReport()
{
	Report r = SampleEeReport();
	r.pc = 0x118d03100;
	r.where = {Area::Vu0Blocks, 0x3100, HostMemoryMap::mVU0recOffset + 0x3100};
	r.block_cpu = Cpu::VU0;
	r.block_found = false;
	r.guest = Cpu::None;
	r.code_count = 0;
	VuHit& h = r.vu;
	h.found = true;
	h.contains = true;
	h.program = 3;
	h.current_program = 3;
	h.block = Vb(0x0120, 0x118d030c0, 0x180, 12);
	h.tpc = 0x01a8;
	h.window[0] = Vb(0x0000, 0x118d03000, 0x400, 4);
	h.window[1] = Vb(0x0120, 0x118d030c0, 0x180, 12);
	h.window[2] = Vb(0x0040, 0x118d03400, 0, 0);
	h.window_count = 3;
	h.blocks_below = 5;
	h.blocks_unsized = 1;
	return r;
}

TEST(RecCrashReport, MicroVuCodeNamesTheBlockHoldingThePc)
{
	const Report r = SampleVuReport();
	const std::vector<std::string> expected_vu = {
		"  pc 0x0000000118d03100: microVU0 +0x00003100 (blocks), code area +0x07003100, instruction f9400021",
		"  block: VU0 0x0120, nearest entry below the pc in program 3 (start 0x0000), host 0x0000000118d030c0 +0x00000040 of 0x00000180",
		"  VU0 TPC 0x01a8 as last stored",
		"  VU0 block extent: host 0x0000000118d030c0 +0x00000040 of 0x00000180, guest 0x0120 to 0x0178 (12 pairs), holds the pc",
		"  VU0 block map, program 3 (start 0x0000), by host address; > the block named, + another block holding the pc:",
		"    (5 lower blocks not shown)",
		"  + 0x0000  host 0x0000000118d03000  size 0x00000400, 4 pairs",
		"  > 0x0120  host 0x0000000118d030c0  size 0x00000180, 12 pairs",
		"    0x0040  host 0x0000000118d03400  size not recorded",
		"  host registers:",
	};
	const std::vector<std::string> lines = Lines(Text(r));
	ASSERT_EQ(lines.size(), 2u + expected_vu.size() + 10u) << Text(r);
	for (size_t i = 0; i < expected_vu.size(); i++)
		EXPECT_EQ(lines[2 + i], expected_vu[i]) << "line " << 2 + i;
	EXPECT_EQ(lines.back(), "Aborting application.");
}

TEST(RecCrashReport, MicroVuCodeOutsideEveryBlockSaysSo)
{
	Report r = SampleVuReport();
	r.vu.contains = false;
	r.vu.block = Vb(0x0040, 0x118d03000, 0x80, 2);
	r.vu.window[0] = r.vu.block;
	r.vu.window_count = 1;
	r.vu.blocks_below = 0;
	r.vu.blocks_above = 7;
	std::vector<std::string> lines = Lines(Text(r));
	ASSERT_GE(lines.size(), 9u);
	EXPECT_EQ(lines[3], "  block: VU0 0x0040, nearest entry below the pc in program 3 (start 0x0000), host 0x0000000118d03000 +0x00000100 of 0x00000080");
	EXPECT_EQ(lines[5], "  VU0 block extent: pc is not inside any block of program 3 or of another program of VU0 (1 blocks of program 3 have no recorded size)");
	EXPECT_EQ(lines[7], "  > 0x0040  host 0x0000000118d03000  size 0x00000080, 2 pairs");
	EXPECT_EQ(lines[8], "    (7 higher blocks not shown)");
	EXPECT_EQ(lines[9], "  host registers:");

	// The block of another program holds it.
	r = SampleVuReport();
	r.vu.program = 5;
	EXPECT_EQ(Lines(Text(r))[5], "  VU0 block extent: host 0x0000000118d030c0 +0x00000040 of 0x00000180, guest 0x0120 to 0x0178 (12 pairs), holds the pc; program 5 is not the current program 3");

	// A block with no recorded size: the old line, word for word.
	r = SampleVuReport();
	r.vu.contains = false;
	r.vu.block.host_size = 0;
	lines = Lines(Text(r));
	EXPECT_EQ(lines[3], "  block: VU0 0x0120, nearest entry below the pc in program 3 (start 0x0000), host 0x0000000118d030c0 +0x00000040");

	r.vu = VuHit{};
	r.vu.tpc = 0x01a8;
	lines = Lines(Text(r));
	ASSERT_GE(lines.size(), 7u);
	EXPECT_EQ(lines[3], "  block: none found");
	EXPECT_EQ(lines[4], "  VU0 TPC 0x01a8 as last stored");
	EXPECT_EQ(lines[5], "  VU0 block extent: no current program");
	EXPECT_EQ(lines[6], "  host registers:");
}

TEST(RecCrashReport, MicroVuCodeShowsTheBlocksMicrocode)
{
	Report r = SampleVuReport();
	r.vu_code[0] = {0x0120, 0x8000033c, 0x8000033c, true, true};
	r.vu_code[1] = {0x0124, 0x000002ff, 0x000002fe, true, true};
	r.vu_code[2] = {0x0128, 0, 0x4a0002bc, false, true};
	r.vu_code[3] = {0x012c, 0x000002ff, 0, true, false};
	r.vu_code[4] = {0x0130, 0, 0, false, false};
	r.vu_code_count = 5;
	const std::vector<std::string> lines = Lines(Text(r));
	ASSERT_GE(lines.size(), 13u);
	EXPECT_EQ(lines[5].rfind("  VU0 block extent: ", 0), 0u);
	EXPECT_EQ(lines[6], "  VU0 code at the block:");
	EXPECT_EQ(lines[7], "    0120  8000033c");
	EXPECT_EQ(lines[8], "    0124  000002ff  micro memory now 000002fe");
	EXPECT_EQ(lines[9], "    0128  4a0002bc  from micro memory, not in the program's copy");
	EXPECT_EQ(lines[10], "    012c  000002ff");
	EXPECT_EQ(lines[11], "    0130  ????????");
	EXPECT_EQ(lines[12].rfind("  VU0 block map, ", 0), 0u);
	EXPECT_EQ(lines.size(), Lines(Text(SampleVuReport())).size() + 6);
}

TEST(RecCrashReport, VuCodeIsTheBlocksLengthAndStaysInMicroMemory)
{
	u8 micro[0x100];
	for (u32 i = 0; i < sizeof(micro); i++)
		micro[i] = static_cast<u8>(i);
	VuWord code[kMaxCodeWords];
	const auto hit = [](VuBlock b, u32 mask = 0) {
		VuHit h;
		h.found = true;
		h.block = b;
		for (u32 i = 0; i < kMaxCodeWords; i++)
			h.program_code[i] = 0xc0de0000 + i;
		h.program_code_mask = mask;
		return h;
	};

	// Three pairs: six words, the program's copy where it holds them.
	ASSERT_EQ(ReadVuCode(micro, sizeof(micro), hit(Vb(0x10, 0, 0x40, 3), 0x3), code), 6u);
	EXPECT_EQ(code[0].addr, 0x10u);
	EXPECT_TRUE(code[0].in_program);
	EXPECT_EQ(code[0].program, 0xc0de0000u);
	EXPECT_EQ(code[0].live, 0x13121110u);
	EXPECT_TRUE(code[1].in_program);
	EXPECT_FALSE(code[2].in_program);
	EXPECT_EQ(code[5].addr, 0x24u);
	EXPECT_TRUE(code[5].live_readable);
	// Long, or not recorded: sixteen.
	EXPECT_EQ(ReadVuCode(micro, sizeof(micro), hit(Vb(0x10, 0, 0x40, 40)), code), kMaxCodeWords);
	EXPECT_EQ(ReadVuCode(micro, sizeof(micro), hit(Vb(0x10, 0, 0, 0)), code), kMaxCodeWords);
	// Near the end of micro memory: cut there.
	EXPECT_EQ(ReadVuCode(micro, sizeof(micro), hit(Vb(0xf0, 0, 0x40, 40)), code), 4u);
	EXPECT_EQ(code[3].addr, 0xfcu);
	EXPECT_EQ(ReadVuCode(micro, sizeof(micro), hit(Vb(0xfe, 0, 0x40, 1)), code), 0u);
	EXPECT_EQ(ReadVuCode(micro, sizeof(micro), hit(Vb(0x100, 0, 0x40, 1)), code), 0u);
	// No micro memory: the program's copy still.
	ASSERT_EQ(ReadVuCode(nullptr, sizeof(micro), hit(Vb(0, 0, 0x40, 1), 0x1), code), 2u);
	EXPECT_TRUE(code[0].in_program);
	EXPECT_FALSE(code[0].live_readable);
}

// The triage parser (bugreport-worker/triage/hostfault.py) reads the block
// line by this prefix.
TEST(RecCrashReport, TheMicroVuBlockLineKeepsTheParsersPrefix)
{
	const std::vector<std::string> lines = Lines(Text(SampleVuReport()));
	EXPECT_EQ(lines[3].rfind("  block: VU0 0x0120, nearest entry below the pc", 0), 0u) << lines[3];
}

TEST(RecCrashReport, AVuBlockScanFindsTheInnermostBlockHoldingThePc)
{
	const auto scan = [](uptr pc, std::initializer_list<VuBlock> blocks, VuHit* hit) {
		VuBlockScan s(pc);
		for (const VuBlock& b : blocks)
			s.Add(b);
		hit->found = s.Best(&hit->block);
		hit->contains = s.Contains();
		s.FillWindow(hit);
	};
	// A at 0x1000 for 0x300 holds B (0x1100 for 0x80), compiled inside it; C
	// at 0x2000 for 0x40, after a gap.
	const VuBlock a = Vb(0x0000, 0x1000, 0x300, 3);
	const VuBlock b = Vb(0x0018, 0x1100, 0x80, 2);
	const VuBlock c = Vb(0x0030, 0x2000, 0x40, 1);
	VuHit h;

	scan(0x1104, {c, a, b}, &h);
	EXPECT_TRUE(h.found && h.contains);
	EXPECT_EQ(h.block.guest_pc, 0x0018) << "the nested block, not the one it was compiled in";
	EXPECT_EQ(h.block.host_size, 0x80u);

	// Past B's end, still in A: A, though B is the nearer entry.
	h = VuHit{};
	scan(0x1200, {a, b, c}, &h);
	EXPECT_TRUE(h.contains);
	EXPECT_EQ(h.block.guest_pc, 0x0000);

	// In the gap between A and C: the nearest entry, not holding it.
	h = VuHit{};
	scan(0x1800, {a, b, c}, &h);
	EXPECT_TRUE(h.found);
	EXPECT_FALSE(h.contains);
	EXPECT_EQ(h.block.guest_pc, 0x0018);

	// The last byte of C, and the first past it.
	h = VuHit{};
	scan(0x203f, {a, b, c}, &h);
	EXPECT_TRUE(h.contains);
	EXPECT_EQ(h.block.guest_pc, 0x0030);
	h = VuHit{};
	scan(0x2040, {a, b, c}, &h);
	EXPECT_FALSE(h.contains);

	// Below every block: nothing named, the blocks above in the window.
	h = VuHit{};
	scan(0x800, {a, b, c}, &h);
	EXPECT_FALSE(h.found);
	EXPECT_EQ(h.window_count, 3u);
	EXPECT_EQ(h.window[0].host_start, 0x1000u);

	// A block with no recorded size holds nothing.
	h = VuHit{};
	scan(0x1104, {Vb(0x0018, 0x1100, 0, 0)}, &h);
	EXPECT_TRUE(h.found);
	EXPECT_FALSE(h.contains);
	EXPECT_EQ(h.blocks_unsized, 1u);
}

TEST(RecCrashReport, TheVuBlockWindowIsTheBlocksNearestThePc)
{
	// 100 blocks of 0x40 bytes, back to back from 0x10000; the pc in block 60.
	VuBlockScan scan(0x10000 + 60 * 0x40 + 8);
	for (u32 i = 100; i-- > 0;)
		scan.Add(Vb(static_cast<u16>(i * 8), 0x10000 + i * 0x40, 0x40, 1));
	VuHit h;
	h.found = scan.Best(&h.block);
	scan.FillWindow(&h);
	ASSERT_EQ(h.window_count, kMaxVuWindow);
	EXPECT_EQ(h.window[0].guest_pc, (61 - (kMaxVuWindow - kMaxVuWindowAbove)) * 8) << "the lowest one shown";
	EXPECT_EQ(h.window[kMaxVuWindow - kMaxVuWindowAbove - 1].guest_pc, 60 * 8) << "the one holding the pc";
	EXPECT_EQ(h.window[kMaxVuWindow - 1].guest_pc, (60 + kMaxVuWindowAbove) * 8);
	EXPECT_EQ(h.blocks_below, 61 - (kMaxVuWindow - kMaxVuWindowAbove));
	EXPECT_EQ(h.blocks_above, 39 - kMaxVuWindowAbove);
	for (u32 i = 1; i < h.window_count; i++)
		EXPECT_LT(h.window[i - 1].host_start, h.window[i].host_start);

	// Fewer below than the window holds: more above make up the rest.
	VuBlockScan low(0x10000 + 2 * 0x40);
	for (u32 i = 0; i < 100; i++)
		low.Add(Vb(static_cast<u16>(i * 8), 0x10000 + i * 0x40, 0x40, 1));
	VuHit l;
	low.FillWindow(&l);
	ASSERT_EQ(l.window_count, kMaxVuWindow);
	EXPECT_EQ(l.window[0].guest_pc, 0);
	EXPECT_EQ(l.blocks_below, 0u);
	EXPECT_EQ(l.blocks_above, 100 - kMaxVuWindow);

	// The printed map: one line per block, and a marker for each side cut.
	Report r = SampleVuReport();
	r.pc = 0x10000 + 60 * 0x40 + 8;
	r.vu.block = h.block;
	std::copy(std::begin(h.window), std::end(h.window), std::begin(r.vu.window));
	r.vu.window_count = h.window_count;
	r.vu.blocks_below = h.blocks_below;
	r.vu.blocks_above = h.blocks_above;
	r.vu.blocks_unsized = 0;
	const std::vector<std::string> lines = Lines(Text(r));
	ASSERT_EQ(lines.size(), 7u + 1 + kMaxVuWindow + 1 + 10 + 1) << Text(r);
	EXPECT_EQ(lines[7], "    (21 lower blocks not shown)");
	EXPECT_EQ(lines[8 + kMaxVuWindow], "    (31 higher blocks not shown)");
	EXPECT_EQ(lines[8 + kMaxVuWindow - kMaxVuWindowAbove - 1].rfind("  > 0x01e0  host ", 0), 0u);
}

// The whole report has a cap, and the last line is written past it.
TEST(RecCrashReport, TheReportStopsAtItsCap)
{
	Report r = SampleVuReport();
	r.vu.window_count = kMaxVuWindow;
	for (u32 i = 0; i < kMaxVuWindow; i++)
		r.vu.window[i] = Vb(static_cast<u16>(i * 8), 0x118d00000 + i * 0x40, 0x40, 1);
	std::string text;
	Write(r, AppendTo, &text, 1000);
	const std::vector<std::string> lines = Lines(text);
	EXPECT_LE(text.size(), 1000u);
	ASSERT_GE(lines.size(), 3u);
	EXPECT_EQ(lines[lines.size() - 2], "  (report cut short here, at its size cap)");
	EXPECT_EQ(lines.back(), "Aborting application.");

	// Everything at once, as long as it gets: well under the cap.
	r.guest = Cpu::EE;
	r.code_count = kMaxCodeWords;
	r.vu_code_count = kMaxCodeWords;
	std::memset(r.thread_name, 'x', sizeof(r.thread_name) - 1);
	text = Text(r);
	EXPECT_LT(text.size(), kMaxReportBytes / 2) << text.size();
	EXPECT_EQ(text.find("cut short"), std::string::npos);
}

// Real blocks: each records its extent, the lookup finds each at its first
// and last word, and a block compiled inside another is the one named.
TEST(RecCrashReport, CompiledVuBlocksRecordTheirExtent)
{
	using namespace vu;
	const auto lower = [](u32 op) { return VuOp{op, VNOP_U()}; };
	VuTestHarness h(0);
	h.SetVi(1, 1);
	h.LoadProgram({
		/* 0x00 */ lower(VIADDIU_L(vi::vi2, vi::vi0, 3)),
		/* 0x08 */ lower(VIBNE_L(vi::vi1, vi::vi0, 3)), // to 0x30
		/* 0x10 */ lower(0), // delay slot
		/* 0x18 */ lower(VIADDIU_L(vi::vi3, vi::vi0, 1)), // not taken
		/* 0x20 */ EBit(lower(0)),
		/* 0x28 */ lower(0),
		/* 0x30 */ EBit(lower(VIADDIU_L(vi::vi4, vi::vi0, 1))), // taken
	});
	h.RunNoDiff(); // its interpreter pass drops the compiled blocks
	h.RunJitPreserveBlockCache();

	VuHit all;
	FindVuBlock(0, UINTPTR_MAX, &all);
	ASSERT_GE(all.program, 0);
	ASSERT_GE(all.window_count, 2u);
	EXPECT_EQ(all.blocks_unsized, 0u);
	bool nested = false;
	for (u32 i = 0; i < all.window_count; i++)
	{
		const VuBlock& b = all.window[i];
		ASSERT_GT(b.host_size, 0u);
		ASSERT_GT(b.guest_pairs, 0u);
		if (b.guest_pc == 0)
		{
			EXPECT_EQ(b.guest_pairs, 3u) << "the entry block runs to the branch's delay slot";
			VuHit at;
			FindVuBlock(0, b.host_start, &at);
			VuWord code[kMaxCodeWords];
			ASSERT_EQ(ReadVuCode(vuRegs[0].Micro, VU0_PROGSIZE, at, code), 6u);
			for (u32 k = 0; k < 6; k++)
			{
				EXPECT_TRUE(code[k].in_program) << "word " << k << " is in the program's copy";
				EXPECT_EQ(code[k].program, code[k].live) << "word " << k;
			}
			EXPECT_EQ(code[0].program, VIADDIU_L(vi::vi2, vi::vi0, 3)) << "a pair's lower instruction first";
			EXPECT_EQ(code[1].program, VNOP_U());
			EXPECT_EQ(code[2].program, VIBNE_L(vi::vi1, vi::vi0, 3));

			// Micro memory written after the compile: the copy keeps what was
			// compiled, and the change shows.
			u32 saved;
			std::memcpy(&saved, vuRegs[0].Micro + 8, 4);
			const u32 changed = saved ^ 0x00010000;
			std::memcpy(vuRegs[0].Micro + 8, &changed, 4);
			ASSERT_EQ(ReadVuCode(vuRegs[0].Micro, VU0_PROGSIZE, at, code), 6u);
			std::memcpy(vuRegs[0].Micro + 8, &saved, 4);
			EXPECT_EQ(code[2].program, VIBNE_L(vi::vi1, vi::vi0, 3));
			EXPECT_EQ(code[2].live, changed);
		}

		VuHit at;
		FindVuBlock(0, b.host_start, &at);
		EXPECT_TRUE(at.contains);
		EXPECT_EQ(at.block.host_start, b.host_start) << "block 0x" << std::hex << b.guest_pc;
		at = VuHit{};
		FindVuBlock(0, b.host_start + b.host_size - 4, &at);
		EXPECT_TRUE(at.contains);
		for (u32 k = 0; k < all.window_count; k++)
		{
			const VuBlock& o = all.window[k];
			if (k != i && o.host_start > b.host_start && o.host_start + o.host_size <= b.host_start + b.host_size)
				nested = true;
		}
	}
	EXPECT_TRUE(nested) << "the not-taken path is compiled inside the branching block";

	// Past the last block's code.
	const VuBlock& last = all.window[all.window_count - 1];
	VuHit past;
	FindVuBlock(0, last.host_start + last.host_size + 0x10000, &past);
	EXPECT_TRUE(past.found);
	EXPECT_FALSE(past.contains);
}

// The program cache keeps the extent: a hydrated program's blocks have the
// sizes the compiled ones had.
TEST(RecCrashReport, AHydratedVuProgramKeepsItsExtents)
{
	using namespace vu;
	mVUPersist::SetRecordingEnabled(true);
	VuTestHarness h(0);
	h.SetVi(1, 1);
	h.LoadProgram({
		VuOp{VIADDIU_L(vi::vi2, vi::vi0, 3), VNOP_U()},
		VuOp{VIBNE_L(vi::vi1, vi::vi0, 1), VNOP_U()},
		VuOp{0, VNOP_U()},
		EBit(VuOp{0, VNOP_U()}),
	});
	h.RunNoDiff(); // its interpreter pass drops the compiled blocks
	h.RunJitPreserveBlockCache();
	VuHit compiled;
	FindVuBlock(0, UINTPTR_MAX, &compiled);
	std::vector<u8> image;
	ASSERT_TRUE(mVUPersist::TestSerializeNewestProgram(0, image));

	RecompilerTestEnvironment::ResetVuBlockCache(0);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	ASSERT_TRUE(mVUPersist::TestHydrate(0, image.data(), image.size()));
	const u64 compiles = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUPersist::GetBlockCompileCount(0), compiles) << "the hydrated blocks ran, none was compiled";
	VuHit hydrated;
	FindVuBlock(0, UINTPTR_MAX, &hydrated);
	mVUPersist::SetRecordingEnabled(false);
	RecompilerTestEnvironment::ResetVuBlockCache(0);

	ASSERT_GT(compiled.window_count, 0u);
	ASSERT_EQ(hydrated.window_count, compiled.window_count);
	for (u32 i = 0; i < compiled.window_count; i++)
	{
		EXPECT_EQ(hydrated.window[i].guest_pc, compiled.window[i].guest_pc);
		EXPECT_EQ(hydrated.window[i].host_size, compiled.window[i].host_size);
		EXPECT_EQ(hydrated.window[i].guest_pairs, compiled.window[i].guest_pairs);
	}
}

// A fault inside a compiled VU0 block, end to end: the child overwrites the
// block's code with a load through a bad pointer and calls it.
TEST(RecCrashReport, AFaultInAVuBlockNamesTheBlockAndItsMicrocode)
{
	using namespace vu;
	VuTestHarness h(0);
	h.LoadProgram({
		VuOp{VIADDIU_L(vi::vi2, vi::vi0, 3), VNOP_U()},
		EBit(VuOp{VIADDIU_L(vi::vi3, vi::vi0, 4), VNOP_U()}),
	});
	h.RunNoDiff(); // its interpreter pass drops the compiled blocks
	h.RunJitPreserveBlockCache();
	VuHit all;
	FindVuBlock(0, UINTPTR_MAX, &all);
	VuBlock entry;
	for (u32 i = 0; i < all.window_count; i++)
	{
		if (all.window[i].guest_pc == 0)
			entry = all.window[i];
	}
	ASSERT_NE(entry.host_start, 0u);
	ASSERT_GE(entry.host_size, 12u);

	const uptr fnptr = entry.host_start;
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
	ASSERT_GE(lines.size(), 12u) << c.log;
	EXPECT_NE(lines[2].find(": microVU0 +0x"), std::string::npos) << lines[2];
	char expect[256];
	std::snprintf(expect, sizeof(expect),
		"  block: VU0 0x0000, nearest entry below the pc in program %d (start 0x0000), host 0x%016llx +0x00000004 of 0x%08x",
		all.program, static_cast<unsigned long long>(fnptr), entry.host_size);
	EXPECT_EQ(lines[3], expect);
	EXPECT_EQ(lines[4].rfind("  VU0 TPC 0x", 0), 0u) << lines[4];
	std::snprintf(expect, sizeof(expect),
		"  VU0 block extent: host 0x%016llx +0x00000004 of 0x%08x, guest 0x0000 to 0x%04x (%u pairs), holds the pc",
		static_cast<unsigned long long>(fnptr), entry.host_size, (entry.guest_pairs - 1u) * 8u, entry.guest_pairs);
	EXPECT_EQ(lines[5], expect);
	EXPECT_EQ(lines[6], "  VU0 code at the block:");
	std::snprintf(expect, sizeof(expect), "    0000  %08x", VIADDIU_L(vi::vi2, vi::vi0, 3));
	EXPECT_EQ(lines[7], expect);
	std::snprintf(expect, sizeof(expect), "    0004  %08x", VNOP_U());
	EXPECT_EQ(lines[8], expect);
	bool map = false, named = false;
	for (const std::string& l : lines)
	{
		map |= l.rfind("  VU0 block map, program ", 0) == 0;
		named |= l.rfind("  > 0x0000  host ", 0) == 0;
	}
	EXPECT_TRUE(map && named) << c.log;
	EXPECT_EQ(lines.back(), "Aborting application.");
	RecompilerTestEnvironment::ResetVuBlockCache(0);
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

TEST(RecCrashReport, APcInALibraryNamesItsModuleAndBuildId)
{
	Report r;
	r.signal = SIGSEGV;
	r.si_code = 1;
	r.pc = 0x7a1b2c3d40;
	r.have_module = true;
	std::strcpy(r.module.name, "libvulkan_freedreno.so");
	r.module.offset = 0x1c3d40;
	const u8 id[20] = {0x5f, 0x0c, 0x3a, 0x9e, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
		0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
	std::memcpy(r.module.build_id, id, sizeof(id));
	r.module.build_id_size = sizeof(id);
	EXPECT_EQ(Lines(Text(r))[2], "  pc 0x0000007a1b2c3d40: not in recompiled code "
								 "(libvulkan_freedreno.so+0x1c3d40, build-id 5f0c3a9e0102030405060708090a0b0c0d0e0f10)");

	r.module.build_id_size = 0;
	r.module.offset = 0;
	EXPECT_EQ(Lines(Text(r))[2], "  pc 0x0000007a1b2c3d40: not in recompiled code (libvulkan_freedreno.so+0x0, no build-id)");
}

TEST(RecCrashReport, TheLrLineNamesTheCaller)
{
	Report r = SampleEeReport();
	r.have_lr = true;
	r.lr = 0x111d01200;
	r.lr_where = {Area::EeBlocks, 0x1200, 0x1200};
	std::vector<std::string> lines = Lines(Text(r));
	ASSERT_EQ(lines.size(), 30u);
	EXPECT_EQ(lines[3].rfind("  block: EE ", 0), 0u);
	EXPECT_EQ(lines[4], "  lr 0x0000000111d01200: EE recompiler +0x00001200 (blocks), code area +0x00001200");
	EXPECT_EQ(lines[5], "  host registers:");

	r.lr = 0x7a1b2c3d40;
	r.lr_where = {};
	r.lr_have_module = true;
	std::strcpy(r.lr_module.name, "libc.so");
	r.lr_module.offset = 0x8d40;
	lines = Lines(Text(r));
	EXPECT_EQ(lines[4], "  lr 0x0000007a1b2c3d40: not in recompiled code (libc.so+0x8d40, no build-id)");
	r.lr_have_module = false;
	EXPECT_EQ(Lines(Text(r))[4], "  lr 0x0000007a1b2c3d40: not in recompiled code");

	// After the microVU lines, too.
	Report v = SampleVuReport();
	v.have_lr = true;
	v.lr = 0x118d00cc4;
	v.lr_where = {Area::Vu0Shared, 0xcc4, HostMemoryMap::mVU0recOffset + 0xcc4};
	lines = Lines(Text(v));
	ASSERT_GE(lines.size(), 13u);
	EXPECT_EQ(lines[11], "  lr 0x0000000118d00cc4: microVU0 +0x00000cc4 (shared code), code area +0x07000cc4");
	EXPECT_EQ(lines[12], "  host registers:");
}

// A return address signed by PACIASP comes back as it was signed; on a core
// without pointer authentication both are NOPs and nothing changes.
TEST(RecCrashReport, TheLrLosesItsPointerAuthCode)
{
	const uptr plain = reinterpret_cast<uptr>(&FileBuildId);
	EXPECT_EQ(StripPointerAuth(plain), plain);
	uptr signed_lr;
	asm volatile("mov x30, %1\n\thint #25\n\tmov %0, x30" : "=r"(signed_lr) : "r"(plain) : "x30"); // PACIASP
	EXPECT_EQ(StripPointerAuth(signed_lr), plain) << std::hex << "signed as 0x" << signed_lr;
}

// The module table, looked up as the handler does: the test binary and libc,
// with the build ids their files carry.
TEST(RecCrashReport, TheModuleMapNamesTheExecutableAndLibc)
{
	ModuleMap::Refresh();
	EXPECT_GT(ModuleMap::Count(), 2u);
	EXPECT_EQ(ModuleMap::Dropped(), 0u);

	// The executable: no name from the dynamic linker, so its file's.
	const std::string exe = ExePath();
	ModuleMap::Hit hit;
	const uptr in_exe = reinterpret_cast<uptr>(&FileBuildId);
	ASSERT_TRUE(ModuleMap::Find(in_exe, &hit));
	EXPECT_EQ(std::string(hit.name), BaseName(exe));
	EXPECT_EQ(HitBuildId(hit), FileBuildId(exe.c_str()));
	Dl_info info = {};
	ASSERT_NE(dladdr(reinterpret_cast<void*>(in_exe), &info), 0);
	EXPECT_EQ(hit.offset, in_exe - reinterpret_cast<uptr>(info.dli_fbase)) << "the ELF address (first segment at 0)";

	// libc.
	const uptr in_libc = reinterpret_cast<uptr>(&::write);
	ASSERT_NE(dladdr(reinterpret_cast<void*>(in_libc), &info), 0);
	ASSERT_TRUE(ModuleMap::Find(in_libc, &hit));
	EXPECT_EQ(std::string(hit.name), BaseName(info.dli_fname));
	EXPECT_EQ(std::string(hit.name).rfind("libc.so", 0), 0u) << hit.name;
	EXPECT_EQ(HitBuildId(hit), FileBuildId(info.dli_fname));
	EXPECT_GT(hit.build_id_size, 0u);

	// Nothing there.
	EXPECT_FALSE(ModuleMap::Find(0x10, &hit));
}

// A library the emulator loads is in the table straight after.
TEST(RecCrashReport, TheModuleMapIsRefreshedWhenALibraryIsLoaded)
{
	DynamicLibrary lib;
	if (!lib.Open("libvulkan.so.1", nullptr))
		GTEST_SKIP() << "no libvulkan.so.1 here";
	void* sym = nullptr;
	ASSERT_TRUE(lib.GetSymbol("vkGetInstanceProcAddr", &sym));
	ModuleMap::Hit hit;
	ASSERT_TRUE(ModuleMap::Find(reinterpret_cast<uptr>(sym), &hit));
	EXPECT_EQ(std::string(hit.name).rfind("libvulkan.so", 0), 0u) << hit.name;
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
	// The fault is in this executable.
	const std::string exe = ExePath();
	const std::string module = ": not in recompiled code (" + BaseName(exe) + "+0x";
	EXPECT_NE(lines[2].find(module), std::string::npos) << lines[2];
	const std::string id = FileBuildId(exe.c_str());
	EXPECT_NE(lines[2].find(id.empty() ? ", no build-id)" : ", build-id " + id + ")"), std::string::npos) << lines[2];
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
	// The lr names where the jump came from.
	ASSERT_GE(lines.size(), 4u);
	EXPECT_EQ(lines[3].rfind("  lr 0x", 0), 0u) << lines[3];
	EXPECT_NE(lines[3].find(": not in recompiled code (" + BaseName(ExePath()) + "+0x"), std::string::npos) << lines[3];
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
	ASSERT_GE(lines.size(), 31u) << c.log;
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
	// The block was called from this executable.
	const std::string from_exe = ": not in recompiled code (" + BaseName(ExePath()) + "+0x";
	EXPECT_EQ(lines[4].rfind("  lr 0x", 0), 0u) << lines[4];
	EXPECT_NE(lines[4].find(from_exe), std::string::npos) << lines[4];
	EXPECT_EQ(lines[5], "  host registers:");
	EXPECT_EQ(lines[6].rfind("      x0 0000000000000008", 0), 0u) << lines[6];
	EXPECT_EQ(lines[15], "  EE registers (pinned ones from the host registers):");
	EXPECT_EQ(lines[26], "  EE code at the block:");
	std::snprintf(expect, sizeof(expect), "    %08x  %08x", kProgramPc, first_word);
	EXPECT_EQ(lines[27], expect);
	EXPECT_EQ(lines.back(), "Aborting application.");
}

// An illegal instruction in recompiled code: the same report, and the process
// dies by SIGILL. SIGILL's fault address is the pc.
TEST(RecCrashReport, AnIllegalInstructionInAnEeBlockIsReportedAndKillsWithSigill)
{
	uptr fnptr = 0;
	u32 host_size = 0;
	ASSERT_TRUE(CompileEeBlock(&fnptr, &host_size));
	ASSERT_GE(host_size, 8u);

	const Crash c = RunCrash([fnptr] { CallCode(fnptr, {kNop, kUdf}); });
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGILL);

	const std::vector<std::string> lines = Lines(c.log);
	ASSERT_GE(lines.size(), 6u) << c.log;
	char expect[256];
	std::snprintf(expect, sizeof(expect), "Host fault: SIGILL (ILL_ILLOPC) at 0x%016llx",
		static_cast<unsigned long long>(fnptr + 4));
	EXPECT_EQ(lines[0], expect);
	EXPECT_EQ(lines[1].rfind("  thread ", 0), 0u) << lines[1];
	EXPECT_NE(lines[1].find(", signal 4, si_code 1"), std::string::npos) << lines[1];
	const Where w = Classify(CurrentLayout(), fnptr + 4);
	std::snprintf(expect, sizeof(expect),
		"  pc 0x%016llx: EE recompiler +0x%08llx (blocks), code area +0x%08llx, instruction 00000000",
		static_cast<unsigned long long>(fnptr + 4), static_cast<unsigned long long>(w.region_offset),
		static_cast<unsigned long long>(w.code_offset));
	EXPECT_EQ(lines[2], expect);
	std::snprintf(expect, sizeof(expect), "host 0x%016llx +0x00000004 of 0x%08x",
		static_cast<unsigned long long>(fnptr), host_size);
	EXPECT_EQ(lines[3].rfind("  block: EE ", 0), 0u) << lines[3];
	EXPECT_NE(lines[3].find(expect), std::string::npos) << lines[3];
	EXPECT_EQ(lines.back(), "Aborting application.");
}

// The second-boot reports' shape: an illegal word at the start of microVU1's
// region, where its dispatcher is generated.
TEST(RecCrashReport, AnIllegalInstructionAtTheMicroVu1RegionStartIsReported)
{
	const Layout l = CurrentLayout();
	ASSERT_NE(l.code_base, 0u);
	const uptr region = l.code_base + HostMemoryMap::mVU1recOffset;

	const Crash c = RunCrash([region] { CallCode(region, {kUdf}); });
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGILL);

	const std::vector<std::string> lines = Lines(c.log);
	ASSERT_GE(lines.size(), 5u) << c.log;
	EXPECT_EQ(lines[0].rfind("Host fault: SIGILL (ILL_ILLOPC) at 0x", 0), 0u) << lines[0];
	char expect[256];
	std::snprintf(expect, sizeof(expect),
		"  pc 0x%016llx: microVU1 +0x00000000 (shared code), code area +0x%08x, instruction 00000000",
		static_cast<unsigned long long>(region), HostMemoryMap::mVU1recOffset);
	EXPECT_EQ(lines[2], expect);
	EXPECT_EQ(lines[3], "  block: none found");
	EXPECT_EQ(lines[4].rfind("  VU1 TPC 0x", 0), 0u) << lines[4];
	EXPECT_EQ(lines.back(), "Aborting application.");
}

// In C++ code: the module line, and SIGILL kills it.
TEST(RecCrashReport, AnIllegalInstructionInCppCodeNamesTheModule)
{
	const Crash c = RunCrash(ExecuteUdf);
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGILL);
	EXPECT_NE(c.err.find("Aborting application."), std::string::npos) << c.err;

	const std::vector<std::string> lines = Lines(c.log);
	ASSERT_GE(lines.size(), 3u) << c.log;
	EXPECT_EQ(lines[0].rfind("Host fault: SIGILL (ILL_ILLOPC) at 0x", 0), 0u) << lines[0];
	EXPECT_NE(lines[2].find(": not in recompiled code (" + BaseName(ExePath()) + "+0x"), std::string::npos) << lines[2];
	EXPECT_EQ(lines.back(), "Aborting application.");
}

TEST(RecCrashReport, ASentSigillIsSentAgain)
{
	const Crash c = RunCrash([] { raise(SIGILL); });
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGILL);
	// A sent signal has no fault address; the report says who sent it.
	const std::vector<std::string> lines = Lines(c.log);
	ASSERT_GE(lines.size(), 1u) << c.log;
	EXPECT_EQ(lines[0].rfind("Host fault: SIGILL (SI_TKILL) at 0x", 0), 0u) << lines[0];
}

TEST(RecCrashReport, OurHandlersHandingASigillToEachOtherDoNotLoop)
{
	const Crash c = RunCrash([] {
		CrashHandler::Install();
		ExecuteUdf();
	});
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGILL);
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
