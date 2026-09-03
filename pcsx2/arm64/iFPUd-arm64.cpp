// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 EE FPU (COP1) — "Full" / DOUBLE-precision codegen.
//
// This is the arm64 port of pcsx2/x86/iFPUd.cpp: the PS2-accurate FPU that
// widens each single to IEEE double, performs the op in double, then narrows
// back to a PS2 single with the hardware's overflow/underflow/clamp semantics.
// It is selected when CHECK_FPU_FULL (EmuConfig.Cpu.Recompiler.fpuFullMode, the
// GameDB eeClampMode 3 and up — FFX, Max Payne, Dark Cloud 2, Klonoa 2 …).
// Default config runs the single-precision fast path in iFPU-arm64.cpp.
//
// It serves eeClampMode 3 and 4, which differ at emitDefectiveFmul and at
// emitDivideUnitIsland below.
//
// The algorithm is translated from the x86 semantics; the codegen follows the
// iFPU-arm64.cpp idioms (scalar Fcvt, GPR bit-twiddle via Fmov, the
// armLoadEERegPtr fprc[31]/ACCflag accessors). The shared interpreter
// (FPU.cpp fpuDouble) has no double path, so this codegen has no interpreter
// counterpart.

#include "arm64/iR5900-arm64.h"

#include <cfloat>

namespace a64 = vixl::aarch64;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace COP1 {
namespace DOUBLE {

#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

#define FPUflagO  0x00008000
#define FPUflagU  0x00004000
#define FPUflagSO 0x00000010
#define FPUflagSU 0x00000008
#define FPUflagI  0x00020000
#define FPUflagD  0x00010000
#define FPUflagSI 0x00000040
#define FPUflagSD 0x00000020

// ---- The guest FPR file -----------------------------------------------------
//
// The file holds each word relocated into double position and scaled by
// 2^-kEeFprScaleExp (EeFpuFormat.h); this file works in words and bridges at
// the edges. Widening is one exact multiply against the pinned scale, and
// FPCR.FZ takes an EE denormal to a zero of the same sign there. `dstidx` may
// be `srcidx`.
static void SlotToDouble(int dstidx, int srcidx)
{
	armAsm->Fmul(armDRegister(dstidx), armDRegister(srcidx),
		a64::VRegister(NEON_RESERVED_EEFPU_UNSCALE, 64));
}

// The other half of the bridge: an architectural single in an S lane as a slot.
static void SingleToSlot(int dstidx, int srcidx)
{
	armEmitEeFprFromS(armDRegister(dstidx), armSRegister(srcidx), RXSCRATCH);
}

// ---- IEEE double -> PS2 single (full overflow/underflow/flag handling) -----
//
// Port of x86 ToPS2FPU_Full. `idx` holds the double result (D lane); `absidx`
// is a scratch NEON reg. On return the PS2 single is in `idx`'s S lane.
// Comparisons are done on the integer bit pattern of |x| — valid because every
// operand here is a finite double, so unsigned-integer order == magnitude order
// (sidesteps NaN/unordered, which never reach this point for ADD/SUB/MUL).
static void ToPS2FPU_Full(int idx, bool flags, int /*absidx*/, bool acc, bool addsub)
{
	const a64::VRegister s = armSRegister(idx);
	const a64::VRegister d = armDRegister(idx);

	if (flags)
	{
		armLoadEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
		armAsm->Bic(RWSCRATCH, RWSCRATCH, FPUflagO | FPUflagU);
		armStoreEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
		if (acc)
		{
			armLoadEERegPtr(RWSCRATCH, &fpuRegs.ACCflag);
			armAsm->Bic(RWSCRATCH, RWSCRATCH, 1);
			armStoreEERegPtr(RWSCRATCH, &fpuRegs.ACCflag);
		}
	}

	// abs = |reg| (integer, low 63 bits)
	armAsm->Fmov(RXSCRATCH, d);
	armAsm->And(RXARG1, RXSCRATCH, 0x7fffffffffffffffULL);

	a64::Label toComplex, toUnderflow, toOverflow, end;

	armAsm->Mov(RXARG2, static_cast<u64>(1151) << 52);   // dbl_cvt_overflow (2^128)
	armAsm->Cmp(RXARG1, RXARG2);
	armAsm->B(&toComplex, a64::hs);

	armAsm->Mov(RXARG2, static_cast<u64>(897) << 52);    // dbl_underflow (2^-126)
	armAsm->Cmp(RXARG1, RXARG2);
	armAsm->B(&toUnderflow, a64::lo);

	// In-range: plain narrow.
	armAsm->Fcvt(s, d);
	armAsm->B(&end);

	armAsm->Bind(&toComplex);
	// Saturate above the EE MAXIMUM, not above 2^129.
	//
	// x86 iFPUd.cpp uses dbl_ps2_overflow == 2^129 here, but the largest number
	// this FPU has -- kEeFpuMax, as the comments below name it -- is 0x7FFFFFFF
	// == (2 - 2^-23) * 2^128, a whole binade below it. Everything in
	// (kEeFpuMax, 2^129) therefore fell into the halving arm below, and under
	// the divide unit's round-to-nearest FPCR that arm's +0x00800000 carried
	// out of the exponent field into the sign bit (0x7f800000 + 0x00800000 ==
	// 0x80000000): the largest magnitude the FPU can produce came back as
	// negative zero. Only RSQRT can land in the band; see
	// EeRecFpuFull.RsqrtAboveEeMaxSaturatesInsteadOfWrappingToNegativeZero for
	// why DIV and SQRT cannot.
	//
	// `hi`, not `hs`: kEeFpuMax itself is representable and belongs to the
	// halving arm, which handles it exactly (halved it is +FLT_MAX, and
	// 0x7f7fffff + 0x00800000 == 0x7fffffff).
	//
	// The test is on the rounded magnitude: the adder normalises and truncates
	// before anything looks at the exponent field, so a sum above kEeFpuMax can
	// chop back onto it and did not saturate. kEeFpuMax + 2^104 is 2^129 - 2^104,
	// which needs 25 significant bits and chops to kEeFpuMax; one exponent
	// higher the sum is 2^129 and no rounding brings it back.
	//
	// Chopping the low 29 bits is the rounding only under round-toward-zero,
	// the arithmetic FPCR. The divide unit's callers run under round-to-nearest
	// and pass flags=false, which is the same split.
	if (flags)
		armAsm->And(RXARG1, RXARG1, UINT64_C(0xFFFFFFFFE0000000));
	armAsm->Mov(RXARG2, UINT64_C(0x47FFFFFFE0000000)); // (2 - 2^-23) * 2^128
	armAsm->Cmp(RXARG1, RXARG2);
	armAsm->B(&toOverflow, a64::hi);

	// Large but PS2-representable (exp-0xff range): lower double exp, narrow,
	// raise single exp — the inverse of the widening, in the single domain.
	armAsm->Mov(RXARG2, static_cast<u64>(1) << 52);
	armAsm->Sub(RXSCRATCH, RXSCRATCH, RXARG2);
	armAsm->Fmov(d, RXSCRATCH);
	armAsm->Fcvt(s, d);
	armAsm->Fmov(RWSCRATCH, s);
	armAsm->Add(RWSCRATCH, RWSCRATCH, 0x00800000);
	armAsm->Fmov(s, RWSCRATCH);
	armAsm->B(&end);

	armAsm->Bind(&toOverflow);
	// Beyond PS2 range: narrow then clamp to +/-max (keep sign, set all other bits).
	armAsm->Fcvt(s, d);
	armAsm->Fmov(RWSCRATCH, s);
	armAsm->Orr(RWSCRATCH, RWSCRATCH, 0x7fffffff);
	armAsm->Fmov(s, RWSCRATCH);
	if (flags)
	{
		armLoadEERegPtr(RWARG1, &fpuRegs.fprc[31]);
		armAsm->Orr(RWARG1, RWARG1, FPUflagO | FPUflagSO);
		armStoreEERegPtr(RWARG1, &fpuRegs.fprc[31]);
		if (acc)
		{
			armLoadEERegPtr(RWARG1, &fpuRegs.ACCflag);
			armAsm->Orr(RWARG1, RWARG1, 1);
			armStoreEERegPtr(RWARG1, &fpuRegs.ACCflag);
		}
	}
	armAsm->B(&end);

	armAsm->Bind(&toUnderflow);
	a64::Label uDone;
	if (flags)
	{
		// Set U|SU unless the result is exactly +/-0.
		armAsm->Fmov(RXSCRATCH, d);
		armAsm->And(RXARG1, RXSCRATCH, 0x7fffffffffffffffULL);
		a64::Label isZero;
		armAsm->Cbz(RXARG1, &isZero);
		armLoadEERegPtr(RWARG2, &fpuRegs.fprc[31]);
		armAsm->Orr(RWARG2, RWARG2, FPUflagU | FPUflagSU);
		armStoreEERegPtr(RWARG2, &fpuRegs.fprc[31]);
		if (addsub)
		{
			// ADD/SUB leave the (post-normalization) mantissa bits in place;
			// reconstruct a PS2 denormal single: bits[22:0] = dbl_mant[51:29],
			// bit31 = sign, exp = 0. (x86 PSLL.Q 12 / PSRL.Q 41 / sign<<31 / POR.)
			armAsm->Fmov(RXSCRATCH, d);
			armAsm->Lsl(RXARG1, RXSCRATCH, 12);
			armAsm->Lsr(RXARG1, RXARG1, 41);
			armAsm->Lsr(RXARG2, RXSCRATCH, 63);
			armAsm->Lsl(RXARG2, RXARG2, 31);
			armAsm->Orr(RWSCRATCH, RWARG1, RWARG2);
			armAsm->Fmov(s, RWSCRATCH);
			armAsm->B(&uDone);
		}
		armAsm->Bind(&isZero);
	}
	// Flush to +/-0 (keep sign).
	armAsm->Fcvt(s, d);
	armAsm->Fmov(RWSCRATCH, s);
	armAsm->And(RWSCRATCH, RWSCRATCH, 0x80000000);
	armAsm->Fmov(s, RWSCRATCH);

	armAsm->Bind(&uDone);
	armAsm->Bind(&end);
}

// ---- IEEE double -> PS2-single value, left in double format ---------------
//
// The rounding half of ToPS2FPU_Full with the format change taken out. On
// return `idx`'s D lane holds a double whose value is exactly the single
// ToPS2FPU_Full would have produced -- low 29 mantissa bits zero, |x| <=
// kEeFpuMax, sub-2^-126 flushed to signed zero -- and the same O/U flags have
// been raised. Used where the caller is going to widen the result straight back
// (recMaddsub), so the narrow/widen round trip never happens.
//
// Rounding to a 24-bit significand is masking off the low 29 mantissa bits.
// That is valid only under round-toward-zero, which is the arithmetic FPCR
// (FPUFPCR) this path runs under. DIV/SQRT/RSQRT run under the divide unit's
// round-to-nearest FPCR, where the mask would be plain truncation -- they keep
// ToPS2FPU_Full.
//
// The "large but PS2-representable" arm of ToPS2FPU_Full disappears entirely:
// an exponent-0xff PS2 single is an ordinary double, so in the wide domain
// there is nothing to halve, narrow and re-raise -- it is just a chop like any
// other in-range value. Only the saturation bound still needs the finer test.
//
// addsub is not a parameter: the one caller is the multiply stage, which passes
// addsub=false, so the underflow arm never reconstructs a denormal.
static void ToPS2FPU_Wide(int idx)
{
	const a64::VRegister d = armDRegister(idx);

	armLoadEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
	armAsm->Bic(RWSCRATCH, RWSCRATCH, FPUflagO | FPUflagU);
	armStoreEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);

