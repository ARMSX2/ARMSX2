// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ee_divtrace.h"

#include "Memory.h"
#include "MemoryTypes.h"
#include "VUmicro.h"
#include "Counters.h"
#include "IopCounters.h"
#include "IopMem.h"
#include "R3000A.h"

#include <cstring>

// Self-contained xxh3 (same guard pattern as GS/GSXXH.h) for the memory hash.
#ifndef XXH_versionNumber
	#define XXH_STATIC_LINKING_ONLY 1
	#define XXH_INLINE_ALL 1
	#include <xxhash.h>
#endif

#ifdef PCSX2_RECOMPILER_TESTS
#include "Common.h"
#include "SaveState.h"
#include "Sif.h"
#include "SIO/Multitap/MultitapProtocol.h"
#include "SIO/Sio0.h"
#include "SIO/Sio2.h"
#include "SPU2/spu2.h"
#include "StateWrapper.h"

extern u64 g_twinEeEventTests;
extern u64 g_twinIopSlices;
extern s16 _spu2mem[0x200000 / sizeof(s16)]; // SPU2/spu2sys.cpp; SPU2/defs.h is private to the SPU2
#endif

namespace ee_divtrace
{
	std::atomic<bool>       g_enabled{false};
	bool                    g_emit_block_hook = false;
	bool                    g_fp_exclude = false;
	bool                    g_vu0_include = false;
	bool                    g_vu0_full = false;
	bool                    g_sys_full = false;
	bool                    g_skip_flushcache_syscall = false;
	std::vector<Sample>     g_stream;
	std::vector<FullSnap>   g_snaps;
	u32                     g_full_lo = 0;
	u32                     g_full_hi = 0;

	// FNV-1a style mix, matching microVU_Divtrace so the two diagnostics read
	// the same way.
	static inline u64 hash_mix(u64 h, u32 v)
	{
		h ^= static_cast<u64>(v);
		h *= 1099511628211ull;
		return h;
	}
	static inline u64 hash_mix64(u64 h, u64 v)
	{
		h = hash_mix(h, static_cast<u32>(v));
		h = hash_mix(h, static_cast<u32>(v >> 32));
		return h;
	}

