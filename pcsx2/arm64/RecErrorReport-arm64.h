// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the arm64 recompilers write to the log when they stop on an error they
// detect themselves: a jump into a page with no mapping, or a register jump or
// an ERET to an address that is not a multiple of 4. The dialog says only that; the log
// gets what is needed to find out why: the jump that went wrong, the
// registers, the code around it, and for a jump into nothing, whether the
// guest ran off the end of memory and what its stack holds.
//
// Error paths only. Nothing here runs while the guest runs normally.

#pragma once

#include "common/Pcsx2Defs.h"

#include <optional>
#include <string>
#include <vector>

namespace RecErrorReport
{
	enum class Cpu : u8
	{
		EE,
		IOP,
	};

	enum class Kind : u8
	{
		UnmappedPage,
		UnalignedJump,
		UnalignedEret,
	};

	struct Word
	{
		u32 addr = 0;
		u32 value = 0;
		bool readable = false;
	};

	// The JR or JALR that jumped to an unaligned address.
	struct Jump
	{
		u32 pc = 0; // of the jump instruction
		u32 rs = 0; // register index it read
		u32 target = 0; // what it read there, before its delay slot ran
	};

	// The ERET that returned to an unaligned address.
	struct Eret
	{
		u32 pc = 0; // of the ERET
		bool from_error_epc = false; // ErrorEPC (Status.ERL was set), else EPC
	};

	// Which register an ERET just took the pc from, worked out from the state
	// it left: ERET with Status.ERL set clears ERL and leaves EXL, without it
	// clears EXL. So EXL still set means ErrorEPC. With EXL clear, either: then
	// the register holding the pc says, and when both hold it the answer is
	// EPC, which is right for the value if not for the reason.
	bool EretUsedErrorEpc(u32 status_after, u32 pc, u32 epc, u32 error_epc);

	struct Facts
	{
		Cpu cpu = Cpu::EE;
		Kind kind = Kind::UnmappedPage;
		u32 pc = 0; // where the guest was going

		// Known when the jump itself found the target unaligned; not when the
		// recompiler found the pc already unaligned on entry.
		std::optional<Jump> jump;
		std::optional<Eret> eret;

		u64 gpr[32] = {}; // the IOP's are 32 bits
		u64 hi = 0;
		u64 lo = 0;
		u32 status = 0;
		u32 cause = 0;
		u32 epc = 0;
		u32 error_epc = 0; // EE only
		u64 cycle = 0;
		u32 frame = 0;

		// Around the jump or the ERET, or the words before the pc.
		std::vector<Word> code;

		// Jump into an unmapped page.
		const char* end_of_region = nullptr; // the region pc is the end of, if any
		u32 zero_words_before = 0; // zero words (NOPs) immediately before pc
		std::vector<Word> stack; // words from $sp up
	};

	// One word of the EE's or the IOP's RAM or ROM, by guest address; false for
	// an address with nothing behind it. No allocation and no locks, so the
	// crash post-mortem (RecCrashReport-arm64.h) reads guest memory with these
	// too.
	bool ReadEeWord(u32 addr, u32* value);
	bool ReadIopWord(u32 addr, u32* value);

	// The facts as lines of text, without a trailing newline.
	std::string Format(const Facts& facts);

	// The facts read from the CPU's registers and memory. Reads RAM and ROM
	// only, so an address with nothing behind it is reported unreadable, not
	// faulted on. The EE's pinned registers must already be in cpuRegs.
	Facts CaptureEe(Kind kind, const std::optional<Jump>& jump, const std::optional<Eret>& eret = std::nullopt);
	Facts CaptureIop(Kind kind, const std::optional<Jump>& jump);

	// Captures, formats and writes the facts to the log with Console.Error,
	// once per (cpu, pc) and for at most kMaxLogged errors in all: a Resume
	// meets the same pc again, through the check on entry.
	static constexpr u32 kMaxLogged = 8;
	void LogEe(Kind kind, const std::optional<Jump>& jump);
	void LogEeEret(u32 eret_pc);
	void LogIop(Kind kind, const std::optional<Jump>& jump);

#ifdef PCSX2_RECOMPILER_TESTS
	// The last text LogEe or LogIop wrote, emptied by the call; and a way to
	// forget which errors were logged.
	std::string TakeLoggedForTests();
	void ResetForTests();
#endif
} // namespace RecErrorReport
