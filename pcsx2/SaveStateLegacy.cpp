// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Deserializer for legacy-format `PCSX2 Internal Structures.dat` blobs, as
// written by the AetherSX2/NetherSX2 era of upstream PCSX2:
//
//   major 0x9A2C — upstream PCSX2 at 0312e902 (AetherSX2 v1.5-era builds)
//   major 0x9A34 — upstream PCSX2 at 7e939b75 (NetherSX2 v2.1 build 4248)
//
// The reader consumes the old byte layout field-by-field directly into live
// emulator state. Old layouts are declared in OldState below, each cited to
// the upstream sha its layout was taken from and guarded by a static_assert
// of the byte-exact size proven against real state files. Two deviations from
// stock 0312e902 exist in every AetherSX2-written 0x9A2C file (byte-proven,
// not header-derived): see psxRegisters_9A2C and CyclesBlock_9A2C.
//
// Where a block is byte-identical across eras the existing freeze function is
// reused; everything else is remapped here. All 32-bit cycle counters widen
// relative to their domain base (WidenCycle) so wrap-straddling deltas
// survive the u32->u64 conversion.

#include "SaveStateLegacy.h"

#include "CDVD/CDVD.h"
#include "CDVD/CDVDcommon.h"
#include "CDVD/Ps1CD.h"
#include "Config.h"
#include "Host.h"
#include "Counters.h"
#include "GS.h"
#include "Gif.h"
#include "Gif_Unit.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "IPU/IPU_Fifo.h"
#include "IPU/IPU_MultiISA.h"
#include "IopBios.h"
#include "IopCounters.h"
#include "IopMem.h"
#include "MTVU.h"
#include "MemoryTypes.h"
#include "R3000A.h"
#include "R5900.h"
#include "SIO/Multitap/MultitapProtocol.h"
#include "SIO/Sio.h"
#include "SIO/Sio0.h"
#include "SIO/Sio2.h"
#include "SaveState.h"
#include "Sif.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include "ps2/BiosTools.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"

#include <cstring>
#include <memory>

bool SaveStateLegacy::IsSupportedVersion(u32 savever)
{
	const u32 major = savever >> 16;
	return major == 0x9A2C || major == 0x9A34;
}

namespace OldState
{
	// layout per upstream PCSX2 0312e902, pcsx2/R5900.h (32-bit cycle counters).
	// GPR/HI/LO/CP0/PERF register blocks are era-stable; the current types are
	// reused for them.
	struct cpuRegisters_9A2C
	{
		GPRregs GPR;
		GPR_reg HI;
		GPR_reg LO;
		CP0regs CP0;
		u32 sa;
		u32 IsDelaySlot;
		u32 pc;
		u32 code;
		PERFregs PERF;
		u32 eCycle[32];
		u32 sCycle[32];
		u32 cycle;
		u32 interrupt;
		int branch;
		int opmode;
		u32 tempcycles;
	};
	static_assert(sizeof(cpuRegisters_9A2C) == 984, "cpuRegisters at 0312e902 is 984 bytes");

	// layout per upstream PCSX2 7e939b75, pcsx2/R5900.h (cycle/writeback fields
	// moved in-struct at 0x9A31, still 32-bit).
	struct cpuRegisters_9A34
	{
		GPRregs GPR;
		GPR_reg HI;
		GPR_reg LO;
		CP0regs CP0;
		u32 sa;
		u32 IsDelaySlot;
		u32 pc;
		u32 code;
		PERFregs PERF;
		u32 eCycle[32];
		u32 sCycle[32];
		u32 cycle;
		u32 interrupt;
		int branch;
		int opmode;
		u32 tempcycles;
		u32 dmastall;
		u32 pcWriteback;
		u32 nextEventCycle;
		u32 lastEventCycle;
		u32 lastCOP0Cycle;
		u32 lastPERFCycle[2];
	};
	static_assert(sizeof(cpuRegisters_9A34) == 1008, "cpuRegisters at 7e939b75 is 1008 bytes");

	// The newer layout only appends, so the older one reads as a prefix of it
	// and both eras normalize onto the newer shape by copying that prefix.
	// Copying it means copying exactly this many bytes and no more: the older
	// struct is four bytes longer than its last field, and the newer one puts
	// dmastall in that padding, so a sizeof()-sized copy would land padding in
	// a live field.
	static constexpr size_t CPUREGS_SHARED_BYTES = offsetof(cpuRegisters_9A2C, tempcycles) + sizeof(cpuRegisters_9A2C::tempcycles);
	static_assert(offsetof(cpuRegisters_9A2C, tempcycles) == offsetof(cpuRegisters_9A34, tempcycles),
		"cpuRegisters at 7e939b75 appends to 0312e902's layout without moving a field");
	static_assert(offsetof(cpuRegisters_9A34, dmastall) == CPUREGS_SHARED_BYTES,
		"the fields 7e939b75 appends start where 0312e902's last field ends");

	// layout per upstream PCSX2 0312e902, pcsx2/R3000A.h — plus one extra u32
	// observed in every AetherSX2-written 0x9A2C state (796 bytes on file, not
	// upstream's 792), sitting exactly where upstream later placed pcWriteback
	// (fd194124a9). Named accordingly; its value is not used.
	struct psxRegisters_9A2C
	{
		GPRRegs GPR;
		CP0Regs CP0;
		CP2Data CP2D;
		CP2Ctrl CP2C;
		u32 pc;
		u32 code;
		u32 cycle;
		u32 interrupt;
		u32 pcWriteback;
		u32 sCycle[32];
		s32 eCycle[32];
	};
	static_assert(sizeof(psxRegisters_9A2C) == 796, "psxRegisters as written by AetherSX2 0x9A2C states is 796 bytes");

