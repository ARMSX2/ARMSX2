// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 EE FPU (COP1) — "Full" / DOUBLE-precision codegen.
//
// This is the arm64 port of pcsx2/x86/iFPUd.cpp: the PS2-accurate FPU that
// widens each single to IEEE double, performs the op in double, then narrows
// back to a PS2 single with the hardware's overflow/underflow/clamp semantics.
// It is selected only when CHECK_FPU_FULL (EmuConfig.Cpu.Recompiler.fpuFullMode,
// the GameDB `eeClampMode:3` path — FFX, Max Payne, Dark Cloud 2, Klonoa 2 …).
// Default config runs the single-precision fast path in iFPU-arm64.cpp.
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

// ---- PS2 single -> IEEE double --------------------------------------------
//
// A PS2 single with exponent field 0xff is a *normal* large number (1.m * 2^128),
// but IEEE reads exp 0xff as Inf/NaN — so a plain cvtss2sd would corrupt it.
// For those (and only those) lower the exponent by one in the single domain,
// widen exactly, then raise the exponent by one in the double domain. Mirrors
// x86 ToDouble (xPSUB.D one_exp / xCVTSS2SD / xPADD.Q dbl_one_exp).
//
// Reads `srcidx`'s S lane, writes `dstidx`'s D lane, and never writes the
// source. The two may be the same register (that is ToDouble below).
//
// The source is allowed to be an allocator-resident guest FPR or the ACC, which
// is the point: the complex arm needs somewhere to put the exponent-lowered
// single before Fcvt, and it uses the destination's S lane — a temp the caller
// owns — instead of scribbling on the source. Every widening site used to pay a
// `copySrc` Fmov purely to make that scribble legal.
static void ToDoubleFrom(int dstidx, int srcidx)
{
	const a64::VRegister ss = armSRegister(srcidx);
	const a64::VRegister sd = armSRegister(dstidx);
	const a64::VRegister dd = armDRegister(dstidx);

	a64::Label simple, done;
	armAsm->Fmov(RWSCRATCH, ss);
	armAsm->And(RWARG1, RWSCRATCH, 0x7f800000);
	armAsm->Cmp(RWARG1, 0x7f800000);
	armAsm->B(&simple, a64::ne);

	// Complex: exp field == 0xff (Inf/NaN to IEEE, finite to PS2).
	armAsm->Sub(RWSCRATCH, RWSCRATCH, 0x00800000);   // lower exponent by one (single)
	armAsm->Fmov(sd, RWSCRATCH);
	armAsm->Fcvt(dd, sd);                            // cvtss2sd (now finite)
	armAsm->Fmov(RXSCRATCH, dd);
	armAsm->Mov(RXARG1, static_cast<u64>(1) << 52);  // dbl_one_exp
	armAsm->Add(RXSCRATCH, RXSCRATCH, RXARG1);       // raise exponent by one (double)
	armAsm->Fmov(dd, RXSCRATCH);
	armAsm->B(&done);

	armAsm->Bind(&simple);
	armAsm->Fcvt(dd, ss);

	armAsm->Bind(&done);
}

