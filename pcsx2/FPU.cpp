// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Config.h"

#include "common/FPControl.h"

#include <cmath>
#include <cstring>

// Helper Macros
//****************************************************************

// IEEE 754 Values
#define PosInfinity 0x7f800000
#define NegInfinity 0xff800000
#define posFmax 0x7F7FFFFF
#define negFmax 0xFF7FFFFF


/*	Used in compare function to compensate for differences between IEEE 754 and the FPU.
	Setting it to ~0x00000000 = Compares Exact Value. (comment out this macro for faster Exact Compare method)
	Setting it to ~0x00000001 = Discards the least significant bit when comparing.
	Setting it to ~0x00000003 = Discards the least 2 significant bits when comparing... etc..  */
//#define comparePrecision ~0x00000001

// Operands
#define _Ft_         ( ( cpuRegs.code >> 16 ) & 0x1F )
#define _Fs_         ( ( cpuRegs.code >> 11 ) & 0x1F )
#define _Fd_         ( ( cpuRegs.code >>  6 ) & 0x1F )

// Floats
#define _FtValf_     fpuRegs.fpr[ _Ft_ ].f
#define _FsValf_     fpuRegs.fpr[ _Fs_ ].f
#define _FdValf_     fpuRegs.fpr[ _Fd_ ].f
#define _FAValf_     fpuRegs.ACC.f

// U32's
#define _FtValUl_    fpuRegs.fpr[ _Ft_ ].UL
#define _FsValUl_    fpuRegs.fpr[ _Fs_ ].UL
#define _FdValUl_    fpuRegs.fpr[ _Fd_ ].UL
#define _FAValUl_    fpuRegs.ACC.UL

// S32's - useful for ensuring sign extension when needed.
#define _FtValSl_    fpuRegs.fpr[ _Ft_ ].SL
#define _FsValSl_    fpuRegs.fpr[ _Fs_ ].SL
#define _FdValSl_    fpuRegs.fpr[ _Fd_ ].SL
#define _FAValSl_    fpuRegs.ACC.SL

// FPU Control Reg (FCR31)
#define _ContVal_    fpuRegs.fprc[ 31 ]

// FCR31 Flags
#define FPUflagC	0X00800000
#define FPUflagI	0X00020000
#define FPUflagD	0X00010000
#define FPUflagO	0X00008000
#define FPUflagU	0X00004000
#define FPUflagSI	0X00000040
#define FPUflagSD	0X00000020
#define FPUflagSO	0X00000010
#define FPUflagSU	0X00000008

//****************************************************************

/*	The EE value of a raw FPR word, exactly, as a double.

	The only way an operand enters this file: every arithmetic op, every flag
	decision and every compare reads through here. Here 0x7F800000 is 2^128, an
	ordinary number, and 0x7FFFFFFF is the largest one; a double holds the whole
	EE range, so nothing is rewritten on the way in.

	fpuDouble()/fpuOperandBits(), which this replaced, folded exponent 255 to
	+-0x7F7FFFFF to fit a host single, so the op ran on a different operand than
	the one it was given; 368 of the 1147 captured cases touch the top binade.
	The arithmetic stopped reading them in ccae642180 and 4ce2b543cb, the
	compares last.

	Denormal operands flush to signed zero. The EE has none, and U is raised only
	when the result computed from the flushed operands is nonzero and below the
	smallest normal -- "mul 1.0, MIN_DENORM" returns +0 with FCR31 untouched
	(autocases_fpuovf.h).
*/
static double eeToDouble(u32 f)
{
	const u32 exp = (f >> 23) & 0xFF;
	u64 bits = static_cast<u64>(f & 0x80000000u) << 32;
	if (exp != 0)
	{
		bits |= (static_cast<u64>(exp) + (1023 - 127)) << 52;
		bits |= static_cast<u64>(f & 0x007FFFFFu) << 29;
	}
	double d;
	std::memcpy(&d, &bits, sizeof(d));
	return d;
}

/*	The ends of the EE's representable range.
	  0x7FFFFFFF == (2 - 2^-23) * 2^128, one binade above IEEE single's max,
	  because exponent 255 is an ordinary exponent on this FPU.
	  0x00800000 == 2^-126, the smallest normal; there is nothing below it.
*/
static constexpr double kEeFpuMax = 0x1.fffffep+128;
static constexpr double kEeMinNormal = 0x1p-126;

/*	Fold a result the host produced into something the EE could hold: an
	infinity to +/-fMax, a denormal to signed zero. This is what
	checkOverflow()/checkUnderflow() did to the value, unchanged; the +/-FLT_MAX
	saturation compromise in it is pinned by
	EeFpuOverflowConsole.DefaultClampModeSaturatesToFltMaxOnBothEngines.

	DIV.S, SQRT.S and RSQRT.S call this and no flag helper: they touch I and D
	but must leave O and U as they found them.
*/
static void clampToEeRange(u32& xReg)
{
	if ((xReg & ~0x80000000) == PosInfinity)
		xReg = (xReg & 0x80000000) | posFmax;
	else if (((xReg & 0x7F800000) == 0) && ((xReg & 0x007FFFFF) != 0))
		xReg &= 0x80000000;
}

/*	One rounding step's worth of FCR31 O/U maintenance, from the magnitude of
	the exact result. `exact` is the step's result recomputed through
	eeToDouble(), where nothing was clamped, rounded away or flushed.

	checkOverflow()/checkUnderflow() used to ask instead whether xReg had come
	back as a host infinity or a host denormal. Neither ever appears in the FP
	environment the EE actually runs in: rounding toward zero makes an
	overflowing multiply saturate to FLT_MAX, and FZ flushes an underflowing one
	to zero. Both are the shipping default (Pcsx2Config.cpp), so O and U were
	raised only under a rounding mode no game selects.

	Both causes are cleared before either is set, so an overflow clears U, which
	the old early-return structure did not: silicon returns O|SO|SU, U clear,
	for MUL.S of +FLT_MAX by itself with U preset.

	This rule and the two-step rule below reproduce O/U/SO/SU on all 674
	arithmetic cases of the FP matrix corpus's console column.
*/
static void raiseOrClearOU(double exact)
{
	_ContVal_ &= ~(FPUflagO | FPUflagU);
	if (std::fabs(exact) > kEeFpuMax)
		_ContVal_ |= FPUflagO | FPUflagSO;
	else if (exact != 0.0 && std::fabs(exact) < kEeMinNormal)
		_ContVal_ |= FPUflagU | FPUflagSU;
}