	// layout per upstream PCSX2 7e939b75, pcsx2/R3000A.h.
	struct psxRegisters_9A34
	{
		GPRRegs GPR;
		CP0Regs CP0;
		CP2Data CP2D;
		CP2Ctrl CP2C;
		u32 pc;
		u32 code;
		u32 cycle;
		u32 interrupt;
		u32 pcWriteback;
		u32 iopNextEventCycle;
		s32 iopBreak;
		s32 iopCycleEE;
		u32 sCycle[32];
		s32 eCycle[32];
	};
	static_assert(sizeof(psxRegisters_9A34) == 808, "psxRegisters at 7e939b75 is 808 bytes");
	// Here the newer layout inserts rather than appends, so only the head up to
	// the cycle arrays is shared; the arrays are copied separately.
	static_assert(offsetof(psxRegisters_9A2C, sCycle) == offsetof(psxRegisters_9A34, iopNextEventCycle),
		"psxRegisters at 7e939b75 inserts its new fields after the shared head");
	static_assert(sizeof(psxRegisters_9A2C::sCycle) == sizeof(psxRegisters_9A34::sCycle) &&
					  sizeof(psxRegisters_9A2C::eCycle) == sizeof(psxRegisters_9A34::eCycle),
		"the IOP cycle arrays did not change shape between the two eras");

	// layout per upstream PCSX2 0312e902, pcsx2/SaveState.cpp "Cycles" block.
	// The COP0/PERF last-cycle area is 8 bytes in every AetherSX2-written file
	// where stock upstream writes 12 (s_iLastCOP0Cycle + s_iLastPERFCycle[2]);
	// byte-proven via the duplicated nextCounter/nextsCounter pair that
	// rcntFreeze rewrites later in the same blob. The area is discarded on
	// load (the mapped state resyncs those cycles to the load point).
	struct CyclesBlock_9A2C
	{
		s32 EEsCycle;
		u32 EEoCycle;
		s32 iopCycleEE;
		s32 iopBreak;
		u32 g_nextEventCycle;
		u32 g_iopNextEventCycle;
		u32 cop0PerfCycleArea[2];
		s32 nextCounter;
		u32 nextsCounter;
		u32 psxNextsCounter;
		s32 psxNextCounter;
	};
	static_assert(sizeof(CyclesBlock_9A2C) == 48, "Cycles block in AetherSX2 0x9A2C states is 48 bytes");

	// layout per upstream PCSX2 7e939b75, pcsx2/SaveState.cpp "Cycles" block
	// (the moved fields live in the CPU structs at this vintage).
	struct CyclesBlock_9A34
	{
		s32 EEsCycle;
		u32 EEoCycle;
		s32 nextCounter;
		u32 nextsCounter;
		u32 psxNextsCounter;
		s32 psxNextCounter;
	};
	static_assert(sizeof(CyclesBlock_9A34) == 24, "Cycles block at 7e939b75 is 24 bytes");

	// layout per upstream PCSX2 0312e902, pcsx2/R5900.h struct tlbs: the four
	// raw registers followed by eight derived caches (recomputed today).
	struct tlbs_9A2C
	{
		u32 PageMask, EntryHi;
		u32 EntryLo0, EntryLo1;
		u32 Mask, nMask;
		u32 G;
		u32 ASID;
		u32 VPN2;
		u32 PFN0;
		u32 PFN1;
		u32 S;
	};
	static_assert(sizeof(tlbs_9A2C) == 48, "tlbs at 0312e902 is 48 bytes per entry");

	// layout per upstream PCSX2 0312e902, pcsx2/Counters.h.
	struct Counter_9A2C
	{
		u32 count;
		u32 modeval;
		u32 target, hold;
		u32 rate, interrupt;
		u32 sCycleT;
	};
	static_assert(sizeof(Counter_9A2C) == 28, "Counter at 0312e902 is 28 bytes");

	struct SyncCounter_9A2C
	{
		u32 Mode;
		u32 sCycle;
		s32 CycleT;
	};
	static_assert(sizeof(SyncCounter_9A2C) == 12, "SyncCounter at 0312e902 is 12 bytes");

	// layout per upstream PCSX2 0312e902, pcsx2/IopCounters.h.
	struct psxCounter_9A2C
	{
		u64 count, target;
		u32 mode;
		u32 rate, interrupt;
		u32 sCycleT;
		s32 CycleT;
	};
	static_assert(sizeof(psxCounter_9A2C) == 40, "psxCounter at 0312e902 is 40 bytes");

	// Internal mode bits of the legacy psxCounter.mode word that do not map
	// positionally into today's psxCounterMode bitfield (which mirrors the
	// hardware register bits 0-14 exactly).
	static constexpr u32 PSXCNT_STOPPED_9A2C = 0x10000000u; // IOPCNT_STOPPED at 0312e902
	static constexpr u32 PSXCNT_HW_MODE_MASK = 0x7FFFu; // bits 0-14: hw register incl. flag + prescale bits

	// layout per upstream PCSX2 0312e902, pcsx2/VU.h (32-bit pipe cycles).
	struct fmacPipe_9A2C
	{
		u32 regupper;
		u32 reglower;
		int flagreg;
		u32 xyzwupper;
		u32 xyzwlower;
		u32 sCycle;
		u32 Cycle;
		u32 macflag;
		u32 statusflag;
		u32 clipflag;
	};
	static_assert(sizeof(fmacPipe_9A2C) == 40, "fmacPipe at 0312e902 is 40 bytes");

	struct fdivPipe_9A2C
	{
		int enable;
		REG_VI reg;
		u32 sCycle;
		u32 Cycle;
		u32 statusflag;
	};
	static_assert(sizeof(fdivPipe_9A2C) == 32, "fdivPipe at 0312e902 is 32 bytes");

	struct efuPipe_9A2C
	{
		int enable;
		REG_VI reg;
		u32 sCycle;
		u32 Cycle;
	};
	static_assert(sizeof(efuPipe_9A2C) == 28, "efuPipe at 0312e902 is 28 bytes");

	struct ialuPipe_9A2C
	{
		int reg;
		u32 sCycle;
		u32 Cycle;
	};
	static_assert(sizeof(ialuPipe_9A2C) == 12, "ialuPipe at 0312e902 is 12 bytes");

	// layout per upstream PCSX2 0312e902, pcsx2/Vif_Dma.h. Same total size as
	// today's vifStruct, but the interior differs: today a TRXPOS register sits
	// inside the transfer-register union, shifting everything after BITBLTBUF.
	struct vifStruct_9A2C
	{
		alignas(16) u128 MaskRow;
		alignas(16) u128 MaskCol;

