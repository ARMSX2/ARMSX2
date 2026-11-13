// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the process writes to the log when it takes a SIGSEGV or SIGBUS that
// nothing handles (PageFaultHandler gave up on it), or a SIGILL, before the
// signal kills it: the signal, the thread, where the host pc is (which recompiler's code,
// and which part of it), the recompiled block it is in, the host registers,
// and for EE or IOP code the guest registers and the block's guest code.
// RecErrorReport-arm64.h is the counterpart for errors the recompilers detect
// themselves.
//
// It runs inside the signal handler of a dying process, once per process (a
// second fault, on any thread, writes nothing more), and is async-signal-safe:
// it formats into a buffer on the stack and writes each line with write(2) to
// the log file's descriptor (stderr when there is no log file). No allocation,
// no locks, no stdio. What it reads:
//   - the ucontext and siginfo the kernel passed;
//   - the code area's layout: SysMemory's code pointer and pointers each
//     recompiler sets at reserve and reset;
//   - the faulting instruction word, inside the code area, which is mapped
//     readable as a whole;
//   - the loaded-module table ModuleMap keeps, for a pc outside the code area;
//   - the block lists (BASEBLOCKEX arrays, microVU's programs), only
//     for a pc in that recompiler's code, so only on the thread that compiles
//     into them, which was running generated code and not changing them;
//   - cpuRegs, psxRegs, vuRegs, and guest RAM and ROM through RecErrorReport's
//     region tables.
// The first three lines are written before the block lookup, the one part
// that walks heap data. A fault inside the report itself takes the same way
// out as any other (the report is not started twice), so the tombstone then
// shows that fault, and the log keeps what was written before it.
//
// The format. The bug-report tooling parses it, so a change here is a change
// there; rec_crash_report_tests.cpp holds a complete example, line for line.
// The lines, in order (shown here after "// "):
//
// Host fault: SIGSEGV (SEGV_MAPERR) at 0x0000000000000008
//   thread 4242 "CPU Thread", signal 11, si_code 1
//   pc 0x0000000111d01234: EE recompiler +0x00001234 (blocks), code area +0x00001234, instruction f9400021
//   block: EE 0x00123450, 12 instructions, host 0x0000000111d01200 +0x00000034 of 0x00000180
//   lr 0x0000aaaadd71e9cc: not in recompiled code (armsx2-qt+0x64e9cc, build-id <all of it, in hex>)
//   host registers:
//       x0 0000000000000000   x1 0000000000000001   x2 0000000000000002   x3 0000000000000003
//     (8 such lines: x0 to x30, then sp; names right-aligned in 4 columns)
//     pstate 0000000060000000
//   EE registers (pinned ones from the host registers):
//     zero 0000000000000000   at 0000000000000000   v0 0000000000000001   v1 0000000000000000
//     (8 such lines, as in RecErrorReport's post-mortem)
//      hi 0000000000000011   lo 0000000000000022
//   EE pc 0x00123450 as last stored, frame 1234
//   EE code at the block:
//     00123450  27bdfff0
//     (up to 16 words; ???????? for a word with no memory behind it)
// Aborting application.
//
// The report is at most 16 KiB (kMaxReportBytes); its parts are bounded well
// below that. Past the cap, one line reads "  (report cut short here, at its
// size cap)", the rest is dropped, and "Aborting application." still ends it.
//
// The first line names the signal and its si_code ("si_code N" when the code
// has no name) and gives the fault address; for SIGILL that is the address of
// the instruction, the pc, as the kernel reports it (ILL_ILLOPC for an
// undefined instruction). The thread line has the kernel's
// thread id and name. The pc line, for a pc outside the code area, ends
// "not in recompiled code" and has no instruction; when the pc is in a loaded
// module it goes on to name it, the pc's ELF address in it (what addr2line
// and llvm-symbolizer take: the pc minus the module's load bias) and its GNU
// build id, the hex of the note's bytes, all of them (40 digits for the usual
// 20-byte id):
//   pc 0x0000007a1b2c3d40: not in recompiled code (libvulkan_freedreno.so+0x1c3d40, build-id 5f0c3a9e0102030405060708090a0b0c0d0e0f10)
// or "no build-id" for a module without one. The offset has as many digits as
// it needs. The module table is refreshed at startup, after each library the
// emulator loads (DynamicLibrary) and after the Vulkan instance and device are
// made; a library loaded otherwise since the last refresh is not named.
// Otherwise the pc line gives the
// region (EE recompiler, IOP recompiler, VIF0 unpack recompiler, VIF1 unpack
// recompiler, microVU0, microVU1, VIF unpack functions, GS software renderer)
// with the pc's offset into it, for a recompiler the part in parentheses
// (shared code: the dispatchers and stubs made at reset; blocks; cold side
// exits, EE only; constant pool), the offset into the whole code area, and the
// instruction word at the pc.
//
// The block line is there for EE, IOP and microVU code. For EE and IOP it
// names the block whose host code holds the pc: its guest start address as
// the recompiler keys it (physical for the EE), its length in instructions,
// and the pc's offset into its host code; or it reads "block: none found".
//
// For microVU, the lines after the pc line are:
//   block: VU1 0x0120, nearest entry below the pc in program 3 (start 0x0000), host 0x0000000118d030c0 +0x00000040 of 0x00000180
//   VU1 TPC 0x01a8 as last stored
//   VU1 block extent: host 0x0000000118d030c0 +0x00000040 of 0x00000180, guest 0x0120 to 0x0178 (12 pairs), holds the pc
//   VU1 block map, program 3 (start 0x0000), by host address; > the block named, + another block holding the pc:
//     (5 lower blocks not shown)
//   + 0x0000  host 0x0000000118d03000  size 0x00000400, 4 pairs
//   > 0x0120  host 0x0000000118d030c0  size 0x00000180, 12 pairs
//     0x0040  host 0x0000000118d03400  size not recorded
//     (2 higher blocks not shown)
// and between the extent line and the map, the block's microcode, when a
// block is named:
//   VU1 code at the block:
//     0120  8000033c
//     0124  000002ff  micro memory now 000002fe
//     0128  4a0002bc  from micro memory, not in the program's copy
//     (one word a line, each pair's lower instruction first; the block's
//     length, at most 16 words, cut at the end of micro memory)
// A word comes from the program's copy of micro memory, the bytes the program
// was compiled from, which holds its ranges only. When micro memory has since
// changed, the line adds what it holds now; a word outside the copy is read
// from micro memory and says so.
// Each block records its extent when it is compiled (microBlock::extent): the
// host bytes its compile produced and its guest instruction pairs. A block's
// host extent includes the blocks compiled inside its compile (the not-taken
// path of a branch is compiled in place), so extents nest. The block named is
// the innermost one holding the pc, in the program being run or failing that
// in another program of the VU, which the extent line then names; with no
// block holding the pc it is the entry nearest below the pc in the program
// being run, and the extent line reads
//   VU1 block extent: pc is not inside any block of program 3 or of another program of VU1
// adding "(N blocks of program 3 have no recorded size)" when some have none:
// blocks placed by vu-aot record none, so they are never said to hold the pc.
// The block line keeps the wording the triage parser matches; " of 0x..." is
// there only when the block's size is recorded, and the line is "block: none
// found" when the program has no block at or below the pc. With no current
// program, the extent line reads "no current program" and there is no map.
// The map lists up to 48 blocks of the program named, by host address: the
// ones nearest the pc, up to 8 of them above it. Guest addresses are byte
// addresses in micro memory; "to" names the last pair's address.
// The lr line is x30, the caller's return address, with any pointer-
// authentication code taken off. It reads like the pc line, without the
// instruction: the recompiler region and part, or "not in recompiled code"
// with the module, offset and build id. Its block is not looked up. It comes
// after the block lines and everything the microVU section adds, just before
// "host registers:"; a report built without the registers has none (the
// pinned examples in rec_crash_report_tests.cpp, except the lr test).
//
// The guest register, guest pc and guest code lines are there for EE and IOP
// code only; the IOP's registers are 32 bits and its header is "IOP registers:".
// The pinned EE registers are read from the host registers, as they are
// authoritative while EE code runs; the rest come from cpuRegs, as last stored.