	armAsm->Fmov(RXSCRATCH, d);
	armAsm->And(RXARG1, RXSCRATCH, 0x7fffffffffffffffULL);

	a64::Label chop, toComplex, toUnderflow, isZero, end;

	// Both bounds below are single MOVZ; the exact kEeFpuMax pattern is not, so
	// keep it off the common path and test 2^128 first (as ToPS2FPU_Full does).
	armAsm->Mov(RXARG2, static_cast<u64>(1151) << 52);   // 2^128
	armAsm->Cmp(RXARG1, RXARG2);
	armAsm->B(&toComplex, a64::hs);

	armAsm->Mov(RXARG2, static_cast<u64>(897) << 52);    // 2^-126
	armAsm->Cmp(RXARG1, RXARG2);
	armAsm->B(&toUnderflow, a64::lo);

	armAsm->Bind(&chop);
	armAsm->And(RXSCRATCH, RXSCRATCH, UINT64_C(0xffffffffe0000000));
	armAsm->Fmov(d, RXSCRATCH);
	armAsm->B(&end);

	armAsm->Bind(&toComplex);
	// Rounded magnitude, as in ToPS2FPU_Full: a product that chops back onto
	// kEeFpuMax did not saturate and must not raise O.
	armAsm->And(RXARG1, RXARG1, UINT64_C(0xFFFFFFFFE0000000));
	armAsm->Mov(RXARG2, UINT64_C(0x47FFFFFFE0000000)); // (2 - 2^-23) * 2^128
	armAsm->Cmp(RXARG1, RXARG2);
	armAsm->B(&chop, a64::ls);   // in [2^128, kEeFpuMax]: an ordinary chop

	// Beyond PS2 range: keep the sign, set the magnitude to kEeFpuMax (still in
	// RXARG2). The single-domain form of this is `Orr 0x7fffffff`.
	armAsm->And(RXSCRATCH, RXSCRATCH, UINT64_C(0x8000000000000000));
	armAsm->Orr(RXSCRATCH, RXSCRATCH, RXARG2);
	armAsm->Fmov(d, RXSCRATCH);
	armLoadEERegPtr(RWARG1, &fpuRegs.fprc[31]);
	armAsm->Orr(RWARG1, RWARG1, FPUflagO | FPUflagSO);
	armStoreEERegPtr(RWARG1, &fpuRegs.fprc[31]);
	armAsm->B(&end);

	armAsm->Bind(&toUnderflow);
	// RXSCRATCH/RXARG1 still hold the bits and |bits| from entry.
	armAsm->Cbz(RXARG1, &isZero);
	armLoadEERegPtr(RWARG2, &fpuRegs.fprc[31]);
	armAsm->Orr(RWARG2, RWARG2, FPUflagU | FPUflagSU);
	armStoreEERegPtr(RWARG2, &fpuRegs.fprc[31]);
	armAsm->Bind(&isZero);
	armAsm->And(RXSCRATCH, RXSCRATCH, UINT64_C(0x8000000000000000));
	armAsm->Fmov(d, RXSCRATCH);

