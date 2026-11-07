// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "arm64/RecErrorReport-arm64.h"

#include "Counters.h"
#include "DebugTools/Debug.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <utility>

namespace RecErrorReport
{
	namespace
	{
		// The disassembler writes static state, and the EE and the IOP can stop
		// on separate threads.
		std::mutex s_mutex;

		constexpr u32 kCodeBefore = 16; // words before the jump
		constexpr u32 kCodeAfter = 2; // words after it
		constexpr u32 kStackWords = 16;

		const char* RegName(u32 index)
		{
			return R5900::GPR_REG[index & 31];
		}

		// The R5900 disassembler for both CPUs, as the debugger's IOP view does
		// (R3000DebugInterface::disasm): the IOP's own one prints the live
		// register contents beside each operand.
		std::string Disassemble(u32 addr, u32 word)
		{
			std::string text;
			R5900::disR5900Fasm(text, word, addr, false);
			std::replace(text.begin(), text.end(), '\t', ' ');
			return text;
		}

		const Word* FindWord(const std::vector<Word>& words, u32 addr)
		{
			for (const Word& w : words)
			{
				if (w.addr == addr)
					return &w;
			}
			return nullptr;
		}

		std::string RegValue(Cpu cpu, u64 value)
		{
			return cpu == Cpu::EE ? fmt::format("{:016x}", value) : fmt::format("{:08x}", static_cast<u32>(value));
		}

		// A region of guest memory with code in it, and the segments it shows
		// up in. The same map the recompilers' recLUT is built from.
		struct Region
		{
			const char* name;
			u32 start; // within a segment
			u32 size;
			u32 mask; // applied to the offset into the region: RAM mirrors
			const u8* data;
			const u32* segments;
			size_t segment_count;
		};

		constexpr u32 kEeRamSegments[] = {0x00000000, 0x20000000, 0x30000000, 0x80000000, 0xa0000000, 0xb0000000,
			0xc0000000, 0xd0000000};
		constexpr u32 kRomSegments[] = {0x00000000, 0x80000000, 0xa0000000};

		std::vector<Region> EeRegions()
		{
			if (!eeMem)
				return {};
			return {
				{"RAM", 0, Ps2MemSize::ExposedRam, ~0u, eeMem->Main, kEeRamSegments, std::size(kEeRamSegments)},
				{"the BIOS ROM", 0x1fc00000, Ps2MemSize::Rom, ~0u, eeMem->ROM, kRomSegments, std::size(kRomSegments)},
				{"ROM1", 0x1e000000, Ps2MemSize::Rom1, ~0u, eeMem->ROM1, kRomSegments, std::size(kRomSegments)},
				{"ROM2", 0x1e400000, Ps2MemSize::Rom2, ~0u, eeMem->ROM2, kRomSegments, std::size(kRomSegments)},
			};
		}

		std::vector<Region> IopRegions()
		{
			if (!iopMem || !eeMem)
				return {};
			// IOP RAM shows up mirrored through the first 8 MB, and ROM2 is eight
			// pages (recResetIOP).
			return {
				{"IOP RAM", 0, static_cast<u32>(_8mb), Ps2MemSize::ExposedIopRam - 1, iopMem->Main, kRomSegments, std::size(kRomSegments)},
				{"the BIOS ROM", 0x1fc00000, Ps2MemSize::Rom, ~0u, eeMem->ROM, kRomSegments, std::size(kRomSegments)},
				{"ROM1", 0x1e000000, Ps2MemSize::Rom1, ~0u, eeMem->ROM1, kRomSegments, std::size(kRomSegments)},
				{"ROM2", 0x1e400000, 8 * _64kb, ~0u, eeMem->ROM2, kRomSegments, std::size(kRomSegments)},
			};
		}

		bool ReadWord(const std::vector<Region>& regions, u32 addr, u32* value)
		{
			addr &= ~3u;
			for (const Region& r : regions)
			{
				for (size_t i = 0; i < r.segment_count; i++)
				{
					const u32 off = addr - r.segments[i] - r.start;
					if (off < r.size)
					{
						std::memcpy(value, r.data + (off & r.mask), sizeof(u32));
						return true;
					}
				}
			}
			return false;
		}

		const char* EndOfRegion(const std::vector<Region>& regions, u32 addr)
		{
			for (const Region& r : regions)
			{
				for (size_t i = 0; i < r.segment_count; i++)
				{
					if (addr - r.segments[i] - r.start == r.size)
						return r.name;
				}
			}
			return nullptr;
		}

