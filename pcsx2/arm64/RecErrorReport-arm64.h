// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the arm64 recompilers write to the log when they stop on an error they
// detect themselves: a jump into a page with no mapping, or a register jump to
// an address that is not a multiple of 4. The dialog says only that; the log
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

	struct Facts
	{
		Cpu cpu = Cpu::EE;
		Kind kind = Kind::UnmappedPage;
		u32 pc = 0; // where the guest was going

		// Known when the jump itself found the target unaligned; not when the
		// recompiler found the pc already unaligned on entry.
		std::optional<Jump> jump;

		u64 gpr[32] = {}; // the IOP's are 32 bits
		u64 hi = 0;
		u64 lo = 0;
		u32 status = 0;
		u32 cause = 0;
		u32 epc = 0;
		u32 error_epc = 0; // EE only
		u64 cycle = 0;
		u32 frame = 0;

		// Around the jump, or the words before the pc.
		std::vector<Word> code;

		// Jump into an unmapped page.
		const char* end_of_region = nullptr; // the region pc is the end of, if any
		u32 zero_words_before = 0; // zero words (NOPs) immediately before pc
		std::vector<Word> stack; // words from $sp up
	};

	// The facts as lines of text, without a trailing newline.
	std::string Format(const Facts& facts);

	// The facts read from the CPU's registers and memory. Reads RAM and ROM
	// only, so an address with nothing behind it is reported unreadable, not
	// faulted on. The EE's pinned registers must already be in cpuRegs.
	Facts CaptureEe(Kind kind, const std::optional<Jump>& jump);
	Facts CaptureIop(Kind kind, const std::optional<Jump>& jump);

	// Captures, formats and writes the facts to the log with Console.Error,
	// once per (cpu, kind, pc) and for at most kMaxLogged errors in all: a
	// Resume repeats the same error.
	static constexpr u32 kMaxLogged = 8;
	void LogEe(Kind kind, const std::optional<Jump>& jump);
	void LogIop(Kind kind, const std::optional<Jump>& jump);

#ifdef PCSX2_RECOMPILER_TESTS
	// The last text LogEe or LogIop wrote, emptied by the call; and a way to
	// forget which errors were logged.
	std::string TakeLoggedForTests();
	void ResetForTests();
#endif
} // namespace RecErrorReport