/*	The multiply-accumulates round twice, so they raise twice: once on the
	intermediate product, once on the accumulate. These two predicates are what
	the product hands on to the second step.

	An underflowing product is flushed to signed zero before the accumulate, so
	the accumulate sees ACC and clears the cause U again, leaving the sticky SU
	up. 68 cases in the capture come back with SU set and U clear; all are
	multiply-accumulates and no plain MUL/MULA ever does, which is what says two
	steps rather than one.

	An overflowing product ends the instruction. Silicon saturates there and the
	accumulate cannot bring it back: MADD of 2^128 by 2.0 onto an ACC of -2^128
	returns +0x7FFFFFFF with O|SO, not the 2^128 the arithmetic says.
	eeMulAccumulate() applies the same test to the value. The fast path has no
	such test -- recMADD_S_xmm accumulates the raw product in the default clamp
	mode -- so that corner is an engine divergence by design.
*/
static bool madAccumulandOverflowed(double product)
{
	return std::fabs(product) > kEeFpuMax;
}

static double madFlushedProduct(double product)
{
	if (product != 0.0 && std::fabs(product) < kEeMinNormal)
		return std::copysign(0.0, product);
	return product;
}

__fi u32 fp_max(u32 a, u32 b)
{
	return ((s32)a < 0 && (s32)b < 0) ? std::min<s32>(a, b) : std::max<s32>(a, b);
}

__fi u32 fp_min(u32 a, u32 b)
{
	return ((s32)a < 0 && (s32)b < 0) ? std::max<s32>(a, b) : std::min<s32>(a, b);
}

/*	Checks if Divide by Zero will occur. (z/y = x)
	cFlagsToSet1 = Flags to set if (z != 0)
	cFlagsToSet2 = Flags to set if (z == 0)
	( Denormals are counted as "0" )
*/
bool checkDivideByZero(u32& xReg, u32 yDivisorReg, u32 zDividendReg, u32 cFlagsToSet1, u32 cFlagsToSet2) {

	if ( (yDivisorReg & 0x7F800000) == 0 ) {
		_ContVal_ |= ( (zDividendReg & 0x7F800000) == 0 ) ? cFlagsToSet2 : cFlagsToSet1;
		// Rows 38-43 of the overflow capture are all divide-by-zero: all
		// 0x7FFFFFFF, the EE's maximum, and all signed with the xor of the two
		// operands.
		xReg = ( (yDivisorReg ^ zDividendReg) & 0x80000000 ) | 0x7FFFFFFF;
		return true;
	}

	return false;
}

/*	Clears the "Cause Flags" of the Control/Status Reg
	The "EE Core Users Manual" implies that all the Cause flags are cleared every instruction...
	But, the "EE Core Instruction Set Manual" says that only certain Cause Flags are cleared
	for specific instructions... I'm just setting them to clear when the Instruction Set Manual
	says to... (cottonvibes)
*/
#define clearFPUFlags(cFlags) {  \
	_ContVal_ &= ~( cFlags ) ;  \
}

#ifdef comparePrecision
// This compare discards the least-significant bit(s) in order to solve some rounding issues.
	#define C_cond_S(cond) {  \
		FPRreg tempA, tempB;  \
		tempA.UL = _FsValUl_ & comparePrecision;  \
		tempB.UL = _FtValUl_ & comparePrecision;  \
		_ContVal_ = ( ( tempA.f ) cond ( tempB.f ) ) ?  \
					( _ContVal_ | FPUflagC ) :  \
					( _ContVal_ & ~FPUflagC );  \
	}
#else
/*	Used for Comparing; This compares if the floats are exactly the same.

	In doubles, which hold every EE value exactly. Host singles cannot be used
	here: 0x7FFFFFFF is the EE's largest number and the same bits are a NaN to
	the host, unordered against everything.

	Both operands used to come through fpuDouble(), whose fold collapsed the
	whole top binade onto one value: every operand from 0x7F800000 up compared
	equal to 0x7F7FFFFF and less than nothing. Console rows 432/434 (c.eq.s of
	0x7F7FFFFF and 0x7F800000 against 0x7FFFFFFF, both false on silicon) and
	452/454 (the c.lt.s of the same pairs, both true) are the four the clamp
	lost. */
	#define C_cond_S(cond) {  \
	   _ContVal_ = ( eeToDouble(_FsValUl_) cond eeToDouble(_FtValUl_) ) ?  \
				   ( _ContVal_ | FPUflagC ) :  \
				   ( _ContVal_ & ~FPUflagC );  \
	}
#endif

// Conditional Branch
#define BC1(cond)                               \
   if ( ( _ContVal_ & FPUflagC ) cond 0 ) {   \
      intDoBranch( _BranchTarget_ );            \
   }

