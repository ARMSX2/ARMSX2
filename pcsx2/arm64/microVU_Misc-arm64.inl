// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

//------------------------------------------------------------------
// Micro VU - NEON Reg Loading/Saving/Shuffling/Unpacking/Merging
//------------------------------------------------------------------

// Broadcast a single component to all 4 lanes
// xyzw: 0=X, 1=Y, 2=Z, 3=W
void mVUunpack_xyzw(const a64::VRegister& dstreg, const a64::VRegister& srcreg, int xyzw)
{
	// DUP Vd.4S, Vn.S[lane] — broadcast lane to all 4 slots
	armAsm->Dup(dstreg.V4S(), srcreg.V4S(), xyzw);
}

// Load VF register components from memory.
// xyzw bitmask: 8=X, 4=Y, 2=Z, 1=W. Single-component loads zero-extend.
// For full load (xyzw=0xF or non-single), loads all 128 bits.
void mVUloadReg(const a64::VRegister& reg, const void* ptr, int xyzw)
{
	switch (xyzw)
	{
		case 8: // X only — load 32-bit scalar, upper lanes zeroed
			armLoadPtr(a64::VRegister(reg.GetCode(), 32), ptr);
			break;
		case 4: // Y only
			armLoadPtr(a64::VRegister(reg.GetCode(), 32), (const u8*)ptr + 4);
			break;
		case 2: // Z only
			armLoadPtr(a64::VRegister(reg.GetCode(), 32), (const u8*)ptr + 8);
			break;
		case 1: // W only
			armLoadPtr(a64::VRegister(reg.GetCode(), 32), (const u8*)ptr + 12);
			break;
		default: // Full 128-bit load
			armLoadPtr(reg, ptr);
			break;
	}
}

// base+offset overload — for call sites where the address is reachable as
// `[base, #off]` (e.g. base=gprVUState, off within VURegs). Emits a single
// Ldr per partial lane, no scratch+materialize round-trip. The full-load
// case uses imm12 scaled by 16 (Q-reg) so reaches up to 64KB from base.
__fi void mVUloadReg(const a64::VRegister& reg, const a64::Register& base, int64_t off, int xyzw)
{
	switch (xyzw)
	{
		case 8: armAsm->Ldr(a64::VRegister(reg.GetCode(), 32), a64::MemOperand(base, off));      break;
		case 4: armAsm->Ldr(a64::VRegister(reg.GetCode(), 32), a64::MemOperand(base, off + 4));  break;
		case 2: armAsm->Ldr(a64::VRegister(reg.GetCode(), 32), a64::MemOperand(base, off + 8));  break;
		case 1: armAsm->Ldr(a64::VRegister(reg.GetCode(), 32), a64::MemOperand(base, off + 12)); break;
		default: armAsm->Ldr(reg, a64::MemOperand(base, off)); break;
	}
}

// Load from a runtime address (held in a 64-bit base register) into reg,
// following the regalloc lane convention enforced by writeBackNeon, which
// calls mVUsaveReg(modXYZW=true):
//   - Single-lane partial (X/Y/Z/W): 32-bit load that zeroes upper lanes
//     places the value in lane 0. mVUsaveReg's Y/Z/W cases with modXYZW=true
//     write lane 0 back to the natural byte slot. (X is lane 0 either way.)
//   - Multi-lane partial / full mask: full 16-byte load into natural lanes.
//     Multi-lane mVUsaveReg cases store from natural lanes (modXYZW ignored).
//
// LQ/LQD/LQI must use this — using `Ldr Q, [base]` plus
// mVUmergeRegs(natural-lane) for partial writes leaves single-lane Y/Z/W
// values in their natural lane while the writeback reads lane 0, silently
// clobbering VU memory with whatever happened to be in the recycled
// Q register's lane 0.
__fi void mVUloadMem(const a64::VRegister& reg, const mVUmemRef& addr, int xyzw)
{
	int lane;
	switch (xyzw)
	{
		case 0x8: lane = 0;  break; // X — naturally lane 0
		case 0x4: lane = 4;  break; // Y — into lane 0
		case 0x2: lane = 8;  break; // Z — into lane 0
		case 0x1: lane = 12; break; // W — into lane 0
		default:
			armAsm->Ldr(reg, addr.at());
			return;
	}
	armAsm->Ldr(a64::VRegister(reg.GetCode(), 32), addr.at(lane));
}

// Whether every access mVUloadMem makes for `xyzw` is at the address itself:
// all but the single-lane y, z and w loads, which add their lane's offset.
static __fi bool mVUloadMemAtZero(int xyzw)
{
	return xyzw != 0x4 && xyzw != 0x2 && xyzw != 0x1;
}

