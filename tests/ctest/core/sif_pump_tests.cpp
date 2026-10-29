// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A characterisation test for the SIF0 and SIF1 pumps (SIF0Dma, SIF1Dma).
//
// The pumps move data between IOP RAM and EE RAM through a 128-word FIFO. One call loops "IOP side
// step, EE side step" until neither side can move, then clears its status bits in SBUS F240, and the
// completion interrupts of both sides are scheduled on the way. The test pins down what the pumps do,
// so that restructuring the loop body (for example into one function per side) cannot change it
// unnoticed. It runs scripted transfers through the real entry points (dmaSIF0/dmaSIF1 from the EE
// side, psxDma9/psxDma10 from the IOP side) and compares, per script:
//
//   - EE RAM (the 32 MB the DMA address translation exposes) and IOP RAM (2 MB);
//   - the EE channel registers (sif0ch, sif1ch, sif2dma) and the IOP ones (hw_dma9, hw_dma10);
//   - the sif0, sif1 and sif2 structures, field by field;
//   - the scheduled interrupts: cpuRegs and psxRegs interrupt masks, start and end cycles, the next
//     event cycles, and the IOP slice break accounting that CPU_INT touches;
//   - both hardware register pages, which holds SBUS F200..F260 and the DMAC registers;
//   - a digest of everything but the RAMs, taken after every entry call, so the state a transfer is
//     left in half way through (one side started, the other not yet) is covered too.
//
// against recorded hashes (sif_pump_golden.inc). A script is a chain of packets of 1 to 4096 words,
// with or without end and interrupt tags, started from either side. Some scripts end with a side
// still armed, and one drives SIF1 through the DMAC's stall control. The test does not say the
// recorded behaviour is right; it says it did not move.
//
// What it cannot see: which of the two steps of a loop iteration runs first, and where in the
// iteration SIF0's junk write sits. The producer's completion always comes before the consumer's
// whichever way the steps are ordered, because the consumer can only finish after the producer's last
// push. A change to the progress accounting, the stall flag, the junk write being repeated each
// iteration, or the cycle counts is caught.
//
// A hash proves nothing if the script never ran, so the scripts that finish also check what a
// finished transfer has to look like: the payload arrived, both sides went idle, both completion
// interrupts are scheduled.
//
// To record the hashes again (after a deliberate behaviour change), run
//   core_test --gtest_also_run_disabled_tests --gtest_filter='SifPumpRecord.DISABLED_PrintGoldens'
//   core_test --gtest_also_run_disabled_tests --gtest_filter='SifPumpRecord.DISABLED_PrintSweepGoldens'
// and replace the rows of sif_pump_golden.inc and sif_pump_sweep_golden.inc with the lines they print.
//
// Memory. The pumps reach EE RAM through eeMem and IOP RAM through iopMem, both plain pointers.
// This test points them at heap blocks instead of bringing up SysMemory (the host memory map, the
// code arenas and vtlb), none of which the pumps use. psxCpu is set to the IOP interpreter because
// WriteFifoToIOP calls psxCpu->Clear, which is empty there. Every global the scripts touch is saved
// before the first script and put back after the last.

#include "Common.h"
#include "Config.h"
#include "Dmac.h"
#include "Hw.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "R3000A.h"
#include "R5900.h"
#include "Sif.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

extern EE_intProcessStatus eeRunInterruptScan; // defined in R5900.cpp, declared in no header

namespace
{
	// XXH64-shaped rounds, written out here so the test depends on nothing else and the recorded values
	// are the same on every host (the PS2 side of all of this is little endian).
	constexpr u64 kP1 = 0x9E3779B185EBCA87ull;
	constexpr u64 kP2 = 0xC2B2AE3D27D4EB4Full;
	constexpr u64 kP3 = 0x165667B19E3779F9ull;

	inline u64 Rotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
	inline u64 Round(u64 acc, u64 in) { return Rotl(acc + in * kP2, 31) * kP1; }

	inline u64 Avalanche(u64 h)
	{
		h ^= h >> 33;
		h *= kP2;
		h ^= h >> 29;
		h *= kP3;
		h ^= h >> 32;
		return h;
	}

	// Four lanes keep the hash of a 32 MB RAM to a few milliseconds.
	u64 HashBytes(const void* data, size_t size)
	{
		const u8* p = static_cast<const u8*>(data);
		u64 lane[4] = {kP1 + 1, kP1 + 2, kP1 + 3, kP1 + 4};
		size_t i = 0;
		for (; i + 32 <= size; i += 32)
		{
			for (int k = 0; k < 4; k++)
			{
				u64 w;
				std::memcpy(&w, p + i + 8 * k, 8);
				lane[k] = Round(lane[k], w);
			}
		}
		for (; i + 8 <= size; i += 8)
		{
			u64 w;
			std::memcpy(&w, p + i, 8);
			lane[0] = Round(lane[0], w);
		}
		u64 tail = 0;
		std::memcpy(&tail, p + i, size - i); // fewer than 8 bytes, zero padded; the size is hashed below
		lane[1] = Round(lane[1], tail);
		u64 h = static_cast<u64>(size);
		for (int k = 0; k < 4; k++)
			h = Round(h, lane[k]);
		return Avalanche(h);
	}

	class Digest
	{
	public:
		void Add(u64 v) { m_h = Round(m_h, v); }
		void AddBytes(const void* data, size_t size) { Add(HashBytes(data, size)); }
		u64 Value() const { return Avalanche(m_h); }

	private:
		u64 m_h = kP3;
	};

	// Deterministic payload words: xorshift64*, seeded from the script's name.
	class Rng
	{
	public:
		explicit Rng(const std::string& seed)
		{
			m_s = 0xCBF29CE484222325ull; // FNV-1a
			for (const char c : seed)
			{
				m_s ^= static_cast<u8>(c);
				m_s *= 0x100000001B3ull;
			}
			if (m_s == 0)
				m_s = 1;
		}
		u32 Next()
		{
			m_s ^= m_s >> 12;
			m_s ^= m_s << 25;
			m_s ^= m_s >> 27;
			return static_cast<u32>((m_s * 0x2545F4914F6CDD1Dull) >> 32);
		}

	private:
		u64 m_s;
	};

