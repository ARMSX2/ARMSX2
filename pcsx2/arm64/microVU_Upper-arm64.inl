// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

//------------------------------------------------------------------
// Micro VU - ARM64 NEON Upper Instructions (FMAC pipeline)
//------------------------------------------------------------------

//------------------------------------------------------------------
// NEON Arithmetic Functions
//------------------------------------------------------------------

// An operand the caller already left inside +/-fMax, so its mVUclamp3 is
// dropped. MADD/MSUB and the A-forms set this for the product, which
// mVUclamp4 clamped to the same bounds one emitter call earlier.
enum
{
	preClampTo   = 1,
	preClampFrom = 2,
};

// The lane-fold multiply: Fs *= Ft[lane], by element, as a vector or as
// a scalar for the single-lane forms.
static __fi void mVUemitLaneMul(mV, const a64::VRegister& Fs, const a64::VRegister& Ft, int lane, bool scalar);

// What an arithmetic step reads in place of `to`. When the last word emitted is
// the clone-write copy that filled `to`, the step reads the register it was
// copied from and the copy goes: every step below writes all four lanes of
// `to` (a scalar op zeroes lanes 1-3), so nothing the copy put there survives
// it. A clamp of `to` emitted in between ends the copy's run, and then the
// step reads `to` as before.
static __fi a64::VRegister mVUstepSource(mV, const a64::VRegister& to)
{
	const int src = mVU.regAlloc->takeCloneSource(to.GetCode());
	return (src >= 0) ? a64::VRegister(src, 128) : to;
}

static __fi void mVUemitLaneMul(mV, const a64::VRegister& Fs, const a64::VRegister& Ft, int lane, bool scalar)
{
	// vm MUST be the scalar .S() view: vixl's by-element emitter keys element
	// size off vm's scalar format, and a V4S vm silently selects the
	// half-precision opcode once Devel strips the VIXL_ASSERT.
	const a64::VRegister a = mVUstepSource(mVU, Fs);
	if (scalar)
		armAsm->Fmul(Fs.S(), a.S(), Ft.S(), lane);
	else
		armAsm->Fmul(Fs.V4S(), a.V4S(), Ft.S(), lane);
}

// The adder's guard bits (armEmitVuGuardMask), between the clamps and the add.
//
// The masked pair goes to the scratch trio rather than over the operands: `to`
// is a live destination and `from` is often a VF cache slot, and both have to
// come out unchanged bar the result. q29/q30/q31 are free at every call --
// mVUclamp3 has finished with q29, and the FMAC bodies' own q30/q31 use is the
// U/O models' scratch, dead before the arithmetic. qmmPQ is q28 and reaches
// here as `from`, which is why the trio starts at q29.
//
// vuClampMode 4 only, on CHECK_VU_EXACT(mVU.index): both VUs, unlike the macro
// path's VU0-only gate.
static __fi void mVUemitAddSub(mV, const a64::VRegister& to, const a64::VRegister& from,
	bool issub, bool scalar)
{
	a64::VRegister a = to, b = from;
	if (CHECK_VU_EXACT(mVU.index))
	{
		pxAssert(to.GetCode() < RQSCRATCH3.GetCode() && from.GetCode() < RQSCRATCH3.GetCode());
		armEmitVuGuardMask(RQSCRATCH, RQSCRATCH2, to, from, RQSCRATCH3);
		a = RQSCRATCH;
		b = RQSCRATCH2;
	}
	else
	{
		a = mVUstepSource(mVU, to);
	}

	if (scalar)
	{
		const a64::SRegister sd(to.GetCode()), sa(a.GetCode()), sb(b.GetCode());
		if (issub) armAsm->Fsub(sd, sa, sb);
		else       armAsm->Fadd(sd, sa, sb);
	}
	else
	{
		if (issub) armAsm->Fsub(to.V4S(), a.V4S(), b.V4S());
		else       armAsm->Fadd(to.V4S(), a.V4S(), b.V4S());
	}
}

// Both operands of one arithmetic step, so the clone-write copy in front of
// each folds rather than only the last one made (mVUclampOperands). `from` is
// the older allocation at every call site below: a body hands out Ft, or the
// accumulator, before the register the step writes into.
static __fi void mVUclampStepOperands(mV, const a64::VRegister& to, const a64::VRegister& from,
	int preClamped, int xyzw)
{
	if (!clampE)
		return;
	mVUclampOperands(mVU, {(preClamped & preClampFrom) ? a64::NoVReg : from,
	                       (preClamped & preClampTo) ? a64::NoVReg : to}, xyzw, true,
	                 (xyzw == 0x8) ? to : a64::NoVReg);
}

static void NEON_ADDPS(mV, const a64::VRegister& to, const a64::VRegister& from, int preClamped = 0)
{
	mVUclampStepOperands(mVU, to, from, preClamped, _X_Y_Z_W);
	mVUemitAddSub(mVU, to, from, false, false);
	mVUclamp4(mVU, to, RQSCRATCH3, _X_Y_Z_W);
}

static void NEON_SUBPS(mV, const a64::VRegister& to, const a64::VRegister& from, int preClamped = 0)
{
	mVUclampStepOperands(mVU, to, from, preClamped, _X_Y_Z_W);
	mVUemitAddSub(mVU, to, from, true, false);
	mVUclamp4(mVU, to, RQSCRATCH3, _X_Y_Z_W);
}

// The multiplier's one-ULP deficit (armEmitVuDefectiveMul), between the clamps
// and the result clamp, on both VUs at vuClampMode 4.
//
// The scratch pair is q29 and q31, free across every FMAC body's arithmetic for
// the reason mVUemitAddSub gives about the trio. `to` is the destination and
// the first multiplicand at once, so the product is computed twice rather than
// parked; ft is `from`, and it is the operand the predicate reads.
//
// q30, the third of the trio, is free in a multiply body: the only other user
// is the adder's guard mask, and no FMAC body runs both. So the operand the
// model overwrites is kept in a register here, where the COP2 macro path has to
// store it to memory.
//
// The two cases the model does not decide go to the same helper the COP2 macro
// path calls. The scalar form needs no separate test: a scalar FMUL zeroes the
// lanes above the first, so their product exponent is 0 and both conditions
// below reject them.
//
// The AX-14 lane fold in setupFtReg cannot reach this: it needs !willClamp and
// clampE is on from vuClampMode 2 up.
static __fi void mVUemitMul(mV, const a64::VRegister& to, const a64::VRegister& from, bool scalar)
{
	if (!CHECK_VU_EXACT(mVU.index))
	{
		const a64::VRegister a = mVUstepSource(mVU, to);
		if (scalar)
			armAsm->Fmul(to.S(), a.S(), from.S());
		else
			armAsm->Fmul(to.V4S(), a.V4S(), from.V4S());
		return;
	}

	const a64::VRegister& t = RQSCRATCH3;
	const a64::VRegister& u = RQSCRATCH2;
	const a64::VRegister& fs = RQSCRATCH;
	const bool aliasFt = from.Is(to);

	armAsm->Mov(fs.V16B(), to.V16B());
	armEmitVuDefectiveMul(to, to, from, t, u, scalar, &RWARG2);

	// As on the COP2 macro path, the condition below only has to avoid false
	// negatives: eeMulOneUlpLow checks the tail itself, and a lane the model
	// already decremented has a residue of exactly one ULP.
	armAsm->Mov(t.V16B(), to.V16B());
	armAsm->Fmls(t.V4S(), fs.V4S(), aliasFt ? fs.V4S() : from.V4S());

	armAsm->Shl(t.V4S(), t.V4S(), 1);
	armAsm->Ushr(t.V4S(), t.V4S(), 24);       // the residue's exponent
	armAsm->Shl(u.V4S(), to.V4S(), 1);
	armAsm->Ushr(u.V4S(), u.V4S(), 24);       // the product's
	armAsm->Uqsub(u.V4S(), u.V4S(), t.V4S());
	armAsm->Ushr(u.V4S(), u.V4S(), 5);        // exponents at least 32 apart
	armAsm->Umin(u.V4S(), u.V4S(), t.V4S());  // and the residue is not zero
	armAsm->Umaxv(u.S(), u.V4S());
	armAsm->Fmov(RWARG1, u.S());
	armAsm->Orr(RWARG1, RWARG1, RWARG2); // plus the lanes the exponent test cut

	a64::Label done;
	armAsm->Cbz(RWARG1, &done);
	VuMulBandSlot* slot = &g_vuMulBand[mVU.index];
	armMoveAddressToReg(a64::x8, slot);
	armAsm->Str(fs, a64::MemOperand(a64::x8, offsetof(VuMulBandSlot, fs)));
	armAsm->Str(aliasFt ? fs : from, a64::MemOperand(a64::x8, offsetof(VuMulBandSlot, ft)));
	armAsm->Str(to, a64::MemOperand(a64::x8, offsetof(VuMulBandSlot, product)));
	mVUemitModelCall(mVU, mVUModelStubMulShortTailBand);
	armAsm->Ldr(to, a64::MemOperand(a64::x8, offsetof(VuMulBandSlot, product)));
	armAsm->Bind(&done);
}

static void NEON_MULPS(mV, const a64::VRegister& to, const a64::VRegister& from, int preClamped = 0)
{
	mVUclampStepOperands(mVU, to, from, preClamped, _X_Y_Z_W);
	mVUemitMul(mVU, to, from, false);
	mVUclamp4(mVU, to, RQSCRATCH3, _X_Y_Z_W);
}

static void NEON_ADDSS(mV, const a64::VRegister& to, const a64::VRegister& from, int preClamped = 0)
{
	mVUclampStepOperands(mVU, to, from, preClamped, 0x8);
	mVUemitAddSub(mVU, to, from, false, true);
	mVUclamp4(mVU, to, RQSCRATCH3, 0x8);
}

static void NEON_SUBSS(mV, const a64::VRegister& to, const a64::VRegister& from, int preClamped = 0)
{
	mVUclampStepOperands(mVU, to, from, preClamped, 0x8);
	mVUemitAddSub(mVU, to, from, true, true);
	mVUclamp4(mVU, to, RQSCRATCH3, 0x8);
}