// Store VF register components to memory with xyzw mask.
// Handles all 15 non-zero xyzw combinations.
//
// modXYZW semantics (match x86):
//   - modXYZW == true  : value is in lane 0 of `reg` (from SS-path shuffle
//                        in allocReg's clone-write). Write lane 0 to the
//                        target slot. writeBackNeon passes this path.
//   - modXYZW == false : value is at the natural lane of `reg`. Write the
//                        natural lane to the target slot. Used by VIF path
//                        which loads data directly in natural positions.
//
// IMPORTANT: ARM64 ST1-single-lane has NO immediate-offset addressing form —
// VIXL's `St1(V, lane, MemOperand(base, imm))` silently drops `imm` in
// release builds (and asserts in debug). Partial stores here therefore
// advance `x8` with Add before each ST1 at a non-zero slot. `x8` is a
// scratch register (not RSCRATCHADDR), so clobbering it is fine.
// Internal helper — assumes x8 already holds the destination address.
// Both mVUsaveReg overloads route through this to share the case body.
static void mVUsaveRegAtX8(const a64::VRegister& reg, int xyzw, bool modXYZW);

void mVUsaveReg(const a64::VRegister& reg, const void* ptr, int xyzw, bool modXYZW)
{
	armMoveAddressToReg(a64::x8, ptr);
	mVUsaveRegAtX8(reg, xyzw, modXYZW);
}

// base+offset overload — for call sites where the address is reachable as
// `[base, #off]` (e.g. base=gprVUState, off within VURegs). The full-store
// (xyzw=0xF) case bypasses x8 entirely and uses [base, #off] directly,
// saving an extra Add. Partial stores still need x8 because ST1-single-lane
// has no immediate-offset form on aarch64 — see notes on the absolute-ptr
// overload above.
void mVUsaveReg(const a64::VRegister& reg, const a64::Register& base, int64_t off, int xyzw, bool modXYZW)
{
	if (xyzw == 0xF)
	{
		armAsm->Str(reg, a64::MemOperand(base, off));
		return;
	}
	if (off != 0)
		armAsm->Add(a64::x8, base, off);
	else
		armAsm->Mov(a64::x8, base);
	mVUsaveRegAtX8(reg, xyzw, modXYZW);
}

static void mVUsaveRegAtX8(const a64::VRegister& reg, int xyzw, bool modXYZW)
{
	switch (xyzw)
	{
		case 0xF: // XYZW — full store
			armAsm->Str(reg, a64::MemOperand(a64::x8));
			break;
		case 0x8: // X — always lane 0 (X is lane 0 natively)
			armAsm->St1(reg.V4S(), 0, a64::MemOperand(a64::x8));
			break;
		case 0x4: // Y
			armAsm->Add(a64::x8, a64::x8, 4);
			armAsm->St1(reg.V4S(), modXYZW ? 0 : 1, a64::MemOperand(a64::x8));
			break;
		case 0x2: // Z
			armAsm->Add(a64::x8, a64::x8, 8);
			armAsm->St1(reg.V4S(), modXYZW ? 0 : 2, a64::MemOperand(a64::x8));
			break;
		case 0x1: // W
			armAsm->Add(a64::x8, a64::x8, 12);
			armAsm->St1(reg.V4S(), modXYZW ? 0 : 3, a64::MemOperand(a64::x8));
			break;
		case 0xC: // XY
			armAsm->Str(a64::DRegister(reg.GetCode()), a64::MemOperand(a64::x8));
			break;
		case 0x3: // ZW
			armAsm->Add(a64::x8, a64::x8, 8);
			armAsm->St1(reg.V2D(), 1, a64::MemOperand(a64::x8));
			break;
		case 0xE: // XYZ — D-reg store (XY) post-indexes 8 bytes; Z lane store at [x8].
			armAsm->Str(a64::DRegister(reg.GetCode()), a64::MemOperand(a64::x8, 8, a64::PostIndex));
			armAsm->St1(reg.V4S(), 2, a64::MemOperand(a64::x8));
			break;
		case 0x7: // YZW — Y lane post-indexes the V4S element size (4); ZW V2D at [x8].
			armAsm->Add(a64::x8, a64::x8, 4);
			armAsm->St1(reg.V4S(), 1, a64::MemOperand(a64::x8, 4, a64::PostIndex));
			armAsm->St1(reg.V2D(), 1, a64::MemOperand(a64::x8));
			break;
		case 0xD: // XYW — D-reg store (XY) post-indexes 12 bytes; W lane store at [x8].
			armAsm->Str(a64::DRegister(reg.GetCode()), a64::MemOperand(a64::x8, 12, a64::PostIndex));
			armAsm->St1(reg.V4S(), 3, a64::MemOperand(a64::x8));
			break;
		case 0xB: // XZW
			armAsm->St1(reg.V4S(), 0, a64::MemOperand(a64::x8));
			armAsm->Add(a64::x8, a64::x8, 8);
			armAsm->St1(reg.V2D(), 1, a64::MemOperand(a64::x8));
			break;
		case 0xA: // XZ
			armAsm->St1(reg.V4S(), 0, a64::MemOperand(a64::x8));
			armAsm->Add(a64::x8, a64::x8, 8);
			armAsm->St1(reg.V4S(), 2, a64::MemOperand(a64::x8));
			break;
		case 0x9: // XW
			armAsm->St1(reg.V4S(), 0, a64::MemOperand(a64::x8));
			armAsm->Add(a64::x8, a64::x8, 12);
			armAsm->St1(reg.V4S(), 3, a64::MemOperand(a64::x8));
			break;
		case 0x6: // YZ — Y lane post-indexes the V4S element size (4); Z lane at [x8].
			armAsm->Add(a64::x8, a64::x8, 4);
			armAsm->St1(reg.V4S(), 1, a64::MemOperand(a64::x8, 4, a64::PostIndex));
			armAsm->St1(reg.V4S(), 2, a64::MemOperand(a64::x8));
			break;
		case 0x5: // YW
			armAsm->Add(a64::x8, a64::x8, 4);
			armAsm->St1(reg.V4S(), 1, a64::MemOperand(a64::x8));
			armAsm->Add(a64::x8, a64::x8, 8); // x8 now points at +12 (W)
			armAsm->St1(reg.V4S(), 3, a64::MemOperand(a64::x8));
			break;
		default:
			break;
	}
}