#pragma once

#include "arm64/RecErrorReport-arm64.h"

#include "common/ModuleMap.h"
#include "common/Pcsx2Defs.h"

#include <cstddef>

namespace RecCrashReport
{
	// Where a recompiler's parts start, as absolute addresses; 0 when not known
	// (not reserved or reset yet). Its shared code starts at the region start.
	struct RecSlices
	{
		uptr blocks_start = 0; // end of the shared code
		uptr cold_start = 0; // EE only
		uptr pool_start = 0;
	};

	struct Layout
	{
		uptr code_base = 0; // the code area (HostMemoryMap), 0 when there is none
		RecSlices ee;
		RecSlices iop;
		uptr vu_blocks_start[2] = {}; // microVU: end of the shared code
	};

	enum class Area : u8
	{
		None,
		EeShared,
		EeBlocks,
		EeColdExits,
		EeConstants,
		IopShared,
		IopBlocks,
		IopConstants,
		Vif0Dynarec,
		Vif1Dynarec,
		Vu0Shared,
		Vu0Blocks,
		Vu1Shared,
		Vu1Blocks,
		VifUnpack,
		SwRenderer,
	};

	struct Where
	{
		Area area = Area::None;
		uptr region_offset = 0; // from the start of the region (EE recompiler, ...)
		uptr code_offset = 0; // from the start of the code area
	};

