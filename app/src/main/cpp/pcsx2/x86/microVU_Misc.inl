// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <bitset>
#include <optional>

//------------------------------------------------------------------
// Micro VU - Reg Loading/Saving/Shuffling/Unpacking/Merging...
//------------------------------------------------------------------

void mVUunpack_xyzw(const xmm& dstreg, const xmm& srcreg, int xyzw)
{
    switch (xyzw)
    {
//		case 0: xPSHUF.D(dstreg, srcreg, 0x00); break; // XXXX
        case 0: armAsm->Dup(dstreg.V4S(), srcreg.V4S(), 0); break; // XXXX
//		case 1: xPSHUF.D(dstreg, srcreg, 0x55); break; // YYYY
        case 1: armAsm->Dup(dstreg.V4S(), srcreg.V4S(), 1); break; // YYYY
//		case 2: xPSHUF.D(dstreg, srcreg, 0xaa); break; // ZZZZ
        case 2: armAsm->Dup(dstreg.V4S(), srcreg.V4S(), 2); break; // ZZZZ
//		case 3: xPSHUF.D(dstreg, srcreg, 0xff); break; // WWWW
        case 3: armAsm->Dup(dstreg.V4S(), srcreg.V4S(), 3); break; // WWWW
    }
}

void mVUloadReg(const xmm& reg, const a64::MemOperand& ptr, int xyzw)
{
    switch (xyzw)
    {
        case 8: {
//            xMOVSSZX(reg, ptr32[ptr]);
            armAsm->Ldr(reg.S(), ptr);
            break; // X
        }
        case 4: {
//            xMOVSSZX(reg, ptr32[ptr + 4]);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 4);
            armAsm->Ldr(reg.S(), a64::MemOperand(RXVIXLSCRATCH));
            break; // Y
        }
        case 2: {
//            xMOVSSZX(reg, ptr32[ptr + 8]);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 8);
            armAsm->Ldr(reg.S(), a64::MemOperand(RXVIXLSCRATCH));
            break; // Z
        }
        case 1: {
//            xMOVSSZX(reg, ptr32[ptr + 12]);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 12);
            armAsm->Ldr(reg.S(), a64::MemOperand(RXVIXLSCRATCH));
            break; // W
        }
        default: {
//            xMOVAPS(reg, ptr128[ptr]);
            armAsm->Ldr(reg.Q(), ptr);
            break;
        }
    }
}

void mVUloadIreg(const xmm& reg, int xyzw, VURegs* vuRegs)
{
//	xMOVSSZX(reg, ptr32[&vuRegs->VI[REG_I].UL]);
    armAsm->Ldr(reg, PTR_CPU(vuRegs[g_cpuRegistersPack.vuRegs->idx].VI[REG_I].UL));
	if (!_XYZWss(xyzw)) {
//        xSHUF.PS(reg, reg, 0);
        armSHUFPS(reg, reg, 0);
    }
}