static void NEON_MULSS(mV, const a64::VRegister& to, const a64::VRegister& from, int preClamped = 0)
{
	mVUclampStepOperands(mVU, to, from, preClamped, 0x8);
	mVUemitMul(mVU, to, from, true);
	mVUclamp4(mVU, to, RQSCRATCH3, 0x8);
}

// The same clamps without the deficit, for the EFU's polynomials
// (microVU_Lower-arm64.inl). Those are not FMAC products: below vuClampMode 4
// the recompiler evaluates each series in host arithmetic, and at 4 the whole
// op becomes one VuEfuModel call, which carries the multiplier itself. Two of
// the series' multiply shapes -- the coefficient multiply and the squares --
// reach neither this function nor NEON_MULSS, so the deficit could only ever
// cover part of a series, and it moves no row of autocases_efu.h.
static void NEON_MULSS_Series(mV, const a64::VRegister& to, const a64::VRegister& from)
{
	mVUclampStepOperands(mVU, to, from, 0, 0x8);
	armAsm->Fmul(to.S(), mVUstepSource(mVU, to).S(), from.S());
	mVUclamp4(mVU, to, RQSCRATCH3, 0x8);
}

// ADD2 variants — ADDi (opType 5). The PS form needs no special handling; the
// SS form implements the tri-ace VuAddSubHack when the gamefix is enabled.
static void NEON_ADD2PS(mV, const a64::VRegister& to, const a64::VRegister& from, int preClamped = 0)
{
	NEON_ADDPS(mVU, to, from, preClamped);
}

// Port of x86 ADD_SS_TriAceHack (microVU_Misc.inl). Tri-ace games need ADDi to be
// bit-accurate: if the two operands' exponents differ by >= 25, the smaller one is
// flushed to a signed zero (sign bit kept, exponent+mantissa of lane 0 cleared —
// the x86 PAND against {0x80000000, ~0, ~0, ~0}) before the scalar add. Unclamped,
// matching x86. Without it this is NEON_ADDSS.
static void NEON_ADD2SS(mV, const a64::VRegister& to, const a64::VRegister& from, int /*preClamped*/ = 0)
{
	// The gamefix flushed whichever operand sat 25 or more exponents below the
	// other, which is the last row of the adder's own guard mask. With the mask
	// emitted at every separation it has nothing left to add.
	if (!CHECK_VUADDSUBHACK || CHECK_VU_EXACT(mVU.index))
	{
		NEON_ADDSS(mVU, to, from);
		return;
	}

	// Exponent difference (from_exp - to_exp), bits 23..30 of each lane-0 word.
	armAsm->Umov(gprT1.W(), to.V4S(), 0);
	armAsm->Umov(gprT2.W(), from.V4S(), 0);
	armAsm->Ubfx(gprT1.W(), gprT1.W(), 23, 8);
	armAsm->Ubfx(gprT2.W(), gprT2.W(), 23, 8);
	armAsm->Sub(gprT3.W(), gprT2.W(), gprT1.W());

	a64::Label case_neg_big, case_end;
	armAsm->Cmp(gprT3.W(), -25);
	armAsm->B(&case_neg_big, a64::le); // from much smaller -> flush from
	armAsm->Cmp(gprT3.W(), 25);
	armAsm->B(&case_end, a64::lt);     // within range -> no flush

	// to much smaller -> flush to: keep only the sign bit of lane 0.
	armAsm->Umov(gprT1.W(), to.V4S(), 0);
	armAsm->And(gprT1.W(), gprT1.W(), 0x80000000);
	armAsm->Ins(to.V4S(), 0, gprT1.W());
	armAsm->B(&case_end);

	armAsm->Bind(&case_neg_big);
	armAsm->Umov(gprT1.W(), from.V4S(), 0);
	armAsm->And(gprT1.W(), gprT1.W(), 0x80000000);
	armAsm->Ins(from.V4S(), 0, gprT1.W());

	armAsm->Bind(&case_end);
	armAsm->Fadd(a64::SRegister(to.GetCode()), a64::SRegister(to.GetCode()), a64::SRegister(from.GetCode()));
}

//------------------------------------------------------------------
// MAX/MIN — Integer comparison approach
//------------------------------------------------------------------
// IEEE FMAX/FMIN have NaN propagation issues that don't match PS2 behavior.
// Use the same integer comparison trick as x86: convert float bit patterns
// so that the integer comparison order matches the float comparison order.
// For each lane: t = (val >> 31) ? (val ^ 0x7fffffff) : val
// Then CMGT.4S selects the correct operand.

static void NEON_MAXPS(mV, const a64::VRegister& to, const a64::VRegister& from, int /*preClamped*/ = 0)
{
	const a64::VRegister& t1 = mVU.regAlloc->allocReg();
	const a64::VRegister& t2 = mVU.regAlloc->allocReg();

	// Transform 'to' for integer comparison
	armAsm->Sshr(t1.V4S(), to.V4S(), 31);     // All sign bits replicated
	armAsm->Ushr(t1.V4S(), t1.V4S(), 1);       // 0x7fffffff where negative, 0 where positive
	armAsm->Eor(t1.V16B(), t1.V16B(), to.V16B());

	// Transform 'from' for integer comparison
	armAsm->Sshr(t2.V4S(), from.V4S(), 31);
	armAsm->Ushr(t2.V4S(), t2.V4S(), 1);
	armAsm->Eor(t2.V16B(), t2.V16B(), from.V16B());

	// MAX: select 'to' where t1 > t2, else 'from'. Bif (Bitwise Insert if
	// False) writes 'from' into 'to' where the mask is 0 — fuses BSL+Mov.
	armAsm->Cmgt(t1.V4S(), t1.V4S(), t2.V4S());
	armAsm->Bif(to.V16B(), from.V16B(), t1.V16B());

	mVU.regAlloc->clearNeeded(t1);
	mVU.regAlloc->clearNeeded(t2);
}

static void NEON_MINPS(mV, const a64::VRegister& to, const a64::VRegister& from, int /*preClamped*/ = 0)
{
	const a64::VRegister& t1 = mVU.regAlloc->allocReg();
	const a64::VRegister& t2 = mVU.regAlloc->allocReg();

	// Transform 'to' for integer comparison
	armAsm->Sshr(t1.V4S(), to.V4S(), 31);
	armAsm->Ushr(t1.V4S(), t1.V4S(), 1);
	armAsm->Eor(t1.V16B(), t1.V16B(), to.V16B());

	// Transform 'from' for integer comparison
	armAsm->Sshr(t2.V4S(), from.V4S(), 31);
	armAsm->Ushr(t2.V4S(), t2.V4S(), 1);
	armAsm->Eor(t2.V16B(), t2.V16B(), from.V16B());

	// MIN: select 'to' where t2 > t1 (i.e., to < from), else 'from'.
	// Bif fuses the BSL+Mov into a single insn.
	armAsm->Cmgt(t2.V4S(), t2.V4S(), t1.V4S());
	armAsm->Bif(to.V16B(), from.V16B(), t2.V16B());

	mVU.regAlloc->clearNeeded(t1);
	mVU.regAlloc->clearNeeded(t2);
}

static void NEON_MAXSS(mV, const a64::VRegister& to, const a64::VRegister& from, int /*preClamped*/ = 0)
{
	const a64::VRegister& t1 = mVU.regAlloc->allocReg();

	// Transform to[0] — read 'to' straight into the scratch; no copy needed (the
	// in-place shift would only re-read what Sshr can read directly).
	armAsm->Sshr(RQSCRATCH.V4S(), to.V4S(), 31);
	armAsm->Ushr(RQSCRATCH.V4S(), RQSCRATCH.V4S(), 1);
	armAsm->Eor(RQSCRATCH.V16B(), RQSCRATCH.V16B(), to.V16B());

	// Transform from[0] — read 'from' straight into t1; no copy needed.
	armAsm->Sshr(t1.V4S(), from.V4S(), 31);
	armAsm->Ushr(t1.V4S(), t1.V4S(), 1);
	armAsm->Eor(t1.V16B(), t1.V16B(), from.V16B());

	// Compare lane 0 as integers: if to_xform > from_xform, keep to, else take from
	armAsm->Cmgt(RQSCRATCH.V4S(), RQSCRATCH.V4S(), t1.V4S());
	// Use BSL: where mask=1 keep to, where mask=0 keep from
	// Only lane 0 is relevant — write result into to[0]
	armAsm->Bsl(RQSCRATCH.V16B(), to.V16B(), from.V16B());
	armAsm->Ins(to.V4S(), 0, RQSCRATCH.V4S(), 0);

	mVU.regAlloc->clearNeeded(t1);
}

static void NEON_MINSS(mV, const a64::VRegister& to, const a64::VRegister& from, int /*preClamped*/ = 0)
{
	const a64::VRegister& t1 = mVU.regAlloc->allocReg();

	// Transform to[0] — read 'to' straight into the scratch; no copy needed (the
	// in-place shift would only re-read what Sshr can read directly).
	armAsm->Sshr(RQSCRATCH.V4S(), to.V4S(), 31);
	armAsm->Ushr(RQSCRATCH.V4S(), RQSCRATCH.V4S(), 1);
	armAsm->Eor(RQSCRATCH.V16B(), RQSCRATCH.V16B(), to.V16B());

	// Transform from[0] — read 'from' straight into t1; no copy needed.
	armAsm->Sshr(t1.V4S(), from.V4S(), 31);
	armAsm->Ushr(t1.V4S(), t1.V4S(), 1);
	armAsm->Eor(t1.V16B(), t1.V16B(), from.V16B());

	// MIN: where from_xform > to_xform (i.e., to is smaller), keep to
	armAsm->Cmgt(t1.V4S(), t1.V4S(), RQSCRATCH.V4S());
	armAsm->Bsl(t1.V16B(), to.V16B(), from.V16B());
	armAsm->Ins(to.V4S(), 0, t1.V4S(), 0);

	mVU.regAlloc->clearNeeded(t1);
}

