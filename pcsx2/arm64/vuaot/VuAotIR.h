// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// vu-aot's intermediate form: SSA over the program graph (VuAotGraph), with
// VU-semantic operations.
//
// - One IR block per graph node (a microVU block variant), plus one entry
//   block per program entry that defines the incoming architectural state.
// - Every piece of VU state is an SSA variable: each VF lane separately (so a
//   .xyz write leaves .w's definition untouched and lane liveness falls out of
//   ordinary use-def chains), ACC lanes, VI1-15, the two qmmPQ Q lanes and
//   two P lanes, I, R, the four status / MAC / clip ring instances, the
//   pending divide flags, the VI backup slot, the latched XGKICK address, and
//   VU data memory as a token.
// - Flags are values. An FMAC whose MAC instance microVU writes defines that
//   ring variable with a MacFlags op; a status update reads the previous
//   instance and defines the next. A ring reorder at a block link is a
//   renaming of variables, not an instruction. Dead flag work is then plain
//   dead code.
// - Arithmetic ops are opaque and name the VU instruction they come from; the
//   backend lowers each to microVU's exact NEON sequence.
//
// Construction is Braun et al., "Simple and Efficient Construction of Static
// Single Assignment Form" (CC 2013), including its SCC pass that removes the
// redundant phi cycles irreducible control flow leaves behind.

#include "VuAotGraph.h"

#include <algorithm>
#include <initializer_list>
#include <type_traits>

#include <string>
#include <vector>

namespace vuaot::ir
{
	using ValueId = u32; // 0 = none

	// An instruction's operands: up to 12 inline (an FMAC's fs, ft and acc
	// lanes), more on the heap (the node end's every-variable list). A region
	// compile builds tens of thousands of instructions; most never allocate.
	class ArgVec
	{
	public:
		static constexpr u32 kInline = 12;
		ArgVec() = default;
		ArgVec(size_t n, ValueId v)
		{
			reserve(n);
			std::fill(data(), data() + n, v);
			size_ = static_cast<u32>(n);
		}
		ArgVec(std::initializer_list<ValueId> l) { Assign(l.begin(), l.size()); }
		ArgVec(const std::vector<ValueId>& v) { Assign(v.data(), v.size()); }
		ArgVec(const ArgVec& o) { Assign(o.data(), o.size_); }
		ArgVec(ArgVec&& o) noexcept { Take(o); }
		ArgVec& operator=(const ArgVec& o)
		{
			if (this != &o)
				Assign(o.data(), o.size_);
			return *this;
		}
		ArgVec& operator=(ArgVec&& o) noexcept
		{
			if (this != &o)
			{
				delete[] heap_;
				heap_ = nullptr;
				Take(o);
			}
			return *this;
		}
		ArgVec& operator=(const std::vector<ValueId>& v)
		{
			Assign(v.data(), v.size());
			return *this;
		}
		~ArgVec() { delete[] heap_; }

		size_t size() const { return size_; }
		bool empty() const { return size_ == 0; }
		ValueId* data() { return heap_ ? heap_ : inl_; }
		const ValueId* data() const { return heap_ ? heap_ : inl_; }
		ValueId& operator[](size_t i) { return data()[i]; }
		const ValueId& operator[](size_t i) const { return data()[i]; }
		ValueId* begin() { return data(); }
		ValueId* end() { return data() + size_; }
		const ValueId* begin() const { return data(); }
		const ValueId* end() const { return data() + size_; }
		ValueId& back() { return data()[size_ - 1]; }
		const ValueId& back() const { return data()[size_ - 1]; }
		void reserve(size_t n)
		{
			if (n > cap_)
				Grow(static_cast<u32>(n));
		}
		void push_back(ValueId v)
		{
			if (size_ == cap_)
				Grow(cap_ * 2);
			data()[size_++] = v;
		}
		// Appending only (pos must be end()).
		void insert(const ValueId* pos, size_t n, ValueId v)
		{
			(void)pos;
			for (size_t i = 0; i < n; i++)
				push_back(v);
		}
		template <typename It, typename = std::enable_if_t<!std::is_integral_v<It>>>
		void insert(const ValueId* pos, It first, It last)
		{
			(void)pos;
			for (; first != last; ++first)
				push_back(*first);
		}