// Modifies the Source Reg!
void mVUsaveReg(const xmm& reg, const a64::MemOperand& ptr, int xyzw, bool modXYZW)
{
    switch (xyzw)
    {
        case 5: // YW
//			xEXTRACTPS(ptr32[ptr + 4], reg, 1);
//			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 4);
            armAsm->St1(reg.V4S(), 1, a64::MemOperand(RXVIXLSCRATCH)); // Y
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 12);
            armAsm->St1(reg.V4S(), 3, a64::MemOperand(RXVIXLSCRATCH)); // W
            break;
        case 6: // YZ
//			xPSHUF.D(reg, reg, 0xc9);
//			xMOVL.PS(ptr64[ptr + 4], reg);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 4);
            armAsm->St1(reg.V4S(), 1, a64::MemOperand(RXVIXLSCRATCH, 4, a64::PostIndex));
            armAsm->St1(reg.V4S(), 2, a64::MemOperand(RXVIXLSCRATCH));
            break;
        case 7: // YZW
//			xMOVH.PS(ptr64[ptr + 8], reg);
//			xEXTRACTPS(ptr32[ptr + 4], reg, 1);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 4);
            armAsm->St1(reg.V4S(), 1, a64::MemOperand(RXVIXLSCRATCH, 4, a64::PostIndex));
            armAsm->St1(reg.V2D(), 1, a64::MemOperand(RXVIXLSCRATCH));
            break;
        case 9: // XW
//			xMOVSS(ptr32[ptr], reg);
//			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 12);
            armAsm->Str(reg.S(), ptr); // X
            armAsm->St1(reg.V4S(), 3, a64::MemOperand(RXVIXLSCRATCH)); // W
            break;
        case 10: // XZ
//			xMOVSS(ptr32[ptr], reg);
//			xEXTRACTPS(ptr32[ptr + 8], reg, 2);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 8);
            armAsm->Str(reg.S(), ptr); // X
            armAsm->St1(reg.V4S(), 2, a64::MemOperand(RXVIXLSCRATCH)); // Z
            break;
        case 11: // XZW
//			xMOVSS(ptr32[ptr], reg);
//			xMOVH.PS(ptr64[ptr + 8], reg);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 8);
            armAsm->Str(reg.S(), ptr); // X
            armAsm->St1(reg.V2D(), 1, a64::MemOperand(RXVIXLSCRATCH)); // ZW
            break;
        case 13: // XYW
//			xMOVL.PS(ptr64[ptr], reg);
//			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 12);
            armAsm->Str(reg.D(), ptr);
            armAsm->St1(reg.V4S(), 3, a64::MemOperand(RXVIXLSCRATCH));
            break;
        case 14: // XYZ
//			xMOVL.PS(ptr64[ptr], reg);
//			xEXTRACTPS(ptr32[ptr + 8], reg, 2);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 8);
            armAsm->Str(reg.D(), ptr);
            armAsm->St1(reg.V4S(), 2, a64::MemOperand(RXVIXLSCRATCH)); // Z
            break;
        case 4: // Y
            if (!modXYZW)
                mVUunpack_xyzw(reg, reg, 1);
//			xMOVSS(ptr32[ptr + 4], reg);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 4);
            armAsm->Str(reg.S(), a64::MemOperand(RXVIXLSCRATCH));
            break;
        case 2: // Z
            if (!modXYZW)
                mVUunpack_xyzw(reg, reg, 2);
//			xMOVSS(ptr32[ptr + 8], reg);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 8);
            armAsm->Str(reg.S(), a64::MemOperand(RXVIXLSCRATCH));
            break;
        case 1: // W
            if (!modXYZW)
                mVUunpack_xyzw(reg, reg, 3);
//			xMOVSS(ptr32[ptr + 12], reg);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 12);
            armAsm->Str(reg.S(), a64::MemOperand(RXVIXLSCRATCH));
            break;
        case 8: // X
//			xMOVSS(ptr32[ptr], reg);
            armAsm->Str(reg.S(), ptr);
            break;
        case 12: // XY
//			xMOVL.PS(ptr64[ptr], reg);
            armAsm->Str(reg.D(), ptr);
            break;
        case 3: // ZW
//			xMOVH.PS(ptr64[ptr + 8], reg);
            armGetMemOperandInRegister(RXVIXLSCRATCH, ptr, 8);
            armAsm->St1(reg.V2D(), 1, a64::MemOperand(RXVIXLSCRATCH));
            break;
        default: // XYZW
//			xMOVAPS(ptr128[ptr], reg);
            armAsm->Str(reg.Q(), ptr);
            break;
    }
}