// MAX/MINI against VF0, whose four words are +0, +0, +0 and +1.0: every one of
// them has its sign bit clear. With at least one operand non-negative the two
// operands are never both negative, which is the one case where the integer
// order of the raw words differs from the order the transform above builds
// (negative raw words compare backwards among themselves). Otherwise both
// orders agree: a negative word is below every non-negative one either way,
// and two non-negative words compare as plain integers in both. Equal keys
// are equal words. So the select is one SMAX or SMIN on the raw words.
//
// Single-lane forms run on the whole register too: lane 0 is the one the
// allocator writes back, and the clone's other lanes are dead.
static void mVUemitMaxMinSignClear(mV, const a64::VRegister& to, const a64::VRegister& from, bool isMax)
{
	const a64::VRegister a = mVUstepSource(mVU, to);
	if (isMax)
		armAsm->Smax(to.V4S(), a.V4S(), from.V4S());
	else
		armAsm->Smin(to.V4S(), a.V4S(), from.V4S());
}

//------------------------------------------------------------------
// Function Pointer Tables
//------------------------------------------------------------------
// opType: 0=ADD, 1=SUB, 2=MUL, 3=MAX, 4=MIN, 5=ADD2

typedef void (*NEONarithPS)(microVU&, const a64::VRegister&, const a64::VRegister&, int);

static NEONarithPS const NEON_PS[] = {
	NEON_ADDPS,  // 0
	NEON_SUBPS,  // 1
	NEON_MULPS,  // 2
	NEON_MAXPS,  // 3
	NEON_MINPS,  // 4
	NEON_ADD2PS, // 5
};

static NEONarithPS const NEON_SS[] = {
	NEON_ADDSS,  // 0
	NEON_SUBSS,  // 1
	NEON_MULSS,  // 2
	NEON_MAXSS,  // 3
	NEON_MINSS,  // 4
	NEON_ADD2SS, // 5
};

//------------------------------------------------------------------
// Single Scalar Lane Rotation
//------------------------------------------------------------------
// For _XYZW_SS2 (single scalar, NOT X): rotate the target lane into
// lane 0 for scalar operations, then rotate back afterward.
// Uses EXT to rotate the 128-bit vector by N lanes.

// Rotate vector so that lane 'offsetReg' moves to lane 0.
// offsetReg: 0=X(nop), 1=Y, 2=Z, 3=W
static void shuffleSSto0(const a64::VRegister& reg, int lane)
{
	if (lane == 0) return;
	// EXT #(lane*4) rotates left by lane*4 bytes, bringing lane N to position 0
	armAsm->Ext(reg.V16B(), reg.V16B(), reg.V16B(), lane * 4);
}

// Rotate vector back: undo the rotation done by shuffleSSto0.
static void shuffleSSfrom0(const a64::VRegister& reg, int lane)
{
	if (lane == 0) return;
	// Rotate right by lane*4 bytes = rotate left by (16 - lane*4)
	armAsm->Ext(reg.V16B(), reg.V16B(), reg.V16B(), (4 - lane) * 4);
}

//------------------------------------------------------------------
// Clamp Modes
//------------------------------------------------------------------

enum clampModes
{
	cFt  = 0x01, // Clamp Ft / I-reg / Q-reg
	cFs  = 0x02, // Clamp Fs
	cACC = 0x04, // Clamp ACC
};

// Whether the lane-fold multiply is a multiply by VF0.w, which is +1.0, of an
// Fs the body has just clamped with FMINNM/FMAXNM under a VU FPCR with FZ. FZ
// flushes a denormal input of that clamp to zero and the clamp brings exponent
// 255 down to the finite bound, so Fs holds a normal number or a zero, and x *
// 1.0 is x, bit for bit, for every such word in every rounding mode. Then the
// multiply is the identity and is not emitted. (The sign-preserving clamp
// passes denormals through, which the multiply would flush, so it does not
// qualify.)
static __fi bool mVUlaneMulIsIdentity(mV, const a64::VRegister& Fs, int opCase, int lane, int clampType)
{
	const bool ftz = (isVU0 ? EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR).GetFlushToZero();
	return opCase == 2 && _Ft_ == 0 && lane == 3 && (clampType & cFs) && ftz
	       && mVUclamp1Emits(mVU, false) && !mVUclamp2Signs(mVU, false)
	       && mVU.regAlloc->checkVFClamp(Fs.GetCode());
}

// The lane-fold multiply of an FMAC body, unless it is the identity.
// `scalar`: the body computes its one lane in lane 0 (the rotated form).
static __fi void mVUemitBodyLaneMul(mV, const a64::VRegister& Fs, const a64::VRegister& Ft, int lane,
	int opCase, int clampType, bool scalar)
{
	if (!mVUlaneMulIsIdentity(mVU, Fs, opCase, lane, clampType))
		mVUemitLaneMul(mVU, Fs, Ft, lane, scalar);
}

// Whether a single-lane FMAC body computes its lane in place, full width,
// instead of rotating it (and ACC) to lane 0 for scalar arithmetic and back.
// Each lane of a vector op is the scalar op on that lane's words, so the
// written lane is the same word either way. The other lanes of a single-lane
// destination are dead: allocReg's natural slots write back and merge just
// that lane, and mVUupdateFlags takes the single-scalar weights from it
// (ssLane), so every MAC and STATUS bit is unchanged. Only below clampE,
// where the step's only clamp is the body's own operand clamp, run full width
// on the clone; at clampE the steps clamp ACC as an operand, and a full-width
// clamp would rewrite ACC's other lanes in its cached register. X needs no
// rotation in the first place. The TriAce ADDi path reads lane 0 by hand.
static __fi bool mVUssNatural(mV, int opType)
{
	return _XYZW_SS2 && !clampE && !(opType == 5 && CHECK_VUADDSUBHACK && !CHECK_VU_EXACT(mVU.index));
}

static __fi int mVUssLane(mV)
{
	return _X ? 0 : (_Y ? 1 : (_Z ? 2 : 3));
}

//------------------------------------------------------------------
// Logging Helper
//------------------------------------------------------------------

static void mVU_printOP(microVU& mVU, int opCase, microOpcode opEnum, bool isACC)
{
	mVUlog(microOpcodeName[opEnum]);
	opCase1 { if (isACC) { mVUlogACC(); } else { mVUlogFd(); } mVUlogFt(); }
	opCase2 { if (isACC) { mVUlogACC(); } else { mVUlogFd(); } mVUlogBC(); }
	opCase3 { if (isACC) { mVUlogACC(); } else { mVUlogFd(); } mVUlogI();  }
	opCase4 { if (isACC) { mVUlogACC(); } else { mVUlogFd(); } mVUlogQ();  }
}

//------------------------------------------------------------------
// Pass 1 Setup (Analysis — platform-independent)
//------------------------------------------------------------------

static void setupPass1(microVU& mVU, int opCase, bool isACC, bool noFlagUpdate)
{
	opCase1 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, _Ft_); }
	opCase2 { mVUanalyzeFMAC3(mVU, ((isACC) ? 0 : _Fd_), _Fs_, _Ft_); }
	opCase3 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, 0); }
	opCase4 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, 0); }

	if (noFlagUpdate) // Max/Min ops
		sFLAG.doFlag = false;
}

//------------------------------------------------------------------
// Safe Subtraction — X minus X = 0 (avoids NaN from inf-inf)
//------------------------------------------------------------------

static bool doSafeSub(microVU& mVU, int opCase, int opType, bool isACC)
{
	opCase1
	{
		if ((opType == 1) && (_Ft_ == _Fs_) && (opCase == 1))
		{
			const a64::VRegister& Fs = mVU.regAlloc->allocReg(-1, isACC ? 32 : _Fd_, _X_Y_Z_W);
			armAsm->Movi(Fs.V4S(), 0); // Set to positive zero
			mVUupdateFlags(mVU, Fs);
			mVU.regAlloc->clearNeeded(Fs);
			return true;
		}
	}
	return false;
}

//------------------------------------------------------------------
// Ft Register Setup for Normal/BC/I/Q Cases
//------------------------------------------------------------------

