// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include <cmath>

#include "VUmicro.h"

/*****************************************/
/*          NEW FLAGS                    */ //By asadr. Thnkx F|RES :p
/*****************************************/

/*	The four cause bits one lane's result raises, and the word the FMAC writes
	for it.

	None of them can be read off the value alone. Exponent 255 is an ordinary
	exponent here -- the VU's largest number is 0x7FFFFFFF, one binade above
	FLT_MAX -- so O is not "the exponent field filled up" but "the exact result
	was past that", which only the unit that produced it knows; and once a
	subnormal result has been flushed, the signed zero left behind is the same
	word an exact cancellation returns. Both facts come in from the caller.

	Reading exponent 255 as O instead is what made 2^128 * (1 + 2^-23) raise O
	and come back clamped, where the console returns 0x7F800001 with no flag at
	all (captures/vusat).
*/
static __ri u32 VU_MAC_UPDATE( int shift, VURegs * VU, u32 v, bool overflow, bool underflow )
{
	const u32 s = v & 0x80000000;

	if (s)
		VU->macflag |= 0x0010<<shift;
	else
		VU->macflag &= ~(0x0010<<shift);

	if (overflow)
	{
		VU->macflag = (VU->macflag&~(0x0101<<shift)) | (0x1000<<shift);
		return s | 0x7fffffff;
	}

	if (underflow)
	{
		VU->macflag = (VU->macflag&~(0x1000<<shift)) | (0x0101<<shift);
		return s;
	}

	if ((v & 0x7fffffff) == 0)
	{
		VU->macflag = (VU->macflag & ~(0x1100<<shift)) | (0x0001<<shift);
		return v;
	}

	VU->macflag = (VU->macflag & ~(0x1101<<shift));
	return v;
}

__fi u32 VU_MACx_UPDATE(VURegs * VU, u32 x, bool overflow, bool underflow)
{
	return VU_MAC_UPDATE(3, VU, x, overflow, underflow);
}

__fi u32 VU_MACy_UPDATE(VURegs * VU, u32 y, bool overflow, bool underflow)
{
	return VU_MAC_UPDATE(2, VU, y, overflow, underflow);
}

__fi u32 VU_MACz_UPDATE(VURegs * VU, u32 z, bool overflow, bool underflow)
{
	return VU_MAC_UPDATE(1, VU, z, overflow, underflow);
}

__fi u32 VU_MACw_UPDATE(VURegs * VU, u32 w, bool overflow, bool underflow)
{
	return VU_MAC_UPDATE(0, VU, w, overflow, underflow);
}

__fi void VU_MACx_CLEAR(VURegs * VU)
{
	VU->macflag&= ~(0x1111<<3);
}

__fi void VU_MACy_CLEAR(VURegs * VU)
{
	VU->macflag&= ~(0x1111<<2);
}

__fi void VU_MACz_CLEAR(VURegs * VU)
{
	VU->macflag&= ~(0x1111<<1);
}

__fi void VU_MACw_CLEAR(VURegs * VU)
{
	VU->macflag&= ~(0x1111<<0);
}

__ri void VU_STAT_UPDATE(VURegs * VU, u32 extraSticky) {
	int newflag = 0 ;
	if (VU->macflag & 0x000F) newflag = 0x1;
	if (VU->macflag & 0x00F0) newflag |= 0x2;
	if (VU->macflag & 0x0F00) newflag |= 0x4;
	if (VU->macflag & 0xF000) newflag |= 0x8;
	// Replace the ZSUO cause nibble and OR the matching sticky bits in, keeping
	// the sticky field this op did not touch. The sticky OR has to happen HERE,
	// at the point the flags are produced -- statusflag is seeded from the whole
	// STATUS register (vu0ExecMicro), not from a bare cause nibble.
	//
	// Assigning `newflag` outright instead left the sticky field to be
	// re-derived from the cause nibble by whoever consumed statusflag later.
	// That is idempotent right up until an FSSET clears the sticky field, at
	// which point the stale cause regenerates the bit FSSET just cleared.
	//
	// The D/I pair is deliberately not carried here: it belongs to the div unit,
	// which maintains it in statusflag independently (VU_STICKY_DI).
	//
	// extraSticky is the cause nibble of a stage the cause field no longer
	// shows: a multiply-accumulate rounds twice, and its product's flags
	// survive only in the sticky half.
	VU->statusflag = (VU->statusflag & 0xFC0) | newflag | ((newflag | extraSticky) << 6);
}