// Modifies the Source Reg! (ToDo: Optimize modXYZW = 1 cases)
void mVUmergeRegs(const xmm& dest, const xmm& src, int xyzw, bool modXYZW)
{
    xyzw &= 0xf;
    if (!dest.Is(src) && (xyzw != 0))
    {
        if (xyzw == 0x8)
            armAsm->Mov(dest.V4S(), 0, src.V4S(), 0);
        else if (xyzw == 0xf)
            armAsm->Mov(dest.Q(), src.Q());
        else
        {
            if (modXYZW)
            {
                if (xyzw == 1)
                {
                    armAsm->Ins(dest.V4S(), 3, src.V4S(), 0);
                    return;
                }
                else if (xyzw == 2)
                {
                    armAsm->Ins(dest.V4S(), 2, src.V4S(), 0);
                    return;
                }
                else if (xyzw == 4)
                {
                    armAsm->Ins(dest.V4S(), 1, src.V4S(), 0);
                    return;
                }
            }

            if (xyzw == 0)
                return;
            if (xyzw == 15)
            {
                armAsm->Mov(dest, src);
                return;
            }

            //if (xyzw == 14 && canModifySrc)
            if (xyzw == 14)
            {
                // xyz - we can get rid of the mov if we swap the RA around
                armAsm->Mov(src.V4S(), 3, dest.V4S(), 3);
                armAsm->Mov(dest.V16B(), src.V16B());
                return;
            }

            // reverse
            xyzw = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);

            if ((xyzw & 3) == 3)
            {
                // xy
                armAsm->Mov(dest.V2D(), 0, src.V2D(), 0);
                xyzw &= ~3;
            }
            else if ((xyzw & 12) == 12)
            {
                // zw
                armAsm->Mov(dest.V2D(), 1, src.V2D(), 1);
                xyzw &= ~12;
            }

            // xyzw
            u32 i;
            for (i = 0; i < 4; ++i)
            {
                if (xyzw & (1u << i))
                    armAsm->Mov(dest.V4S(), i, src.V4S(), i);
            }
        }
    }
}

//------------------------------------------------------------------
// Micro VU - Misc Functions
//------------------------------------------------------------------

// Backup Volatile Regs (EAX, ECX, EDX, MM0~7, XMM0~7, are all volatile according to 32bit Win/Linux ABI)
__fi void mVUbackupRegs(microVU& mVU, bool toMemory = false, bool onlyNeeded = false)
{
    if (toMemory)
    {
        int i, e = iREGCNT_GPR;
        for (i = 0; i < e; ++i)
        {
            if (!armIsCallerSaved(i) || i == 4)
                continue;

            if (!onlyNeeded || mVU.regAlloc->checkCachedGPR(i)) {
                armAsm->Push(a64::xzr, a64::XRegister(i));
            }
        }

        ////

        e = iREGCNT_XMM;
        for (i = 0; i < e; ++i)
        {
            if (!armIsCallerSavedXmm(i))
                continue;

            if (!onlyNeeded || mVU.regAlloc->checkCachedReg(i) || xmmPQ.GetCode() == i) {
                armAsm->Push(a64::xzr, a64::DRegister(i));
            }
        }
    }
    else
    {
        // TODO(Stenzek): get rid of xmmbackup
        mVU.regAlloc->flushAll(); // Flush Regalloc
//		xMOVAPS(ptr128[&mVU.xmmBackup[xmmPQ.GetCode()][0]], xmmPQ);
        armAsm->Str(xmmPQ.Q(), PTR_MVU(microVU[mVU.index].xmmBackup[xmmPQ.GetCode()][0]));
    }
}

// Restore Volatile Regs
__fi void mVUrestoreRegs(microVU& mVU, bool fromMemory = false, bool onlyNeeded = false)
{
    if (fromMemory)
    {
        int i, e = iREGCNT_XMM - 1;
        for (i = e; i >= 0; --i)
        {
            if (!armIsCallerSavedXmm(i))
                continue;

            if (!onlyNeeded || mVU.regAlloc->checkCachedReg(i) || xmmPQ.GetCode() == i) {
                armAsm->Pop(a64::DRegister(i), a64::xzr);
            }
        }

        ////

        e = iREGCNT_GPR - 1;
        for (i = e; i >= 0; --i)
        {
            if (!armIsCallerSaved(i)  || i == 4)
                continue;

            if (!onlyNeeded || mVU.regAlloc->checkCachedGPR(i)) {
                armAsm->Pop(a64::XRegister(i), a64::xzr);
            }
        }
    }
    else
    {
//		xMOVAPS(xmmPQ, ptr128[&mVU.xmmBackup[xmmPQ.GetCode()][0]]);
        armAsm->Ldr(xmmPQ.Q(), PTR_MVU(microVU[mVU.index].xmmBackup[xmmPQ.GetCode()][0]));
    }
}