	u64 FingerprintCpu()
	{
		u64 h = 1469598103934665603ull;
		// GPRs — full 128 bits each (.UD[0]/.UD[1]).
		for (int i = 0; i < 32; ++i)
		{
			// Skip k0/k1 (26/27): the EE kernel exception handler loads EPC/Cause
			// into these reserved scratch registers, so they inherit the same
			// cycle-phase interrupt noise as EPC and linger into user code until
			// overwritten; they reconverge by frame end. Excluding them from the
			// alignment fingerprint lets the zoom walk through every interrupt
			// handler. (The frame-level full-snapshot comparison still reports them.)
			if (i == 26 || i == 27)
				continue;
			h = hash_mix64(h, cpuRegs.GPR.r[i].UD[0]);
			h = hash_mix64(h, cpuRegs.GPR.r[i].UD[1]);
		}
		h = hash_mix64(h, cpuRegs.HI.UD[0]);
		h = hash_mix64(h, cpuRegs.HI.UD[1]);
		h = hash_mix64(h, cpuRegs.LO.UD[0]);
		h = hash_mix64(h, cpuRegs.LO.UD[1]);
		// CP0 — skip the dispatcher-driven counters:
		//   1 Random, 9 Count, 11 Compare, plus 13 Cause + 14 EPC (interrupt
		//   TIMING: EPC/Cause IP bits are a pure function of cpuRegs.cycle phase,
		//   so JIT/interp take the same interrupt a few cycles apart in an idle
		//   poll loop; excluding them keeps the streams aligned across every
		//   interrupt instead of breaking at each one).
		for (int i = 0; i < 32; ++i)
		{
			if (i == 1 || i == 9 || i == 11 || i == 13 || i == 14)
				continue;
			h = hash_mix(h, cpuRegs.CP0.r[i]);
		}
		// FPU register file + accumulator (control regs are excluded). Omitted
		// when g_fp_exclude is set, so the alignment walk skips past FP-register
		// divergences to hunt a non-FP (integer/control) divergence.
		if (!g_fp_exclude)
		{
			for (int i = 0; i < 32; ++i)
				h = hash_mix(h, fpuRegs.fpr[i].Word());
			h = hash_mix(h, fpuRegs.ACC.Word());
		}
		// VU0 macro-visible state — opt-in (EERUNNER_VU0FP): catches COP2 macro
		// divergence at the producing block instead of downstream in GPRs/memory.
		if (g_vu0_include)
		{
			for (int i = 0; i < 32; ++i)
			{
				h = hash_mix64(h, VU0.VF[i].UD[0]);
				h = hash_mix64(h, VU0.VF[i].UD[1]);
			}
			h = hash_mix64(h, VU0.ACC.UD[0]);
			h = hash_mix64(h, VU0.ACC.UD[1]);
			h = hash_mix(h, VU0.VI[REG_STATUS_FLAG].UL);
			h = hash_mix(h, VU0.VI[REG_MAC_FLAG].UL);
			h = hash_mix(h, VU0.VI[REG_CLIP_FLAG].UL);
		}
		if (g_vu0_full)
		{
			for (int i = 0; i < 32; ++i)
				h = hash_mix(h, VU0.VI[i].UL);
			h = hash_mix64(h, VU0.cycle);
			h = hash_mix(h, VU0.flags);
			if (!g_vu0_include)
			{
				for (int i = 0; i < 32; ++i)
				{
					h = hash_mix64(h, VU0.VF[i].UD[0]);
					h = hash_mix64(h, VU0.VF[i].UD[1]);
				}
				h = hash_mix64(h, VU0.ACC.UD[0]);
				h = hash_mix64(h, VU0.ACC.UD[1]);
			}
			h = hash_mix64(h, XXH3_64bits(VU0.Mem, VU0_MEMSIZE));
			h = hash_mix64(h, XXH3_64bits(VU0.Micro, VU0_PROGSIZE));
		}
		h = hash_mix(h, cpuRegs.pc);
		h = hash_mix(h, cpuRegs.sa);
		if (g_sys_full)
			h = hash_mix64(h, FingerprintSystem());
		return h;
	}

	template <typename T>
	static inline u64 hash_words(u64 h, const T& v)
	{
		static_assert(sizeof(T) % 4 == 0);
		const u32* w = reinterpret_cast<const u32*>(&v);
		for (size_t i = 0; i < sizeof(T) / 4; ++i)
			h = hash_mix(h, w[i]);
		return h;
	}