	armAsm->Bind(&end);
}

// ---- PS2 add/sub guard-bit emulation --------------------------------------
//
// The EE FPU has no guard bits to the right of the mantissa; subtraction (and
// add of mixed signs) can shift the mantissa left and expose what would have
// been guard bits. This masks the low mantissa bits of the smaller operand by
// the exponent difference so they read as zero. Port of x86 FPU_ADD_SUB
// (pcsx2/x86/iFPUd.cpp), which states the law over the architectural single and
// runs before the widening.
//
// Here both operands are already doubles holding EE singles exactly (low 29
// mantissa bits zero, |x| <= kEeFpuMax) in temp NEON regs `idxd`/`idxt`, and
// are mutated in place. Two changes from the single-domain form:
//
//  * The exponent field is bits 52..62 instead of 23..30, and the bias is 896
//    higher. The bias cancels in the difference, so the case split is unchanged
//    for two normals. It does not cancel when exactly one operand is zero
//    (single e-0=e, double (e+896)-0), which moves such a pair from the
//    mask-low-bits arm into the sign-only arm -- but the operand those arms
//    touch is the zero one, and +/-0 is invariant under both (masking low bits
//    of a zero, or reducing it to its sign, both leave it alone), so the two
//    domains still agree. Verified: 0 disagreements over 1,572,864 pairs
//    covering every (expd, expt) combination, 12,240 of them in exactly that
//    class, against an off-by-one liveness control that moves 5,588 of 65,025.
//    (A PS2 denormal cannot reach here: SlotToDouble runs under FZ, which
//    flushes it to a zero of the same sign.)
//  * A single's mantissa bit k is double bit k+29, so masking the single's low
//    (diff-1) bits is masking the double's low (diff-1)+29. The extra 29 are
//    already zero, so only the shift amount changes: `diff - 1` -> `diff + 28`.
static void FPU_ADD_SUB_D(int idxd, int idxt)
{
	const a64::VRegister dd = armDRegister(idxd);
	const a64::VRegister dt = armDRegister(idxt);

	armAsm->Fmov(RXARG1, dd);  // d bits
	armAsm->Fmov(RXARG2, dt);  // t bits
	// GE-M2: the exponent-diff and mask temps use the reserved load/store scratch
	// x9/x10, not the RWARG3/RWARG4 (w2/w3) pool hosts they replaced — w2/w3 are
	// EE-allocatable, so under the residency flip they can hold a live guest GPR,
	// and this hand-emitted path never flushes the allocator. This span has no
	// load/store or C-call, so x9/x10 are free scratch here. (x86 uses GPR temps
	// too; only the register choice is our scratch-discipline constraint.)
	armAsm->Ubfx(a64::x9, RXARG1, 52, 11);    // expd
	armAsm->Ubfx(RXSCRATCH, RXARG2, 52, 11);  // expt
	armAsm->Sub(a64::w9, a64::w9, RWSCRATCH); // diff = expd - expt (signed)

	a64::Label caseD25, casePos, caseEq, caseDn25, done;
	armAsm->Cmp(a64::w9, 25);
	armAsm->B(&caseD25, a64::ge);
	armAsm->Cmp(a64::w9, 0);
	armAsm->B(&casePos, a64::gt);
	armAsm->B(&caseEq, a64::eq);
	armAsm->Cmn(a64::w9, 25);                 // cmp diff, -25
	armAsm->B(&caseDn25, a64::le);

	// diff in -24..-1 (expd < expt): mask tempd's low (-diff-1)+29 bits.
	armAsm->Neg(RWSCRATCH, a64::w9);
	armAsm->Add(RWSCRATCH, RWSCRATCH, 28);
	armAsm->Mov(a64::x10, UINT64_C(0xffffffffffffffff));
	armAsm->Lsl(a64::x10, a64::x10, RXSCRATCH);
	armAsm->And(RXARG1, RXARG1, a64::x10);
	armAsm->Fmov(dd, RXARG1);
	armAsm->B(&done);

	armAsm->Bind(&caseD25);
	// diff >= 25 (expt much smaller): tempt keeps only its sign.
	armAsm->And(RXARG2, RXARG2, UINT64_C(0x8000000000000000));
	armAsm->Fmov(dt, RXARG2);
	armAsm->B(&done);

	armAsm->Bind(&casePos);
	// diff in 1..24 (expt smaller): mask tempt's low (diff-1)+29 bits.
	armAsm->Add(RWSCRATCH, a64::w9, 28);
	armAsm->Mov(a64::x10, UINT64_C(0xffffffffffffffff));
	armAsm->Lsl(a64::x10, a64::x10, RXSCRATCH);
	armAsm->And(RXARG2, RXARG2, a64::x10);
	armAsm->Fmov(dt, RXARG2);
	armAsm->B(&done);

	armAsm->Bind(&caseDn25);
	// diff <= -25 (expd much smaller): tempd keeps only its sign.
	armAsm->And(RXARG1, RXARG1, UINT64_C(0x8000000000000000));
	armAsm->Fmov(dd, RXARG1);

	armAsm->Bind(&caseEq);  // diff == 0: nothing
	armAsm->Bind(&done);
}

// ---- Op cores --------------------------------------------------------------

// ADD/SUB/ADDA/SUBA: widen both slots -> guard mask -> op in double -> narrow.
static void recFPUOp(int info, int eeRecDst, int op /*0=add,1=sub*/, bool acc)
{
	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();

	SlotToDouble(sreg, EEREC_S);
	SlotToDouble(treg, EEREC_T);
	FPU_ADD_SUB_D(sreg, treg);

	if (op == 0)
		armAsm->Fadd(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));
	else
		armAsm->Fsub(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));

	ToPS2FPU_Full(sreg, true, treg, acc, true);
	SingleToSlot(eeRecDst, sreg);

	_freeNEONreg(sreg);
	_freeNEONreg(treg);
}

// ---- Out-of-line calls into the interpreter's models -----------------------
//
// The multiply array's one-ULP deficit and the divide/square-root digit
// recurrence are not host arithmetic under any rounding mode, so mode 4 calls
// the models FPU.cpp states. The callees are plain AAPCS: every caller-saved
// home the allocator is using is spilled across the call, and the EE pin
// mirrors go through their flush/reload pair. They are pure arithmetic on their
// arguments, so unlike the vtlb slow paths they need no pc/code flush and no
// cycle spill. x8 carries the result back out, being neither allocatable nor a
// pin.
struct IslandFrame
{
	u8 gprs[8];
	u8 fprs[NUM_ARM_NEON_REGS];
	u32 ngpr, nfpr, frame, spare;
};

// `spare` bytes above the saved registers, addressed through IslandSpare, for
// an island that has to carry a value across a call of its own.
static void emitIslandEnter(IslandFrame& f, u32 spare = 0)
{
	f.ngpr = 0;
	f.nfpr = 0;
	for (int i = 0; i < NUM_ARM_GPR_REGS; i++)
	{
		// Leaves x4-x7 and x14/x15, the caller-saved half of the EE pool. x0-x3
		// and x8-x10 are scratch, x11-x13 are pins flushed below, x16+ are
		// reserved or callee-saved.
		if (i >= 16 || (i >= 8 && i <= 13) || i <= 3)
			continue;
		if (arm64gprs[i].inuse)
			f.gprs[f.ngpr++] = static_cast<u8>(i);
	}
	for (int i = 0; i < NUM_ARM_NEON_REGS; i++)
	{
		// AAPCS64 preserves only the low 64 bits of q8-q15, and the allocator
		// keeps 128-bit classes there, so every live one is saved in full.
		if (arm64neon[i].inuse)
			f.fprs[f.nfpr++] = static_cast<u8>(i);
	}

	f.spare = spare;
	f.frame = (f.ngpr * 8 + f.nfpr * 16 + spare + 15u) & ~15u;
	if (f.frame)
		armAsm->Sub(a64::sp, a64::sp, f.frame);
	u32 off = 0;
	for (u32 i = 0; i < f.ngpr; i++, off += 8)
		armAsm->Str(a64::XRegister(f.gprs[i]), a64::MemOperand(a64::sp, off));
	for (u32 i = 0; i < f.nfpr; i++, off += 16)
		armAsm->Str(a64::QRegister(f.fprs[i]), a64::MemOperand(a64::sp, off));
	// Flush before, reload after: the pin mirrors are lazily dirty, so a reload
	// on its own would lose the writes the block has made to them. Both halves
	// address RSTATE, so neither disturbs the argument or result registers.
	armFlushEEClobberedPins();
}

static a64::MemOperand IslandSpare(const IslandFrame& f)
{
	return a64::MemOperand(a64::sp, f.ngpr * 8 + f.nfpr * 16);
}

static void emitIslandLeave(const IslandFrame& f)
{
	u32 off = 0;
	for (u32 i = 0; i < f.ngpr; i++, off += 8)
		armAsm->Ldr(a64::XRegister(f.gprs[i]), a64::MemOperand(a64::sp, off));
	for (u32 i = 0; i < f.nfpr; i++, off += 16)
		armAsm->Ldr(a64::QRegister(f.fprs[i]), a64::MemOperand(a64::sp, off));
	if (f.frame)
		armAsm->Add(a64::sp, a64::sp, f.frame);
	armReloadEEClobberedPins();
}

