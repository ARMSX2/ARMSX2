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

	Nothing is clamped, unlike fpuDouble(): a double holds the whole EE range,
	so exponent 255 stays an ordinary binade and 0x7F800000 is 2^128. The O/U
	decision is about the exact result, so it must not inherit the operand
	clamp a host single forces on fpuDouble().

	Denormal operands flush to signed zero; the EE has none. U is raised only
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
// Used for Comparing; This compares if the floats are exactly the same.
	#define C_cond_S(cond) {  \
	   _ContVal_ = ( fpuDouble(_FsValUl_) cond fpuDouble(_FtValUl_) ) ?  \
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

/*	The operand model as bits. Exponent 0 is zero on this FPU -- there are no
	denormals -- and exponent 255 folds to +-0x7F7FFFFF, since a host single
	cannot hold the EE's top binade at all. fpuDouble() is this same mapping read
	back as a float.

	The arithmetic ops no longer use either: the fold destroys the operand
	before they see it, so they read eeToDouble() and round through
	eeRoundToSingle(). Still the model for the compare ops (C.EQ and friends),
	which only part from the unfolded operands where both are in the top binade,
	and those rows pass against the capture. */
static u32 fpuOperandBits(u32 f)
{
	switch (f & 0x7f800000)
	{
		case 0x0:        return f & 0x80000000;
		case 0x7f800000: return (f & 0x80000000) | 0x7f7fffff;
		default:         return f;
	}
}

float fpuDouble(u32 f)
{
	FPRreg r;
	r.UL = fpuOperandBits(f);
	return r.f;
}

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
	    had the range.
	  * Denormal results flush to signed zero, written out rather than left to
	    the ambient FPCR's FZ.

	The (float) casts round under the host FPCR, so the EE's rounding mode is
	honoured here without naming it -- including the divide/sqrt unit's separate
	mode, which its callers scope in.
*/
static u32 eeRoundToSingle(double exact)
{
	const double mag = std::fabs(exact);

	if (mag > kEeFpuMax)
		return (std::signbit(exact) ? 0x80000000u : 0u) | 0x7FFFFFFFu;

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
	if ((r.UL & 0x7F800000) == 0)
		r.UL &= 0x80000000; // denormal or zero -- the EE has only the zero
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
	return eeRoundToSingle(eeToDouble(a) + eeToDouble(b));
}

/*	Divide two EE singles with exactly one rounding.

	Not through eeToDouble(), the way the adder and the multiplier go: their
	results are exact in a double and a quotient is not, so widening and then
	narrowing rounds twice. Chopping would forgive that, but the divide unit
	rounds to nearest (FPUDivFPCR, scoped in by the callers), where the second
	rounding can land a ULP away.

	So rescale instead of widening. Both operands are forced to exponent 127, so
	the division happens between two significands in [1,2) where nothing can
	overflow or underflow and the host performs the EE's single rounding; the
	exponents are added back onto the quotient afterwards. Scaling by a power of
	two leaves a significand alone, so the quotient's significand and its
	rounding do not depend on where the operands sat in the range -- only the
	reassembled exponent does, and that is integer arithmetic.

	The divisor must already be known nonzero: a zero divisor is a flag question
	the callers answer first.
*/
static u32 eeDivide(u32 a, u32 b)
{
	const s32 ea = (s32)((a >> 23) & 0xFF);
	const s32 eb = (s32)((b >> 23) & 0xFF);

	if (ea == 0)
		return (a ^ b) & 0x80000000; // zero dividend, sign from both operands

	FPRreg ma, mb, q;
	ma.UL = (a & 0x807FFFFFu) | (127u << 23);
	mb.UL = (b & 0x807FFFFFu) | (127u << 23);
	q.f = ma.f / mb.f;

	// |q| is in (0.5, 2], so its exponent field carries 126, 127 or -- if the
	// rounding pushed it to exactly 2.0 -- 128.
	const s32 e = (s32)((q.UL >> 23) & 0xFF) + ea - eb;
	if (e > 255)
		return (q.UL & 0x80000000u) | 0x7FFFFFFFu;
	if (e < 1)
		return q.UL & 0x80000000u; // the EE has no denormals to underflow into
	return (q.UL & 0x807FFFFFu) | ((u32)e << 23);
}

/*	sqrt(|Ft|) as EE bits, including the top binade. The exponent-255 arm is the
	same |Ft|/4 prescale SQRT.S does inline below, where the reason for it is. */