// Merge selected components from src into dest.
// xyzw bitmask: 8=X, 4=Y, 2=Z, 1=W.
// Single shared definition for microVU AND the VIF unpack dynarec
// (declared in microVU-arm64.h and Vif_UnpackNEON.h), mirroring x86's
// one helper in microVU_Misc.inl.
//
// modXYZW semantics (match x86): for single-lane masks the value is in
// lane 0 of src (SS-path shuffle convention); multi-lane masks always
// merge natural lanes.
void mVUmergeRegs(const a64::VRegister& dest, const a64::VRegister& src, int xyzw, bool modXYZW)
{
	xyzw &= 0xf;
	if (dest.IsNone() || src.IsNone() || (dest.GetCode() == src.GetCode()) || xyzw == 0)
		return;

	if (xyzw == 0xF)
	{
		armAsm->Mov(dest.V16B(), src.V16B());
		return;
	}

	if (modXYZW)
	{
		// Source has the value in lane 0 — insert into target lane
		switch (xyzw)
		{
			case 0x8: armAsm->Ins(dest.V4S(), 0, src.V4S(), 0); return; // X
			case 0x4: armAsm->Ins(dest.V4S(), 1, src.V4S(), 0); return; // Y
			case 0x2: armAsm->Ins(dest.V4S(), 2, src.V4S(), 0); return; // Z
			case 0x1: armAsm->Ins(dest.V4S(), 3, src.V4S(), 0); return; // W
			default: break; // Multi-lane: natural-lane merge below
		}
	}

	// Natural-lane merge. Reverse the mask into lane order (bit i = lane i)
	// so an X/Y or Z/W pair can move as one 64-bit lane insert.
	int lanes = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);

	if ((lanes & 3) == 3) // XY
	{
		armAsm->Mov(dest.V2D(), 0, src.V2D(), 0);
		lanes &= ~3;
	}
	else if ((lanes & 12) == 12) // ZW
	{
		armAsm->Mov(dest.V2D(), 1, src.V2D(), 1);
		lanes &= ~12;
	}

	for (u32 i = 0; i < 4; i++)
	{
		if (lanes & (1u << i))
			armAsm->Ins(dest.V4S(), i, src.V4S(), i);
	}
}

//------------------------------------------------------------------
// Micro VU - Backup/Restore Regs for C Calls
//------------------------------------------------------------------

// Flush all register allocations and save PQ before a C function call.
// The dispatcher already saved callee-saved regs, so once the regalloc is
// flushed the only state that must survive the call is PQ. toMemory/onlyNeeded
// are accepted for signature parity with x86 mVUbackupRegs and ignored here.
__fi void mVUbackupRegs(microVU& mVU, bool toMemory = false, bool onlyNeeded = false)
{
	// Every mid-block C call is wrapped in this backup — the plain-AAPCS
	// callee clobbers caller-saved pool regs, so the branch-condition carry
	// can't survive past it.
	mVUclearBranchCondCarry(mVU);
	mVU.regAlloc->flushAll();
	armAsm->Str(qmmPQ, mVUneonBackupMem(qmmPQ.GetCode()));
}

__fi void mVUrestoreRegs(microVU& mVU, bool fromMemory = false, bool onlyNeeded = false)
{
	armAsm->Ldr(qmmPQ, mVUneonBackupMem(qmmPQ.GetCode()));
	// A plain-AAPCS callee has been through qmmClampMax and qmmClampMin with
	// the rest of the caller-saved NEON file. This is the only seam that
	// needs them back: a block's other C calls either end it, go through the
	// waitMTVU thunk, which spills q0-q28 itself, or reach an EE FPU / EFU
	// model stub, whose callee model_call_contract_tests holds below q8.
	mVUemitClampConsts(mVU);
}

