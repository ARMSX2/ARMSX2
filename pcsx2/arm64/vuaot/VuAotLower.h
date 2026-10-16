// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Lowering of one microVU block's body, from vu-aot's per-node IR
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
// choice and the moves around them are free. pcsx2-vurunner --vuaot-run
// checks it on the whole corpus.
//
// Supported: VU1, clamp modes 0-3, no clamp-affecting gamefixes.

#include "VuAotIR.h"

#include <bitset>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vuaot
{
	// One flag per IR variable (milestone 8's region liveness).
	using LiveSet = std::bitset<ir::var::Count>;

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
		// Milestone 9: list-schedule each node's operations for an in-order
		// core (Cortex-A55/A53) before lowering.
		bool schedule = false;
		// microVU's micro mode keeps VURegs::Mem in x29 (gprVUMem) for the
		// whole dispatch: memory operations address off it.
		bool vuMemPinned = false;
		// Region mode: the sticky half of STATUS is the OR of every FMAC's zero
		// and sign lanes since the last reset, but only the instances somebody
		// reads (a reader, a bail or exit to microVU, a program end) are ever
		// seen. An FMAC whose instance is not read adds its lanes to vector
		// accumulators (HomeMap::saZ / saS) instead of packing a status word, and
		// the chain's word lives in HomeMap::stkReg, short of the bits the
		// accumulators still owe it. An instance somebody reads gets the exact
		// word microVU would have built (the node lowering folds the accumulators
		// in); the nodes' STATUS ring variables then carry only those instances.
		bool stickyAcc = false;
		// Relaxation 3 (BuildOptions::dropSticky): a STATUS update builds the instance from the
		// bits above the sticky byte of the one it depends on and its own pack, no sticky byte, and
		// a divide's flags carry the instance's I / D bits without the sticky I / D bits.
		bool dropSticky = false;
		// Test only: break relaxation 3 on purpose (1: the bits above the sticky byte are dropped too,
		// 2: the update's own pack is left out, 3: a divide's own I / D bits are left out).
		int dropStickyMutation = 0;
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
		// Milestone 5: a register holding the clamped copy of a vector
		// register the region never writes (1..31 VF, 32 ACC), or -1. Filled by
		// the region's entry loader; never stored back.
		s8 clampHome[33] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
		// Milestone 10: or a slot (0-7) of the top of microVU's backup area
		// holding that clamped copy, filled where the map is entered.
		s8 clampSlot[33] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

		// A register holding VF0 (+0, +0, +0, +1.0) for the whole map, or -1.
		s8 vf0Reg = -1;

		// Vector homes (bit r: VF r, bit 32: ACC) whose w lane the region never
		// writes: it stays in microVU's location, which stays valid, and the
		// register's w lane holds nothing at a node boundary. A write of x, y and
		// z then goes whole into the register; a node reading w loads it into the
		// register first; boundary stores write x, y and z only.
		u64 wMem = 0;
		bool WMem(u32 r) const { return (wMem >> r) & 1; }

		// LowerEnv::stickyAcc: the general register holding the status chain's
		// word (without the sticky bits the accumulators still owe it), and the
		// zero / sign accumulators. Fixed for every map of a region and taken from
		// the allocation pools, not the home lists; filled by the region's node
		// entries from microVU, never stored back. -1: none.
		s8 stkReg = -1, saZ = -1, saS = -1;

		u32 VRegMask() const;   // NEON registers taken by homes
		u32 GRegMask() const;   // general registers taken by homes
		// VRegMask() / GRegMask(), cached by Seal() once the homes are chosen
		// (the lowering reads them for every node).
		u32 vRegs = 0, gRegs = 0;
		void Seal()
		{
			vRegs = VRegMask();
			gRegs = GRegMask();
		}
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
		// Inside a vu-aot region: the branch condition is left sign-extended in
		// w8, or a JR/JALR's target byte address (the region's tail compares
		// it); nothing is stored to mVU.branch.
		Region,
	};

	// Debugging: when set, the lowering records where each IR instruction's
	// code starts.
	extern thread_local std::vector<std::pair<const u8*, ir::ValueId>>* g_lowerMarks;

	// Checks the IR can be lowered, then emits it at armAsm's cursor. Emits
	// nothing when it declines.
	LowerResult LowerNodeBody(const ir::Function& f, const LowerEnv& env);
	// checkOnly: decide only, emit nothing.
	LowerResult LowerNode(const ir::Function& f, const LowerEnv& env, const HomeMap& homes, LowerMode mode, bool checkOnly = false);

	// Region boundary helpers: move every register-homed variable between its
	// register and microVU's boundary location (and the cycle counter between
	// its register and mVU.cycles when `cyclesOff` is given).
	void EmitLoadHomes(const HomeMap& homes, const LowerEnv& env, s32 cyclesOff);
	// After EmitLoadHomes and the clamp bounds: the clamped homes.
	void EmitClampHomes(const HomeMap& homes, const LowerEnv& env);
	// A region node's lowering, analysed once when the region is built and
	// emitted once its homes are known.
	class NodeLowering
	{
	public:
		// Whether the node lowers inside a region (checked against microVU's
		// boundary homes; null if not). Adds to `counts` how many clamped uses
		// of each vector register's incoming value it has (index 1..31 VF,
		// 32 ACC), as the lowering would clamp them. `f` must outlive the
		// result.
		// `liveOut` (var::Count flags, or null for all): the variables live at
		// the node's end; the others' final values are neither computed nor
		// stored (milestone 8).
		static std::unique_ptr<NodeLowering> Check(const ir::Function& f, const LowerEnv& env, u32 counts[33], const LiveSet* liveOut = nullptr);
		// Whether Check would succeed, without its analysis (a region decides
		// node or exit with this, then analyses the nodes once it knows their
		// liveness).
		static bool Lowerable(const ir::Function& f, const LowerEnv& env);
		// Emits the node against the region's homes (at most once).
		LowerResult Emit(const HomeMap& homes);

		// LowerEnv::stickyAcc. Whether the node can leave the accumulators
		// holding bits the chain word lacks, for a node entered with none
		// ([0]) or possibly some ([1]); the region's dataflow over the node
		// links. Valid after Check.
		struct ChainEffect
		{
			bool dirtyOut[2] = {false, false};
			// Status updates the node accumulates instead of packing.
			u32 accumulated = 0;
			// Times the node folds the accumulators in (a reduce), entered clean / dirty.
			u32 reduces[2] = {0, 0};
		};
		ChainEffect Chain() const;
		// Whether the accumulators may hold such bits when the node is entered
		// (default yes). Set before Emit.
		void SetDirtyIn(bool dirty);
		~NodeLowering();
		struct Impl;

	private:
		explicit NodeLowering(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> impl_;
	};
	void EmitStoreHomes(const HomeMap& homes, const LowerEnv& env, s32 cyclesOff);
	// Milestone 10: moves the variables in `live` (null: all) from where
	// `from` keeps them to where `to` does (registers, microVU's locations),
	// and fills `to`'s clamped homes. The cycle counter and memory base are
	// the same in every map. Uses v0 and w16 as scratch.
	void EmitHomeTransfer(const HomeMap& from, const HomeMap& to, const LiveSet* live, const LowerEnv& env);
} // namespace vuaot