// bcLane: out-param for the AX-14 lane-indexed FMUL broadcast fold. -1 = Ft is
// a materialized full-width operand (the normal path). 0..3 = Ft is the RAW
// source VF register (read-only allocator mapping, broadcast NOT materialized)
// and the caller's multiply step must fold the lane select into a by-element
// FMUL on that lane. Only set when canLaneFold (the sole Ft consumer is the
// multiply step) and no Ft clamp will be emitted.
static void setupFtReg(microVU& mVU, a64::VRegister& Ft, a64::VRegister& tempFt, int opCase, int clampType, int& bcLane, bool canLaneFold,
	bool natural = false)
{
	bcLane = -1;
	// Whether any clamp of Ft is emitted: the body's own cFt operand clamp, or
	// the clampE operand/result clamps inside NEON_MULPS/SS.
	const bool willClamp = (clampE || ((clampType & cFt) && !clampE && (CHECK_VU_OVERFLOW(mVU.index) || CHECK_VU_SIGN_OVERFLOW(mVU.index))));
	opCase1
	{
		// A natural body reads Ft in place, full width (it never clamps Ft:
		// below clampE no single-lane op has cFt).
		if (natural)        { Ft = mVU.regAlloc->allocReg(_Ft_);                tempFt = a64::NoVReg; }
		else if (_XYZW_SS2) { Ft = mVU.regAlloc->allocReg(_Ft_, 0, _X_Y_Z_W); tempFt = Ft; }
		else if (willClamp) { Ft = mVU.regAlloc->allocReg(_Ft_, 0, 0xf);       tempFt = Ft; }
		else                { Ft = mVU.regAlloc->allocReg(_Ft_);                tempFt = a64::NoVReg; }
	}
	opCase2
	{
		// AX-14 (idea from ARMSX2's tryEmitFmulLaneBroadcast — Tyler Bochard,
		// GPLv3): when the only consumer is the multiply step and no Ft clamp
		// will be emitted, skip materializing the broadcast (scratch allocReg +
		// Dup) and let the consumer emit Fmul Vd.4S, Vn.4S, Vm.S[_bc_] —
		// bit-identical per lane (proven on the full 5818-capture corpus), one
		// fewer insn and one fewer NEON reg. willClamp mirrors the opCase1
		// expression: under !willClamp neither the body's clampType&cFt
		// mVUclamp2 nor NEON_MULPS's clampE-gated mVUclamp3/4 emit anything,
		// so bypassing NEON_MULPS loses no clamps (and the raw guest reg is
		// never clamped in place). x86 SSE cannot express this fold.
		//
		// The single-lane forms fold too: the scalar FMUL by element reads the
		// same word lane 0 of the broadcast would hold.
		if (canLaneFold && !willClamp)
		{
			Ft     = mVU.regAlloc->allocReg(_Ft_); // read-only mapping, held needed until the consumer's Fmul
			tempFt = a64::NoVReg;
			bcLane = _bc_;
			return;
		}
		tempFt = mVU.regAlloc->allocReg(_Ft_);
		Ft     = mVU.regAlloc->allocReg();
		mVUunpack_xyzw(Ft, tempFt, _bc_);
		mVU.regAlloc->clearNeeded(tempFt);
		tempFt = Ft;
	}
	opCase3
	{
		// A natural body takes I broadcast, as the multi-lane forms do. (Never
		// a read-only mapping of 33: nothing invalidates one when the next
		// I-bit immediate lands in REG_I.)
		Ft = mVU.regAlloc->allocReg(33, 0, natural ? 0xf : _X_Y_Z_W);
		tempFt = Ft;
	}
	opCase4
	{
		// The same fold for Q: the multiply reads its instance's lane of qmmPQ
		// by element instead of a broadcast of it (getQreg).
		if (canLaneFold && !willClamp)
		{
			Ft     = qmmPQ;
			tempFt = a64::NoVReg;
			bcLane = mVUinfo.readQ;
			return;
		}
		if (!clampE && _XYZW_SS && !mVUinfo.readQ && !natural)
		{
			Ft = qmmPQ;
			tempFt = a64::NoVReg;
		}
		else
		{
			Ft = mVU.regAlloc->allocReg();
			tempFt = Ft;
			getQreg(Ft, mVUinfo.readQ);
		}
	}
}

//------------------------------------------------------------------
// MAC U and MAC O — the exact models
//------------------------------------------------------------------
// armEmitVuMulOverflow, armEmitVuMulExactZero and armEmitVuAddSubOverflow
// (VuFmacFlags-arm64.h) carry the rules. Here is where they fit a microVU
// upper op.
//
// They read the operands, so they run before mVUclamp2 -- which at this mode
// moves an exponent-255 operand a binade down -- and before the arithmetic,
// which writes into Fs in place. The predicates are allocator temps rather than
// the fixed scratch, so they survive the clamps and the arithmetic between here
// and mVUupdateFlags; q29/q31 are the compute scratch, free at that point in
// every FMAC body, and the add's third goes in q30.
//
// Both sit at vuClampMode 4, on both VUs, unlike the macro path's VU0-only
// gate. O is ten instructions on every FMAC and the saturation ceiling reads
// it, so it is a value model as much as a flag one. U is three on a multiply,
// but it also holds an allocator temp across the arithmetic and puts
// mVUupdateFlags on a wider weight variant.
struct mVUfmacUO
{
	a64::VRegister underflow = a64::NoVReg;
	a64::VRegister overflow = a64::NoVReg;
	a64::VRegister sign = a64::NoVReg;
};

static bool mVUwantExactO(mV)
{
	return CHECK_VU_EXACT(mVU.index) && _X_Y_Z_W != 0;
}

// U is read by nothing but the flag registers, so it follows their liveness.
// O also picks the saturated lanes for mVUemitSaturateAtMax, so it does not.
static bool mVUwantExactU(mV)
{
	return CHECK_VU_EXACT(mVU.index) && _X_Y_Z_W != 0
	       && (sFLAG.doFlag || mFLAG.doFlag);
}

// A multiply's pair. Only a multiply gets U: a sum that underflows keeps its
// mantissa bits on the console and loses them here, behind a value divergence.
static mVUfmacUO mVUemitMulUO(mV, const a64::VRegister& a, const a64::VRegister& b)
{
	mVUfmacUO uo;
	if (mVUwantExactO(mVU))
	{
		uo.overflow = mVU.regAlloc->allocReg();
		armEmitVuMulOverflow(uo.overflow, a, b, RQSCRATCH2, RQSCRATCH3);
	}
	if (mVUwantExactU(mVU))
	{
		uo.underflow = mVU.regAlloc->allocReg();
		armEmitVuMulExactZero(uo.underflow, a, b, RQSCRATCH3);
	}
	return uo;
}

static mVUfmacUO mVUemitAddSubO(mV, const a64::VRegister& a, const a64::VRegister& b, bool issub)
{
	mVUfmacUO uo;
	if (!mVUwantExactO(mVU))
		return uo;
	uo.overflow = mVU.regAlloc->allocReg();
	armEmitVuAddSubOverflow(uo.overflow, a, b, issub, RQSCRATCH3, RQSCRATCH2, RQSCRATCH);
	// The ceiling substitutes the first addend's sign and the arithmetic writes
	// over that register, so it is copied rather than read back. The result
	// usually carries it anyway, mVUclamp3's integer clamp being sign
	// preserving, but a body that skips that clamp would take it off a NaN.
	uo.sign = mVU.regAlloc->allocReg();
	armAsm->Mov(uo.sign.V16B(), a.V16B());
	return uo;
}

// Which models an FMAC family gets, by opType: 0 ADD, 1 SUB, 2 MUL, 5 ADDi.
// MAX and MIN write no flags. MADD, MSUB, their A-forms and OPMSUB take none of
// this: their accumulate is handed a product the host has already saturated at
// FLT_MAX where the console accumulates the unsaturated one, and a NaN product
// arrives as +FLT_MAX too.
static mVUfmacUO mVUemitFmacUO(mV, int opType, const a64::VRegister& a, const a64::VRegister& b)
{
	if (opType == 2)
		return mVUemitMulUO(mVU, a, b);
	if (opType == 0 || opType == 5)
		return mVUemitAddSubO(mVU, a, b, false);
	if (opType == 1)
		return mVUemitAddSubO(mVU, a, b, true);
	return mVUfmacUO{};
}

// The saturated word, in the lanes the O predicate marks
// (armEmitVuSaturateAtMax). It runs on the arithmetic's register rather than a
// merged ACC: a dest field narrower than xyzw leaves the other lanes of an ACC
// clone standing for the writeback, and the O predicate says nothing useful
// about them. A multiply has no `uo.sign` and takes the sign off its own
// result; RQSCRATCH3 is dead by here in every FMAC body.
static void mVUemitSaturateAtMax(mV, const a64::VRegister& dst, const mVUfmacUO& uo)
{
	if (!uo.overflow.IsValid())
		return;
	armEmitVuSaturateAtMax(dst, uo.overflow, uo.sign.IsValid() ? uo.sign : dst, RQSCRATCH3);
}

static void mVUclearUO(mV, const mVUfmacUO& uo)
{
	if (uo.underflow.IsValid())
		mVU.regAlloc->clearNeeded(uo.underflow);
	if (uo.sign.IsValid())
		mVU.regAlloc->clearNeeded(uo.sign);
	if (uo.overflow.IsValid())
		mVU.regAlloc->clearNeeded(uo.overflow);
}

//------------------------------------------------------------------
// mVU_FMACa — Normal FMAC Opcodes (ADD/SUB/MUL/MAX/MIN and ACC variants)
//------------------------------------------------------------------

