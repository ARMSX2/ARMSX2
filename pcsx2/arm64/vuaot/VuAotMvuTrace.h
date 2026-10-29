// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// microVU's per-block pass-1 decisions in a form that does not need the
// microVU headers. Two producers fill it: microVU itself, through a test-only
// hook at the end of mVUcompile's first pass, and vu-aot's port of that pass.
// Comparing the two is how the port is validated.
//
// Every field mirrors a microOp / microRegInfo field of the same name. The
// pipeline-state blobs are byte copies of microRegInfo (96 bytes).

#include "common/Pcsx2Defs.h"

#include <vector>

namespace vuaot
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
	// microVU and the active configuration.
	struct MvuEmitEnv
	{
		u32 vuIndex = 0;
		s32 offDivFlag = 0;    // microVU fields, as offsets from &mVU.macFlag[0] (x24)
		s32 offVIbackup = 0;
		s32 offVIxgkick = 0;
		s32 offBranch = 0;
		s32 offNeonBackup = 0;
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
		bool vuSyncHack = false;      // VUSyncHack / FullVU0SyncHack (cycle accounting differs)

		// What replacing microVU's block framing (vu-aot regions) needs from microVU.
		s32 offCycles = 0;                // mVU.cycles, from x24
		const void* cycleBreak = nullptr; // mVU.cycleBreak stub
		void* rootBlock = nullptr;        // the microBlock being compiled (pState at offset 0)
		// microBlockManager::search for (pc, key); returns the microBlock or null.
		void* (*searchBlock)(u32 vu, u32 pc, const u8* key) = nullptr;
		// Registers a block for (pc, key) with no code yet; returns the microBlock.
		void* (*addBlock)(u32 vu, u32 pc, const u8* key) = nullptr;
		void (*setBlockEntry)(void* block, void* entry) = nullptr;
		void* (*blockEntry)(void* block) = nullptr;
		// mVUblockFetch: the host entry for (pc, key), compiling at the cursor if needed.
		void* (*fetchBlock)(u32 vu, u32 pc, const u8* key) = nullptr;
		// mVUsetupRange for a block from startPC through its last pair.
		void (*addRange)(u32 vu, u32 startPC, u32 lastPC) = nullptr;
		// A program end inside a region (mVUendProgram's tail).
		const void* clearLpState = nullptr; // mVU1clearlpStateJIT
		const void* endFlagsB = nullptr;    // mVU.endProgramFlagsB
		const void* exitEBit = nullptr;     // mVUexitEBit
		const void* vpuStat = nullptr;      // &VU0.VI[REG_VPU_STAT].UL
		bool threadVU1 = false;             // THREAD_VU1
		// A JR/JALR inside a region, for targets the region does not hold:
		// emits microVU's own dispatch (normJumpCompile after its setupBranch)
		// for `block`, whose end state is `endKey`; the target is in mVU.branch.
		void (*emitJumpTail)(u32 vu, void* block, const u8* endKey) = nullptr;
		// microVU's own code for a block a region holds, for a run that is short of
		// the region's budget test. `rec` is the region's bail record: PC at 0, at 4
		// the signed distance from the record to a word holding the block's
		// microProgram, the block's microBlock at 8, the code address at 16 (written
		// once compiled). Called from generated code.
		void* (*bailTarget)(u32 vu, u8* rec) = nullptr;
		// x29 holds VURegs::Mem in micro mode (gprVUMem).
		bool vuMemPinned = false;
		// The microProgram being compiled (mVU.prog.cur). A region's bail
		// record keeps it: the bail runs when the region does, and
		// mVU.prog.cur is only microVU's idea of the current program (a jump
		// cache hit enters another program's code without updating it).
		void* program = nullptr;
		// offsetof(microProgram, vuAotBails): the region's bail code counts into it.
		s32 offProgBails = 0;
		// Told of every 8-byte data word a region writes that holds an address (the
		// program word and each bail record's block pointer), so a saved region can
		// re-point them. Null outside the background worker.
		void (*noteDataWord)(const u8* at, u64 value) = nullptr;
	};

#ifdef PCSX2_RECOMPILER_TESTS
	// Called by microVU at the end of every block's first pass when set.
	// Testing-only: absent from builds without the recompiler test hooks.
	using MvuBlockHook = void (*)(const MvuBlockTrace& block, void* user);
	extern MvuBlockHook g_mvuBlockHook;
	extern void* g_mvuBlockHookUser;
#endif

	// The compile hooks. Tests install them through the globals below;
	// the live tier (VuAotLive.h) hands microVU its own for a program it has
	// tiered up, so every build carries the call sites. A hook is only ever
	// consulted for VU1 blocks it asked for: with none set, microVU compiles
	// as it always has.

	// Called by microVU at the start of every block's second pass when set.
	// Returning true means the hook emitted the whole block body at armAsm's
	// cursor, honouring microVU's block-boundary state contract, and microVU
	// emits only its framing (cycle test already done, links, ends).
	using VuAotBodyHook = bool (*)(const MvuBlockTrace& block, const MvuEmitEnv& env, void* user);
	extern VuAotBodyHook g_vuAotBodyHook;
	extern void* g_vuAotBodyHookUser;

	// Called by microVU after every block's first pass, before it emits
	// anything, when set. Returning true means the hook emitted the block in
	// full at armAsm's cursor (a vu-aot region rooted at it: its own cycle
	// tests, links and exits back into microVU) and microVU emits nothing.
	using VuAotRegionHook = bool (*)(const MvuBlockTrace& block, const MvuEmitEnv& env, void* user);
	extern VuAotRegionHook g_vuAotRegionHook;
	extern void* g_vuAotRegionHookUser;
	// Set around a compile that must be microVU's own (a region's bail
	// target): the hooks above are skipped for that block.
	extern thread_local bool g_vuAotShadowCompile;
} // namespace vuaot