//------------------------------------------------------------------
// Micro VU - VU Memory Address Translation
//------------------------------------------------------------------

static inline u32 branchAddr(const mV)
{
	pxAssumeMsg(islowerOP, "MicroVU: Expected Lower OP code for valid branch addr.");
	return ((((iPC + 2) + (_Imm11_ * 2)) & mVU.progMemMask) * 4);
}

// Called by the waitMTVU thunk when VU0 (or COP2 macro mode) addresses VU1's register window.
// `vi` is the address mVUaddrFix was fixing up (the thunk passes gprT1). Under MTVU this waits for
// the VU1 thread. Regardless of threading, an access to the quadword holding VU1's STATUS register
// is reported to the vu-aot tier: something outside VU1 can see the sticky bits it drops.
static void mVUwaitMTVU(u32 vi)
{
	if (THREAD_VU1)
	{
		if (IsDevBuild)
			DevCon.WriteLn("microVU0: Waiting on VU1 thread to access VU1 regs!");
		vu1Thread.WaitVU();
	}
	if ((vi & 0x3f) == VU0_WINDOW_VU1_STATUS_QWORD)
		vuAotNoteOutsideStatusRead();
}

static void mVUTBit()
{
	u32 old = vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUTBit, std::memory_order_release);
	if (old & VU_Thread::InterruptFlagVUTBit)
		DevCon.Warning("Old TBit not registered");
}

static void mVUEBit()
{
	vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUEBit, std::memory_order_release);
}

// Transform VI register address to valid VU0/VU1 memory pointer offset.
// gprReg holds the VI value (address in VU quadwords).
// On exit, gprReg holds byte offset into VU memory.
__fi void mVUaddrFix(mV, const a64::Register& gprReg)
{
	if (isVU1)
	{
		// VU1: mask to 0x3FF quadwords, shift left 4 (x16 bytes)
		armAsm->Ubfiz(gprReg.W(), gprReg.W(), 4, 10);
	}
	else
	{
		// VU0: if addr & 0x400, accessing VU1 register space
		a64::Label notVU1Access, done;

		armAsm->Tst(gprReg.W(), 0x400);
		armAsm->B(&notVU1Access, a64::eq);

		// Accessing VU1 regs from VU0
		pxAssert(gprReg.GetCode() == gprT1.GetCode()); // (the waitMTVU thunk reads the address from gprT1)
		if (THREAD_VU1)
		{
			// COP2 macro mode emits this inline into an EE block where the
			// caller-saved EE pins (x11/x12/x13 since Arm D) are live across
			// the call — waitMTVU is plain AAPCS, not preserve_most, so the
			// full clobber reload applies; micro mode runs behind a C
			// boundary whose call site reloads instead. Flush-before is a
			// lazy-dirty no-op in write-through mode.
			if (mVU.cop2)
				armEmitEEClobberedPinFlushForCOP2();
			// Need to wait for VU1 thread (and to tell the vu-aot tier if this is the STATUS quadword)
			armEmitCall((void*)mVU.waitMTVU);
			if (mVU.cop2)
				armEmitEEClobberedPinReloadForCOP2();
		}
		else if (mVUvuAotVU0WindowHook())
		{
			// Nothing to wait for, but an access to VU1's STATUS register (VI[16], quadword 48 of the
			// window) still goes to the same thunk. The thunk saves every register a C call can
			// clobber, so no pin or cached value needs flushing around it. Other quadwords cost only
			// the compare and branch. Emitted only while the vu-aot tier is wanted.
			a64::Label notStatus;
			armAsm->And(RWSCRATCH, gprReg.W(), 0x3f);
			armAsm->Cmp(RWSCRATCH, VU0_WINDOW_VU1_STATUS_QWORD);
			armAsm->B(&notStatus, a64::ne);
			armEmitCall((void*)mVU.waitMTVU);
			armAsm->Bind(&notStatus);
		}
		// The window is 64 quadwords starting at VU1's VF[0]: VF0..VF31, then VI0..VI31. Callers add
		// what this answers to VU0.Mem, so the answer is the byte distance from VU0.Mem to the
		// quadword. That distance is computed here at run time, from two registers, and never as a
		// constant: VU1's registers are in the emulator image and VU0.Mem is in the data arena, the
		// two move apart from one process to the next, and code the program cache persists would
		// carry the recording process's distance. VU1's registers sit a fixed way past VU0's in
		// vuState, which gprVUState points at (in macro mode too, see mVUmacroEmitPrologue).
		static_assert(offsetof(VuStateStore, regs) == 0);
		constexpr int64_t kVU1VFFromState = sizeof(VURegs) + offsetof(VURegs, VF);
		armAsm->Ubfiz(gprReg.W(), gprReg.W(), 4, 6);
		armAsm->Add(gprReg.X(), gprReg.X(), gprVUState);
		armAsm->Add(gprReg.X(), gprReg.X(), kVU1VFFromState);
		if (mVU.cop2)
		{
			// x29 is the EE's $v0 pin here, not VU0.Mem.
			armAsm->Ldr(RXSCRATCH, mVUstateMem(offsetof(VURegs, Mem)));
			armAsm->Sub(gprReg.X(), gprReg.X(), RXSCRATCH);
		}
		else
		{
			armAsm->Sub(gprReg.X(), gprReg.X(), gprVUMem);
		}
		armAsm->B(&done);

		armAsm->Bind(&notVU1Access);
		armAsm->And(gprReg.W(), gprReg.W(), 0xff);
		armAsm->Lsl(gprReg.X(), gprReg.X(), 4);

		armAsm->Bind(&done);
	}
}