// ---- The EE multiplier's one-ULP deficit -----------------------------------
//
// The console's multiply array does not round correctly: it comes back exactly
// one step closer to zero on a large fraction of operands, and which operands
// depends on the operand order. `mul.s` is one ULP low iff both:
//
//   1. the exact product has nothing below the single's ULP to absorb the
//      deficit -- the deficit is at most ~27308 against an ULP of 2^23, so a
//      non-zero tail hides it; and
//   2. ft's mantissa fires the Booth predicate below. fs does not enter it at
//      all, which is exactly why the operation is not commutative:
//      mul.s(1.0, x) is one ULP low for 8257536 of the 2^23 significands while
//      mul.s(x, 1.0) is exact for all of them.
//
// The interpreter models a superset (FPU.cpp eeMulRound / eeMulOneUlpLow /
// eeMulArray): it reconstructs the array's truncated low half, so it also
// catches the rows where the tail is non-zero but smaller than the borrow.
// This is the double tier's codegen for the zero-tail law. FpuMulHack is a
// one-point sample of the same rule and this subsumes it, asymmetry included.
//
// The product is computed in double, where a 24x24 significand multiply is
// exact, so neither condition needs an integer multiply: the tail is the 29
// bits below the single's ULP, and the predicate is a function of ft alone.
//
// eeClampMode 3 emits the Booth term alone; 4 adds the boundary term and the
// array call below.
//
// ft is read out of the allocator-resident guest register, which holds the word
// relocated into double position: the single's mantissa bit k is bit k+29
// there. The predicate has two terms:
//
//   * `mant & 0x2AA` -- bits 1,3,5,7,9, the sign bits of the five lowest
//     radix-4 Booth digits, at slot bits 30-38. 0x2AA << 29 is not an aarch64
//     logical immediate and neither is the pair of masks' intersection, but
//     0x5555555555555555 and 0x7fc0000000 both are, so two Ands do it.
//   * a boundary term at the truncation column,
//     `bit11 != (8 <= (mant >> 12 & 0xF) <= 13)`. The right-hand side is
//     `b15 & ~(b14 & b13)`, so the term is three shifted-register ops landing
//     on slot bit 44 and a mask to isolate it.
//
// The decrement is a whole EE ULP: a zero-tail product has its low 29 bits
// clear, so subtracting 1 << 29 lands on another exactly-representable single
// that no narrowing can round back.
//
// A zero product is excluded by its exponent field. Under FZ a zero or denormal
// operand widens to +/-0 and the product is exactly +/-0, whose pattern would
// decrement to a NaN. That covers both of the interpreter's guards, since a
// product is exactly +/-0 only when an operand was zero or denormal -- the
// smallest product of two EE normals is ~2^-252, an ordinary double.
//
// The interpreter's two remaining guards need no codegen. A saturating result
// is unreachable-by-one-ULP: products are multiples of 2^81 at that exponent
// while a double ULP there is 2^76, so no decrement can walk a product from
// above kEeFpuMax down to it, and a product landing exactly on kEeFpuMax is
// decremented by the interpreter too. "A decrement would leave the normals"
// (w == 0x00800000) needs ma*mb == 2^46 with both in [2^23, 2^24), forcing
// ma == mb == 2^23 -- ft mantissa 0, predicate off.
//
// A tail below the array's 2^15 borrow goes out of line to eeMulOneUlpLow,
// which reconstructs the truncated columns. The guard is one-directional: bits
// 28..21 of the product pattern are clear on every row in the band and on some
// rows outside it, and eeMulOneUlpLow re-tests the tail itself, so a false
// entry costs a call and returns false. A tighter mask spelled on the tail
// alone would miss rows.
//
static void emitMulArrayIsland(const a64::VRegister& prod, int fsslotidx, int ftslotidx)
{
	IslandFrame f;
	emitIslandEnter(f);

	// The stub takes the architectural words; the slots hold them relocated.
	armEmitEeFprNarrow(RXARG1, armDRegister(fsslotidx), RXSCRATCH);
	armEmitEeFprNarrow(RXARG2, armDRegister(ftslotidx), RXSCRATCH);
	armEmitCall(reinterpret_cast<const void*>(
		&R5900::Interpreter::OpcodeImpl::COP1::eeMulOneUlpLow));
	// AAPCS64 leaves everything above a bool return's one byte unspecified.
	armAsm->And(RXSCRATCH, RXARG1, 1);

	emitIslandLeave(f);

	armAsm->Fmov(RXARG2, prod);
	armAsm->Sub(RXARG2, RXARG2, a64::Operand(RXSCRATCH, a64::LSL, 29));
	armAsm->Fmov(prod, RXARG2);
}

// `dstidx` holds the widened fs on entry and the product on exit, `tidx` holds
// the widened ft, `fsslotidx` and `ftslotidx` are the untouched guest operands.
// x0/x1/x8 are the scratch this file uses everywhere, ToPS2FPU_Wide included.
static void emitDefectiveFmul(int dstidx, int tidx, int fsslotidx, int ftslotidx)
{
	const a64::VRegister prod = armDRegister(dstidx);

	// Hoisted above the Fmul: the predicate is not on its dependency chain.
	armAsm->Fmov(RXSCRATCH, armDRegister(ftslotidx));
	if (CHECK_FPU_EXACT)
	{
		armAsm->And(RXARG1, RXSCRATCH, a64::Operand(RXSCRATCH, a64::LSL, 1)); // bit43 = b14 & b13
		armAsm->Bic(RXARG1, RXSCRATCH, a64::Operand(RXARG1, a64::LSL, 1));    // bit44 = b15 & ~(b14 & b13)
		armAsm->Eor(RXARG1, RXARG1, a64::Operand(RXSCRATCH, a64::LSL, 4));    // bit44 ^= b11
		armAsm->And(RXARG1, RXARG1, UINT64_C(0x100000000000));
		armAsm->And(RXSCRATCH, RXSCRATCH, UINT64_C(0x5555555555555555));
		armAsm->And(RXSCRATCH, RXSCRATCH, UINT64_C(0x7fc0000000));
		armAsm->Orr(RXARG1, RXARG1, RXSCRATCH);
	}
	else
	{
		armAsm->And(RXARG1, RXSCRATCH, UINT64_C(0x5555555555555555));
		armAsm->And(RXARG1, RXARG1, UINT64_C(0x7fc0000000));
	}

	armAsm->Fmul(prod, prod, armDRegister(tidx));

	// One flag chain: the predicate fired, the tail is empty, the product is not
	// zero. Each stage's false arm sets the flags so the next condition cannot
	// hold, leaving the final ne false.
	armAsm->Fmov(RXARG2, prod);
	armAsm->And(RXSCRATCH, RXARG2, UINT64_C(0x1fffffff));
	armAsm->Cmp(RXARG1, 0);
	armAsm->Ccmp(RXSCRATCH, 0, a64::NoFlag, a64::ne);
	armAsm->And(RXSCRATCH, RXARG2, UINT64_C(0x7ff0000000000000));
	armAsm->Ccmp(RXSCRATCH, 0, a64::ZFlag, a64::eq);
	armAsm->Mov(RXARG1, UINT64_C(1) << 29);
	armAsm->Csel(RXARG1, RXARG1, a64::xzr, a64::ne);
	armAsm->Sub(RXARG2, RXARG2, RXARG1);
	armAsm->Fmov(prod, RXARG2);

	if (!CHECK_FPU_EXACT)
		return;

	// The rest of the law is the array's. The decrement above cannot have
	// changed the tail read here: it only fires on a zero tail, and 1 << 29
	// leaves the low 29 bits alone.
	a64::Label done;
	armAsm->And(RXSCRATCH, RXARG2, UINT64_C(0x1fffffff));
	armAsm->Cbz(RXSCRATCH, &done);
	armAsm->Tst(RXSCRATCH, UINT64_C(0x1fe00000));
	armAsm->B(&done, a64::ne);
	emitMulArrayIsland(prod, fsslotidx, ftslotidx);
	armAsm->Bind(&done);
}