static void mVU_FMACa(microVU& mVU, int recPass, int opCase, int opType, bool isACC, microOpcode opEnum, int clampType)
{
	pass1 { setupPass1(mVU, opCase, isACC, ((opType == 3) || (opType == 4))); }
	pass2
	{
		if (doSafeSub(mVU, opCase, opType, isACC))
			return;

		a64::VRegister Fs = a64::NoVReg;
		a64::VRegister Ft = a64::NoVReg;
		a64::VRegister ACC = a64::NoVReg;
		a64::VRegister tempFt = a64::NoVReg;
		int bcLane = -1;

		const bool natural = mVUssNatural(mVU, opType);
		const bool scalar = _XYZW_SS && !natural;
		setupFtReg(mVU, Ft, tempFt, opCase, clampType, bcLane, opType == 2, natural);

		// A full-width ACC write needs no ACC register of its own: the result is
		// computed in Fs's clone and that register becomes ACC's written slot
		// afterwards (rebindFullWrite), instead of being copied into one.
		const bool accInFs = isACC && (_X_Y_Z_W == 0xf);
		if (isACC)
		{
			Fs  = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W, true, natural);
			if (!accInFs)
				ACC = mVU.regAlloc->allocReg(32, 32, 0xf, false);
			if (_XYZW_SS2 && !natural)
				shuffleSSto0(ACC, offsetReg); // Rotate target lane to lane 0
		}
		else
		{
			Fs = mVU.regAlloc->allocReg(_Fs_, _Fd_, _X_Y_Z_W, true, natural);
		}

		// Before the clamps and before the arithmetic overwrites Fs. The AX-14
		// fold cannot be live here: it needs !willClamp, which the models' own
		// mode rules out.
		const mVUfmacUO uo = mVUemitFmacUO(mVU, opType, Fs, Ft);
		pxAssert(!uo.overflow.IsValid() || bcLane < 0);

		// A single-lane ADD/SUB/MUL writes Fs with scalar arithmetic next
		// (mVUclampOperands' scalarDst). MAX/MINI clamp nothing.
		mVUclampOperands(mVU, {((clampType & cFt) && bcLane < 0) ? Ft : a64::NoVReg,
		                       (clampType & cFs) ? Fs : a64::NoVReg}, natural ? 0xf : _X_Y_Z_W, false,
		                 (scalar && opType != 3 && opType != 4) ? Fs : a64::NoVReg);

		// Lane fold (== broadcast + NEON_MULPS/SS under the no-clamp gate).
		// MAX/MINI with VF0 on either side: see mVUemitMaxMinSignClear. Ft
		// holds VF0 itself, its broadcast lane, or (single lane) its rotated
		// lane; Fs, a clone of it.
		const bool vf0MaxMin = (opType == 3 || opType == 4) && (opCase == 1 || opCase == 2)
		                       && (_Ft_ == 0 || _Fs_ == 0);

		if (bcLane >= 0)   mVUemitBodyLaneMul(mVU, Fs, Ft, bcLane, opCase, clampType, scalar);
		else if (vf0MaxMin) mVUemitMaxMinSignClear(mVU, Fs, Ft, opType == 3);
		else if (scalar)   NEON_SS[opType](mVU, Fs, Ft, 0);
		else               NEON_PS[opType](mVU, Fs, Ft, 0);

		mVUemitSaturateAtMax(mVU, Fs, uo);

		if (accInFs)
		{
			mVU.regAlloc->rebindFullWrite(Fs, 32);
			mVUupdateFlags(mVU, Fs, tempFt, a64::NoVReg, true, uo.underflow, uo.overflow);
		}
		else if (isACC)
		{
			if (natural)
				armAsm->Ins(ACC.V4S(), mVUssLane(mVU), Fs.V4S(), mVUssLane(mVU));
			else if (_XYZW_SS)
				armAsm->Ins(ACC.V4S(), 0, Fs.V4S(), 0); // MOVSS equivalent
			else
				mVUmergeRegs(ACC, Fs, _X_Y_Z_W);
			mVUupdateFlags(mVU, ACC, Fs, tempFt, true, uo.underflow, uo.overflow, natural ? mVUssLane(mVU) : 0);
			if (_XYZW_SS2 && !natural)
				shuffleSSfrom0(ACC, offsetReg); // Rotate lane 0 back to original position
			mVU.regAlloc->clearNeeded(ACC);
		}
		else if (opType < 3 || opType == 5) // Not Min/Max or is ADDi (opType 5)
		{
			mVUupdateFlags(mVU, Fs, tempFt, a64::NoVReg, true, uo.underflow, uo.overflow, natural ? mVUssLane(mVU) : 0);
		}

		mVUclearUO(mVU, uo);
		mVU.regAlloc->clearNeeded(Fs); // Always clear written reg first
		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opEnum);
	}
	pass3 { mVU_printOP(mVU, opCase, opEnum, isACC); }
	pass4
	{
		if ((opType != 3) && (opType != 4))
			mVUregs.needExactMatch |= 8;
	}
}

//------------------------------------------------------------------
// mVU_FMACb — MADDA/MSUBA Opcodes (MUL then ADD/SUB into ACC)
//------------------------------------------------------------------

static void mVU_FMACb(microVU& mVU, int recPass, int opCase, int opType, microOpcode opEnum, int clampType)
{
	pass1 { setupPass1(mVU, opCase, true, false); }
	pass2
	{
		a64::VRegister Fs = a64::NoVReg;
		a64::VRegister Ft = a64::NoVReg;
		a64::VRegister ACC = a64::NoVReg;
		a64::VRegister tempFt = a64::NoVReg;
		int bcLane = -1;

		const bool natural = mVUssNatural(mVU, opType);
		const bool scalar = _XYZW_SS && !natural;
		setupFtReg(mVU, Ft, tempFt, opCase, clampType, bcLane, true, natural);

		Fs  = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W, true, natural);
		ACC = mVU.regAlloc->allocReg(32, 32, 0xf, false);

		if (_XYZW_SS2 && !natural)
			shuffleSSto0(ACC, offsetReg); // Rotate target lane to lane 0

		mVUclampOperands(mVU, {((clampType & cFt) && bcLane < 0) ? Ft : a64::NoVReg,
		                       (clampType & cFs) ? Fs : a64::NoVReg}, natural ? 0xf : _X_Y_Z_W, false,
		                 scalar ? Fs : a64::NoVReg);

		// mVUclamp4 does not run behind the AX-14 lane fold, which replaces
		// NEON_*[2], nor in the sign-preserving mode, which clamps operands only.
		const bool prodClamped = (bcLane < 0) && !CHECK_VU_SIGN_OVERFLOW(mVU.index);

		// Step 1: Multiply Fs * Ft
		if (bcLane >= 0)   mVUemitBodyLaneMul(mVU, Fs, Ft, bcLane, opCase, clampType, scalar);
		else if (scalar)   NEON_SS[2](mVU, Fs, Ft, 0);
		else               NEON_PS[2](mVU, Fs, Ft, 0);

		// Step 2: ADD/SUB the product to/from ACC
		if (_XYZW_SS || _X_Y_Z_W == 0xf)
		{
			if (_XYZW_SS)
			{
				// ACC is written back with a full 0xf mask, so its non-target
				// lanes must survive this single-lane accumulate. AArch64 scalar
				// FP writes ZERO the upper lanes of the dest V register (unlike
				// x86 ADDSS/SUBSS, which preserve them — the x86 mVU relies on
				// that). Accumulate on a scratch copy and merge only lane 0 back,
				// mirroring the load+Ins pattern mVU_FMACa uses for its ACC.
				// A natural body accumulates full width and inserts its own lane.
				const a64::VRegister& accSS = mVU.regAlloc->allocReg();
				armAsm->Mov(accSS.V16B(), ACC.V16B());
				mVU.regAlloc->noteClone(accSS.GetCode(), ACC.GetCode());
				if (natural)
				{
					NEON_PS[opType](mVU, accSS, Fs, prodClamped ? preClampFrom : 0);
					armAsm->Ins(ACC.V4S(), mVUssLane(mVU), accSS.V4S(), mVUssLane(mVU));
				}
				else
				{
					NEON_SS[opType](mVU, accSS, Fs, prodClamped ? preClampFrom : 0);
					armAsm->Ins(ACC.V4S(), 0, accSS.V4S(), 0);
				}
				mVU.regAlloc->clearNeeded(accSS);
			}
			else
			{
				NEON_PS[opType](mVU, ACC, Fs, prodClamped ? preClampFrom : 0);
			}
			mVUupdateFlags(mVU, ACC, Fs, tempFt, true, a64::NoVReg, a64::NoVReg, natural ? mVUssLane(mVU) : 0);
			if (_XYZW_SS && _X_Y_Z_W != 8 && !natural)
				shuffleSSfrom0(ACC, offsetReg); // Rotate back
		}
		else
		{
			const a64::VRegister& tempACC = mVU.regAlloc->allocReg();
			armAsm->Mov(tempACC.V16B(), ACC.V16B());
			mVU.regAlloc->noteClone(tempACC.GetCode(), ACC.GetCode());
			NEON_PS[opType](mVU, tempACC, Fs, prodClamped ? preClampFrom : 0);
			mVUmergeRegs(ACC, tempACC, _X_Y_Z_W);
			mVUupdateFlags(mVU, ACC, Fs, tempFt);
			mVU.regAlloc->clearNeeded(tempACC);
		}

		mVU.regAlloc->clearNeeded(ACC);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opEnum);
	}
	pass3 { mVU_printOP(mVU, opCase, opEnum, true); }
	pass4 { mVUregs.needExactMatch |= 8; }
}

//------------------------------------------------------------------
// mVU_FMACc — MADD Opcodes (MUL then ADD from ACC into Fd)
//------------------------------------------------------------------
// No FMA: separate MUL then ADD (matches PS2 rounding)

static void mVU_FMACc(microVU& mVU, int recPass, int opCase, microOpcode opEnum, int clampType)
{
	pass1 { setupPass1(mVU, opCase, false, false); }
	pass2
	{
		a64::VRegister Fs = a64::NoVReg;
		a64::VRegister Ft = a64::NoVReg;
		a64::VRegister ACC = a64::NoVReg;
		a64::VRegister tempFt = a64::NoVReg;
		int bcLane = -1;

		const bool natural = mVUssNatural(mVU, 2);
		const bool scalar = _XYZW_SS && !natural;
		setupFtReg(mVU, Ft, tempFt, opCase, clampType, bcLane, true, natural);

		ACC = mVU.regAlloc->allocReg(32);
		Fs  = mVU.regAlloc->allocReg(_Fs_, _Fd_, _X_Y_Z_W, true, natural);

		if (_XYZW_SS2 && !natural)
			shuffleSSto0(ACC, offsetReg); // Rotate target lane to lane 0

		mVUclampOperands(mVU, {((clampType & cFt) && bcLane < 0) ? Ft : a64::NoVReg,
		                       (clampType & cFs) ? Fs : a64::NoVReg,
		                       (clampType & cACC) ? ACC : a64::NoVReg}, natural ? 0xf : _X_Y_Z_W, false,
		                 scalar ? Fs : a64::NoVReg);

		const bool prodClamped = (bcLane < 0) && !CHECK_VU_SIGN_OVERFLOW(mVU.index);

		// Step 1: Fs = Fs * Ft
		// Step 2: Fs = Fs + ACC
		if (bcLane >= 0) mVUemitBodyLaneMul(mVU, Fs, Ft, bcLane, opCase, clampType, scalar);
		else if (scalar) NEON_SS[2](mVU, Fs, Ft, 0);
		else             NEON_PS[2](mVU, Fs, Ft, 0);
		if (scalar) NEON_SS[0](mVU, Fs, ACC, prodClamped ? preClampTo : 0);
		else        NEON_PS[0](mVU, Fs, ACC, prodClamped ? preClampTo : 0);

		if (_XYZW_SS2 && !natural)
			shuffleSSfrom0(ACC, offsetReg); // Rotate back

		mVUupdateFlags(mVU, Fs, tempFt, a64::NoVReg, true, a64::NoVReg, a64::NoVReg, natural ? mVUssLane(mVU) : 0);

		mVU.regAlloc->clearNeeded(Fs); // Always clear written reg first
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(ACC);
		mVU.profiler.EmitOp(opEnum);
	}
	pass3 { mVU_printOP(mVU, opCase, opEnum, false); }
	pass4 { mVUregs.needExactMatch |= 8; }
}

