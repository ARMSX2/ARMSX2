// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "EeFpuModel.h"

/*	The EFU, one entry point per opcode.

	Each of the thirteen is a chain of divide-unit and FMAC steps on the EE
	FPU's model rather than on host floats -- ESIN is four multiplies and four
	adds, EATAN twenty-five of them around a divide -- so a recompiler calls
	what the interpreter calls and the two engines cannot part on a rounding.
	Inline emission would run to hundreds of instructions per op; the ops
	declare 11 to 54 cycles of P latency, which is what makes a call affordable
	here where it is not on an FMAC.

	The arguments are raw VF words: the EFU folds nothing through vuDouble on
	the way in, and the model's range is wider than the clamp's. */
namespace VuEfuModel
{
	EEFPU_MODEL_CALL u32 Sum(u32 x, u32 y, u32 z, u32 w);      // ESUM
	EEFPU_MODEL_CALL u32 SquareSum(u32 x, u32 y, u32 z);       // ESADD
	EEFPU_MODEL_CALL u32 RecipSquareSum(u32 x, u32 y, u32 z);  // ERSADD
	EEFPU_MODEL_CALL u32 Length(u32 x, u32 y, u32 z);          // ELENG
	EEFPU_MODEL_CALL u32 RecipLength(u32 x, u32 y, u32 z);     // ERLENG

	EEFPU_MODEL_CALL u32 Recip(u32 fs);      // ERCPR
	EEFPU_MODEL_CALL u32 Sqrt(u32 fs);       // ESQRT
	EEFPU_MODEL_CALL u32 RecipSqrt(u32 fs);  // ERSQRT
	EEFPU_MODEL_CALL u32 Sin(u32 fs);        // ESIN
	EEFPU_MODEL_CALL u32 Exp(u32 fs);        // EEXP
	EEFPU_MODEL_CALL u32 Atan(u32 fs);       // EATAN

	// EATANxy and EATANxz, which reduce atan(b/a) where EATAN reduces atan(a).
	EEFPU_MODEL_CALL u32 AtanRatio(u32 a, u32 b);
} // namespace VuEfuModel
