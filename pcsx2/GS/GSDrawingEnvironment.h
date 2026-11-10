// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

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
__forceinline_odr void CopyDrawingEnvironment(GSDrawingEnvironment& dst, const GSDrawingEnvironment& src)
{
#if defined(__clang__)
	__builtin_memcpy_inline(&dst, &src, sizeof(GSDrawingEnvironment));
#else
	std::memcpy(&dst, &src, sizeof(GSDrawingEnvironment));
#endif
}