// MUL/MULA: widen -> multiply in double (with the multiplier deficit) -> narrow.
static void recMULop(int info, int eeRecDst, bool acc)
{
	// Both temps before any emit: _allocTempNEONreg can evict, and an eviction's
	// writeback must not land between an operand's copy and its use.
	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();

	SlotToDouble(sreg, EEREC_S);
	SlotToDouble(treg, EEREC_T);
	emitDefectiveFmul(sreg, treg, EEREC_S, EEREC_T);

	ToPS2FPU_Full(sreg, true, treg, acc, false);
	SingleToSlot(eeRecDst, sreg);

	_freeNEONreg(sreg);
	_freeNEONreg(treg);
}

// MADD/MSUB/MADDA/MSUBA: (Fd or ACC) = ACC +/- Fs*Ft, with two PS2-accurate
// roundings (the multiply, then the accumulate) and overflow propagation from
// BOTH the product and the prior ACC. Port of x86 recMaddsub.
//
// The control flow mirrors x86: do the full-mode multiply (which may raise O),
// guard-mask ACC against the product, then branch on whether the product
// overflowed (FPUflagO) or the incoming ACC was already saturated (ACCflag&1).
// If either did, the accumulate is dominated by a 2^128-class term and the
// result is just +/-max with the dominant sign — skip the double add entirely.
// Only when both are finite is the accumulation performed in double.
//
// Everything between the two roundings stays wide. The invariant from the
// multiply stage to the final ToPS2FPU_Full is that the double is exactly a PS2
// single — low 29 mantissa bits zero, |x| <= kEeFpuMax, no denormals — which
// the guard mask preserves and which makes the accumulate exact: two 24-bit
// significands at an exponent distance of at most 24 sum in 48 bits, inside a
// double's 53. The accovf arm leaves the wide domain early, kEeFpuMax having no
// single a narrowing could reach.
static void recMaddsub(int info, int eeRecDst, int op /*0=add,1=sub*/, bool acc)
{
	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();

	// --- multiply stage: sreg = ToPS2FPU(widen(s) * widen(t)). Sets O on
	//     product overflow; never touches ACCflag here. ---
	//
	// The product is rounded but not narrowed: ToPS2FPU_Wide leaves it as a
	// double holding an exact PS2 single. Everything downstream of it in this
	// emitter -- the guard mask, the SUB sign flip, the accumulate -- wants the
	// wide form back, and narrowing here only to re-widen 13 instructions later
	// was the round trip this shape exists to remove.
	SlotToDouble(sreg, EEREC_S);
	SlotToDouble(treg, EEREC_T);
	emitDefectiveFmul(sreg, treg, EEREC_S, EEREC_T);
	ToPS2FPU_Wide(sreg);

	// --- widen the (allocator-resident) ACC slot straight into treg, then
	//     guard-mask it against the product in the wide domain. ---
	SlotToDouble(treg, EEREC_ACC);
	FPU_ADD_SUB_D(treg, sreg);

	a64::Label mulovf, accovf, operation, skipall;

	// product overflowed? -> mulovf
	armLoadEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
	armAsm->Tst(RWSCRATCH, FPUflagO);
	armAsm->B(&mulovf, a64::ne);

	// prior ACC saturated? -> accovf
	armLoadEERegPtr(RWSCRATCH, &fpuRegs.ACCflag);
	armAsm->Tst(RWSCRATCH, 1);
	armAsm->B(&accovf, a64::ne);
	armAsm->B(&operation);

	armAsm->Bind(&mulovf);
	// Product saturated at +/-kEeFpuMax; for SUB negate its sign, then it
	// becomes the accumulate result. Falls through into accovf.
	if (op == 1)
	{
		armAsm->Fmov(RXSCRATCH, armDRegister(sreg));
		armAsm->Eor(RXSCRATCH, RXSCRATCH, UINT64_C(0x8000000000000000));
		armAsm->Fmov(armDRegister(sreg), RXSCRATCH);
	}
	armAsm->Fmov(armDRegister(treg), armDRegister(sreg));

	armAsm->Bind(&accovf);
	// SetMaxValue(treg): keep sign, set all lower bits -> +/-PS2 max. This arm
	// leaves the wide domain for good -- kEeFpuMax has no single encoding a
	// narrowing could reach (Fcvt would give +/-FLT_MAX), so build the result
	// single directly from the double's sign, which is bit 31 of its high half.
	armAsm->Fmov(RXSCRATCH, armDRegister(treg));
	armAsm->Lsr(RXSCRATCH, RXSCRATCH, 32);
	armAsm->Orr(RWSCRATCH, RWSCRATCH, 0x7fffffff);
	armAsm->Fmov(armSRegister(treg), RWSCRATCH);
	// Clear O|U then raise O|SO (and ACCflag for the *A variants).
	armLoadEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
	armAsm->Bic(RWSCRATCH, RWSCRATCH, FPUflagO | FPUflagU);
	armAsm->Orr(RWSCRATCH, RWSCRATCH, FPUflagO | FPUflagSO);
	armStoreEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
	if (acc)
	{
		armLoadEERegPtr(RWSCRATCH, &fpuRegs.ACCflag);
		armAsm->Orr(RWSCRATCH, RWSCRATCH, 1);
		armStoreEERegPtr(RWSCRATCH, &fpuRegs.ACCflag);
	}
	armAsm->B(&skipall);

	armAsm->Bind(&operation);
	// Both finite: accumulate in double, narrow with flags.
	if (op == 1)
		armAsm->Fsub(armDRegister(treg), armDRegister(treg), armDRegister(sreg));
	else
		armAsm->Fadd(armDRegister(treg), armDRegister(treg), armDRegister(sreg));
	ToPS2FPU_Full(treg, true, sreg, acc, true);

	armAsm->Bind(&skipall);
	SingleToSlot(eeRecDst, treg);

	_freeNEONreg(sreg);
	_freeNEONreg(treg);
}

// ---- Per-opcode DOUBLE emitters (called by the CHECK_FPU_FULL branch in
//      iFPU-arm64.cpp via eeFPURecompileCode) -------------------------------

void recADD_S_xmm(int info)  { recFPUOp(info, EEREC_D,   0, false); }
void recSUB_S_xmm(int info)  { recFPUOp(info, EEREC_D,   1, false); }
void recADDA_S_xmm(int info) { recFPUOp(info, EEREC_ACC, 0, true);  }
void recSUBA_S_xmm(int info) { recFPUOp(info, EEREC_ACC, 1, true);  }
void recMUL_S_xmm(int info)  { recMULop(info, EEREC_D,   false); }
void recMULA_S_xmm(int info) { recMULop(info, EEREC_ACC, true);  }
void recMADD_S_xmm(int info)  { recMaddsub(info, EEREC_D,   0, false); }
void recMSUB_S_xmm(int info)  { recMaddsub(info, EEREC_D,   1, false); }
void recMADDA_S_xmm(int info) { recMaddsub(info, EEREC_ACC, 0, true);  }
void recMSUBA_S_xmm(int info) { recMaddsub(info, EEREC_ACC, 1, true);  }

// ---- GE-20: the non-arith DOUBLE bodies (x86 iFPUd.cpp ports) --------------

// x86 CLEAR_OU_FLAGS. Memory RMW is coherent with the GE-12 FCR31 residency
// because fpuTryAllocFCR31 refuses to allocate under CHECK_FPU_FULL — in FULL
// mode fprc[31] memory is the only home.
static void ClearOUFlags()
{
	armLoadEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
	armAsm->Bic(RWSCRATCH, RWSCRATCH, FPUflagO | FPUflagU);
	armStoreEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
}