	private:
		void Grow(u32 n)
		{
			ValueId* p = new ValueId[n];
			std::copy(data(), data() + size_, p);
			delete[] heap_;
			heap_ = p;
			cap_ = n;
		}
		void Assign(const ValueId* p, size_t n)
		{
			size_ = 0;
			reserve(n);
			std::copy(p, p + n, data());
			size_ = static_cast<u32>(n);
		}
		void Take(ArgVec& o)
		{
			size_ = o.size_;
			cap_ = o.cap_;
			heap_ = o.heap_;
			if (!heap_)
				std::copy(o.inl_, o.inl_ + size_, inl_);
			o.heap_ = nullptr;
			o.size_ = 0;
			o.cap_ = kInline;
		}
		ValueId inl_[kInline] = {};
		ValueId* heap_ = nullptr;
		u32 size_ = 0, cap_ = kInline;
	};

	// SSA variables (the VU state).
	namespace var
	{
		constexpr u32 VF(u32 reg, u32 lane) { return reg * 4 + lane; } // reg 1..31, lane 0..3 = x..w
		constexpr u32 ACC(u32 lane) { return 128 + lane; }
		constexpr u32 VI(u32 reg) { return 132 + reg; } // reg 1..15
		constexpr u32 QL0 = 148, QL1 = 149, PL0 = 150, PL1 = 151;
		constexpr u32 I = 152, R = 153;
		constexpr u32 S(u32 i) { return 154 + i; }
		constexpr u32 M(u32 i) { return 158 + i; }
		constexpr u32 C(u32 i) { return 162 + i; }
		constexpr u32 DIVFLAG = 166, VIBACKUP = 167, XGKADDR = 168, MEM = 169;
		constexpr u32 Count = 170;
		std::string Name(u32 v);
	} // namespace var

	enum class Type : u8
	{
		None,
		Vec,    // 128-bit, 4 float lanes (a lane variable reads one lane of it)
		Int,    // 32-bit integer (VI, flag words, addresses)
		Float,  // scalar float (Q, P, I, R)
		Mem,    // VU data memory state
		Bool,
	};