// In-place form: widen temp NEON reg `idx` from its own S lane.
static void ToDouble(int idx)
{
	ToDoubleFrom(idx, idx);
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
	// 0x7f7fffff + 0x00800000 == 0x7fffffff). This is the same bound the
	// interpreter's eeRoundToSingle saturates at (FPU.cpp).
	armAsm->Mov(RXARG2, UINT64_C(0x47FFFFFFE0000000)); // (2 - 2^-23) * 2^128
	armAsm->Cmp(RXARG1, RXARG2);
	armAsm->B(&toOverflow, a64::hi);

	// Large but PS2-representable (exp-0xff range): lower double exp, narrow,
	// raise single exp — the inverse of ToDouble's complex path.
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
// the exponent difference so they read as zero. Port of x86 FPU_ADD_SUB; both
// operands (single, in temp NEON regs `idxd`/`idxt`) are mutated in place.
static void FPU_ADD_SUB(int idxd, int idxt)
{
	const a64::VRegister sd = armSRegister(idxd);
	const a64::VRegister st = armSRegister(idxt);

	armAsm->Fmov(RWARG1, sd);  // d bits
	armAsm->Fmov(RWARG2, st);  // t bits
	// GE-M2: the exponent-diff and mask temps use the reserved load/store scratch
	// x9/x10, not the RWARG3/RWARG4 (w2/w3) pool hosts they replaced — w2/w3 are
	// EE-allocatable, so under the residency flip they can hold a live guest GPR,
	// and this hand-emitted path never flushes the allocator. This span has no
	// load/store or C-call, so x9/x10 are free scratch here. (x86 uses GPR temps
	// too: pcsx2/x86/iFPU.cpp FPU_ADD_SUB; only the register choice is our
	// scratch-discipline constraint.)
	armAsm->Ubfx(a64::w9, RWARG1, 23, 8);    // expd
	armAsm->Ubfx(RWSCRATCH, RWARG2, 23, 8); // expt
	armAsm->Sub(a64::w9, a64::w9, RWSCRATCH); // diff = expd - expt (signed)

	a64::Label caseD25, casePos, caseEq, caseDn25, done;
	armAsm->Cmp(a64::w9, 25);
	armAsm->B(&caseD25, a64::ge);
	armAsm->Cmp(a64::w9, 0);
	armAsm->B(&casePos, a64::gt);
	armAsm->B(&caseEq, a64::eq);
	armAsm->Cmn(a64::w9, 25);                 // cmp diff, -25
	armAsm->B(&caseDn25, a64::le);

	// diff in -24..-1 (expd < expt): mask tempd's low (-diff-1) bits.
	armAsm->Neg(RWSCRATCH, a64::w9);
	armAsm->Sub(RWSCRATCH, RWSCRATCH, 1);
	armAsm->Mov(a64::w10, 0xffffffff);
	armAsm->Lsl(a64::w10, a64::w10, RWSCRATCH);
	armAsm->And(RWARG1, RWARG1, a64::w10);
	armAsm->Fmov(sd, RWARG1);
	armAsm->B(&done);

	armAsm->Bind(&caseD25);
	// diff >= 25 (expt much smaller): tempt keeps only its sign.
	armAsm->And(RWARG2, RWARG2, 0x80000000);
	armAsm->Fmov(st, RWARG2);
	armAsm->B(&done);

	armAsm->Bind(&casePos);
	// diff in 1..24 (expt smaller): mask tempt's low (diff-1) bits.
	armAsm->Sub(RWSCRATCH, a64::w9, 1);
	armAsm->Mov(a64::w10, 0xffffffff);
	armAsm->Lsl(a64::w10, a64::w10, RWSCRATCH);
	armAsm->And(RWARG2, RWARG2, a64::w10);
	armAsm->Fmov(st, RWARG2);
	armAsm->B(&done);

	armAsm->Bind(&caseDn25);
	// diff <= -25 (expd much smaller): tempd keeps only its sign.
	armAsm->And(RWARG1, RWARG1, 0x80000000);
	armAsm->Fmov(sd, RWARG1);

	armAsm->Bind(&caseEq);  // diff == 0: nothing
	armAsm->Bind(&done);
}

// ---- PS2 add/sub guard-bit emulation, wide form ---------------------------
//
// Same law as FPU_ADD_SUB, for operands that are already doubles holding EE
// singles exactly (low 29 mantissa bits zero, |x| <= kEeFpuMax). Two changes,
// neither of which costs an instruction:
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
//    (A PS2 denormal cannot reach here: ToDouble runs under FZ, which flushes
//    it to a zero of the same sign -- measured on this host, not assumed.)
//  * A single's mantissa bit k is double bit k+29, so masking the single's low
//    (diff-1) bits is masking the double's low (diff-1)+29. The extra 29 are
//    already zero, so only the shift amount changes: `diff - 1` -> `diff + 28`.
static void FPU_ADD_SUB_D(int idxd, int idxt)
{
	const a64::VRegister dd = armDRegister(idxd);
	const a64::VRegister dt = armDRegister(idxt);

	armAsm->Fmov(RXARG1, dd);  // d bits
	armAsm->Fmov(RXARG2, dt);  // t bits
	// GE-M2: x9/x10 for the diff and mask temps, not the w2/w3 pool hosts -- see
	// the note in FPU_ADD_SUB. This span has no load/store or C-call either.
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

// Copy an allocator-resident FP source (EEREC_S/EEREC_T) into a fresh temp so
// the emitter can mutate it without corrupting the guest fpr slot.
//
// Only the paths that mutate the operand in the single domain still need this:
// recFPUOp's FPU_ADD_SUB guard mask, and the Fabs in SQRT/RSQRT. A site that
// only widens uses ToDoubleFrom(temp, EEREC_x) instead and pays no copy.
static int copySrc(int eerec)
{
	const int idx = _allocTempNEONreg();
	armAsm->Fmov(armSRegister(idx), armSRegister(eerec));
	return idx;
}

// ADD/SUB/ADDA/SUBA: FPU_ADD_SUB guard mask -> widen -> op in double -> narrow.
static void recFPUOp(int info, int eeRecDst, int op /*0=add,1=sub*/, bool acc)
{
	const int sreg = copySrc(EEREC_S);
	const int treg = copySrc(EEREC_T);

	FPU_ADD_SUB(sreg, treg);
	ToDouble(sreg);
	ToDouble(treg);

	if (op == 0)
		armAsm->Fadd(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));
	else
		armAsm->Fsub(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));

	ToPS2FPU_Full(sreg, true, treg, acc, true);
	armAsm->Fmov(armSRegister(eeRecDst), armSRegister(sreg));

	_freeNEONreg(sreg);
	_freeNEONreg(treg);
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
// This is the mode-3 codegen for the zero-tail law. FpuMulHack is a one-point
// sample of the same rule and this subsumes it, including the asymmetry -- it
// is not folded in here because iFPUd never had it.
//
// The product is computed in double, where a 24x24 significand multiply is
// exact, so condition 1 needs no integer multiply and no tail extraction:
// one integer ULP of the double is strictly below one ULP of the single, so
// decrementing the double's raw bit pattern drops an exactly-representable
// product one single-step and leaves everything else inside the same single
// bucket. The rounding downstream performs the tail test -- and it is the same
// test under either of the two narrowings this file uses: ToPS2FPU_Wide's
// mask-off-the-low-29 is truncation by construction, and ToPS2FPU_Full's Fcvt
// runs under FPUFPCR, whose mode is round-toward-zero (the argument is spelled
// out at ToPS2FPU_Wide). That equivalence is structural. The FPCR is the
// dependency that actually matters: under round-to-nearest the decremented
// double would round straight back up to the product it came from and this
// would emit nothing at all.
//
// A NEON compare result is all-ones, which as a 64-bit lane is -1, so `Add` of
// the mask is the conditional decrement. Two traps, both already paid for once:
//
//   * the Add is a 64-bit lane, so the mask must be all-ones across the full 64
//     bits. A 32-bit compare adds +0xFFFFFFFF instead of -1; that scores 98.28%
//     and reads like a predicate bug when it is a mask bug.
//   * the Bic is `.8b` because there is no scalar BIC. AdvSIMD BIC has only the
//     .8B/.16B register forms and the .4H/.8H/.2S/.4S immediate form; `bic
//     d30, d30, d31` is rejected outright by an assembler. vixl guards it with
//     a VIXL_ASSERT, which this tree compiles out so it emits 0x5eff1fde, an
//     undefined encoding, and the JIT takes SIGILL the first time the block
//     runs.
//   * a zero product must be excluded, which is the Fcmeq/Bic pair. Under FZ a
//     zero or denormal operand widens to +/-0 and the product is exactly +/-0,
//     whose pattern decrements to 0xFFFFFFFFFFFFFFFF -- a NaN. Testing the
//     product covers both of the interpreter's guards at once: a product is
//     exactly +/-0 only when an operand was zero or denormal, because the
//     smallest product of two EE normals is ~2^-252, an ordinary double.
//
// The interpreter's two remaining guards need no codegen. A saturating result
// is unreachable-by-one-ULP: products are multiples of 2^81 at that exponent
// while a double ULP there is 2^76, so no decrement can walk a product from
// above kEeFpuMax down to it, and a product landing exactly on kEeFpuMax is
// decremented by the interpreter too. "A decrement would leave the normals"
// (w == 0x00800000) needs ma*mb == 2^46 with both in [2^23, 2^24), forcing
// ma == mb == 2^23 -- ft mantissa 0, predicate off.
//
// The predicate is the cheap half of the measured one. `mant & 0x2AA` -- bits
// 1,3,5,7,9, the sign bits of the five lowest radix-4 Booth digits -- is the
// whole rule except for a boundary term at the truncation column,
// `bit11 != (8 <= (mant >> 12 & 0xF) <= 13)`, which is dropped. It fires on
// exactly 1/64 of significands, and only a zero tail lets it change anything:
// counted over all 2^46 significand pairs, 76,236,820 have a zero tail
// -- 1 pair in 923,028 -- so the term decides 1 pair in 59,073,813. When fs is
//  a power of two the tail is always zero, and that class scores 98.4375%
// instead of 100%. Tested against hardware, this predicate gives
// 98.4375 / 99.8421 / 99.8482 / 99.8091 per fs significand against the full
// one's 100 / 99.8421 / 99.8482 / 99.8091.
// The other three rows are not evidence that the term buys nothing elsewhere:
// their fs values have a zero tail on 1 or 2 rows out of 2^23, so the term
// cannot show in them at all. The fpmul3 sweeps, whose fs values have trailing
// zeros, do populate that cell: at fs = 1.5 there are 2,796,203 zero-tail rows
// and the term decides 65,536 of them (2.34%). Pooled over fpmul3's 7,196,506
// zero-tail rows the full predicate is exactly hardware -- 0 missed, 0 wrong --
// while this one misses 229,142 and is likewise never wrong. So the gap is a
// real 3.3% of the reachable class; what makes it acceptable is that it is
// one-directional (it can only miss a deficit, never invent one) and rare in
// general operand space, per the count above. The term needs a bitfield extract
// NEON has no equivalent for, so it has to go through GPRs and come back --
// sketched at ten instructions against this predicate's one.
// The resulting interpreter divergence is pinned by
// EeRecFpuFull.MulDefectDropsTheBoundaryTermTheInterpreterModels.
//
// ft is read narrow, out of the allocator-resident guest register: Cmtst on the
// 64-bit lane only looks at bits 0..9 -- the single's mantissa bits 0..9,
// whatever the register's upper half happens to hold -- so the mask is the
// single-domain 0x2AA and not the double-domain 0x2AA << 29.
//
// The mask is not materialized here: it is parked in d10 for the whole JIT
// session by _DynGen_EnterRecompiledCode, next to the s8/s9 clamp constants and
// under the same AAPCS64 argument: the low 64 bits of d8-d15 are callee-saved,
// so the constant survives every C call without compile-time liveness tracking.
// That is what took this from six instructions to four, and it applies to the
// first multiply in a block as much as the tenth -- a liveness flag in a
// caller-saved register would still have paid mov+fmov to open every span, and
// would have had to be invalidated at C-call seams, branch forks, superblock
// side-exit bodies (recEmitColdSideExits emits several per emission session,
// each reachable only through its own island) and backpatched fastmem thunks,
// where a single missed seam is a silent one-ULP error the corpus cannot see.
// The register costs one slot out of the callee-saved allocator range; the full
// contract, including why microVU needs no pool gate for it, is on
// NEON_RESERVED_FPU_MULMASK in iCore-arm64.h.
//
// `dstidx` holds ToDouble(fs) on entry and the product on exit, `tidx` holds
// ToDouble(ft), `ftnarrowidx` is the untouched guest ft. RQSCRATCH/RQSCRATCH2
// (q30/q31) are outside the allocator pool, so no temp aliases them.
//
// What comes out, decoded from the code buffer (the Fmul was already there, so
// four of these five are the cost):
//
//     cmtst d30, d11, d10            ; d11 == EEREC_T (narrow guest ft), d10 == the parked mask
//     fmul  d0, d0, d1
//     fcmeq d31, d0, #0.0
//     bic   v30.8b, v30.8b, v31.8b
//     add   d0, d0, d30
static void emitDefectiveFmul(int dstidx, int tidx, int ftnarrowidx)
{
	const a64::VRegister prod = armDRegister(dstidx);

	// Hoisted above the Fmul: the predicate is not on its dependency chain.
	armAsm->Cmtst(RDSCRATCH, armDRegister(ftnarrowidx), a64::VRegister(NEON_RESERVED_FPU_MULMASK, 64));

	armAsm->Fmul(prod, prod, armDRegister(tidx));

	armAsm->Fcmeq(RDSCRATCH2, prod, 0.0);
	armAsm->Bic(RQSCRATCH.V8B(), RQSCRATCH.V8B(), RQSCRATCH2.V8B());
	armAsm->Add(prod, prod, RDSCRATCH);
}

// MUL/MULA: widen -> multiply in double (with the multiplier deficit) -> narrow.
static void recMULop(int info, int eeRecDst, bool acc)
{
	// Both temps before any emit: _allocTempNEONreg can evict, and an eviction's
	// writeback must not land between an operand's copy and its use.
	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();

	ToDoubleFrom(sreg, EEREC_S);
	ToDoubleFrom(treg, EEREC_T);
	emitDefectiveFmul(sreg, treg, EEREC_T);

	ToPS2FPU_Full(sreg, true, treg, acc, false);
	armAsm->Fmov(armSRegister(eeRecDst), armSRegister(sreg));

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
// Representation: unlike the x86 port and unlike recFPUOp/recMULop, everything
// between the two roundings stays wide. The invariant from the multiply stage
// to the final ToPS2FPU_Full is "this double is exactly a PS2 single" — low 29
// mantissa bits zero, |x| <= kEeFpuMax, no denormals — which the guard mask
// preserves (it only clears low bits or reduces an operand to its sign) and
// which is what makes the accumulate exact: two 24-bit significands at an
// exponent distance of at most 24 sum in 48 bits, well inside a double's 53.
// The one arm that leaves the wide domain early is accovf, because kEeFpuMax
// has no single a narrowing could reach.
static void recMaddsub(int info, int eeRecDst, int op /*0=add,1=sub*/, bool acc)
{
	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();

	// --- multiply stage: sreg = ToPS2FPU(ToDouble(s) * ToDouble(t)). Sets O on
	//     product overflow; never touches ACCflag here. ---
	//
	// The product is rounded but not narrowed: ToPS2FPU_Wide leaves it as a
	// double holding an exact PS2 single. Everything downstream of it in this
	// emitter -- the guard mask, the SUB sign flip, the accumulate -- wants the
	// wide form back, and narrowing here only to re-widen 13 instructions later
	// was the round trip this shape exists to remove.
	ToDoubleFrom(sreg, EEREC_S);
	ToDoubleFrom(treg, EEREC_T);
	emitDefectiveFmul(sreg, treg, EEREC_T);
	ToPS2FPU_Wide(sreg);

	// --- widen the (allocator-resident, still narrow) ACC straight into treg,
	//     then guard-mask it against the product in the wide domain. ---
	ToDoubleFrom(treg, EEREC_ACC);
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
	armAsm->Fmov(armSRegister(eeRecDst), armSRegister(treg));

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
	armAsm->Fabs(armSRegister(EEREC_D), armSRegister(EEREC_S));
}

void recNEG_S_xmm(int info)
{
	ClearOUFlags();
	armAsm->Fneg(armSRegister(EEREC_D), armSRegister(EEREC_S));
}

// MAX/MIN: PS2 semantics on ALL values (incl. denormals — no FTZ, no clamp).
// Port of x86 recMINMAX's integer-ordering trick: for each operand build the
// 64-bit double pattern {lo32 = raw float bits, hi32 = sign | 0x40000000} and
// compare as doubles. The fixed 0x400-exponent upper word makes IEEE-double
// ordering equal PS2 total (sign, magnitude) ordering over the raw bits, and
// no constructed input can be NaN/Inf (the double exponent field is constant),
// so Fmin/Fmax's NaN propagation can never trigger. Result = lower 32 bits of
// the selected pattern.
static void recMINMAX(int info, bool ismin)
{
	// Temps FIRST: the alloc's eviction stores must not land between the GPR
	// pattern builds and their consuming Fmovs (alloc-before-emit rule).
	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();

	ClearOUFlags();

	armAsm->Fmov(RWSCRATCH, armSRegister(EEREC_S)); // x8 = zext(s bits)
	armAsm->And(RWARG1, RWSCRATCH, 0x80000000);
	armAsm->Orr(RWARG1, RWARG1, 0x40000000);
	armAsm->Orr(RXSCRATCH, RXSCRATCH, a64::Operand(RXARG1, a64::LSL, 32));

	armAsm->Fmov(RWARG2, armSRegister(EEREC_T)); // x1 = zext(t bits)
	// GE-M2: exp/sign pattern temp in reserved scratch x9 (was RWARG3/w2, an
	// EE-allocatable pool host — see FPU_ADD_SUB). No load/store or C-call spans it.
	armAsm->And(a64::w9, RWARG2, 0x80000000);
	armAsm->Orr(a64::w9, a64::w9, 0x40000000);
	armAsm->Orr(RXARG2, RXARG2, a64::Operand(a64::x9, a64::LSL, 32));

	armAsm->Fmov(armDRegister(sreg), RXSCRATCH);
	armAsm->Fmov(armDRegister(treg), RXARG2);
	if (ismin)
		armAsm->Fmin(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));
	else
		armAsm->Fmax(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));
	armAsm->Fmov(RXSCRATCH, armDRegister(sreg));
	armAsm->Fmov(armSRegister(EEREC_D), RWSCRATCH); // lower 32 = winner's raw bits
	_freeNEONreg(sreg);
	_freeNEONreg(treg);
}