// ABS/NEG: raw sign-bit ops — NO clamp (a pseudo-inf stays a pseudo-inf) —
// plus the O/U clear. ARM FABS/FNEG are non-arithmetic bit operations (no
// exceptions, NaN patterns pass through with only the sign changed), so they
// match x86's AND/XOR-with-mask exactly.
void recABS_S_xmm(int info)
{
	ClearOUFlags();
	armAsm->Fabs(armDRegister(EEREC_D), armDRegister(EEREC_S));
}

void recNEG_S_xmm(int info)
{
	ClearOUFlags();
	armAsm->Fneg(armDRegister(EEREC_D), armDRegister(EEREC_S));
}

// MAX/MIN: PS2 semantics on ALL values (incl. denormals — no FTZ, no clamp).
// Order the two words by (sign, magnitude) and write the winner's word through
// unchanged (iFPU-arm64.cpp recMINMAX derives the ordering key).
//
// The relocation is order-preserving — sign to 63, magnitude to 59..29, 62..60
// left clear — so the key is the same expression a register width up and Csel
// picks between untouched slots.
//
// Same GPR scratch contract as FPU_ADD_SUB_D: x0/x1/x8 and the non-allocatable
// x9. ClearOUFlags() runs first, so any eviction it emits lands before the raw
// scratch goes live.
static void recMINMAX(int info, bool ismin)
{
	ClearOUFlags();

	const a64::Register sbits = RXARG1, tbits = RXARG2;
	const a64::Register skey = RXSCRATCH, tkey = a64::x9;

	armAsm->Fmov(sbits, armDRegister(EEREC_S));
	armAsm->Fmov(tbits, armDRegister(EEREC_T));
	armAsm->Asr(skey, sbits, 63);
	armAsm->Eor(skey, sbits, a64::Operand(skey, a64::LSR, 1));
	armAsm->Asr(tkey, tbits, 63);
	armAsm->Eor(tkey, tbits, a64::Operand(tkey, a64::LSR, 1));
	armAsm->Cmp(skey, tkey);
	// Equal keys mean identical slots, so either arm is correct there.
	armAsm->Csel(sbits, sbits, tbits, ismin ? a64::le : a64::ge);
	armAsm->Fmov(armDRegister(EEREC_D), sbits);
}

void recMAX_S_xmm(int info) { recMINMAX(info, false); }
void recMIN_S_xmm(int info) { recMINMAX(info, true); }

// C.cond: widen both operands with SlotToDouble and compare as doubles — a PS2
// pseudo-inf compares as the finite 2^128-scale number it is, with no operand
// clamping (x86 recCMP + recC_*_xmm). The widening never yields NaN, so the
// compare is always ordered and the lt/le/eq condition reads are exact.
static void recCMP(int info)
{
	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();
	SlotToDouble(sreg, EEREC_S);
	SlotToDouble(treg, EEREC_T);
	armAsm->Fcmp(armDRegister(sreg), armDRegister(treg));
	_freeNEONreg(sreg);
	_freeNEONreg(treg);
}

static void recCcond(int info, a64::Condition cond)
{
	recCMP(info);
	// NZCV is live from the Fcmp: _freeNEONreg emits at most plain stores and
	// the fprc load below is a plain Ldr — neither touches the flags.
	armLoadEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
	armAsm->Cset(RWARG1, cond);
	armAsm->Bfi(RWSCRATCH, RWARG1, 23, 1); // FPUflagC = bit 23
	armStoreEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
}

void recC_EQ_xmm(int info) { recCcond(info, a64::eq); }
void recC_LT_xmm(int info) { recCcond(info, a64::lt); }
void recC_LE_xmm(int info) { recCcond(info, a64::le); }

// ---- DIV / SQRT / RSQRT ----------------------------------------------------

// GE-13's immediate-FPCR idiom (local copy of iFPU-arm64.cpp emitLoadFPCR —
// the value is bake-safe: a CPU-config change resets the recompilers).
static void emitLoadFPCRImm(u64 bitmask)
{
	armAsm->Mov(a64::x9, bitmask);
	armAsm->Msr(a64::FPCR, a64::x9);
}

// Plain memory RMWs on fprc[31] (FULL mode ⇒ never GPR-resident, see
// ClearOUFlags). No allocator calls — safe inside conditional emit arms.
static void SetFprcOr(u32 bits)
{
	armLoadEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
	armAsm->Orr(RWSCRATCH, RWSCRATCH, bits);
	armStoreEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
}

static void ClearIDFlags()
{
	armLoadEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
	armAsm->Bic(RWSCRATCH, RWSCRATCH, FPUflagI | FPUflagD);
	armStoreEERegPtr(RWSCRATCH, &fpuRegs.fprc[31]);
}

// x86 SetMaxValue: keep the sign bit, force every magnitude bit set.
//
// The constant is 0x7fffffff, NOT the 0x7f7fffff (+FLT_MAX) that the
// single-precision bodies use. x86 iFPUd.cpp SetMaxValue() reads:
//
//     if (FPU_RESULT)                                  // #define FPU_RESULT 1
//         xOR.PS(regd, s_const.pos[0]);                // 0x7fffffff  <- live
//     else { xAND.PS(regd, s_const.neg[0]);            //             (dead)
//            xOR.PS(regd, g_maxvals[0]); }             // 0x7f7fffff
//
// so only the first arm is ever emitted; the else-arm is dead code. ToPS2FPU's
// overflow clamp (above) uses the same 0x7fffffff, which is why this file is
// otherwise consistent. The result carries exponent field 0xff — on the EE
// that is an ordinary large finite float (the EE has no NaN/Inf), but guest
// softfloat routines do classify exp==0xff separately, so the one-ULP-band
// difference from +FLT_MAX is game-visible. kEeFprMaxBits is that word's slot,
// so this is the same two masks a register width up.
static void SetMaxValueSlot(int dstidx, int srcidx)
{
	armAsm->Fmov(RXSCRATCH, armDRegister(srcidx));
	armAsm->And(RXSCRATCH, RXSCRATCH, kEeFprSignBit);
	armAsm->Orr(RXSCRATCH, RXSCRATCH, kEeFprMaxBits);
	armAsm->Fmov(armDRegister(dstidx), RXSCRATCH);
}

// ---- The EE's divide/square-root unit ---------------------------------------
//
// It is a radix-2 SRT digit recurrence with no rounding step in it, so an Fdiv
// or an Fsqrt does not reproduce it under any rounding mode. FPU.cpp states the
// model above eeSrtDigit; this is the call into it.
//
// Only mode 4 pays for it. Modes 1 to 3 keep the host instruction and the
// FPUDivFPCR swap, which is right on most operands and one ULP out on the rest;
// Mode 3 also takes it, from the integer guards below.
//
// Silicon composes RSQRT.S out of the other two with an ordinary single in
// between, so this does as well; the intermediate crosses the sqrt's call
// through the island's own scratch, x0 being the only register it could
// otherwise live in.
enum class DivUnitOp
{
	Divide,    // eeDivide(fs, ft)
	Sqrt,      // eeSqrtBits(ft)
	RecipSqrt, // eeDivide(fs, eeSqrtBits(ft))
};