	enum class Chan
	{
		Sif0, // IOP RAM to EE RAM
		Sif1, // EE RAM to IOP RAM
	};

	// How the last packet ends the transfer.
	enum class Term
	{
		Both, // end tag on the IOP tag and END on the EE tag
		TieIrq, // IRQ on the IOP tag, and the EE tag's IRQ with the channel's TIE set
		IopOnly, // only the IOP tag ends; the EE side is left waiting for a tag (SIF0), or runs into zeros (SIF1)
		EeOnly, // only the EE tag ends; the IOP side is left waiting (SIF1), or reads zero tags (SIF0)
	};

	enum class Start
	{
		EeFirst, // dmaSIFn, then psxDmaN
		IopFirst, // psxDmaN, then dmaSIFn
	};

	struct Script
	{
		std::string name;
		Chan chan = Chan::Sif0;
		std::vector<u32> words; // payload words per packet
		Term term = Term::Both;
		Start start = Start::EeFirst;
		bool normalMode = false; // EE channel in NORMAL mode: no tags, the first QW is the tag the other side sent
		bool tte = false; // SIF1: CHCR.TTE, so the EE tag's second half goes into the FIFO
		bool midIrq = false; // an EE tag in the middle of the chain carries IRQ with TIE clear
		bool refTag = false; // SIF1: the second packet is a REF tag, data elsewhere
		bool scanRunning = false; // eeRunInterruptScan is INT_RUNNING, which changes CPU_INT for short delays
		bool iopSlice = false; // an IOP slice is running (psxRegs.iopCycleEE > 0), which changes CPU_INT for short delays
		bool stall = false; // SIF1 through the DMAC's stall control
		bool sweep = false; // from the pseudo-random list: compared with the recording only, never judged
	};

	void PrintTo(const Script& s, std::ostream* os) { *os << s.name; }

	enum class Call
	{
		EeStart, // dmaSIF0 / dmaSIF1
		IopStart, // psxDma9 / psxDma10
		RaiseStall, // move STADR out of the way of the stalled transfer
	};

	// What a script put into memory, kept to check the transfers that finish.
	struct Payload
	{
		std::vector<std::vector<u32>> packets; // the words each packet carried
		std::vector<u32> dst; // where each packet's words must land (EE address for SIF0, IOP address for SIF1)
	};

	struct Built
	{
		std::vector<Call> calls;
		Payload payload;
	};

	template <typename T>
	T Load(const u8* base, u32 off)
	{
		T v;
		std::memcpy(&v, base + off, sizeof(T));
		return v;
	}

	void Store32(u8* base, u32 off, u32 v) { std::memcpy(base + off, &v, 4); }

	// cpuRegs is a macro for a member of the register pack, so the saved copies have other names.
	struct SavedGlobals
	{
		EEVM_MemoryAllocMess* ee_mem = nullptr;
		IopVM_MemoryAllocMess* iop_mem = nullptr;
		R3000Acpu* psx_cpu = nullptr;
		cpuRegisters cpu;
		psxRegisters psx;
		u8 ee_hw[Ps2MemSize::Hardware];
		u8 iop_hw[Ps2MemSize::IopHardware];
		_sif s0, s1, s2;
		EE_intProcessStatus scan = INT_NOT_RUNNING;
		bool iop_event_action = false;
		bool ee_timing_hack = false;
		u32 ps2clk = 0, psxclk = 0;
	};

	SavedGlobals* s_saved = nullptr;
	EEVM_MemoryAllocMess* s_ee_block = nullptr;
	IopVM_MemoryAllocMess* s_iop_block = nullptr;

	bool BringUpMachine()
	{
		s_saved = new SavedGlobals;
		s_saved->ee_mem = eeMem;
		s_saved->iop_mem = iopMem;
		s_saved->psx_cpu = psxCpu;
		s_saved->cpu = cpuRegs;
		s_saved->psx = psxRegs;
		std::memcpy(s_saved->ee_hw, eeHw, sizeof(eeHw));
		std::memcpy(s_saved->iop_hw, iopHw, sizeof(iopHw));
		s_saved->s0 = sif0;
		s_saved->s1 = sif1;
		s_saved->s2 = sif2;
		s_saved->scan = eeRunInterruptScan;
		s_saved->iop_event_action = iopEventAction;
		s_saved->ee_timing_hack = EmuConfig.Gamefixes.EETimingHack;
		s_saved->ps2clk = PS2CLK;
		s_saved->psxclk = PSXCLK;

		// Lazily committed, so the 270 MB of address space costs the pages the scripts write.
		s_ee_block = static_cast<EEVM_MemoryAllocMess*>(std::calloc(1, sizeof(EEVM_MemoryAllocMess)));
		s_iop_block = static_cast<IopVM_MemoryAllocMess*>(std::calloc(1, sizeof(IopVM_MemoryAllocMess)));
		if (!s_ee_block || !s_iop_block)
			return false;
		eeMem = s_ee_block;
		iopMem = s_iop_block;
		psxCpu = &psxInt;
		EmuConfig.Gamefixes.EETimingHack = false;
		PS2CLK = PS2CLK_DEFAULT; // PSX_INT scales IOP cycles to EE cycles by their ratio
		PSXCLK = 36864000;
		return true;
	}

	void TearDownMachine()
	{
		if (!s_saved)
			return;
		eeMem = s_saved->ee_mem;
		iopMem = s_saved->iop_mem;
		psxCpu = s_saved->psx_cpu;
		cpuRegs = s_saved->cpu;
		psxRegs = s_saved->psx;
		std::memcpy(eeHw, s_saved->ee_hw, sizeof(eeHw));
		std::memcpy(iopHw, s_saved->iop_hw, sizeof(iopHw));
		sif0 = s_saved->s0;
		sif1 = s_saved->s1;
		sif2 = s_saved->s2;
		eeRunInterruptScan = s_saved->scan;
		iopEventAction = s_saved->iop_event_action;
		EmuConfig.Gamefixes.EETimingHack = s_saved->ee_timing_hack;
		PS2CLK = s_saved->ps2clk;
		PSXCLK = s_saved->psxclk;
		std::free(s_ee_block);
		std::free(s_iop_block);
		s_ee_block = nullptr;
		s_iop_block = nullptr;
		delete s_saved;
		s_saved = nullptr;
	}

