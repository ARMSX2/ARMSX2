// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 EE Multiply/Divide Instruction Codegen — memory-based
// MULT/DIV write to HI:LO registers, optionally Rd.
// ARM64 has native SMULL/UMULL and SDIV/UDIV.
// All operands via cpuRegs memory.

#include "arm64/iR5900-arm64.h"

#include <bit>

namespace a64 = vixl::aarch64;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {

namespace Interp = R5900::Interpreter::OpcodeImpl;


// Flush dirty registers without materializing pending constants.
static void flushDirtyAllocatorRegs()
{
	_flushArm64GPRregs();
	_flushNEONregs();
}

// Fetch Rs/Rt lower 32 bits for the mul/div ops. Substitution-aware
// (EE-SRA 2 WS-C6): returns the pin / MODE_READ allocator reg directly (zero
// insns) or materializes into w1/w9. Every caller sits right after
// flushDirtyAllocatorRegs (the post-flush coherence contract), and every
// consumer below is read-only on the sources — EXCEPT the DIVU remainder Msub,
// which therefore targets w10 rather than writing a source in place.
//
// ⚠️ Scratch choice: w9/w10 (non-allocatable), NOT w2/w3. A resident FCR31
// (ARM64TYPE_FPRC, GE-12) lives in the {x2-x7, x14, x15} pool ACROSS ops —
// flushDirtyAllocatorRegs writes it back but keeps residency, so a raw w2/w3
// clobber here poisons the flag a later BC1x/CFC1/DIV-flag-RMW reads (same
// class as the fpuEmitGuardedAddSub SotC bug; pinned by
// EeRecFpu.CompareSurvivesInterposed{Mult,Div}).
static a64::Register loadRs32()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		armAsm->Mov(a64::w1, g_cpuConstRegs[_Rs_].UL[0]);
		return a64::w1;
	}
	return _eeGetGPRSourceReg(a64::w1, _Rs_);
}

static a64::Register loadRt32()
{
	if (GPR_IS_CONST1(_Rt_))
	{
		armAsm->Mov(a64::w9, g_cpuConstRegs[_Rt_].UL[0]);
		return a64::w9;
	}
	return _eeGetGPRSourceReg(a64::w9, _Rt_);
}

// Write LO and HI from 64-bit result in x0 (clobbers x0)
// lo = lower 32, hi = upper 32, both sign-extended to 64 bits
static void recWritebackHILO(bool upper)
{
	armAsm->Sxtw(RXSCRATCH, a64::w0);
	armAsm->Str(RXSCRATCH, armCpuRegMem(upper ? &cpuRegs.LO.UD[1] : &cpuRegs.LO.UD[0]));

	// Asr(x0, #32) already yields the sign-extended upper half — store it directly (GE-01).
	armAsm->Asr(a64::x0, a64::x0, 32);
	armAsm->Str(a64::x0, armCpuRegMem(upper ? &cpuRegs.HI.UD[1] : &cpuRegs.HI.UD[0]));
}

// Write Rd from the sign-extended LO value recWritebackHILO leaves in
// RXSCRATCH (GE-09). Contract: every caller invokes this IMMEDIATELY after
// recWritebackHILO — the old Ldr from LO.UD[half] was a store→load
// round-trip on the value computed one instruction earlier. Same helper
// serves both pipelines (the RXSCRATCH value is the rd value either way).
// _deleteEEreg's NEON writeback emits only Str q/pin Movs — x8 untouched.
static void recWritebackRd()
{
	if (!_Rd_) return;

	_deleteEEreg(_Rd_, 0);
	GPR_DEL_CONST(_Rd_);
	_eeStoreGPRDestReg(_Rd_, RXSCRATCH);
}

