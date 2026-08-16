// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstring>

/*	An EE FPR word held in a host double:

	    stored = sign << 63 | (word & 0x7FFFFFFF) << 29

	Uniformly stored == eeValue(word) / 2^896. The scale puts 2^-126 on
	0x1p-1022, so FPCR.FZ flushes at the EE's boundary; it keeps the double
	exponent field under 256, so the EE's top binade is an ordinary number
	here; and it is bijective, so a raw int32 parked in an FPR survives.
*/

static constexpr int kEeFprScaleExp = 896;

__fi static constexpr u64 eeFprWidenBits(u32 word)
{
	return (static_cast<u64>(word & 0x80000000u) << 32) | (static_cast<u64>(word & 0x7FFFFFFFu) << 29);
}

__fi static constexpr u32 eeFprNarrowBits(u64 stored)
{
	return static_cast<u32>((stored >> 32) & 0x80000000u) | static_cast<u32>((stored >> 29) & 0x7FFFFFFFu);
}

// Bounds and scale factor for code computing in this domain. 0x7FFFFFFF is the
// largest number the FPU has.
static constexpr u64 kEeFprMaxBits = eeFprWidenBits(0x7FFFFFFFu);
static constexpr u64 kEeFprMinBits = eeFprWidenBits(0xFFFFFFFFu);
static constexpr u64 kEeFprUnscaleBits = UINT64_C(0x77F0000000000000);
static constexpr u64 kEeFprSignBit = UINT64_C(0x8000000000000000);
static constexpr u64 kEeFprExpMask = UINT64_C(0x7FF0000000000000);

static_assert(kEeFprMaxBits == UINT64_C(0x0FFFFFFFE0000000));
static_assert(kEeFprMinBits == UINT64_C(0x8FFFFFFFE0000000));
static_assert((kEeFprUnscaleBits >> 52) == static_cast<u64>(1023 + kEeFprScaleExp),
	"kEeFprUnscaleBits must be 2^kEeFprScaleExp");

__fi static double eeFprWiden(u32 word)
{
	const u64 bits = eeFprWidenBits(word);
	double d;
	std::memcpy(&d, &bits, sizeof(d));
	return d;
}

__fi static u32 eeFprNarrow(double stored)
{
	u64 bits;
	std::memcpy(&bits, &stored, sizeof(bits));
	return eeFprNarrowBits(bits);
}

/*	Whether the FPR file is in this domain right now. It follows the EE FPU's
	clamp mode: only iFPUd computes here, and modes 0 to 2 leave the
	architectural word in the slot's low half. Read it through
	FPRreg::Word()/SetWord().
*/
extern bool g_eeFprSlotsRelocated;

/*	Put the file in the format the current clamp mode calls for. Emitted code
	assumes one format, so call this with the code-cache reset a mode change
	already forces.
*/
void eeFprSyncSlotFormat();