		std::vector<Word> ReadWords(const std::vector<Region>& regions, u32 first, u32 count)
		{
			std::vector<Word> words;
			for (u32 i = 0; i < count; i++)
			{
				Word w;
				w.addr = first + i * 4;
				w.readable = ReadWord(regions, w.addr, &w.value);
				words.push_back(w);
			}
			return words;
		}

		void CaptureMemory(Facts& f, const std::vector<Region>& regions, u32 sp)
		{
			if (f.jump)
			{
				f.code = ReadWords(regions, f.jump->pc - kCodeBefore * 4, kCodeBefore + 1 + kCodeAfter);
				return;
			}
			if (f.kind != Kind::UnmappedPage)
				return;

			f.end_of_region = EndOfRegion(regions, f.pc);
			u32 value;
			for (u32 addr = (f.pc & ~3u) - 4; ReadWord(regions, addr, &value) && value == 0; addr -= 4)
			{
				if (++f.zero_words_before == Ps2MemSize::TotalRam / 4)
					break;
			}
			// After a run of NOPs the words before the pc say nothing more.
			if (f.zero_words_before < kCodeBefore)
				f.code = ReadWords(regions, (f.pc & ~3u) - kCodeBefore * 4, kCodeBefore);
			f.stack = ReadWords(regions, sp & ~3u, kStackWords);
		}

		struct Key
		{
			Cpu cpu;
			Kind kind;
			u32 pc;
		};
		std::vector<Key> s_logged;

#ifdef PCSX2_RECOMPILER_TESTS
		std::string s_last_logged;
#endif

		bool ShouldLog(Cpu cpu, Kind kind, u32 pc)
		{
			for (const Key& k : s_logged)
			{
				if (k.cpu == cpu && k.kind == kind && k.pc == pc)
					return false;
			}
			if (s_logged.size() >= kMaxLogged)
				return false;
			s_logged.push_back({cpu, kind, pc});
			return true;
		}

		void Write(const std::string& text)
		{
			size_t start = 0;
			while (start <= text.size())
			{
				const size_t end = std::min(text.find('\n', start), text.size());
				Console.Error("%s", text.substr(start, end - start).c_str());
				start = end + 1;
			}
#ifdef PCSX2_RECOMPILER_TESTS
			s_last_logged = text;
#endif
		}
	} // namespace

	std::string Format(const Facts& f)
	{
		const char* const cpu = f.cpu == Cpu::EE ? "EE" : "IOP";
		std::string out = fmt::format("{} recompiler stopped: jump to {} address 0x{:08x}", cpu,
			f.kind == Kind::UnmappedPage ? "unmapped" : "unaligned", f.pc);

		if (f.jump)
		{
			const Jump& j = *f.jump;
			const Word* w = FindWord(f.code, j.pc);
			const std::string insn = (w && w->readable) ? fmt::format(" ({})", Disassemble(j.pc, w->value)) : std::string();
			const char* const rs = RegName(j.rs);
			out += fmt::format("\n  The jump at 0x{:08x}{} read {} = 0x{:08x} before its delay slot; {} now holds {}",
				j.pc, insn, rs, j.target, rs, RegValue(f.cpu, f.gpr[j.rs & 31]));
		}
		else if (f.kind == Kind::UnalignedJump)
		{
			out += "\n  The pc was already unaligned when the recompiler was entered; the jump that set it is not known.";
		}

		if (f.kind == Kind::UnmappedPage)
		{
			if (f.end_of_region)
				out += fmt::format("\n  0x{:08x} is the end of {}: the guest ran off the end of it.", f.pc, f.end_of_region);
			if (f.zero_words_before)
				out += fmt::format("\n  The {} words before it are zero (NOPs), back to 0x{:08x}.", f.zero_words_before,
					(f.pc & ~3u) - f.zero_words_before * 4);
			else
				out += "\n  The word before it is not zero, or not in RAM or ROM.";
			out += fmt::format("\n  ra {}  sp {}", RegValue(f.cpu, f.gpr[31]), RegValue(f.cpu, f.gpr[29]));
			if (!f.stack.empty())
			{
				out += "\n  Stack at sp:";
				for (size_t i = 0; i < f.stack.size(); i += 4)
				{
					out += fmt::format("\n    {:08x} ", f.stack[i].addr);
					for (size_t k = i; k < std::min(i + 4, f.stack.size()); k++)
						out += f.stack[k].readable ? fmt::format(" {:08x}", f.stack[k].value) : std::string(" ????????");
				}
			}
		}

		out += fmt::format("\n  cycle {}, frame {}", f.cycle, f.frame);
		out += fmt::format("\n  Status 0x{:08x}  Cause 0x{:08x}  EPC 0x{:08x}", f.status, f.cause, f.epc);
		if (f.cpu == Cpu::EE)
			out += fmt::format("  ErrorEPC 0x{:08x}", f.error_epc);

		out += "\n  Registers:";
		for (u32 i = 0; i < 32; i += 4)
		{
			out += "\n   ";
			for (u32 k = i; k < i + 4; k++)
				out += fmt::format(" {:>4} {}", RegName(k), RegValue(f.cpu, f.gpr[k]));
		}
		out += fmt::format("\n     hi {}   lo {}", RegValue(f.cpu, f.hi), RegValue(f.cpu, f.lo));

		if (!f.code.empty())
		{
			out += f.jump ? "\n  Code (> is the jump):" : "\n  Code before the pc:";
			for (const Word& w : f.code)
			{
				const char mark = (f.jump && w.addr == f.jump->pc) ? '>' : ' ';
				if (w.readable)
					out += fmt::format("\n  {} {:08x}  {:08x}  {}", mark, w.addr, w.value, Disassemble(w.addr, w.value));
				else
					out += fmt::format("\n  {} {:08x}  ????????", mark, w.addr);
			}
		}
		return out;
	}

