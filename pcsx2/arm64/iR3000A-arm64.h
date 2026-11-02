// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "R3000A.h"
#include "arm64/iCore-arm64.h"
#include "arm64/AsmHelpers.h"

// x21: Pointer to psxRegs struct (callee-saved). Loaded once at IOP JIT entry
// by EnterRecompiledCode and never modified for the duration of IOP execution.
// Use armPsxRegMem() to construct psxRegs-relative MemOperands cheaply.
#define RPSXSTATE vixl::aarch64::x21

// The IOP clocks while compiled code runs, in two callee-saved registers (the IOP
// rec saves them in its stack frame, see iopBeginStackFrame, and uses neither for
// anything else):
//
//   RIOPCYC (x19) = psxRegs.cycle - psxRegs.iopNextEventCycle, signed 64-bit.
//                   A block end adds the block's cycles; the event test is due
//                   when it is >= 0 (cycle >= iopNextEventCycle).
//   RIOPEE  (w20) = psxRegs.iopCycleEE, the EE-cycle budget of the slice left.
//
// Memory is not updated at a block end. It is made current (spill) before
// anything that can read or write those fields -- every C call that reaches an
// IOP device or the event machinery -- and the registers are taken back from
// memory (reload) after it, since the callee may have moved iopNextEventCycle
// or ended the slice by zeroing iopCycleEE. The slice exit spills too. The
// values C code sees during a block are the ones it always saw: those of the
// start of the block. See iopSpillStub / iopReloadStub.
#define RIOPCYC vixl::aarch64::x19
#define RIOPEE vixl::aarch64::w20

// Build a MemOperand addressing a psxRegs field via RPSXSTATE.
// Mirrors the EE armCpuRegMem pattern. ARM64 LDR with imm12 covers offsets up
// to 32760 bytes (64-bit) — easily larger than psxRegs, so a single instruction
// suffices for every reachable field.
static __fi vixl::aarch64::MemOperand armPsxRegMem(const void* field)
{
	const ptrdiff_t off = reinterpret_cast<const u8*>(field) - reinterpret_cast<const u8*>(&psxRegs);
	return vixl::aarch64::MemOperand(RPSXSTATE, static_cast<int64_t>(off));
}

static __fi bool armIsPsxRegPtr(const void* field)
{
	const u8* base = reinterpret_cast<const u8*>(&psxRegs);
	const u8* p    = reinterpret_cast<const u8*>(field);
	return p >= base && p < base + sizeof(psxRegs);
}
static __fi void armLoadPsxRegPtr(const vixl::aarch64::CPURegister& reg, const void* field)
{
	if (armIsPsxRegPtr(field))
		armAsm->Ldr(reg, armPsxRegMem(field));
	else
		armLoadPtr(reg, field);
}
static __fi void armStorePsxRegPtr(const vixl::aarch64::CPURegister& reg, const void* field)
{
	if (armIsPsxRegPtr(field))
		armAsm->Str(reg, armPsxRegMem(field));
	else
		armStorePtr(reg, field);
}

// Cycle penalties for particularly slow IOP instructions.
static const int psxInstCycles_Mult = 7;
static const int psxInstCycles_Div = 40;

static const int psxInstCycles_Peephole_Store = 0;
static const int psxInstCycles_Store = 0;
static const int psxInstCycles_Load = 0;

// HI/LO register indices — consistent with EE naming
#define PSX_HI NEONGPR_HI
#define PSX_LO NEONGPR_LO

extern uptr psxRecLUT[];

// Block-coverage counters over the HWADDR window (defined and maintained in
// iR3000A-arm64.cpp). g_iopCodeCov[g] counts live recBlocks entries whose
// span overlaps 256-byte granule g; zero means no store into that granule can
// touch compiled code. rpsxStoreGeneric emits an inline probe of this array
// on the RAM-store fast path, calling iopStoreClearHit only on nonzero.
static constexpr u32 kIopCovShift = 8;
static constexpr u32 kIopCovSpan = 0x800000; // 8MB: IOP RAM incl. extraRam mirrors
static constexpr u32 kIopCovGranules = kIopCovSpan >> kIopCovShift;
extern u16 g_iopCodeCov[kIopCovGranules];

// Slow tail of the RAM-store fast-path stubs: the store landed in a granule
// with live block coverage, so run the full SMC clear (same single-word form
// iopMemWrite* uses via psxCpu->Clear).
void iopStoreClearHit(u32 addr);