// One operand a constant 0 or 2^k: the product is the other operand's low
// word, sign- or zero-extended, shifted left by k. The signed forms stop at
// k = 30, as 0x80000000 is INT_MIN. Returns false for the multiply path.
static bool recMultConstOperand(bool is_signed, bool upper)
{
	int var_gpr;
	u32 c;
	if (GPR_IS_CONST1(_Rs_))
	{
		c = g_cpuConstRegs[_Rs_].UL[0];
		var_gpr = _Rt_;
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		c = g_cpuConstRegs[_Rt_].UL[0];
		var_gpr = _Rs_;
	}
	else
		return false;

	if (c == 0)
	{
		armAsm->Str(a64::xzr, armCpuRegMem(upper ? &cpuRegs.LO.UD[1] : &cpuRegs.LO.UD[0]));
		armAsm->Str(a64::xzr, armCpuRegMem(upper ? &cpuRegs.HI.UD[1] : &cpuRegs.HI.UD[0]));
		if (_Rd_)
		{
			_deleteEEreg(_Rd_, 0);
			g_cpuConstRegs[_Rd_].SD[0] = 0;
			GPR_SET_CONST(_Rd_);
		}
		return true;
	}

	if (!std::has_single_bit(c) || (is_signed && c == 0x80000000u))
		return false;

	flushDirtyAllocatorRegs();
	const a64::Register var = _eeGetGPRSourceReg(a64::w1, var_gpr).X();
	const unsigned k = std::countr_zero(c);
	if (is_signed)
		armAsm->Sbfiz(a64::x0, var, k, 32);
	else
		armAsm->Ubfiz(a64::x0, var, k, 32);

	recWritebackHILO(upper);
	recWritebackRd();
	return true;
}

//// MULT — signed 32-bit multiply, result in HI:LO, optionally Rd
void recMULT()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		s64 result = (s64)(s32)g_cpuConstRegs[_Rs_].UL[0] * (s64)(s32)g_cpuConstRegs[_Rt_].UL[0];

		armAsm->Mov(RXSCRATCH, (s64)(s32)(u32)result);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[0]));

		armAsm->Mov(RXSCRATCH, (s64)(s32)(u32)(result >> 32));
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[0]));

		if (_Rd_)
		{
			_deleteEEreg(_Rd_, 0);
			g_cpuConstRegs[_Rd_].SD[0] = (s32)(u32)result;
			GPR_SET_CONST(_Rd_);
		}
		return;
	}

	if (recMultConstOperand(true, false))
		return;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	armAsm->Smull(a64::x0, rs32, rt32);

	recWritebackHILO(false);
	recWritebackRd();
}

//// MULTU — unsigned 32-bit multiply
void recMULTU()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = (u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0];

		armAsm->Mov(RXSCRATCH, (s64)(s32)(u32)result);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[0]));

		armAsm->Mov(RXSCRATCH, (s64)(s32)(u32)(result >> 32));
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[0]));

		if (_Rd_)
		{
			_deleteEEreg(_Rd_, 0);
			g_cpuConstRegs[_Rd_].SD[0] = (s32)(u32)result;
			GPR_SET_CONST(_Rd_);
		}
		return;
	}

	if (recMultConstOperand(false, false))
		return;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	armAsm->Umull(a64::x0, rs32, rt32);

	recWritebackHILO(false);
	recWritebackRd();
}