// mVUaddrFix with the VI index in another register. VU1's UBFIZ takes the
// quadword index from bits 0-9 of `src` wherever it sits, so the copy into
// `dst` (and any sign or zero extension, which touches only bits 16 and up)
// that would stand in front of it is not needed. VU0 copies and runs the
// full path.
__fi void mVUaddrFixFrom(mV, const a64::Register& dst, const a64::Register& src)
{
	if (isVU1)
	{
		armAsm->Ubfiz(dst.W(), src.W(), 4, 10);
		return;
	}
	if (dst.GetCode() != src.GetCode())
		armAsm->Mov(dst.W(), src.W());
	mVUaddrFix(mVU, dst);
}

// Turn a byte index into VU data memory, already in gprIdxq, into something a
// load or store can address: on VU1 one Add off the pin, the distance riding
// in the access's own offset field; on VU0 the pointer is still loaded.
//
// `atZeroOnly`: every access the caller makes is at the address itself, with
// no lane offset on top. Then micro mode needs nothing emitted at all: the
// access takes [gprVUMem, index].
__fi mVUmemRef mVUmemAtIndex(mV, const a64::Register& gprIdxq, bool atZeroOnly = false)
{
	if (atZeroOnly && !mVU.cop2)
		return {gprVUMem, 0, gprIdxq};

	if (isVU1)
	{
		armAsm->Add(gprIdxq, gprVUState, gprIdxq);
		return {gprIdxq, kVU1MemFromState};
	}

	armAsm->Ldr(gprT2q, mVUstateMem(offsetof(VURegs, Mem)));
	armAsm->Add(gprIdxq, gprT2q, gprIdxq);
	return {gprIdxq, 0};
}

// Constant-address fold for loadstores whose base VI is vi00 (always 0). With a
// constant base the whole address is known at compile time, so the runtime
// moveVIToGPR + imm-add + mVUaddrFix (mask/shift) + base-add chain collapses to
// a single Mem-pointer load plus (at most) one immediate add -- or, on VU1,
// to nothing at all, the whole address being a displacement off the pin.
// On a return of true `out` addresses &VU.Mem[const]; on false the caller
// emits the normal runtime path. Mirrors x86 mVUoptimizeConstantAddr
// (microVU_Misc.inl). The VU0 cross-VU-register window (addr & 0x400) and the
// IbitHack runtime-reconstruct path are deliberately left to mVUaddrFix.
// Ported 2026-06-23 from upstream 6018936dc (postdates the leak/4248; correct
// and ABI-neutral, so adopted per the "newer-upstream-pattern" rule).
//
// dispLimit is the largest displacement the caller's own accesses encode: an
// unsigned-offset load or store scales its immediate by its own width, so what
// a quadword reaches a halfword does not. Past it the pointer load comes back,
// rather than let the assembler materialise the displacement into a scratch --
// which costs the same and spends a register the emitter never budgeted.
__fi bool mVUoptimizeConstantAddr(mV, u32 srcreg, s32 offset, s32 offsetSS_,
	const a64::Register& gprOutQ, int64_t dispLimit, mVUmemRef& out)
{
	if (srcreg != 0 || EmuConfig.Gamefixes.IbitHack)
		return false;

	s32 byteAddr;
	if (isVU1)
	{
		byteAddr = ((offset & 0x3ff) << 4) + offsetSS_;

		if (kVU1MemFromState + byteAddr <= dispLimit)
		{
			out = {gprVUState, kVU1MemFromState + byteAddr};
			return true;
		}
	}
	else
	{
		if (offset & 0x400)
			return false; // cross-VU-register access — runtime path handles it
		byteAddr = ((offset & 0xff) << 4) + offsetSS_;

		// Micro mode holds VU0's memory pointer in gprVUMem.
		if (!mVU.cop2 && byteAddr <= dispLimit)
		{
			out = {gprVUMem, byteAddr};
			return true;
		}
	}

	armAsm->Ldr(gprOutQ, mVUstateMem(offsetof(VURegs, Mem)));
	if (byteAddr != 0)
		armAsm->Add(gprOutQ, gprOutQ, byteAddr);
	out = {gprOutQ, 0};
	return true;
}