	Facts CaptureEe(Kind kind, const std::optional<Jump>& jump)
	{
		Facts f;
		f.cpu = Cpu::EE;
		f.kind = kind;
		f.pc = cpuRegs.pc;
		f.jump = jump;
		for (u32 i = 0; i < 32; i++)
			f.gpr[i] = cpuRegs.GPR.r[i].UD[0];
		f.hi = cpuRegs.HI.UD[0];
		f.lo = cpuRegs.LO.UD[0];
		f.status = cpuRegs.CP0.n.Status.val;
		f.cause = cpuRegs.CP0.n.Cause;
		f.epc = cpuRegs.CP0.n.EPC;
		f.error_epc = cpuRegs.CP0.n.ErrorEPC;
		f.cycle = cpuRegs.cycle;
		f.frame = g_FrameCount;
		CaptureMemory(f, EeRegions(), cpuRegs.GPR.n.sp.UL[0]);
		return f;
	}

	Facts CaptureIop(Kind kind, const std::optional<Jump>& jump)
	{
		Facts f;
		f.cpu = Cpu::IOP;
		f.kind = kind;
		f.pc = psxRegs.pc;
		f.jump = jump;
		for (u32 i = 0; i < 32; i++)
			f.gpr[i] = psxRegs.GPR.r[i];
		f.hi = psxRegs.GPR.n.hi;
		f.lo = psxRegs.GPR.n.lo;
		f.status = psxRegs.CP0.n.Status;
		f.cause = psxRegs.CP0.n.Cause;
		f.epc = psxRegs.CP0.n.EPC;
		f.cycle = psxRegs.cycle;
		// The EE thread's counter; on the IOP thread it may be a frame out.
		f.frame = g_FrameCount;
		CaptureMemory(f, IopRegions(), psxRegs.GPR.n.sp);
		return f;
	}

	void LogEe(Kind kind, const std::optional<Jump>& jump)
	{
		std::lock_guard lock(s_mutex);
		if (ShouldLog(Cpu::EE, kind, cpuRegs.pc))
			Write(Format(CaptureEe(kind, jump)));
	}

	void LogIop(Kind kind, const std::optional<Jump>& jump)
	{
		std::lock_guard lock(s_mutex);
		if (ShouldLog(Cpu::IOP, kind, psxRegs.pc))
			Write(Format(CaptureIop(kind, jump)));
	}

#ifdef PCSX2_RECOMPILER_TESTS
	std::string TakeLoggedForTests()
	{
		std::lock_guard lock(s_mutex);
		return std::exchange(s_last_logged, {});
	}

	void ResetForTests()
	{
		std::lock_guard lock(s_mutex);
		s_logged.clear();
		s_last_logged.clear();
	}
#endif
} // namespace RecErrorReport