#if 0
// Gets called by mVUaddrFix at execution-time
static void mVUwarningRegAccess(u32 prog, u32 pc)
{
	Console.Error("microVU0 Warning: Accessing VU1 Regs! [%04x] [%x]", pc, prog);
}
#endif

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

static inline u32 branchAddr(const mV)
{
	pxAssumeMsg(islowerOP, "MicroVU: Expected Lower OP code for valid branch addr.");
    // return ((((iPC + 2) + (_Imm11_ * 2)) & mVU.progMemMask) * 4)
	return ((((iPC + 2) + (_Imm11_ << 1)) & mVU.progMemMask) << 2);
}

static void mVUwaitMTVU()
{
	if (IsDevBuild)
		DevCon.WriteLn("microVU0: Waiting on VU1 thread to access VU1 regs!");
	vu1Thread.WaitVU();
}

// Transforms the Address in gprReg to valid VU0/VU1 Address
__fi void mVUaddrFix(mV, const a64::Register& gprReg)
{
    auto reg32 = gprReg.W();
	if (isVU1)
	{
//		xAND(xRegister32(gprReg.Id), 0x3ff); // wrap around
        armAsm->And(reg32, reg32, 0x3ff);
//		xSHL(xRegister32(gprReg.Id), 4);
        armAsm->Lsl(reg32, reg32, 4);
	}
	else
	{
//		xTEST(xRegister32(gprReg.Id), 0x400);
        armAsm->Tst(reg32, 0x400);
//		xForwardJNZ8 jmpA; // if addr & 0x4000, reads VU1's VF regs and VI regs
        a64::Label jmpA;
        armAsm->B(&jmpA, a64::Condition::ne);
//			xAND(xRegister32(gprReg.Id), 0xff); // if !(addr & 0x4000), wrap around
            armAsm->And(reg32, reg32, 0xff);
//			xForwardJump32 jmpB;
            a64::Label jmpB;
            armAsm->B(&jmpB);
//		jmpA.SetTarget();
        armBind(&jmpA);
			if (THREAD_VU1)
			{
#if 0
					if (IsDevBuild && !isCOP2) // Lets see which games do this!
					{
						xMOV(gprT1, mVU.prog.cur->idx); // Note: Kernel does it via COP2 to initialize VU1!
						xMOV(gprT2, xPC);               // So we don't spam console, we'll only check micro-mode...
						mVUbackupRegs(mVU, true, false);
						xFastCall((void*)mVUwarningRegAccess, arg1regd, arg2regd);
						mVUrestoreRegs(mVU, true, false);
					}
#endif
//				xFastCall((void*)mVU.waitMTVU);
                armEmitCall(reinterpret_cast<void*>(mVU.waitMTVU));
			}
//			xAND(xRegister32(gprReg.Id), 0x3f); // ToDo: theres a potential problem if VU0 overrides VU1's VF0/VI0 regs!
            armAsm->And(reg32, reg32, 0x3f);

//			xADD(gprReg, (u128*)VU1.VF - (u128*)VU0.Mem);
            a64::MemOperand mop1 = PTR_CPU(vuRegs[1].VF);
            a64::MemOperand mop2 = PTR_CPU(vuRegs[0].Mem);
            armAsm->Sub(REX, mop1.GetBaseRegister(), mop2.GetBaseRegister());
            armAsm->Add(gprReg, gprReg, REX);

//		jmpB.SetTarget();
        armBind(&jmpB);
//		xSHL(gprReg, 4); // multiply by 16 (shift left by 4)
        armAsm->Lsl(gprReg, gprReg, 4);
	}
}