//------------------------------------------------------------------
// Micro VU - micro-memory addresses in VU1 code
//------------------------------------------------------------------

// VU1 code holds no micro-memory address of its own. A branch target, a link
// value, the TPC an exit stores and the address of an immediate read at run
// time are each the placement's root (the low bits of mVU.place, written by
// the entry stub of the block that was entered) plus the distance the compiler
// knows, wrapped the way the VU wraps: pair indices to 11 bits, byte addresses
// to 16 KiB. The same code is therefore right for a copy of the program
// anywhere in micro memory, given that copy's root.
//
// This holds while no VU address reaches the code any other way. Constant
// propagation would bake vi15 and a JR target in.
static_assert(!doConstProp, "VU1 code takes its micro-memory addresses from mVU.place");

// The code holds no pointer to a block object of the program it was compiled
// in, and no branch to a block outside its chunk, either: those come from the
// placement's slots (mVU.placeSlots), numbered by the chunk and found through
// the high bits of mVU.place.

// VU0 keeps its addresses as constants: its compiler decides on the absolute
// PC (the M-bit check, the spin loop detector).
__fi static bool mVUrootRelative(mV) { return isVU1; }

// Bytes of the part of a VU1 block's entry stub that stores mVU.place.
static constexpr u32 mVUrootStubBytes = 12;
// Bytes the stub goes on with under the IbitHack gamefix, to store
// mVU.placeMicro.
static constexpr u32 mVUplaceMicroStubBytes = 16;
static constexpr u32 mVUplaceRootBits = 11;

// Bytes of an entry stub as the compiler emits them now.
__fi static u32 mVUplaceStubBytes()
{
	return mVUrootStubBytes + (EmuConfig.Gamefixes.IbitHack ? mVUplaceMicroStubBytes : 0);
}

// The value a placement's entry stubs store in mVU.place.
__fi static u32 mVUplaceWord(u32 rootPC, u32 slotBase)
{
	pxAssert(slotBase < (1u << (32 - mVUplaceRootBits)));
	return ((rootPC / 8) & 0x7ff) | (slotBase << mVUplaceRootBits);
}

// An entry stub: three instructions that store `word` in mVU.place. Under
// the IbitHack gamefix four more store the placement's micro memory in
// mVU.placeMicro, for the run-time immediate reads (mVUemitLoadCurI). w9 is
// free at a block entry: mVUtestCycles loads it first thing. So is x17, the
// address scratch.
static void mVUemitPlaceStub(mV, u32 word)
{
	vixl::ExactAssemblyScope scope(armAsm, mVUplaceStubBytes());
	armAsm->movz(a64::w9, word & 0xffff);
	armAsm->movk(a64::w9, word >> 16, 16);
	armAsm->str(a64::w9, mVUfieldMem(mVU, &mVU.place));
	if (EmuConfig.Gamefixes.IbitHack)
	{
		armAsm->ubfiz(a64::x9, a64::x9, 3, mVUplaceRootBits);
		armAsm->ldr(RSCRATCHADDR, mVUstateMem(offsetof(VURegs, Micro)));
		armAsm->add(a64::x9, RSCRATCHADDR, a64::Operand(a64::x9));
		armAsm->str(a64::x9, mVUfieldMem(mVU, &mVU.placeMicro));
	}
}

// The entry stub of a block being compiled: every way into the block from
// outside its chunk (the dispatcher, the jump cache, a link from another
// chunk, a resume) lands here and gets the placement set. Links inside the
// chunk skip it.
static void mVUemitRootStub(mV)
{
	u8* const at = armGetCurrentCodePointer();
	mVUemitPlaceStub(mVU, mVUplaceWord(mVU.compileRoot, mVU.chunk.slotBase));
	mVUPersist::OnPlaceStub(mVU, at);
}

// Gives the chunk being compiled its next slot and fills it for the placement
// the compile runs as. Returns the slot's number within the chunk.
static u32 mVUchunkAddSlot(mV, microBlock* block, microBlock* target, u32 targetPC, const microRegInfo* key)
{
	const u32 n = static_cast<u32>(mVU.chunk.slots.size());
	pxAssertRel(mVU.chunk.slotBase + n < mVUplaceSlotCapacity, "microVU: out of placement slots");
	mVUChunkSlot slot = {};
	slot.kind = target ? mVUslotLink : mVUslotBlock;
	slot.block = block;
	slot.target = target;
	slot.targetPC = targetPC;
	if (key)
		std::memcpy(&slot.key, key, sizeof(microRegInfo));
	mVU.chunk.slots.push_back(slot);
	mVU.placeSlots[mVU.chunk.slotBase + n] = reinterpret_cast<u64>(target ? target->hostEntry : static_cast<void*>(block));
	mVU.placeSlotsUsed = mVU.chunk.slotBase + n + 1;
	return n;
}