static u32 eeSqrtBits(u32 t)
{
	FPRreg r;
	if ((t & 0x7F800000) == 0)
	{
		r.UL = 0; // +/-0 and the denormals: the EE drops the sign here
	}
	else if ((t & 0x7F800000) == 0x7F800000)
	{
		FPRreg quarter;
		quarter.UL = (t & 0x7FFFFFFF) - 0x01000000; // |Ft| / 4
		r.f = 2.0 * sqrt((double)quarter.f);
	}
	else
	{
		FPRreg mag;
		mag.UL = t & 0x7FFFFFFF;
		r.f = sqrt(mag.f);
	}
	return r.UL;
}

/*	The EE's divide/square-root unit rounds to nearest even when the rest of the
	FPU is chopping toward zero, which is why PCSX2 carries a second control
	register, FPUDivFPCR, whose only difference from FPUFPCR is the rounding
	mode. Both recompilers swap the host rounding mode around DIV/SQRT/RSQRT
	(arm64 recDIV_S_xmm / recSQRT_S_xmm / recRSQRT_S_xmm in iFPU-arm64.cpp and
	the DOUBLE:: twins in iFPUd-arm64.cpp; x86 iFPU.cpp / iFPUd.cpp do the same
	with xLDMXCSR). The interpreter never did, so those three ops came out one
	ULP low against both recompilers and the console whenever a game is in the
	default chop mode.

	Gated the way the emitters gate it: where the two registers already agree
	there is nothing to swap.
*/
class ScopedDivRoundMode
{
public:
	__fi ScopedDivRoundMode()
		: m_swap(EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
	{
		if (m_swap)
		{
			m_prev = FPControlRegister::GetCurrent();
			FPControlRegister::SetCurrent(EmuConfig.Cpu.FPUDivFPCR);
		}
	}
	__fi ~ScopedDivRoundMode()
	{
		if (m_swap)
			FPControlRegister::SetCurrent(m_prev);
	}

	ScopedDivRoundMode(const ScopedDivRoundMode&) = delete;
	ScopedDivRoundMode& operator=(const ScopedDivRoundMode&) = delete;

private:
	FPControlRegister m_prev;
	bool m_swap;
};

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
	const ScopedDivRoundMode div_round;
	if (checkDivideByZero( _FdValUl_, _FtValUl_, _FsValUl_, FPUflagD | FPUflagSD, FPUflagI | FPUflagSI)) return;
	_FdValUl_ = eeDivide( _FsValUl_, _FtValUl_ );
}

/*	The EE multiplier's one-ULP deficit.

	The console's multiply array is not a correctly-rounding multiplier: it
	comes back exactly one step closer to zero on a large fraction of operands,
	and which operands depends on operand order. Upstream states the rule in a
	comment (`pcsx2/x86/iFPU.cpp:500`) and never tests it; FpuMulHack is a
	one-point sample of it.

	Measured on SCPH-90000 (FCR0 0x2e40), captures/fpmul/ in the session
	archive, 25M probes over three runs:

	  * `mul.s(1.0, x)` was measured for all 2^23 significands. 8257536 of them
	    come back one ULP low and 131072 exact -- and nothing ever came back
	    high, or two ULP low, in 16.8M probes.
	  * `mul.s(x, 1.0)` is exact for all 2^23. The asymmetry is total, not
	    statistical: the predicate reads ft and never fs, which is exactly why
	    the operation is not commutative.
	  * Unchanged across twelve exponent-field pairs from (1,254) to (254,1),
	    so it is a significand-domain effect with no exponent term.

	Bits 1,3,5,7,9 of ft's mantissa are the sign bits of the five lowest
	radix-4 Booth digits, which is what identifies the mechanism: ft is the
	recoded operand and the array's low columns are not built, so each low
	negative digit's two's-complement correction is dropped. The bit-11 term
	does not follow from that mechanism; it is what the capture shows at the
	truncation column. In Booth form the same predicate reads "exact iff digits
	0..4 are non-negative and (d5<0) == (d7<0)", which was checked equivalent
	over the whole 2^23 space.

	What this does not model: the deficit is smaller than one ULP -- at most
	~27308 against an ULP of 2^23 -- so it only reaches the result when the
	exact product has nothing below the ULP to absorb it. That is the `tail`
	test below, and it is the whole of the modelled class. When the tail is
	non-zero the console is one ULP low iff the tail is smaller than the
	deficit, and the deficit is not identifiable from mul.s observations: it is
	only ever visible through the single comparison the instruction performs.
	That residual is ~0.1% of random operand pairs and 0 rows of the console
	corpus.

	Applied only where it was measured; the three guards are in eeMulRound().
*/
static bool eeMulDefectiveFt(u32 ft)
{
	const u32 m = ft & 0x7FFFFF;
	if (m & 0x2AA) // a negative Booth digit among 0..4
		return true;
	const u32 h = (m >> 12) & 0xF;
	return ((m >> 11) & 1u) != ((h >= 8 && h <= 13) ? 1u : 0u);
}

