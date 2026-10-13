// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// microVU's per-block pass-1 decisions in a form that does not need the
// microVU headers. Two producers fill it: microVU itself, through a test-only
// hook at the end of mVUcompile's first pass, and vuopt's port of that pass.
// Comparing the two is how the port is validated.
//
// Every field mirrors a microOp / microRegInfo field of the same name. The
// pipeline-state blobs are byte copies of microRegInfo (96 bytes).

#include "common/Pcsx2Defs.h"

#include <vector>

namespace vuopt
{
	struct MvuVFRegTrace
	{
		u8 reg = 0, x = 0, y = 0, z = 0, w = 0;
		bool operator==(const MvuVFRegTrace& o) const { return reg == o.reg && x == o.x && y == o.y && z == o.z && w == o.w; }
	};

	struct MvuFlagInstTrace
	{
		u8 doFlag = 0, doNonSticky = 0, write = 0, lastWrite = 0, read = 0;
	};

	struct MvuOpTrace
	{
		u32 pc = 0;
		// microOp
		u8 stall = 0;
		u8 isBadOp = 0, isEOB = 0, isBdelay = 0, swapOps = 0, backupVF = 0;
		u8 doXGKICK = 0, doDivFlag = 0;
		u32 XGKICKPC = 0;
		s32 readQ = 0, writeQ = 0, readP = 0, writeP = 0;
		MvuFlagInstTrace sFlag, mFlag, cFlag;
		// microUpperOp
		u8 eBit = 0, iBit = 0, mBit = 0, tBit = 0, dBit = 0;
		MvuVFRegTrace uVFwrite, uVFread0, uVFread1;
		// microLowerOp
		MvuVFRegTrace lVFwrite, lVFread0, lVFread1;
		u8 VIwriteReg = 0, VIwriteUsed = 0;
		u8 VIread0Reg = 0, VIread0Used = 0, VIread1Reg = 0, VIread1Used = 0;
		u8 constJumpValid = 0;
		u32 constJumpValue = 0;
		u32 branch = 0;
		u32 kickcycles = 0;
		u8 badBranch = 0, evilBranch = 0, isNOP = 0, isFSSET = 0, noWriteVF = 0;
		u8 backupVI = 0, memReadIs = 0, memReadIt = 0, readFlags = 0, isMemWrite = 0, isKick = 0;
	};

	struct MvuBlockTrace
	{
		u32 vuIndex = 0;
		u32 startPC = 0;            // byte offset
		u8 entryState[96] = {};     // mVUpBlock->pState
		u8 endState[96] = {};       // mVUregs after mVUsetFlags + mVUoptimizePipeState
		u32 count = 0;              // mVUcount
		u32 cycles = 0;             // mVUcycles
		u8 flagHack = 0;            // mVUsFlagHack latched for this compile
		u8 needFlagFinalize = 0;
		std::vector<MvuOpTrace> ops;
	};

	// What a block body emitted in place of microVU's needs to know about
	// microVU and the active configuration (milestone 2's per-block lowering).
	struct MvuEmitEnv
	{
		u32 vuIndex = 0;
		s32 offDivFlag = 0;    // microVU fields, as offsets from &mVU.macFlag[0] (x24)
		s32 offVIbackup = 0;
		s32 offVIxgkick = 0;
		s32 offBranch = 0;
		const void* xgkickFn = nullptr;
		const void* vifTop = nullptr;
		const void* vifItop = nullptr;
		bool clampOverflow = false;   // CHECK_VU_OVERFLOW
		bool clampExtra = false;      // CHECK_VU_EXTRA_OVERFLOW
		bool clampSign = false;       // CHECK_VU_SIGN_OVERFLOW
		bool clampExact = false;      // CHECK_VU_EXACT
		bool softwareFlush = false;   // mVUneedsSoftwareFlush
		bool xgkickHack = false;
		bool ibitHack = false;
		bool overflowHack = false;    // CHECK_VUOVERFLOWHACK
		bool addSubHack = false;      // CHECK_VUADDSUBHACK
		bool flagHack = false;
	};

#ifdef PCSX2_RECOMPILER_TESTS
	// Called by microVU at the end of every block's first pass when set.
	// Testing-only: absent from builds without the recompiler test hooks.
	using MvuBlockHook = void (*)(const MvuBlockTrace& block, void* user);
	extern MvuBlockHook g_mvuBlockHook;
	extern void* g_mvuBlockHookUser;

	// Called by microVU at the start of every block's second pass when set.
	// Returning true means the hook emitted the whole block body at armAsm's
	// cursor, honouring microVU's block-boundary state contract, and microVU
	// emits only its framing (cycle test already done, links, ends).
	// Testing-only, like the block hook.
	using VuoptBodyHook = bool (*)(const MvuBlockTrace& block, const MvuEmitEnv& env, void* user);
	extern VuoptBodyHook g_vuoptBodyHook;
	extern void* g_vuoptBodyHookUser;
#endif
} // namespace vuopt
