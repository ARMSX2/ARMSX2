// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "arm64/AsmHelpers.h"

#include "common/Assertions.h"

namespace a64 = vixl::aarch64;

// ========================================================================
//  The VU FMAC's MAC U and MAC O, as four-lane predicates
// ========================================================================
// All-ones-or-zero lane masks for armEmitPackSignZeroBits. Both the COP2 macro
// path (iCOP2-arm64.cpp) and microVU (microVU_Upper-arm64.inl) emit them.
//
// The VU's largest value is 0x7FFFFFFF, a binade above single precision, so
// overflow cannot be read off a host Inf. Every threshold below is tested on a
// scaled copy instead, the scale an exact exponent shift; under the VU's
// round-toward-zero FPCR the scaled compare decides the unscaled one.
//
// Both predicates read the operands, before any clamp moves an exponent-255 one
// a binade down and before the arithmetic overwrites them.

// dst = all ones per lane where |a * b| is past the VU's largest value, i.e.
//
//     |a| * 2^-96  *  |b| * 2^-96  >=  2^-63
//
// Nothing on that path reaches the host's Inf/NaN range, so an exponent-255
// operand takes part as the number it is. A magnitude the Uqsub saturates was
// below 2^-30 and could not have overflowed against anything the VU can hold.
// The scaled product is at most 2^65, so the threshold is its top three bits.
//
// Clobbers `k` and `tmp`, reads `a` and `b`. Both operands are read before the
// constant is built, so `k` may be the register `b` came in -- VOPMULA's
// rotated pair needs that.
__fi static void armEmitVuMulOverflow(const a64::VRegister& dst, const a64::VRegister& a,
	const a64::VRegister& b, const a64::VRegister& k, const a64::VRegister& tmp)
{
	pxAssert(!tmp.Is(dst) && !k.Is(dst) && !k.Is(tmp) && !b.Is(dst));

	armAsm->Fabs(dst.V4S(), a.V4S());
	armAsm->Fabs(tmp.V4S(), b.V4S());
	armAsm->Movi(k.V4S(), 0x30, a64::LSL, 24); // 96 exponents
	armAsm->Uqsub(dst.V4S(), dst.V4S(), k.V4S());
	armAsm->Uqsub(tmp.V4S(), tmp.V4S(), k.V4S());
	armAsm->Fmul(dst.V4S(), dst.V4S(), tmp.V4S());
	armAsm->Ushr(dst.V4S(), dst.V4S(), 29);
	armAsm->Cmtst(dst.V4S(), dst.V4S(), dst.V4S());
}

// dst = all ones per lane where a zero product is an exact zero rather than a
// flushed underflow. armEmitPackSignZeroBits takes the complement as U.
//
// Under FZ a product of two non-zero operands is zero only when it underflowed,
// and host and console flush it to the same signed zero -- they differ in the
// flag alone. A denormal operand counts as zero, as FCMEQ against 0.0 makes it
// under FZ and as vuDouble makes it on the interpreter.
//
// Add and sub are not this: the console keeps the mantissa bits of a sum that
// underflows where the host flushes them, so their U sits behind a value
// divergence.
//
// `b` is read first because it may be dst (VOPMULA's rotated Ft).
__fi static void armEmitVuMulExactZero(const a64::VRegister& dst, const a64::VRegister& a,
	const a64::VRegister& b, const a64::VRegister& tmp)
{
	pxAssert(!a.Is(dst) && !a.Is(tmp) && !tmp.Is(dst));

	armAsm->Fcmeq(tmp.V4S(), b.V4S(), 0.0);
	armAsm->Fcmeq(dst.V4S(), a.V4S(), 0.0);
	armAsm->Orr(dst.V16B(), dst.V16B(), tmp.V16B());
}

// The word the console leaves in a lane that saturated: 0x7FFFFFFF with the
// result's own sign, a binade above the +/-FLT_MAX either emitter's result
// clamp can reach. Substituted from the O predicate rather than computed, so it
// makes that predicate an input to the VALUE and not only to the flags -- which
// is why both emitters build it at vuClampMode 3 whether or not anything reads
// MAC or STATUS.
//
// `dst` keeps its sign and stays non-zero, so a MAC pack downstream reads the
// same Z and S off it. `k` and `sat` are scratch and may be neither `dst` nor
// `overflow`.
__fi static void armEmitVuSaturateAtMax(const a64::VRegister& dst,
	const a64::VRegister& overflow, const a64::VRegister& k, const a64::VRegister& sat)
{
	armAsm->Mvni(k.V4S(), 0x80, a64::LSL, 24); // 0x7FFFFFFF
	armAsm->Sshr(sat.V4S(), dst.V4S(), 31);    // the sign, spread over the lane
	armAsm->Orr(sat.V16B(), sat.V16B(), k.V16B());
	armAsm->Bit(dst.V16B(), sat.V16B(), overflow.V16B());
}