static void emitDivideUnitIsland(DivUnitOp op, int dstidx, int fsslotidx, int ftslotidx)
{
	namespace Interp = R5900::Interpreter::OpcodeImpl::COP1;

	IslandFrame f;
	emitIslandEnter(f, op == DivUnitOp::RecipSqrt ? 16 : 0);

	// The models take the architectural words; the slots hold them relocated.
	// eeSqrtBits drops the operand's sign itself, so the host path's |Ft| has no
	// counterpart here.
	if (op == DivUnitOp::Sqrt)
	{
		armEmitEeFprNarrow(RXARG1, armDRegister(ftslotidx), RXSCRATCH);
	}
	else
	{
		armEmitEeFprNarrow(RXARG1, armDRegister(fsslotidx), RXSCRATCH);
		armEmitEeFprNarrow(RXARG2, armDRegister(ftslotidx), RXSCRATCH);
	}

	if (op == DivUnitOp::RecipSqrt)
	{
		armAsm->Str(RXARG1.W(), IslandSpare(f));
		armAsm->Mov(RXARG1.W(), RXARG2.W());
	}
	if (op != DivUnitOp::Divide)
		armEmitCall(reinterpret_cast<const void*>(&Interp::eeSqrtBits));
	if (op == DivUnitOp::RecipSqrt)
	{
		armAsm->Mov(RXARG2.W(), RXARG1.W());
		armAsm->Ldr(RXARG1.W(), IslandSpare(f));
	}
	if (op != DivUnitOp::Sqrt)
		armEmitCall(reinterpret_cast<const void*>(&Interp::eeDivide));

	armAsm->Mov(RWSCRATCH, RXARG1.W());
	emitIslandLeave(f);
	armEmitEeFprWiden(armDRegister(dstidx), RWSCRATCH, RXSCRATCH);
}

// Mode 3 keeps the host quotient unless the divide unit's word truncates to a
// different integer. The unit's word is the host's or the adjacent word on the
// side the host rounded away from (FPU.cpp, eeDivideCap); the sign of
// |fs| - q*|ft| gives that side, zero means exact. Computed at value scale: in
// slot scale it can underflow under FZ.
//
// areg, treg, qreg: fs, ft, q at value scale, clobbered; the caller owns them.
// sreg: the slot; the island overwrites it.
static void emitDivideIntegerGuard(int sreg, int areg, int treg, int qreg, int srcS, int srcT)
{
	a64::Label same;

	armAsm->Fabs(armDRegister(areg), armDRegister(areg));
	armAsm->Fabs(armDRegister(treg), armDRegister(treg));
	armAsm->Fabs(armDRegister(qreg), armDRegister(qreg));
	armAsm->Fmsub(armDRegister(areg), armDRegister(qreg), armDRegister(treg), armDRegister(areg));
	armAsm->Fcmp(armDRegister(areg), 0.0);
	armAsm->B(&same, a64::eq);

	// Csneg reads the Fcmp's flags.
	armAsm->Fcvtzs(RWSCRATCH, armDRegister(qreg));
	armAsm->Fmov(RXARG1, armDRegister(qreg));
	armAsm->Mov(RXARG2, static_cast<u64>(1) << 29); // one word of the single
	armAsm->Csneg(RXARG2, RXARG2, RXARG2, a64::gt);
	armAsm->Add(RXARG1, RXARG1, RXARG2);
	armAsm->Fmov(armDRegister(treg), RXARG1);
	armAsm->Fcvtzs(RWARG1, armDRegister(treg));
	armAsm->Cmp(RWSCRATCH, RWARG1);
	armAsm->B(&same, a64::eq);

	emitDivideUnitIsland(DivUnitOp::Divide, sreg, srcS, srcT);
	armAsm->Bind(&same);
}

// The unit's root is the nearest single or one word below it, and nearest
// whenever the host rounded down or the root was exact
// (TheUnitsRootIsNearestOrTheWordBelow). Only a rounded-up root is tested.
//
// xreg: |ft|, qreg: the root, at value scale, both freed here. sreg: the slot;
// the island overwrites it.
static void emitSqrtIntegerGuard(int sreg, int xreg, int qreg, int srcT)
{
	a64::Label same;

	armAsm->Fmsub(armDRegister(xreg), armDRegister(qreg), armDRegister(qreg), armDRegister(xreg));
	armAsm->Fcmp(armDRegister(xreg), 0.0);
	armAsm->B(&same, a64::ge);

	armAsm->Fcvtzs(RWSCRATCH, armDRegister(qreg));
	armAsm->Fmov(RXARG1, armDRegister(qreg));
	armAsm->Mov(RXARG2, static_cast<u64>(1) << 29);
	armAsm->Sub(RXARG1, RXARG1, RXARG2);
	armAsm->Fmov(armDRegister(xreg), RXARG1);
	armAsm->Fcvtzs(RWARG1, armDRegister(xreg));
	armAsm->Cmp(RWSCRATCH, RWARG1);
	armAsm->B(&same, a64::eq);

	_freeNEONreg(xreg);
	_freeNEONreg(qreg);
	emitDivideUnitIsland(DivUnitOp::Sqrt, sreg, srcT, srcT);
	armAsm->Bind(&same);
}

// The unit's RSQRT.S word lies within 2 words below the host's and 4 above
// (RsqrtSAtFullModeStaysInsideTheWindow). The guard tests whether an integer
// boundary falls inside that window. A zero quotient is skipped.
//
// qreg: q at value scale, clobbered. sreg: the slot; the island overwrites it.
static void emitRsqrtIntegerGuard(int sreg, int qreg, int srcS, int srcT)
{
	a64::Label same;

	armAsm->Fabs(armDRegister(qreg), armDRegister(qreg));
	armAsm->Fmov(RXARG1, armDRegister(qreg));
	armAsm->Cbz(RXARG1, &same);
	armAsm->Mov(RXARG2, static_cast<u64>(2) << 29);
	armAsm->Sub(RXARG2, RXARG1, RXARG2);
	armAsm->Fmov(armDRegister(qreg), RXARG2);
	armAsm->Fcvtzs(RWSCRATCH, armDRegister(qreg));
	armAsm->Mov(RXARG2, static_cast<u64>(4) << 29);
	armAsm->Add(RXARG2, RXARG1, RXARG2);
	armAsm->Fmov(armDRegister(qreg), RXARG2);
	armAsm->Fcvtzs(RWARG1, armDRegister(qreg));
	armAsm->Cmp(RWSCRATCH, RWARG1);
	armAsm->B(&same, a64::eq);

	emitDivideUnitIsland(DivUnitOp::RecipSqrt, sreg, srcS, srcT);
	armAsm->Bind(&same);
}

// x86 recDIVhelper1 (FPU_FLAGS_ID == 1 unconditionally): divide-by-zero
// flag/result shape, otherwise the quotient -- from the recurrence under mode 4
// and in double below it, guarded at mode 3. sreg/treg/areg/qreg are write-only
// temps the caller allocated before anything was emitted -- an allocation can
// evict, and an eviction's writeback emitted inside the normal arm is skipped
// by every divide by zero -- srcS/srcT the guest slots; the result lands in
// sreg as a slot on both arms. treg, areg and qreg are -1 under mode 4, which
// has no double to hold.
// The Fcmp-with-zero runs under the EE FPCR whose FZ bit flushes denormal
// inputs — same divisor-is-zero net as x86's DAZ'd CMPEQ.SS. The double
// quotient of two in-range PS2 values is always finite (max magnitude
// ~2^255), so ToPS2FPU_Full's finite-only contract holds.
static void recDIVhelper1(int sreg, int treg, int areg, int qreg, int srcS, int srcT)
{
	ClearIDFlags();

	a64::Label normal, xOverZero, setDone, done;
	armAsm->Fcmp(armDRegister(srcT), 0.0);
	armAsm->B(&normal, a64::ne);

	// Divisor is ±0: pick the flag pair, then result = (fs ^ ft) | 0x7fffffff
	// (x86 SetMaxValue under FPU_RESULT — see SetMaxValueS above; masking the
	// XOR down to its sign bit first is equivalent, the OR sets bits 0..30).
	armAsm->Fcmp(armDRegister(srcS), 0.0);
	armAsm->B(&xOverZero, a64::ne);
	SetFprcOr(FPUflagI | FPUflagSI); // 0/0
	armAsm->B(&setDone);
	armAsm->Bind(&xOverZero);
	SetFprcOr(FPUflagD | FPUflagSD); // x/0
	armAsm->Bind(&setDone);

	armAsm->Fmov(RXSCRATCH, armDRegister(srcS));
	armAsm->Fmov(RXARG1, armDRegister(srcT));
	armAsm->Eor(RXSCRATCH, RXSCRATCH, RXARG1);
	armAsm->And(RXSCRATCH, RXSCRATCH, kEeFprSignBit);
	armAsm->Orr(RXSCRATCH, RXSCRATCH, kEeFprMaxBits);
	armAsm->Fmov(armDRegister(sreg), RXSCRATCH);
	armAsm->B(&done);

	armAsm->Bind(&normal);
	if (CHECK_FPU_EXACT)
	{
		emitDivideUnitIsland(DivUnitOp::Divide, sreg, srcS, srcT);
	}
	else
	{
		SlotToDouble(areg, srcS);
		SlotToDouble(treg, srcT);
		armAsm->Fdiv(armDRegister(sreg), armDRegister(areg), armDRegister(treg));
		ToPS2FPU_Full(sreg, false, treg, false, false);
		armAsm->Fcvt(armDRegister(qreg), armSRegister(sreg));
		SingleToSlot(sreg, sreg);
		emitDivideIntegerGuard(sreg, areg, treg, qreg, srcS, srcT);
	}

	armAsm->Bind(&done);
}