//------------------------------------------------------------------
// mVU_FMACd — MSUB Opcodes (ACC - Fs*Ft into Fd)
//------------------------------------------------------------------

static void mVU_FMACd(microVU& mVU, int recPass, int opCase, microOpcode opEnum, int clampType)
{
	pass1 { setupPass1(mVU, opCase, false, false); }
	pass2
	{
		a64::VRegister Fs = a64::NoVReg;
		a64::VRegister Ft = a64::NoVReg;
		a64::VRegister Fd = a64::NoVReg;
		a64::VRegister tempFt = a64::NoVReg;
		int bcLane = -1;

		const bool natural = mVUssNatural(mVU, 1);
		const bool scalar = _XYZW_SS && !natural;
		setupFtReg(mVU, Ft, tempFt, opCase, clampType, bcLane, true, natural);

		// A natural body takes its ACC-to-Fd clone after the multiply, so the
		// clone is the word in front of the subtract, which then reads ACC's
		// register in its place (mVUstepSource). It has no ACC clamp to order:
		// cACC is COP2-only, and natural is below clampE.
		Fs = mVU.regAlloc->allocReg(_Fs_,  0, _X_Y_Z_W, true, natural);
		if (!natural)
			Fd = mVU.regAlloc->allocReg(32, _Fd_, _X_Y_Z_W);

		mVUclampOperands(mVU, {((clampType & cFt) && bcLane < 0) ? Ft : a64::NoVReg,
		                       (clampType & cFs) ? Fs : a64::NoVReg,
		                       (clampType & cACC) ? Fd : a64::NoVReg}, natural ? 0xf : _X_Y_Z_W, false,
		                 scalar ? Fs : a64::NoVReg);

		const bool prodClamped = (bcLane < 0) && !CHECK_VU_SIGN_OVERFLOW(mVU.index);

		// Step 1: Fs = Fs * Ft
		// Step 2: Fd = Fd - Fs  (Fd starts as ACC)
		if (bcLane >= 0) mVUemitBodyLaneMul(mVU, Fs, Ft, bcLane, opCase, clampType, scalar);
		else if (scalar) NEON_SS[2](mVU, Fs, Ft, 0);
		else             NEON_PS[2](mVU, Fs, Ft, 0);
		if (natural)
			Fd = mVU.regAlloc->allocReg(32, _Fd_, _X_Y_Z_W, true, true);
		if (scalar) NEON_SS[1](mVU, Fd, Fs, prodClamped ? preClampFrom : 0);
		else        NEON_PS[1](mVU, Fd, Fs, prodClamped ? preClampFrom : 0);

		mVUupdateFlags(mVU, Fd, Fs, tempFt, true, a64::NoVReg, a64::NoVReg, natural ? mVUssLane(mVU) : 0);

		mVU.regAlloc->clearNeeded(Fd); // Always clear written reg first
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opEnum);
	}
	pass3 { mVU_printOP(mVU, opCase, opEnum, false); }
	pass4 { mVUregs.needExactMatch |= 8; }
}

//------------------------------------------------------------------
// ABS Opcode
//------------------------------------------------------------------

mVUop(mVU_ABS)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		if (!_Ft_)
			return;
		const a64::VRegister& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));
		// PS2 ABS clears the sign bit per lane. Fabs is the dedicated single
		// insn for that on aarch64 and matches AND-with-0x7fffffff for every
		// IEEE bit pattern (normal, denormal, Inf, NaN — sign cleared, payload
		// preserved).
		armAsm->Fabs(Fs.V4S(), Fs.V4S());
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opABS);
	}
	pass3
	{
		mVUlog("ABS");
		mVUlogFtFs();
	}
}

//------------------------------------------------------------------
// The OP ops — cross-product FMACs
//------------------------------------------------------------------
//
//     OPMULA: ACC.<dest> = {Fs.y, Fs.z, Fs.x, Fs.w} * {Ft.z, Ft.x, Ft.y, +0}
//     OPMSUB: Fd.<dest>  = ACC - the same product
//
// Lane w's second multiplicand is a hard +0, not Ft.w, so the product there is
// a zero carrying Fs.w's sign and nothing of Ft.
//
// Both take their multiplicands full width and reach the destination register
// separately, which the rotation forces: allocReg's single-scalar path loads
// the dest lane alone into lane 0, and for a rotated op that is not the lane
// its operands come from. The product is rotated into lane 0 afterwards.

static void mVUopRotateToDestLane(mV, const a64::VRegister& reg)
{
	if (!_XYZW_SS || _X)
		return;
	armAsm->Ext(reg.V16B(), reg.V16B(), reg.V16B(), (_Y ? 1 : (_Z ? 2 : 3)) * 4);
}

// The two permuted multiplicands, built into `dst` from the unchanged
// source. Lane w is only built when the dest field writes w: otherwise it
// feeds nothing but a product lane the field masks off, which no register
// write, MAC weight or STATUS reduction reads (the weights take the dest
// field, and every O/U predicate reaches STATUS through them).
//
// Fs: {Y, Z, X, W}. EXT gives {Y, Z, W, X}; one INS puts X in lane 2.
static void mVUopPermuteFs(mV, const a64::VRegister& dst, const a64::VRegister& Fs)
{
	armAsm->Ext(dst.V16B(), Fs.V16B(), Fs.V16B(), 4); // {Y, Z, W, X}
	armAsm->Ins(dst.V4S(), 2, Fs.V4S(), 0);           // {Y, Z, X, X}
	if (_W)
		armAsm->Ins(dst.V4S(), 3, Fs.V4S(), 3);       // {Y, Z, X, W}
}

// Ft: {Z, X, Y, +0}. EXT gives {W, X, Y, Z}; one INS puts Z in lane 0.
static void mVUopPermuteFt(mV, const a64::VRegister& dst, const a64::VRegister& Ft)
{
	armAsm->Ext(dst.V16B(), Ft.V16B(), Ft.V16B(), 12); // {W, X, Y, Z}
	armAsm->Ins(dst.V4S(), 0, Ft.V4S(), 2);            // {Z, X, Y, Z}
	if (_W)
		armAsm->Ins(dst.V4S(), 3, a64::wzr);           // {Z, X, Y, +0}
}

mVUop(mVU_OPMULA)
{
	pass1 { mVUanalyzeFMAC1(mVU, 0, _Fs_, _Ft_); }
	pass2
	{
		// An empty dest field writes nothing and masks every MAC weight out, so
		// the destination is a plain temp rather than a clean copy of ACC.
		const a64::VRegister& Fs = mVU.regAlloc->allocReg(_Fs_);
		const a64::VRegister& ACC = mVU.regAlloc->allocReg(-1, _X_Y_Z_W ? 32 : -1, _X_Y_Z_W);
		mVUopPermuteFs(mVU, ACC, Fs);
		mVU.regAlloc->clearNeeded(Fs);

		// Ft is read where it is cached; the permuted copy is a temp.
		const a64::VRegister& FtSrc = mVU.regAlloc->allocReg(_Ft_);
		const a64::VRegister& Ft = mVU.regAlloc->allocReg();
		mVUopPermuteFt(mVU, Ft, FtSrc);
		mVU.regAlloc->clearNeeded(FtSrc);

		// An ordinary multiply, so it carries the multiply's U and O, taken from
		// the rotated multiplicands. The rotation to the dest lane comes after,
		// so the predicates take it too.
		const mVUfmacUO uo = mVUemitMulUO(mVU, ACC, Ft);

		NEON_MULPS(mVU, ACC, Ft);
		mVU.regAlloc->clearNeeded(Ft);
		mVUopRotateToDestLane(mVU, ACC);
		if (uo.overflow.IsValid())
			mVUopRotateToDestLane(mVU, uo.overflow);
		if (uo.underflow.IsValid())
			mVUopRotateToDestLane(mVU, uo.underflow);
		mVUemitSaturateAtMax(mVU, ACC, uo);
		mVUupdateFlags(mVU, ACC, a64::NoVReg, a64::NoVReg, true, uo.underflow, uo.overflow);
		mVUclearUO(mVU, uo);
		mVU.regAlloc->clearNeeded(ACC);
		mVU.profiler.EmitOp(opOPMULA);
	}
	pass3
	{
		mVUlog("OPMULA");
		mVUlogACC();
		mVUlogFt();
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

//------------------------------------------------------------------
// OPMSUB Opcode — Cross product subtract from ACC
//------------------------------------------------------------------

mVUop(mVU_OPMSUB)
{
	pass1 { mVUanalyzeFMAC1(mVU, _Fd_, _Fs_, _Ft_); }
	pass2
	{
		// Both multiplicands are read where they are cached and permuted into
		// temps, so neither needs a clone of its own.
		const a64::VRegister& FtSrc = mVU.regAlloc->allocReg(_Ft_);
		const a64::VRegister& FsSrc = mVU.regAlloc->allocReg(_Fs_);
		const a64::VRegister& Ft = mVU.regAlloc->allocReg();
		const a64::VRegister& Fs = mVU.regAlloc->allocReg();
		mVUopPermuteFs(mVU, Fs, FsSrc);
		mVUopPermuteFt(mVU, Ft, FtSrc);
		mVU.regAlloc->clearNeeded(FsSrc);
		mVU.regAlloc->clearNeeded(FtSrc);

		NEON_MULPS(mVU, Fs,  Ft);
		mVUopRotateToDestLane(mVU, Fs);

		// The ACC side of an empty dest field, for the reason in mVU_OPMULA:
		// a full-width clone bound to VF0, which the allocator discards.
		// Allocated last, so its clone is the word in front of the subtract,
		// which reads ACC's register in its place (mVUstepSource).
		const a64::VRegister& ACC = _X_Y_Z_W
			? mVU.regAlloc->allocReg(32, _Fd_, _X_Y_Z_W)
			: mVU.regAlloc->allocReg(32, 0, 0xf);
		NEON_SUBPS(mVU, ACC, Fs);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVUupdateFlags(mVU, ACC);
		mVU.regAlloc->clearNeeded(ACC);
		mVU.profiler.EmitOp(opOPMSUB);
	}
	pass3
	{
		mVUlog("OPMSUB");
		mVUlogFd();
		mVUlogFt();
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

//------------------------------------------------------------------
// FTOI0/4/12/15 — Float to Int conversion
//------------------------------------------------------------------
// FCVTZS: converts float to signed int, truncating toward zero.
// For unrepresentable positive values, x86 CVTTPS2DQ returns 0x80000000
// and then XORs with 0xffffffff to get 0x7fffffff. ARM64 FCVTZS
// saturates to INT_MAX (0x7fffffff) or INT_MIN (0x80000000) natively for
// finite overflow and ±Inf, so no correction XOR is needed there.
//
// NaN, however, diverges: ARM64 Fcvtzs(NaN)=0, but the interp's floatToInt
// (VUops.cpp) and x86 PCSX2 mVU both saturate a NaN by its sign bit —
// positive NaN → 0x7fffffff, negative NaN → 0x80000000 (exp 0xFF satisfies
// the interp's `>= 0x4f000000` saturation test). Feeding a NaN into FTOI0
// would otherwise produce JIT=0 vs interp=0x7fffffff. NaN lanes are therefore
// patched to the sign-based INT saturation, matching the interp and x86
// exactly (regression-pinned in vu_ftoi_saturation_tests.cpp).

static void mVU_FTOIx(mP, const float* addr, microOpcode opEnum)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		if (!_Ft_)
			return;
		const a64::VRegister& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));

		if (addr)
		{
			// Scale by fixed-point multiplier before conversion
			armAsm->Ldr(RQSCRATCH3, mVUglobMem(addr));
			armAsm->Fmul(Fs.V4S(), Fs.V4S(), RQSCRATCH3.V4S());
		}

		// NaN saturation correction (see header comment): build, BEFORE the
		// convert clobbers Fs, a per-lane mask of which lanes are NaN and the
		// sign-based saturation value for those lanes.
		const a64::VRegister& notNan = mVU.regAlloc->allocReg(); // 0xffffffff where NOT NaN
		const a64::VRegister& sat    = mVU.regAlloc->allocReg(); // sign ? INT_MIN : INT_MAX
		armAsm->Fcmeq(notNan.V4S(), Fs.V4S(), Fs.V4S());         // a==a is false only for NaN
		armAsm->Sshr(sat.V4S(), Fs.V4S(), 31);                   // 0xffffffff if sign set, else 0
		armAsm->Ldr(RQSCRATCH3, mVUglobMem(&mVUglob.absclip[0])); // 0x7fffffff (INT_MAX bits)
		armAsm->Eor(sat.V16B(), sat.V16B(), RQSCRATCH3.V16B());  // +NaN→0x7fffffff, -NaN→0x80000000

		// Convert float to signed int (truncating toward zero). Finite overflow
		// and ±Inf saturate correctly here; NaN lanes become 0 and are fixed up.
		armAsm->Fcvtzs(Fs.V4S(), Fs.V4S());

		// Where notNan==0 (a NaN lane), replace the Fcvtzs 0 with the saturation
		// value; non-NaN lanes keep the converted result. BIF: dst bit <- src bit
		// where mask bit is 0.
		armAsm->Bif(Fs.V16B(), sat.V16B(), notNan.V16B());

		mVU.regAlloc->clearNeeded(notNan);
		mVU.regAlloc->clearNeeded(sat);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opEnum);
	}
	pass3
	{
		mVUlog(microOpcodeName[opEnum]);
		mVUlogFtFs();
	}
}