void recMAX_S_xmm(int info) { recMINMAX(info, false); }
void recMIN_S_xmm(int info) { recMINMAX(info, true); }

// C.cond: widen both operands with ToDouble and compare as doubles — a PS2
// pseudo-inf compares as the finite 2^128-scale number it is, with no operand
// clamping (x86 recCMP + recC_*_xmm). ToDouble never yields NaN, so the
// compare is always ordered and the lt/le/eq condition reads are exact.
static void recCMP(int info)
{
	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();
	ToDoubleFrom(sreg, EEREC_S);
	ToDoubleFrom(treg, EEREC_T);
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
// difference from +FLT_MAX is game-visible.
static void SetMaxValueS(int idx)
{
	armAsm->Fmov(RWSCRATCH, armSRegister(idx));
	armAsm->And(RWSCRATCH, RWSCRATCH, 0x80000000);
	armAsm->Orr(RWSCRATCH, RWSCRATCH, 0x7fffffff);
	armAsm->Fmov(armSRegister(idx), RWSCRATCH);
}

// x86 recDIVhelper1 (FPU_FLAGS_ID == 1 unconditionally): divide-by-zero
// flag/result shape in the single domain, otherwise divide in double.
// sreg/treg are write-only temps and srcS/srcT the allocator-resident operands,
// which are only ever read; the result lands in sreg (S lane on the zero-divisor
// arm, S lane after ToPS2FPU_Full on the normal one).
// The Fcmp-with-zero runs under the EE FPCR whose FZ bit flushes denormal
// inputs — same divisor-is-zero net as x86's DAZ'd CMPEQ.SS. The double
// quotient of two in-range PS2 values is always finite (max magnitude
// ~2^255), so ToPS2FPU_Full's finite-only contract holds.
static void recDIVhelper1(int sreg, int treg, int srcS, int srcT)
{
	ClearIDFlags();

	a64::Label normal, xOverZero, setDone, done;
	armAsm->Fcmp(armSRegister(srcT), 0.0);
	armAsm->B(&normal, a64::ne);

	// Divisor is ±0: pick the flag pair, then result = (fs ^ ft) | 0x7fffffff
	// (x86 SetMaxValue under FPU_RESULT — see SetMaxValueS above; masking the
	// XOR down to its sign bit first is equivalent, the OR sets bits 0..30).
	armAsm->Fcmp(armSRegister(srcS), 0.0);
	armAsm->B(&xOverZero, a64::ne);
	SetFprcOr(FPUflagI | FPUflagSI); // 0/0
	armAsm->B(&setDone);
	armAsm->Bind(&xOverZero);
	SetFprcOr(FPUflagD | FPUflagSD); // x/0
	armAsm->Bind(&setDone);

	armAsm->Fmov(RWSCRATCH, armSRegister(srcS));
	armAsm->Fmov(RWARG1, armSRegister(srcT));
	armAsm->Eor(RWSCRATCH, RWSCRATCH, RWARG1);
	armAsm->And(RWSCRATCH, RWSCRATCH, 0x80000000);
	armAsm->Orr(RWSCRATCH, RWSCRATCH, 0x7fffffff);
	armAsm->Fmov(armSRegister(sreg), RWSCRATCH);
	armAsm->B(&done);

	armAsm->Bind(&normal);
	ToDoubleFrom(sreg, srcS);
	ToDoubleFrom(treg, srcT);
	armAsm->Fdiv(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));
	ToPS2FPU_Full(sreg, false, treg, false, false);

	armAsm->Bind(&done);
}