	// The same starting point for every script, whatever ran before it.
	void ResetMachine(const Script& s)
	{
		std::memset(eeMem->Main, 0, Ps2MemSize::ExposedRam);
		std::memset(eeMem->Scratch, 0, sizeof(eeMem->Scratch));
		std::memset(iopMem->Main, 0, Ps2MemSize::ExposedIopRam);
		std::memset(eeHw, 0, sizeof(eeHw));
		std::memset(iopHw, 0, sizeof(iopHw));
		sifReset();
		std::memset(&sif2, 0, sizeof(sif2));

		std::memset(&cpuRegs, 0, sizeof(cpuRegs));
		std::memset(&psxRegs, 0, sizeof(psxRegs));
		cpuRegs.cycle = 0x100001234ull; // past 32 bits, so a truncation shows
		cpuRegs.nextEventCycle = cpuRegs.cycle + 3072;
		psxRegs.cycle = 0x20000;
		// With an IOP slice running (iopCycleEE > 0), CPU_INT breaks it and zeroes iopCycleEE, and PSX_INT
		// asks the EE to look sooner when iopCycleEE is below the IOP's next event (8 EE cycles per IOP
		// cycle). The slice scripts put an IOP event 1 IOP cycle out, so a transfer whose EE completion is
		// due in 9 to 28 cycles shows which completion is scheduled first in the EE's next event cycle.
		psxRegs.iopNextEventCycle = psxRegs.cycle + (s.iopSlice ? 1 : 1000);
		psxRegs.iopCycleEE = s.iopSlice ? 500 : 0;
		eeRunInterruptScan = s.scanRunning ? INT_RUNNING : INT_NOT_RUNNING;
		iopEventAction = false;

		// The SBUS registers. The pumps clear bits 0x20/0x2000 (SIF0) and 0x40/0x4000 (SIF1) of F240; the
		// other words are there to show a stray write.
		psHu32(SBUS_F200) = 0x0200F200;
		psHu32(SBUS_F210) = 0x0210F210;
		psHu32(SBUS_F220) = 0x0220F220;
		psHu32(SBUS_F230) = 0x0230F230;
		psHu32(SBUS_F240) = 0x00006061;
		psHu32(SBUS_F250) = 0x0250F250;
		psHu32(SBUS_F260) = 0x0260F260;
	}

	constexpr u32 kIopEnd = 0x40000000; // ID & 4 in an IOP tag
	constexpr u32 kIopIrq = 0x80000000;

	u32 EeTag(u32 qwc, u32 id, u32 irq) { return qwc | (id << 28) | (irq << 31); }

	struct EndFlags
	{
		u32 iopFlags = 0; // OR-ed into the IOP tag's address word
		u32 eeId = TAG_CNT; // the EE tag's ID
		u32 eeIrq = 0; // the EE tag's IRQ
	};

	EndFlags EndFlagsFor(Term term, u32 notLastId)
	{
		EndFlags f;
		f.eeId = notLastId;
		switch (term)
		{
			case Term::Both:
				f.iopFlags = kIopEnd;
				f.eeId = TAG_END;
				break;
			case Term::TieIrq:
				f.iopFlags = kIopIrq;
				f.eeIrq = 1;
				break;
			case Term::IopOnly:
				f.iopFlags = kIopEnd;
				break;
			case Term::EeOnly:
				f.eeId = TAG_END;
				break;
		}
		return f;
	}

	std::vector<Call> StartCalls(Start start)
	{
		if (start == Start::EeFirst)
			return {Call::EeStart, Call::IopStart};
		return {Call::IopStart, Call::EeStart};
	}

	// The IOP tags sit at kIopTags, 16 bytes each: { data address | flags, words, EE tag word 0, EE
	// destination address }. The words 2 and 3 are the EE tag SIF0 sends along, and the first two
	// words of the NEXT IOP tag are what follows it in the FIFO, which the junk logic in Sif.h reads.
	Built BuildSif0(const Script& s, Rng& rng)
	{
		constexpr u32 kIopTags = 0x1000, kIopData = 0x20000, kEeDst = 0x100000, kStride = 0x8000;
		Built b;
		u8* const iop = iopMem->Main;
		const size_t n = s.words.size();
		u32 totalQwc = 0;

		for (size_t i = 0; i < n; i++)
		{
			const bool last = (i + 1 == n);
			const u32 w = s.words[i];
			const u32 dataAddr = kIopData + static_cast<u32>(i) * kStride;
			const u32 dst = kEeDst + static_cast<u32>(i) * kStride;
			const u32 qwc = (w + 3) / 4;
			totalQwc += qwc;

			std::vector<u32> payload(w);
			for (u32 j = 0; j < w; j++)
			{
				payload[j] = rng.Next();
				Store32(iop, dataAddr + 4 * j, payload[j]);
			}
			b.payload.packets.push_back(std::move(payload));
			b.payload.dst.push_back(dst);

			EndFlags f;
			if (last)
				f = EndFlagsFor(s.term, TAG_CNT);
			if (s.midIrq && i == 0 && !last)
				f.eeIrq = 1;

			const u32 tag = kIopTags + 16 * static_cast<u32>(i);
			Store32(iop, tag + 0, dataAddr | f.iopFlags);
			Store32(iop, tag + 4, w);
			Store32(iop, tag + 8, EeTag(qwc, f.eeId, f.eeIrq));
			Store32(iop, tag + 12, dst);
		}

		hw_dma9.tadr = kIopTags;
		hw_dma9.madr = 0;
		hw_dma9.bcr = 0;
		hw_dma9.chcr = 0x01000201;

		sif0ch.chcr.set(0);
		sif0ch.chcr.MOD = s.normalMode ? NORMAL_MODE : CHAIN_MODE;
		sif0ch.chcr.TIE = (s.term == Term::TieIrq) ? 1 : 0;
		sif0ch.chcr.STR = 1;
		if (s.normalMode)
		{
			// No tag read from the FIFO, so the QW the IOP sends in front of the data is data too.
			sif0ch.madr = kEeDst;
			sif0ch.qwc = 1 + totalQwc;
		}

		b.calls = StartCalls(s.start);
		return b;
	}