// DIV/DIV1 by a constant ±1 or ±2^k without an sdiv. A negative dividend is
// biased by 2^k - 1 before the shift; a negative divisor negates the quotient
// after HI is taken. INT_MIN is the k = 31 case, and INT_MIN / -1 wraps to
// LO = INT_MIN, HI = 0 as sdiv does. Returns false for any other divisor.
static bool recDivConstPow2(bool upper)
{
	if (!GPR_IS_CONST1(_Rt_))
		return false;

	const u32 d = g_cpuConstRegs[_Rt_].UL[0];
	const bool negative = static_cast<s32>(d) < 0;
	const u32 mag = negative ? 0u - d : d;
	if (!std::has_single_bit(mag))
		return false;
	const unsigned k = std::countr_zero(mag);

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::MemOperand lo = armCpuRegMem(upper ? &cpuRegs.LO.UD[1] : &cpuRegs.LO.UD[0]);
	const a64::MemOperand hi = armCpuRegMem(upper ? &cpuRegs.HI.UD[1] : &cpuRegs.HI.UD[0]);

	if (k == 0)
	{
		if (negative)
		{
			armAsm->Neg(a64::w0, rs32);
			armAsm->Sxtw(RXSCRATCH, a64::w0);
		}
		else
			armAsm->Sxtw(RXSCRATCH, rs32);
		armAsm->Str(RXSCRATCH, lo);
		armAsm->Str(a64::xzr, hi);
		return true;
	}

	armAsm->Asr(a64::w10, rs32, 31);
	armAsm->Add(a64::w0, rs32, a64::Operand(a64::w10, a64::LSR, 32 - k));
	armAsm->Asr(a64::w0, a64::w0, k);
	armAsm->Sub(a64::w10, rs32, a64::Operand(a64::w0, a64::LSL, k));
	if (negative)
		armAsm->Neg(a64::w0, a64::w0);

	armAsm->Sxtw(RXSCRATCH, a64::w0);
	armAsm->Str(RXSCRATCH, lo);
	armAsm->Sxtw(RXSCRATCH, a64::w10);
	armAsm->Str(RXSCRATCH, hi);
	return true;
}

// DIVU/DIVU1 by a constant 2^k without a udiv. For k >= 1 LO and HI have bit
// 31 clear, so they need no sxtw. Returns false for any other divisor.
static bool recDivuConstPow2(bool upper)
{
	if (!GPR_IS_CONST1(_Rt_))
		return false;

	const u32 d = g_cpuConstRegs[_Rt_].UL[0];
	if (!std::has_single_bit(d))
		return false;
	const unsigned k = std::countr_zero(d);

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::MemOperand lo = armCpuRegMem(upper ? &cpuRegs.LO.UD[1] : &cpuRegs.LO.UD[0]);
	const a64::MemOperand hi = armCpuRegMem(upper ? &cpuRegs.HI.UD[1] : &cpuRegs.HI.UD[0]);

	if (k == 0)
	{
		armAsm->Sxtw(RXSCRATCH, rs32);
		armAsm->Str(RXSCRATCH, lo);
		armAsm->Str(a64::xzr, hi);
		return true;
	}

	armAsm->Ubfx(RXSCRATCH, rs32.X(), k, 32 - k);
	armAsm->Str(RXSCRATCH, lo);
	armAsm->And(RXSCRATCH, rs32.X(), (u64{1} << k) - 1);
	armAsm->Str(RXSCRATCH, hi);
	return true;
}

// DIV, DIVU and the pipeline-1 forms by a constant divisor the shift forms
// leave, with no zero test. A zero divisor stores the divide-by-zero result,
// LO = -1 (DIV: 1 for a negative rs) and HI = rs; any other is the generic
// path's divide and msub, the divisor built in w9 for the msub. Returns false
// for a divisor that is not constant.
static bool recDivConstDivisor(bool is_signed, bool upper)
{
	if (!GPR_IS_CONST1(_Rt_))
		return false;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::MemOperand lo = armCpuRegMem(upper ? &cpuRegs.LO.UD[1] : &cpuRegs.LO.UD[0]);
	const a64::MemOperand hi = armCpuRegMem(upper ? &cpuRegs.HI.UD[1] : &cpuRegs.HI.UD[0]);

	if (g_cpuConstRegs[_Rt_].UL[0] == 0)
	{
		armAsm->Mov(RXSCRATCH, -1);
		if (is_signed)
		{
			armAsm->Cmp(rs32, 0);
			armAsm->Cneg(RXSCRATCH, RXSCRATCH, a64::lt);
		}
		armAsm->Str(RXSCRATCH, lo);
		armAsm->Sxtw(RXSCRATCH, rs32);
		armAsm->Str(RXSCRATCH, hi);
		return true;
	}

	const a64::Register rt32 = loadRt32();
	if (is_signed)
		armAsm->Sdiv(a64::w0, rs32, rt32);
	else
		armAsm->Udiv(a64::w0, rs32, rt32);
	armAsm->Msub(a64::w10, a64::w0, rt32, rs32);

	armAsm->Sxtw(RXSCRATCH, a64::w0);
	armAsm->Str(RXSCRATCH, lo);
	armAsm->Sxtw(RXSCRATCH, a64::w10);
	armAsm->Str(RXSCRATCH, hi);
	return true;
}

