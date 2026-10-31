// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 EE Shift Instruction Codegen — memory-based
// All operands loaded from / stored to cpuRegs.GPR memory.
// No register allocation for scalar ops.

#include "arm64/iR5900-arm64.h"
#include "Memory.h"
#include "Patch.h"

#include <algorithm>
#include <bit>

namespace a64 = vixl::aarch64;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {

namespace Interp = R5900::Interpreter::OpcodeImpl;


// Operand helpers. Loads return the register holding the operand, coherent by
// construction: a pin mirror when the guest reg is pinned, an allocator-
// resident slot once residency is flipped on, else the scratch (memory load).
// Callers must treat the returned register as READ-ONLY and put results in
// scratch (or pass them straight to the store helper).
static a64::Register memLoadS32()
{
	return _eeGetGPRSourceReg(RWARG1, _Rs_);
}

static a64::Register memLoadS64()
{
	return _eeGetGPRSourceReg(RXARG1, _Rs_);
}

static a64::Register memLoadT32()
{
	return _eeGetGPRSourceReg(RWSCRATCH, _Rt_);
}

static a64::Register memLoadT64()
{
	return _eeGetGPRSourceReg(RXSCRATCH, _Rt_);
}

static void memStoreD(const a64::Register& src)
{
	_eeStoreGPRDestReg(_Rd_, src);
}

// Dest home for the shift result: the FINAL result-producing instruction
// targets it, then memStoreD deposits (a pin/memory store, or a deferred
// allocator-slot store under the flip). See _eeGetGPRDestReg's contract —
// intermediates stay in scratch. alloc_if_used=false keeps Phase-1 behavior.
static a64::Register memDestD()
{
	return _eeGetGPRDestReg(_Rd_, RXSCRATCH);
}

/*********************************************************
 * Shift pairs — two instructions, one bitfield op        *
 *********************************************************/

// The second instruction reads and overwrites the first one's destination:
//   SLL then SRA/SRL, DSLL/DSLL32 then DSRA/DSRL/DSRA32/DSRL32: sbfx/ubfx or
//   sbfiz/ubfiz. SLL then SRL by 0 is signed.
//   SRL/SRA/DSRL/DSRA/DSRL32/DSRA32 then ANDI 2^n-1: ubfx. An arithmetic
//   shift fuses only when the field stays below the sign bit.
//   ANDI 2^n-1 then SLL/DSLL/DSLL32: ubfiz, or sbfiz once SLL moves the field
//   into bit 31.
struct ShiftPairField
{
	bool is_signed;
	bool insert; // bfiz when set, bfx otherwise
	u32 lsb;
	u32 width;
};

enum ShiftKind { kNotShift, kLeft, kRightLogical, kRightArith };

// A constant-amount shift's kind, total amount and word size.
static ShiftKind decodeShift(u32 code, u32* amount, u32* bits)
{
	if ((code >> 26) != 0)
		return kNotShift;
	const u32 sa = (code >> 6) & 0x1f;
	*amount = sa;
	*bits = 32;
	switch (code & 0x3f)
	{
		case 0x00: return kLeft; // SLL
		case 0x02: return kRightLogical; // SRL
		case 0x03: return kRightArith; // SRA
	}
	*bits = 64;
	switch (code & 0x3f)
	{
		case 0x38: return kLeft; // DSLL
		case 0x3a: return kRightLogical; // DSRL
		case 0x3b: return kRightArith; // DSRA
	}
	*amount = sa + 32;
	switch (code & 0x3f)
	{
		case 0x3c: return kLeft; // DSLL32
		case 0x3e: return kRightLogical; // DSRL32
		case 0x3f: return kRightArith; // DSRA32
	}
	return kNotShift;
}

// n for an ANDI mask of 2^n-1, else 0.
static u32 andiLowMaskBits(u32 code)
{
	const u32 m = code & 0xffff;
	return (m & (m + 1)) == 0 ? static_cast<u32>(std::popcount(m)) : 0;
}

static bool shiftPairField(u32 first, u32 second, ShiftPairField* f)
{
	const bool first_andi = (first >> 26) == 0x0c;
	const u32 dest = first_andi ? (first >> 16) & 0x1f : (first >> 11) & 0x1f;
	const bool second_andi = (second >> 26) == 0x0c;
	const u32 second_src = second_andi ? (second >> 21) & 0x1f : (second >> 16) & 0x1f;
	const u32 second_dest = second_andi ? (second >> 16) & 0x1f : (second >> 11) & 0x1f;
	if (dest == 0 || second_src != dest || second_dest != dest)
		return false;

	u32 a, a_bits, b, b_bits;
	if (first_andi)
	{
		const u32 n = andiLowMaskBits(first);
		if (n == 0 || decodeShift(second, &b, &b_bits) != kLeft)
			return false;
		if (b_bits == 32 && b + n >= 32)
			*f = {true, true, b, 32 - b};
		else
			*f = {false, true, b, std::min(n, b_bits - b)};
		return true;
	}

	const ShiftKind k = decodeShift(first, &a, &a_bits);
	if (k == kNotShift)
		return false;
	if (k != kLeft)
	{
		const u32 n = second_andi ? andiLowMaskBits(second) : 0;
		if (n == 0 || (k == kRightArith && a + n > a_bits))
			return false;
		*f = {false, false, a, std::min(n, a_bits - a)};
		return true;
	}

	const ShiftKind k2 = decodeShift(second, &b, &b_bits);
	if ((k2 != kRightLogical && k2 != kRightArith) || b_bits != a_bits || (a_bits == 64 && a == 0 && b == 0))
		return false;
	const bool is_signed = k2 == kRightArith || (a_bits == 32 && b == 0);
	if (b >= a)
		*f = {is_signed, false, b - a, a_bits - b};
	else
		*f = {is_signed, true, a - b, a_bits - a};
	return true;
}

// Called by the first instruction's non-constant path. When the next
// instruction completes a pair in the same block, emit the pair and leave its
// pc in g_eeShiftPairPc so recompileNextInstruction emits nothing for it.
// Returns false otherwise, having emitted nothing.
bool recShiftPairLead()
{
	// In a delay slot pc is still the slot's own address.
	if (g_recompilingDelaySlot || pc >= recCurrentBlockEndPC())
		return false;
	// recompileNextInstruction would apply these before reading the second
	// instruction; applying them twice writes the same words.
	if (EmuConfig.EnablePatches)
		Patch::ApplyDynamicPatches(pc);
	const u32 second = memRead32(pc);
#ifdef PCSX2_RECOMPILER_TESTS
	if (EERecFallback::Selected(second))
		return false;
#endif
	ShiftPairField f;
	if (!shiftPairField(cpuRegs.code, second, &f))
		return false;

	const bool andi = _Opcode_ == 0x0c;
	const int dest = andi ? _Rt_ : _Rd_;
	const a64::Register src = _eeGetGPRSourceReg(RXSCRATCH, andi ? _Rs_ : _Rt_);
	const a64::Register dst = _eeGetGPRDestReg(dest, RXSCRATCH);
	if (f.insert && f.is_signed)
		armAsm->Sbfiz(dst, src, f.lsb, f.width);
	else if (f.insert)
		armAsm->Ubfiz(dst, src, f.lsb, f.width);
	else if (f.is_signed)
		armAsm->Sbfx(dst, src, f.lsb, f.width);
	else
		armAsm->Ubfx(dst, src, f.lsb, f.width);
	_eeStoreGPRDestReg(dest, dst);
	g_eeShiftPairPc = pc;
	return true;
}

/*********************************************************
 * Shift with constant amount — rd = rt SHIFT sa         *
 * Uses eeRecompileCodeRC2_MEM                            *
 *********************************************************/

//// SLL — rd = sign_extend(rt << sa)
static void recSLL_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] << _Sa_);
}