	// The EE chain sits at kChain: per packet an EE tag, then a quadword holding the IOP tag
	// { destination | flags, words, 0, 0 }, then the payload rounded up to quadwords. With TTE the
	// first half of the IOP tag rides in the EE tag and the quadword holds two filler words and the
	// start of the payload.
	Built BuildSif1(const Script& s, Rng& rng)
	{
		constexpr u32 kChain = 0x100000, kRefData = 0x400000, kIopDst = 0x30000, kStride = 0x8000;
		Built b;
		u8* const ee = eeMem->Main;
		const size_t n = s.words.size();
		u32 tagAddr = kChain;
		u32 totalQwc = 0;

		for (size_t i = 0; i < n; i++)
		{
			const bool last = (i + 1 == n);
			const u32 w = s.words[i];
			// The IOP tag is the first quadword of the data, or with TTE the two words the EE tag sends
			// along followed by two more words from the front of the data.
			const u32 lead = s.tte ? 2 : 4;
			const u32 qwc = (lead + w + 3) / 4;
			const u32 iopDst = kIopDst + static_cast<u32>(i) * kStride;
			const bool ref = s.refTag && i == 1 && !last;
			const u32 dataAddr = ref ? kRefData : (s.normalMode ? kChain : tagAddr + 16);
			totalQwc += qwc;

			EndFlags f;
			f.eeId = ref ? TAG_REF : TAG_CNT;
			if (last)
				f = EndFlagsFor(s.term, f.eeId);
			if (s.midIrq && i == 0 && !last)
				f.eeIrq = 1;

			if (!s.normalMode)
			{
				Store32(ee, tagAddr + 0, EeTag(qwc, f.eeId, f.eeIrq));
				Store32(ee, tagAddr + 4, dataAddr);
				// Words 2 and 3 are sent only with TTE, and then they are the first half of the IOP tag.
				Store32(ee, tagAddr + 8, s.tte ? (iopDst | f.iopFlags) : 0xE0E0E0E0 + static_cast<u32>(i));
				Store32(ee, tagAddr + 12, s.tte ? w : 0xE1E1E1E1 + static_cast<u32>(i));
			}

			if (s.tte)
			{
				Store32(ee, dataAddr + 0, 0xF0F0F0F0 + static_cast<u32>(i)); // the other half of the IOP tag
				Store32(ee, dataAddr + 4, 0xF1F1F1F1 + static_cast<u32>(i));
			}
			else
			{
				Store32(ee, dataAddr + 0, iopDst | f.iopFlags);
				Store32(ee, dataAddr + 4, w);
				Store32(ee, dataAddr + 8, 0);
				Store32(ee, dataAddr + 12, 0);
			}

			std::vector<u32> payload(w);
			const u32 paddedWords = qwc * 4 - lead;
			for (u32 j = 0; j < paddedWords; j++)
			{
				const u32 v = rng.Next(); // the padding in the last QW is random too
				Store32(ee, dataAddr + 4 * lead + 4 * j, v);
				if (j < w)
					payload[j] = v;
			}
			b.payload.packets.push_back(std::move(payload));
			b.payload.dst.push_back(iopDst);

			tagAddr = ref ? tagAddr + 16 : tagAddr + 16 + qwc * 16;
		}

		hw_dma10.madr = 0;
		hw_dma10.bcr = 0;
		hw_dma10.chcr = 0x01000200;

		sif1ch.chcr.set(0);
		sif1ch.chcr.MOD = s.normalMode ? NORMAL_MODE : CHAIN_MODE;
		sif1ch.chcr.TIE = (s.term == Term::TieIrq) ? 1 : 0;
		sif1ch.chcr.TTE = s.tte ? 1 : 0;
		sif1ch.chcr.STR = 1;
		if (s.normalMode)
		{
			sif1ch.madr = kChain;
			sif1ch.qwc = totalQwc;
		}
		else
		{
			sif1ch.tadr = kChain;
		}

		if (s.stall)
		{
			// The stall address is 36 quadwords in. The first chunk the EE side sends (32 quadwords, the
			// FIFO's size) is under it; the next one (9 quadwords) is not, so the transfer stalls after the
			// first chunk with the IOP side partway through it. The third call is made while still stalled
			// and must touch nothing, the cycle counts included. Then STADR moves and the transfer resumes.
			dmacRegs.ctrl.STD = STD_SIF1;
			dmacRegs.stadr.ADDR = sif1ch.madr + 36 * 16;
			if (s.start == Start::EeFirst)
				b.calls = {Call::EeStart, Call::IopStart, Call::EeStart, Call::RaiseStall, Call::EeStart};
			else
				b.calls = {Call::IopStart, Call::EeStart, Call::EeStart, Call::RaiseStall, Call::EeStart};
		}
		else
		{
			b.calls = StartCalls(s.start);
		}
		return b;
	}

	void DoCall(const Script& s, Call c)
	{
		switch (c)
		{
			case Call::EeStart:
				if (s.chan == Chan::Sif0)
					dmaSIF0();
				else
					dmaSIF1();
				break;
			case Call::IopStart:
				if (s.chan == Chan::Sif0)
					psxDma9(hw_dma9.madr, hw_dma9.bcr, hw_dma9.chcr);
				else
					psxDma10(hw_dma10.madr, hw_dma10.bcr, hw_dma10.chcr);
				break;
			case Call::RaiseStall:
				dmacRegs.stadr.ADDR = 0x1FF0000;
				break;
		}
	}

	struct Hashes
	{
		u64 eeram = 0;
		u64 iopram = 0;
		u64 eechan = 0; // sif0ch, sif1ch, sif2dma
		u64 iopdma = 0; // hw_dma9, hw_dma10
		u64 sifs = 0; // sif0, sif1, sif2
		u64 sched = 0; // scheduled interrupts
		u64 hw = 0; // both hardware register pages
		u64 trace = 0; // digest after every entry call
	};

