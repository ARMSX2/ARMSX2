// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the process writes to the log when it takes a SIGSEGV or SIGBUS that
// nothing handles (PageFaultHandler gave up on it), before the signal kills
// it: the signal, the thread, where the host pc is (which recompiler's code,
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
//   - the block lists (BASEBLOCKEX arrays, microVU's current program), only
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
// The first line names the signal and its si_code ("si_code N" when the code
// has no name) and gives the fault address. The thread line has the kernel's
// thread id and name. The pc line, for a pc outside the code area, ends
// "not in recompiled code" and has no instruction. Otherwise it gives the
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
// microVU blocks record no size, so for microVU it names the block entry
// nearest below the pc in the program being run, and a TPC line follows:
//   block: VU1 0x0120, nearest entry below the pc in program 3 (start 0x0000), host 0x0000000118d030c0 +0x00000040
//   VU1 TPC 0x01a8 as last stored
// The guest register, guest pc and guest code lines are there only for EE and
// IOP code; the IOP's registers are 32 bits and its header is "IOP registers:".
// The pinned EE registers are read from the host registers, as they are
// authoritative while EE code runs; the rest come from cpuRegs, as last stored.

#pragma once

#include "arm64/RecErrorReport-arm64.h"

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

	// The microVU block entry nearest below a host pc, in the current program.
	struct VuHit
	{
		bool found = false;
		int program = -1; // -1: no current program
		u32 program_start = 0;
		u32 block_pc = 0;
		uptr host_start = 0;
		u32 tpc = 0;
	};

	static constexpr u32 kMaxCodeWords = 16;

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

		RecErrorReport::Word code[kMaxCodeWords];
		u32 code_count = 0;
	};

	// Receives the report a line at a time, newline included.
	using Sink = void (*)(void* context, const char* text, size_t length);

	// Writes the report in the format above. Allocates nothing.
	void Write(const Report& report, Sink sink, void* context);

	// The layout of the live code area and recompilers.
	Layout CurrentLayout();

	// Fills a report from a signal handler's arguments and the live
	// recompilers.
	void Capture(int signal, const void* siginfo, const void* ucontext, const Layout& layout, Report* report);

	// The PageFaultHandler reporter: captures and writes the report, the first
	// time it is called in the process.
	void OnUnhandledFault(int signal, void* siginfo, void* ucontext);

	// Registers OnUnhandledFault with PageFaultHandler.
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