//------------------------------------------------------------------
// ITOF0/4/12/15 — Int to Float conversion
//------------------------------------------------------------------

static void mVU_ITOFx(mP, const float* addr, microOpcode opEnum)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		if (!_Ft_)
			return;
		const a64::VRegister& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));

		// Convert signed int to float
		armAsm->Scvtf(Fs.V4S(), Fs.V4S());

		if (addr)
		{
			// Scale by fixed-point divisor after conversion
			armAsm->Ldr(RQSCRATCH3, mVUglobMem(addr));
			armAsm->Fmul(Fs.V4S(), Fs.V4S(), RQSCRATCH3.V4S());
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opEnum);
	}
	pass3
	{
		mVUlog(microOpcodeName[opEnum]);
		mVUlogFtFs();
	}
}

//------------------------------------------------------------------
// CLIP Opcode — Clip flag computation
//------------------------------------------------------------------

mVUop(mVU_CLIP)
{
	pass1 { mVUanalyzeFMAC4(mVU, _Fs_, _Ft_); }
	pass2
	{
		// Fs and Ft are read where they are cached; every register written
		// below is a temp. Fn is the -x vector.
		const a64::VRegister& Fs = mVU.regAlloc->allocReg(_Fs_);
		const a64::VRegister& FtSrc = mVU.regAlloc->allocReg(_Ft_);
		const a64::VRegister& Ft = mVU.regAlloc->allocReg();
		const a64::VRegister& t1 = mVU.regAlloc->allocReg();
		const a64::VRegister& Fn = mVU.regAlloc->allocReg();

		// Broadcast Ft.w to all lanes
		mVUunpack_xyzw(Ft, FtSrc, 3);
		mVU.regAlloc->clearNeeded(FtSrc);

		// Get previous clip flag and shift left by 6
		mVUallocCFLAGa(mVU, gprT1, cFLAG.lastWrite);
		armAsm->Lsl(gprT1.W(), gprT1.W(), 6);

		// Denormal check: if exponent is zero, treat as zero. Use the
		// immediate-zero form of CMEQ to avoid materialising a zero vector.
		armAsm->Ldr(RQSCRATCH3, mVUglobMem(&mVUglob.exponent[0]));
		armAsm->And(t1.V16B(), Fs.V16B(), RQSCRATCH3.V16B());
		armAsm->Cmeq(t1.V4S(), t1.V4S(), 0); // All 1s where denormal
		armAsm->Bic(t1.V16B(), Fs.V16B(), t1.V16B()); // Zero out denormals

		// |Ft.w| (absolute value)
		armAsm->Ldr(RQSCRATCH3, mVUglobMem(&mVUglob.absclip[0]));
		armAsm->And(Ft.V16B(), Ft.V16B(), RQSCRATCH3.V16B());

		// Negate Fs (for -w comparison)
		armAsm->Ldr(RQSCRATCH3, mVUglobMem(&mVUglob.signbit[0]));
		armAsm->Eor(Fn.V16B(), t1.V16B(), RQSCRATCH3.V16B());

		// t1 > Ft means +component > +w: bit set
		armAsm->Cmgt(t1.V4S(), t1.V4S(), Ft.V4S());   // +x>+w, +y>+w, +z>+w, (ignored)
		// Fn > Ft means -component > +w (i.e., component < -w): bit set
		armAsm->Cmgt(Fn.V4S(), Fn.V4S(), Ft.V4S());    // -x>+w, -y>+w, -z>+w, (ignored)

		// Extract bits from the comparison results
		// t1 lanes: [0]=+x>w, [1]=+y>w, [2]=+z>w
		// Fn lanes: [0]=-x>w, [1]=-y>w, [2]=-z>w
		// Required layout: bit0=+x>w, bit1=-x>w, bit2=+y>w, bit3=-y>w, bit4=+z>w, bit5=-z>w
		//
		// A comparison leaves its lane all ones or all zero, so its low
		// halfword carries the answer whole: UZP1 on the halfword view packs
		// all eight into one register and one weight apiece moves each to its
		// bit. The weights are distinct powers of two, so the lane sum ADDV
		// forms is the OR the layout wants.
		armAsm->Ldr(RQSCRATCH3, mVUglobMem(&mVUglob.clipWeights[0]));
		armAsm->Uzp1(t1.V8H(), t1.V8H(), Fn.V8H());
		armAsm->And(t1.V16B(), t1.V16B(), RQSCRATCH3.V16B());
		armAsm->Addv(t1.H(), t1.V8H());
		armAsm->Umov(gprT2.W(), t1.V8H(), 0);

		// Combine with shifted previous clip flag; the weighted sum cannot
		// reach past bit 5, so it needs no mask of its own.
		armAsm->And(gprT1.W(), gprT1.W(), 0xffffff);
		armAsm->Orr(gprT1.W(), gprT1.W(), gprT2.W());

		mVUallocCFLAGb(mVU, gprT1, cFLAG.write);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(Fn);
		mVU.profiler.EmitOp(opCLIP);
	}
	pass3
	{
		mVUlog("CLIP");
		mVUlogCLIP();
	}
}

//------------------------------------------------------------------
// Micro VU Micromode Upper Instructions — Opcode Dispatch
//------------------------------------------------------------------