	u64 FingerprintSystem()
	{
		u64 h = 1469598103934665603ull;
		// EE: the whole register struct. Every field is a u32/u64 laid out
		// without padding, and it holds no host pointers.
		h = hash_words(h, cpuRegs);
		h = hash_words(h, fpuRegs);
		h = hash_mix(h, static_cast<u32>(EEsCycle));
		h = hash_mix64(h, EEoCycle);
		h = hash_mix64(h, nextStartCounter);
		h = hash_mix(h, static_cast<u32>(nextDeltaCounter));
		for (const Counter& c : counters)
		{
			h = hash_mix(h, c.count);
			h = hash_mix(h, c.modeval);
			h = hash_mix(h, c.target);
			h = hash_mix(h, c.hold);
			h = hash_mix(h, c.rate);
			h = hash_mix(h, c.interrupt);
			h = hash_mix64(h, c.startCycle);
		}
		// IOP: field by field, since psxRegisters has padding.
		h = hash_words(h, psxRegs.GPR);
		h = hash_words(h, psxRegs.CP0);
		h = hash_words(h, psxRegs.CP2D);
		h = hash_words(h, psxRegs.CP2C);
		h = hash_mix(h, psxRegs.pc);
		h = hash_mix(h, psxRegs.code);
		h = hash_mix64(h, psxRegs.cycle);
		h = hash_mix(h, psxRegs.interrupt);
		h = hash_mix(h, psxRegs.pcWriteback);
		h = hash_mix64(h, psxRegs.iopNextEventCycle);
		h = hash_mix(h, static_cast<u32>(psxRegs.iopBreak));
		h = hash_mix(h, static_cast<u32>(psxRegs.iopCycleEE));
		h = hash_mix(h, psxRegs.iopCycleEECarry);
		for (int i = 0; i < 32; ++i)
		{
			h = hash_mix64(h, psxRegs.sCycle[i]);
			h = hash_mix(h, static_cast<u32>(psxRegs.eCycle[i]));
		}
		h = hash_mix64(h, psxNextStartCounter);
		h = hash_mix(h, static_cast<u32>(psxNextDeltaCounter));
		for (const psxCounter& c : psxCounters)
		{
			h = hash_mix64(h, c.count);
			h = hash_mix64(h, c.target);
			h = hash_mix(h, c.rate);
			h = hash_mix(h, c.interrupt);
			h = hash_mix64(h, c.startCycle);
			h = hash_mix(h, static_cast<u32>(c.deltaCycles));
			h = hash_mix(h, c.mode.modeval);
		}
		for (int i = 0; i < 32; ++i)
		{
			h = hash_mix64(h, VU1.VF[i].UD[0]);
			h = hash_mix64(h, VU1.VF[i].UD[1]);
			h = hash_mix(h, VU1.VI[i].UL);
		}
		h = hash_mix64(h, VU1.ACC.UD[0]);
		h = hash_mix64(h, VU1.ACC.UD[1]);
		h = hash_mix64(h, VU1.cycle);
		h = hash_mix(h, VU1.flags);
#ifdef PCSX2_RECOMPILER_TESTS
		h = hash_mix64(h, g_twinEeEventTests);
		h = hash_mix64(h, g_twinIopSlices);
#endif
		return h;
	}

#ifdef PCSX2_RECOMPILER_TESTS
	// Takes the cdvd freeze the way a savestate does, but hashes the bytes
	// instead of storing them. SaveStateBase::cdvdFreeze is protected, hence the
	// subclass. It reports IsSaving() == true, so cdvdFreeze only reads: its
	// loading branch (re-read the current track from the disc) never runs.
	static SaveStateBase::VmStateBuffer s_unused_state_buffer; // SaveStateBase wants one; the Freeze calls here never touch it

	class HashingSaveState final : public SaveStateBase
	{
	public:
		HashingSaveState()
			: SaveStateBase(s_unused_state_buffer)
		{
			XXH3_64bits_reset(&m_xxh);
		}

		void FreezeMem(void* data, int size) override { XXH3_64bits_update(&m_xxh, data, static_cast<size_t>(size)); }
		bool IsSaving() const override { return true; }

		u64 HashCdvd()
		{
			return cdvdFreeze() ? XXH3_64bits_digest(&m_xxh) : 0;
		}

	private:
		XXH3_state_t m_xxh;
	};

	// Field by field: _sif has two bytes of padding after each pair of bools.
	static u64 hash_sif(u64 h, const _sif& s)
	{
		static_assert(sizeof(_sif) == 580, "_sif changed: add the new field to hash_sif");
		h = hash_words(h, s.fifo);
		h = hash_mix(h, s.ee.end);
		h = hash_mix(h, s.ee.busy);
		h = hash_mix(h, static_cast<u32>(s.ee.cycles));
		h = hash_mix(h, s.iop.end);
		h = hash_mix(h, s.iop.busy);
		h = hash_mix(h, static_cast<u32>(s.iop.cycles));
		h = hash_mix(h, static_cast<u32>(s.iop.writeJunk));
		h = hash_mix(h, static_cast<u32>(s.iop.counter));
		h = hash_words(h, s.iop.data);
		return h;
	}