// dst = all ones per lane where a +/- b is past the VU's largest value:
//
//     O  <=>  the two addends have the same sign, and (|a| + |b|) / 4 >= 2^127
//
// Opposite signs never overflow, the sum's magnitude being below the larger
// addend; a magnitude test alone gets that half wrong. A quarter is enough of a
// scale: the largest pair the VU can hold sums to twice its maximum, and a
// quarter of that is FLT_MAX exactly, so the scaled add cannot saturate and
// lose the answer.
//
// Adding the scale back turns the >= into the sign bit, so the threshold costs
// no second constant. Both answers then sit in bit 31 -- the sum's carried
// there by the Uqadd, the sign comparison's by the Eor -- so one Cmlt does for
// both.
//
// `t1`, `t2` and `k` are scratch; `t1` and `t2` may be `a` and `b` where the
// caller is done with them, which is why the sign comparison is taken first.
// `dst` must be none of the five.
__fi static void armEmitVuAddSubOverflow(const a64::VRegister& dst, const a64::VRegister& a,
	const a64::VRegister& b, bool issub, const a64::VRegister& t1, const a64::VRegister& t2,
	const a64::VRegister& k)
{
	pxAssert(!dst.Is(a) && !dst.Is(b) && !dst.Is(t1) && !dst.Is(t2) && !dst.Is(k));
	pxAssert(!t1.Is(t2) && !t1.Is(k) && !t2.Is(k));

	armAsm->Eor(dst.V16B(), a.V16B(), b.V16B());
	armAsm->Movi(k.V4S(), 0x01, a64::LSL, 24); // two exponents
	armAsm->Fabs(t1.V4S(), a.V4S());
	armAsm->Fabs(t2.V4S(), b.V4S());
	armAsm->Uqsub(t1.V4S(), t1.V4S(), k.V4S());
	armAsm->Uqsub(t2.V4S(), t2.V4S(), k.V4S());
	armAsm->Fadd(t1.V4S(), t1.V4S(), t2.V4S());
	armAsm->Uqadd(t1.V4S(), t1.V4S(), k.V4S());
	if (issub)
		armAsm->And(dst.V16B(), t1.V16B(), dst.V16B());
	else
		armAsm->Bic(dst.V16B(), t1.V16B(), dst.V16B());
	armAsm->Cmlt(dst.V4S(), dst.V4S(), 0);
}

// ========================================================================
//  The adder's guard bits
// ========================================================================
// The EE adder keeps one guard bit below its 24-bit significand: whichever
// operand the alignment shift moves right loses its low (|diff| - 1) mantissa
// bits, and past 24 it keeps nothing but its sign. fpuGuardMask (FPU.cpp) is
// the same rule for the EE FPU, and the VU's FMAC is that same adder.
//
// fpuEmitGuardedAddSub (iFPU-arm64.cpp) branches on the exponent difference.
// Four lanes can want different arms of it at once, so this one is branchless:
//
//     d    = |expa - expb|
//     keep = 25 > d        // all ones per lane while the mask still has bits
//     mask = ((keep << d) >> 1) | 0x80000000
//
// keep doubles as the shift's ones-source: on the >= 25 arm it is zero, the
// shift yields nothing, and the sign is all the operand has left. Shifting left
// by d and back right by one gives the (d - 1) the rule asks for without a
// second constant. Nothing clamps the shift amount -- USHL reads the low byte
// of each lane as a signed count, so a difference past 127 shifts right rather
// than left -- but keep is already zero past 24, and zero shifts either way to
// zero.
//
// Masking is what makes the sum exact, so under the VU's round-toward-zero FPCR
// one single-precision add of the masked pair is the interpreter's chopped
// exact sum.
//
// `outA` and `outB` receive the masked operands and `tmp` is clobbered. All
// three must be distinct and none may be `a` or `b`, which are read-only; the
// caller's destination may be either operand, both being consumed here before
// it is written.
__fi static void armEmitVuGuardMask(const a64::VRegister& outA, const a64::VRegister& outB,
	const a64::VRegister& a, const a64::VRegister& b, const a64::VRegister& tmp)
{
	pxAssert(!outA.Is(outB) && !outA.Is(tmp) && !outB.Is(tmp));
	for (const a64::VRegister& r : {outA, outB, tmp})
		pxAssert(!r.Is(a) && !r.Is(b));

	armAsm->Shl(tmp.V4S(), a.V4S(), 1); // drop the sign, keep exp + mantissa
	armAsm->Ushr(tmp.V4S(), tmp.V4S(), 24);
	armAsm->Shl(outB.V4S(), b.V4S(), 1);
	armAsm->Ushr(outB.V4S(), outB.V4S(), 24);

	armAsm->Cmhi(outA.V4S(), outB.V4S(), tmp.V4S()); // all ones where a is the smaller
	armAsm->Uabd(tmp.V4S(), tmp.V4S(), outB.V4S());
	armAsm->Movi(outB.V4S(), 25);
	armAsm->Cmhi(outB.V4S(), outB.V4S(), tmp.V4S()); // keep
	armAsm->Ushl(tmp.V4S(), outB.V4S(), tmp.V4S());
	armAsm->Ushr(tmp.V4S(), tmp.V4S(), 1);
	armAsm->Orr(tmp.V4S(), 0x80, 24);

	armAsm->Orr(outB.V16B(), tmp.V16B(), outA.V16B());
	armAsm->And(outB.V16B(), b.V16B(), outB.V16B());
	armAsm->Orn(outA.V16B(), tmp.V16B(), outA.V16B());
	armAsm->And(outA.V16B(), a.V16B(), outA.V16B());
}
