// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Milestone 2 lowering: one microVU block's body, from vuopt's per-node IR
// (ir::BuildNodeLocal) to AArch64 through vixl.
//
// The body runs inside microVU's block framing, so it honours microVU's
// block-boundary contract: VF/VI/ACC/I/R in VURegs, status instances in
// w20-w23, MAC and clip instances at [x24], Q/P in the lanes of q28, the
// divide flags, VI backup, kick address and branch value in microVU's own
// fields. Incoming values are read from those homes where they are used;
// every final value is written back at the end. Every other value lives in a
// scratch slot. Each operation emits the instruction sequence microVU's
// emitter for it emits in the shipped configuration, so results are
// bit-identical by construction; pcsx2-vurunner --vuopt-run checks it.
//
// Supported: VU1, clamp mode 1 (overflow only), no clamp-affecting gamefixes.

#include "VuOptIR.h"

#include <string>

namespace vuopt
{
	struct LowerEnv
	{
		u32 vuIndex = 1;
		// Offsets from gprMVUFlag (x24, &mVU.macFlag[0]) of microVU's fields.
		s32 offDivFlag = 0;
		s32 offVIbackup = 0;
		s32 offVIxgkick = 0;
		s32 offBranch = 0;
		const void* xgkickFn = nullptr; // void mVU_XGKICK_(u32 addr)
		const void* vifTop = nullptr;   // &VIFregisters.top
		const void* vifItop = nullptr;  // &VIFregisters.itop
		bool clampOverflow = true;      // CHECK_VU_OVERFLOW
		bool softwareFlush = false;     // !FPCR.DAZ: the divide unit's denormal flush in software
	};

	struct LowerResult
	{
		bool ok = false;
		std::string why;   // why the block was declined (nothing emitted)
		u32 bytes = 0;     // host code emitted
	};

	// Checks the IR can be lowered, then emits it at armAsm's cursor. Emits
	// nothing when it declines.
	LowerResult LowerNodeBody(const ir::Function& f, const LowerEnv& env);
} // namespace vuopt