	enum class Op : u8
	{
		None,
		// structure
		LiveIn,      // incoming architectural state of `var` (imm) at a program entry
		Phi,
		Const,       // Int/Float: imm; Vec: VF0 (0,0,0,1)
		// upper
		Fmac,        // sub = UOp; args: fs[4], ft[4] (or ft[0] = I/Q/bc lane), acc[4]
		Opmula,      // ACC = Fs.yzx * Ft.zxy (fs[4], ft[4])
		Opmsub,      // Fd = ACC - Fs.yzx * Ft.zxy (fs[4], ft[4], acc[4])
		Fmac2,       // ABS/FTOI/ITOF: sub = UOp; args fs[4]
		ClipBits,    // the 6 judgement bits of CLIP: fs[4], ft.w
		// flags
		MacFlags,    // MAC word of an FMAC result: args result; mask
		StatusUpd,   // status update from an FMAC: args prev, result; mask, imm = doNonSticky
		StatusDiv,   // mVUdivSet: args prev, divflag
		StatusSet,   // FSSET: args prev; imm = Imm12
		ClipUpd,     // clip shift chain: args prev, bits
		ClipSet,     // FCSET: imm
		FlagRead,    // FMAND/FMEQ/FMOR/FSAND/FSEQ/FSOR/FCAND/FCEQ/FCOR/FCGET: sub = LOp; args flag[, vi]; imm
		// lower, integer
		IAlu,        // IADD/ISUB/IAND/IOR/IADDI/IADDIU/ISUBIU: sub = LOp; args a[, b]; imm
		IncVI,       // LQI/SQI post-increment and LQD/SQD pre-decrement: args vi; imm = +1/-1
		Mtir,        // args fs lane
		Xtop,        // XTOP / XITOP: sub = LOp (reads VIF registers)
		// lower, float / vector
		LoadQ,       // LQ/LQI/LQD: args mem, addr; imm = offset; mask
		LoadI,       // ILW/ILWR: args mem, addr; imm; mask
		StoreQ,      // SQ/SQI/SQD: args mem, addr, fs[4]; imm; mask -> Mem
		StoreI,      // ISW/ISWR: args mem, addr, it; imm; mask -> Mem
		Move,        // MOVE: args fs[4]
		Mr32,        // MR32: args fs[4]
		Mfir,        // MFIR: args vi
		Mfp,         // MFP: args p
		Div,         // DIV/SQRT/RSQRT result: sub = LOp; args fs lane, ft lane
		DivFlags,    // D/I flags of the same divide: sub = LOp; args fs lane, ft lane
		Efu,         // EFU op result (P): sub = LOp; args fs[4]
		RUpd,        // RINIT/RXOR/RNEXT: sub = LOp; args r[, fs lane]
		RGet,        // RGET/RNEXT vector result: args r
		AccClamp,    // clamp modes 2/3: MADD clamps microVU's cached ACC in place; mask = lanes clamped; args acc[4]
		IConst,      // link address of BAL/JALR: imm
		Kick,        // XGKICK transfer: args mem, addr -> Mem
		// control
		Cond,        // IBxx condition: sub = LOp; args a[, b]
		YieldCheck,  // block-entry cycle-budget test: may exit with the args as state
		ExitIf,      // run-time T-bit exit
		Exit,        // program end / bail: args = every variable; sub = ExitKind
		Leave,       // per-node mode: end of the node body; args = every variable,
		             // then the branch condition and the jump target (0 if none)
	};

	enum class ExitKind : u8
	{
		EBit,       // E-bit end
		JumpBail,   // indirect jump to a target outside the compiled set
		Unsupported // a construct vu-aot does not compile (evil branch, VU0 M-bit)
	};

	struct Inst
	{
		Op op = Op::None;
		Type type = Type::None;
		u8 mask = 0;      // dest / store mask, bit 3 = x
		u8 lane = 0;      // Phi / LiveIn on a lane variable: the lane
		u16 sub = 0;      // UOp / LOp / ExitKind
		u32 var = ~0u;    // Phi / LiveIn: the variable
		s32 imm = 0;
		u32 pc = 0;
		u32 word = 0;     // the VU instruction word (upper or lower) the op came from
		int block = -1;
		ArgVec args;
		bool dead = false; // removed trivial phi
	};

	enum class Term : u8
	{
		None, Jump, Branch, IndirectJump, Exit
	};

	struct Block
	{
		int node = -1;              // graph node, -1 for entry blocks
		std::vector<ValueId> phis;
		std::vector<ValueId> insts;
		std::vector<int> preds;
		std::vector<int> succs;     // Jump: [target]; Branch: [taken, fall]; IndirectJump: static targets
		std::vector<int> dispatchTargets; // IndirectJump: observed targets, entered through memory state
		Term term = Term::None;
		ValueId cond = 0;           // Branch condition, IndirectJump target VI
		u32 cycles = 0;             // microVU's static cycle count of the block
	};

	// A node that ends the program inside a vu-aot region (mVUendProgram):
	// what its tail needs once the Leave has handed every value back.
	struct EndInfo
	{
		bool valid = false;
		int fS = 0, fM = 0, fC = 0; // flag instances the end finalises
		int qInst = 0, pInst = 0;   // which qmmPQ lanes hold Q / P after the run-out
		u32 tpc = 0;                // TPC stored
	};