		struct
		{
			vifCode tag;
			int cmd;
			int pass;
			int cl;
			u8 usn;
			u8 start_aligned;
			u8 StructEnd;
		};

		int irq;

		bool done;
		tVIF_CTRL vifstalled;
		bool stallontag;
		bool waitforvu;
		int unpackcalls;
		tBITBLTBUF BITBLTBUF;
		tTRXREG TRXREG;
		u32 GSLastDownloadSize;

		tVIF_CTRL irqoffset;
		u32 vifpacketsize;
		u8 inprogress;
		u8 dmamode;

		bool queued_program;
		u32 queued_pc;
		bool queued_gif_wait;
	};
	static_assert(sizeof(vifStruct_9A2C) == 144, "vifStruct at 0312e902 is 144 bytes");

	// layout per upstream PCSX2 0312e902, pcsx2/CDVD/CDVD.h. Field-for-field
	// the same order as today's cdvdStruct under older names; RTCcount was an
	// int and RotSpeed did not exist yet.
	struct cdvdStruct_9A2C
	{
		u8 nCommand;
		u8 Ready;
		u8 Error;
		u8 IntrStat;
		u8 Status;
		u8 StatusSticky;
		u8 Type;
		u8 sCommand;
		u8 sDataIn;
		u8 sDataOut;
		u8 HowTo;

		u8 NCMDParam[16];
		u8 SCMDParam[16];
		u8 SCMDResult[16];

		u8 NCMDParamC;
		u8 NCMDParamP;
		u8 SCMDParamC;
		u8 SCMDParamP;
		u8 SCMDResultC;
		u8 SCMDResultP;

		u8 CBlockIndex;
		u8 COffset;
		u8 CReadWrite;
		u8 CNumBlocks;

		int RTCcount;
		cdvdRTC RTC;

		u32 Sector;
		int nSectors;
		int Readed;
		int Reading;
		int WaitingDMA;
		int ReadMode;
		int BlockSize;
		int Speed;
		int RetryCnt;
		int RetryCntP;
		int RErr;
		int SpindlCtrl;

		u8 Key[16];
		u8 KeyXor;
		u8 decSet;

		u8 mg_buffer[65536];
		int mg_size;
		int mg_maxsize;
		int mg_datatype;
		u8 mg_kbit[16];
		u8 mg_kcon[16];

		u8 TrayTimeout;
		u8 Action;
		u32 SeekToSector;
		u32 MaxSector;
		u32 ReadTime;
		bool Spinning;
		cdvdTrayTimer Tray;
		u8 nextSectorsBuffered;
		bool AbortRequested;
	};
	static_assert(sizeof(cdvdStruct_9A2C) == 65764, "cdvdStruct at 0312e902 is 65764 bytes");

	// SIO-era blob sizes; the payloads are discarded (SIO is reset to its
	// transfer-quiescent boot state on load), only the memcard CRCs matter.
	// layout per upstream PCSX2 0312e902, pcsx2/Sio.h + Sio.cpp sioFreeze /
	// pcsx2/IopSio2.h + IopSio2.cpp sio2Freeze.
	static constexpr int SIO_BLOB_9A2C = 540;
	static constexpr int SIO2_BLOB_9A2C = 8640;
	// layout per upstream PCSX2 7e939b75, pcsx2/Sio.h sioFreeze/sio2Freeze
	// (class Sio0 / class Sio2 raw dumps + two FreezeDeque<u8> fifos + CRCs).
	static constexpr int SIO0_BLOB_9A34 = 32;
	static constexpr int SIO2_BLOB_9A34 = 176;

	// vuJITFreeze payload: microVU's lpState (microRegInfo), whose size is
	// static_asserted at each vintage. Discarded on load — PreLoadPrep's
	// ClearCPUExecutionCaches already reset the VU recs to a zero lpState.
	static constexpr int MICROREGINFO_9A2C = 176; // 0312e902:pcsx2/x86/microVU_IR.h
	static constexpr int MICROREGINFO_9A34 = 160; // 7e939b75:pcsx2/x86/microVU_IR.h

	// Sizes of CURRENT types that the legacy byte layout pins, because the
	// reader either freezes them directly or reaches them through a freeze
	// function shared with the current format. They are era-invariant today;
	// if one ever changes upstream, the legacy blob desyncs mid-stream, so
	// pin them here to fail the build instead of the load.
	static_assert(sizeof(fpuRegisters) == 264, "fpuRegs block is 264 bytes in every era");
	static_assert(sizeof(VECTOR) == 16 && sizeof(REG_VI) == 16, "VU register views are 128-bit in every era");
	static_assert(sizeof(ipu_fifo) == 288 && sizeof(g_BP) == 48 && sizeof(decoder) == 3028 && sizeof(ipu_cmd) == 32,
		"IPU block payload is era-invariant");
	static_assert(sizeof(g_ipu_vqclut) == 32 && sizeof(g_ipu_thresh) == 4 && sizeof(coded_block_pattern) == 4,
		"IPU block payload is era-invariant");
	static_assert(sizeof(gifUnit.stat) == 4 && sizeof(gifUnit.gsSIGNAL) == 12 && sizeof(gifUnit.lastTranType) == 4,
		"Gif Unit block head is era-invariant apart from GS_FINISH");
	static_assert(sizeof(Gif_Path) - sizeof(Gif_Path_MTVU) == 120, "gifPathFreeze writes a 120-byte struct head in every era");
	static_assert(sizeof(sif0) == 580, "SIFdma block is era-invariant");
	static_assert(sizeof(gif) == 28 && sizeof(gif_fifo) == 260, "GIFdma block is era-invariant");
	static_assert(sizeof(cdr) == 39352, "cdrom block is era-invariant");
	static_assert(sizeof(iopMem->Sif) == 256, "IOP-Subsystems SIF window is era-invariant");
	static_assert(sizeof(gsVideoMode) == 4 && sizeof(gsIsInterlaced) == 1, "video mode fields are era-invariant");
} // namespace OldState