static void recSLL_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	if (_Sa_ != 0)
	{
		// Sbfiz(sa, 32-sa) == sign_extend((s32)(rt << sa)) in ONE insn (GE-06):
		// source bits [31-sa:0] land at [31:sa], sign-extended from bit 31.
		// Reads only the low 32 source bits, so pin upper halves are inert.
		armAsm->Sbfiz(dst, rt.X(), _Sa_, 32 - _Sa_);
	}
	else
	{
		armAsm->Sxtw(dst, rt);
	}
	memStoreD(dst);
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, SLL, XMMINFO_WRITED | XMMINFO_READT);

//// SRL — rd = sign_extend(rt >> sa) (logical)
static void recSRL_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] >> _Sa_);
}

static void recSRL_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	if (_Sa_ != 0)
	{
		// sa>0 clears bit 31 of the 32-bit result, so zero-extension IS the
		// MIPS sign-extension: Ubfx(sa, 32-sa) in ONE insn (GE-06).
		armAsm->Ubfx(dst, rt.X(), _Sa_, 32 - _Sa_);
	}
	else
	{
		armAsm->Sxtw(dst, rt);
	}
	memStoreD(dst);
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, SRL, XMMINFO_WRITED | XMMINFO_READT);

//// SRA — rd = sign_extend(rt >> sa) (arithmetic)
static void recSRA_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].SL[0] >> _Sa_);
}

static void recSRA_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	if (_Sa_ != 0)
	{
		// Sbfx(sa, 32-sa) == sign_extend((s32)rt >> sa) in ONE insn (GE-06).
		armAsm->Sbfx(dst, rt.X(), _Sa_, 32 - _Sa_);
	}
	else
	{
		armAsm->Sxtw(dst, rt);
	}
	memStoreD(dst);
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, SRA, XMMINFO_WRITED | XMMINFO_READT);