// Gives the chunk being compiled a slot that holds no block: 0 for the code
// to fill when it runs, or the program of the placement.
static u32 mVUchunkAddDataSlot(mV, mVUChunkSlotKind kind)
{
	const u32 n = static_cast<u32>(mVU.chunk.slots.size());
	pxAssertRel(mVU.chunk.slotBase + n < mVUplaceSlotCapacity, "microVU: out of placement slots");
	mVUChunkSlot slot = {};
	slot.kind = kind;
	mVU.chunk.slots.push_back(slot);
	mVU.placeSlots[mVU.chunk.slotBase + n] = kind == mVUslotProgram ? reinterpret_cast<u64>(mVU.prog.cur) : 0;
	mVU.placeSlotsUsed = mVU.chunk.slotBase + n + 1;
	return n;
}

// The slot the chunk being compiled gave `block`, one of its own blocks.
static u32 mVUchunkBlockSlot(mV, const microBlock* block)
{
	for (size_t n = mVU.chunk.slots.size(); n-- > 0;)
	{
		if (mVU.chunk.slots[n].kind == mVUslotBlock && mVU.chunk.slots[n].block == block)
			return static_cast<u32>(n);
	}
	pxFailRel("microVU: a block of the chunk has no slot");
	return 0;
}

// dst = slot `n` of the placement now running. Leaves the flags alone.
static void mVUemitLoadSlot(mV, const a64::Register& dst, const a64::Register& tmp, u32 n)
{
	armAsm->Ldr(dst.W(), mVUfieldMem(mVU, &mVU.place));
	armAsm->Ldr(tmp.X(), mVUfieldMem(mVU, &mVU.placeSlots));
	armAsm->Lsr(dst.W(), dst.W(), mVUplaceRootBits);
	armAsm->Add(tmp.X(), tmp.X(), a64::Operand(dst.X(), a64::LSL, 3));
	armAsm->Ldr(dst.X(), a64::MemOperand(tmp.X(), static_cast<s64>(n) * 8));
}

// dst = root + (pc / 8 - compile root), not yet wrapped.
__fi static void mVUemitRootPlus(mV, const a64::Register& dst, u32 pc)
{
	const u32 dist = ((pc - mVU.compileRoot) / 8) & 0x7ff;
	armAsm->Ldr(dst.W(), mVUfieldMem(mVU, &mVU.place));
	if (dist)
		armAsm->Add(dst.W(), dst.W(), dist);
}

// dst = the micro-memory byte address `pc`.
static void mVUemitPC(mV, const a64::Register& dst, u32 pc)
{
	if (!mVUrootRelative(mVU))
	{
		armAsm->Mov(dst.W(), pc);
		return;
	}
	mVUemitRootPlus(mVU, dst, pc);
	armAsm->Ubfiz(dst.W(), dst.W(), 3, 11);
}

// dst = the link value of the BAL or JALR at `pc`: its own pair index plus 2.
// The pair wraps at the top of micro memory and the sum does not, so a link
// from the last two pairs is 0x800 or 0x801, as the console writes it.
static void mVUemitLinkValue(mV, const a64::Register& dst, u32 pc)
{
	if (!mVUrootRelative(mVU))
	{
		armAsm->Mov(dst.W(), pc / 8 + 2);
		return;
	}
	mVUemitRootPlus(mVU, dst, pc);
	armAsm->And(dst.W(), dst.W(), 0x7ff);
	armAsm->Add(dst.W(), dst.W(), 2);
}

// Under IbitHack, the bits of lower word `code` the compiled code depends on
// when the op reads its immediate from micro memory at run time; 0 for any
// other op. The run-time immediates: LQ/SQ/ILW/ISW imm11 (bits 0-10),
// IADDIU/ISUBIU imm15 (bits 0-10 and 21-24), IADDI imm5 (bits 6-10).
static u32 mVUibitHackKeptBits(u32 code)
{
	switch (code >> 25)
	{
		case 0x00: case 0x01: case 0x04: case 0x05: // LQ, SQ, ILW, ISW
			return ~0x000007FFu;
		case 0x08: case 0x09:                       // IADDIU, ISUBIU
			return ~0x01E007FFu;
		case 0x40:                                  // IADDI
			return ((code & 0x3F) == 0x32) ? ~0x000007C0u : 0;
		default:
			return 0;
	}
}