	void AddSif(Digest& d, const _sif& s)
	{
		d.AddBytes(s.fifo.data, sizeof(s.fifo.data));
		d.AddBytes(s.fifo.junk, sizeof(s.fifo.junk));
		d.Add(static_cast<u32>(s.fifo.readPos));
		d.Add(static_cast<u32>(s.fifo.writePos));
		d.Add(static_cast<u32>(s.fifo.size));
		d.Add(s.ee.end);
		d.Add(s.ee.busy);
		d.Add(static_cast<u32>(s.ee.cycles));
		d.Add(s.iop.end);
		d.Add(s.iop.busy);
		d.Add(static_cast<u32>(s.iop.cycles));
		d.Add(static_cast<u32>(s.iop.writeJunk));
		d.Add(static_cast<u32>(s.iop.counter));
		d.Add(static_cast<u32>(s.iop.data.data));
		d.Add(static_cast<u32>(s.iop.data.words));
		d.Add(s.iop.data.tag_lo._u32);
		d.Add(s.iop.data.tag_hi._u32);
	}

	u64 HashSifs()
	{
		Digest d;
		AddSif(d, sif0);
		AddSif(d, sif1);
		AddSif(d, sif2);
		return d.Value();
	}

	u64 HashSched()
	{
		Digest d;
		d.Add(cpuRegs.interrupt);
		d.Add(cpuRegs.dmastall);
		d.Add(cpuRegs.cycle);
		d.Add(cpuRegs.nextEventCycle);
		for (int i = 0; i < 32; i++)
		{
			d.Add(cpuRegs.sCycle[i]);
			d.Add(cpuRegs.eCycle[i]);
		}
		d.Add(psxRegs.interrupt);
		d.Add(psxRegs.cycle);
		d.Add(psxRegs.iopNextEventCycle);
		d.Add(static_cast<u32>(psxRegs.iopBreak));
		d.Add(static_cast<u32>(psxRegs.iopCycleEE));
		d.Add(psxRegs.iopCycleEECarry);
		for (int i = 0; i < 32; i++)
		{
			d.Add(psxRegs.sCycle[i]);
			d.Add(static_cast<u32>(psxRegs.eCycle[i]));
		}
		d.Add(static_cast<u32>(eeRunInterruptScan));
		d.Add(iopEventAction);
		return d.Value();
	}

	u64 HashEeChan()
	{
		Digest d;
		d.AddBytes(&sif0ch, sizeof(DMACh));
		d.AddBytes(&sif1ch, sizeof(DMACh));
		d.AddBytes(&sif2dma, sizeof(DMACh));
		return d.Value();
	}

	u64 HashIopDma()
	{
		Digest d;
		d.AddBytes(&hw_dma9, sizeof(dma_mbct));
		d.AddBytes(&hw_dma10, sizeof(dma_mbc));
		return d.Value();
	}

	u64 HashHw()
	{
		Digest d;
		d.AddBytes(eeHw, sizeof(eeHw));
		d.AddBytes(iopHw, sizeof(iopHw));
		return d.Value();
	}

	// Everything but the RAMs.
	u64 StepDigest()
	{
		Digest d;
		d.Add(HashSifs());
		d.Add(HashSched());
		d.Add(HashEeChan());
		d.Add(HashIopDma());
		d.Add(HashHw());
		return d.Value();
	}

	struct RunResult
	{
		Hashes h;
		Built built;
		std::string summary; // for the comment on a recorded row; never compared
	};

	// Where the transfer ended up, in words a person can check against the script.
	std::string Summarise(const Script& s)
	{
		const _sif& sif = (s.chan == Chan::Sif0) ? sif0 : sif1;
		const EE_EventType eeEvt = (s.chan == Chan::Sif0) ? DMAC_SIF0 : DMAC_SIF1;
		const IopEventId iopEvt = (s.chan == Chan::Sif0) ? IopEvt_SIF0 : IopEvt_SIF1;
		char buf[256];
		std::snprintf(buf, sizeof(buf), "ee %s%s iop %s%s fifo %d | EE irq %s +%d | IOP irq %s +%d | EE next event +%d, slice break %d",
			sif.ee.busy ? "busy" : "idle", sif.ee.end ? "+end" : "", sif.iop.busy ? "busy" : "idle", sif.iop.end ? "+end" : "",
			static_cast<int>(sif.fifo.size),
			(cpuRegs.interrupt & (1u << eeEvt)) ? "set" : "clear", static_cast<int>(cpuRegs.eCycle[eeEvt]),
			(psxRegs.interrupt & (1u << iopEvt)) ? "set" : "clear", static_cast<int>(psxRegs.eCycle[iopEvt]),
			static_cast<int>(cpuRegs.nextEventCycle - cpuRegs.cycle), static_cast<int>(psxRegs.iopBreak));
		return buf;
	}

	RunResult RunScript(const Script& s)
	{
		ResetMachine(s);
		Rng rng(s.name);
		RunResult r;
		r.built = (s.chan == Chan::Sif0) ? BuildSif0(s, rng) : BuildSif1(s, rng);

		Digest trace;
		trace.Add(r.built.calls.size());
		for (const Call c : r.built.calls)
		{
			DoCall(s, c);
			trace.Add(StepDigest());
		}

		r.h.eeram = HashBytes(eeMem->Main, Ps2MemSize::ExposedRam);
		r.h.iopram = HashBytes(iopMem->Main, Ps2MemSize::ExposedIopRam);
		r.h.eechan = HashEeChan();
		r.h.iopdma = HashIopDma();
		r.h.sifs = HashSifs();
		r.h.sched = HashSched();
		r.h.hw = HashHw();
		r.h.trace = trace.Value();
		r.summary = Summarise(s);
		return r;
	}

	const char* TermName(Term t)
	{
		switch (t)
		{
			case Term::Both:
				return "both";
			case Term::TieIrq:
				return "tieirq";
			case Term::IopOnly:
				return "iopend";
			case Term::EeOnly:
				return "eeend";
		}
		return "?";
	}

	std::string Join(const std::vector<u32>& words)
	{
		std::string out;
		for (size_t i = 0; i < words.size(); i++)
			out += (i ? "_" : "") + std::to_string(words[i]);
		return out;
	}

	Script Make(Chan chan, Start start, std::vector<u32> words, Term term, const char* tag = "")
	{
		Script s;
		s.chan = chan;
		s.start = start;
		s.words = std::move(words);
		s.term = term;
		s.name = std::string(chan == Chan::Sif0 ? "sif0" : "sif1") + (start == Start::EeFirst ? "_ee" : "_iop") + "_w" + Join(s.words) + "_" + TermName(term) + tag;
		return s;
	}