//// DSLL — rd = rt << sa (64-bit)
static void recDSLL_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rt_].UD[0] << _Sa_;
}

static void recDSLL_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT64();
	if (_Sa_ != 0)
	{
		const a64::Register dst = memDestD();
		armAsm->Lsl(dst, rt, _Sa_);
		memStoreD(dst);
	}
	else
	{
		memStoreD(rt);
	}
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, DSLL, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRL
static void recDSRL_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rt_].UD[0] >> _Sa_;
}

static void recDSRL_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT64();
	if (_Sa_ != 0)
	{
		const a64::Register dst = memDestD();
		armAsm->Lsr(dst, rt, _Sa_);
		memStoreD(dst);
	}
	else
	{
		memStoreD(rt);
	}
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, DSRL, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRA
static void recDSRA_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = g_cpuConstRegs[_Rt_].SD[0] >> _Sa_;
}

static void recDSRA_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT64();
	if (_Sa_ != 0)
	{
		const a64::Register dst = memDestD();
		armAsm->Asr(dst, rt, _Sa_);
		memStoreD(dst);
	}
	else
	{
		memStoreD(rt);
	}
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, DSRA, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSLL32 — rd = rt << (sa + 32)
static void recDSLL32_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rt_].UD[0] << (_Sa_ + 32);
}

static void recDSLL32_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT64();
	const a64::Register dst = memDestD();
	armAsm->Lsl(dst, rt, _Sa_ + 32);
	memStoreD(dst);
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, DSLL32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRL32
static void recDSRL32_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rt_].UD[0] >> (_Sa_ + 32);
}

static void recDSRL32_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT64();
	const a64::Register dst = memDestD();
	armAsm->Lsr(dst, rt, _Sa_ + 32);
	memStoreD(dst);
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, DSRL32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRA32
static void recDSRA32_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = g_cpuConstRegs[_Rt_].SD[0] >> (_Sa_ + 32);
}

static void recDSRA32_(int info)
{
	if (recShiftPairLead())
		return;
	const a64::Register rt = memLoadT64();
	const a64::Register dst = memDestD();
	armAsm->Asr(dst, rt, _Sa_ + 32);
	memStoreD(dst);
}