__fi std::optional<a64::MemOperand> mVUoptimizeConstantAddr(mV, u32 srcreg, s32 offset, s32 offsetSS_)
{
	// if we had const prop for VIs, we could do that here..
	if (srcreg != 0)
		return std::nullopt;

//    armMoveAddressToReg(REX, mVU.regs().Mem);
    armAsm->Ldr(REX, PTR_CPU(vuRegs[mVU.index].Mem));

	const s32 addr = 0 + offset;
	if (isVU1)
	{
//		return ptr[mVU.regs().Mem + ((addr & 0x3FFu) << 4) + offsetSS_];
        armAsm->Add(REX, REX, ((addr & 0x3FFu) << 4) + offsetSS_);
	}
	else
	{
		if (addr & 0x400)
			return std::nullopt;

//		return ptr[mVU.regs().Mem + ((addr & 0xFFu) << 4) + offsetSS_];
        armAsm->Add(REX, REX, ((addr & 0xFFu) << 4) + offsetSS_);
	}
    return a64::MemOperand(REX);
}

//------------------------------------------------------------------
// Micro VU - Custom SSE Instructions
//------------------------------------------------------------------

//struct SSEMasks
//{
//	u32 MIN_MAX_1[4], MIN_MAX_2[4], ADD_SS[4];
//};

//alignas(16) static const SSEMasks sseMasks =
//{
//	{0xffffffff, 0x80000000, 0xffffffff, 0x80000000},
//	{0x00000000, 0x40000000, 0x00000000, 0x40000000},
//	{0x80000000, 0xffffffff, 0xffffffff, 0xffffffff},
//};


// Warning: Modifies t1 and t2
void MIN_MAX_PS(microVU& mVU, const xmm& to, const xmm& from, const xmm& t1in, const xmm& t2in, bool min)
{
	const xmm& t1 = t1in.IsNone() ? mVU.regAlloc->allocReg() : t1in;
	const xmm& t2 = t2in.IsNone() ? mVU.regAlloc->allocReg() : t2in;

    if (0) // use double comparison
    {
        // ZW
//		xPSHUF.D(t1, to, 0xfa); 1234 => 3344
        armPSHUFD(t1, to, 0xfa);
//		xPAND   (t1, ptr128[sseMasks.MIN_MAX_1]);
        armAsm->And(t1.V16B(), t1.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_1)).V16B());
//		xPOR    (t1, ptr128[sseMasks.MIN_MAX_2]);
        armAsm->Orr(t1.V16B(), t1.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_2)).V16B());
//		xPSHUF.D(t2, from, 0xfa); 1234 => 3344
        armPSHUFD(t2, from, 0xfa);
//		xPAND   (t2, ptr128[sseMasks.MIN_MAX_1]);
        armAsm->And(t2.V16B(), t2.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_1)).V16B());
//		xPOR    (t2, ptr128[sseMasks.MIN_MAX_2]);
        armAsm->Orr(t2.V16B(), t2.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_2)).V16B());
//		if (min) xMIN.PD(t1, t2);
        if (min) armAsm->Fminnm(t1.V2D(), t1.V2D(), t2.V2D());
//		else     xMAX.PD(t1, t2);
        else     armAsm->Fmaxnm(t1.V2D(), t1.V2D(), t2.V2D());

        // XY
//		xPSHUF.D(t2, from, 0x50); 1234 => 1122
        armPSHUFD(t2, from, 0x50);
//		xPAND   (t2, ptr128[sseMasks.MIN_MAX_1]);
        armAsm->And(t2.V16B(), t2.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_1)).V16B());
//		xPOR    (t2, ptr128[sseMasks.MIN_MAX_2]);
        armAsm->Orr(t2.V16B(), t2.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_2)).V16B());
//		xPSHUF.D(to, to, 0x50); 1234 => 1122
        armPSHUFD(to, to, 0x50);
//		xPAND   (to, ptr128[sseMasks.MIN_MAX_1]);
        armAsm->And(to.V16B(), to.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_1)).V16B());
//		xPOR    (to, ptr128[sseMasks.MIN_MAX_2]);
        armAsm->Orr(to.V16B(), to.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_2)).V16B());
//		if (min) xMIN.PD(to, t2);
//		else     xMAX.PD(to, t2);
        if (min) armAsm->Fminnm(to.V2D(), to.V2D(), t2.V2D());
        else     armAsm->Fmaxnm(to.V2D(), to.V2D(), t2.V2D());