	std::vector<Script> AllScripts()
	{
		std::vector<Script> all;
		for (const Chan chan : {Chan::Sif0, Chan::Sif1})
		{
			for (const Start start : {Start::EeFirst, Start::IopFirst})
			{
				// One packet, ended properly, at the sizes where the FIFO and the junk logic change shape:
				// below a quadword, one quadword, one word short of the FIFO, exactly the FIFO, one word
				// over it, and 32 times it.
				for (const u32 w : {1u, 3u, 4u, 127u, 128u, 129u, 4096u})
					all.push_back(Make(chan, start, {w}, Term::Both));

				for (const Term term : {Term::TieIrq, Term::IopOnly, Term::EeOnly})
					for (const u32 w : {4u, 127u, 129u, 4096u})
						all.push_back(Make(chan, start, {w}, term));

				// Chains. SIF1 truncates a packet's word count to a multiple of 4 and leaves the rest of the
				// quadword in the FIFO, which the next packet would read as its tag, so every packet but the
				// last has a multiple of 4 there.
				if (chan == Chan::Sif0)
				{
					all.push_back(Make(chan, start, {4, 127, 129}, Term::Both));
					all.push_back(Make(chan, start, {128, 128, 128}, Term::Both));
					all.push_back(Make(chan, start, {129, 4096, 4}, Term::Both));
					all.push_back(Make(chan, start, {1, 2, 3, 4, 5}, Term::Both));
					Script mid = Make(chan, start, {127, 129, 3}, Term::Both, "_midirq");
					mid.midIrq = true;
					all.push_back(mid);
				}
				else
				{
					all.push_back(Make(chan, start, {4, 128, 4096}, Term::Both));
					all.push_back(Make(chan, start, {128, 128, 128, 128}, Term::Both));
					all.push_back(Make(chan, start, {4, 8, 12, 16, 20}, Term::Both));
					all.push_back(Make(chan, start, {4096, 4}, Term::Both));
					Script mid = Make(chan, start, {64, 128, 5}, Term::Both, "_midirq");
					mid.midIrq = true;
					all.push_back(mid);
					Script ref = Make(chan, start, {4, 128, 4}, Term::Both, "_ref");
					ref.refTag = true;
					all.push_back(ref);
					// One packet only: TTE sends 2 + 4 * qwc words, the IOP takes 4 + a multiple of 4 and the
					// two words left over would be read as the start of the next tag.
					//
					// IOP first only. Started from the EE first, the two TTE words leave the FIFO with 2
					// words free, WriteEEtoFifo then moves 0 quadwords, HandleEETransfer still counts that
					// as progress, and with the IOP side not started SIF1Dma never returns: the script hangs.
					if (start == Start::IopFirst)
					{
						Script tte = Make(chan, start, {128}, Term::Both, "_tte");
						tte.tte = true;
						all.push_back(tte);
					}
				}

				{
					Script n = Make(chan, start, {128}, Term::Both, "_normal");
					n.normalMode = true;
					all.push_back(n);
				}

				// CPU_INT with a delay under four cycles takes a different road when the EE is inside its
				// interrupt scan; with an IOP slice running and a delay of 28 or less it breaks the slice.
				{
					// (SIF1 counts the IOP tag's quadword in the EE's cycles, so its one-cycle transfer has
					// no payload.)
					Script run = Make(chan, start, {chan == Chan::Sif0 ? 4u : 0u}, Term::Both, "_scanrun");
					run.scanRunning = true;
					all.push_back(run);
					Script slice = Make(chan, start, {4}, Term::Both, "_slice");
					slice.iopSlice = true;
					all.push_back(slice);
					for (const u32 w : {32u, 64u})
					{
						Script more = Make(chan, start, {w}, Term::Both, "_slice");
						more.iopSlice = true;
						all.push_back(more);
					}
				}
			}
		}

		// SIF1 stopped by the DMAC's stall control, polled while still stalled, then let go.
		for (const Start start : {Start::EeFirst, Start::IopFirst})
		{
			Script s = Make(Chan::Sif1, start, {160}, Term::Both, "_stall");
			s.normalMode = true;
			s.stall = true;
			all.push_back(s);
		}
		return all;
	}

	std::string Describe(const Script& s)
	{
		std::string d = std::string(s.chan == Chan::Sif0 ? "sif0 " : "sif1 ") + (s.start == Start::EeFirst ? "ee-first" : "iop-first") + " words " + Join(s.words) + " end " + TermName(s.term);
		if (s.midIrq)
			d += " midirq";
		if (s.scanRunning)
			d += " scanrun";
		if (s.iopSlice)
			d += " slice";
		return d;
	}

	// A few hundred pseudo-random chains, so that the order of the steps inside a pump, and the junk
	// logic meeting a full FIFO, are met in many alignments and not only in the hand-picked scripts.
	// The generator is fixed, so the list is the same every time.
	std::vector<Script> SweepScripts()
	{
		std::vector<Script> all;
		Rng rng("sif_pump_sweep");
		for (const Chan chan : {Chan::Sif0, Chan::Sif1})
		{
			for (int i = 0; i < 200; i++)
			{
				Script s;
				s.chan = chan;
				s.start = (rng.Next() & 1) ? Start::IopFirst : Start::EeFirst;
				s.term = static_cast<Term>(rng.Next() % 4);
				const u32 packets = 1 + rng.Next() % 4;
				for (u32 k = 0; k < packets; k++)
				{
					u32 w;
					switch (rng.Next() % 10)
					{
						case 0:
						case 1:
						case 2:
							w = 1 + rng.Next() % 12; // around a quadword
							break;
						case 3:
							w = 13 + rng.Next() % 44; // a few quadwords
							break;
						case 4:
						case 5:
						case 6:
							w = 112 + rng.Next() % 32; // around the FIFO's size
							break;
						case 7:
							w = 240 + rng.Next() % 40; // around twice it
							break;
						case 8:
							w = 500 + rng.Next() % 600;
							break;
						default:
							w = 4 * (1 + rng.Next() % 40);
							break;
					}
					// SIF1 hands the IOP a multiple of four words; the rest of the quadword would be
					// read as the next packet's tag. Only the last packet may have a remainder.
					if (chan == Chan::Sif1 && k + 1 != packets)
						w = (w & ~3u) ? (w & ~3u) : 4;
					s.words.push_back(w);
				}
				s.midIrq = packets > 1 && rng.Next() % 4 == 0;
				s.scanRunning = rng.Next() % 8 == 0;
				s.iopSlice = rng.Next() % 4 == 0;
				char name[32];
				std::snprintf(name, sizeof(name), "sweep_%s_%03d", chan == Chan::Sif0 ? "sif0" : "sif1", i);
				s.name = name;
				s.sweep = true;
				all.push_back(std::move(s));
			}
		}
		return all;
	}

