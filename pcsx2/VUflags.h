// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "VU.h"

// Neither `overflow` nor `underflow` is recoverable from the word: exponent
// 255 is an ordinary exponent on this unit, and a flushed subnormal is the
// same signed zero an exact cancellation returns.
extern u32  VU_MACx_UPDATE(VURegs * VU, u32 x, bool overflow = false, bool underflow = false);
extern u32  VU_MACy_UPDATE(VURegs * VU, u32 y, bool overflow = false, bool underflow = false);
extern u32  VU_MACz_UPDATE(VURegs * VU, u32 z, bool overflow = false, bool underflow = false);
extern u32  VU_MACw_UPDATE(VURegs * VU, u32 w, bool overflow = false, bool underflow = false);
extern void VU_MACx_CLEAR(VURegs * VU);
extern void VU_MACy_CLEAR(VURegs * VU);
extern void VU_MACz_CLEAR(VURegs * VU);
extern void VU_MACw_CLEAR(VURegs * VU);
// extraSticky carries the ZSUO of a stage the cause nibble does not show.
extern void VU_STAT_UPDATE(VURegs * VU, u32 extraSticky = 0);