using SaveStateLegacy::WidenCycle;

// Staged EE/IOP register state: blocks 1 (cpuRegs) and 2 (Cycles) interlock
// (fields moved between them at 0x9A31), so both are read before either is
// committed to the live registers.
namespace
{
	struct StagedRegs
	{
		// normalized (version-independent) view of the legacy data
		OldState::cpuRegisters_9A34 cpu; // 9A2C loads leave the tail fields unset
		OldState::psxRegisters_9A34 psx;
		fpuRegisters fpu;
		OldState::tlbs_9A2C tlb[48];
		bool allowParams1, allowParams2;
		u32 elfCRC;
		char discSerial[256];

		s32 EEsCycle;
		u32 EEoCycle;
	};
} // namespace

// Verifies the identity fields the legacy format embeds in the cpuRegs block
// against the running VM. A state from a different disc corrupts emulation,
// so a mismatch is a hard failure.
static bool CheckLegacyIdentity(const StagedRegs& st, Error* error)
{
	const std::string current_serial = VMManager::GetDiscSerial();
	const u32 current_crc = VMManager::GetCurrentCRC();

	if (current_serial.empty())
	{
		Console.Warning("(LegacyState) No disc serial available to verify state identity against.");
		return true;
	}

	if (current_serial != st.discSerial)
	{
		Error::SetStringFmt(error, TRANSLATE_FS("SaveState", "This AetherSX2 save state is for '{0}', but '{1}' is running."),
			st.discSerial, current_serial);
		return false;
	}

	// The ELF CRC only means anything once the VM has actually booted an ELF.
	// Loading a state at startup resets the VM first, which clears the CRC, so
	// an unknown CRC here is the normal case and is not a mismatch — the disc
	// serial checked above is what identifies the game.
	if (current_crc != 0 && st.elfCRC != current_crc)
	{
		Error::SetStringFmt(error, TRANSLATE_FS("SaveState", "This AetherSX2 save state is for CRC {0:08X}, but the running game is CRC {1:08X}."),
			st.elfCRC, current_crc);
		return false;
	}

	return true;
}

