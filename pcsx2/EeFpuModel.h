// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/Pcsx2Types.h"

/*	The EE FPU's arithmetic in bits: a range that runs to 0x7FFFFFFF rather than
	to FLT_MAX, an adder with no guard bits, a multiplier array whose low columns
	are truncated, denormals flushed on the way in and out, and chop. FPU.cpp
	states the model and carries the console rows behind each part of it.

	The VU FMAC is the same unit, so it reads this rather than carrying a second
	copy. Its own capture -- range, saturation, the flush, the multiplier's
	one-ULP deficit and its non-commutativity -- is reproduced with nothing added
	(tests/ctest/core/recompilers/vu0_macro_fmac_range_console_tests.cpp).

	The flag registers are not here: the EE writes FCR31 and the VU writes a
	per-lane MAC nibble beside a sticky STATUS field, so each caller spells its
	own out of the facts a Result carries.
*/
namespace EeFpuModel
{
	// What one rounding step produced.
	struct Result
	{
		u32 bits;       // the EE single it wrote
		bool overflow;  // past 0x7FFFFFFF once rounded, so saturated to it
		bool underflow; // nonzero below 2^-126, so flushed to a signed zero
	};

	Result AddSub(u32 a, u32 b, bool issub);
	Result Mul(u32 fs, u32 ft);

	/*	The two roundings a multiply-accumulate takes. An overflowing product
		ends the instruction where it is: `result` is then the saturated product
		and the accumulator is never read, so an addend of -0x7FFFFFFF does not
		cancel it. */
	struct Accumulate
	{
		Result product;
		Result result;
	};

	Accumulate MulAccumulate(u32 acc, u32 fs, u32 ft, bool issub);
} // namespace EeFpuModel