//// DIV — signed 32-bit divide. LO = quotient, HI = remainder.
// PS2 div-by-zero: LO = (rs >= 0 ? -1 : 1), HI = rs (sign-extended into 64-bit
// HI/LO). Matches the interpreter and the PS2 hardware spec.
void recDIV()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		s32 rs = g_cpuConstRegs[_Rs_].SL[0];
		s32 rt = g_cpuConstRegs[_Rt_].SL[0];

		s32 lo, hi;
		if (rt == 0)
		{
			lo = (rs >= 0) ? -1 : 1;
			hi = rs;
		}
		else if (rs == (s32)0x80000000 && rt == -1)
		{
			lo = (s32)0x80000000;
			hi = 0;
		}
		else
		{
			lo = rs / rt;
			hi = rs % rt;
		}

		armAsm->Mov(RXSCRATCH, (s64)lo);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[0]));

		armAsm->Mov(RXSCRATCH, (s64)hi);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[0]));
		return;
	}

	if (recDivConstPow2(false))
		return;
	if (recDivConstDivisor(true, false))
		return;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	// Branch on rt == 0 → div-by-zero handler.
	a64::Label divByZero;
	a64::Label done;
	armAsm->Cbz(rt32, &divByZero);

	// Normal path: SDIV w0 = rs/rt; MSUB w10 = rs - w0*rt (remainder).
	armAsm->Sdiv(a64::w0, rs32, rt32);
	armAsm->Msub(a64::w10, a64::w0, rt32, rs32);
	armAsm->B(&done);

	// Div-by-zero: w0 = (rs >= 0 ? -1 : 1), w10 = rs.
	// Cneg w0, w0, lt: if rs < 0, w0 = -(-1) = 1; else w0 = -1.
	armAsm->Bind(&divByZero);
	armAsm->Mov(a64::w0, -1);
	armAsm->Cmp(rs32, 0);
	armAsm->Cneg(a64::w0, a64::w0, a64::lt);
	armAsm->Mov(a64::w10, rs32);                       // HI = rs

	armAsm->Bind(&done);

	// Store LO = sign_extend(quotient or -1/1)
	armAsm->Sxtw(RXSCRATCH, a64::w0);
	armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[0]));

	// Store HI = sign_extend(remainder or rs)
	armAsm->Sxtw(RXSCRATCH, a64::w10);
	armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[0]));
}