	Where Classify(const Layout& layout, uptr pc);

	// A code address with any pointer-authentication code taken off (XPACLRI).
	uptr StripPointerAuth(uptr address);
	const char* RegionName(Area area); // "EE recompiler", ...; null for None
	const char* PartName(Area area); // "blocks", ...; null for a region with one part

	enum class Cpu : u8
	{
		None,
		EE,
		IOP,
		VU0,
		VU1,
	};

	// The EE or IOP block holding a host pc.
	struct BlockHit
	{
		u32 guest_pc = 0;
		u32 guest_words = 0;
		uptr host_start = 0;
		u32 host_size = 0;
	};

	// One microVU block: its guest start (a byte address in micro memory), where
	// its host code starts, and its extent (0 when not recorded).
	struct VuBlock
	{
		uptr host_start = 0;
		u32 host_size = 0;
		u16 guest_pc = 0;
		u16 guest_pairs = 0;
	};

	static constexpr u32 kMaxVuWindow = 48;
	static constexpr u32 kMaxVuWindowAbove = 8;
	static constexpr u32 kMaxCodeWords = 16;

	// The microVU block holding a host pc: the innermost block of the program
	// being run whose extent holds it, else the one of any other program of that
	// VU, else the entry nearest below the pc in the program being run. And the
	// blocks around the pc, by host address, from the program named.
	struct VuHit
	{
		bool found = false; // block is set
		bool contains = false; // and its extent holds the pc
		int program = -1; // the program block is in; -1: no current program
		u32 program_start = 0;
		int current_program = -1; // the program being run
		VuBlock block;
		u32 tpc = 0;

		// Up to kMaxVuWindow blocks of `program`, in host address order: the ones
		// nearest the pc, at most kMaxVuWindowAbove of them above it.
		VuBlock window[kMaxVuWindow];
		u32 window_count = 0;
		u32 blocks_below = 0; // blocks of the program below the window
		u32 blocks_above = 0; // and above it
		u32 blocks_unsized = 0; // blocks of the program with no recorded extent

		// The first kMaxCodeWords words of the block named, from the program's
		// copy of micro memory (what the program was compiled from); bit i of the
		// mask is set when the copy holds word i (it holds the program's ranges
		// only).
		u32 program_code[kMaxCodeWords] = {};
		u32 program_code_mask = 0;
	};
	static_assert(kMaxCodeWords <= 32, "VuHit::program_code_mask has a bit per word");

	// A word of VU microcode at the block: from the program's copy when it holds
	// it, and as micro memory has it now.
	struct VuWord
	{
		u32 addr = 0; // byte address in micro memory
		u32 program = 0;
		u32 live = 0;
		bool in_program = false;
		bool live_readable = false;
	};

	// Picks VuHit's block and window out of a program's blocks, given one at a
	// time. FindVuBlock's logic, apart from the walk over microVU's lists.
	class VuBlockScan
	{
	public:
		explicit VuBlockScan(uptr host_pc) : m_pc(host_pc) {}
		void Add(const VuBlock& block);
		bool Contains() const { return m_inner_found; }
		// The innermost block holding the pc, else the nearest entry below it.
		bool Best(VuBlock* block) const;
		void FillWindow(VuHit* hit) const;