void recDIV_S_xmm(int info)
{
	// PS2 DIV rounds to nearest (x86 swaps MXCSR to FPUDivFPCR around the op).
	const bool swapFpcr = EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask;
	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUDivFPCR.bitmask);

	const int sreg = _allocTempNEONreg();
	const int treg = _allocTempNEONreg();
	recDIVhelper1(sreg, treg, EEREC_S, EEREC_T);
	armAsm->Fmov(armSRegister(EEREC_D), armSRegister(sreg));
	_freeNEONreg(sreg);
	_freeNEONreg(treg);

	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUFPCR.bitmask);
}

void recSQRT_S_xmm(int info)
{
	// Round-to-nearest for the double Fsqrt + the ToPS2FPU narrowing, like
	// x86's roundmode_nearest swap (FPUDivFPCR is the nearest-mode FPCR).
	const bool swapFpcr = EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask;
	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUDivFPCR.bitmask);

	const int treg = copySrc(EEREC_T); // SQRT.S reads FT

	ClearIDFlags();
	// x86 DOUBLE tests the raw SIGN BIT (unlike the fast body's exp-field
	// gate): sqrt(-0) sets I|SI too, then |t| makes the operand positive.
	// x86-JIT is the FULL-mode oracle for this corner.
	armAsm->Fmov(RWARG1, armSRegister(treg));
	a64::Label tPositive;
	armAsm->Tbz(RWARG1, 31, &tPositive);
	SetFprcOr(FPUflagI | FPUflagSI);
	armAsm->Fabs(armSRegister(treg), armSRegister(treg));
	armAsm->Bind(&tPositive);

	ToDouble(treg);
	armAsm->Fsqrt(armDRegister(treg), armDRegister(treg));
	ToPS2FPU_Full(treg, false, treg, false, false);
	armAsm->Fmov(armSRegister(EEREC_D), armSRegister(treg));
	_freeNEONreg(treg);

	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUFPCR.bitmask);
}