static bool eeMulOneUlpLow(u32 fs, u32 ft)
{
	if ((fs & 0x7F800000) == 0 || (ft & 0x7F800000) == 0)
		return false; // a zero operand (denormals are zero): the product is zero

	const u64 a = 0x800000u | (fs & 0x7FFFFF);
	const u64 b = 0x800000u | (ft & 0x7FFFFF);
	const u64 prod = a * b; // 47 or 48 significant bits, exact in 64
	const int k = (prod >> 47) ? 24 : 23;
	if (prod & ((1ull << k) - 1u))
		return false; // the tail below the ULP absorbs the deficit

	return eeMulDefectiveFt(ft);
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
	needs its bits, and naming it takes the accumulate out of a contracting
	compiler's reach: `_FAValf_ += fs * ft` is one expression it can turn into a
	single-rounded FMA, with only the -ffp-contract=off line in
	pcsx2/CMakeLists.txt between it and that. On corpus cases 567 and 1130 the
	fused value is the console's, so a contracting build hid the guard-bit defect
	on MSUBA.

	The A-forms read the accumulator raw where the d-forms route it through
	fpuDouble; the flag path uses the architectural value in both. That
	inconsistency in the value path is pre-existing and left alone.
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
	exactly at any exponent and eeRoundToSingle() does the only rounding. The
	multiplier's own one-ULP deficit rides on top, in eeMulRound(); what it
	models and what it does not is at eeMulDefectiveFt. The flags stay on the
	exact product, per raiseOrClearOU().
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
	const ScopedDivRoundMode div_round;
	clearFPUFlags(FPUflagD | FPUflagI);

	if ( ( _FtValUl_ & 0x7F800000 ) == 0 ) { // Ft is zero (Denormals are Zero)
		_ContVal_ |= FPUflagD | FPUflagSD;
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

	//
	// Neither operand is clamped any more, and it has to be both: with the clamp
	// left on the sqrt alone, rsqrt(2^128, 2^128) came out right only because
	// the two clamps cancelled. Still 1 ULP out at rsqrt(EEMAX, EEMAX), from the
	// two-step rounding above rather than the operand model.
	_FdValUl_ = eeDivide( _FsValUl_, eeSqrtBits( _FtValUl_ ) );
}

void SQRT_S() {
	const ScopedDivRoundMode div_round;
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

	if ( ( _FtValUl_ & 0x7F800000 ) == 0 ) // If Ft = +/-0 (denormals included)
	{
		_FdValUl_ = 0;                     // +0: the EE drops the sign here, and
		                                   // both recompilers already do (they
		                                   // take |Ft| before the sqrt). See
		                                   // EeRecFpu.SqrtSOfNegativeZeroIsPositiveZero.
	}
	else if ( ( _FtValUl_ & 0x7F800000 ) == 0x7F800000 )
	{
		// Exponent 255 is an ordinary binade on the EE -- the representable max
		// is 0x7FFFFFFF, not FLT_MAX -- so fpuDouble()'s clamp hands sqrt a
		// different operand rather than a rounded one: sqrt(0x7FFFFFFF) came
		// back 0x5F7FFFFF where the console gives 0x5FB504F3. Square-root
		// |Ft|/4 and double it instead. 4 is an even power of two, so its own
		// square root is exact and the sqrt below stays the only rounding step.
		// recSQRT_S_xmm (iFPU-arm64.cpp) emits the same two steps and carries
		// the rest of the argument.
		//
		// RSQRT_S does not get this: its two clamped operands cancel on
		// rsqrt(2^128, 2^128), so unclamping only the sqrt breaks that row.
		// It is all-or-nothing and is a separate change.
		FPRreg quarter;
		quarter.UL = ( _FtValUl_ & 0x7FFFFFFF ) - 0x01000000; // |Ft| / 4
		_FdValf_ = 2.0 * sqrt( (double)quarter.f );
	}
	else
	{
		_FdValf_ = sqrt( fabs( fpuDouble( _FtValUl_ ) ) ); // sqrt of |Ft|
	}
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