//// DIVU — unsigned 32-bit divide. PS2 div-by-zero: LO = -1 (0xffffffff
//// sign-extended), HI = rs.
void recDIVU()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u32 rs = g_cpuConstRegs[_Rs_].UL[0];
		u32 rt = g_cpuConstRegs[_Rt_].UL[0];

		s32 lo, hi;
		if (rt == 0)
		{
			lo = -1;
			hi = (s32)rs;
		}
		else
		{
			lo = (s32)(rs / rt);
			hi = (s32)(rs % rt);
		}

		armAsm->Mov(RXSCRATCH, (s64)lo);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[0]));

		armAsm->Mov(RXSCRATCH, (s64)hi);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[0]));
		return;
	}

	if (recDivuConstPow2(false))
		return;
	if (recDivConstDivisor(false, false))
		return;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	a64::Label divByZero;
	a64::Label done;
	armAsm->Cbz(rt32, &divByZero);

	// Normal path: UDIV w0; MSUB remainder into w10 — NOT in place over the Rs
	// source, which may be a pin (WS-C6 pin-safety; the old in-place form
	// predates operand substitution).
	armAsm->Udiv(a64::w0, rs32, rt32);
	armAsm->Msub(a64::w10, a64::w0, rt32, rs32);
	armAsm->B(&done);

	// Div-by-zero: w0 = -1; HI = rs.
	armAsm->Bind(&divByZero);
	armAsm->Mov(a64::w0, -1);
	armAsm->Mov(a64::w10, rs32);

	armAsm->Bind(&done);

	// Store LO = sign_extend(quotient or -1)
	armAsm->Sxtw(RXSCRATCH, a64::w0);
	armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[0]));

	// Store HI = sign_extend(remainder or rs)
	armAsm->Sxtw(RXSCRATCH, a64::w10);
	armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[0]));
}


// Pipeline-1 Rd writeback — identical to recWritebackRd since GE-09 (the
// rd value rides RXSCRATCH out of recWritebackHILO for either pipeline).
static void recWritebackRd1()
{
	recWritebackRd();
}

//// MULT1 — signed 32-bit multiply, pipeline 1 (HI1:LO1)
void recMULT1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		s64 result = (s64)(s32)g_cpuConstRegs[_Rs_].UL[0] * (s64)(s32)g_cpuConstRegs[_Rt_].UL[0];

		armAsm->Mov(RXSCRATCH, (s64)(s32)(u32)result);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[1]));

		armAsm->Mov(RXSCRATCH, (s64)(s32)(u32)(result >> 32));
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[1]));

		if (_Rd_)
		{
			_deleteEEreg(_Rd_, 0);
			g_cpuConstRegs[_Rd_].SD[0] = (s32)(u32)result;
			GPR_SET_CONST(_Rd_);
		}
		return;
	}

	if (recMultConstOperand(true, true))
		return;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	armAsm->Smull(a64::x0, rs32, rt32);

	recWritebackHILO(true);
	recWritebackRd1();
}

//// MULTU1 — unsigned 32-bit multiply, pipeline 1
void recMULTU1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = (u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0];

		armAsm->Mov(RXSCRATCH, (s64)(s32)(u32)result);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[1]));

		armAsm->Mov(RXSCRATCH, (s64)(s32)(u32)(result >> 32));
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[1]));

		if (_Rd_)
		{
			_deleteEEreg(_Rd_, 0);
			g_cpuConstRegs[_Rd_].SD[0] = (s32)(u32)result;
			GPR_SET_CONST(_Rd_);
		}
		return;
	}

	if (recMultConstOperand(false, true))
		return;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	armAsm->Umull(a64::x0, rs32, rt32);

	recWritebackHILO(true);
	recWritebackRd1();
}

//// DIV1 — signed 32-bit divide, pipeline 1. Same div-by-zero spec as DIV.
void recDIV1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		s32 rs = g_cpuConstRegs[_Rs_].SL[0];
		s32 rt = g_cpuConstRegs[_Rt_].SL[0];

		s32 lo, hi;
		if (rt == 0)
		{
			lo = (rs >= 0) ? -1 : 1;
			hi = rs;
		}
		else if (rs == (s32)0x80000000 && rt == -1)
		{
			lo = (s32)0x80000000;
			hi = 0;
		}
		else
		{
			lo = rs / rt;
			hi = rs % rt;
		}

		armAsm->Mov(RXSCRATCH, (s64)lo);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[1]));

		armAsm->Mov(RXSCRATCH, (s64)hi);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[1]));
		return;
	}

	if (recDivConstPow2(true))
		return;
	if (recDivConstDivisor(true, true))
		return;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	a64::Label divByZero;
	a64::Label done;
	armAsm->Cbz(rt32, &divByZero);

	armAsm->Sdiv(a64::w0, rs32, rt32);
	armAsm->Msub(a64::w10, a64::w0, rt32, rs32);
	armAsm->B(&done);

	// Div-by-zero: w0 = (rs >= 0 ? -1 : 1), w10 = rs.  See recDIV for Cneg rationale.
	armAsm->Bind(&divByZero);
	armAsm->Mov(a64::w0, -1);
	armAsm->Cmp(rs32, 0);
	armAsm->Cneg(a64::w0, a64::w0, a64::lt);
	armAsm->Mov(a64::w10, rs32);                       // HI = rs

	armAsm->Bind(&done);

	armAsm->Sxtw(RXSCRATCH, a64::w0);
	armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[1]));

	armAsm->Sxtw(RXSCRATCH, a64::w10);
	armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[1]));
}