bool SaveStateBase::FreezeInternalsLegacy(Error* error)
{
	pxAssert(IsLoading());
	const u32 major = m_version >> 16;

	// Legacy states are always 32MB-EE-RAM states; memFreeze (which carries
	// the flag today) does not exist in them.
	if (Ps2MemSize::ExposedRam != Ps2MemSize::MainRam)
	{
		Error::SetString(error, "Legacy save states require 32MB memory mode, but the VM is using extra memory.");
		return false;
	}

	// ---------------------------------------------------------------- Block 1+2
	StagedRegs st = {};

	if (!FreezeTag("cpuRegs"))
		return false;

	if (major == 0x9A2C)
	{
		OldState::cpuRegisters_9A2C cpu;
		OldState::psxRegisters_9A2C psx;
		Freeze(cpu);
		Freeze(psx);
		std::memcpy(&st.cpu, &cpu, OldState::CPUREGS_SHARED_BYTES);
		std::memcpy(&st.psx, &psx, offsetof(OldState::psxRegisters_9A2C, sCycle));
		std::memcpy(st.psx.sCycle, psx.sCycle, sizeof(psx.sCycle));
		std::memcpy(st.psx.eCycle, psx.eCycle, sizeof(psx.eCycle));
	}
	else
	{
		Freeze(st.cpu);
		Freeze(st.psx);
	}

	Freeze(st.fpu);
	Freeze(st.tlb);
	Freeze(st.allowParams1);
	Freeze(st.allowParams2);
	FreezeSkip(2); // g_GameStarted, g_GameLoading — identity lives in the running VM
	Freeze(st.elfCRC);
	Freeze(st.discSerial);
	st.discSerial[sizeof(st.discSerial) - 1] = 0;

	if (!FreezeTag("Cycles"))
		return false;

	// The counter deadlines this block also carries are ignored: rcntFreeze and
	// the iopCounters block write the same four values later in the same blob,
	// which is what makes them a landmark for pinning the block's size.
	if (major == 0x9A2C)
	{
		OldState::CyclesBlock_9A2C cyc;
		Freeze(cyc);
		st.EEsCycle = cyc.EEsCycle;
		st.EEoCycle = cyc.EEoCycle;
		// fields that moved into the CPU structs at 0x9A31:
		st.psx.iopCycleEE = cyc.iopCycleEE;
		st.psx.iopBreak = cyc.iopBreak;
		st.cpu.nextEventCycle = cyc.g_nextEventCycle;
		st.psx.iopNextEventCycle = cyc.g_iopNextEventCycle;
		// cyc.cop0PerfCycleArea is discarded; COP0/PERF resync to the load point.
		st.cpu.lastCOP0Cycle = st.cpu.cycle;
		st.cpu.lastPERFCycle[0] = st.cpu.cycle;
		st.cpu.lastPERFCycle[1] = st.cpu.cycle;
	}
	else
	{
		OldState::CyclesBlock_9A34 cyc;
		Freeze(cyc);
		st.EEsCycle = cyc.EEsCycle;
		st.EEoCycle = cyc.EEoCycle;
	}

	if (!IsOkay())
		return false;

	if (!CheckLegacyIdentity(st, error))
		return false;

	// Commit. Domain bases for cycle widening: the old EE/IOP cycle values,
	// zero-extended (the absolute epoch is arbitrary; consistency is what
	// matters).
	const u32 ee_base = st.cpu.cycle;
	const u64 ee_new = static_cast<u64>(st.cpu.cycle);
	const u32 iop_base = st.psx.cycle;
	const u64 iop_new = static_cast<u64>(st.psx.cycle);
	const auto widen_ee = [&](u32 v) { return WidenCycle(v, ee_base, ee_new); };
	const auto widen_iop = [&](u32 v) { return WidenCycle(v, iop_base, iop_new); };

	cpuRegs.GPR = st.cpu.GPR;
	cpuRegs.HI = st.cpu.HI;
	cpuRegs.LO = st.cpu.LO;
	cpuRegs.CP0 = st.cpu.CP0;
	cpuRegs.sa = st.cpu.sa;
	cpuRegs.IsDelaySlot = st.cpu.IsDelaySlot;
	cpuRegs.pc = st.cpu.pc;
	cpuRegs.code = st.cpu.code;
	cpuRegs.PERF = st.cpu.PERF;
	std::memcpy(cpuRegs.eCycle, st.cpu.eCycle, sizeof(cpuRegs.eCycle));
	for (int i = 0; i < 32; i++)
		cpuRegs.sCycle[i] = widen_ee(st.cpu.sCycle[i]);
	cpuRegs.cycle = ee_new;
	cpuRegs.interrupt = st.cpu.interrupt;
	cpuRegs.branch = st.cpu.branch;
	cpuRegs.opmode = st.cpu.opmode;
	cpuRegs.tempcycles = st.cpu.tempcycles;
	cpuRegs.dmastall = st.cpu.dmastall; // zero for 0x9A2C, which has no such field
	cpuRegs.pcWriteback = 0;
	cpuRegs.nextEventCycle = widen_ee(st.cpu.nextEventCycle);
	cpuRegs.lastEventCycle = ee_new;
	cpuRegs.lastCOP0Cycle = widen_ee(st.cpu.lastCOP0Cycle);
	cpuRegs.lastPERFCycle[0] = widen_ee(st.cpu.lastPERFCycle[0]);
	cpuRegs.lastPERFCycle[1] = widen_ee(st.cpu.lastPERFCycle[1]);

	psxRegs.GPR = st.psx.GPR;
	psxRegs.CP0 = st.psx.CP0;
	psxRegs.CP2D = st.psx.CP2D;
	psxRegs.CP2C = st.psx.CP2C;
	psxRegs.pc = st.psx.pc;
	psxRegs.code = st.psx.code;
	psxRegs.cycle = iop_new;
	psxRegs.interrupt = st.psx.interrupt;
	psxRegs.pcWriteback = 0;
	psxRegs.iopNextEventCycle = widen_iop(st.psx.iopNextEventCycle);
	psxRegs.iopBreak = st.psx.iopBreak;
	psxRegs.iopCycleEE = st.psx.iopCycleEE;
	psxRegs.iopCycleEECarry = 0;
	for (int i = 0; i < 32; i++)
		psxRegs.sCycle[i] = widen_iop(st.psx.sCycle[i]);
	std::memcpy(psxRegs.eCycle, st.psx.eCycle, sizeof(psxRegs.eCycle));

	fpuRegs = st.fpu;

	for (int i = 0; i < 48; i++)
	{
		tlb[i].PageMask.UL = st.tlb[i].PageMask;
		tlb[i].EntryHi.UL = st.tlb[i].EntryHi;
		tlb[i].EntryLo0.UL = st.tlb[i].EntryLo0;
		tlb[i].EntryLo1.UL = st.tlb[i].EntryLo1;
	}
	// cachedTlbs is deliberately untouched: PostLoadPrep diffs tlb[] against
	// the pre-load backup and Unmap/MapTLB maintain the cache incrementally,
	// exactly as for an in-session TLB rewrite.

	AllowParams1 = st.allowParams1;
	AllowParams2 = st.allowParams2;

	EEsCycle = st.EEsCycle;
	EEoCycle = widen_ee(st.EEoCycle);

	// ------------------------------------------------------- EE-Subsystems
	if (!FreezeTag("EE-Subsystems"))
		return false;

	// rcnt (untagged). vSyncInfo and the legacy `gates` word are discarded:
	// both are recomputed (UpdateVSyncRate(true) in PostLoadPrep, cpuRcntSet
	// here).
	{
		OldState::Counter_9A2C oc[4];
		OldState::SyncCounter_9A2C oh, ov;
		s32 nc;
		u32 nsc;
		Freeze(oc);
		Freeze(oh);
		Freeze(ov);
		Freeze(nc);
		Freeze(nsc);
		FreezeSkip(40); // vSyncTimingInfo
		Freeze(gsVideoMode); // enum layout is unchanged since 0312e902
		Freeze(gsIsInterlaced);
		FreezeSkip(4); // gates
		if (!IsOkay())
			return false;

		for (int i = 0; i < 4; i++)
		{
			counters[i].count = oc[i].count;
			counters[i].modeval = oc[i].modeval;
			counters[i].target = oc[i].target;
			counters[i].hold = oc[i].hold;
			counters[i].rate = oc[i].rate;
			counters[i].interrupt = oc[i].interrupt;
			counters[i].startCycle = widen_ee(oc[i].sCycleT);
		}
		hsyncCounter.Mode = oh.Mode;
		hsyncCounter.startCycle = widen_ee(oh.sCycle);
		hsyncCounter.deltaCycles = oh.CycleT;
		vsyncCounter.Mode = ov.Mode;
		vsyncCounter.startCycle = widen_ee(ov.sCycle);
		vsyncCounter.deltaCycles = ov.CycleT;
		nextDeltaCounter = nc;
		nextStartCounter = widen_ee(nsc);
		// Rescheduling (cpuRcntSet) happens in PostLoadPrep via
		// UpdateVSyncRate(true), which also rebuilds vSyncInfo.
	}

	if (!gsFreeze())
		return false;

	// vuMicro (tagged): field-by-field in both eras; identical legacy layout
	// for 0x9A2C and 0x9A34. blockhasmbit was removed upstream and is
	// discarded; the 32-bit VU cycles widen in the EE domain (VUs are
	// scheduled against cpuRegs.cycle).
	if (!FreezeTag("vuMicroRegs"))
		return false;

	const auto legacyVuPipes = [&](VURegs& vu) {
		OldState::fmacPipe_9A2C fmac[4];
		OldState::fdivPipe_9A2C fdiv;
		OldState::efuPipe_9A2C efu;
		OldState::ialuPipe_9A2C ialu[4];
		Freeze(fmac);
		Freeze(vu.fmacreadpos);
		Freeze(vu.fmacwritepos);
		Freeze(vu.fmaccount);
		Freeze(fdiv);
		Freeze(efu);
		Freeze(ialu);
		Freeze(vu.ialureadpos);
		Freeze(vu.ialuwritepos);
		Freeze(vu.ialucount);

		for (int i = 0; i < 4; i++)
		{
			vu.fmac[i].regupper = fmac[i].regupper;
			vu.fmac[i].reglower = fmac[i].reglower;
			vu.fmac[i].flagreg = fmac[i].flagreg;
			vu.fmac[i].xyzwupper = fmac[i].xyzwupper;
			vu.fmac[i].xyzwlower = fmac[i].xyzwlower;
			vu.fmac[i].sCycle = widen_ee(fmac[i].sCycle);
			vu.fmac[i].Cycle = fmac[i].Cycle;
			vu.fmac[i].macflag = fmac[i].macflag;
			vu.fmac[i].statusflag = fmac[i].statusflag;
			vu.fmac[i].clipflag = fmac[i].clipflag;

			vu.ialu[i].reg = ialu[i].reg;
			vu.ialu[i].sCycle = widen_ee(ialu[i].sCycle);
			vu.ialu[i].Cycle = ialu[i].Cycle;
		}
		vu.fdiv.enable = fdiv.enable;
		vu.fdiv.reg = fdiv.reg;
		vu.fdiv.sCycle = widen_ee(fdiv.sCycle);
		vu.fdiv.Cycle = fdiv.Cycle;
		vu.fdiv.statusflag = fdiv.statusflag;
		vu.efu.enable = efu.enable;
		vu.efu.reg = efu.reg;
		vu.efu.sCycle = widen_ee(efu.sCycle);
		vu.efu.Cycle = efu.Cycle;
	};

	const auto legacyVuMid = [&](VURegs& vu) {
		u32 cycle;
		s32 nextBlockCycles;
		Freeze(cycle);
		Freeze(vu.flags);
		Freeze(vu.code);
		Freeze(vu.start_pc);
		Freeze(vu.branch);
		Freeze(vu.branchpc);
		Freeze(vu.delaybranchpc);
		Freeze(vu.takedelaybranch);
		Freeze(vu.ebit);
		Freeze(vu.pending_q);
		Freeze(vu.pending_p);
		FreezeSkip(4); // blockhasmbit — removed upstream
		Freeze(vu.micro_macflags);
		Freeze(vu.micro_clipflags);
		Freeze(vu.micro_statusflags);
		Freeze(vu.macflag);
		Freeze(vu.statusflag);
		Freeze(vu.clipflag);
		Freeze(nextBlockCycles);
		vu.cycle = widen_ee(cycle);
		vu.nextBlockCycles = nextBlockCycles;
	};

	const auto legacyVuTail = [&](VURegs& vu) {
		Freeze(vu.VIBackupCycles);
		Freeze(vu.VIOldValue);
		Freeze(vu.VIRegNumber);
		legacyVuPipes(vu);
	};

	Freeze(VU0.ACC);
	Freeze(VU0.VF);
	Freeze(VU0.VI);
	Freeze(VU0.q);
	legacyVuMid(VU0);
	legacyVuTail(VU0);

	Freeze(VU1.ACC);
	Freeze(VU1.VF);
	Freeze(VU1.VI);
	Freeze(VU1.q);
	Freeze(VU1.p);
	legacyVuMid(VU1);
	{
		u32 xgkicklastcycle;
		Freeze(VU1.xgkickaddr);
		Freeze(VU1.xgkickdiff);
		Freeze(VU1.xgkicksizeremaining);
		Freeze(xgkicklastcycle);
		Freeze(VU1.xgkickcyclecount);
		Freeze(VU1.xgkickenable);
		Freeze(VU1.xgkickendpacket);
		VU1.xgkicklastcycle = widen_ee(xgkicklastcycle);
	}
	legacyVuTail(VU1);
	if (!IsOkay())
		return false;

	// vuJIT (untagged): the era's microRegInfo pair. PreLoadPrep's
	// ClearCPUExecutionCaches already reset the VU recompilers (which zeroes
	// lpState), so the payload is skipped.
	FreezeSkip(2 * ((major == 0x9A2C) ? OldState::MICROREGINFO_9A2C : OldState::MICROREGINFO_9A34));

	// VIF: same block framing as today, but the vifStruct interior moved
	// (TRXPOS union). Field remap; the unpack buffer is bounds-checked before
	// the raw read.
	const auto legacyVif = [&](int idx) {
		if (!FreezeTag(idx ? "VIF1dma" : "VIF0dma"))
			return false;

		Freeze(idx ? g_vif1Cycles : g_vif0Cycles);

		OldState::vifStruct_9A2C ov;
		Freeze(ov);
		if (!IsOkay())
			return false;

		vifStruct& vif = idx ? vif1 : vif0;
		vif.MaskRow = ov.MaskRow;
		vif.MaskCol = ov.MaskCol;
		vif.tag = ov.tag;
		vif.cmd = ov.cmd;
		vif.pass = ov.pass;
		vif.cl = ov.cl;
		vif.usn = ov.usn;
		vif.start_aligned = ov.start_aligned;
		vif.irq = ov.irq;
		vif.done = ov.done;
		vif.vifstalled = ov.vifstalled;
		vif.stallontag = ov.stallontag;
		vif.waitforvu = ov.waitforvu;
		vif.unpackcalls = ov.unpackcalls;
		vif.BITBLTBUF = ov.BITBLTBUF;
		vif.TRXPOS = {}; // did not exist at this vintage
		vif.TRXREG = ov.TRXREG;
		vif.GSLastDownloadSize = ov.GSLastDownloadSize;
		vif.irqoffset = ov.irqoffset;
		vif.vifpacketsize = ov.vifpacketsize;
		vif.inprogress = ov.inprogress;
		vif.dmamode = ov.dmamode;
		vif.queued_program = ov.queued_program;
		vif.queued_pc = ov.queued_pc;
		vif.queued_gif_wait = ov.queued_gif_wait;

		u32 bSize;
		Freeze(bSize);
		if (bSize > sizeof(nVif[idx].buffer))
		{
			Error::SetStringFmt(error, "Legacy VIF{} unpack buffer is implausibly large ({} bytes).", idx, bSize);
			return false;
		}
		nVif[idx].bSize = bSize;
		FreezeMem(nVif[idx].buffer, bSize);
		return IsOkay();
	};

	if (!legacyVif(0) || !legacyVif(1))
		return false;

	if (!sifFreeze()) // byte-identical since 0312e902
		return false;

	// IPU: the register/decoder payload is byte-identical; IPUCoreStatus
	// (today appended to this block) is synthesized from the legacy IPUdma
	// status below.
	if (!FreezeTag("IPU"))
		return false;
	Freeze(ipu_fifo);
	Freeze(g_BP);
	Freeze(g_ipu_vqclut);
	Freeze(g_ipu_thresh);
	Freeze(coded_block_pattern);
	Freeze(decoder);
	Freeze(ipu_cmd);

	if (!FreezeTag("IPUdma"))
		return false;
	{
		// layout per upstream PCSX2 0312e902, pcsx2/IPU/IPUdma.h struct
		// IPUStatus {InProgress, DMAFinished, DataRequested}.
		bool inProgress = false, dmaFinished = false, dataRequested = false;
		Freeze(inProgress);
		Freeze(dmaFinished);
		Freeze(dataRequested);
		if (major == 0x9A34)
			FreezeSkip(1); // CommandExecuteQueued (added 0x9A2D, gone today)
		if (!IsOkay())
			return false;

		IPU1Status.InProgress = inProgress;
		IPU1Status.DMAFinished = dmaFinished;
		IPUCoreStatus.DataRequested = dataRequested;
		IPUCoreStatus.WaitingOnIPUFrom = false;
		IPUCoreStatus.WaitingOnIPUTo = false;
	}

	// Gif Unit: head remap (GS_FINISH grew a second flag), then the per-path
	// freeze is unchanged since 0312e902 and handles the serialized host
	// buffer pointer itself.
	{
		bool mtvuMode = false;
		if (!FreezeTag("Gif Unit"))
			return false;
		Freeze(mtvuMode);
		Freeze(gifUnit.stat);
		Freeze(gifUnit.gsSIGNAL);
		u8 finishFired = 0; // layout per 0312e902:pcsx2/Gif_Unit.h GS_FINISH {bool}
		Freeze(finishFired);
		gifUnit.gsFINISH.gsFINISHFired = (finishFired != 0);
		gifUnit.gsFINISH.gsFINISHPending = false;
		Freeze(gifUnit.lastTranType);
		if (!gifPathFreeze(GIF_PATH_1) || !gifPathFreeze(GIF_PATH_2) || !gifPathFreeze(GIF_PATH_3))
			return false;
		if (mtvuMode != THREAD_VU1)
			DevCon.Warning("gifUnit: MTVU Mode has switched between save/load state");
	}

	if (!gifDmaFreeze() || !sprFreeze() || !mtvuFreeze()) // all byte-identical since 0312e902
		return false;

	// ------------------------------------------------------ IOP-Subsystems
	if (!FreezeTag("IOP-Subsystems"))
		return false;

	FreezeMem(iopMem->Sif, sizeof(iopMem->Sif));

	// iopCounters: widen + mode-word translation. Today's psxCounterMode
	// bitfield mirrors the hardware register bits 0-14 positionally, so the
	// legacy word maps by masking; the legacy internal stopped bit moves to
	// the new bitfield's `stopped`. The legacy blank-gate bytes are dropped —
	// hBlanking/vBlanking resync at the next blank edge.
	if (!FreezeTag("iopCounters"))
		return false;
	{
		OldState::psxCounter_9A2C oc[8];
		s32 pnc;
		u32 pnsc;
		Freeze(oc);
		Freeze(pnc);
		Freeze(pnsc);
		FreezeSkip(2); // psxvblankgate, psxhblankgate
		if (!IsOkay())
			return false;

		for (int i = 0; i < 8; i++)
		{
			psxCounters[i].count = oc[i].count;
			psxCounters[i].target = oc[i].target;
			psxCounters[i].rate = oc[i].rate;
			psxCounters[i].interrupt = oc[i].interrupt;
			psxCounters[i].startCycle = widen_iop(oc[i].sCycleT);
			psxCounters[i].deltaCycles = oc[i].CycleT;
			psxCounters[i].mode.modeval = oc[i].mode & OldState::PSXCNT_HW_MODE_MASK;
			psxCounters[i].mode.stopped = (oc[i].mode & OldState::PSXCNT_STOPPED_9A2C) ? 1 : 0;
			psxCounters[i].currentIrqMode.repeatInterrupt = psxCounters[i].mode.repeatIntr;
			psxCounters[i].currentIrqMode.toggleInterrupt = psxCounters[i].mode.toggleIntr;
		}
		psxNextDeltaCounter = pnc;
		psxNextStartCounter = widen_iop(pnsc);
		psxRcntUpdate();
	}

	// SIO: the legacy transfer-engine blobs are discarded — savestates are
	// only taken at transfer-quiescent points, so the boot/reset state is
	// correct — but the memcard CRCs are honored so unchanged cards are not
	// spuriously ejected (mirrors Sio2::DoState).
	{
		u64 mcdCrcs[2][4] = {};
		if (major == 0x9A2C)
		{
			if (!FreezeTag("sio"))
				return false;
			FreezeSkip(OldState::SIO_BLOB_9A2C);
			u64 crcs[2][8];
			Freeze(crcs);
			for (int port = 0; port < 2; port++)
			{
				for (int slot = 0; slot < 4; slot++)
					mcdCrcs[port][slot] = crcs[port][slot];
			}
			if (!FreezeTag("sio2"))
				return false;
			FreezeSkip(OldState::SIO2_BLOB_9A2C);
		}
		else
		{
			if (!FreezeTag("sio0"))
				return false;
			FreezeSkip(OldState::SIO0_BLOB_9A34);
			if (!FreezeTag("sio2"))
				return false;
			FreezeSkip(OldState::SIO2_BLOB_9A34);
			for (int i = 0; i < 2; i++) // FreezeDeque<u8> fifoIn/fifoOut
			{
				u32 count = 0;
				Freeze(count);
				if (count > 0x10000)
				{
					Error::SetStringFmt(error, "Legacy SIO2 fifo is implausibly large ({} bytes).", count);
					return false;
				}
				FreezeSkip(static_cast<int>(count));
			}
			Freeze(mcdCrcs);
		}
		if (!IsOkay())
			return false;

		bool ejected = false;
		for (u32 port = 0; port < SIO::PORTS && !ejected; port++)
		{
			for (u32 slot = 0; slot < SIO::SLOTS; slot++)
			{
				if (mcdCrcs[port][slot] != mcds[port][slot].GetChecksum())
				{
					AutoEject::SetAll();
					ejected = true;
					break;
				}
			}
		}

		g_Sio0.SoftReset();
		g_Sio2.SoftReset();
		g_MultitapArr.at(0).FullReset();
		g_MultitapArr.at(1).FullReset();
	}

	if (!cdrFreeze()) // byte-identical since 0312e902
		return false;

	// cdvd: same field order under older names; RTCcount widened to double,
	// RotSpeed recomputed for the current disc/speed.
	if (!FreezeTag("cdvd"))
		return false;
	{
		auto ov = std::make_unique<OldState::cdvdStruct_9A2C>();
		Freeze(*ov);
		if (!IsOkay())
			return false;

		cdvd.nCommand = ov->nCommand;
		cdvd.Ready = ov->Ready;
		cdvd.Error = ov->Error;
		cdvd.IntrStat = ov->IntrStat;
		cdvd.Status = ov->Status;
		cdvd.StatusSticky = ov->StatusSticky;
		cdvd.DiscType = ov->Type;
		cdvd.sCommand = ov->sCommand;
		cdvd.sDataIn = ov->sDataIn;
		cdvd.sDataOut = ov->sDataOut;
		cdvd.HowTo = ov->HowTo;
		std::memcpy(cdvd.NCMDParamBuff, ov->NCMDParam, sizeof(cdvd.NCMDParamBuff));
		std::memcpy(cdvd.SCMDParamBuff, ov->SCMDParam, sizeof(cdvd.SCMDParamBuff));
		std::memcpy(cdvd.SCMDResultBuff, ov->SCMDResult, sizeof(cdvd.SCMDResultBuff));
		cdvd.NCMDParamCnt = ov->NCMDParamC;
		cdvd.NCMDParamPos = ov->NCMDParamP;
		cdvd.SCMDParamCnt = ov->SCMDParamC;
		cdvd.SCMDParamPos = ov->SCMDParamP;
		cdvd.SCMDResultCnt = ov->SCMDResultC;
		cdvd.SCMDResultPos = ov->SCMDResultP;
		cdvd.CBlockIndex = ov->CBlockIndex;
		cdvd.COffset = ov->COffset;
		cdvd.CReadWrite = ov->CReadWrite;
		cdvd.CNumBlocks = ov->CNumBlocks;
		cdvd.RTCcount = static_cast<double>(ov->RTCcount);
		cdvd.RTC = ov->RTC;
		cdvd.CurrentSector = ov->Sector;
		cdvd.SectorCnt = ov->nSectors;
		cdvd.SeekCompleted = ov->Readed;
		cdvd.Reading = ov->Reading;
		cdvd.WaitingDMA = ov->WaitingDMA;
		cdvd.ReadMode = ov->ReadMode;
		cdvd.BlockSize = ov->BlockSize;
		cdvd.Speed = ov->Speed;
		cdvd.RetryCntMax = ov->RetryCnt;
		cdvd.CurrentRetryCnt = ov->RetryCntP;
		cdvd.ReadErr = ov->RErr;
		cdvd.SpindlCtrl = ov->SpindlCtrl;
		std::memcpy(cdvd.Key, ov->Key, sizeof(cdvd.Key));
		cdvd.KeyXor = ov->KeyXor;
		cdvd.decSet = ov->decSet;
		std::memcpy(cdvd.mg_buffer, ov->mg_buffer, sizeof(cdvd.mg_buffer));
		cdvd.mg_size = ov->mg_size;
		cdvd.mg_maxsize = ov->mg_maxsize;
		cdvd.mg_datatype = ov->mg_datatype;
		std::memcpy(cdvd.mg_kbit, ov->mg_kbit, sizeof(cdvd.mg_kbit));
		std::memcpy(cdvd.mg_kcon, ov->mg_kcon, sizeof(cdvd.mg_kcon));
		cdvd.TrayTimeout = ov->TrayTimeout;
		cdvd.Action = ov->Action;
		cdvd.SeekToSector = ov->SeekToSector;
		cdvd.MaxSector = ov->MaxSector;
		cdvd.ReadTime = ov->ReadTime;
		cdvd.Spinning = ov->Spinning;
		cdvd.Tray = ov->Tray;
		cdvd.nextSectorsBuffered = ov->nextSectorsBuffered;
		cdvd.AbortRequested = ov->AbortRequested;

		cdvdRecalculateRotSpeed();

		// Same post-load behavior as cdvdFreeze: make sure the source has the
		// expected track buffered (a seek may be in progress).
		if (cdvd.Reading)
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekCompleted ? cdvd.CurrentSector : cdvd.SeekToSector, cdvd.ReadMode);
	}

	if (!deci2Freeze()) // byte-identical since 0312e902
		return false;

	// Tail: 0x9A2C-era builds compile input recording out, so their blob ends
	// after deci2; 0x9A34 (NetherSX2 4248) carries the InputRecording block.
	// Neither has hostHandles — mirror handleFreeze's load-side reset.
	if (major == 0x9A34)
	{
		if (!InputRecordingFreeze())
			return false;
	}

	if (EmuConfig.HostFs)
		R3000A::ioman::reset();

	// The legacy layouts are fully accounted; any residue means the blob was
	// not what the version id claimed, and the half-applied load is unwound
	// by the caller (VMManager::Reset).
	if (m_idx != static_cast<int>(m_memory.size()))
	{
		Error::SetStringFmt(error, "Legacy save state has {} unexpected trailing bytes (version {:x}).",
			static_cast<int>(m_memory.size()) - m_idx, m_version);
		return false;
	}

	return IsOkay();
}
