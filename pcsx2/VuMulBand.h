// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "EeFpuModel.h"

/*	Out-of-line half of the multiplier's one-ULP deficit.

	The emitted model decides a lane from ft's mantissa alone. That is only
	correct when the exact 48-bit mantissa product has no bits below the
	single-precision result's last bit. Two cases fall outside it and both
	emitters branch here for them:

	  * those low bits are non-zero but smaller than 0x8000, where the array's
	    truncation still crosses the last bit;
	  * the product is below 2^-79, where the emitted exactness test does not
	    work because the rounding error has flushed to zero.

	Each VU gets its own storage. microVU compiles VU1 on the MTVU thread while
	the EE thread may be inside a COP2 macro op, so one shared area would be
	written from two threads at once. The COP2 macro path uses storage in the
	recompiler's own state instead, which its pinned base register addresses
	with a plain offset.  */
struct VuMulBandSlot
{
	alignas(16) u32 fs[4];
	alignas(16) u32 ft[4];
	alignas(16) u32 product[4];
};

extern VuMulBandSlot g_vuMulBand[2];

void vuMulShortTailBandLanes(const u32* fs, const u32* ft, u32* product);
EEFPU_MODEL_CALL void vuMulShortTailBandVu0();
EEFPU_MODEL_CALL void vuMulShortTailBandVu1();