	struct Golden
	{
		const char* name;
		u64 eeram, iopram, eechan, iopdma, sifs, sched, hw, trace;
	};

	const Golden kGolden[] = {
		{nullptr, 0, 0, 0, 0, 0, 0, 0, 0},
#include "sif_pump_golden.inc"
	};

	const Golden* FindGolden(const std::string& name)
	{
		for (const Golden& g : kGolden)
			if (g.name && name == g.name)
				return &g;
		return nullptr;
	}

	// The sweep scripts are recorded as one hash each, of all eight values above.
	struct SweepGolden
	{
		const char* name;
		u64 all;
	};

	const SweepGolden kSweepGolden[] = {
		{nullptr, 0},
#include "sif_pump_sweep_golden.inc"
	};

	const SweepGolden* FindSweepGolden(const std::string& name)
	{
		for (const SweepGolden& g : kSweepGolden)
			if (g.name && name == g.name)
				return &g;
		return nullptr;
	}

	u64 Combine(const Hashes& h)
	{
		Digest d;
		for (const u64 v : {h.eeram, h.iopram, h.eechan, h.iopdma, h.sifs, h.sched, h.hw, h.trace})
			d.Add(v);
		return d.Value();
	}

	std::string SweepRow(const Script& s, const Hashes& h)
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf), "\t{\"%s\", 0x%016llxull}, // %s\n", s.name.c_str(), static_cast<unsigned long long>(Combine(h)), Describe(s).c_str());
		return buf;
	}

	std::string Row(const std::string& name, const Hashes& h, const std::string& summary = std::string())
	{
		char buf[768];
		std::snprintf(buf, sizeof(buf),
			"\t{\"%s\", 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull}, // %s\n",
			name.c_str(), static_cast<unsigned long long>(h.eeram), static_cast<unsigned long long>(h.iopram),
			static_cast<unsigned long long>(h.eechan), static_cast<unsigned long long>(h.iopdma),
			static_cast<unsigned long long>(h.sifs), static_cast<unsigned long long>(h.sched),
			static_cast<unsigned long long>(h.hw), static_cast<unsigned long long>(h.trace), summary.c_str());
		return buf;
	}

	// Whether the script is one whose finished transfer can be checked from the script alone. A random
	// chain can end a side early (an IRQ tag on a TIE channel), or overfill the FIFO, and then the
	// recording is all there is to compare with.
	bool FinishesCleanly(const Script& s)
	{
		return !s.sweep && !s.stall && !s.normalMode && (s.term == Term::Both || s.term == Term::TieIrq);
	}

	// A pump that stops returning would otherwise hang the whole test binary until the CI timeout.
	// The watchdog turns it into an abort that names the script.
	class Watchdog
	{
	public:
		void Start()
		{
			m_stop = false;
			m_thread = std::thread([this] {
				u64 seen = m_beat.load();
				int idle = 0;
				while (!m_stop)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					const u64 now = m_beat.load();
					idle = (now == seen) ? idle + 1 : 0;
					seen = now;
					if (idle > 300)
					{
						std::fprintf(stderr, "sif_pump_tests: script %s did not finish in 30 s; a pump is not returning\n", m_current.c_str());
						std::abort();
					}
				}
			});
		}
		void Stop()
		{
			m_stop = true;
			if (m_thread.joinable())
				m_thread.join();
		}
		void Begin(const std::string& name)
		{
			m_current = name; // read by the watchdog thread only after a 30 s silence
			m_beat++;
		}

	private:
		std::thread m_thread;
		std::atomic<bool> m_stop{false};
		std::atomic<u64> m_beat{0};
		std::string m_current;
	};
	Watchdog s_watchdog;

	class SifPump : public ::testing::TestWithParam<Script>
	{
	protected:
		static void SetUpTestSuite()
		{
			s_ready = BringUpMachine();
			s_watchdog.Start();
		}
		static void TearDownTestSuite()
		{
			s_watchdog.Stop();
			TearDownMachine();
			s_ready = false;
		}
		void SetUp() override { ASSERT_TRUE(s_ready) << "could not allocate the EE and IOP RAM blocks"; }

		static bool s_ready;
	};
	bool SifPump::s_ready = false;

	// A transfer that finished has to look finished: the words arrived where the tags said, both sides
	// went idle, both completion interrupts are pending, and the pump cleared its F240 bits.
	void ExpectFinished(const Script& s, const Built& b)
	{
		const u8* const dstMem = (s.chan == Chan::Sif0) ? eeMem->Main : iopMem->Main;
		for (size_t i = 0; i < b.payload.packets.size(); i++)
		{
			// SIF1 hands the IOP a multiple of four words per packet.
			const u32 w = static_cast<u32>(b.payload.packets[i].size());
			const u32 got = (s.chan == Chan::Sif0) ? w : (w & ~3u);
			for (u32 j = 0; j < got; j++)
				ASSERT_EQ(b.payload.packets[i][j], Load<u32>(dstMem, b.payload.dst[i] + 4 * j)) << "packet " << i << " word " << j;
		}

		const _sif& sif = (s.chan == Chan::Sif0) ? sif0 : sif1;
		EXPECT_FALSE(sif.ee.busy);
		EXPECT_FALSE(sif.iop.busy);
		EXPECT_FALSE(sif.ee.end);
		EXPECT_FALSE(sif.iop.end);

		const EE_EventType eeEvt = (s.chan == Chan::Sif0) ? DMAC_SIF0 : DMAC_SIF1;
		const IopEventId iopEvt = (s.chan == Chan::Sif0) ? IopEvt_SIF0 : IopEvt_SIF1;
		EXPECT_NE(0u, cpuRegs.interrupt & (1u << eeEvt)) << "EE completion not scheduled";
		EXPECT_NE(0u, psxRegs.interrupt & (1u << iopEvt)) << "IOP completion not scheduled";

		const u32 bits = (s.chan == Chan::Sif0) ? 0x2020u : 0x4040u;
		EXPECT_EQ(0u, psHu32(SBUS_F240) & bits);
	}
} // namespace