void recDIV_S_xmm(int info)
{
	// PS2 DIV rounds to nearest (x86 swaps MXCSR to FPUDivFPCR around the op).
	const bool swapFpcr = EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask;
	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUDivFPCR.bitmask);

	// EEREC_D may be either operand and the normal arm writes before it has read
	// both, so the result is built in a temp.
	const int sreg = _allocTempNEONreg();
	const int treg = CHECK_FPU_EXACT ? -1 : _allocTempNEONreg();
	const int areg = CHECK_FPU_EXACT ? -1 : _allocTempNEONreg();
	const int qreg = CHECK_FPU_EXACT ? -1 : _allocTempNEONreg();
	recDIVhelper1(sreg, treg, areg, qreg, EEREC_S, EEREC_T);
	armAsm->Fmov(armDRegister(EEREC_D), armDRegister(sreg));
	_freeNEONreg(sreg);
	if (!CHECK_FPU_EXACT)
	{
		_freeNEONreg(treg);
		_freeNEONreg(areg);
		_freeNEONreg(qreg);
	}

	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUFPCR.bitmask);
}

void recSQRT_S_xmm(int info)
{
	// Round-to-nearest for the double Fsqrt and the narrowing Fcvt, like x86's
	// roundmode_nearest swap (FPUDivFPCR is the nearest-mode FPCR).
	const bool swapFpcr = EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask;
	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUDivFPCR.bitmask);

	// SQRT.S reads FT. The recurrence takes it as a word, so only the host arm
	// needs it widened.
	const int treg = CHECK_FPU_EXACT ? -1 : _allocTempNEONreg();
	if (!CHECK_FPU_EXACT)
		SlotToDouble(treg, EEREC_T);

	ClearIDFlags();
	// x86 DOUBLE tests the raw sign bit (unlike the fast body's exp-field
	// gate): sqrt(-0) sets I|SI too, then |t| makes the operand positive.
	// x86-JIT is the FULL-mode oracle for this corner. The slot carries that
	// bit at 63, so the test needs no word.
	armAsm->Fmov(RXARG1, armDRegister(EEREC_T));
	a64::Label tPositive;
	armAsm->Tbz(RXARG1, 63, &tPositive);
	SetFprcOr(FPUflagI | FPUflagSI);
	if (!CHECK_FPU_EXACT)
		armAsm->Fabs(armDRegister(treg), armDRegister(treg));
	armAsm->Bind(&tPositive);

	if (CHECK_FPU_EXACT)
	{
		emitDivideUnitIsland(DivUnitOp::Sqrt, EEREC_D, EEREC_T, EEREC_T);
	}
	else
	{
		// Temps, not EEREC_D: it may be EEREC_T, which the island reads.
		const int qreg = _allocTempNEONreg();
		const int sreg = _allocTempNEONreg();
		armAsm->Fsqrt(armDRegister(qreg), armDRegister(treg));
		// A root cannot leave the in-range band, so the narrowing is the plain
		// Fcvt with none of ToPS2FPU_Full's arms around it: the largest operand
		// is a shade under 2^129 and roots to under 2^65, the smallest one FZ
		// does not flush is 2^-126 and roots to 2^-63, and both sit inside
		// [2^-126, 2^128). The one result outside it is a zero, which the
		// underflow arm would have flushed to the same zero.
		armAsm->Fcvt(armSRegister(qreg), armDRegister(qreg));
		SingleToSlot(sreg, qreg);
		armAsm->Fcvt(armDRegister(qreg), armSRegister(qreg));
		emitSqrtIntegerGuard(sreg, treg, qreg, EEREC_T);
		armAsm->Fmov(armDRegister(EEREC_D), armDRegister(sreg));
		_freeNEONreg(sreg);
	}

	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUFPCR.bitmask);
}

// x86 recRSQRThelper1: negative-divisor I|SI + |t|, zero-divisor flag pair
// with SetMaxValue keyed off the DIVIDEND's sign, else fs / sqrt(ft) — through
// the recurrence under mode 4 and in double below it. (The interp keys the
// zero-divisor sign off the DIVISOR — x86-JIT wins that disagreement under
// FULL.)
void recRSQRT_S_xmm(int info)
{
	const bool swapFpcr = EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask;
	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUDivFPCR.bitmask);

	// As in recDIV_S_xmm, the result is built in a temp.
	const int sreg = _allocTempNEONreg();
	const int treg = CHECK_FPU_EXACT ? -1 : _allocTempNEONreg();

	ClearIDFlags();

	armAsm->Fmov(RXARG1, armDRegister(EEREC_T));
	a64::Label tPositive;
	armAsm->Tbz(RXARG1, 63, &tPositive);
	SetFprcOr(FPUflagI | FPUflagSI);
	armAsm->Bind(&tPositive);
	// Unconditional: |t| is a no-op on the positive arm and doubles as the copy
	// that keeps ft's slot intact. eeSqrtBits drops the sign itself, so mode 4
	// tests ft where it lies -- Fcmp puts -0 equal to 0 either way.
	if (!CHECK_FPU_EXACT)
		armAsm->Fabs(armDRegister(treg), armDRegister(EEREC_T));

	a64::Label normal, zeroOverZero, setDone, done;
	armAsm->Fcmp(armDRegister(CHECK_FPU_EXACT ? EEREC_T : treg), 0.0);
	armAsm->B(&normal, a64::ne);

	armAsm->Fcmp(armDRegister(EEREC_S), 0.0);
	armAsm->B(&zeroOverZero, a64::eq);
	SetFprcOr(FPUflagD | FPUflagSD); // x/0
	armAsm->B(&setDone);
	armAsm->Bind(&zeroOverZero);
	SetFprcOr(FPUflagI | FPUflagSI); // 0/0
	armAsm->Bind(&setDone);
	SetMaxValueSlot(sreg, EEREC_S);
	armAsm->B(&done);

	armAsm->Bind(&normal);
	if (CHECK_FPU_EXACT)
	{
		emitDivideUnitIsland(DivUnitOp::RecipSqrt, sreg, EEREC_S, EEREC_T);
	}
	else
	{
		SlotToDouble(treg, treg);
		SlotToDouble(sreg, EEREC_S);
		armAsm->Fsqrt(armDRegister(treg), armDRegister(treg));
		armAsm->Fdiv(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));
		ToPS2FPU_Full(sreg, false, treg, false, false);
		armAsm->Fcvt(armDRegister(treg), armSRegister(sreg));
		SingleToSlot(sreg, sreg);
		emitRsqrtIntegerGuard(sreg, treg, EEREC_S, EEREC_T);
	}

	armAsm->Bind(&done);
	armAsm->Fmov(armDRegister(EEREC_D), armDRegister(sreg));
	_freeNEONreg(sreg);
	if (!CHECK_FPU_EXACT)
		_freeNEONreg(treg);

	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUFPCR.bitmask);
}

#undef _Ft_
#undef _Fs_
#undef _Fd_

} // namespace DOUBLE
} // namespace COP1
} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