// dst = the word at iPC, read from micro memory when the block runs (the
// IbitHack gamefix). `runtimeBits` are the bits of it the caller goes on to
// use: the compiled code does not depend on them. Clobbers RSCRATCHADDR, as
// armLoadPtr does.
//
// VU1 reads the word at its distance from mVU.placeMicro, the placement's
// root in micro memory, with no wrap. The distance is the word's own address
// less the root's, signed, so at the compile's own root every read is in micro
// memory, a chunk that runs across the top included. The chunk store places
// the code at another root only where every word the chunk read, these among
// them, is in micro memory there (mVUchunkBodyMatches).
static void mVUemitLoadCurI(mV, const a64::Register& dst, u32 runtimeBits)
{
	if (!mVUrootRelative(mVU))
	{
		armLoadPtr(dst.W(), &rawI);
		return;
	}
	if (mVU.chunk.recording)
		mVU.chunk.runtimeBits.push_back({iPC, runtimeBits});
	const s32 off = (static_cast<s32>(xPC) - static_cast<s32>(mVU.compileRoot)) + (iPC & 1) * 4;
	armAsm->Ldr(dst.X(), mVUfieldMem(mVU, &mVU.placeMicro));
	if (off >= -256)
	{
		armAsm->Ldr(dst.W(), a64::MemOperand(dst.X(), off));
	}
	else
	{
		armAsm->Mov(RSCRATCHADDR, off);
		armAsm->Ldr(dst.W(), a64::MemOperand(dst.X(), RSCRATCHADDR));
	}
}

// The body of the block of the chunk being compiled whose entry is
// `hostEntry`, or null if that entry is not a block of this chunk.
static u8* mVUchunkBody(mV, const void* hostEntry)
{
	for (const mVUChunkEpisode::Block& cb : mVU.chunk.blocks)
	{
		if (cb.stub == hostEntry && cb.block->hostEntry == hostEntry)
			return cb.body;
	}
	return nullptr;
}

// The slot of the link from the chunk being compiled to the block outside it
// that a search at `pc` with `key` finds and whose entry is `hostEntry`.
// -1 if no such block is registered: the entry is then code only this compile
// knows about.
static int mVUchunkLinkSlot(mV, microBlock* target, const void* hostEntry, u32 pc, microRegInfo* key)
{
	if (!target && mVUblocks[pc / 8])
		target = mVUblocks[pc / 8]->search(mVU, key);
	if (!target || target->hostEntry != hostEntry)
		return -1;
	return static_cast<int>(mVUchunkAddSlot(mVU, nullptr, target, pc, key));
}

// Branches to the block at `pc` whose entry is `hostEntry`; a search there
// with `key` found it (`target`, or null to have it searched for again).
//
// A block of the chunk being compiled is entered at its body by a direct
// branch: the placement is already the one its stub would write. A block
// outside the chunk is a different block for every placement of this code, so
// the branch goes through the placement's slot for it and lands on that
// block's stub.
static void mVUemitLink(mV, microBlock* target, void* hostEntry, u32 pc, microRegInfo* key)
{
	if (!mVUrootRelative(mVU))
	{
		armEmitJmp(hostEntry);
		return;
	}
	if (const u8* body = mVUchunkBody(mVU, hostEntry))
	{
		armEmitJmp(body);
		return;
	}
	const int slot = mVUchunkLinkSlot(mVU, target, hostEntry, pc, key);
	if (slot < 0)
	{
		mVU.chunk.unstorable = true;
		armEmitJmp(hostEntry);
		return;
	}
	mVUemitLoadSlot(mVU, a64::x9, a64::x10, static_cast<u32>(slot));
	armAsm->Br(a64::x9);
}

// mVUemitLink taken on `cond`.
static void mVUemitCondLink(mV, a64::Condition cond, microBlock* target, u32 pc, microRegInfo* key)
{
	void* const hostEntry = target->hostEntry;
	const u8* body = mVUrootRelative(mVU) ? mVUchunkBody(mVU, hostEntry) : static_cast<const u8*>(hostEntry);
	if (body)
	{
		armEmitCondBranch(cond, body);
		return;
	}
	const int slot = mVUchunkLinkSlot(mVU, target, hostEntry, pc, key);
	if (slot < 0)
	{
		mVU.chunk.unstorable = true;
		armEmitCondBranch(cond, hostEntry);
		return;
	}
	a64::Label skip;
	armAsm->B(&skip, a64::InvertCondition(cond));
	mVUemitLoadSlot(mVU, a64::x9, a64::x10, static_cast<u32>(slot));
	armAsm->Br(a64::x9);
	armAsm->Bind(&skip);
}

// The compile is about to go on at `pc`, the pair after the one it just did.
// Pair 0 can only follow the last pair of micro memory.
__fi static void mVUchunkNoteFallThrough(mV, u32 pc)
{
	if (pc == 0)
		mVU.chunk.wrapped = true;
}

// An FSAND, FSEQ or FSOR is being compiled.
__fi static void mVUnoteStatusReader(mV, u32 imm12)
{
	mVU.chunk.statusImm |= imm12;
	mVUvuAotOnStatusReader(mVU, imm12);
}