TEST_P(SifPump, MatchesRecordedBehaviour)
{
	const Script& s = GetParam();
	s_watchdog.Begin(s.name);
	const RunResult r = RunScript(s);

	if (FinishesCleanly(s))
		ExpectFinished(s, r.built);

	if (s.sweep)
	{
		const SweepGolden* sg = FindSweepGolden(s.name);
		if (!sg)
			ADD_FAILURE() << "no recorded value for " << s.name << "; this run gives\n"
						  << SweepRow(s, r.h);
		else if (sg->all != Combine(r.h))
			ADD_FAILURE() << s.name << " (" << Describe(s) << ") now gives\n"
						  << SweepRow(s, r.h) << "its parts are\n"
						  << Row(s.name, r.h, r.summary);
		return;
	}

	const Golden* g = FindGolden(s.name);
	if (!g)
	{
		ADD_FAILURE() << "no recorded values for " << s.name << "; this run gives\n"
					  << Row(s.name, r.h, r.summary);
		return;
	}
	bool same = true;
#define SIF_PUMP_FIELD(f, what) \
	do \
	{ \
		EXPECT_EQ(g->f, r.h.f) << what; \
		same = same && g->f == r.h.f; \
	} while (0)
	SIF_PUMP_FIELD(eeram, "EE RAM");
	SIF_PUMP_FIELD(iopram, "IOP RAM");
	SIF_PUMP_FIELD(eechan, "EE channel registers (sif0ch, sif1ch, sif2dma)");
	SIF_PUMP_FIELD(iopdma, "IOP channel registers (hw_dma9, hw_dma10)");
	SIF_PUMP_FIELD(sifs, "sif0, sif1, sif2 structures");
	SIF_PUMP_FIELD(sched, "scheduled interrupts");
	SIF_PUMP_FIELD(hw, "hardware register pages");
	SIF_PUMP_FIELD(trace, "state after each entry call");
#undef SIF_PUMP_FIELD
	if (!same)
		ADD_FAILURE() << s.name << " now gives\n"
					  << Row(s.name, r.h, r.summary);
}

INSTANTIATE_TEST_SUITE_P(Scripts, SifPump, ::testing::ValuesIn(AllScripts()),
	[](const ::testing::TestParamInfo<Script>& info) { return info.param.name; });
INSTANTIATE_TEST_SUITE_P(Sweep, SifPump, ::testing::ValuesIn(SweepScripts()),
	[](const ::testing::TestParamInfo<Script>& info) { return info.param.name; });

// The recorded table and the script list have to agree both ways, so a deleted script cannot leave a
// row behind to mislead, and a new script cannot run without a recording.
TEST(SifPumpGoldens, TableAndScriptsAgree)
{
	const std::vector<Script> scripts = AllScripts();
	for (const Script& s : scripts)
		EXPECT_NE(nullptr, FindGolden(s.name)) << "no row for script " << s.name;
	size_t rows = 0;
	for (const Golden& g : kGolden)
	{
		if (!g.name)
			continue;
		rows++;
		bool found = false;
		for (const Script& s : scripts)
			found = found || s.name == g.name;
		EXPECT_TRUE(found) << "row for a script that no longer exists: " << g.name;
	}
	EXPECT_EQ(scripts.size(), rows);
}

TEST(SifPumpGoldens, SweepTableAndScriptsAgree)
{
	const std::vector<Script> scripts = SweepScripts();
	for (const Script& s : scripts)
		EXPECT_NE(nullptr, FindSweepGolden(s.name)) << "no row for script " << s.name;
	size_t rows = 0;
	for (const SweepGolden& g : kSweepGolden)
	{
		if (!g.name)
			continue;
		rows++;
		bool found = false;
		for (const Script& s : scripts)
			found = found || s.name == g.name;
		EXPECT_TRUE(found) << "row for a script that no longer exists: " << g.name;
	}
	EXPECT_EQ(scripts.size(), rows);
	for (const SweepGolden& a : kSweepGolden)
		for (const SweepGolden& b : kSweepGolden)
			if (a.name && b.name && &a < &b)
				EXPECT_NE(a.all, b.all) << a.name << " and " << b.name << " are recorded as identical";
}

// The scripts must differ from each other in what they leave behind, or the table says less than it
// seems to. Two scripts with the same RAM, FIFO and schedule would be one script.
TEST(SifPumpGoldens, ScriptsAreDistinguishable)
{
	for (const Golden& a : kGolden)
	{
		if (!a.name)
			continue;
		for (const Golden& b : kGolden)
		{
			if (!b.name || &a >= &b)
				continue;
			const bool same = a.eeram == b.eeram && a.iopram == b.iopram && a.sifs == b.sifs && a.sched == b.sched && a.trace == b.trace;
			EXPECT_FALSE(same) << a.name << " and " << b.name << " are recorded as identical";
		}
	}
}

// Prints the rows for sif_pump_sweep_golden.inc. Disabled; see the top of the file.
TEST(SifPumpRecord, DISABLED_PrintSweepGoldens)
{
	ASSERT_TRUE(BringUpMachine());
	for (const Script& s : SweepScripts())
		std::fputs(SweepRow(s, RunScript(s).h).c_str(), stdout);
	TearDownMachine();
}

// Prints the rows for sif_pump_golden.inc. Disabled; see the top of the file.
TEST(SifPumpRecord, DISABLED_PrintGoldens)
{
	ASSERT_TRUE(BringUpMachine());
	for (const Script& s : AllScripts())
	{
		const RunResult r = RunScript(s);
		std::fputs(Row(s.name, r.h, r.summary).c_str(), stdout);
	}
	TearDownMachine();
}