// x86 recRSQRThelper1: negative-divisor I|SI + |t|, zero-divisor flag pair
// with SetMaxValue keyed off the DIVIDEND's sign, else fs / sqrt(ft) in
// double. (The interp keys the zero-divisor sign off the DIVISOR — x86-JIT
// wins that disagreement under FULL.)
void recRSQRT_S_xmm(int info)
{
	const bool swapFpcr = EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask;
	if (swapFpcr)
		emitLoadFPCRImm(EmuConfig.Cpu.FPUDivFPCR.bitmask);

	const int sreg = copySrc(EEREC_S);
	const int treg = copySrc(EEREC_T);

	ClearIDFlags();

	armAsm->Fmov(RWARG1, armSRegister(treg));
	a64::Label tPositive;
	armAsm->Tbz(RWARG1, 31, &tPositive);
	SetFprcOr(FPUflagI | FPUflagSI);
	armAsm->Fabs(armSRegister(treg), armSRegister(treg));
	armAsm->Bind(&tPositive);

	a64::Label normal, zeroOverZero, setDone, done;
	armAsm->Fcmp(armSRegister(treg), 0.0);
	armAsm->B(&normal, a64::ne);

	armAsm->Fcmp(armSRegister(sreg), 0.0);
	armAsm->B(&zeroOverZero, a64::eq);
	SetFprcOr(FPUflagD | FPUflagSD); // x/0
	armAsm->B(&setDone);
	armAsm->Bind(&zeroOverZero);
	SetFprcOr(FPUflagI | FPUflagSI); // 0/0
	armAsm->Bind(&setDone);
	SetMaxValueS(sreg);
	armAsm->B(&done);

	armAsm->Bind(&normal);
	ToDouble(treg);
	ToDouble(sreg);
	armAsm->Fsqrt(armDRegister(treg), armDRegister(treg));
	armAsm->Fdiv(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));
	ToPS2FPU_Full(sreg, false, treg, false, false);

	armAsm->Bind(&done);
	armAsm->Fmov(armSRegister(EEREC_D), armSRegister(sreg));
	_freeNEONreg(sreg);
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
