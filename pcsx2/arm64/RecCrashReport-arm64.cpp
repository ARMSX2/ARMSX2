// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "arm64/RecCrashReport-arm64.h"

// Linux and Android only: it reads Linux's ucontext_t, and only
// LnxHostSys.cpp's page fault handler calls a reporter.
#if defined(__linux__)

#include "Counters.h"
#include "DebugTools/Debug.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "VU.h"

#include "common/Console.h"
#include "common/HostSys.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace RecCrashReport
{
	namespace
	{
		std::atomic<bool> s_reported{false};

		// One line of text in a buffer on the stack. Too long a line is cut
		// short, never allocated for.
		class Line
		{
		public:
			Line(Sink sink, void* context)
				: m_sink(sink)
				, m_context(context)
			{
			}

			Line& Str(const char* s)
			{
				while (*s)
					Put(*s++);
				return *this;
			}

			// Right-aligned in width columns.
			Line& Str(const char* s, size_t width)
			{
				for (size_t len = std::strlen(s); len < width; len++)
					Put(' ');
				return Str(s);
			}

			Line& Hex(u64 value, int digits)
			{
				for (int i = digits - 1; i >= 0; i--)
					Put("0123456789abcdef"[(value >> (i * 4)) & 0xf]);
				return *this;
			}

			Line& Dec(s64 value)
			{
				if (value < 0)
				{
					Put('-');
					return Dec(static_cast<u64>(-(value + 1)) + 1);
				}
				return Dec(static_cast<u64>(value));
			}

			Line& Dec(u64 value)
			{
				char digits[20];
				int n = 0;
				do
				{
					digits[n++] = static_cast<char>('0' + value % 10);
					value /= 10;
				} while (value);
				while (n)
					Put(digits[--n]);
				return *this;
			}

			Line& Dec(u32 value) { return Dec(static_cast<u64>(value)); }
			Line& Dec(int value) { return Dec(static_cast<s64>(value)); }

			void End()
			{
				m_buf[m_len++] = '\n';
				m_sink(m_context, m_buf, m_len);
				m_len = 0;
			}

		private:
			void Put(char c)
			{
				if (m_len < sizeof(m_buf) - 1)
					m_buf[m_len++] = c;
			}

			Sink m_sink;
			void* m_context;
			char m_buf[256];
			size_t m_len = 0;
		};

		const char* SignalName(int signal)
		{
			switch (signal)
			{
				// clang-format off
				case SIGSEGV: return "SIGSEGV";
				case SIGBUS: return "SIGBUS";
				case SIGILL: return "SIGILL";
				case SIGFPE: return "SIGFPE";
				case SIGTRAP: return "SIGTRAP";
				default: return nullptr;
				// clang-format on
			}
		}

		const char* CodeName(int signal, int code)
		{
			switch (code)
			{
				// clang-format off
				case SI_USER: return "SI_USER";
				case SI_QUEUE: return "SI_QUEUE";
				case SI_TKILL: return "SI_TKILL";
				default: break;
				// clang-format on
			}
			if (signal == SIGSEGV)
			{
				switch (code)
				{
					// clang-format off
					case 1: return "SEGV_MAPERR";
					case 2: return "SEGV_ACCERR";
					case 3: return "SEGV_BNDERR";
					case 4: return "SEGV_PKUERR";
					case 8: return "SEGV_MTEAERR";
					case 9: return "SEGV_MTESERR";
					default: return nullptr;
					// clang-format on
				}
			}
			if (signal == SIGBUS)
			{
				switch (code)
				{
					// clang-format off
					case 1: return "BUS_ADRALN";
					case 2: return "BUS_ADRERR";
					case 3: return "BUS_OBJERR";
					default: return nullptr;
					// clang-format on
				}
			}
			return nullptr;
		}

		const char* CpuName(Cpu cpu)
		{
			switch (cpu)
			{
				// clang-format off
				case Cpu::EE: return "EE";
				case Cpu::IOP: return "IOP";
				case Cpu::VU0: return "VU0";
				case Cpu::VU1: return "VU1";
				default: return "?";
				// clang-format on
			}
		}

		struct RegionInfo
		{
			u32 offset;
			u32 size;
		};

		RegionInfo RegionOf(Area area)
		{
			switch (area)
			{
				case Area::EeShared:
				case Area::EeBlocks:
				case Area::EeColdExits:
				case Area::EeConstants:
					return {HostMemoryMap::EErecOffset, HostMemoryMap::EErecSize};
				case Area::IopShared:
				case Area::IopBlocks:
				case Area::IopConstants:
					return {HostMemoryMap::IOPrecOffset, HostMemoryMap::IOPrecSize};
				case Area::Vif0Dynarec:
					return {HostMemoryMap::VIF0recOffset, HostMemoryMap::VIF0recSize};
				case Area::Vif1Dynarec:
					return {HostMemoryMap::VIF1recOffset, HostMemoryMap::VIF1recSize};
				case Area::Vu0Shared:
				case Area::Vu0Blocks:
					return {HostMemoryMap::mVU0recOffset, HostMemoryMap::mVU0recSize};
				case Area::Vu1Shared:
				case Area::Vu1Blocks:
					return {HostMemoryMap::mVU1recOffset, HostMemoryMap::mVU1recSize};
				case Area::VifUnpack:
					return {HostMemoryMap::VIFUnpackRecOffset, HostMemoryMap::VIFUnpackRecSize};
				case Area::SwRenderer:
					return {HostMemoryMap::SWrecOffset, HostMemoryMap::SWrecSize};
				default:
					return {0, 0};
			}
		}

		// The part of a recompiler's region a pc is in, from the boundaries it
		// recorded. Unknown boundaries (0) are passed over.
		Area Part(uptr pc, uptr blocks_start, uptr cold_start, uptr pool_start, Area shared, Area blocks, Area cold,
			Area pool)
		{
			if (pool_start && pc >= pool_start)
				return pool;
			if (cold_start && pc >= cold_start)
				return cold;
			if (blocks_start && pc < blocks_start)
				return shared;
			return blocks;
		}

		void WriteRegisters(const Report& r, Sink sink, void* context)
		{
			const bool ee = r.guest == Cpu::EE;
			const int digits = ee ? 16 : 8;
			Line line(sink, context);
			line.Str(ee ? "  EE registers (pinned ones from the host registers):" : "  IOP registers:").End();
			for (u32 i = 0; i < 32; i += 4)
			{
				line.Str("   ");
				for (u32 k = i; k < i + 4; k++)
					line.Str(" ").Str(R5900::GPR_REG[k], 4).Str(" ").Hex(r.gpr[k], digits);
				line.End();
			}
			line.Str("     hi ").Hex(r.hi, digits).Str("   lo ").Hex(r.lo, digits).End();
			line.Str("  ").Str(CpuName(r.guest)).Str(" pc 0x").Hex(r.guest_pc, 8).Str(" as last stored, frame ").Dec(r.frame).End();
			if (r.code_count)
			{
				line.Str("  ").Str(CpuName(r.guest)).Str(" code at the block:").End();
				for (u32 i = 0; i < r.code_count; i++)
				{
					line.Str("    ").Hex(r.code[i].addr, 8).Str("  ");
					if (r.code[i].readable)
						line.Hex(r.code[i].value, 8);
					else
						line.Str("????????");
					line.End();
				}
			}
		}

		void WriteToFd(void* context, const char* text, size_t length)
		{
			const int fd = static_cast<int>(reinterpret_cast<intptr_t>(context));
			while (length > 0)
			{
				const ssize_t n = write(fd, text, length);
				if (n < 0 && errno == EINTR)
					continue;
				if (n <= 0)
					return;
				text += n;
				length -= static_cast<size_t>(n);
			}
		}
	} // namespace

	Where Classify(const Layout& l, uptr pc)
	{
		Where w;
		if (!l.code_base || pc < l.code_base || pc - l.code_base >= HostMemoryMap::CodeSize)
			return w;

		const uptr off = pc - l.code_base;
		w.code_offset = off;
		const auto in = [off](u32 start, u32 size) { return off - start < size; };
		if (in(HostMemoryMap::EErecOffset, HostMemoryMap::EErecSize))
			w.area = Part(pc, l.ee.blocks_start, l.ee.cold_start, l.ee.pool_start, Area::EeShared, Area::EeBlocks,
				Area::EeColdExits, Area::EeConstants);
		else if (in(HostMemoryMap::IOPrecOffset, HostMemoryMap::IOPrecSize))
			w.area = Part(pc, l.iop.blocks_start, 0, l.iop.pool_start, Area::IopShared, Area::IopBlocks, Area::IopBlocks,
				Area::IopConstants);
		else if (in(HostMemoryMap::VIF0recOffset, HostMemoryMap::VIF0recSize))
			w.area = Area::Vif0Dynarec;
		else if (in(HostMemoryMap::VIF1recOffset, HostMemoryMap::VIF1recSize))
			w.area = Area::Vif1Dynarec;
		else if (in(HostMemoryMap::mVU0recOffset, HostMemoryMap::mVU0recSize))
			w.area = Part(pc, l.vu_blocks_start[0], 0, 0, Area::Vu0Shared, Area::Vu0Blocks, Area::Vu0Blocks, Area::Vu0Blocks);
		else if (in(HostMemoryMap::mVU1recOffset, HostMemoryMap::mVU1recSize))
			w.area = Part(pc, l.vu_blocks_start[1], 0, 0, Area::Vu1Shared, Area::Vu1Blocks, Area::Vu1Blocks, Area::Vu1Blocks);
		else if (in(HostMemoryMap::VIFUnpackRecOffset, HostMemoryMap::VIFUnpackRecSize))
			w.area = Area::VifUnpack;
		else
			w.area = Area::SwRenderer;
		w.region_offset = off - RegionOf(w.area).offset;
		return w;
	}

	const char* RegionName(Area area)
	{
		switch (area)
		{
			case Area::EeShared:
			case Area::EeBlocks:
			case Area::EeColdExits:
			case Area::EeConstants:
				return "EE recompiler";
			case Area::IopShared:
			case Area::IopBlocks:
			case Area::IopConstants:
				return "IOP recompiler";
			case Area::Vif0Dynarec:
				return "VIF0 unpack recompiler";
			case Area::Vif1Dynarec:
				return "VIF1 unpack recompiler";
			case Area::Vu0Shared:
			case Area::Vu0Blocks:
				return "microVU0";
			case Area::Vu1Shared:
			case Area::Vu1Blocks:
				return "microVU1";
			case Area::VifUnpack:
				return "VIF unpack functions";
			case Area::SwRenderer:
				return "GS software renderer";
			default:
				return nullptr;
		}
	}

	const char* PartName(Area area)
	{
		switch (area)
		{
			case Area::EeShared:
			case Area::IopShared:
			case Area::Vu0Shared:
			case Area::Vu1Shared:
				return "shared code";
			case Area::EeBlocks:
			case Area::IopBlocks:
			case Area::Vu0Blocks:
			case Area::Vu1Blocks:
				return "blocks";
			case Area::EeColdExits:
				return "cold side exits";
			case Area::EeConstants:
			case Area::IopConstants:
				return "constant pool";
			default:
				return nullptr;
		}
	}

	// The first three lines: the signal, the thread and the pc. Written before
	// the block lookup, the one part of the report that walks heap data.
	static void WriteHead(const Report& r, Sink sink, void* context)
	{
		Line line(sink, context);

		line.Str("Host fault: ");
		if (const char* name = SignalName(r.signal))
			line.Str(name);
		else
			line.Str("signal ").Dec(r.signal);
		if (const char* code = CodeName(r.signal, r.si_code))
			line.Str(" (").Str(code).Str(")");
		else
			line.Str(" (si_code ").Dec(r.si_code).Str(")");
		line.Str(" at 0x").Hex(r.fault_address, 16).End();

		line.Str("  thread ").Dec(r.tid).Str(" \"").Str(r.thread_name).Str("\", signal ").Dec(r.signal);
		line.Str(", si_code ").Dec(r.si_code).End();

		line.Str("  pc 0x").Hex(r.pc, 16).Str(": ");
		if (const char* region = RegionName(r.where.area))
		{
			line.Str(region).Str(" +0x").Hex(r.where.region_offset, 8);
			if (const char* part = PartName(r.where.area))
				line.Str(" (").Str(part).Str(")");
			line.Str(", code area +0x").Hex(r.where.code_offset, 8);
			if (r.have_instruction)
				line.Str(", instruction ").Hex(r.instruction, 8);
		}
		else
		{
			line.Str("not in recompiled code");
		}
		line.End();
	}

	static void WriteRest(const Report& r, Sink sink, void* context)
	{
		Line line(sink, context);

		if (r.block_cpu == Cpu::EE || r.block_cpu == Cpu::IOP)
		{
			if (r.block_found)
			{
				line.Str("  block: ").Str(CpuName(r.block_cpu)).Str(" 0x").Hex(r.block.guest_pc, 8).Str(", ");
				line.Dec(r.block.guest_words).Str(" instructions, host 0x").Hex(r.block.host_start, 16);
				line.Str(" +0x").Hex(r.pc - r.block.host_start, 8).Str(" of 0x").Hex(r.block.host_size, 8).End();
			}
			else
			{
				line.Str("  block: none found").End();
			}
		}
		else if (r.block_cpu == Cpu::VU0 || r.block_cpu == Cpu::VU1)
		{
			const char* const vu = CpuName(r.block_cpu);
			if (r.vu.found)
			{
				line.Str("  block: ").Str(vu).Str(" 0x").Hex(r.vu.block_pc, 4).Str(", nearest entry below the pc in program ");
				line.Dec(r.vu.program).Str(" (start 0x").Hex(r.vu.program_start, 4).Str("), host 0x").Hex(r.vu.host_start, 16);
				line.Str(" +0x").Hex(r.pc - r.vu.host_start, 8).End();
			}
			else
			{
				line.Str("  block: none found").End();
			}
			line.Str("  ").Str(vu).Str(" TPC 0x").Hex(r.vu.tpc, 4).Str(" as last stored").End();
		}

		if (r.have_host_registers)
		{
			line.Str("  host registers:").End();
			for (u32 i = 0; i < 32; i += 4)
			{
				line.Str("   ");
				for (u32 k = i; k < i + 4; k++)
				{
					char name[4] = {'s', 'p', 0, 0};
					if (k < 31)
					{
						name[0] = 'x';
						name[1] = static_cast<char>(k < 10 ? '0' + k : '0' + k / 10);
						name[2] = static_cast<char>(k < 10 ? 0 : '0' + k % 10);
					}
					line.Str(" ").Str(name, 4).Str(" ").Hex(k < 31 ? r.x[k] : r.sp, 16);
				}
				line.End();
			}
			line.Str("    pstate ").Hex(r.pstate, 16).End();
		}

		if (r.guest == Cpu::EE || r.guest == Cpu::IOP)
			WriteRegisters(r, sink, context);

		line.Str("Aborting application.").End();
	}

	void Write(const Report& r, Sink sink, void* context)
	{
		WriteHead(r, sink, context);
		WriteRest(r, sink, context);
	}

	Layout CurrentLayout()
	{
		Layout l;
		if (!SysMemory::HasCodeMemory())
			return l;
		l.code_base = reinterpret_cast<uptr>(SysMemory::GetCodePtr(0));
		l.ee = EeSlices();
		l.iop = IopSlices();
		l.vu_blocks_start[0] = VuBlocksStart(0);
		l.vu_blocks_start[1] = VuBlocksStart(1);
		return l;
	}

	// The facts the kernel passed, and where the pc is.
	static void CaptureHead(int signal, const void* siginfo, const void* ucontext, const Layout& layout, Report* r)
	{
		const siginfo_t* const info = static_cast<const siginfo_t*>(siginfo);
		const ucontext_t* const uc = static_cast<const ucontext_t*>(ucontext);

		r->signal = signal;
		r->si_code = info->si_code;
		r->fault_address = reinterpret_cast<uptr>(info->si_addr);
		r->tid = static_cast<long>(syscall(SYS_gettid));
		prctl(PR_GET_NAME, r->thread_name);

		r->pc = static_cast<uptr>(uc->uc_mcontext.pc);
		for (int i = 0; i < 31; i++)
			r->x[i] = uc->uc_mcontext.regs[i];
		r->sp = uc->uc_mcontext.sp;
		r->pstate = uc->uc_mcontext.pstate;
		r->have_host_registers = true;

		r->where = Classify(layout, r->pc);
		if (r->where.area == Area::None)
			return;

		// The whole code area is mapped readable.
		std::memcpy(&r->instruction, reinterpret_cast<const void*>(r->pc & ~static_cast<uptr>(3)), sizeof(u32));
		r->have_instruction = true;
	}

	// The block, and the guest state of the CPU whose code it is.
	static void CaptureRest(Report* r)
	{
		bool (*read_word)(u32, u32*) = nullptr;
		switch (r->where.area)
		{
			case Area::EeShared:
			case Area::EeBlocks:
			case Area::EeColdExits:
			case Area::EeConstants:
			{
				r->block_cpu = Cpu::EE;
				r->block_found = FindEeBlock(r->pc, &r->block);
				r->guest = Cpu::EE;
				for (u32 i = 0; i < 32; i++)
					r->gpr[i] = cpuRegs.GPR.r[i].UD[0];
				EePinnedRegisters(r->x, r->gpr);
				r->hi = cpuRegs.HI.UD[0];
				r->lo = cpuRegs.LO.UD[0];
				r->guest_pc = cpuRegs.pc;
				read_word = RecErrorReport::ReadEeWord;
				break;
			}

			case Area::IopShared:
			case Area::IopBlocks:
			case Area::IopConstants:
			{
				r->block_cpu = Cpu::IOP;
				r->block_found = FindIopBlock(r->pc, &r->block);
				r->guest = Cpu::IOP;
				for (u32 i = 0; i < 32; i++)
					r->gpr[i] = psxRegs.GPR.r[i];
				r->hi = psxRegs.GPR.n.hi;
				r->lo = psxRegs.GPR.n.lo;
				r->guest_pc = psxRegs.pc;
				read_word = RecErrorReport::ReadIopWord;
				break;
			}

			case Area::Vu0Shared:
			case Area::Vu0Blocks:
			case Area::Vu1Shared:
			case Area::Vu1Blocks:
			{
				const int vu = (r->where.area == Area::Vu1Shared || r->where.area == Area::Vu1Blocks) ? 1 : 0;
				r->block_cpu = vu ? Cpu::VU1 : Cpu::VU0;
				FindVuBlock(vu, r->pc, &r->vu);
				r->vu.tpc = vuRegs[vu].VI[REG_TPC].UL;
				break;
			}

			default:
				break;
		}

		if (r->guest != Cpu::None)
			r->frame = g_FrameCount;

		if (read_word && r->block_found)
		{
			r->code_count = std::min(r->block.guest_words, kMaxCodeWords);
			for (u32 i = 0; i < r->code_count; i++)
			{
				RecErrorReport::Word& w = r->code[i];
				w.addr = r->block.guest_pc + i * 4;
				w.readable = read_word(w.addr, &w.value);
			}
		}
	}

	void Capture(int signal, const void* siginfo, const void* ucontext, const Layout& layout, Report* r)
	{
		CaptureHead(signal, siginfo, ucontext, layout, r);
		CaptureRest(r);
	}

	void OnUnhandledFault(int signal, void* siginfo, void* ucontext)
	{
		if (s_reported.exchange(true))
			return;

		int fd = Log::GetFileLogDescriptor();
		if (fd < 0)
			fd = STDERR_FILENO;
		void* const context = reinterpret_cast<void*>(static_cast<intptr_t>(fd));

		// If the block lookup faults, the first lines are in the log already.
		Report report;
		CaptureHead(signal, siginfo, ucontext, CurrentLayout(), &report);
		WriteHead(report, WriteToFd, context);
		CaptureRest(&report);
		WriteRest(report, WriteToFd, context);
	}

	void Install()
	{
		PageFaultHandler::SetUnhandledFaultReporter(OnUnhandledFault);
	}

#ifdef PCSX2_RECOMPILER_TESTS
	void ResetForTests()
	{
		s_reported.store(false);
	}
#endif
} // namespace RecCrashReport

#endif // defined(__linux__)