	struct Function
	{
		std::vector<Inst> insts;    // index = ValueId; insts[0] unused
		std::vector<Block> blocks;
		std::vector<int> entryBlocks;
		bool unsupported = false;
		std::string unsupportedWhy;
		EndInfo end;                // node-local region builds of a program end
		std::vector<u32> leaveArgs; // node-local builds: the Leave's non-zero operand indices, ascending
		// Node-local builds: the ring slot of the newest STATUS instance when the
		// block is entered (the next status write takes the slot after it), and the
		// one the successor will take as its newest, in the successor's numbering
		// (-1: the block ends the program).
		int statusHead = -1;
		int statusHeadOut = -1;

		// construction statistics
		u32 phisCreated = 0;
		u32 phisRemovedTrivial = 0;
		u32 phisRemovedScc = 0;
		u32 badOps = 0;             // illegal upper opcodes reached (program ends there, as in microVU)

		u32 LivePhis() const;
		u32 LiveInsts() const;
	};

	struct BuildOptions
	{
		u32 vuIndex = 1;
		bool clampIImmediate = true; // CHECK_VU_OVERFLOW && !CHECK_VU_SIGN_OVERFLOW (doIbit)
		bool xgkickHack = false;
		bool ibitHack = false;
		// Emit microVU's block-entry cycle-budget test (with the whole state
		// live) in every block. Off only for measuring what it costs.
		bool yieldChecks = true;
		// Node-local build for a vu-aot region: the Leave hands back each
		// variable after microVU's link-time ring reorder and Q/P lane swap
		// (mVUsetupBranch), i.e. in the successor's numbering.
		bool remapAtLeave = false;
		// With remapAtLeave: a plain E-bit end (or one on B/BAL) is modelled
		// in the node: the run-out's divide-flag update and kick happen before
		// the Leave, and Function::end says how to finish.
		bool regionEnds = false;
		// Clamp modes 2/3 (CHECK_VU_EXTRA_OVERFLOW / CHECK_VU_SIGN_OVERFLOW).
		bool clampExtra = false;
		bool clampSign = false;
		// The exactness contract's relaxation 3 (flag hack on, no program reads a sticky STATUS
		// bit): a STATUS update does not carry the sticky byte of the instance before it, so it
		// depends on that instance only through the bits a divide or FSSET put there (bits 18 and
		// up), which it takes from the last instance that wrote them; the updates in between are
		// then dead unless somebody reads them. A sticky-only update adds nothing.
		bool dropSticky = false;
	};

	// One microVU block alone, for milestone 2's per-block lowering: incoming
	// state comes from microVU's block-boundary homes (LiveIn in the entry
	// block), the body ends in a Leave that hands every variable's final value
	// back, and microVU's own framing does the cycle test, links and ends.
	Function BuildNodeLocal(const BlockResult& block, const BuildOptions& opts);
	// The same, borrowing `block` for the build instead of copying it (it is
	// handed back unchanged).
	Function BuildNodeLocal(BlockResult& block, const BuildOptions& opts);

	Function BuildSSA(const ProgramGraph& g, const BuildOptions& opts);

	// Node-local builds: whether the node's status updates form one chain, each
	// reading the previous update's instance (the first one reading the newest
	// instance on entry), and the instance the successor takes as its newest is
	// the chain's last. microVU's FSSET kill can leave a ring slot that no
	// instruction wrote as the "previous" one: the stale value then flows on, and
	// a lowering that carries the chain in registers cannot follow it.
	bool StatusChainLinear(const Function& f);

	// Checks SSA invariants: every operand defined, phis have one operand per
	// predecessor, a definition dominates each use. Returns problems found.
	std::vector<std::string> Verify(const Function& f);

	std::string Dump(const Function& f, const ProgramGraph& g);
	const char* OpName(Op op);
} // namespace vuaot::ir