//		xSHUF.PS(to, t1, 0x88);
        armSHUFPS(to, t1, 0x88);
    }
    else // use integer comparison
    {
        const xmm& c1 = min ? t2 : t1;
        const xmm& c2 = min ? t1 : t2;

//		xMOVAPS  (t1, to);
        armAsm->Mov(t1, to);
//		xPSRA.D  (t1, 31);
        armAsm->Sshr(t1.V4S(), t1.V4S(), 31);
//		xPSRL.D  (t1,  1);
        armAsm->Ushr(t1.V4S(), t1.V4S(), 1);
//		xPXOR    (t1, to);
        armAsm->Eor(t1.V16B(), t1.V16B(), to.V16B());

//		xMOVAPS  (t2, from);
        armAsm->Mov(t2, from);
//		xPSRA.D  (t2, 31);
        armAsm->Sshr(t2.V4S(), t2.V4S(), 31);
//		xPSRL.D  (t2,  1);
        armAsm->Ushr(t2.V4S(), t2.V4S(), 1);
//		xPXOR    (t2, from);
        armAsm->Eor(t2.V16B(), t2.V16B(), from.V16B());

//		xPCMP.GTD(c1, c2);
        armAsm->Cmgt(c1.V4S(), c1.V4S(), c2.V4S());
//		xPAND    (to, c1);
        armAsm->And(to.V16B(), to.V16B(), c1.V16B());
//		xPANDN   (c1, from);
        armAsm->Bic(c1.V16B(), from.V16B(), c1.V16B());
//		xPOR     (to, c1);
        armAsm->Orr(to.V16B(), to.V16B(), c1.V16B());
    }

//	if (t1 != t1in) mVU.regAlloc->clearNeeded(t1);
    if (!t1.Is(t1in)) mVU.regAlloc->clearNeeded(t1);
//	if (t2 != t2in) mVU.regAlloc->clearNeeded(t2);
    if (!t2.Is(t2in)) mVU.regAlloc->clearNeeded(t2);
}

// Warning: Modifies to's upper 3 vectors, and t1
void MIN_MAX_SS(mV, const xmm& to, const xmm& from, const xmm& t1in, bool min)
{
	const xmm& t1 = t1in.IsNone() ? mVU.regAlloc->allocReg() : t1in;
//	xSHUF.PS(to, from, 0);
    armSHUFPS(to, from, 0);
//	xPAND   (to, ptr128[sseMasks.MIN_MAX_1]);
    armAsm->And(to.V16B(), to.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_1)).V16B());
//	xPOR    (to, ptr128[sseMasks.MIN_MAX_2]);
    armAsm->Orr(to.V16B(), to.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.MIN_MAX_2)).V16B());
//	xPSHUF.D(t1, to, 0xee);
    armAsm->Dup(t1.V2D(), to.V2D(), 1); //v3v2v3v2
//	if (min) xMIN.PD(to, t1);
//	else	 xMAX.PD(to, t1);
    if (min) armAsm->Fminnm(to.V2D(), to.V2D(), t1.V2D());
    else     armAsm->Fmaxnm(to.V2D(), to.V2D(), t1.V2D());
    if (!t1.Is(t1in)) 
		mVU.regAlloc->clearNeeded(t1);
}

