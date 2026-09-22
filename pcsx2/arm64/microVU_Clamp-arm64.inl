// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

//------------------------------------------------------------------
// Micro VU - ARM64 NEON Clamp Functions
//------------------------------------------------------------------

#ifdef PCSX2_RECOMPILER_TESTS
u32 g_mvuClampConstEstablishCount = 0;
#endif

// Lay the two bounds mVUclamp1 reads into qmmClampMax/qmmClampMin: once at a
// block's entry, and again wherever a block resumes from a C call.
static void mVUemitClampConsts(microVU& mVU)
{
	if (!CHECK_VU_OVERFLOW(mVU.index))
		return;
	armAsm->Ldp(qmmClampMin, qmmClampMax, mVUglobMem(&mVUglob.minvals[0]));
#ifdef PCSX2_RECOMPILER_TESTS
	g_mvuClampConstEstablishCount++;
#endif
}

// Whether mVUclamp2 takes its own sign-preserving path rather than falling
// through to mVUclamp1, and whether mVUclamp1 emits at all. mVUclampFoldable
// reads both, and has to decide exactly as they do.
static __fi bool mVUclamp2Signs(microVU& mVU, bool bClampE)
{
	return (!clampE && CHECK_VU_SIGN_OVERFLOW(mVU.index))
	       || (clampE && bClampE && CHECK_VU_SIGN_OVERFLOW(mVU.index));
}

static __fi bool mVUclamp1Emits(microVU& mVU, bool bClampE)
{
	return (!clampE && CHECK_VU_OVERFLOW(mVU.index)) || (clampE && bClampE);
}

// `cloned` on the two clamps below: the register a clone-write copy in front of
// the clamp was copied from, so the clamp reads it and the copy goes away.
// mVUcloneTakeHere asks the clamp to look for one itself, which is right
// wherever the clamp is the first thing emitted after the copy.
static constexpr int mVUcloneTakeHere = -2;

// Result clamping: clamp to [minFloat, maxFloat].
// Uses FMINNM/FMAXNM (number-preserving) so NaN inputs clamp to ±maxfloat,
// matching x86 SSE MINPS/MAXPS NaN-eating semantics. Plain FMIN/FMAX are
// IEEE-strict and propagate NaN — using them here lets NaN flow through
// matrix-multiply chains and corrupts vertex output.
void mVUclamp1(microVU& mVU, const a64::VRegister& reg, const a64::VRegister& regT1, int xyzw,
	bool bClampE = false, int cloned = mVUcloneTakeHere)
{
	if (mVUclamp1Emits(mVU, bClampE) && mVU.regAlloc->checkVFClamp(reg.GetCode()))
	{
		// Macro mode is not on that contract -- there the two registers are the
		// EE's, established lazily by its own first clamp site. None of the
		// twelve emitters the COP2 macro adapter routes is arithmetic.
		pxAssertRel(!mVU.cop2, "microVU: macro mode reached a clamp with no bounds of its own");

		switch (xyzw)
		{
			case 1: case 2: case 4: case 8:
			{
				// Clamp ONLY lane 0 to [minFloat, maxFloat], PRESERVING lanes 1-3.
				// A 32-bit scalar NEON FP op zeroes the dest V-reg's upper lanes,
				// unlike x86 MIN.SS/MAX.SS which leave them intact. Single-scalar
				// FMAC ops rotate the live lane to lane 0 (shuffleSSto0), clamp +
				// operate on lane 0, then rotate the siblings back (shuffleSSfrom0)
				// — so the siblings parked in lanes 1-3 MUST survive the clamp.
				// Zeroing them clobbered carried state (e.g. the ACC accumulator
				// across single-component MADD chains), which is SoulCalibur III's
				// vuClampMode:2 SPS / trembling geometry. Compute the clamped
				// scalar in RQSCRATCH3 and INS it back into lane 0 only, mirroring
				// the x86 mVUclamp1 SS path.
				//
				// Both bounds land on the scratch before the one writeback: only
				// the second bound's result reaches `reg`, so returning the first
				// one there and reading it back is a round trip through a lane
				// nothing else can see.
				pxAssert(cloned < 0);
				const a64::VRegister t(RQSCRATCH3.GetCode(), 32);
				armAsm->Fminnm(t, a64::VRegister(reg.GetCode(), 32), a64::VRegister(qmmClampMax.GetCode(), 32));
				armAsm->Fmaxnm(t, t, a64::VRegister(qmmClampMin.GetCode(), 32));
				armAsm->Ins(reg.V4S(), 0, RQSCRATCH3.V4S(), 0);
				break;
			}
			default:
			{
				// Fminnm rewrites every lane of reg, so a clone-write copy
				// standing in front of it is dead weight: read the original.
				if (cloned == mVUcloneTakeHere)
					cloned = mVU.regAlloc->takeCloneSource(reg.GetCode());
				const a64::VRegister src = (cloned >= 0) ? a64::VRegister(cloned, 128) : reg;
				armAsm->Fminnm(reg.V4S(), src.V4S(), qmmClampMax.V4S());
				armAsm->Fmaxnm(reg.V4S(), reg.V4S(), qmmClampMin.V4S());
				break;
			}
		}
	}
}