	private:
		static bool Holds(const VuBlock& b, uptr pc) { return b.host_size && pc - b.host_start < b.host_size; }

		uptr m_pc;
		bool m_inner_found = false;
		VuBlock m_inner;
		bool m_nearest_found = false;
		VuBlock m_nearest;
		u32 m_unsized = 0;
		// The nearest blocks at or below the pc, highest first, and above it,
		// lowest first.
		VuBlock m_below[kMaxVuWindow];
		u32 m_below_count = 0;
		u32 m_below_total = 0;
		VuBlock m_above[kMaxVuWindow];
		u32 m_above_count = 0;
		u32 m_above_total = 0;
	};

	// The cap on the whole report. Its parts are bounded so it stays far below;
	// past it, lines are dropped, one line says so, and the last line is still
	// written.
	static constexpr size_t kMaxReportBytes = 16 * 1024;

	struct Report
	{
		int signal = 0;
		int si_code = 0;
		uptr fault_address = 0;
		long tid = 0;
		char thread_name[17] = {};

		uptr pc = 0;
		Where where;
		bool have_instruction = false;
		u32 instruction = 0;
		bool have_module = false; // a pc outside the code area, in a known module
		ModuleMap::Hit module;

		bool have_lr = false; // x30, written as the lr line
		uptr lr = 0; // without a pointer-authentication code
		Where lr_where;
		bool lr_have_module = false;
		ModuleMap::Hit lr_module;

		Cpu block_cpu = Cpu::None; // whose block line to write, if any
		bool block_found = false;
		BlockHit block;
		VuHit vu;

		bool have_host_registers = false;
		u64 x[31] = {};
		u64 sp = 0;
		u64 pstate = 0;

		Cpu guest = Cpu::None; // EE or IOP, whose registers follow
		u64 gpr[32] = {};
		u64 hi = 0;
		u64 lo = 0;
		u32 guest_pc = 0;
		u32 frame = 0;

		// The guest code at the block: EE or IOP instructions.
		RecErrorReport::Word code[kMaxCodeWords];
		u32 code_count = 0;
		// For microVU, the microcode at the block.
		VuWord vu_code[kMaxCodeWords];
		u32 vu_code_count = 0;
	};

	// Receives the report a line at a time, newline included.
	using Sink = void (*)(void* context, const char* text, size_t length);

	// Writes the report in the format above, at most `limit` bytes of it (the
	// last line is always written). Allocates nothing.
	void Write(const Report& report, Sink sink, void* context, size_t limit = kMaxReportBytes);

	// Up to kMaxCodeWords words of microcode from the named block's guest pc:
	// the block's length (all 16 when it is not recorded), cut at the end of
	// micro memory. Each from the program's copy in hit.program_code where it
	// holds the word, and from micro memory (null: none to read).
	u32 ReadVuCode(const u8* micro, u32 micro_size, const VuHit& hit, VuWord* code);

	// The layout of the live code area and recompilers.
	Layout CurrentLayout();

	// Fills a report from a signal handler's arguments and the live
	// recompilers.
	void Capture(int signal, const void* siginfo, const void* ucontext, const Layout& layout, Report* report);

	// The PageFaultHandler reporter: captures and writes the report, the first
	// time it is called in the process.
	void OnUnhandledFault(int signal, void* siginfo, void* ucontext);

	// Registers OnUnhandledFault with PageFaultHandler, and takes the first
	// snapshot of the loaded modules (common/ModuleMap.h).
	void Install();

	// Defined by the recompilers, for Capture and CurrentLayout.
	RecSlices EeSlices();
	bool FindEeBlock(uptr host_pc, BlockHit* hit);
	void EePinnedRegisters(const u64 host_x[31], u64 gpr[32]);
	RecSlices IopSlices();
	bool FindIopBlock(uptr host_pc, BlockHit* hit);
	uptr VuBlocksStart(int vu);
	void FindVuBlock(int vu, uptr host_pc, VuHit* hit);

#ifdef PCSX2_RECOMPILER_TESTS
	// Lets OnUnhandledFault write a report again.
	void ResetForTests();
#endif
} // namespace RecCrashReport
