// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/Pcsx2Types.h"

/*	The recompilers call this mid-block, where a plain AAPCS call would cost
	them their register allocators. preserve_all moves the cost into the callee,
	which saves the general registers its digit loop uses and no vector
	register, so a site spills q0-q7 and x2-x8 and nothing else.

	Without the attribute the convention is plain AAPCS;
	EEFPU_MODEL_CALL_SPARES_MOST is what the emitters read to widen the spill to
	the whole caller-saved set.

	clang-cl answers __has_attribute(preserve_all), but the Microsoft C++
	mangler has no encoding for the convention and rejects every declaration
	that reaches codegen carrying it, so the arm64 Windows build takes the wide
	spill. C linkage would carry the attribute past the mangler, at the price of
	one flat scope for both namespaces, where EeFpuModel::RecipSqrt and
	VuEfuModel::RecipSqrt share a name.  */
#if defined(__has_attribute) && __has_attribute(preserve_all) && !defined(_MSC_VER)
#define EEFPU_MODEL_CALL __attribute__((preserve_all))
#define EEFPU_MODEL_CALL_SPARES_MOST 1
#else
#define EEFPU_MODEL_CALL
#define EEFPU_MODEL_CALL_SPARES_MOST 0
#endif

/*	The EE FPU's arithmetic in bits: a range that runs to 0x7FFFFFFF rather than
	to FLT_MAX, an adder with no guard bits, a multiplier array whose low columns
	are truncated, denormals flushed on the way in and out, and chop. FPU.cpp
	states the model and carries the console rows behind each part of it.

	The VU FMAC is the same unit and reads it from here.

	The flag registers are not modelled: the EE writes FCR31 and the VU writes a
	per-lane MAC nibble beside a sticky STATUS field, so each caller spells its
	own out of a Result.
*/
namespace EeFpuModel
{
	// What one rounding step produced.
	struct Result
	{
		u32 bits;       // the EE single the unit wrote
		bool overflow;  // past 0x7FFFFFFF once rounded, so saturated to it
		bool underflow; // nonzero below 2^-126, so flushed to a signed zero
	};

	EEFPU_MODEL_CALL Result AddSub(u32 a, u32 b, bool issub);
	EEFPU_MODEL_CALL Result Mul(u32 fs, u32 ft);

	struct Accumulate
	{
		Result product;
		Result result;
	};

	// An overflowing product ends the instruction where it is: `result` is then
	// the saturated product and the accumulator is never read, so an addend of
	// -0x7FFFFFFF does not cancel it.
	Accumulate MulAccumulate(u32 acc, u32 fs, u32 ft, bool issub);

	// The divide unit: integer digit recurrences with no rounding step, which
	// no host rounding mode reproduces. A zero divisor must be answered by the
	// caller -- what it raises and what sign it saturates with differ between
	// DIV and RSQRT.
	EEFPU_MODEL_CALL u32 Divide(u32 a, u32 b);
	EEFPU_MODEL_CALL u32 SqrtBits(u32 t);

	// The composition silicon performs, as one call: the root has nowhere to
	// live across a second.
	EEFPU_MODEL_CALL u32 RecipSqrt(u32 a, u32 t);

#ifdef PCSX2_RECOMPILER_TESTS
	/*	The divide unit's digit recurrence on the two 24-bit significands, hidden
		bit in, cap shortcut included, in the form Divide normalises: 25 bits when
		ma >= mb, 24 when not. DivideSignificand is what Divide runs;
		DivideSignificandPortable is the reference loop. */
	namespace Internal
	{
		u32 DivideSignificand(u32 ma, u32 mb);
		u32 DivideSignificandPortable(u32 ma, u32 mb);
	} // namespace Internal
#endif

	/*	The same unit against a register file in EeFpuFormat.h's relocated form.
		The recompiler holds every FPR as a double there, so reaching the word
		form above costs it a narrow at each operand and a widen at the result,
		re-emitted at every call site for a conversion that is the same at all
		of them and can be done here instead.  */
	namespace Slot
	{
		EEFPU_MODEL_CALL u64 Divide(u64 fs, u64 ft);
		EEFPU_MODEL_CALL u64 Sqrt(u64 ft);
		EEFPU_MODEL_CALL u64 RecipSqrt(u64 fs, u64 ft);

		// `product` decremented by the one EE ULP the multiply array loses on
		// the rows its truncated columns decide, and returned unchanged on the
		// rest.
		EEFPU_MODEL_CALL u64 MulDeficit(u64 fs, u64 ft, u64 product);
	} // namespace Slot
} // namespace EeFpuModel
