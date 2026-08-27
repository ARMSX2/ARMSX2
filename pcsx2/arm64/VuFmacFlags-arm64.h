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
// result's sign, a binade above the +/-FLT_MAX either emitter's result clamp
// can reach. It is substituted from the O predicate rather than computed, so
// that predicate feeds the value and not only the flags -- which is why both
// emitters build it whether or not anything reads MAC or STATUS.
//
// `sign` is the first addend for an add or a sub. ADD overflows only when the
// two signs agree and SUB only when they differ, so either way the magnitudes
// add and the sign they share is `a`'s. Taking it off the result loses it
// wherever the host arithmetic returned a NaN, which is every pair with an
// exponent-255 operand in it. A multiply passes `dst`, whose sign the host
// product already carries. Only bit 31 of a `sign` lane is read, so an operand
// register goes in whole.
//
// Two spellings, so that whichever input is late arrives one instruction from
// the answer: `overflow` is the late one for an add and `dst` for a multiply,
// where the sign is already in place and an all-ones lane shifted right by one
// is 0x7FFFFFFF for free.
//
// `dst` keeps that sign and stays non-zero, so a MAC pack downstream reads the
// same Z and S off it. `k` is scratch and may be neither `overflow` nor `sign`.
__fi static void armEmitVuSaturateAtMax(const a64::VRegister& dst,
	const a64::VRegister& overflow, const a64::VRegister& sign, const a64::VRegister& k)
{
	pxAssert(!k.Is(overflow) && !k.Is(sign));

	if (sign.GetCode() == dst.GetCode())
	{
		pxAssert(!k.Is(dst));
		armAsm->Ushr(k.V4S(), overflow.V4S(), 1);
		armAsm->Orr(dst.V16B(), dst.V16B(), k.V16B());
		return;
	}

	armAsm->Mvni(k.V4S(), 0x80, a64::LSL, 24); // 0x7FFFFFFF
	armAsm->Orr(k.V16B(), sign.V16B(), k.V16B());
	armAsm->Bit(dst.V16B(), k.V16B(), overflow.V16B());
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

// ========================================================================
//  The multiplier's one-ULP deficit
// ========================================================================
// The console's multiply array is not a correctly-rounding multiplier: it comes
// back one step closer to zero on a large fraction of operands, and which ones
// depends on ft alone wherever the exact product is representable in single.
// FPU.cpp's eeMulArray carries the law and the measurement behind it; the VU's
// multiplier is that multiplier, which is why VUops.cpp reaches it through
// EeFpuModel::Mul.
//
// Where the exact product is NOT representable the decision needs fs as well,
// so nothing built out of ft can reach it. iFPUd-arm64.cpp runs the array
// itself there, on a double product it keeps for the purpose. Four singles in a
// Q register have no such product to keep, and a relocated VF would take two
// registers where a relocated FPR takes one. FMLS stands in for it: `t = p;
// FMLS t, fs, ft` leaves the exact error of the rounding, and `t == 0` says the
// product was representable -- what the double product's low 29 bits say there,
// four lanes at a time.
//
// Three conjuncts, ANDed as lane masks and added: an all-ones lane added to the
// raw word is the decrement, and it steps toward zero at either sign.
//
//   * the product is exact, from the FMLS residue;
//   * ft fires the predicate;
//   * the product's exponent field is at least 48.
//
// The last is not eeMulRound's own guard, which is only that the decrement must
// not walk out of the normals. It is wider because the residue cannot be
// trusted below it: under FZ an error smaller than 2^-126 flushes to zero and
// reads as "exact", and the error of a product with exponent e is as small as
// 2^(e-47). 48 is where that stops, so it is the floor of the method, and
// products under 2^-79 keep the word the host gave them.
//
// vuClampMode 4 only, on both emitters. The mask on ft is not a cheaper rung of
// this on its own: the exactness test is what turns a property of ft into a
// property of the product, and a model without it claims 24337908 of the
// 33554432 rows of the four-significand mul.s sweep where the console is low on
// 8299538 -- a worse answer than the plain FMUL it would replace.

// Builds the ft half of the decrement condition into `dst`, as an all-ones
// lane mask. `tmp` is a second scratch. Neither `dst` nor `tmp` may be `b`.
__fi static void armEmitVuMulDeficitPredicate(const a64::VRegister& dst,
	const a64::VRegister& b, const a64::VRegister& tmp)
{
	pxAssert(!dst.Is(b) && !tmp.Is(b) && !dst.Is(tmp));

	// Bit 11 against a boundary term on bits 12..15. iFPUd-arm64.cpp spells that
	// term b15 & ~(b14 & b13), then ^ b11, which wants an XOR against a shifted
	// operand; NEON has no such form, so the same bit comes out of
	// (b ^ (b << 4)) ^ (b13 & b14 & b15).
	armAsm->Shl(dst.V4S(), b.V4S(), 1);
	armAsm->And(dst.V16B(), dst.V16B(), b.V16B());
	armAsm->Shl(dst.V4S(), dst.V4S(), 1);
	armAsm->And(dst.V16B(), dst.V16B(), b.V16B()); // bit 15 = b13 & b14 & b15

	armAsm->Shl(tmp.V4S(), b.V4S(), 4);
	armAsm->Eor(tmp.V16B(), tmp.V16B(), b.V16B()); // bit 15 = b15 ^ b11
	armAsm->Eor(tmp.V16B(), tmp.V16B(), dst.V16B());
	armAsm->Shl(tmp.V4S(), tmp.V4S(), 16);
	armAsm->Ushr(tmp.V4S(), tmp.V4S(), 31); // the boundary term, as 0 or 1

	armAsm->Movi(dst.V4S(), 0x02, a64::LSL, 8);
	armAsm->Orr(dst.V4S(), 0xAA); // mantissa bits 1,3,5,7,9
	armAsm->And(dst.V16B(), dst.V16B(), b.V16B());
	armAsm->Orr(dst.V16B(), dst.V16B(), tmp.V16B());
	armAsm->Cmtst(dst.V4S(), dst.V4S(), dst.V4S());
}

// `dst = a * b`, decremented where the array comes up short. `b` is the recoded
// operand, the one the predicate reads -- the operation is not commutative.
//
// `t` and `u` are clobbered and must be distinct from each other and from all
// three of dst/a/b. ft's half is built first, while both are still free. `dst`
// may be `a` or `b`; the product is then computed twice rather than parked,
// which is the same one instruction either way.
//
// `scalar` is microVU's single-lane form. Only the two multiplies narrow: a
// scalar FMUL zeroes the lanes above it, so the mask the rest of this builds
// comes out zero there and adds nothing.
//
// `floorBlocked` is optional. When a caller passes a register, it is set to a
// non-zero value if any lane satisfied the first two conditions but failed the
// exponent test. Those are the lanes whose FMLS residue flushed to zero, so
// the exactness test could not distinguish an exact product from a rounded
// one. eeMulOneUlpLow recomputes the mantissa product and can, so a caller
// that is able to call it should use this to select those lanes.
__fi static void armEmitVuDefectiveMul(const a64::VRegister& dst, const a64::VRegister& a,
	const a64::VRegister& b, const a64::VRegister& t, const a64::VRegister& u,
	bool scalar = false, const a64::Register* floorBlocked = nullptr)
{
	for (const a64::VRegister& r : {t, u})
		pxAssert(!r.Is(dst) && !r.Is(a) && !r.Is(b));
	pxAssert(!t.Is(u));

	armEmitVuMulDeficitPredicate(u, b, t);

	const auto product = [&](const a64::VRegister& into) {
		if (scalar)
			armAsm->Fmul(into.S(), a.S(), b.S());
		else
			armAsm->Fmul(into.V4S(), a.V4S(), b.V4S());
	};

	const bool aliased = dst.Is(a) || dst.Is(b);
	if (aliased)
	{
		product(t);
	}
	else
	{
		product(dst);
		armAsm->Mov(t.V16B(), dst.V16B());
	}
	armAsm->Fmls(t.V4S(), a.V4S(), b.V4S()); // the rounding's exact error
	armAsm->Fcmeq(t.V4S(), t.V4S(), 0.0);
	armAsm->And(t.V16B(), t.V16B(), u.V16B());

	if (aliased)
		product(dst);

	// The exponent floor, carried inside the mask rather than beside it: a
	// masked-off lane comes out zero, which is below the floor.
	armAsm->And(t.V16B(), t.V16B(), dst.V16B());
	armAsm->Shl(t.V4S(), t.V4S(), 1); // drop the sign
	armAsm->Ushr(t.V4S(), t.V4S(), 24);
	armAsm->Movi(u.V4S(), 48);

	if (!floorBlocked)
	{
		armAsm->Cmhs(t.V4S(), t.V4S(), u.V4S());
		armAsm->Add(dst.V4S(), dst.V4S(), t.V4S());
		return;
	}

	// The comparison writes u rather than t so that t keeps the exponents: at
	// this point t holds the product's biased exponent on lanes that satisfied
	// the first two conditions and zero elsewhere. Clearing the lanes u accepted
	// therefore leaves a non-zero value only where the exponent test was what
	// rejected the lane.
	armAsm->Cmhs(u.V4S(), t.V4S(), u.V4S());
	armAsm->Add(dst.V4S(), dst.V4S(), u.V4S());
	armAsm->Bic(t.V16B(), t.V16B(), u.V16B());
	armAsm->Umaxv(t.S(), t.V4S());
	armAsm->Fmov(*floorBlocked, t.S());
}