//// DIVU1 — unsigned 32-bit divide, pipeline 1. Same div-by-zero spec as DIVU.
void recDIVU1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u32 rs = g_cpuConstRegs[_Rs_].UL[0];
		u32 rt = g_cpuConstRegs[_Rt_].UL[0];

		s32 lo, hi;
		if (rt == 0)
		{
			lo = -1;
			hi = (s32)rs;
		}
		else
		{
			lo = (s32)(rs / rt);
			hi = (s32)(rs % rt);
		}

		armAsm->Mov(RXSCRATCH, (s64)lo);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[1]));

		armAsm->Mov(RXSCRATCH, (s64)hi);
		armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[1]));
		return;
	}

	if (recDivuConstPow2(true))
		return;
	if (recDivConstDivisor(false, true))
		return;

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	a64::Label divByZero;
	a64::Label done;
	armAsm->Cbz(rt32, &divByZero);

	// Remainder into w10, not in place — see recDIVU (WS-C6 pin-safety).
	armAsm->Udiv(a64::w0, rs32, rt32);
	armAsm->Msub(a64::w10, a64::w0, rt32, rs32);
	armAsm->B(&done);

	armAsm->Bind(&divByZero);
	armAsm->Mov(a64::w0, -1);
	armAsm->Mov(a64::w10, rs32);

	armAsm->Bind(&done);

	armAsm->Sxtw(RXSCRATCH, a64::w0);
	armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.LO.UD[1]));

	armAsm->Sxtw(RXSCRATCH, a64::w10);
	armAsm->Str(RXSCRATCH, armCpuRegMem(&cpuRegs.HI.UD[1]));
}

//// MADD — signed multiply-add: HI:LO += Rs * Rt, Rd = LO
void recMADD()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		s64 result = (s64)(s32)g_cpuConstRegs[_Rs_].UL[0] * (s64)(s32)g_cpuConstRegs[_Rt_].UL[0];

		// Add to existing HI:LO — load, add, store
		flushDirtyAllocatorRegs();
		armLoadEERegPtr(a64::w1, &cpuRegs.LO.UL[0]);
		armLoadEERegPtr(a64::w9, &cpuRegs.HI.UL[0]);
		armAsm->Orr(a64::x1, a64::x1, a64::Operand(a64::x9, a64::LSL, 32));
		armAsm->Mov(RXSCRATCH, result);
		armAsm->Add(a64::x0, a64::x1, RXSCRATCH);

		recWritebackHILO(false);
		recWritebackRd();
		return;
	}

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	armLoadEERegPtr(a64::w10, &cpuRegs.LO.UL[0]);
	armLoadEERegPtr(RWSCRATCH, &cpuRegs.HI.UL[0]); // w8: reserved scratch (w4 is allocatable)
	armAsm->Orr(a64::x10, a64::x10, a64::Operand(RXSCRATCH, a64::LSL, 32));
	armAsm->Smaddl(a64::x0, rs32, rt32, a64::x10);

	recWritebackHILO(false);
	recWritebackRd();
}