// Out-of-line RAM-store fast-path stubs (JIT-emitted per reset alongside the
// dispatchers): index 0/1/2 = 8/16/32-bit. w0 = address, w1 = value; sites
// call them with a single BL so the fast path adds no per-site icache cost.
extern const void* g_iopStoreStub[3];

// Make memory current from RIOPCYC/RIOPEE (spill) and take the registers back
// from memory (reload). Called with BL from compiled code and from the other
// stubs; spill clobbers x9, reload x9 and x10, neither touches anything else
// (a C call's result in x0 survives a reload).
extern const void* g_iopSpillStub;
extern const void* g_iopReloadStub;

// Spill, iopEventTest(), reload. For the cold side of a block end. Preserves nothing
// but the callee-saved registers: the allocator is empty at a block end.
extern const void* g_iopEventStub;

// Kinds of guest load, indexing g_iopLoadColdStub.
enum class IopLoadKind : int
{
	LB = 0,
	LBU = 1,
	LH = 2,
	LHU = 3,
	LW = 4,
};

// Slow path of the inline RAM-load fast path, one per IopLoadKind, JIT-emitted
// per reset with the dispatchers: w8 = address in, extended value in w9 out,
// every allocator register preserved (see _DynGen_LoadColdStub).
extern const void* g_iopLoadColdStub[5];

// Emit "dst = load(kind) from base + imm" with a RAM fast path inline and a
// cold site (emitted by psxEmitLoadColdSites at the end of the block) for any
// other address. Does not flush or free any allocator register: dst and base
// are host registers the caller allocated (base is read before dst is
// written, so they may be the same register); everything else in the
// allocator is untouched across the load. Clobbers w8, w9 and x17.
void psxEmitRamLoad(IopLoadKind kind, const vixl::aarch64::Register& dst, const vixl::aarch64::Register& base, s32 imm);

// The same for an address known at compile time: a RAM address is one
// instruction pair, anything else a call of the cold stub in line.
void psxEmitConstLoad(IopLoadKind kind, const vixl::aarch64::Register& dst, u32 addr);

// Emit the cold sites queued for the block being compiled (the slow paths of its
// loads and the event-test calls of its block ends). Called once, after the
// block's last instruction.
void psxEmitColdSites();

// Emit a spill / a reload at a compile-time C-call seam (see RIOPCYC).
void psxEmitSpill();
void psxEmitReload();

void _psxFlushConstReg(int reg);
void _psxFlushConstRegs();

void _psxDeleteReg(int reg, int flush);
void _psxFlushCall(int flushtype);
void _psxFlushAllDirty();

void _psxOnWriteReg(int reg);

void _psxMoveGPRtoR(const vixl::aarch64::Register& to, int fromgpr);

extern u32 psxpc;       // recompiler pc
extern int psxbranch;   // set for branch
extern u32 g_iopCyclePenalty;

void psxSaveBranchState();
void psxLoadBranchState();

extern void psxSetBranchReg();
extern void psxSetBranchImm(u32 imm);
extern void psxRecompileNextInstruction(bool delayslot, bool swapped_delayslot);

////////////////////////////////////////////////////////////////////
// IOP Constant Propagation

#define PSX_IS_CONST1(reg) ((reg) < 32 && (g_psxHasConstReg & (1 << (reg))))
#define PSX_IS_CONST2(reg1, reg2) ((g_psxHasConstReg & (1 << (reg1))) && (g_psxHasConstReg & (1 << (reg2))))
#define PSX_IS_DIRTY_CONST(reg) ((reg) < 32 && (g_psxHasConstReg & (1 << (reg))) && (!(g_psxFlushedConstReg & (1 << (reg)))))
#define PSX_SET_CONST(reg) \
	{ \
		if ((reg) < 32) \
		{ \
			g_psxHasConstReg |= (1u << (reg)); \
			g_psxFlushedConstReg &= ~(1u << (reg)); \
		} \
	}

#define PSX_DEL_CONST(reg) \
	{ \
		if ((reg) < 32) \
			g_psxHasConstReg &= ~(1 << (reg)); \
	}

extern u32 g_psxConstRegs[32];
extern u32 g_psxHasConstReg, g_psxFlushedConstReg;

bool psxTrySwapDelaySlot(u32 rs, u32 rt, u32 rd);

// IOP v1.0 IRX-import HLE backdoor (0x2400xxxx marker). Emits the HLE call +
// re-dispatch; defined in iR3000A-arm64.cpp where iopDispatcherReg is visible.
void psxRecompileIrxImport();
