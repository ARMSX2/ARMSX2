// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 EE FPU (COP1) — "Full" / DOUBLE-precision codegen.
//
// This is the arm64 port of pcsx2/x86/iFPUd.cpp: the PS2-accurate FPU that
// widens each single to IEEE double, performs the op in double, then narrows
// back to a PS2 single with the hardware's overflow/underflow/clamp semantics.
// It is selected when CHECK_FPU_FULL (EmuConfig.Cpu.Recompiler.fpuFullMode, the
// GameDB `eeClampMode:3` path — FFX, Max Payne, Dark Cloud 2, Klonoa 2 …).
// Default config runs the single-precision fast path in iFPU-arm64.cpp.
//
// It serves eeClampMode 3 and 4, which differ only at emitDefectiveFmul below.
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

// The narrowing pair: a slot as the architectural single in an S lane, and back.
static void SlotToSingle(int dstidx, int srcidx)
{
	armEmitEeFprToS(armSRegister(dstidx), armDRegister(srcidx), RWSCRATCH, a64::x9);
}

static void SingleToSlot(int dstidx, int srcidx)
{
	armEmitEeFprFromS(armDRegister(dstidx), armSRegister(srcidx), RXSCRATCH);
}

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

// Read a guest slot (EEREC_S/EEREC_T) into a fresh temp as the architectural
// single, which the emitter can then mutate without touching the slot.
//
// The paths that need the single are the ones that mutate or test the word:
// recFPUOp's FPU_ADD_SUB guard mask, the Fabs and sign tests in SQRT/RSQRT, and
// recDIVhelper1's zero test. A site that only widens uses SlotToDouble.
static int narrowSrc(int eerec)
{
	const int idx = _allocTempNEONreg();
	SlotToSingle(idx, eerec);
	return idx;
}

// ADD/SUB/ADDA/SUBA: FPU_ADD_SUB guard mask -> widen -> op in double -> narrow.
static void recFPUOp(int info, int eeRecDst, int op /*0=add,1=sub*/, bool acc)
{
	const int sreg = narrowSrc(EEREC_S);
	const int treg = narrowSrc(EEREC_T);

	FPU_ADD_SUB(sreg, treg);
	ToDouble(sreg);
	ToDouble(treg);

	if (op == 0)
		armAsm->Fadd(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));
	else
		armAsm->Fsub(armDRegister(sreg), armDRegister(sreg), armDRegister(treg));

	ToPS2FPU_Full(sreg, true, treg, acc, true);
	SingleToSlot(eeRecDst, sreg);

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
// Registers around the call: the stub is plain AAPCS, so every caller-saved
// home the allocator is using is spilled across it and the EE pin mirrors go
// through their flush/reload pair. It is pure arithmetic on its two arguments
// -- it cannot raise, take an exception or read cpuRegs -- so unlike the vtlb
// slow paths it needs no pc/code flush and no cycle spill. x8 carries the
// verdict back because it is neither allocatable nor a pin, so no restore
// touches it.
static void emitMulArrayIsland(const a64::VRegister& prod, int fsslotidx, int ftslotidx)
{
	u8 gprs[8], fprs[NUM_ARM_NEON_REGS];
	u32 ngpr = 0, nfpr = 0;
	for (int i = 0; i < NUM_ARM_GPR_REGS; i++)
	{
		// Leaves x4-x7 and x14/x15, the caller-saved half of the EE pool. x0-x3
		// and x8-x10 are scratch, x11-x13 are pins flushed below, x16+ are
		// reserved or callee-saved.
		if (i >= 16 || (i >= 8 && i <= 13) || i <= 3)
			continue;
		if (arm64gprs[i].inuse)
			gprs[ngpr++] = static_cast<u8>(i);
	}
	for (int i = 0; i < NUM_ARM_NEON_REGS; i++)
	{
		// AAPCS64 preserves only the low 64 bits of q8-q15, and the allocator
		// keeps 128-bit classes there, so every live one is saved in full.
		if (arm64neon[i].inuse)
			fprs[nfpr++] = static_cast<u8>(i);
	}

	const u32 frame = (ngpr * 8 + nfpr * 16 + 15u) & ~15u;
	if (frame)
		armAsm->Sub(a64::sp, a64::sp, frame);
	u32 off = 0;
	for (u32 i = 0; i < ngpr; i++, off += 8)
		armAsm->Str(a64::XRegister(gprs[i]), a64::MemOperand(a64::sp, off));
	for (u32 i = 0; i < nfpr; i++, off += 16)
		armAsm->Str(a64::QRegister(fprs[i]), a64::MemOperand(a64::sp, off));

	// The stub takes the architectural words; the slots hold them relocated.
	armEmitEeFprNarrow(RXARG1, armDRegister(fsslotidx), RXSCRATCH);
	armEmitEeFprNarrow(RXARG2, armDRegister(ftslotidx), RXSCRATCH);
	// Flush before, reload after: under the lazy-dirty pin policy the mirrors
	// are newer than canonical memory, so reloading on its own would lose the
	// writes the block has made to them.
	armFlushEEClobberedPins();
	armEmitCall(reinterpret_cast<const void*>(
		&R5900::Interpreter::OpcodeImpl::COP1::eeMulOneUlpLow));
	// AAPCS64 leaves everything above a bool return's one byte unspecified.
	armAsm->And(RXSCRATCH, RXARG1, 1);

	off = 0;
	for (u32 i = 0; i < ngpr; i++, off += 8)
		armAsm->Ldr(a64::XRegister(gprs[i]), a64::MemOperand(a64::sp, off));
	for (u32 i = 0; i < nfpr; i++, off += 16)
		armAsm->Ldr(a64::QRegister(fprs[i]), a64::MemOperand(a64::sp, off));
	if (frame)
		armAsm->Add(a64::sp, a64::sp, frame);
	armReloadEEClobberedPins();

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

	armEmitEeFprNarrow(RWSCRATCH, armDRegister(EEREC_S), a64::x9); // x8 = zext(s bits)
	armAsm->And(RWARG1, RWSCRATCH, 0x80000000);
	armAsm->Orr(RWARG1, RWARG1, 0x40000000);
	armAsm->Orr(RXSCRATCH, RXSCRATCH, a64::Operand(RXARG1, a64::LSL, 32));

	armEmitEeFprNarrow(RWARG2, armDRegister(EEREC_T), a64::x9); // x1 = zext(t bits)
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
	armEmitEeFprWiden(armDRegister(EEREC_D), RWSCRATCH, RXSCRATCH); // lower 32 = winner's raw bits
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
	const int nsreg = narrowSrc(EEREC_S);
	const int ntreg = narrowSrc(EEREC_T);
	recDIVhelper1(sreg, treg, nsreg, ntreg);
	SingleToSlot(EEREC_D, sreg);
	_freeNEONreg(sreg);
	_freeNEONreg(treg);
	_freeNEONreg(nsreg);
	_freeNEONreg(ntreg);

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

	const int treg = narrowSrc(EEREC_T); // SQRT.S reads FT

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
	SingleToSlot(EEREC_D, treg);
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

	const int sreg = narrowSrc(EEREC_S);
	const int treg = narrowSrc(EEREC_T);

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
	SingleToSlot(EEREC_D, sreg);
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
