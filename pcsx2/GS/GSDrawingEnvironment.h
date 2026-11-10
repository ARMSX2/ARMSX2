// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <bit>
#include <cstring>
#if defined(_M_ARM64) || defined(__aarch64__)
#include <arm_neon.h>
#endif

class alignas(32) GSDrawingEnvironment
{
public:
	GIFRegPRIM       PRIM;
	GIFRegPRMODE     PRMODE;
	GIFRegPRMODECONT PRMODECONT;
	GIFRegTEXCLUT    TEXCLUT;
	GIFRegSCANMSK    SCANMSK;
	GIFRegTEXA       TEXA;
	GIFRegFOGCOL     FOGCOL;
	GIFRegDIMX       DIMX;
	GIFRegDTHE       DTHE;
	GIFRegCOLCLAMP   COLCLAMP;
	GIFRegPABE       PABE;
	GIFRegBITBLTBUF  BITBLTBUF;
	GIFRegTRXDIR     TRXDIR;
	GIFRegTRXPOS     TRXPOS;
	GIFRegTRXREG     TRXREG;
	GSDrawingContext CTXT[2];

	void Reset();

	void Dump(const std::string& filename) const;
};

// Copies a whole drawing environment as straight-line vector loads and stores. The pipelined split copies two of these
// per draw on each side of the ring; at 768 bytes the compiler calls the C library's copy, which spends about two and a
// half times the instructions per byte that an inline copy of a known size does.
static_assert(sizeof(GSDrawingEnvironment) % 64 == 0 && sizeof(GSDrawingEnvironment) / 64 <= 16,
	"the draw record's environment delta works in 64-byte blocks and a 16-bit mask");

__forceinline_odr void CopyDrawingEnvironment(GSDrawingEnvironment& dst, const GSDrawingEnvironment& src)
{
#if defined(__clang__)
	__builtin_memcpy_inline(&dst, &src, sizeof(GSDrawingEnvironment));
#else
	std::memcpy(&dst, &src, sizeof(GSDrawingEnvironment));
#endif
}

// The draw record's environment delta, producer side. Compares `live` with `sent` (what the consumer holds) in 64-byte
// blocks; each block that differs is written to the same offset of `dst` (the record) and into `sent`. Returns the
// mask of blocks written.
__forceinline_odr u32 DeltaDrawingEnvironment(GSDrawingEnvironment& dst, const GSDrawingEnvironment& live, GSDrawingEnvironment& sent)
{
	constexpr u32 blocks = sizeof(GSDrawingEnvironment) / 64;
	const u8* l = reinterpret_cast<const u8*>(&live);
	u8* s = reinterpret_cast<u8*>(&sent);
	u8* d = reinterpret_cast<u8*>(&dst);
	u32 mask = 0;
	for (u32 b = 0; b < blocks; b++)
	{
		const u32 o = b * 64;
#if defined(_M_ARM64) || defined(__aarch64__)
		const uint32x4_t l0 = vld1q_u32(reinterpret_cast<const u32*>(l + o));
		const uint32x4_t l1 = vld1q_u32(reinterpret_cast<const u32*>(l + o + 16));
		const uint32x4_t l2 = vld1q_u32(reinterpret_cast<const u32*>(l + o + 32));
		const uint32x4_t l3 = vld1q_u32(reinterpret_cast<const u32*>(l + o + 48));
		const uint32x4_t x = vorrq_u32(
			vorrq_u32(veorq_u32(l0, vld1q_u32(reinterpret_cast<const u32*>(s + o))), veorq_u32(l1, vld1q_u32(reinterpret_cast<const u32*>(s + o + 16)))),
			vorrq_u32(veorq_u32(l2, vld1q_u32(reinterpret_cast<const u32*>(s + o + 32))), veorq_u32(l3, vld1q_u32(reinterpret_cast<const u32*>(s + o + 48)))));
		if (vmaxvq_u32(x) == 0)
			continue;
		vst1q_u32(reinterpret_cast<u32*>(d + o), l0);
		vst1q_u32(reinterpret_cast<u32*>(d + o + 16), l1);
		vst1q_u32(reinterpret_cast<u32*>(d + o + 32), l2);
		vst1q_u32(reinterpret_cast<u32*>(d + o + 48), l3);
		vst1q_u32(reinterpret_cast<u32*>(s + o), l0);
		vst1q_u32(reinterpret_cast<u32*>(s + o + 16), l1);
		vst1q_u32(reinterpret_cast<u32*>(s + o + 32), l2);
		vst1q_u32(reinterpret_cast<u32*>(s + o + 48), l3);
#else
		if (std::memcmp(l + o, s + o, 64) == 0)
			continue;
		std::memcpy(d + o, l + o, 64);
		std::memcpy(s + o, l + o, 64);
#endif
		mask |= 1u << b;
	}
	return mask;
}

// Consumer side: applies the record's blocks to `held` (what the producer's `sent` holds), so `held` equals the
// producer's live environment at the time of the record.
__forceinline_odr void ApplyDrawingEnvironmentDelta(GSDrawingEnvironment& held, const GSDrawingEnvironment& rec, u32 mask)
{
	u8* h = reinterpret_cast<u8*>(&held);
	const u8* r = reinterpret_cast<const u8*>(&rec);
	while (mask != 0)
	{
		const u32 o = static_cast<u32>(std::countr_zero(mask)) * 64;
		mask &= mask - 1;
#if defined(__clang__)
		__builtin_memcpy_inline(h + o, r + o, 64);
#else
		std::memcpy(h + o, r + o, 64);
#endif
	}
}
