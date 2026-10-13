// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Lowering of one microVU block's body, from vuopt's per-node IR
// (ir::BuildNodeLocal) to AArch64 through vixl.
//
// Every piece of VU state has a home: the place it lives whenever control is
// at a node boundary. Milestone 2/3's homes are microVU's own block-boundary
// locations (VF/VI/ACC/I/R in VURegs, status instances in w20-w23, MAC and
// clip instances at [x24], Q/P in the lanes of q28, the divide flags, VI
// backup and kick address in microVU's fields), so the body can run inside
// microVU's block framing. Milestone 4's regions move some homes into host
// registers for the whole region (HomeMap).
//
// Inside a node, values live in host registers chosen by a local allocator:
// an incoming value is read from its home where it is first used and kept;
// a result stays in a register until its last use; the final value of each
// piece of state is moved to its home at the end of the node. A result that
// is the final value of a register-homed variable is computed straight into
// its home. Spills go to microVU's NEON backup area, which nothing else uses
// while a block body runs.
//
// Each operation emits the arithmetic microVU's emitter for it emits in the
// shipped configuration (same operations, operand order and clamp
// placement), so results are bit-identical by construction; the register
// choice and the moves around them are free. pcsx2-vurunner --vuopt-run
// checks it on the whole corpus.
//
// Supported: VU1, clamp modes 0-3, no clamp-affecting gamefixes.

#include "VuOptIR.h"

#include <string>
#include <utility>
#include <vector>

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
		s32 offNeonBackup = 48;         // mVU.neonBackup: the spill area
		const void* xgkickFn = nullptr; // void mVU_XGKICK_(u32 addr)
		const void* vifTop = nullptr;   // &VIFregisters.top
		const void* vifItop = nullptr;  // &VIFregisters.itop
		bool clampOverflow = true;      // CHECK_VU_OVERFLOW (clamp modes 1-3)
		bool clampExtra = false;        // CHECK_VU_EXTRA_OVERFLOW (modes 2-3): every FMAC step clamps its operands
		bool clampSign = false;         // CHECK_VU_SIGN_OVERFLOW (mode 3): those clamps preserve the sign; no result clamp
		bool softwareFlush = false;     // !FPCR.DAZ: the divide unit's denormal flush in software
		// Testing: allocate from at most this many NEON / general registers
		// (0 = all), to reach spills and early write-backs in small programs.
		u32 poolLimitV = 0, poolLimitG = 0;
	};

	// Where a variable lives at node boundaries.
	enum class HomeKind : u8
	{
		Mem,  // microVU's boundary location
		VReg, // a NEON register (VF / ACC: the whole vector)
		GReg, // a general register (VI, I, R, flag instances, divide flags, VI backup, kick address)
	};

	struct Home
	{
		HomeKind kind = HomeKind::Mem;
		u8 reg = 0;
	};

	// The status instances always live in w20-w23 and Q/P in q28's lanes, as
	// microVU keeps them; the map ignores those variables.
	struct HomeMap
	{
		Home home[ir::var::Count];
		// Register holding x19 + the VU1 memory offset, or -1.
		s8 memBase = -1;
		// Register holding mVU.cycles (region mode), or -1.
		s8 cycles = -1;

		u32 VRegMask() const;   // NEON registers taken by homes
		u32 GRegMask() const;   // general registers taken by homes
	};

	struct LowerResult
	{
		bool ok = false;
		std::string why;   // why the block was declined (nothing emitted)
		u32 bytes = 0;     // host code emitted
	};

	enum class LowerMode : u8
	{
		// Inside microVU's framing: the branch value / jump target is stored
		// to mVU.branch for microVU's tail.
		Framed,
		// Inside a vuopt region: the branch condition is left sign-extended in
		// w8 (the region's tail compares it); nothing is stored to mVU.branch.
		Region,
	};

	// Debugging: when set, the lowering records where each IR instruction's
	// code starts.
	extern std::vector<std::pair<const u8*, ir::ValueId>>* g_lowerMarks;

	// Checks the IR can be lowered, then emits it at armAsm's cursor. Emits
	// nothing when it declines.
	LowerResult LowerNodeBody(const ir::Function& f, const LowerEnv& env);
	// checkOnly: decide only, emit nothing.
	LowerResult LowerNode(const ir::Function& f, const LowerEnv& env, const HomeMap& homes, LowerMode mode, bool checkOnly = false);

	// Region boundary helpers: move every register-homed variable between its
	// register and microVU's boundary location (and the cycle counter between
	// its register and mVU.cycles when `cyclesOff` is given).
	void EmitLoadHomes(const HomeMap& homes, const LowerEnv& env, s32 cyclesOff);
	void EmitStoreHomes(const HomeMap& homes, const LowerEnv& env, s32 cyclesOff);
} // namespace vuopt