//// MADDU — unsigned multiply-add: HI:LO += Rs * Rt, Rd = LO
void recMADDU()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = (u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0];

		flushDirtyAllocatorRegs();
		armLoadEERegPtr(a64::w1, &cpuRegs.LO.UL[0]);
		armLoadEERegPtr(a64::w9, &cpuRegs.HI.UL[0]);
		armAsm->Orr(a64::x1, a64::x1, a64::Operand(a64::x9, a64::LSL, 32));
		armAsm->Mov(RXSCRATCH, result);
		armAsm->Add(a64::x0, a64::x1, RXSCRATCH);

		recWritebackHILO(false);
		recWritebackRd();
		return;
	}

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	armLoadEERegPtr(a64::w10, &cpuRegs.LO.UL[0]);
	armLoadEERegPtr(RWSCRATCH, &cpuRegs.HI.UL[0]); // w8: reserved scratch (w4 is allocatable)
	armAsm->Orr(a64::x10, a64::x10, a64::Operand(RXSCRATCH, a64::LSL, 32));
	armAsm->Umaddl(a64::x0, rs32, rt32, a64::x10);

	recWritebackHILO(false);
	recWritebackRd();
}

//// MADD1 — signed multiply-add, pipeline 1: HI1:LO1 += Rs * Rt, Rd = LO1
void recMADD1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		s64 result = (s64)(s32)g_cpuConstRegs[_Rs_].UL[0] * (s64)(s32)g_cpuConstRegs[_Rt_].UL[0];

		flushDirtyAllocatorRegs();
		armLoadEERegPtr(a64::w1, &cpuRegs.LO.UL[2]);
		armLoadEERegPtr(a64::w9, &cpuRegs.HI.UL[2]);
		armAsm->Orr(a64::x1, a64::x1, a64::Operand(a64::x9, a64::LSL, 32));
		armAsm->Mov(RXSCRATCH, result);
		armAsm->Add(a64::x0, a64::x1, RXSCRATCH);

		recWritebackHILO(true);
		recWritebackRd1();
		return;
	}

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	armLoadEERegPtr(a64::w10, &cpuRegs.LO.UL[2]);  // LO1 = LO.UL[2] (upper 64 bits)
	armLoadEERegPtr(RWSCRATCH, &cpuRegs.HI.UL[2]); // HI1 = HI.UL[2] (w8: reserved scratch — w4 is allocatable)
	armAsm->Orr(a64::x10, a64::x10, a64::Operand(RXSCRATCH, a64::LSL, 32));
	armAsm->Smaddl(a64::x0, rs32, rt32, a64::x10);

	recWritebackHILO(true);
	recWritebackRd1();
}

//// MADDU1 — unsigned multiply-add, pipeline 1
void recMADDU1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = (u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0];

		flushDirtyAllocatorRegs();
		armLoadEERegPtr(a64::w1, &cpuRegs.LO.UL[2]);
		armLoadEERegPtr(a64::w9, &cpuRegs.HI.UL[2]);
		armAsm->Orr(a64::x1, a64::x1, a64::Operand(a64::x9, a64::LSL, 32));
		armAsm->Mov(RXSCRATCH, result);
		armAsm->Add(a64::x0, a64::x1, RXSCRATCH);

		recWritebackHILO(true);
		recWritebackRd1();
		return;
	}

	flushDirtyAllocatorRegs();
	const a64::Register rs32 = loadRs32();
	const a64::Register rt32 = loadRt32();

	armLoadEERegPtr(a64::w10, &cpuRegs.LO.UL[2]);
	armLoadEERegPtr(RWSCRATCH, &cpuRegs.HI.UL[2]); // w8: reserved scratch (w4 is allocatable)
	armAsm->Orr(a64::x10, a64::x10, a64::Operand(RXSCRATCH, a64::LSL, 32));
	armAsm->Umaddl(a64::x0, rs32, rt32, a64::x10);

	recWritebackHILO(true);
	recWritebackRd1();
}

} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