	IopFingerprint FingerprintIop()
	{
		IopFingerprint fp;

		// SPU2: the savestate block, and the sample memory on its own so it stays
		// covered if the block's layout ever stops carrying it.
		static std::vector<u8> s_spu2_block;
		SPU2freezeForFingerprint(s_spu2_block);
		fp.spu2 = XXH3_64bits(s_spu2_block.data(), s_spu2_block.size());
		fp.spu2 = XXH3_64bits_withSeed(_spu2mem, sizeof(_spu2mem), fp.spu2);

		u64 h = 1469598103934665603ull;
		h = hash_sif(h, sif0);
		h = hash_sif(h, sif1);
		h = hash_sif(h, sif2);
		fp.sif = h;

		// The StateWrapper block of SaveStateBase::FreezeInternals, in Write mode.
		StateWrapper::VectorMemoryStream stream(4096);
		StateWrapper sw(&stream, StateWrapper::Mode::Write, g_SaveVersion);
		bool ok = g_Sio0.DoState(sw);
		ok = ok && g_Sio2.DoState(sw);
		ok = ok && g_MultitapArr.at(0).DoState(sw);
		ok = ok && g_MultitapArr.at(1).DoState(sw);
		fp.sio = ok ? XXH3_64bits(stream.GetBuffer().data(), stream.GetBuffer().size()) : 0;

		HashingSaveState cdvd_state;
		fp.cdvd = cdvd_state.HashCdvd();
		return fp;
	}
#endif

	void RecordSample(u32 pc)
	{
		const size_t idx = g_stream.size();
		Sample s;
		s.cycle = cpuRegs.cycle;
		s.fp    = FingerprintCpu();
		s.pc    = pc;
		s._pad  = 0;
		g_stream.push_back(s);

		if (idx >= g_full_lo && idx < g_full_hi)
		{
			FullSnap& fs = g_snaps[idx - g_full_lo];
			std::memcpy(&fs.cpu, &cpuRegs, sizeof(cpuRegisters));
			std::memcpy(&fs.fpu, &fpuRegs, sizeof(fpuRegisters));
			std::memcpy(fs.vu0_vf, VU0.VF, 32 * 16);
			std::memcpy(fs.vu0_vf + 32 * 16, &VU0.ACC, 16);
			fs.vu0_vi_status = VU0.VI[REG_STATUS_FLAG].UL;
			fs.vu0_vi_mac    = VU0.VI[REG_MAC_FLAG].UL;
			fs.vu0_vi_clip   = VU0.VI[REG_CLIP_FLAG].UL;
			fs._pad0 = 0;
			fs.cycle = cpuRegs.cycle;
			fs.pc    = pc;
			fs._pad  = 0;
		}
	}

	u64 HashMemory()
	{
		if (!eeMem)
			return 0;
		// Real PS2 main RAM is 32 MB (TotalRam is the 128 MB address-wrap span;
		// only the first MainRam bytes are physical). Scratchpad is 16 KB.
		u64 h = XXH3_64bits(eeMem->Main, Ps2MemSize::MainRam);
		h = XXH3_64bits_withSeed(eeMem->Scratch, Ps2MemSize::Scratch, h);
		if (g_sys_full)
		{
			h = XXH3_64bits_withSeed(eeHw, Ps2MemSize::Hardware, h);
			if (iopMem)
			{
				h = XXH3_64bits_withSeed(iopMem->Main, Ps2MemSize::ExposedIopRam, h);
				h = XXH3_64bits_withSeed(iopMem->Sif, sizeof(iopMem->Sif), h);
			}
			h = XXH3_64bits_withSeed(iopHw, Ps2MemSize::IopHardware, h);
			h = XXH3_64bits_withSeed(VU1.Mem, VU1_MEMSIZE, h);
			h = XXH3_64bits_withSeed(VU1.Micro, VU1_PROGSIZE, h);
		}
		return h;
	}

	void Reset()
	{
		g_stream.clear();
		if (!g_snaps.empty())
			g_snaps.assign(g_snaps.size(), FullSnap{});
	}

	void ConfigureFullWindow(u32 lo, u32 len)
	{
		g_full_lo = lo;
		g_full_hi = lo + len;
		g_snaps.assign(len, FullSnap{});
	}

	void ReserveStream(size_t samples)
	{
		g_stream.reserve(samples);
	}
} // namespace ee_divtrace

extern "C" void ee_divtrace_jit_block_hook(u32 startpc)
{
	if (ee_divtrace::g_enabled.load(std::memory_order_relaxed))
		ee_divtrace::RecordSample(startpc);
}