// Operand clamping with sign preservation.
// Uses integer SMIN/UMIN to preserve NaN sign bit.
void mVUclamp2(microVU& mVU, const a64::VRegister& reg, const a64::VRegister& regT1in, int xyzw,
	bool bClampE = false, int cloned = mVUcloneTakeHere)
{
	if (mVUclamp2Signs(mVU, bClampE) && mVU.regAlloc->checkVFClamp(reg.GetCode()))
	{
		// Integer min/max to preserve NaN sign
		// SMIN.4S clamps the signed integer representation
		// UMIN.4S clamps the unsigned integer representation
		//
		// Row select mirrors x86 mVUclamp2: single-lane (SS) ops clamp only
		// lane 0 — row 0's lanes 1-3 hold sentinel no-op bounds (INT_MAX /
		// UINT_MAX) so live state parked there survives. Today every SS-site
		// operand is a scratch copy whose lanes 1-3 die at masked writeback
		// (the arm64 accSS/Ins restructuring), so this is x86-parity
		// hardening rather than a live fix — but it removes the trap for any
		// future caller that clamps a register with live sibling lanes
		// (e.g. routing COP2 macro FMACs through these emitters, where
		// mVU_MADDw's cACC would clamp the rotated live ACC). (AX-02)
		const int row = (xyzw == 1 || xyzw == 2 || xyzw == 4 || xyzw == 8) ? 0 : 1;
		// Smin writes all four lanes here even on the single-lane row -- lanes
		// 1-3 pass through the sentinel bounds -- so a clone-write copy in
		// front of it is foldable either way (mVUclamp1's single-lane case,
		// which really does leave lanes 1-3 alone, is not).
		if (cloned == mVUcloneTakeHere)
			cloned = mVU.regAlloc->takeCloneSource(reg.GetCode());
		const a64::VRegister src = (cloned >= 0) ? a64::VRegister(cloned, 128) : reg;
		// The row's two bounds are adjacent, so they arrive in one Ldp. Both
		// scratches die two instructions later, which is inside every caller's
		// own use of them: the U/O models hold their predicates in allocator
		// temps, and the guard mask and the multiply's deficit take the trio
		// after the clamps rather than across them.
		armAsm->Ldp(RQSCRATCH3, RQSCRATCH, mVUglobMem(&mVUglob.signBounds[row][0][0]));
		armAsm->Smin(reg.V4S(), src.V4S(), RQSCRATCH3.V4S());
		armAsm->Umin(reg.V4S(), reg.V4S(), RQSCRATCH.V4S());
		return;
	}
	else
	{
		mVUclamp1(mVU, reg, regT1in, xyzw, bClampE, cloned);
	}
}

// Whether a clamp of `reg` would fold the clone-write copy in front of it. It
// can only do that when what it emits rewrites every lane: mVUclamp1's
// single-lane case leaves lanes 1-3 alone, so the copy still has to fill them
// in, and a clamp the mode switches off emits nothing to fold into.
static __fi bool mVUclampFoldable(microVU& mVU, const a64::VRegister& reg, int xyzw, bool bClampE)
{
	if (!mVU.regAlloc->checkVFClamp(reg.GetCode()))
		return false;
	if (mVUclamp2Signs(mVU, bClampE))
		return true;
	return mVUclamp1Emits(mVU, bClampE)
	       && !(xyzw == 1 || xyzw == 2 || xyzw == 4 || xyzw == 8);
}

// Clamp a set of operands together. `regs` is the order allocReg handed the
// registers out, which is the order their clone-write copies sit in the
// buffer; an invalid entry is an operand the caller does not clamp, and a
// register named twice is clamped once, at its last mention.
//
// A copy is foldable only while it is the last word emitted, so every fold is
// taken before any clamp is emitted, newest first: the first clamp's own
// instructions would bury the copy standing in front of the second. The clamps
// then emit oldest first, which is the order that leaves each folded source
// standing -- allocReg remaps a clone-write destination as it writes it, so no
// later operand can be holding one as its own cached copy.
static void mVUclampOperands(microVU& mVU, std::initializer_list<a64::VRegister> regs, int xyzw,
	bool bClampE = false)
{
	const a64::VRegister* r = regs.begin();
	const int n = static_cast<int>(regs.size());
	pxAssert(n <= microRegAlloc::kCloneRunMax);

	bool live[microRegAlloc::kCloneRunMax];
	for (int i = 0; i < n; i++)
	{
		live[i] = r[i].IsValid();
		for (int j = i + 1; live[i] && j < n; j++)
			live[i] = !(r[j].IsValid() && r[j].Is(r[i]));
	}

	int cloned[microRegAlloc::kCloneRunMax];
	for (int i = n - 1; i >= 0; i--)
	{
		cloned[i] = (live[i] && mVUclampFoldable(mVU, r[i], xyzw, bClampE))
			? mVU.regAlloc->takeCloneSource(r[i].GetCode())
			: -1;
	}
	for (int i = 0; i < n; i++)
	{
		if (live[i])
			mVUclamp2(mVU, r[i], a64::NoVReg, xyzw, bClampE, cloned[i]);
	}
}

// Operand clamping for every arithmetic op (only when extra overflow enabled)
void mVUclamp3(microVU& mVU, const a64::VRegister& reg, const a64::VRegister& regT1, int xyzw)
{
	if (clampE && mVU.regAlloc->checkVFClamp(reg.GetCode()))
		mVUclamp2(mVU, reg, regT1, xyzw, true);
}

// Result clamping for every arithmetic op (when extra overflow but not sign-preserving)
void mVUclamp4(microVU& mVU, const a64::VRegister& reg, const a64::VRegister& regT1, int xyzw)
{
	if (clampE && !CHECK_VU_SIGN_OVERFLOW(mVU.index) && mVU.regAlloc->checkVFClamp(reg.GetCode()))
		mVUclamp1(mVU, reg, regT1, xyzw, true);
}