EERECOMPILE_CODEX_MEM(eeRecompileCodeRC2_MEM, DSRA32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

/*********************************************************
 * Variable shifts — rd = rt SHIFT rs                    *
 * Uses eeRecompileCodeRC0_MEM                            *
 *********************************************************/

//// SLLV — rd = sign_extend((rt << rs[4:0]))
static void recSLLV_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] << (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSLLV_consts(int info)
{
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	u32 sa = g_cpuConstRegs[_Rs_].UL[0] & 0x1f;
	if (sa != 0)
	{
		armAsm->Lsl(RWSCRATCH, rt, sa);
		armAsm->Sxtw(dst, RWSCRATCH);
	}
	else
	{
		armAsm->Sxtw(dst, rt);
	}
	memStoreD(dst);
}

static void recSLLV_constt(int info)
{
	const a64::Register rs = memLoadS32();
	const a64::Register dst = memDestD();
	armAsm->Mov(RWSCRATCH, g_cpuConstRegs[_Rt_].UL[0]);
	armAsm->Lsl(RWSCRATCH, RWSCRATCH, rs);
	armAsm->Sxtw(dst, RWSCRATCH);
	memStoreD(dst);
}

static void recSLLV_(int info)
{
	const a64::Register rs = memLoadS32();
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	armAsm->Lsl(RWSCRATCH, rt, rs);
	armAsm->Sxtw(dst, RWSCRATCH);
	memStoreD(dst);
}

EERECOMPILE_CODERC0_MEM(SLLV, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// SRLV
static void recSRLV_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSRLV_consts(int info)
{
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	u32 sa = g_cpuConstRegs[_Rs_].UL[0] & 0x1f;
	if (sa != 0)
	{
		armAsm->Lsr(RWSCRATCH, rt, sa);
		armAsm->Sxtw(dst, RWSCRATCH);
	}
	else
	{
		armAsm->Sxtw(dst, rt);
	}
	memStoreD(dst);
}

static void recSRLV_constt(int info)
{
	const a64::Register rs = memLoadS32();
	const a64::Register dst = memDestD();
	armAsm->Mov(RWSCRATCH, g_cpuConstRegs[_Rt_].UL[0]);
	armAsm->Lsr(RWSCRATCH, RWSCRATCH, rs);
	armAsm->Sxtw(dst, RWSCRATCH);
	memStoreD(dst);
}

static void recSRLV_(int info)
{
	const a64::Register rs = memLoadS32();
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	armAsm->Lsr(RWSCRATCH, rt, rs);
	armAsm->Sxtw(dst, RWSCRATCH);
	memStoreD(dst);
}

EERECOMPILE_CODERC0_MEM(SRLV, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// SRAV
static void recSRAV_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].SL[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSRAV_consts(int info)
{
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	u32 sa = g_cpuConstRegs[_Rs_].UL[0] & 0x1f;
	if (sa != 0)
	{
		armAsm->Asr(RWSCRATCH, rt, sa);
		armAsm->Sxtw(dst, RWSCRATCH);
	}
	else
	{
		armAsm->Sxtw(dst, rt);
	}
	memStoreD(dst);
}

static void recSRAV_constt(int info)
{
	const a64::Register rs = memLoadS32();
	const a64::Register dst = memDestD();
	armAsm->Mov(RWSCRATCH, g_cpuConstRegs[_Rt_].SL[0]);
	armAsm->Asr(RWSCRATCH, RWSCRATCH, rs);
	armAsm->Sxtw(dst, RWSCRATCH);
	memStoreD(dst);
}

static void recSRAV_(int info)
{
	const a64::Register rs = memLoadS32();
	const a64::Register rt = memLoadT32();
	const a64::Register dst = memDestD();
	armAsm->Asr(RWSCRATCH, rt, rs);
	armAsm->Sxtw(dst, RWSCRATCH);
	memStoreD(dst);
}

EERECOMPILE_CODERC0_MEM(SRAV, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// DSLLV — rd = rt << rs[5:0] (64-bit)
static void recDSLLV_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rt_].UD[0] << (g_cpuConstRegs[_Rs_].UL[0] & 0x3f);
}

static void recDSLLV_consts(int info)
{
	const a64::Register rt = memLoadT64();
	u32 sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	if (sa != 0)
	{
		const a64::Register dst = memDestD();
		armAsm->Lsl(dst, rt, sa);
		memStoreD(dst);
	}
	else
	{
		memStoreD(rt);
	}
}

static void recDSLLV_constt(int info)
{
	const a64::Register rs = memLoadS64();
	const a64::Register dst = memDestD();
	armAsm->Mov(RXSCRATCH, g_cpuConstRegs[_Rt_].UD[0]);
	armAsm->Lsl(dst, RXSCRATCH, rs);
	memStoreD(dst);
}

static void recDSLLV_(int info)
{
	const a64::Register rs = memLoadS64();
	const a64::Register rt = memLoadT64();
	const a64::Register dst = memDestD();
	armAsm->Lsl(dst, rt, rs);
	memStoreD(dst);
}

EERECOMPILE_CODERC0_MEM(DSLLV, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRLV
static void recDSRLV_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rt_].UD[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x3f);
}

static void recDSRLV_consts(int info)
{
	const a64::Register rt = memLoadT64();
	u32 sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	if (sa != 0)
	{
		const a64::Register dst = memDestD();
		armAsm->Lsr(dst, rt, sa);
		memStoreD(dst);
	}
	else
	{
		memStoreD(rt);
	}
}

static void recDSRLV_constt(int info)
{
	const a64::Register rs = memLoadS64();
	const a64::Register dst = memDestD();
	armAsm->Mov(RXSCRATCH, g_cpuConstRegs[_Rt_].UD[0]);
	armAsm->Lsr(dst, RXSCRATCH, rs);
	memStoreD(dst);
}

static void recDSRLV_(int info)
{
	const a64::Register rs = memLoadS64();
	const a64::Register rt = memLoadT64();
	const a64::Register dst = memDestD();
	armAsm->Lsr(dst, rt, rs);
	memStoreD(dst);
}

EERECOMPILE_CODERC0_MEM(DSRLV, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRAV
static void recDSRAV_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = g_cpuConstRegs[_Rt_].SD[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x3f);
}

static void recDSRAV_consts(int info)
{
	const a64::Register rt = memLoadT64();
	u32 sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	if (sa != 0)
	{
		const a64::Register dst = memDestD();
		armAsm->Asr(dst, rt, sa);
		memStoreD(dst);
	}
	else
	{
		memStoreD(rt);
	}
}

static void recDSRAV_constt(int info)
{
	const a64::Register rs = memLoadS64();
	const a64::Register dst = memDestD();
	armAsm->Mov(RXSCRATCH, g_cpuConstRegs[_Rt_].SD[0]);
	armAsm->Asr(dst, RXSCRATCH, rs);
	memStoreD(dst);
}

static void recDSRAV_(int info)
{
	const a64::Register rs = memLoadS64();
	const a64::Register rt = memLoadT64();
	const a64::Register dst = memDestD();
	armAsm->Asr(dst, rt, rs);
	memStoreD(dst);
}

EERECOMPILE_CODERC0_MEM(DSRAV, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_64BITOP);


} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