// Not Used! - TriAce games only need a portion of this code to boot (see function below)
// What this code attempts to do is do a floating point ADD with only 1 guard bit,
// whereas FPU calculations that follow the IEEE standard have 3 guard bits (guard|round|sticky)
// Warning: Modifies all vectors in 'to' and 'from', and Modifies t1in
void ADD_SS_Single_Guard_Bit(microVU& mVU, const xmm& to, const xmm& from, const xmm& t1in)
{
	const xmm& t1 = t1in.IsNone() ? mVU.regAlloc->allocReg() : t1in;

//	xMOVD(eax, to);
    armAsm->Fmov(EAX, to.S());
//	xMOVD(ecx, from);
    armAsm->Fmov(ECX, from.S());
//	xSHR (eax, 23);
    armAsm->Lsr(EAX, EAX, 23);
//	xSHR (ecx, 23);
    armAsm->Lsr(ECX, ECX, 23);
//	xAND (eax, 0xff);
    armAsm->And(EAX, EAX, 0xff);
//	xAND (ecx, 0xff);
    armAsm->And(ECX, ECX, 0xff);
//	xSUB (ecx, eax); // Exponent Difference
    armAsm->Subs(ECX, ECX, EAX);

//	xForwardJL8 case_neg;
    a64::Label case_neg;
    armAsm->B(&case_neg, a64::Condition::lt);
//	xForwardJE8 case_end1;
    a64::Label case_end1;
    armAsm->B(&case_end1, a64::Condition::eq);

//	xCMP (ecx, 24);
    armAsm->Cmp(ECX, 24);
//	xForwardJLE8 case_pos_small;
    a64::Label case_pos_small;
    armAsm->B(&case_pos_small, a64::Condition::le);

    // case_pos_big:
//	xPAND(to, ptr128[sseMasks.ADD_SS]);
    armAsm->And(to.V16B(), to.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.ADD_SS)).V16B());
//	xForwardJump8 case_end2;
    a64::Label case_end2;
    armAsm->B(&case_end2);

//	case_pos_small.SetTarget();
    armBind(&case_pos_small);
//	xDEC   (ecx);
    armAsm->Sub(ECX, ECX, 1);
//	xMOV   (eax, 0xffffffff);
    armAsm->Mov(EAX, 0xffffffff);
//	xSHL   (eax, cl);
    armAsm->Lsl(EAX, EAX, ECX);
//	xMOVDZX(t1, eax);
    armAsm->Fmov(t1.S(), EAX);
//	xPAND  (to, t1);
    armAsm->And(to.V16B(), to.V16B(), t1.V16B());
//	xForwardJump8 case_end3;
    a64::Label case_end3;
    armAsm->B(&case_end3);

//	case_neg.SetTarget();
    armBind(&case_neg);
//	xCMP (ecx, -24);
    armAsm->Cmp(ECX, -24);
//	xForwardJGE8 case_neg_small;
    a64::Label case_neg_small;
    armAsm->B(&case_neg_small, a64::Condition::ge);

    // case_neg_big:
//	xPAND(from, ptr128[sseMasks.ADD_SS]);
    armAsm->And(from.V16B(), from.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.ADD_SS)).V16B());
//	xForwardJump8 case_end4;
    a64::Label case_end4;
    armAsm->B(&case_end4);

//	case_neg_small.SetTarget();
    armBind(&case_neg_small);
//	xNOT   (ecx); // -ecx - 1
    armAsm->Mvn(ECX, ECX);
//	xMOV   (eax, 0xffffffff);
    armAsm->Mov(EAX, 0xffffffff);
//	xSHL   (eax, cl);
    armAsm->Lsl(EAX, EAX, ECX);
//	xMOVDZX(t1, eax);
    armAsm->Fmov(t1.S(), EAX);
//	xPAND  (from, t1);
    armAsm->And(from.V16B(), from.V16B(), t1.V16B());

//	case_end1.SetTarget();
    armBind(&case_end1);
//	case_end2.SetTarget();
    armBind(&case_end2);
//	case_end3.SetTarget();
    armBind(&case_end3);
//	case_end4.SetTarget();
    armBind(&case_end4);

//	xADD.SS(to, from);
    armAsm->Fadd(to.S(), to.S(), from.S());
    if (!t1.Is(t1in))
		mVU.regAlloc->clearNeeded(t1);
}