mVUop(mVU_ADD)    { mVU_FMACa(mVU, recPass, 1, 0, false, opADD,    0);  }
mVUop(mVU_ADDi)   { mVU_FMACa(mVU, recPass, 3, 5, false, opADDi,   0);  }
mVUop(mVU_ADDq)   { mVU_FMACa(mVU, recPass, 4, 0, false, opADDq,   0);  }
mVUop(mVU_ADDx)   { mVU_FMACa(mVU, recPass, 2, 0, false, opADDx,   0);  }
mVUop(mVU_ADDy)   { mVU_FMACa(mVU, recPass, 2, 0, false, opADDy,   0);  }
mVUop(mVU_ADDz)   { mVU_FMACa(mVU, recPass, 2, 0, false, opADDz,   0);  }
mVUop(mVU_ADDw)   { mVU_FMACa(mVU, recPass, 2, 0, false, opADDw,   0);  }
mVUop(mVU_ADDA)   { mVU_FMACa(mVU, recPass, 1, 0, true,  opADDA,   0);  }
mVUop(mVU_ADDAi)  { mVU_FMACa(mVU, recPass, 3, 0, true,  opADDAi,  0);  }
mVUop(mVU_ADDAq)  { mVU_FMACa(mVU, recPass, 4, 0, true,  opADDAq,  0);  }
mVUop(mVU_ADDAx)  { mVU_FMACa(mVU, recPass, 2, 0, true,  opADDAx,  0);  }
mVUop(mVU_ADDAy)  { mVU_FMACa(mVU, recPass, 2, 0, true,  opADDAy,  0);  }
mVUop(mVU_ADDAz)  { mVU_FMACa(mVU, recPass, 2, 0, true,  opADDAz,  0);  }
mVUop(mVU_ADDAw)  { mVU_FMACa(mVU, recPass, 2, 0, true,  opADDAw,  0);  }
mVUop(mVU_SUB)    { mVU_FMACa(mVU, recPass, 1, 1, false, opSUB,  (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBi)   { mVU_FMACa(mVU, recPass, 3, 1, false, opSUBi, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBq)   { mVU_FMACa(mVU, recPass, 4, 1, false, opSUBq, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBx)   { mVU_FMACa(mVU, recPass, 2, 1, false, opSUBx, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBy)   { mVU_FMACa(mVU, recPass, 2, 1, false, opSUBy, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBz)   { mVU_FMACa(mVU, recPass, 2, 1, false, opSUBz, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBw)   { mVU_FMACa(mVU, recPass, 2, 1, false, opSUBw, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBA)   { mVU_FMACa(mVU, recPass, 1, 1, true,  opSUBA,   0);  }
mVUop(mVU_SUBAi)  { mVU_FMACa(mVU, recPass, 3, 1, true,  opSUBAi,  0);  }
mVUop(mVU_SUBAq)  { mVU_FMACa(mVU, recPass, 4, 1, true,  opSUBAq,  0);  }
mVUop(mVU_SUBAx)  { mVU_FMACa(mVU, recPass, 2, 1, true,  opSUBAx,  0);  }
mVUop(mVU_SUBAy)  { mVU_FMACa(mVU, recPass, 2, 1, true,  opSUBAy,  0);  }
mVUop(mVU_SUBAz)  { mVU_FMACa(mVU, recPass, 2, 1, true,  opSUBAz,  0);  }
mVUop(mVU_SUBAw)  { mVU_FMACa(mVU, recPass, 2, 1, true,  opSUBAw,  0);  }
mVUop(mVU_MUL)    { mVU_FMACa(mVU, recPass, 1, 2, false, opMUL,  (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULi)   { mVU_FMACa(mVU, recPass, 3, 2, false, opMULi, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULq)   { mVU_FMACa(mVU, recPass, 4, 2, false, opMULq, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULx)   { mVU_FMACa(mVU, recPass, 2, 2, false, opMULx, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULy)   { mVU_FMACa(mVU, recPass, 2, 2, false, opMULy, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULz)   { mVU_FMACa(mVU, recPass, 2, 2, false, opMULz, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULw)   { mVU_FMACa(mVU, recPass, 2, 2, false, opMULw, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULA)   { mVU_FMACa(mVU, recPass, 1, 2, true,  opMULA,   0);  }
mVUop(mVU_MULAi)  { mVU_FMACa(mVU, recPass, 3, 2, true,  opMULAi,  0);  }
mVUop(mVU_MULAq)  { mVU_FMACa(mVU, recPass, 4, 2, true,  opMULAq,  0);  }
mVUop(mVU_MULAx)  { mVU_FMACa(mVU, recPass, 2, 2, true,  opMULAx,  cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MULAy)  { mVU_FMACa(mVU, recPass, 2, 2, true,  opMULAy,  cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MULAz)  { mVU_FMACa(mVU, recPass, 2, 2, true,  opMULAz,  cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MULAw)  { mVU_FMACa(mVU, recPass, 2, 2, true,  opMULAw, (_XYZW_PS) ? (cFs | cFt) : cFs); } // Clamp (TOTA, DoM, ...) - Ft for Superman
mVUop(mVU_MADD)   { mVU_FMACc(mVU, recPass, 1,           opMADD,   0); }
mVUop(mVU_MADDi)  { mVU_FMACc(mVU, recPass, 3,           opMADDi,  0); }
mVUop(mVU_MADDq)  { mVU_FMACc(mVU, recPass, 4,           opMADDq,  0); }
mVUop(mVU_MADDx)  { mVU_FMACc(mVU, recPass, 2,           opMADDx,  cFs); } // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDy)  { mVU_FMACc(mVU, recPass, 2,           opMADDy,  cFs); } // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDz)  { mVU_FMACc(mVU, recPass, 2,           opMADDz,  cFs); } // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDw)  { mVU_FMACc(mVU, recPass, 2,           opMADDw, (isCOP2)?(cACC|cFt|cFs):cFs);} // Clamp (ICO (COP2), TOTA, DoM)
mVUop(mVU_MADDA)  { mVU_FMACb(mVU, recPass, 1, 0,        opMADDA,  0);  }
mVUop(mVU_MADDAi) { mVU_FMACb(mVU, recPass, 3, 0,        opMADDAi, 0);  }
mVUop(mVU_MADDAq) { mVU_FMACb(mVU, recPass, 4, 0,        opMADDAq, 0);  }
mVUop(mVU_MADDAx) { mVU_FMACb(mVU, recPass, 2, 0,        opMADDAx, cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDAy) { mVU_FMACb(mVU, recPass, 2, 0,        opMADDAy, cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDAz) { mVU_FMACb(mVU, recPass, 2, 0,        opMADDAz, cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDAw) { mVU_FMACb(mVU, recPass, 2, 0,        opMADDAw, cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MSUB)   { mVU_FMACd(mVU, recPass, 1,           opMSUB,  (isCOP2) ? cFs : 0); } // Clamp (Superman)
mVUop(mVU_MSUBi)  { mVU_FMACd(mVU, recPass, 3,           opMSUBi,  0);  }
mVUop(mVU_MSUBq)  { mVU_FMACd(mVU, recPass, 4,           opMSUBq,  0);  }
mVUop(mVU_MSUBx)  { mVU_FMACd(mVU, recPass, 2,           opMSUBx,  0);  }
mVUop(mVU_MSUBy)  { mVU_FMACd(mVU, recPass, 2,           opMSUBy,  0);  }
mVUop(mVU_MSUBz)  { mVU_FMACd(mVU, recPass, 2,           opMSUBz,  0);  }
mVUop(mVU_MSUBw)  { mVU_FMACd(mVU, recPass, 2,           opMSUBw,  0);  }
mVUop(mVU_MSUBA)  { mVU_FMACb(mVU, recPass, 1, 1,        opMSUBA,  0);  }
mVUop(mVU_MSUBAi) { mVU_FMACb(mVU, recPass, 3, 1,        opMSUBAi, 0);  }
mVUop(mVU_MSUBAq) { mVU_FMACb(mVU, recPass, 4, 1,        opMSUBAq, 0);  }
mVUop(mVU_MSUBAx) { mVU_FMACb(mVU, recPass, 2, 1,        opMSUBAx, 0);  }
mVUop(mVU_MSUBAy) { mVU_FMACb(mVU, recPass, 2, 1,        opMSUBAy, 0);  }
mVUop(mVU_MSUBAz) { mVU_FMACb(mVU, recPass, 2, 1,        opMSUBAz, 0);  }
mVUop(mVU_MSUBAw) { mVU_FMACb(mVU, recPass, 2, 1,        opMSUBAw, 0);  }
mVUop(mVU_MAX)    { mVU_FMACa(mVU, recPass, 1, 3, false, opMAX,    0);  }
mVUop(mVU_MAXi)   { mVU_FMACa(mVU, recPass, 3, 3, false, opMAXi,   0);  }
mVUop(mVU_MAXx)   { mVU_FMACa(mVU, recPass, 2, 3, false, opMAXx,   0);  }
mVUop(mVU_MAXy)   { mVU_FMACa(mVU, recPass, 2, 3, false, opMAXy,   0);  }
mVUop(mVU_MAXz)   { mVU_FMACa(mVU, recPass, 2, 3, false, opMAXz,   0);  }
mVUop(mVU_MAXw)   { mVU_FMACa(mVU, recPass, 2, 3, false, opMAXw,   0);  }
mVUop(mVU_MINI)   { mVU_FMACa(mVU, recPass, 1, 4, false, opMINI,   0);  }
mVUop(mVU_MINIi)  { mVU_FMACa(mVU, recPass, 3, 4, false, opMINIi,  0);  }
mVUop(mVU_MINIx)  { mVU_FMACa(mVU, recPass, 2, 4, false, opMINIx,  0);  }
mVUop(mVU_MINIy)  { mVU_FMACa(mVU, recPass, 2, 4, false, opMINIy,  0);  }
mVUop(mVU_MINIz)  { mVU_FMACa(mVU, recPass, 2, 4, false, opMINIz,  0);  }
mVUop(mVU_MINIw)  { mVU_FMACa(mVU, recPass, 2, 4, false, opMINIw,  0);  }
mVUop(mVU_FTOI0)  { mVU_FTOIx(mX, NULL,                  opFTOI0);      }
mVUop(mVU_FTOI4)  { mVU_FTOIx(mX, mVUglob.FTOI_4,        opFTOI4);      }
mVUop(mVU_FTOI12) { mVU_FTOIx(mX, mVUglob.FTOI_12,       opFTOI12);     }
mVUop(mVU_FTOI15) { mVU_FTOIx(mX, mVUglob.FTOI_15,       opFTOI15);     }
mVUop(mVU_ITOF0)  { mVU_ITOFx(mX, NULL,                  opITOF0);      }
mVUop(mVU_ITOF4)  { mVU_ITOFx(mX, mVUglob.ITOF_4,        opITOF4);      }
mVUop(mVU_ITOF12) { mVU_ITOFx(mX, mVUglob.ITOF_12,       opITOF12);     }
mVUop(mVU_ITOF15) { mVU_ITOFx(mX, mVUglob.ITOF_15,       opITOF15);     }
mVUop(mVU_NOP)    { pass2 { mVU.profiler.EmitOp(opNOP); } pass3 { mVUlog("NOP"); } }
