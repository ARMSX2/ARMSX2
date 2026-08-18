// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "VU.h"

// `underflow` says the exact result was nonzero but below the smallest normal.
// The caller has to supply it because the host FPCR has already flushed such a
// result to a signed zero by the time the value arrives here, and an exact zero
// is indistinguishable from a flushed one.
extern u32  VU_MACx_UPDATE(VURegs * VU, float x, bool underflow = false);
extern u32  VU_MACy_UPDATE(VURegs * VU, float y, bool underflow = false);
extern u32  VU_MACz_UPDATE(VURegs * VU, float z, bool underflow = false);
extern u32  VU_MACw_UPDATE(VURegs * VU, float w, bool underflow = false);
extern void VU_MACx_CLEAR(VURegs * VU);
extern void VU_MACy_CLEAR(VURegs * VU);
extern void VU_MACz_CLEAR(VURegs * VU);
extern void VU_MACw_CLEAR(VURegs * VU);
extern void VU_STAT_UPDATE(VURegs * VU);