// Turns out only this is needed to get TriAce games booting with mVU
// Modifies from's lower vector
void ADD_SS_TriAceHack(microVU& mVU, const xmm& to, const xmm& from)
{
//	xMOVD(eax, to);
    armAsm->Fmov(EAX, to.S());
//	xMOVD(ecx, from);
    armAsm->Fmov(ECX, from.S());
//	xSHR (eax, 23);
    armAsm->Lsr(EAX, EAX, 23);
//	xSHR (ecx, 23);
    armAsm->Lsr(ECX, ECX, 23);
//	xAND (eax, 0xff);
    armAsm->And(EAX, EAX, 0xff);
//	xAND (ecx, 0xff);
    armAsm->And(ECX, ECX, 0xff);
//	xSUB (ecx, eax); // Exponent Difference
    armAsm->Sub(ECX, ECX, EAX);

//	xCMP (ecx, -25);
    armAsm->Cmp(ECX, -25);
//	xForwardJLE8 case_neg_big;
    a64::Label case_neg_big;
    armAsm->B(&case_neg_big, a64::Condition::le);
//	xCMP (ecx,  25);
    armAsm->Cmp(ECX, 25);
//	xForwardJL8  case_end1;
    a64::Label case_end1;
    armAsm->B(&case_end1, a64::Condition::lt);

    // case_pos_big:
//	xPAND(to, ptr128[sseMasks.ADD_SS]);
    armAsm->And(to.V16B(), to.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.ADD_SS)).V16B());
//	xForwardJump8 case_end2;
    a64::Label case_end2;
    armAsm->B(&case_end2);

//	case_neg_big.SetTarget();
    armBind(&case_neg_big);
//	xPAND(from, ptr128[sseMasks.ADD_SS]);
    armAsm->And(from.V16B(), from.V16B(), armLoadPtrV(PTR_CPU(mVUss4.sseMasks.ADD_SS)).V16B());

//	case_end1.SetTarget();
    armBind(&case_end1);
//	case_end2.SetTarget();
    armBind(&case_end2);

//	xADD.SS(to, from);
    armAsm->Fadd(to.S(), to.S(), from.S());
}

#define clampOp(opX, isPS) \
	do { \
		mVUclamp3(mVU, to, t1, (isPS) ? 0xf : 0x8); \
        mVUclamp3(mVU, from, t1, (isPS) ? 0xf : 0x8); \
		if (isPS) { \
			opX(to.V4S(), to.V4S(), from.V4S()); \
		} else { \
			opX(RQSCRATCH.S(), to.S(), from.S()); \
			armAsm->Ins(to.V4S(), 0, RQSCRATCH.V4S(), 0); \
		} \
		mVUclamp4(mVU, to, t1, (isPS) ? 0xf : 0x8); \
	} while (0)

void SSE_MAXPS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, false);
}
void SSE_MINPS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, true);
}
void SSE_MAXSS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
	MIN_MAX_SS(mVU, to, from, t1, false);
}
void SSE_MINSS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
	MIN_MAX_SS(mVU, to, from, t1, true);
}
void SSE_ADD2SS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
	if (!CHECK_VUADDSUBHACK) {
//        clampOp(xADD.SS, false);
        clampOp(armAsm->Fadd, false);
    }
	else {
        ADD_SS_TriAceHack(mVU, to, from);
    }
}

// Does same as SSE_ADDPS since tri-ace games only need SS implementation of VUADDSUBHACK...
void SSE_ADD2PS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xADD.PS, true);
    clampOp(armAsm->Fadd, true);
}
void SSE_ADDPS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xADD.PS, true);
    clampOp(armAsm->Fadd, true);
}
void SSE_ADDSS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xADD.SS, false);
    clampOp(armAsm->Fadd, false);
}
void SSE_SUBPS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xSUB.PS, true);
    clampOp(armAsm->Fsub, true);
}
void SSE_SUBSS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xSUB.SS, false);
    clampOp(armAsm->Fsub, false);
}
void SSE_MULPS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xMUL.PS, true);
    clampOp(armAsm->Fmul, true);
}
void SSE_MULSS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xMUL.SS, false);
    clampOp(armAsm->Fmul, false);
}
void SSE_DIVPS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xDIV.PS, true);
    clampOp(armAsm->Fdiv, true);
}
void SSE_DIVSS(mV, const xmm& to, const xmm& from, const xmm& t1 = a64::NoVReg, const xmm& t2 = a64::NoVReg)
{
//	clampOp(xDIV.SS, false);
    clampOp(armAsm->Fdiv, false);
}