// Conditional Branch
#define BC1L(cond)                              \
   if ( ( _ContVal_ & FPUflagC ) cond 0 ) {   \
      intDoBranch( _BranchTarget_ );            \
   } else cpuRegs.pc += 4;

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {
namespace COP1 {

//****************************************************************
// FPU Opcodes
//****************************************************************

/*	fpuDouble() and fpuOperandBits() lived here.

	They were the operand model: exponent 0 to signed zero, which is right,
	this FPU has no denormals; and exponent 255 folded to +-0x7F7FFFFF, which
	was the largest single source of divergence from the console in the whole
	capture. On the EE exponent 255 is an ordinary binade: 0x7F800000 is 2^128
	and 0x7FFFFFFF is the largest number there is. Folding those operands
	destroyed information before the op ever ran; 368 of the 1147 captured cases
	touch the top binade.

	The clamp came off the arithmetic first (ccae642180, 4ce2b543cb), which left
	the compares as its last user on the grounds that clamped and unclamped
	compares agree except where both operands are in the top binade. One is
	enough: it only has to collide with an unclamped 0x7F7FFFFF, and four
	captured rows do. See C_cond_S above. The arithmetic reads eeToDouble(),
	which clamps nothing, and rounds once through eeRoundToSingle().

	SQRT_S was the other caller with work to do on exponent 255, and does it
	itself now: eeSqrtBits() works in integers, so nothing has to be clamped to
	keep the operand in a host single. */

/*	Round an exact result into the EE encoding. This is the only rounding step
	the arithmetic ops below perform.

	Three things a bare (float) cast does not do:

	  * Saturation is at the EE's maximum, 0x7FFFFFFF, not at FLT_MAX. On
	    silicon, add.s of +2^128 to itself is 2^129, out of range, and comes
	    back 0x7FFFFFFF; add.s of 0x7F7FFFFF to itself is exactly 0x7FFFFFFF and
	    comes back unrounded. Folding either to 0x7F7FFFFF, which is what
	    clampToEeRange did to a host infinity, is a whole binade short.
	  * The top binade has no host single. Anything at or above 2^126 is rounded
	    through a scaled-down copy and its exponent put back afterwards. Scaling
	    by a power of two is exact and leaves the mantissa alone, so the rounding
	    decision is bit-identical to the one the host would have made if float
	    had the range -- the same reason SQRT.S can compute sqrt(|Ft|/4)*2.
	  * Underflowing results are not all flushed; `addsub` picks the rule. See
	    the block below.

	The (float) casts round under the host FPCR, so the EE's rounding mode is
	honoured here without naming it -- including the divide/sqrt unit's separate
	mode, which its callers scope in.
*/
/*	Underflow: a result strictly below 2^-126 and not zero is not always
	flushed. The add/sub family leaves the mantissa bits where normalisation put
	them and forces the exponent field to 0; MUL and DIV clear them and return
	signed zero. That is the raw output of an adder with no denormal path:
	`exact` is a double 1.m * 2^E, and what comes out is m's top 23 bits, i.e.
	bits [51:29] of the double, with the exponent thrown away. It is not the
	arithmetic answer, only its bits, and no rounding mode produces it.

	The console rows this reproduces, how they were sampled and how they rule
	out flushing and the true denormal value, are in
	tests/ctest/core/recompilers/ee_fpu_underflow_console_tests.cpp.
*/
static u32 eeRoundToSingle(double exact, bool addsub = false)
{
	const double mag = std::fabs(exact);

	if (mag > kEeFpuMax)
		return (std::signbit(exact) ? 0x80000000u : 0u) | 0x7FFFFFFFu;

	if (mag < kEeMinNormal)
	{
		/*	Ahead of the (float) cast, so the answer does not depend on the
			ambient FPCR having FZ set and nothing can round up out of the
			region -- the console returns +0 for a product of 2^-126 - 2^-150,
			which is nearer 2^-126 than to zero. */
		const u32 sign = std::signbit(exact) ? 0x80000000u : 0u;
		if (!addsub || exact == 0.0)
			return sign;

		u64 bits;
		std::memcpy(&bits, &exact, sizeof(bits));
		return sign | static_cast<u32>((bits >> 29) & 0x7FFFFFu);
	}

	FPRreg r;
	if (mag >= 0x1p126)
	{
		/*	Scale down by 2^4, round there, then add the 4 exponents back. The
			scaled exponent field is at most 251, so the +4 cannot carry into
			the sign, and the >= 2^126 floor keeps the scaled value normal, so
			nothing is flushed on the way through. */
		r.f = static_cast<float>(exact * 0x1p-4);
		r.UL += 4u << 23;
		return r.UL;
	}

	r.f = static_cast<float>(exact);
	return r.UL;
}

/*	The EE FPU's adder carries no guard bits to the right of the mantissa. A
	compliant adder shifts the smaller operand right into extra bits it keeps for
	the rounding decision; whatever shifts past this one's mantissa is gone.
	Subtraction -- and addition of unlike signs -- can then renormalise left and
	pull the hole up into the result, landing one ULP toward zero from the IEEE
	answer:

	    sub.s  0x00800000, 0x3F000000  ->  console BF000000, plain IEEE BEFFFFFF

	The model is the exponent difference, which is how far the smaller operand
	gets shifted: it loses its low (|diff| - 1) mantissa bits, and past 24 it has
	nothing left but its sign. |diff| <= 1 masks nothing.

	Ported from x86 FPU_ADD_SUB (x86/iFPU.cpp) and, on arm64, fpuEmitGuardedAddSub
	(iFPU-arm64.cpp, the single-precision fast path) and FPU_ADD_SUB
	(iFPUd-arm64.cpp, the Full-clamp DOUBLE path).

	Both recompilers gate the masking on CHECK_FPU_GUARDED, the fpuGuardedAddSub
	INI bool, so an EE-FPU-heavy title can buy back one op per ADD.S/SUB.S. The
	interpreter does not read it: its target is the console, not the recompiler's
	speed. With fpuGuardedAddSub=false the engines therefore disagree on exactly
	these cases, which EeRecFpuGuardBit.GuardOffDivergesFromInterpreterByDesign
	pins.

	The console rows this reproduces, with their corpus ordinals, are tabulated in
	tests/ctest/core/recompilers/ee_fpu_guarded_addsub_console_tests.cpp.
*/
static void fpuGuardMask(u32& a, u32& b)
{
	const s32 diff = (s32)((a >> 23) & 0xFF) - (s32)((b >> 23) & 0xFF);

	if (diff >= 25)
		b &= 0x80000000;
	else if (diff >= 2)
		b &= 0xffffffffu << (diff - 1);
	else if (diff <= -25)
		a &= 0x80000000;
	else if (diff <= -2)
		a &= 0xffffffffu << (-diff - 1);
}

/*	The EE's adder: mask the guard bits away, add exactly, round once.

	The mask is what makes the add exact. Within 24 exponents the sum needs 48
	bits of the double's 53; beyond that the mask has already reduced the
	smaller operand to +-0. So eeRoundToSingle() below is the only rounding, as
	on the hardware.

	Subtraction is addition of the negated operand, as IEEE defines it: that gets
	the zero signs right, including for a masked +-0. */
static u32 eeGuardedAddSub(u32 a, u32 b, bool issub)
{
	fpuGuardMask(a, b);
	if (issub)
		b ^= 0x80000000;
	return eeRoundToSingle(eeToDouble(a) + eeToDouble(b), true);
}

/*	The EE's divide/square-root unit, digit by digit.

	It is not a correctly rounding divider, and no rounding mode makes it one.
	The exact quotient lies between two singles and silicon returns one of them,
	but which one is decided by where a digit recurrence lands, not by how far the
	exact value sits from either end. So the error reaches nearly a whole ULP in
	both directions on the same divisor, and two operand pairs agreeing in every
	coordinate a rounding rule can see -- branch, divisor, remainder, the exact
	fraction -- can still go opposite ways.

	It is a radix-2 SRT digit recurrence over the signed digit set {-1, 0, +1},
	partial remainder carried in redundant carry-save form, 24 selections
	producing 25 digits, and no rounding step anywhere in it: no round bit, no
	sticky, no final correction. SQRT.S is the same recurrence with the root so
	far fed back in place of the divisor, and RSQRT.S is SQRT.S followed by DIV.S
	with an ordinary 24-bit single in between, which is what silicon does.

	The model is the selection function eeSrtDigit() below.

	  * DIV.S -- 150,994,944 rows. Eighteen divisors swept exhaustively, every
	    one of the 2^23 numerator significands at each, both branches, including
	    the seven degenerate divisors that broke every earlier frame. Zero
	    result-word disagreements, and zero rows where the recurrence lands
	    outside {T, T+1}. Through the same scorer, truncation gets 52,434,906 of
	    those rows wrong and round-to-nearest 29,430,553.
	  * SQRT.S -- 16,777,216 rows, every significand at both exponent parities.
	    Zero disagreements; truncation 3,994,228 and round-to-nearest 4,395,034.
	  * A 3000-operand holdout (seed 0x243F6A88, captured before any of this was
	    modelled): 2000 div and 1000 sqrt, zero disagreements. 229 of the div
	    rows saturate or flush, which the exhaustive sweeps cannot see: they hold
	    both exponents at 127.

	Nothing else moves the result. FCR31 has no rounding-mode field on this FPU
	-- bit 0 reads 1 whatever is written and bit 1 never sticks -- and the answer
	is unchanged when the preceding instruction is varied or the case list
	reversed. What that leaves EmuConfig.Cpu.FPUDivFPCR to do is in the block
	below eeSqrtBits().

	This is PS2Float.cpp's Div() and Sqrt() from the proposed PCSX2 soft-float
	series (GitHubProUser67), whose only documentation is M. Prabhu and
	G. Zyner, "167 MHz radix-8 divide and square root using overlapped
	radix-2 stages", DOI 10.1109/ARITH.1995.465363.
*/

/*	The partial remainder, in the redundant form the recurrence carries it in.
	No step propagates a carry across the width of the operand, which is why the
	selector below cannot see the true remainder. */
struct EeSrtRemainder
{
	u32 sum, carry;
};

/*	The digit, in the form the recurrence consumes it: one all-ones mask per
	sign, both zero for the digit 0. Both set never happens.

	Every use of it inside the step is a select. As -1/0/+1 those are two
	data-dependent branches per step, on a digit sequence nothing predicts, and
	the mispredicts cost more than the step does. */
struct EeSrtDigitMask
{
	u32 plus, minus;
};

/*	Back to -1/0/+1, for the quotient and the root. Nothing the next digit
	depends on reads it. */
static __fi u32 eeSrtDigitValue(EeSrtDigitMask d)
{
	return d.minus - d.plus;
}

static __fi EeSrtRemainder eeSrtCarrySave(u32 a, u32 b, u32 c)
{
	const u32 u = a ^ b;
	const u32 h = (a & b) | (u & c);
	return {u ^ c, h << 1};
}

/*	The selection function: which of -1, 0, +1 the next digit takes.

	It assimilates the redundant remainder only partially -- the carry word is
	added in above bit 23 while the low 24 bits of the sum are OR-ed back rather
	than added -- so it decides on something other than the remainder, and picks a
	different digit from the one an exact comparison would. The thresholds are
	+2^23 and -2^24, with the binary point between bits 24 and 25; that asymmetry
	is what biases the unit toward truncation. The digit set is redundant, so the
	last digit can still be -1 and the result reach T+1 on rows a truncation could
	never reach.

	Its two comparisons are already the masks the next step selects with, so they
	are what it returns. */
static __fi EeSrtDigitMask eeSrtDigit(EeSrtRemainder r)
{
	constexpr u32 mask = (1u << 24) - 1u;
	const s32 estimate = (s32)(((r.sum & ~mask) + r.carry) | (r.sum & mask));
	return {(u32)0 - (u32)(estimate >= (1 << 23)),
			(u32)0 - (u32)(estimate < (s32)(~0u << 24))};
}

/*	On a zero digit the next digit is selected from the un-recompressed pair
	while the state advances with the recompressed one, so selection and state
	see different splittings of the same value. Drop the distinction and the
	model stops reproducing silicon. */
static __fi EeSrtRemainder eeSrtSelect(EeSrtRemainder cur, EeSrtRemainder next, EeSrtDigitMask d)
{
	const u32 m = d.plus | d.minus;
	return {(cur.sum & ~m) | (next.sum & m), (cur.carry & ~m) | (next.carry & m)};
}

/*	The digits the unit does not have to run.

	The recurrence returns T or T+1, T being the truncated quotient of the two
	significands, and on a large share of operands the remainder alone says
	which:

	    lt  = ma < mb              does the quotient need a shift
	    num = ma << (23 + lt)
	    T   = num / mb             the truncated 24-bit significand
	    rem = num - T*mb           0 <= rem < mb
	    u   = mb - rem             how far the exact quotient sits below T+1

	    u > cap  =>  T       cap = 2^22 on A>=B, max(2^23, mb-2^22) on A<B

	Square root is the same shape with the root in place of the quotient: with X
	the placed radicand, R = floor(sqrt(X)) and rem = X - R*R, u is 2R + 1 - rem
	and the cap is 2^23. So one integer division answers 44% of arbitrary
	divides and one integer square root 65% of square roots without a digit
	being run; the rest fall through to the recurrence.

	The caps came out of console captures, not out of the recurrence, so this is
	only ever a shortcut: what it returns has to be what the recurrence would
	have returned. The implication is one-way -- rows below the cap are not
	settled and must fall through -- so a narrowed cap costs only digits, while a
	widened one can change results. EeFpuDivUnitExhaustive and
	EeFpuDivUnitConsole hold the rows it fires on against the console. */
static __fi u32 eeDivideCap(u32 mb, u32 lt)
{
	return lt ? ((mb > (3u << 22)) ? mb - (1u << 22) : (1u << 23)) : (1u << 22);
}

/*	The quotient of two 24-bit significands as 25 digits, weights 2^24 down to
	2^0. A positive digit subtracts the divisor as ~divisor with the +1 fed into
	the carry word, which the selector then sees -- that is one of the places
	the estimate and the state come apart. The value returned is 25 bits when
	sma >= smb and 24 bits when it is not; the caller normalises, and the digit
	that falls off the bottom there is simply dropped. */
static u32 eeDivideSignificand(u32 sma, u32 smb)
{
	{
		const u32 lt = (sma < smb) ? 1u : 0u;
		const u64 num = (u64)sma << (23 + lt);
		const u32 T = (u32)(num / smb);
		const u32 rem = (u32)(num - (u64)T * smb);
		if ((smb - rem) > eeDivideCap(smb, lt))
		{
			// Only the 24 bits the caller keeps are the answer. On A>=B the
			// recurrence's own last digit is as often 1 as 0 and is dropped
			// there, so this arm supplies a 0 for it rather than reproducing
			// it -- do not compare the two below that bit.
			return lt ? T : (T << 1);
		}
	}

	const u32 divisor = smb << 2;
	const u32 ndivisor = ~divisor;
	EeSrtRemainder rem = {sma << 2, 0};
	u32 quotient = 0;
	EeSrtDigitMask digit = {~0u, 0}; // +1

	for (int i = 0; i < 24; ++i)
	{
		quotient = (quotient << 1) + eeSrtDigitValue(digit);
		const u32 addend = (ndivisor & digit.plus) | (divisor & digit.minus);
		// subtracting the mask is the +1 that goes with ~divisor
		const EeSrtRemainder cur = {rem.sum, rem.carry - digit.plus};
		const EeSrtRemainder next = eeSrtCarrySave(cur.sum, cur.carry, addend);
		digit = eeSrtDigit(eeSrtSelect(cur, next, digit));
		rem.sum = next.sum << 1;
		rem.carry = next.carry << 1;
	}
	return (quotient << 1) + eeSrtDigitValue(digit);
}

static u32 eeDivide(u32 a, u32 b)
{
	const s32 ea = (s32)((a >> 23) & 0xFF);
	const s32 eb = (s32)((b >> 23) & 0xFF);
	const u32 sign = (a ^ b) & 0x80000000u;

	if (ea == 0)
		return sign; // zero dividend (denormals are zero), sign from both operands

	// Exponent 255 is an ordinary binade on this FPU, so every finite operand
	// reaches here and the hidden bit is always present. The divisor is already
	// known nonzero: that is a flag question the callers answer first.
	u32 quotient = eeDivideSignificand(0x800000u | (a & 0x7FFFFFu), 0x800000u | (b & 0x7FFFFFu));
	s32 e = ea - eb + 126;
	if (quotient >= (1u << 24))
	{
		quotient >>= 1;
		++e;
	}

	// No carry out of the significand is possible below this point -- there is
	// no rounding step left that could walk the quotient into the next binade.
	if (e > 255)
		return sign | 0x7FFFFFFFu; // the EE's maximum, not FLT_MAX
	if (e < 1)
		return sign; // the EE has no denormals to underflow into
	return sign | ((u32)e << 23) | (quotient & 0x7FFFFFu);
}

/*	sqrt(|Ft|) as EE bits, including the top binade.

	The same recurrence as eeDivideSignificand(), with the root so far fed back
	in place of a fixed divisor: adding digit d at weight w to the root adds
	d*(2*root + d*w) to its square, which is what has to leave the partial
	remainder, so the addend is rebuilt each step instead of being a constant.
	Neither candidate depends on which digit arrives, so both the addends and
	the new roots are built before it does; a zero digit takes neither root.
	Everything else -- the carry-save state, the selector, the zero-digit quirk,
	the absence of any rounding step -- is shared, and so is the evidence: see
	the block comment above eeSrtDigit().

	The radicand is placed so the root is a 24-bit integer. With E the exponent
	field, the significand's hidden bit restored, and the result's exponent
	(E + 127) / 2 rounded down, the operand's own exponent parity decides how far
	to shift: one place when E is odd, two when it is even. The top binade needs
	no special case: in integers it is just another odd E.
*/
/*	floor(sqrt(x)) for x below 2^48, exactly. The double is a seed only: x
	converts to it exactly and its square root is correctly rounded, so the true
	root is within one of the truncation under any host rounding mode, and the
	two corrections run unconditionally. */
static u32 eeISqrt48(u64 x)
{
	u64 r = (u64)std::sqrt((double)x);
	while (r > 0 && r * r > x)
		--r;
	while ((r + 1) * (r + 1) <= x)
		++r;
	return (u32)r;
}

static u32 eeSqrtSignificand(u32 m)
{
	{
		// The radicand in the law's frame, which places it 22 bits further up
		// than the recurrence does. See the comment above eeDivideCap().
		const u64 x = (u64)m << 22;
		const u32 root = eeISqrt48(x);
		if ((2ull * root + 1ull - (x - (u64)root * root)) > (1u << 23))
			return root;
	}

	EeSrtRemainder rem = {m, 0};
	u32 root = 0;
	EeSrtDigitMask digit = {~0u, 0}; // +1

	for (int i = 0; i < 24; ++i)
	{
		const u32 w = 1u << (24 - i);
		const u32 base_plus = root + w, base_minus = root - w;
		const u32 root_plus = base_plus + w, root_minus = base_minus - w;
		const u32 addend = (~base_plus & digit.plus) | (base_minus & digit.minus);
		const u32 any = digit.plus | digit.minus;
		root = (root_plus & digit.plus) | (root_minus & digit.minus) | (root & ~any);
		const EeSrtRemainder cur = {rem.sum, rem.carry - digit.plus};
		const EeSrtRemainder next = eeSrtCarrySave(cur.sum, cur.carry, addend);
		digit = eeSrtDigit(eeSrtSelect(cur, next, digit));
		rem.sum = next.sum << 1;
		rem.carry = next.carry << 1;
	}
	// The last digit carries weight 2^1, below the root's least significant
	// bit, so it only reaches the result by borrowing out of it.
	root += eeSrtDigitValue(digit) << 1;
	return (root >> 2) & 0xFFFFFFu;
}

static u32 eeSqrtBits(u32 t)
{
	const u32 E = (t >> 23) & 0xFFu;
	if (E == 0)
		return 0; // +/-0 and the denormals: the EE drops the sign here, and so
		          // do both recompilers (they take |Ft| first). See
		          // EeRecFpu.SqrtSOfNegativeZeroIsPositiveZero.

	const u32 m = (0x800000u | (t & 0x7FFFFFu)) << ((E & 1u) ? 1 : 2);
	return (((E + 127u) >> 1) << 23) | (eeSqrtSignificand(m) & 0x7FFFFFu);
}

/*	Nothing in the interpreter swaps EmuConfig.Cpu.FPUDivFPCR into the host FPCR
	any more: DIV.S, SQRT.S and RSQRT.S are integer arithmetic now, and no
	rounding mode reaches a digit recurrence. That register is PCSX2's surrogate
	for the divide unit rounding to nearest while the rest of the FPU chops, and
	both recompilers still swap it in, because they run these ops on host singles
	(arm64 recDIV_S_xmm / recSQRT_S_xmm / recRSQRT_S_xmm in iFPU-arm64.cpp and
	the DOUBLE:: twins in iFPUd-arm64.cpp; x86 iFPU.cpp / iFPUd.cpp with
	xLDMXCSR): 1.0 rsqrt 1.5 is 0x3F5105EB on the console and 0x3F5105EC without
	the swap. So the two engines part company on every operand silicon is not
	correctly rounded on, which EeRecFpuDivUnitRounding and EeRecFpuRsqrt pin.
*/

void ABS_S() {
	_FdValUl_ = _FsValUl_ & 0x7fffffff;
	clearFPUFlags( FPUflagO | FPUflagU );
}

/*	Every op below computes `exact` before writing its destination: fd may alias
	fs or ft, and the accumulator forms read the ACC they are about to write.
*/
void ADD_S() {
	const double exact = eeToDouble( _FsValUl_ ) + eeToDouble( _FtValUl_ );
	_FdValUl_ = eeGuardedAddSub( _FsValUl_, _FtValUl_, false );
	raiseOrClearOU( exact );
}

void ADDA_S() {
	const double exact = eeToDouble( _FsValUl_ ) + eeToDouble( _FtValUl_ );
	_FAValUl_ = eeGuardedAddSub( _FsValUl_, _FtValUl_, false );
	raiseOrClearOU( exact );
}

void BC1F() {
	BC1(==);
}

void BC1FL() {
	BC1L(==); // Equal to 0
}

void BC1T() {
	BC1(!=);
}

void BC1TL() {
	BC1L(!=); // different from 0
}

void C_EQ() {
	C_cond_S(==);
}

void C_F() {
	clearFPUFlags( FPUflagC ); //clears C regardless
}

void C_LE() {
	C_cond_S(<=);
}

void C_LT() {
	C_cond_S(<);
}

void CFC1() {
	if (!_Rt_) return;

	// Only bit 4 of the register field is decoded: 0-15 alias FCR0, 16-31
	// alias FCR31. Both recompilers implement this (iFPU.cpp recCFC1,
	// iFPU-arm64.cpp recCFC1); the SD[0] stores force sign extension to 64 bit.
	if (_Fs_ >= 16)
		cpuRegs.GPR.r[_Rt_].SD[0] = (s32)((fpuRegs.fprc[31] & 0x0083c078) | 0x01000001); // drop always-zero bits, set always-one bits
	else
		cpuRegs.GPR.r[_Rt_].SD[0] = (s32)fpuRegs.fprc[0];
}

void CTC1() {
	if ( _Fs_ != 31 ) return;
	fpuRegs.fprc[_Fs_] = cpuRegs.GPR.r[_Rt_].UL[0];
}

void CVT_S() {
	_FdValf_ = (float)_FsValSl_;
}

void CVT_W() {
	if ( ( _FsValUl_ & 0x7F800000 ) <= 0x4E800000 ) { _FdValSl_ = (s32)_FsValf_; }
	else if ( ( _FsValUl_ & 0x80000000 ) == 0 ) { _FdValUl_ = 0x7fffffff; }
	else { _FdValUl_ = 0x80000000; }
}

void DIV_S() {
	// checkDivideByZero only ORs, so the causes are cleared here, as in SQRT.S
	// and RSQRT.S.
	clearFPUFlags(FPUflagI | FPUflagD);

	if (checkDivideByZero( _FdValUl_, _FtValUl_, _FsValUl_, FPUflagD | FPUflagSD, FPUflagI | FPUflagSI)) return;
	_FdValUl_ = eeDivide( _FsValUl_, _FtValUl_ );
}

/*	The EE multiplier's one-ULP deficit.

	The console's multiply array is not a correctly-rounding multiplier: it
	comes back exactly one step closer to zero on a large fraction of operands,
	and which operands depends on operand order. Upstream states the rule in a
	comment (the note above FPU_MUL in `pcsx2/x86/iFPU.cpp`) and never tests it;
	FpuMulHack is a one-point sample of it.

	Measured on SCPH-90000 (FCR0 0x2e40), captures/fpmul/ in the session
	archive. Runs 2 and 3 sweep all 2^23 ft significands at each of twelve fs
	significands, 100663296 rows, and every one of them is either exact or one
	ULP low -- nothing ever came back high, or two ULP low:

	  * `mul.s(1.0, x)` was measured for every one of the 2^23 significands.
	    8257536 of them come back one ULP low and 131072 exact.
	  * `mul.s(x, 1.0)` is exact for all 2^23. The asymmetry is total, not
	    statistical: only ft is recoded, so only ft can contribute a negative
	    digit whose correction the truncated columns drop, which is why the
	    operation is not commutative.
	  * Unchanged across twelve exponent-field pairs from (1,254) to (254,1),
	    so it is a significand-domain effect with no exponent term.

	The mechanism: ft is the Booth-recoded operand and the array's low columns
	are not built, so the low partial products arrive at the summation tree
	missing their bottom bits and each low negative digit's two's-complement
	correction is dropped. eeMulArray() reconstructs that truncated low half and
	compares its column 15 against the exact product's. Where the two disagree
	the array lost exactly 2^15 there, and the loss reaches the result only if
	borrowing it crosses the single ULP.

	The reconstruction is not ours. It is the multiplier out of a proposed PCSX2
	soft-float series -- GitHubProUser67, "Core/EE: Implements Soft-Floats for
	the interpreters", 2025-04-20, crediting Gregory Gaines' write-up, the
	PS2FloatLibrary in MultiServer3 and Goatman13's accurate_int_add_sub branch.
	It is unmerged and still moving: PCSX2 master has no PS2Float, and the
	pcsx2-reliquary fork that carries it as pcsx2/PS2Float.inl has revised it
	since. The shape below is that routine, MulMantissa(), with its two small
	structs unpacked. Bit-exact on all 100663296 measured rows, 15283477 of
	them one ULP low.

	What this replaced: on the 15585118 rows whose product is exactly
	representable the decision collapses to a closed form in ft's mantissa alone
	-- one ULP low iff a negative Booth digit appears among digits 0..4
	(`m & 0x2AA`), or bit 11 disagrees with a boundary term on bits 12..15 --
	the same column-15 test specialised to a zero tail. It is exact there and
	short on 58585 of the remaining rows, where the decision needs fs and so no
	predicate over ft can reach it. The arm64 emitters still implement a cut of
	it, which is cheap where this is not; see iFPUd-arm64.cpp.

	The gate below is exact: a borrow of 2^15 crosses a multiple of 2^k only
	where the tail beneath it is already smaller than 2^15. It leaves the array
	running on 0.27% of random operand pairs.

	Applied only where it was measured: a saturating or flushed result, and a
	decrement that would walk the exponent field to zero, are left alone.
*/
/*	The 3-bit window that selects what digit `bit` of b contributes: 0 and 7
	select zero, 1 and 2 select +a, 3 selects +2a, 4 selects -2a, 5 and 6
	select -a. */
static u32 eeBoothWindow(u32 b, u32 bit)
{
	return (bit ? b >> (bit * 2 - 1) : b << 1) & 7;
}

/*	That digit's partial product of a. 32-bit on purpose: no column above 31 can
	reach a decision taken at column 15, and letting the shift overflow is what
	discards them. A negative digit is left as a one's complement here -- the
	`+1` that would complete the negation is eeBoothCorrection() below. */
static u32 eeBoothPartial(u32 a, u32 b, u32 bit)
{
	const u32 window = eeBoothWindow(b, bit);
	a <<= bit * 2;
	a += (window == 3 || window == 4) ? a : 0;
	if (window >= 4 && window <= 6)
		a ^= 0u - (1u << (bit * 2));
	return (window >= 1 && window <= 6) ? a : 0;
}

/*	The `+1` a negative digit owes, at that digit's own weight. Digits 0..4
	never receive theirs -- their columns are not built, which is the whole
	defect -- so only 5..7 get one. */
static u32 eeBoothCorrection(u32 b, u32 bit)
{
	const u32 window = eeBoothWindow(b, bit);
	return (window >= 4 && window <= 6) ? (1u << (bit * 2)) : 0;
}

/*	One 3:2 carry-save row: returns the sum bits, writes the carry bits. */
static u32 eeCarrySaveAdd(u32 a, u32 b, u32 c, u32& carry)
{
	const u32 u = a ^ b;
	carry = ((u & c) | (a & b)) << 1;
	return u ^ c;
}

/*	The 48-bit significand product as the console's array computes it: the exact
	product, less 2^15 where the truncated low columns come up short there. The
	masks are the columns silicon does not build. */
static u64 eeMulArray(u32 a, u32 b)
{
	const u64 full = static_cast<u64>(a) * static_cast<u64>(b);

	const u32 p0 = eeBoothPartial(a, b, 0);
	const u32 p1 = eeBoothPartial(a, b, 1);
	const u32 p2 = eeBoothPartial(a, b, 2);
	const u32 p3 = eeBoothPartial(a, b, 3);
	const u32 p4 = eeBoothPartial(a, b, 4);
	const u32 p5 = eeBoothPartial(a, b, 5);
	const u32 p6 = eeBoothPartial(a, b, 6);
	const u32 p7 = eeBoothPartial(a, b, 7);

	/*	The tree below is four carry-save levels deep and each lifts a bit by one
		column, so nothing under bit 11 can reach the decision at column 15.
		Digit 4's mask is exactly that boundary -- widening it changes no output,
		narrowing it by one does. Digit 5's sits one higher because its bits 10
		and 11 do not travel through the tree; they are re-injected below. */
	u32 carry0, carry1, carry2, carry3, carry4, carry5;
	const u32 sum0 = eeCarrySaveAdd(p1, p2, p3, carry0);
	const u32 sum1 = eeCarrySaveAdd(p4 & ~0x7ffu, p5 & ~0xfffu, p6, carry1);

	// Digit 5's two surviving product bits, and the corrections digits 5 and 6
	// still receive, ride on rows they did not originate in.
	const u32 hi1 = carry1 | eeBoothCorrection(b, 6) | (p5 & 0x800);
	const u32 row7 = p7 | ((p5 & 0x400) + eeBoothCorrection(b, 5));

	const u32 sum2 = eeCarrySaveAdd(p0, sum0, carry0, carry2);
	const u32 sum3 = eeCarrySaveAdd(row7, sum1, hi1, carry3);
	const u32 sum4 = eeCarrySaveAdd(carry2, sum3, carry3, carry4);
	const u32 sum5 = eeCarrySaveAdd(sum2, sum4, carry4, carry5);

	const u32 lo = sum5 & ~0x7fffu;
	const u32 hi = (carry5 + eeBoothCorrection(b, 7)) & ~0x7fffu;
	return full - (((lo + hi) ^ full) & 0x8000);
}

static bool eeMulOneUlpLow(u32 fs, u32 ft)
{
	if ((fs & 0x7F800000) == 0 || (ft & 0x7F800000) == 0)
		return false; // a zero operand (denormals are zero): the product is zero

	const u32 a = 0x800000u | (fs & 0x7FFFFF);
	const u32 b = 0x800000u | (ft & 0x7FFFFF);
	const u64 prod = static_cast<u64>(a) * static_cast<u64>(b); // exact in 64
	const int k = (prod >> 47) ? 24 : 23;
	if ((prod & ((1ull << k) - 1u)) >= 0x8000u)
		return false; // the tail below the ULP absorbs the whole borrow

	return (prod >> k) != (eeMulArray(a, b) >> k);
}

/*	eeRoundToSingle() for a product, plus the multiplier defect. */
static u32 eeMulRound(u32 fs, u32 ft, double exact)
{
	const u32 w = eeRoundToSingle(exact);

	if (std::fabs(exact) > kEeFpuMax) // saturated: never measured, leave it
		return w;
	if ((w & 0x7F800000) == 0) // flushed to zero
		return w;
	if ((w & 0x7FFFFFFF) == 0x00800000) // a decrement would leave the normals
		return w;

	return eeMulOneUlpLow(fs, ft) ? w - 1u : w;
}

/*	The Instruction Set manual has an overly complicated way of
	determining the flags that are set. Hopefully this shorter
	method provides a similar outcome and is faster. (cottonvibes)
*/
/*	The product is its own named double, so -ffp-contract cannot fuse away the
	two roundings the PS2 ISA mandates -- and so the two flag steps below have
	something separate to look at. See raiseOrClearOU/madFlushedProduct.

	The A-forms name their single-precision product too, because the guarded adder
	needs its bits, and that takes the accumulate out of the compiler's reach as
	well: `_FAValf_ += fs * ft` is one expression a contracting compiler can turn
	into a single-rounded FMA, with only the -ffp-contract=off line in
	pcsx2/CMakeLists.txt between it and that. On corpus cases 567 and 1130 the
	fused value is the console's, so a contracting build hid the guard-bit defect
	on MSUBA.

	Both forms read the accumulator through eeToDouble, so the value path and
	the flag path see the same accumulator.
*/
/*	fd = ACC +/- fs * ft, in the two rounding steps the ISA mandates: the product
	lands in an EE single before the accumulate sees it, and an overflowing one
	ends the instruction there rather than being accumulated. That is the test
	madAccumulandOverflowed() has always made for the flag; the value follows it
	now too.
*/
static u32 eeMulAccumulate(u32 fs, u32 ft, u32 accbits, bool issub)
{
	const double product = eeToDouble( fs ) * eeToDouble( ft );
	const u32 rounded = eeMulRound( fs, ft, product ) ^ (issub ? 0x80000000u : 0u);
	if (madAccumulandOverflowed( product ))
		return rounded;
	return eeGuardedAddSub( accbits, rounded, false );
}

void MADD_S() {
	const double product = eeToDouble( _FsValUl_ ) * eeToDouble( _FtValUl_ );
	const double acc = eeToDouble( _FAValUl_ );
	_FdValUl_ = eeMulAccumulate( _FsValUl_, _FtValUl_, _FAValUl_, false );
	raiseOrClearOU( product );
	if (madAccumulandOverflowed( product )) return;
	raiseOrClearOU( acc + madFlushedProduct( product ) );
}

void MADDA_S() {
	const double product = eeToDouble( _FsValUl_ ) * eeToDouble( _FtValUl_ );
	const double acc = eeToDouble( _FAValUl_ );
	_FAValUl_ = eeMulAccumulate( _FsValUl_, _FtValUl_, _FAValUl_, false );
	raiseOrClearOU( product );
	if (madAccumulandOverflowed( product )) return;
	raiseOrClearOU( acc + madFlushedProduct( product ) );
}

void MAX_S() {
	_FdValUl_  = fp_max( _FsValUl_, _FtValUl_ );
	clearFPUFlags( FPUflagO | FPUflagU );
}

void MFC1() {
	if ( !_Rt_ ) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = _FsValSl_;		// sign extension into 64bit
}

void MIN_S() {
	_FdValUl_ = fp_min(_FsValUl_, _FtValUl_);
	clearFPUFlags( FPUflagO | FPUflagU );
}

void MOV_S() {
	_FdValUl_ = _FsValUl_;
}

void MSUB_S() {
	const double product = eeToDouble( _FsValUl_ ) * eeToDouble( _FtValUl_ );
	const double acc = eeToDouble( _FAValUl_ );
	_FdValUl_ = eeMulAccumulate( _FsValUl_, _FtValUl_, _FAValUl_, true );
	raiseOrClearOU( product );
	if (madAccumulandOverflowed( product )) return;
	raiseOrClearOU( acc - madFlushedProduct( product ) );
}

void MSUBA_S() {
	const double product = eeToDouble( _FsValUl_ ) * eeToDouble( _FtValUl_ );
	const double acc = eeToDouble( _FAValUl_ );
	_FAValUl_ = eeMulAccumulate( _FsValUl_, _FtValUl_, _FAValUl_, true );
	raiseOrClearOU( product );
	if (madAccumulandOverflowed( product )) return;
	raiseOrClearOU( acc - madFlushedProduct( product ) );
}

void MTC1() {
	_FsValUl_ = cpuRegs.GPR.r[_Rt_].UL[0];
}

/*	The product of two EE singles is 48 significand bits, so a double holds it
	exactly at any exponent and eeRoundToSingle() does the only rounding.

	The multiplier's own one-ULP deficit rides on top of that, in eeMulRound --
	see the block comment above eeMulArray for what the array loses and where
	the loss reaches the result. The flag path stays on the exact product: O/U
	is a magnitude test on the exact result and a one-ULP move cannot change it.
*/
void MUL_S() {
	const double exact = eeToDouble( _FsValUl_ ) * eeToDouble( _FtValUl_ );
	_FdValUl_ = eeMulRound( _FsValUl_, _FtValUl_, exact );
	raiseOrClearOU( exact );
}

void MULA_S() {
	const double exact = eeToDouble( _FsValUl_ ) * eeToDouble( _FtValUl_ );
	_FAValUl_ = eeMulRound( _FsValUl_, _FtValUl_, exact );
	raiseOrClearOU( exact );
}

void NEG_S() {
	_FdValUl_  = (_FsValUl_ ^ 0x80000000);
	clearFPUFlags( FPUflagO | FPUflagU );
}

void RSQRT_S() {
	clearFPUFlags(FPUflagD | FPUflagI);

	if ( ( _FtValUl_ & 0x7F800000 ) == 0 ) { // Ft is zero (Denormals are Zero)
		// The dividend decides the cause: zero over zero is invalid, anything
		// else over zero is a divide by zero. Same test checkDivideByZero
		// makes, so a denormal Fs counts as zero here too.
		_ContVal_ |= ( ( _FsValUl_ & 0x7F800000 ) == 0 ) ? ( FPUflagI | FPUflagSI )
		                                                 : ( FPUflagD | FPUflagSD );
		// The EE maximum, and the sign of Fs alone -- no xor, unlike DIV.S:
		// rsqrt takes |Ft|, so the divisor has no sign left to contribute by the
		// time the division happens. Console rows 59 and 63: rsqrt(+0, -0) is
		// +0x7FFFFFFF and rsqrt(-0, -0) is -0x7FFFFFFF, and an xor rule flips
		// both.
		_FdValUl_ = ( _FsValUl_ & 0x80000000 ) | 0x7FFFFFFF;
		return;
	}
	else if ( _FtValUl_ & 0x80000000 ) // Ft is negative
		_ContVal_ |= FPUflagI | FPUflagSI;

	// Both paths divide by a sqrt rounded to single. Dividing by the unrounded
	// double instead lands 1 ULP from the EE FPU and from both recompilers,
	// which stay in single-precision fsqrt+fdiv (x86 recRSQRThelper1, arm64
	// recRSQRT_S_xmm): 1.0 rsqrt 1.5 comes back 0x3F5105EC that way, hardware
	// gives 0x3F5105EB.
	//

	// Neither operand is clamped any more, which is all-or-nothing by design:
	// unclamping only the sqrt used to break rsqrt(2^128, 2^128), which came out
	// right solely because its two clamps cancelled. Unclamping both fixes that
	// row and 13 others. Scored against the console over the corpus, RSQRT.S
	// went 17/32 to 31/32.
	//
	// The composition itself comes from a dedicated console capture: 2231
	// operand pairs over two probes, each pair run as sqrt.s, rsqrt.s and
	// div.s, and rsqrt.s equals div.s(Fs, sqrt.s(Ft)) on every row, with a
	// plain 24-bit single in between. See ee_fpu_divunit_console_tests.cpp.
	_FdValUl_ = eeDivide( _FsValUl_, eeSqrtBits( _FtValUl_ ) );
}

void SQRT_S() {
	clearFPUFlags(FPUflagI | FPUflagD);

	// Invalid-operation keys off the SIGN BIT ALONE. -0 and the negative
	// denormals raise it too, even though they are flushed to -0 and produce a
	// perfectly ordinary +0: the exponent field plays no part. It used to sit
	// inside a negative-normal arm, so those two operand classes came back with
	// FCR31 untouched. x86's recSQRT_S_xmm has always tested the sign
	// bit alone (iFPU.cpp, MOVMSKPS & 1), as has the FULL-mode DOUBLE path in
	// iFPUd-arm64.cpp. Scored against a first-party capture over the sign x
	// exponent matrix -- see EeRecFpu.SqrtSInvalidFlagFollowsTheSignBitAlone.
	if ( _FtValUl_ & 0x80000000 )
		_ContVal_ |= FPUflagI | FPUflagSI;

	_FdValUl_ = eeSqrtBits( _FtValUl_ );
}

void SUB_S() {
	const double exact = eeToDouble( _FsValUl_ ) - eeToDouble( _FtValUl_ );
	_FdValUl_ = eeGuardedAddSub( _FsValUl_, _FtValUl_, true );
	raiseOrClearOU( exact );
}

void SUBA_S() {
	const double exact = eeToDouble( _FsValUl_ ) - eeToDouble( _FtValUl_ );
	_FAValUl_ = eeGuardedAddSub( _FsValUl_, _FtValUl_, true );
	raiseOrClearOU( exact );
}

}	// End Namespace COP1

/////////////////////////////////////////////////////////////////////
// COP1 (FPU)  Load/Store Instructions

// These are actually EE opcodes but since they're related to FPU registers and such they
// seem more appropriately located here.

void LWC1() {
	u32 addr;
	addr = cpuRegs.GPR.r[_Rs_].UL[0] + (s16)(cpuRegs.code & 0xffff);	// force sign extension to 32bit
	if (addr & 0x00000003) { Console.Error( "FPU (LWC1 Opcode): Invalid Unaligned Memory Address" ); return; }  // Should signal an exception?
	fpuRegs.fpr[_Rt_].UL = memRead32(addr);
}

void SWC1() {
	u32 addr;
	addr = cpuRegs.GPR.r[_Rs_].UL[0] + (s16)(cpuRegs.code & 0xffff);	// force sign extension to 32bit
	if (addr & 0x00000003) { Console.Error( "FPU (SWC1 Opcode): Invalid Unaligned Memory Address" ); return; }  // Should signal an exception?
	memWrite32(addr, fpuRegs.fpr[_Rt_].UL);
}

} } }
