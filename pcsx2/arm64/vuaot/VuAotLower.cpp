// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Node lowering. See VuAotLower.h for the contract.
//
// Each operation names the microVU emitter whose arithmetic it reproduces.
// Register choice is free (it cannot change a result); the arithmetic
// sequence, operand order and clamp placement are not, because they decide
// rounding, NaN propagation and flushes. Moves, copies and the order of
// independent instructions are free too.

#include "VuAotLower.h"

#include "VU.h"
#include "common/Assertions.h"
#include "arm64/AsmHelpers.h"
#include "arm64/microVU_Misc-arm64.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace vuaot
{
	namespace a64 = vixl::aarch64;
	using namespace ir;

	thread_local std::vector<std::pair<const u8*, ir::ValueId>>* g_lowerMarks = nullptr;

	namespace
	{
		constexpr u32 kExpMask = 0x7F800000;
		// A divide's flags in microVU's STATUS layout: the instance's own I / D bit (18, 19) and the
		// sticky I / D bit (24, 25; STATUS bits 10 and 11) that goes with it.
		constexpr u32 kDivI = 0x1040000;
		constexpr u32 kDivD = 0x2080000;
		constexpr u32 kDivCurrent = 0x000C0000;
		constexpr int kSlots = 32; // mVU.neonBackup[32]
		// The top of the backup area holds a region's clamped copies of
		// invariant registers kept in memory (HomeMap::clampSlot); node spills
		// never take them.
		constexpr int kClampSlots = 12;
		constexpr int kClampSlot0 = kSlots - kClampSlots;
		// Spills past those go to an overflow area, addressed through x16.
		constexpr int kOverflowSlots = 480;
		alignas(16) u8 s_overflow[kOverflowSlots * 16];

		// Pinned by microVU's dispatcher.
		const a64::Register& xState = a64::x19;
		const a64::Register& xFlag = a64::x24;
		const a64::Register& xGlob = a64::x25;
		constexpr int kPQ = 28, kCMax = 25, kCMin = 26;
		// The region tail's branch condition.
		constexpr int kCondReg = 8;

		// Allocatable registers. NEON: everything but the clamp bounds and PQ.
		// General: the caller-saved scratch and microVU's VI pool, but not
		// x8 (the branch condition), x16/x17 (vixl), x18, x19-x25 (pinned),
		// x29 (left alone for the frame pointer / pinned memory base).
		constexpr u32 kVAll = 0xFFFFFFFFu & ~((1u << kCMax) | (1u << kCMin) | (1u << kPQ));
		constexpr u32 kGAll = 0x0000FEFFu | (1u << 26) | (1u << 27) | (1u << 28);
		// Clobbered by a C call (AAPCS64): x0-x18; all of v0-v31 (only the low
		// halves of v8-v15 survive, and vectors need all 128 bits).
		constexpr u32 kGCallerSaved = 0x0003FFFFu;

		a64::VRegister V(int n) { return a64::VRegister(n, 128); }
		a64::VRegister Q(int n) { return a64::VRegister(n, 128); }
		a64::VRegister D(int n) { return a64::VRegister(n, 64); }
		a64::VRegister S(int n) { return a64::VRegister(n, 32); }
		a64::VRegister H(int n) { return a64::VRegister(n, 16); }
		a64::Register W(int n) { return a64::Register(n, 32); }
		a64::Register X(int n) { return a64::Register(n, 64); }

		s32 G(const void* p) { return static_cast<s32>(reinterpret_cast<const u8*>(p) - reinterpret_cast<const u8*>(&mVUglob)); }

		constexpr s32 kOffVF = static_cast<s32>(offsetof(VURegs, VF));
		constexpr s32 kOffVI = static_cast<s32>(offsetof(VURegs, VI));
		constexpr s32 kOffACC = static_cast<s32>(offsetof(VURegs, ACC));
		constexpr s32 kViStride = static_cast<s32>(sizeof(REG_VI)); // 16: VI registers are padded to 128 bits

		bool Single(u8 mask) { return mask == 8 || mask == 4 || mask == 2 || mask == 1; }
		int SingleLane(u8 mask) { return mask == 8 ? 0 : mask == 4 ? 1 : mask == 2 ? 2 : 3; }
		bool MaskLane(u8 mask, int lane) { return (mask >> (3 - lane)) & 1; }
		u8 FlipMask(u8 m) { return static_cast<u8>(((m & 1) << 3) | ((m & 2) << 1) | ((m & 4) >> 1) | ((m & 8) >> 3)); }
		int Ctz(u32 m) { return __builtin_ctz(m); }

		// Index of the vector home a VF/ACC lane variable belongs to: 1..31 =
		// VF, 32 = ACC; 0 = not a vector variable.
		u32 VecHomeOf(u32 var)
		{
			if (var < 128)
				return var / 4;
			if (var < 132)
				return 32;
			return 0;
		}

		// The variable a vector home's lane 0 is (the HomeMap entry for the
		// whole register).
		u32 VecVar(u32 r) { return r == 32 ? var::ACC(0) : var::VF(r, 0); }

		struct MemLoc
		{
			const a64::Register* base;
			s32 off;
			u8 size; // 2, 4 or 16
		};

		MemLoc BoundaryLoc(u32 v, const LowerEnv& env)
		{
			const u32 vr = VecHomeOf(v);
			if (vr == 32)
				return {&xState, kOffACC, 16};
			if (vr)
				return {&xState, kOffVF + static_cast<s32>(vr) * 16, 16};
			if (v >= var::VI(1) && v <= var::VI(15))
				return {&xState, kOffVI + static_cast<s32>(v - var::VI(0)) * kViStride, 2};
			if (v == var::I)
				return {&xState, kOffVI + REG_I * kViStride, 4};
			if (v == var::R)
				return {&xState, kOffVI + REG_R * kViStride, 4};
			if (v >= var::M(0) && v <= var::M(3))
				return {&xFlag, static_cast<s32>(v - var::M(0)) * 4, 4};
			if (v >= var::C(0) && v <= var::C(3))
				return {&xFlag, 16 + static_cast<s32>(v - var::C(0)) * 4, 4};
			if (v == var::DIVFLAG)
				return {&xFlag, env.offDivFlag, 4};
			if (v == var::VIBACKUP)
				return {&xFlag, env.offVIbackup, 4};
			if (v == var::XGKADDR)
				return {&xFlag, env.offVIxgkick, 4};
			return {nullptr, 0, 0};
		}

		bool IsStatus(u32 v) { return v >= var::S(0) && v <= var::S(3); }
		bool IsPQ(u32 v) { return v >= var::QL0 && v <= var::PL1; }

		// The variables a HomeMap may place in a general register.
		bool GHomeable(u32 v)
		{
			return (v >= var::VI(1) && v <= var::VI(15)) || v == var::I || v == var::R || (v >= var::M(0) && v <= var::C(3)) ||
				   v == var::DIVFLAG || v == var::VIBACKUP || v == var::XGKADDR;
		}

		// Where a value currently is.
		enum class Cls : u8
		{
			None,
			V, // NEON register (Vec, or a Float in lane 0)
			G, // general register (Int)
		};

		// How a value can be produced again without having been kept.
		enum class Remat : u8
		{
			None,     // computed: must be kept or spilled
			Home,     // incoming value, still in its memory home
			PQ,       // incoming Q/P lane of q28
			Const,    // Const / IConst immediate
			VF0,      // the VF0 constant (0,0,0,1), from VURegs.VF[0]
			MemBase,  // x19 + the VU1 memory offset
			Global,   // a vector constant in mVUglob (loaded off x25)
		};

		// The register file a value lives in, by what produces it.
		Cls ClsOf(const Inst& in)
		{
			switch (in.op)
			{
				case Op::LiveIn:
					return (VecHomeOf(in.var) || IsPQ(in.var)) ? Cls::V : (in.type == Type::Mem ? Cls::None : Cls::G);
				case Op::Const:
					return in.type == Type::Vec ? Cls::V : Cls::G;
				case Op::IConst:
				case Op::ClipBits:
				case Op::MacFlags:
				case Op::StatusUpd:
				case Op::StatusDiv:
				case Op::StatusSet:
				case Op::ClipUpd:
				case Op::ClipSet:
				case Op::FlagRead:
				case Op::IAlu:
				case Op::IncVI:
				case Op::Mtir:
				case Op::Xtop:
				case Op::LoadI:
				case Op::DivFlags:
				case Op::RUpd:
				case Op::Cond:
					return Cls::G;
				case Op::Fmac:
				case Op::Opmula:
				case Op::Opmsub:
				case Op::Fmac2:
				case Op::LoadQ:
				case Op::Move:
				case Op::Mr32:
				case Op::Mfir:
				case Op::Mfp:
				case Op::Div:
				case Op::Efu:
				case Op::RGet:
				case Op::AccClamp:
					return Cls::V;
				default:
					return Cls::None;
			}
		}

		class Lowerer
		{
		public:
			Lowerer(const Function& f, const LowerEnv& env, const HomeMap& hm, LowerMode mode)
				: f_(f)
				, env_(env)
				, hm_(&hm)
				, mode_(mode)
			{
				// The per-value tables reuse buffers earlier Lowerers on this
				// thread grew (a region keeps one Lowerer per node alive).
				auto& pool = s_pool;
				if (!pool.empty())
				{
					scratch_ = std::move(pool.back());
					pool.pop_back();
				}
				else
					scratch_ = std::make_unique<Scratch>();
				Swap();
				noEarly_.clear();
				slotBusy_.assign(kSlots + kOverflowSlots, 0);
			}
			~Lowerer()
			{
				Swap();
				if (s_pool.size() < 4096)
					s_pool.push_back(std::move(scratch_));
			}
			// The home-dependent half of Check again, for other homes (nothing
			// emitted yet).
			bool Rebind(const HomeMap& hm, std::string& why)
			{
				hm_ = &hm;
				return BindHomes(why);
			}
			Lowerer(const Lowerer&) = delete;
			Lowerer& operator=(const Lowerer&) = delete;

			bool Check(std::string& why);
			void Emit();

		private:
			// (never shrinks, so the lists past n keep their buffers for the
			// pseudo values and the next node; only lists in use are cleared)
			struct Lists : std::vector<std::vector<u32>>
			{
				size_t live = 0;
			};
			static void ResetLists(Lists& v, size_t n)
			{
				const size_t m = std::min(v.size(), std::max(v.live, n));
				for (size_t i = 0; i < m; i++)
					v[i].clear();
				if (v.size() < n)
					v.resize(n);
				v.live = n;
			}
			static std::vector<u32>& ListAt(Lists& v, size_t i)
			{
				if (v.size() <= i)
					v.resize(i + 1);
				v[i].clear();
				v.live = std::max(v.live, i + 1);
				return v[i];
			}
			// Buffers kept between Lowerers on one thread (compiles allocate
			// little after the first node).
			struct Scratch
			{
				std::vector<ValueId> order, canon, noEarly, clampOf, work;
				std::vector<Cls> cls;
				std::vector<Remat> remat;
				Lists uses, finalVars, cu, endAt, startAt;
				std::vector<u32> useCur, finalOf, constVal, lastUse, posOf, leaveKs;
				std::vector<u8> emit, homeReg, noDrop, slotBusy, clampFixed, folded, chainReal;
				std::vector<s8> reg, clampLane;
				std::vector<s16> slot;
				std::vector<const void*> globAddr;
				Inst leave2;
			};
			static thread_local std::vector<std::unique_ptr<Scratch>> s_pool;
			std::unique_ptr<Scratch> scratch_;
			std::vector<ValueId> work_;
			Lists cu_, endAt_, startAt_;
			std::vector<u32> lastUse_, posOf_;
			std::vector<u32> leaveKs_; // the Leave's non-zero operands (most variables are untouched)
			const LiveSet* liveOut_ = nullptr; // variables live at the node's end (null: all)
			Inst leave2_;                      // the Leave with dead variables zeroed (buffer from the pool)
			bool hasLeave2_ = false;
		public:
			void SetLiveOut(const LiveSet* live) { liveOut_ = live; }
			// Whether the node can be lowered at all, without the analysis.
			static bool CheckShape(const Function& f_, const LowerEnv& env_, std::string& why);
		private:
			const Inst& LeaveInst() const { return hasLeave2_ ? leave2_ : f_.insts[leave_]; }
			// Lane l of vector home r needs no value at the node's end: dead, or the
			// w lane of a HomeMap::wMem home.
			bool LaneFree(u32 r, u32 l) const
			{
				return (liveOut_ && !liveOut_->test(VecVar(r) + l)) || (l == 3 && hm_->WMem(r));
			}
			// The value that is the whole final value of vector home VF(v0):
			// every lane is it, or dead at the node's end (no merge needed for
			// a dead lane); 0 if none.
			ValueId WholeFinal(u32 v0) const
			{
				const Inst& lv = LeaveInst();
				ValueId c = 0;
				for (u32 l = 0; l < 4; l++)
				{
					const u32 v = v0 + l;
					const ValueId a = lv.args[v];
					if (LaneFree(VecHomeOf(v0), l))
						continue; // dead: any value will do
					if (!a)
						return 0;
					const Inst& x = I(C(a));
					if (x.op == Op::LiveIn && VecHomeOf(x.var) == VecHomeOf(v0))
						return 0; // its own incoming value
					if (c && C(a) != c)
						return 0;
					c = C(a);
				}
				return c;
			}
			// When every live lane of vector home VF(v0) is rewritten but by more
			// than one value: the value supplying most of them (two or more), which
			// the node end moves in whole before inserting the rest; 0 if none.
			ValueId MajorFinal(u32 v0) const
			{
				const Inst& lv = LeaveInst();
				ValueId srcs[4] = {};
				for (u32 l = 0; l < 4; l++)
				{
					const u32 v = v0 + l;
					if (LaneFree(VecHomeOf(v0), l))
						continue;
					const ValueId a = lv.args[v];
					if (!a)
						return 0;
					const Inst& x = I(C(a));
					if (x.op == Op::LiveIn && VecHomeOf(x.var) == VecHomeOf(v0))
						return 0;
					srcs[l] = C(a);
				}
				ValueId major = 0;
				int best = 0;
				for (u32 k = 0; k < 4; k++)
				{
					if (!srcs[k])
						continue;
					int n = 0;
					for (u32 j = 0; j < 4; j++)
						n += srcs[j] == srcs[k];
					if (n > best)
					{
						best = n;
						major = srcs[k];
					}
				}
				if (best < 2 || best == 4)
					return 0;
				const Op op = I(major).op;
				if (op == Op::LiveIn || op == Op::Const || op == Op::IConst)
					return 0;
				return major;
			}
			// f(k, arg) for each non-zero operand of v
			template <typename F>
			void ForArgs(ValueId v, const Inst& in, F f) const
			{
				if (v == leave_)
				{
					const ValueId* largs = LeaveInst().args.data();
					for (u32 k : leaveKs_)
						f(k, largs[k]);
					return;
				}
				const ValueId* args = in.args.data();
				const size_t n = in.args.size();
				for (size_t k = 0; k < n; k++)
					if (args[k])
						f(k, args[k]);
			}
			void Swap()
			{
				Scratch& x = *scratch_;
				order_.swap(x.order);
				canon_.swap(x.canon);
				noEarly_.swap(x.noEarly);
				clampOf_.swap(x.clampOf);
				work_.swap(x.work);
				cls_.swap(x.cls);
				remat_.swap(x.remat);
				uses_.swap(x.uses);
				finalVars_.swap(x.finalVars);
				cu_.swap(x.cu);
				endAt_.swap(x.endAt);
				startAt_.swap(x.startAt);
				useCur_.swap(x.useCur);
				finalOf_.swap(x.finalOf);
				constVal_.swap(x.constVal);
				lastUse_.swap(x.lastUse);
				posOf_.swap(x.posOf);
				leaveKs_.swap(x.leaveKs);
				emit_.swap(x.emit);
				homeReg_.swap(x.homeReg);
				noDrop_.swap(x.noDrop);
				slotBusy_.swap(x.slotBusy);
				clampFixed_.swap(x.clampFixed);
				folded_.swap(x.folded);
				chainReal_.swap(x.chainReal);
				reg_.swap(x.reg);
				clampLane_.swap(x.clampLane);
				slot_.swap(x.slot);
				globAddr_.swap(x.globAddr);
				std::swap(leave2_, x.leave2);
			}
			// n empty lists, keeping the buffers of the lists that stay.
			const Function& f_;
			const LowerEnv& env_;
			const HomeMap* hm_;
			const LowerMode mode_;
			u32 analysisN_ = 0; // f_.insts.size() + 1 (values before the pseudo values)
			bool BindHomes(std::string& why);
			int node_ = -1;
			std::vector<ValueId> order_; // the node's instructions
			ValueId leave_ = 0;

			// Per value (index = canonical ValueId; one extra pseudo value for
			// the memory base).
			std::vector<ValueId> canon_;
			std::vector<Cls> cls_;
			std::vector<Remat> remat_;
			Lists uses_;
			std::vector<u32> useCur_;
			std::vector<u8> emit_;      // DCE: instruction is needed
			std::vector<s8> reg_;       // register (in cls_'s file) or -1
			std::vector<s16> slot_;     // spill slot or -1
			std::vector<u8> homeReg_;   // sits in its own register home (pinned there until its last use)
			std::vector<u8> noDrop_;    // remat no longer valid (its memory home is being overwritten)
			ValueId memBase_ = 0;

			// LowerEnv::stickyAcc (region mode). The status chain's operations
			// (StatusUpd / StatusDiv / StatusSet) take the chain word from
			// HomeMap::stkReg rather than from the previous instance, so an
			// instance nobody reads is never built: its FMAC adds its zero and sign
			// lanes to the accumulators and nothing more.
			bool acc_ = false;
			bool dirtyIn_ = true;       // the accumulators may owe the chain word bits at the node's entry
			bool dirty_ = false;        // same, as emission goes
			std::vector<u8> chainReal_; // per chain operation: its instance word is read (by a reader, the node's end, ...)
			public:
			void SetDirtyIn(bool d) { dirtyIn_ = d; }
			// How the chain operations leave `dirty_`, from either entry state; valid after Check.
			void ChainEnds(bool out[2], u32& accumulated, u32 reduces[2]) const;
			private:
			static bool IsChainOp(Op op) { return op == Op::StatusUpd || op == Op::StatusDiv || op == Op::StatusSet; }
			// A status update whose pack the accumulators can take: sticky-only and in
			// the status layout (lane l's bits at l and l + 4; a single-lane field
			// puts its lane's bits at 0 and 4, which AccumulateFlags does by moving
			// the lane to lane 0). A MAC-layout pack reverses the lanes.
			static bool AccCand(const Inst& in) { return in.op == Op::StatusUpd && !(in.imm & 3); }
			static bool DefinesBase(bool real, const Inst& in) { return !(AccCand(in) && !real); }
			bool ChainStep(ValueId v, bool dirty) const;
			// Status operations that define a new chain word: every one but an
			// accumulation. baseIn_: the word each starts from (the previous one's
			// value in this node; absent: stkReg). finalBase_: the last word the
			// node defines, which its end moves into stkReg.
			std::vector<std::pair<ValueId, ValueId>> baseIn_;
			ValueId finalBase_ = 0;
			int BaseReg(ValueId id);
			void AccumulateFlags(ValueId c, u8 mask);
			void ReduceAcc(int dstG);
			void ChainStatusUpd(ValueId id);
			void ChainStatusDiv(ValueId id);
			void ChainStatusSet(ValueId id);

			ValueId vOwner_[32] = {};
			ValueId gOwner_[32] = {};
			u32 vLock_ = 0, gLock_ = 0;   // registers in use by the current instruction
			u32 vBound_ = 0, gBound_ = 0; // registers holding a value
			u32 vTemp_ = 0, gTemp_ = 0;   // temporaries of the current instruction
			u32 vPool_ = 0, gPool_ = 0;   // allocatable (homes excluded)
			std::vector<u8> slotBusy_;
			int AllocSlot();
			void FreeSlot(int s)
			{
				if (s >= kClampSlot0 && s < kSlots)
					return; // a region's clamped copy: it outlives the node
				slotBusy_[s] = 0;
			}
			void SlotStore(bool vec, int r, int s);
			void SlotLoad(bool vec, int r, int s);
			u32 pos_ = 0;

			// Final value per variable (Leave), for home preference.
			std::vector<u32> finalOf_; // per value: 1 + var it is the whole final value of (G homes / V homes), or 0
			// Early write-back: a value whose only remaining use is the end of
			// the node goes to its memory home when evicted, once nothing still
			// needs the incoming value there.
			u32 leavePos_ = 0;
			Lists finalVars_; // per value: the variables (lanes) it is the final value of
			ValueId varLiveIn_[var::Count] = {};      // canonical incoming value of each variable
			u8 early_[var::Count] = {};               // variable already written to its memory home
			std::vector<ValueId> noEarly_;            // read by the end of the node for something else
			bool HomeIsMem(u32 v) const;
			bool EarlyOk(ValueId c) const;
			void StoreEarly(ValueId c);

			const Inst& I(ValueId v) const { return f_.insts[v]; }
			ValueId C(ValueId v) const { return v ? canon_[v] : 0; }

			// ---- liveness ----
			u32 NextUse(ValueId c) const
			{
				const auto& u = uses_[c];
				u32 k = useCur_[c];
				while (k < u.size() && u[k] < pos_)
					k++;
				return k < u.size() ? u[k] : ~0u;
			}
			bool LiveAfter(ValueId c) const
			{
				const auto& u = uses_[c];
				for (u32 k = useCur_[c]; k < u.size(); k++)
					if (u[k] > pos_)
						return true;
				return false;
			}

			// ---- register state ----
			void Bind(ValueId c, int r)
			{
				reg_[c] = static_cast<s8>(r);
				if (cls_[c] == Cls::V)
				{
					vOwner_[r] = c;
					vBound_ |= 1u << r;
				}
				else
				{
					gOwner_[r] = c;
					gBound_ |= 1u << r;
				}
			}
			void Unbind(ValueId c)
			{
				if (reg_[c] < 0)
					return;
				if (cls_[c] == Cls::V)
				{
					vOwner_[reg_[c]] = 0;
					vBound_ &= ~(1u << reg_[c]);
				}
				else
				{
					gOwner_[reg_[c]] = 0;
					gBound_ &= ~(1u << reg_[c]);
				}
				reg_[c] = -1;
			}
			a64::MemOperand SlotMem(int s) const { return a64::MemOperand(xFlag, env_.offNeonBackup + s * 16); }

			void SpillValue(ValueId c);
			int FreeReg(Cls k, u32 avoid);
			int AllocReg(Cls k, ValueId owner, u32 avoid = 0);
			void Materialize(ValueId c, int r);
			int UseReg(ValueId v);          // value in a register of its class, locked
			int TempV();
			int TempG();
			int DefV(ValueId id);
			// inPlace: the instruction is one host instruction that reads its
			// operands before it writes, so the result may take the register of an
			// operand that dies here (x = x + 1 straight into x's home).
			int DefG(ValueId id, bool inPlace = false);
			void EndInst();
		public:
			void CountClampedLiveIns(u32 counts[33]) const
			{
				for (size_t c = 1; c < clampOf_.size(); c++)
				{
					if (!clampOf_[c] || c >= f_.insts.size())
						continue;
					const Inst& in = I(static_cast<ValueId>(c));
					if (in.op == Op::LiveIn && VecHomeOf(in.var))
						counts[VecHomeOf(in.var)] += static_cast<u32>(uses_[clampOf_[c]].size());
				}
			}
		private:
			void ReleaseLocks() { vLock_ = gLock_ = 0; vTemp_ = gTemp_ = 0; }
			void SpillForCall();

			// operand helpers
			int VecOperand(const ArgVec& args, size_t off);
			int ScalarV(ValueId v, int* lane);   // a register and lane holding the scalar (by-element form)
			int ScalarLane0(ValueId v);          // a register whose lane 0 is the scalar
			int IntOperand(ValueId v);           // a W register
			int LaneWord(ValueId v, int lane);   // lane `lane` of a vector value in a W register
			int Broadcast(ValueId v);            // scalar broadcast to 4 lanes (temp)
			int ClampInto(int src);              // mVUclamp1 into a temp
			// Milestone 5: every clamped copy of a value is made once. A clamp is
			// idempotent and microVU's clamps never write the value itself, so
			// the copy can serve every clamped use (the region's invariant
			// copies live in registers for the whole region: HomeMap::clampHome).
			std::vector<ValueId> clampOf_;  // per canonical value: its clamped copy (a pseudo value), or 0
			std::vector<s8> clampLane_;     // per clamped copy: the lane a scalar copy holds its value in
			std::vector<u8> clampFixed_;    // per canonical value: already the output of this mode's clamp (clamping it changes nothing)
			// Milestone 6: VI arithmetic on constants folded at compile time,
			// copies and repeated loads aliased to the value they repeat.
			std::vector<u8> folded_;        // per value: a constant computed here (constVal_)
			std::vector<u32> constVal_;
			bool ConstOf(ValueId v, u32& out) const;
			void FoldAndAlias();
			// Milestone 7: an FMAC's MAC and status packs share one sign/zero
			// comparison of the result, and mVUglob constants are loaded once
			// per block. A status update whose only reader is the next
			// non-sticky update is folded into it: the later update ORs every
			// pack of the chain into the sticky bits at once.
			std::vector<std::pair<std::pair<ValueId, int>, ValueId>> zsOf_; // (result, lane or -1) -> its sign/zero vector
			std::vector<std::pair<const void*, ValueId>> globOf_;            // mVUglob address -> its constant value
			std::vector<std::pair<u32, ValueId>> fcorConst_;                 // FCOR mask (shifted down) -> its constant value
			template <typename T, typename K>
			static ValueId Find(const T& tab, const K& key)
			{
				for (const auto& e : tab)
					if (e.first == key)
						return e.second;
				return 0;
			}
			std::vector<const void*> globAddr_;               // per pseudo value: the constant's address
			static const void* PackWeights(u8 mask, bool mflag);
			int SignZero(ValueId c, u8 mask, int& scratch, bool& owned);
			ValueId NewPseudo(Cls k, Remat r, const std::vector<u32>& uses);
			std::map<ValueId, std::vector<ValueId>> statusChain_; // non-sticky StatusUpd -> the updates folded into it, oldest first
			// A status update in the MAC layout packs the same word as the MacFlags of
			// the same result: (status update, that MacFlags) when both are built here,
			// so the pack is made once.
			std::vector<std::pair<ValueId, ValueId>> macOf_;
			ValueId MacOf(ValueId st) const
			{
				for (const auto& e : macOf_)
					if (e.first == st)
						return e.second;
				return 0;
			}
			// The update's pack in a register: the shared MAC word, else packed into `tmp`
			// (a general temporary).
			int StatusPackReg(ValueId id, int tmp);
			ValueId StatusBase(ValueId v) const; // the status value a (possibly folded) chain starts from
			void FoldStatusChains();
			void Schedule();
			static u32 Latency(const Inst& in);
			ValueId SingleSource(const ArgVec& args, size_t off) const;
			void ClampedOperands(ValueId id, std::vector<ValueId>& out) const;
			template <typename F>
			int ClampOperand(ValueId c, F fetch, int* lane = nullptr);
			void ClampKind(int dst, int src);
			int SignClampInto(int src);          // mVUclamp2's sign-preserving integer clamp into a temp
			int ClampE(int src) { return env_.clampSign ? SignClampInto(src) : ClampInto(src); } // clampE operand clamp
			bool ResultClamp() const { return env_.clampExtra && !env_.clampSign; }              // mVUclamp4
			void KeepOnly(u32 before, int t);
			void MemAddr(ValueId addrV, s32 imm, int& base, int& idx); // idx < 0: constant address, offset memOff_
			s32 memOff_ = 0;
			// base + index as one pointer register and an offset
			void MemPtr(int base, int idx, int& ptr, s32& off)
			{
				if (idx < 0)
				{
					ptr = base;
					off = memOff_;
					return;
				}
				armAsm->Add(X(idx), X(base), X(idx));
				ptr = idx;
				off = 0;
			}
			a64::MemOperand MemAt(int base, int idx) const
			{
				return idx < 0 ? a64::MemOperand(X(base), memOff_) : a64::MemOperand(X(base), X(idx));
			}

			void PackFlags(ValueId c, u8 mask, bool mflag, int dst, ValueId def = 0);
			void MaxMin(int to, int from, bool isMax, int out);
			static int ClampType(const UpperInst& u);

			void Fmac(ValueId id);
			void Opmul(ValueId id);
			void Fmac2(ValueId id);
			void ClipBits(ValueId id);
			void FlagRead(ValueId id);
			void Div(ValueId id);
			void DivFlags(ValueId id);
			void Efu(ValueId id);
			void Kick(ValueId id);
			void Leave(ValueId id);
			void One(ValueId id);

			// Region mode: a VI's register home holds the VI in its low 16 bits
			// and anything above them (no normalisation at node ends; every
			// reader of a VI value already takes only the bits it needs, and the
			// boundary stores are 16 bits wide). The two 32-bit fields that take
			// an old VI value, microVU's VI backup and kick address, still get
			// the zero-extended value they always got.
			bool WideViLiveIn(ValueId c) const;
			// FMEQ compares the whole register, and microVU's register holds a VI
			// from an earlier block zero-extended, so whatever the operand is
			// computed from in this node (VI arithmetic, FMAND / FMOR) must start
			// from zero-extended VIs too: those coming from register homes are
			// zero-extended in place where the node starts.
			void NormalizeFmeqInputs();
			// HomeMap::wMem: a node that reads the w lane of such a home's incoming
			// value loads it into the register where the node starts.
			void FillWMemLanes();
			bool TruncFor(u32 var, ValueId c) const
			{
				return (var == var::VIBACKUP || var == var::XGKADDR) && WideViLiveIn(c);
			}
		};

		//------------------------------------------------------------------
		// Allocation
		//------------------------------------------------------------------

		int Lowerer::AllocSlot()
		{
			for (int k = 0; k < kSlots + kOverflowSlots; k++)
				if (!slotBusy_[k] && !(k >= kClampSlot0 && k < kSlots))
				{
					slotBusy_[k] = 1;
					return k;
				}
			pxFailRel("vu-aot: out of spill slots");
			return 0;
		}

		void Lowerer::SlotStore(bool vec, int r, int s)
		{
			if (s < kSlots)
			{
				if (vec)
					armAsm->Str(Q(r), SlotMem(s));
				else
					armAsm->Str(W(r), SlotMem(s));
				return;
			}
			armMoveAddressToReg(a64::x16, s_overflow + (s - kSlots) * 16);
			if (vec)
				armAsm->Str(Q(r), a64::MemOperand(a64::x16));
			else
				armAsm->Str(W(r), a64::MemOperand(a64::x16));
		}

		void Lowerer::SlotLoad(bool vec, int r, int s)
		{
			if (s < kSlots)
			{
				if (vec)
					armAsm->Ldr(Q(r), SlotMem(s));
				else
					armAsm->Ldr(W(r), SlotMem(s));
				return;
			}
			armMoveAddressToReg(a64::x16, s_overflow + (s - kSlots) * 16);
			if (vec)
				armAsm->Ldr(Q(r), a64::MemOperand(a64::x16));
			else
				armAsm->Ldr(W(r), a64::MemOperand(a64::x16));
		}

		bool Lowerer::HomeIsMem(u32 v) const
		{
			if (IsStatus(v) || IsPQ(v) || v == var::MEM || v < 4)
				return false;
			const u32 vr = VecHomeOf(v);
			return hm_->home[vr ? VecVar(vr) : v].kind == HomeKind::Mem;
		}

		bool Lowerer::EarlyOk(ValueId c) const
		{
			if (c >= finalVars_.size() || finalVars_[c].empty() || remat_[c] != Remat::None || reg_[c] < 0)
				return false;
			if (std::find(noEarly_.begin(), noEarly_.end(), c) != noEarly_.end())
				return false;
			if (uses_[c].empty() || uses_[c].back() != leavePos_ || pos_ >= leavePos_ || NextUse(c) != leavePos_)
				return false;
			for (u32 v : finalVars_[c])
			{
				if (!HomeIsMem(v))
					return false;
				const ValueId li = varLiveIn_[v];
				if (li && NextUse(li) != ~0u)
					return false;
			}
			return true;
		}

		// Stores value c (in its register) to the memory homes of the variables
		// it is the final value of. No temporaries: this runs inside AllocReg.
		void Lowerer::StoreEarly(ValueId c)
		{
			const int r = reg_[c];
			u8 vecLanes[33] = {};
			for (u32 v : finalVars_[c])
			{
				const u32 vr = VecHomeOf(v);
				if (vr)
					vecLanes[vr] |= static_cast<u8>(1u << (v - VecVar(vr)));
				else
				{
					const MemLoc m = BoundaryLoc(v, env_);
					if (cls_[c] == Cls::G)
					{
						if (TruncFor(v, c))
							armAsm->Uxth(W(r), W(r));
						if (m.size == 2)
							armAsm->Strh(W(r), a64::MemOperand(*m.base, m.off));
						else
							armAsm->Str(W(r), a64::MemOperand(*m.base, m.off));
					}
					else
						armAsm->Str(S(r), a64::MemOperand(*m.base, m.off));
				}
				early_[v] = 1;
			}
			for (u32 vr = 1; vr <= 32; vr++)
			{
				const u8 lanes = vecLanes[vr];
				if (!lanes)
					continue;
				const MemLoc m = BoundaryLoc(VecVar(vr), env_);
				if (lanes == 0xf)
				{
					armAsm->Str(Q(r), a64::MemOperand(xState, m.off));
					continue;
				}
				int l = 0;
				while (l < 4)
				{
					if (!(lanes & (1 << l)))
					{
						l++;
						continue;
					}
					const bool pair = (l == 0 || l == 2) && (lanes & (1 << (l + 1)));
					if (l == 0)
					{
						if (pair)
							armAsm->Str(D(r), a64::MemOperand(xState, m.off));
						else
							armAsm->Str(S(r), a64::MemOperand(xState, m.off));
					}
					else
					{
						armAsm->Add(a64::x16, xState, m.off + l * 4);
						if (pair)
							armAsm->St1(V(r).V2D(), 1, a64::MemOperand(a64::x16));
						else
							armAsm->St1(V(r).V4S(), l, a64::MemOperand(a64::x16));
					}
					l += pair ? 2 : 1;
				}
			}
		}

		void Lowerer::SpillValue(ValueId c)
		{
			if (reg_[c] < 0)
				return;
			if (EarlyOk(c))
			{
				StoreEarly(c);
				Unbind(c);
				return;
			}
			if (remat_[c] == Remat::None || noDrop_[c])
			{
				if (slot_[c] < 0)
				{
					slot_[c] = static_cast<s16>(AllocSlot());
					SlotStore(cls_[c] == Cls::V, reg_[c], slot_[c]);
				}
			}
			Unbind(c);
		}

		// A free register of class k outside `avoid`, or -1.
		int Lowerer::FreeReg(Cls k, u32 avoid)
		{
			const u32 pool = (k == Cls::V ? vPool_ : gPool_) & ~avoid & ~(k == Cls::V ? vLock_ : gLock_);
			const ValueId* own = (k == Cls::V) ? vOwner_ : gOwner_;
			for (u32 m = pool; m; m &= m - 1)
			{
				const int r = Ctz(m);
				if (!own[r])
					return r;
			}
			return -1;
		}

		int Lowerer::AllocReg(Cls k, ValueId owner, u32 avoid)
		{
			int r = FreeReg(k, avoid);
			if (r < 0)
			{
				// Belady: the unlocked value used furthest ahead; a value that
				// can be produced again goes first at equal distance.
				const u32 pool = (k == Cls::V ? vPool_ : gPool_) & ~avoid & ~(k == Cls::V ? vLock_ : gLock_);
				const ValueId* own = (k == Cls::V) ? vOwner_ : gOwner_;
				u64 best = 0;
				for (u32 m = pool; m; m &= m - 1)
				{
					const int x = Ctz(m);
					const ValueId c = own[x];
					const u64 nu = NextUse(c);
					const bool cheap = (remat_[c] != Remat::None && !noDrop_[c]) || EarlyOk(c);
					const u64 score = (nu << 1) | (cheap ? 1 : 0);
					if (r < 0 || score > best)
					{
						best = score;
						r = x;
					}
				}
				if (r < 0)
					std::fprintf(stderr, "vu-aot: no register to spill: op %d at %u, class %d, pool %08x locked %08x\n",
						static_cast<int>(I(order_[std::min<size_t>(pos_, order_.size() - 1)]).op), pos_, static_cast<int>(k),
						k == Cls::V ? vPool_ : gPool_, k == Cls::V ? vLock_ : gLock_);
				pxAssertRel(r >= 0, "vu-aot: no register to spill");
				SpillValue(own[r]);
			}
			if (owner)
				Bind(owner, r);
			if (k == Cls::V)
				vLock_ |= 1u << r;
			else
				gLock_ |= 1u << r;
			return r;
		}

		// Produces value c (not currently in a register) into register r.
		void Lowerer::Materialize(ValueId c, int r)
		{
			if (slot_[c] >= 0)
			{
				SlotLoad(cls_[c] == Cls::V, r, slot_[c]);
				return;
			}
			if (remat_[c] == Remat::MemBase)
			{
				armAsm->Add(X(r), xState, kVU1MemFromState);
				return;
			}
			if (remat_[c] == Remat::Global)
			{
				armAsm->Ldr(Q(r), a64::MemOperand(xGlob, G(globAddr_[c])));
				return;
			}
			if (c >= f_.insts.size() && remat_[c] == Remat::Const)
			{
				armAsm->Mov(W(r), constVal_[c]); // a constant pseudo value (general class)
				return;
			}
			const Inst& in = I(c);
			switch (remat_[c])
			{
				case Remat::Home:
				{
					const MemLoc m = BoundaryLoc(in.var, env_);
					if (m.size == 16)
						armAsm->Ldr(Q(r), a64::MemOperand(*m.base, m.off));
					else if (m.size == 2)
						armAsm->Ldrh(W(r), a64::MemOperand(*m.base, m.off));
					else if (cls_[c] == Cls::V)
						armAsm->Ldr(S(r), a64::MemOperand(*m.base, m.off));
					else
						armAsm->Ldr(W(r), a64::MemOperand(*m.base, m.off));
					break;
				}
				case Remat::PQ:
					if (cls_[c] == Cls::V)
						armAsm->Dup(S(r), V(kPQ).V4S(), static_cast<int>(in.var - var::QL0));
					else
						armAsm->Umov(W(r), V(kPQ).V4S(), static_cast<int>(in.var - var::QL0));
					break;
				case Remat::Const:
				{
					const u32 k = folded_[c] ? constVal_[c] : static_cast<u32>(in.imm);
					if (cls_[c] == Cls::V)
					{
						armAsm->Mov(a64::w16, k);
						armAsm->Fmov(S(r), a64::w16);
					}
					else
						armAsm->Mov(W(r), k);
					break;
				}
				case Remat::VF0:
					armAsm->Ldr(Q(r), a64::MemOperand(xState, kOffVF));
					break;
				default:
					pxFailRel("vu-aot: value has no location");
			}
		}

		int Lowerer::UseReg(ValueId v)
		{
			const ValueId c = C(v);
			if (reg_[c] >= 0)
			{
				if (cls_[c] == Cls::V)
					vLock_ |= 1u << reg_[c];
				else
					gLock_ |= 1u << reg_[c];
				return reg_[c];
			}
			const int r = AllocReg(cls_[c], c);
			Materialize(c, r);
			return r;
		}

		int Lowerer::TempV()
		{
			const int r = AllocReg(Cls::V, 0);
			vTemp_ |= 1u << r;
			return r;
		}

		int Lowerer::TempG()
		{
			const int r = AllocReg(Cls::G, 0);
			gTemp_ |= 1u << r;
			return r;
		}

		int Lowerer::DefV(ValueId id)
		{
			const ValueId c = C(id);
			// A result that is the whole final value of a register-homed vector
			// goes straight to its home, once the incoming value there is dead.
			if (finalOf_[c])
			{
				const u32 var = finalOf_[c] - 1;
				const Home& h = hm_->home[var];
				if (h.kind == HomeKind::VReg && !vOwner_[h.reg] && !(vLock_ & (1u << h.reg)))
				{
					Bind(c, h.reg);
					homeReg_[c] = 1;
					vLock_ |= 1u << h.reg;
					return h.reg;
				}
			}
			return AllocReg(Cls::V, c);
		}

		int Lowerer::DefG(ValueId id, bool inPlace)
		{
			const ValueId c = C(id);
			if (finalOf_[c])
			{
				const u32 var = finalOf_[c] - 1;
				int hr = -1;
				if (IsStatus(var))
					hr = 20 + static_cast<int>(var - var::S(0));
				else if (hm_->home[var].kind == HomeKind::GReg)
					hr = hm_->home[var].reg;
				if (hr >= 0 && inPlace && gOwner_[hr] && (gLock_ & (1u << hr)) && !(gTemp_ & (1u << hr)))
				{
					// the home holds an operand of this instruction with no later use
					const ValueId o = gOwner_[hr];
					if (!LiveAfter(o) && !(homeReg_[o] && finalOf_[o]))
					{
						Unbind(o);
						if (slot_[o] >= 0)
						{
							FreeSlot(slot_[o]);
							slot_[o] = -1;
						}
					}
				}
				if (hr >= 0 && !gOwner_[hr] && !(gTemp_ & (1u << hr)) && (inPlace || !(gLock_ & (1u << hr))))
				{
					Bind(c, hr);
					homeReg_[c] = 1;
					gLock_ |= 1u << hr;
					return hr;
				}
			}
			return AllocReg(Cls::G, c);
		}

		void Lowerer::EndInst()
		{
			// Values whose last use was this instruction leave their registers.
			for (int file = 0; file < 2; file++)
			{
				const u32 lock = file ? gLock_ : vLock_;
				ValueId* own = file ? gOwner_ : vOwner_;
				for (u32 m = lock; m; m &= m - 1)
				{
					const int r = Ctz(m);
					const ValueId c = own[r];
					if (!c)
						continue;
					if (!LiveAfter(c))
					{
						// A dead incoming value in its own home frees the home for
						// the final value; a dead final value in its home stays.
						if (homeReg_[c] && finalOf_[c])
							continue;
						Unbind(c);
						if (slot_[c] >= 0)
						{
							FreeSlot(slot_[c]);
							slot_[c] = -1;
						}
					}
				}
			}
			// A value read only through an assembled or clamped operand was never
			// locked: free every register whose value has no use left, or it stays
			// taken until the node ends and is spilled when the file fills.
			for (int file = 0; file < 2; file++)
			{
				ValueId* own = file ? gOwner_ : vOwner_;
				// (home registers too: a dead incoming value there frees the home for
				// the variable's final value)
				for (u32 m = file ? gBound_ : vBound_; m; m &= m - 1)
				{
					const ValueId c = own[Ctz(m)];
					if (!c || (!uses_[c].empty() && uses_[c].back() > pos_) || (homeReg_[c] && finalOf_[c]))
						continue;
					Unbind(c);
					if (slot_[c] >= 0)
					{
						FreeSlot(slot_[c]);
						slot_[c] = -1;
					}
				}
			}
			vLock_ = gLock_ = 0;
			vTemp_ = gTemp_ = 0;
		}

		// Before a C call: nothing a caller-saved register holds survives.
		void Lowerer::SpillForCall()
		{
			for (int r = 0; r < 32; r++)
			{
				if (vOwner_[r] && !homeReg_[vOwner_[r]])
					SpillValue(vOwner_[r]);
				if (gOwner_[r] && (kGCallerSaved & (1u << r)) && !homeReg_[gOwner_[r]])
					SpillValue(gOwner_[r]);
			}
		}

		//------------------------------------------------------------------
		// Operands
		//------------------------------------------------------------------

		// Four lane sources args[off..off+3] (0 = not read) as one vector: the
		// register of a single source covering them, else a temp built with
		// inserts.
		int Lowerer::VecOperand(const ArgVec& args, size_t off)
		{
			const u32 lockedBefore = vLock_;
			ValueId first = 0;
			bool multi = false;
			for (int l = 0; l < 4; l++)
			{
				const ValueId v = off + l < args.size() ? args[off + l] : 0;
				if (!v)
					continue;
				if (!first)
					first = v;
				else if (C(v) != C(first))
					multi = true;
			}
			if (!first)
			{
				const int t = TempV();
				armAsm->Movi(V(t).V4S(), 0);
				return t;
			}
			if (!multi)
			{
				const ValueId c = C(first);
				if (cls_[c] == Cls::V)
					return UseReg(first);
				const int t = TempV();
				armAsm->Fmov(S(t), W(UseReg(first)));
				return t;
			}
			// The source covering the most lanes is the base.
			ValueId base = first;
			int bestCount = 0;
			for (int l = 0; l < 4; l++)
			{
				const ValueId v = args[off + l];
				if (!v)
					continue;
				int n = 0;
				for (int k = 0; k < 4; k++)
					n += args[off + k] && C(args[off + k]) == C(v);
				if (n > bestCount)
				{
					bestCount = n;
					base = v;
				}
			}
			const int t = TempV();
			armAsm->Mov(V(t).V16B(), V(UseReg(base)).V16B());
			for (int l = 0; l < 4; l++)
			{
				const ValueId v = args[off + l];
				if (!v || C(v) == C(base))
					continue;
				if (cls_[C(v)] == Cls::V)
					armAsm->Ins(V(t).V4S(), l, V(UseReg(v)).V4S(), l);
				else
					armAsm->Ins(V(t).V4S(), l, W(UseReg(v)));
			}
			// The sources stay bound; only the assembled copy is in use now.
			vLock_ = lockedBefore | (1u << t);
			return t;
		}

		// A scalar operand where a by-element instruction can read it: the
		// register and lane. Q/P lanes are read in place in q28.
		int Lowerer::ScalarV(ValueId v, int* lane)
		{
			const ValueId c = C(v);
			if (reg_[c] < 0 && slot_[c] < 0 && remat_[c] == Remat::PQ)
			{
				*lane = static_cast<int>(I(c).var - var::QL0);
				return kPQ;
			}
			*lane = 0;
			if (cls_[c] == Cls::V)
				return UseReg(v);
			const int t = TempV();
			armAsm->Fmov(S(t), W(UseReg(v)));
			return t;
		}

		int Lowerer::ScalarLane0(ValueId v)
		{
			int lane;
			const int r = ScalarV(v, &lane);
			if (lane == 0)
				return r;
			const int t = TempV();
			armAsm->Dup(S(t), V(r).V4S(), lane);
			return t;
		}

		int Lowerer::Broadcast(ValueId v)
		{
			int lane;
			const int r = ScalarV(v, &lane);
			const int t = TempV();
			armAsm->Dup(V(t).V4S(), V(r).V4S(), lane);
			return t;
		}

		int Lowerer::IntOperand(ValueId v)
		{
			const ValueId c = C(v);
			if (cls_[c] == Cls::G)
				return UseReg(v);
			if (reg_[c] < 0 && slot_[c] < 0 && remat_[c] == Remat::PQ)
			{
				const int t = TempG();
				armAsm->Umov(W(t), V(kPQ).V4S(), static_cast<int>(I(c).var - var::QL0));
				return t;
			}
			const int r = UseReg(v);
			const int t = TempG();
			armAsm->Fmov(W(t), S(r));
			return t;
		}

		int Lowerer::LaneWord(ValueId v, int lane)
		{
			const ValueId c = C(v);
			if (I(c).type != Type::Vec && !(I(c).op == Op::Const && I(c).type == Type::Vec))
				return IntOperand(v);
			const int r = UseReg(v);
			const int t = TempG();
			armAsm->Umov(W(t), V(r).V4S(), lane);
			return t;
		}

		// mVUclamp1 (mode 1 operand clamp) of a register, into a temp.
		int Lowerer::ClampInto(int src)
		{
			const int t = TempV();
			armAsm->Fminnm(V(t).V4S(), V(src).V4S(), V(kCMax).V4S());
			armAsm->Fmaxnm(V(t).V4S(), V(t).V4S(), V(kCMin).V4S());
			return t;
		}

		bool Lowerer::ConstOf(ValueId v, u32& out) const
		{
			if (!v || v >= folded_.size())
				return false;
			if (folded_[v])
			{
				out = constVal_[v];
				return true;
			}
			const Inst& in = I(v);
			if ((in.op == Op::Const || in.op == Op::IConst) && in.type != Type::Vec && in.type != Type::Float)
			{
				out = static_cast<u32>(in.imm);
				return true;
			}
			return false;
		}

		// VI arithmetic on constants becomes a constant; IADD/IOR with zero,
		// IADDI(U)/ISUBIU by zero become the value itself (32-bit arithmetic,
		// as microVU keeps VI values inside a block); a LoadQ from an address a
		// LoadQ or a full-width StoreQ already accessed, with no store in
		// between, becomes that value. Runs before uses are counted.
		void Lowerer::FoldAndAlias()
		{
			std::vector<std::pair<std::pair<ValueId, u32>, ValueId>> mem; // (address value or 0, imm/constant address) -> value
			const auto key = [&](ValueId addr, s32 imm) {
				u32 cv;
				if (ConstOf(C(addr), cv))
					return std::make_pair(static_cast<ValueId>(0), ((cv + static_cast<u32>(imm)) & 0x3ff));
				return std::make_pair(C(addr), static_cast<u32>(imm));
			};
			for (ValueId v : order_)
			{
				const Inst& in = I(v);
				switch (in.op)
				{
					case Op::IAlu:
					{
						const LOp op = static_cast<LOp>(in.sub);
						const bool two = op == LOp::IADD || op == LOp::ISUB || op == LOp::IAND || op == LOp::IOR;
						u32 a, b = 0;
						const bool ca = ConstOf(C(in.args[0]), a);
						const bool cb = two && in.args.size() > 1 && ConstOf(C(in.args[1]), b);
						if (ca && (!two || cb))
						{
							u32 r;
							switch (op)
							{
								case LOp::IADD: r = a + b; break;
								case LOp::ISUB: r = a - b; break;
								case LOp::IAND: r = a & b; break;
								case LOp::IOR: r = a | b; break;
								case LOp::ISUBIU: r = a - static_cast<u32>(in.imm); break;
								default: r = a + static_cast<u32>(in.imm); break;
							}
							folded_[v] = 1;
							constVal_[v] = r;
							remat_[v] = Remat::Const;
							break;
						}
						// x + 0, x | 0, x - 0 (an operand that is the constant 0)
						if (!two && in.imm == 0 && op != LOp::IAND)
							canon_[v] = C(in.args[0]);
						else if (two && cb && b == 0 && (op == LOp::IADD || op == LOp::ISUB || op == LOp::IOR))
							canon_[v] = C(in.args[0]);
						else if (two && ca && a == 0 && (op == LOp::IADD || op == LOp::IOR))
							canon_[v] = C(in.args[1]);
						break;
					}
					case Op::IncVI:
					{
						u32 a;
						if (ConstOf(C(in.args[0]), a))
						{
							folded_[v] = 1;
							constVal_[v] = in.imm > 0 ? a + 1 : a - 1;
							remat_[v] = Remat::Const;
						}
						break;
					}
					case Op::LoadQ:
					{
						const auto k = key(in.args[1], in.imm);
						bool hit = false;
						for (const auto& e : mem)
							if (e.first == k)
							{
								canon_[v] = C(e.second);
								hit = true;
								break;
							}
						if (!hit)
							mem.push_back({k, v});
						break;
					}
					case Op::StoreQ:
					{
						const auto k = key(in.args[1], in.imm);
						mem.clear(); // any address may alias
						if (in.mask == 0xf)
						{
							const ValueId src = SingleSource(in.args, 2);
							bool all = src != 0;
							for (int l = 0; l < 4 && all; l++)
								all = in.args[2 + l] != 0;
							if (all && cls_[src] == Cls::V)
								mem.push_back({k, src});
						}
						break;
					}
					case Op::StoreI:
						mem.clear();
						break;
					case Op::Cond:
					{
						// IBEQ / IBNE against a constant 0 (VI0) and the one-operand
						// forms test the operand itself.
						u32 b;
						if (in.args.size() < 2 || !in.args[1])
							canon_[v] = C(in.args[0]);
						else if (ConstOf(C(in.args[1]), b) && b == 0)
							canon_[v] = C(in.args[0]);
						else if (ConstOf(C(in.args[0]), b) && b == 0)
							canon_[v] = C(in.args[1]);
						break;
					}
					default:
						break;
				}
			}
		}

		// The single canonical value args[off..off+3] read, or 0 when they come
		// from several values (the operand is assembled) or none.
		ValueId Lowerer::SingleSource(const ArgVec& args, size_t off) const
		{
			ValueId c = 0;
			for (int l = 0; l < 4; l++)
			{
				const ValueId v = off + l < args.size() ? args[off + l] : 0;
				if (!v)
					continue;
				if (c && C(v) != c)
					return 0;
				c = C(v);
			}
			return c;
		}

		// The operands an instruction clamps whole, as Fmac / Opmul / AccClamp
		// emit them: one entry per clamped operand that is a single value.
		void Lowerer::ClampedOperands(ValueId id, std::vector<ValueId>& out) const
		{
			const Inst& in = I(id);
			const bool E = env_.clampExtra;
			if (in.op == Op::AccClamp)
			{
				if (const ValueId c = SingleSource(in.args, 0))
					out.push_back(c);
				return;
			}
			if (in.op == Op::Opmsub)
			{
				if (E)
					if (const ValueId c = SingleSource(in.args, 8))
						out.push_back(c);
				return;
			}
			if (in.op != Op::Fmac || (in.imm & 1))
				return;
			const UpperInst u = DecodeUpper(in.word);
			const bool arith = u.kind != FmacKind::Max && u.kind != FmacKind::Mini;
			const bool step = E && arith;
			const int ct = (env_.clampOverflow && !E) ? ClampType(u) : 0;
			if (step || (ct & 2))
				if (const ValueId c = SingleSource(in.args, 0))
					out.push_back(c);
			if (step || (ct & 1))
			{
				ValueId c = 0;
				if (u.form == FtForm::Reg)
					c = SingleSource(in.args, 4);
				else if (u.form == FtForm::Bc || u.form == FtForm::I || u.form == FtForm::Q)
					c = C(in.args[4]);
				if (c)
					out.push_back(c);
			}
			if (E && (u.kind == FmacKind::Madd || u.kind == FmacKind::Msub))
				if (const ValueId c = SingleSource(in.args, 8))
					out.push_back(c);
		}

		void Lowerer::ClampKind(int dst, int src)
		{
			if (env_.clampSign)
			{
				armAsm->Smin(V(dst).V4S(), V(src).V4S(), V(kCMax).V4S());
				armAsm->Umin(V(dst).V4S(), V(dst).V4S(), V(kCMin).V4S());
			}
			else
			{
				armAsm->Fminnm(V(dst).V4S(), V(src).V4S(), V(kCMax).V4S());
				armAsm->Fmaxnm(V(dst).V4S(), V(dst).V4S(), V(kCMin).V4S());
			}
		}

		// The clamped copy of value c: kept, reloaded, or made now from the
		// operand `fetch` produces (and kept for later clamped uses). `lane`:
		// for a scalar operand, the lane its value sits in.
		template <typename F>
		int Lowerer::ClampOperand(ValueId c, F fetch, int* lane)
		{
			if (c && c < clampFixed_.size() && clampFixed_[c])
				return fetch(); // clamped at its definition
			const ValueId pid = (c && c < clampOf_.size()) ? clampOf_[c] : 0;
			if (pid && (reg_[pid] >= 0 || slot_[pid] >= 0))
			{
				if (lane)
					*lane = clampLane_[pid];
				return UseReg(pid);
			}
			const int src = fetch();
			if (!pid)
			{
				const int t = TempV();
				ClampKind(t, src);
				return t;
			}
			const int r = AllocReg(Cls::V, pid);
			ClampKind(r, src);
			clampLane_[pid] = static_cast<s8>(lane ? *lane : 0);
			return r;
		}

		// mVUclamp2 at the sign-preserving modes: a signed integer min against
		// maxvals then an unsigned integer min against minvals, which maps an
		// exponent-255 word to the largest finite of its sign.
		int Lowerer::SignClampInto(int src)
		{
			const int t = TempV();
			armAsm->Smin(V(t).V4S(), V(src).V4S(), V(kCMax).V4S());
			armAsm->Umin(V(t).V4S(), V(t).V4S(), V(kCMin).V4S());
			return t;
		}

		// After an operand was copied into a temp, the registers it was built
		// from need not stay locked: back to `before` plus the temp.
		void Lowerer::KeepOnly(u32 before, int t) { vLock_ = before | (1u << t); }

		// mVUaddrFix + mVUmemAtIndex on VU1: ((vi + imm) & 0x3ff) * 16 from the
		// VU1 memory base.
		void Lowerer::MemAddr(ValueId addrV, s32 imm, int& base, int& idx)
		{
			u32 cv;
			if (ConstOf(C(addrV), cv))
			{
				// a constant address: an offset from the memory base
				idx = -1;
				memOff_ = static_cast<s32>(((cv + static_cast<u32>(imm)) & 0x3ff) * 16);
				base = UseReg(memBase_);
				return;
			}
			const int a = IntOperand(addrV);
			idx = TempG();
			if (imm > 0)
			{
				armAsm->Add(W(idx), W(a), static_cast<u32>(imm));
				armAsm->Ubfiz(W(idx), W(idx), 4, 10);
			}
			else if (imm < 0)
			{
				armAsm->Sub(W(idx), W(a), static_cast<u32>(-imm));
				armAsm->Ubfiz(W(idx), W(idx), 4, 10);
			}
			else
				armAsm->Ubfiz(W(idx), W(a), 4, 10);
			base = UseReg(memBase_);
		}

		// microVU's operand clamp table (clampType) for the shipped mode.
		int Lowerer::ClampType(const UpperInst& u)
		{
			constexpr int cFt = 1, cFs = 2;
			const bool ps = u.mask == 0xf;
			switch (u.op)
			{
				case UOp::SUB: case UOp::SUBi: case UOp::SUBq: case UOp::SUBbc:
					return ps ? (cFs | cFt) : 0;
				case UOp::MUL: case UOp::MULi: case UOp::MULq: case UOp::MULbc:
					return ps ? (cFs | cFt) : cFs;
				case UOp::MULAbc:
					return (u.bc == 3) ? (ps ? (cFs | cFt) : cFs) : cFs;
				case UOp::MADDbc:
				case UOp::MADDAbc:
					return cFs;
				default:
					return 0;
			}
		}

		// MAX/MINI: integer-order transform, then select (NEON_MAXPS/MINPS).
		void Lowerer::MaxMin(int to, int from, bool isMax, int out)
		{
			// The mask is built in the result register when that is not an operand,
			// so the select needs no copy.
			const bool inOut = out != to && out != from;
			const int a = inOut ? out : TempV(), b = TempV();
			armAsm->Sshr(V(a).V4S(), V(to).V4S(), 31);
			armAsm->Ushr(V(a).V4S(), V(a).V4S(), 1);
			armAsm->Eor(V(a).V16B(), V(a).V16B(), V(to).V16B());
			armAsm->Sshr(V(b).V4S(), V(from).V4S(), 31);
			armAsm->Ushr(V(b).V4S(), V(b).V4S(), 1);
			armAsm->Eor(V(b).V16B(), V(b).V16B(), V(from).V16B());
			if (isMax)
				armAsm->Cmgt(V(a).V4S(), V(a).V4S(), V(b).V4S());
			else
				armAsm->Cmgt(V(a).V4S(), V(b).V4S(), V(a).V4S());
			// out = a ? to : from
			if (!inOut)
				armAsm->Mov(V(out).V16B(), V(a).V16B());
			armAsm->Bsl(V(out).V16B(), V(to).V16B(), V(from).V16B());
		}

		const void* Lowerer::PackWeights(u8 mask, bool mflag)
		{
			if (Single(mask))
			{
				const int shift = mflag ? (3 - SingleLane(mask)) : 0;
				return shift ? static_cast<const void*>(&mVUglob.macWeights.bySSShift[0][shift][0])
							 : static_cast<const void*>(&mVUglob.macWeights.byMask[0][0][1][0]);
			}
			return &mVUglob.macWeights.byMask[0][mflag ? 1 : 0][mflag ? mask : FlipMask(mask)][0];
		}

		// mVUupdateFlags' per-lane sign/zero vector of result c (rotated so a
		// single-lane op's lane is lane 0), shared by the MAC and status packs
		// of the same result. `scratch`: a temporary free after the return, or
		// -1; `owned`: whether the returned register is a temporary.
		int Lowerer::SignZero(ValueId c, u8 mask, int& scratch, bool& owned)
		{
			const int lane = Single(mask) ? SingleLane(mask) : -1;
			const ValueId pid = Find(zsOf_, std::make_pair(c, lane));
			scratch = -1;
			owned = false;
			if (pid && (reg_[pid] >= 0 || slot_[pid] >= 0))
				return UseReg(pid);
			int src = UseReg(c);
			if (lane > 0)
			{
				const int t = TempV();
				armAsm->Ext(V(t).V16B(), V(src).V16B(), V(src).V16B(), lane * 4);
				src = t;
			}
			const int sgn = TempV();
			const int z = pid ? AllocReg(Cls::V, pid) : TempV();
			armAsm->Cmlt(V(sgn).V4S(), V(src).V4S(), 0);
			armAsm->Fcmeq(V(z).V4S(), V(src).V4S(), 0.0);
			armAsm->Sli(V(z).V4S(), V(sgn).V4S(), 4);
			scratch = sgn;
			owned = !pid;
			return z;
		}

		// mVUupdateFlags' sign/zero pack of result c into W(dst), or into the
		// register of value `def` when given (allocated after the operands).
		void Lowerer::PackFlags(ValueId c, u8 mask, bool mflag, int dst, ValueId def)
		{
			int scratch;
			bool zOwned;
			const int z = SignZero(c, mask, scratch, zOwned);
			const void* addr = PackWeights(mask, mflag);
			const ValueId gpid = Find(globOf_, addr);
			int w;
			bool wOwned = false;
			if (gpid)
				w = UseReg(gpid);
			else
			{
				w = scratch >= 0 ? scratch : TempV();
				scratch = -1;
				wOwned = true;
				armAsm->Ldr(Q(w), a64::MemOperand(xGlob, G(addr)));
			}
			const int t = zOwned ? z : wOwned ? w : scratch >= 0 ? scratch : TempV();
			armAsm->And(V(t).V16B(), V(z).V16B(), V(w).V16B());
			armAsm->Addv(S(t), V(t).V4S());
			if (def)
				dst = DefG(def);
			armAsm->Fmov(W(dst), S(t));
		}

		ValueId Lowerer::StatusBase(ValueId v) const
		{
			const auto ch = statusChain_.find(v);
			return C(I(ch != statusChain_.end() ? ch->second.front() : v).args[0]);
		}

		// A StatusUpd read only by the next non-sticky StatusUpd folds into it:
		// non-sticky, (s & 0xfffc00ff) | p | p << 8 overwrites the earlier
		// update's non-sticky bits, and every pack sits in the low byte, so
		// the chain is (base & 0xfffc00ff) | p1 | ... | pn | pn << 8. Runs
		// after dead code is marked, before uses are counted.
		void Lowerer::FoldStatusChains()
		{
			std::vector<u32> readers(canon_.size(), 0);
			for (ValueId v : order_)
			{
				if (!emit_[v])
					continue;
				ForArgs(v, I(v), [&](size_t k, ValueId a) {
					if (I(a).type == Type::Mem)
						return;
					if (v == leave_ && k < var::Count && I(a).op == Op::LiveIn && I(a).var == k)
						return;
					readers[C(a)]++;
				});
			}
			for (ValueId v : order_)
			{
				const Inst& in = I(v);
				if (!emit_[v] || in.op != Op::StatusUpd || !(in.imm & 1))
					continue;
				const ValueId prev = C(in.args[0]);
				if (prev >= f_.insts.size() || !emit_[prev] || I(prev).op != Op::StatusUpd || readers[prev] != 1)
					continue;
				std::vector<ValueId> chain;
				const auto it = statusChain_.find(prev);
				if (it != statusChain_.end())
				{
					chain = std::move(it->second);
					statusChain_.erase(it);
				}
				chain.push_back(prev);
				emit_[prev] = 0;
				statusChain_[v] = std::move(chain);
			}
		}

		//------------------------------------------------------------------
		// Sticky status accumulation (LowerEnv::stickyAcc)
		//------------------------------------------------------------------
		//
		// The status chain's head is always  stk | reduce(accumulators)  (stk =
		// HomeMap::stkReg, the accumulators HomeMap::saZ / saS). A status update
		// moves it on:
		//  - sticky-only, status layout, several lanes, instance unread: adds
		//    the result's zero and sign lanes to the accumulators;
		//  - any other update: builds its pack and ORs it into stk (a non-sticky
		//    one first folds the accumulators in, because its word keeps the
		//    sticky byte);
		//  - an instance somebody reads: built whole (stk | accumulators), and
		//    becomes stk, so the accumulators owe it nothing;
		//  - FSSET: replaces the sticky byte, so the accumulators are cleared.
		// The accumulators only ever add bits the chain is also owed (the sticky
		// byte is a plain OR of the packs since the last reset), so a bit left in
		// them after an instance was built is already in that instance.
		// `dirty_` tracks at compile time whether they may owe stk anything.

		bool Lowerer::ChainStep(ValueId v, bool dirty) const
		{
			const Inst& in = I(v);
			switch (in.op)
			{
				case Op::StatusUpd:
					if (AccCand(in))
						return !chainReal_[v];
					if (in.imm & 1)
						return false; // non-sticky: the low byte folds in
					return chainReal_[v] ? false : dirty;
				case Op::StatusDiv:
					return chainReal_[v] ? false : dirty;
				case Op::StatusSet:
					return false;
				default:
					return dirty;
			}
		}

		void Lowerer::ChainEnds(bool out[2], u32& accumulated, u32 reduces[2]) const
		{
			accumulated = 0;
			for (int init = 0; init < 2; init++)
			{
				bool d = init != 0;
				reduces[init] = 0;
				for (ValueId v : order_)
				{
					if (!emit_[v] || !IsChainOp(I(v).op))
						continue;
					const Inst& in = I(v);
					if (init == 0 && AccCand(in) && !chainReal_[v])
						accumulated++;
					// a reduce: an instance that has to be built while the accumulators owe the chain
					if (d && ((AccCand(in) && chainReal_[v]) || (in.op == Op::StatusUpd && !AccCand(in) && ((in.imm & 1) || chainReal_[v])) ||
							  (in.op == Op::StatusDiv && chainReal_[v])))
						reduces[init]++;
					d = ChainStep(v, d);
				}
				out[init] = d;
			}
		}

		int Lowerer::StatusPackReg(ValueId id, int tmp)
		{
			if (const ValueId m = MacOf(id))
				return UseReg(m);
			const Inst& in = I(id);
			const int p = tmp >= 0 ? tmp : TempG();
			PackFlags(C(in.args[1]), in.mask, (in.imm & 2) != 0, p);
			return p;
		}

		// dst = the sign and zero lanes the accumulators hold, packed like a status
		// update's pack (lane l's zero at bit l, its sign at bit l + 4).
		void Lowerer::ReduceAcc(int dst)
		{
			// s = sign mask lanes (all ones where negative) with the top nibble of the
			// zero mask lanes (all ones where zero) shifted down into bits 3:0: the
			// pack's [31:4] = sign, [3:0] = zero.
			const int s = TempV(), w = TempV();
			armAsm->Cmlt(V(s).V4S(), V(hm_->saS).V4S(), 0);
			armAsm->Sri(V(s).V4S(), V(hm_->saZ).V4S(), 28);
			armAsm->Ldr(V(w), a64::MemOperand(xGlob, G(&mVUglob.macWeights.byMask[0][0][0xf][0])));
			armAsm->And(V(s).V16B(), V(s).V16B(), V(w).V16B());
			armAsm->Addv(S(s), V(s).V4S());
			armAsm->Fmov(W(dst), S(s));
		}

		// Adds the field's lanes of result c to the accumulators: a lane's zero
		// flag (all ones where the lane is zero) and its sign bit.
		void Lowerer::AccumulateFlags(ValueId c, u8 mask)
		{
			if (!mask)
				return;
			const int r = UseReg(c);
			if (Single(mask))
			{
				// microVU rotates a single lane to lane 0 before packing it. Moving it
				// there as a scalar zeroes the other lanes of the copy, and the scalar
				// compare zeroes the other lanes of the mask: they add nothing.
				const int t = TempV(), z = TempV();
				armAsm->Dup(S(t), V(r).V4S(), SingleLane(mask));
				armAsm->Fcmeq(S(z), S(t), 0.0);
				armAsm->Orr(V(hm_->saZ).V16B(), V(hm_->saZ).V16B(), V(z).V16B());
				armAsm->Orr(V(hm_->saS).V16B(), V(hm_->saS).V16B(), V(t).V16B());
				return;
			}
			// The lanes outside the field hold whatever the full-width operation made of
			// the operands' lanes there; nobody reads them (the IR's lane variables name
			// only the field's lanes). Overwrite them with a positive normal number,
			// which adds neither a zero nor a sign.
			u8 outside = 0;
			for (int l = 0; l < 4; l++)
				if (!MaskLane(mask, l))
					outside |= static_cast<u8>(1u << l);
			if (outside)
			{
				int k = kCMax;
				if (!env_.clampOverflow)
				{
					k = TempV();
					armAsm->Movi(V(k).V4S(), 0x3f, a64::LSL, 24);
				}
				for (int h = 0; h < 2; h++)
				{
					const u8 pair = (outside >> (2 * h)) & 3;
					if (pair == 3)
						armAsm->Ins(V(r).V2D(), h, V(k).V2D(), h);
					else if (pair)
						armAsm->Ins(V(r).V4S(), 2 * h + (pair == 2 ? 1 : 0), V(k).V4S(), 2 * h + (pair == 2 ? 1 : 0));
				}
			}
			const int t = TempV();
			armAsm->Fcmeq(V(t).V4S(), V(r).V4S(), 0.0);
			armAsm->Orr(V(hm_->saZ).V16B(), V(hm_->saZ).V16B(), V(t).V16B());
			armAsm->Orr(V(hm_->saS).V16B(), V(hm_->saS).V16B(), V(r).V16B());
		}

		// The chain word a status operation starts from: the previous base-defining
		// operation's value in this node, else the incoming word in stkReg.
		int Lowerer::BaseReg(ValueId id)
		{
			for (const auto& e : baseIn_)
				if (e.first == id)
					return UseReg(e.second);
			return hm_->stkReg;
		}

		void Lowerer::ChainStatusUpd(ValueId id)
		{
			const Inst& in = I(id);
			const bool nonSticky = in.imm & 1;
			const bool real = chainReal_[id];
			const ValueId res = C(in.args[1]);
			pxAssertRel(hm_->stkReg >= 0 && hm_->saZ >= 0 && hm_->saS >= 0, "vu-aot: sticky accumulation without its registers");
			const bool dirty = dirty_;
			if (AccCand(in))
			{
				if (!real)
					AccumulateFlags(res, in.mask);
				else if (dirty)
				{
					const int bs = BaseReg(id);
					AccumulateFlags(res, in.mask);
					const int d = DefG(id);
					ReduceAcc(d);
					armAsm->Orr(W(d), W(d), W(bs));
				}
				else
				{
					const int bs = BaseReg(id);
					const int p = TempG();
					PackFlags(res, in.mask, false, p);
					const int d = DefG(id);
					armAsm->Orr(W(d), W(bs), W(p));
				}
			}
			else
			{
				const int bs = BaseReg(id);
				const int p = StatusPackReg(id, -1);
				const int d = DefG(id);
				if (!nonSticky)
				{
					if (real && dirty)
					{
						ReduceAcc(d);
						armAsm->Orr(W(d), W(d), W(bs));
						armAsm->Orr(W(d), W(d), W(p));
					}
					else
						armAsm->Orr(W(d), W(bs), W(p));
				}
				else
				{
					// (chain word | accumulators) & ~non-sticky, then this pack twice
					int src = bs;
					if (dirty)
					{
						src = TempG();
						ReduceAcc(src);
						armAsm->Orr(W(src), W(src), W(bs));
					}
					armAsm->And(W(d), W(src), 0xfffc00ffu);
					armAsm->Orr(W(d), W(d), W(p));
					armAsm->Orr(W(d), W(d), a64::Operand(W(p), a64::LSL, 8));
				}
			}
			dirty_ = ChainStep(id, dirty);
		}

		// mVUdivSet: the divide flags replace the instance's I / D bits (and set
		// their sticky bits). They sit above the sticky byte, so a pending
		// accumulator does not matter unless the instance is built.
		void Lowerer::ChainStatusDiv(ValueId id)
		{
			const Inst& in = I(id);
			const bool real = chainReal_[id];
			pxAssertRel(hm_->stkReg >= 0 && hm_->saZ >= 0 && hm_->saS >= 0, "vu-aot: sticky accumulation without its registers");
			const int bs = BaseReg(id);
			const int df = IntOperand(in.args[1]);
			const int d = DefG(id);
			if (real && dirty_)
			{
				ReduceAcc(d);
				armAsm->Orr(W(d), W(d), W(bs));
				armAsm->Bic(W(d), W(d), 0x000C0000u);
			}
			else
				armAsm->Bic(W(d), W(bs), 0x000C0000u);
			armAsm->Orr(W(d), W(d), W(df));
			dirty_ = ChainStep(id, dirty_);
		}

		// FSSET: keeps the non-sticky half, replaces the sticky byte (and the sticky
		// I / D / U / O bits): whatever the accumulators held is gone.
		void Lowerer::ChainStatusSet(ValueId id)
		{
			const Inst& in = I(id);
			pxAssertRel(hm_->stkReg >= 0 && hm_->saZ >= 0 && hm_->saS >= 0, "vu-aot: sticky accumulation without its registers");
			u32 imm = 0;
			const u32 i12 = static_cast<u32>(in.imm);
			if (i12 & 0x0040) imm |= 0x000000f;
			if (i12 & 0x0080) imm |= 0x00000f0;
			if (i12 & 0x0100) imm |= 0x0400000;
			if (i12 & 0x0200) imm |= 0x0800000;
			if (i12 & 0x0400) imm |= 0x1000000;
			if (i12 & 0x0800) imm |= 0x2000000;
			const int bs = BaseReg(id);
			const int d = DefG(id);
			armAsm->And(W(d), W(bs), 0xfff00u);
			if (imm)
				armAsm->Orr(W(d), W(d), imm);
			armAsm->Movi(V(hm_->saZ).V2D(), 0);
			armAsm->Movi(V(hm_->saS).V2D(), 0);
			dirty_ = false;
		}

		// Milestone 9: issue-to-result latency of an operation's lowering on
		// an in-order Cortex-A55 (approximate: NEON FP 4, loads 3-4, the flag
		// packs' compare/reduce chains about 6, divides and EFU sequences long).
		u32 Lowerer::Latency(const Inst& in)
		{
			switch (in.op)
			{
				case Op::Fmac: case Op::Opmula: case Op::Opmsub: case Op::AccClamp:
					return 4;
				case Op::Fmac2:
					return 4;
				case Op::MacFlags: case Op::StatusUpd: case Op::ClipBits:
					return 6;
				case Op::ClipUpd: case Op::StatusDiv: case Op::StatusSet: case Op::FlagRead:
					return 2;
				case Op::LoadQ:
					return 4;
				case Op::LoadI:
					return 3;
				case Op::Move: case Op::Mr32: case Op::Mfir: case Op::Mfp: case Op::RGet:
					return 2;
				case Op::Mtir:
					return 3;
				case Op::Div: case Op::DivFlags:
					return 12;
				case Op::Efu:
					return 20;
				case Op::RUpd:
					return 3;
				case Op::Kick:
					return 30;
				default:
					return 1;
			}
		}

		// List scheduling of the node's operations (in order_), keeping every
		// operand before its use, the memory order (a store or kick after every
		// load of the memory state it replaces) and the Leave last. Priority:
		// the longest latency path to the end; ties keep the original order.
		// Runs after dead code is marked, before uses are counted.
		void Lowerer::Schedule()
		{
			const u32 n = static_cast<u32>(order_.size());
			if (n < 3)
				return;
			// Nothing to overlap without two long-latency operations.
			{
				u32 longOps = 0;
				for (ValueId v : order_)
					if (emit_[v] && Latency(I(v)) >= 3)
						longOps++;
				if (longOps < 2)
					return;
			}
			static thread_local std::vector<s32> idx;
			static thread_local std::vector<u32> npred, height, ready, edgeStart, edges, out, readersOf;
			static thread_local std::vector<std::pair<u32, u32>> edgeList;
			const size_t N = f_.insts.size();
			if (idx.size() < N)
				idx.resize(N);
			for (u32 i = 0; i < n; i++)
				idx[order_[i]] = static_cast<s32>(i);
			edgeList.clear();
			const auto inBlock = [&](ValueId a) {
				return a && a < N && idx[a] >= 0 && static_cast<u32>(idx[a]) < n && order_[idx[a]] == a;
			};
			for (u32 i = 0; i < n; i++)
			{
				const ValueId v = order_[i];
				if (v == leave_)
					continue;
				const Inst& in = I(v);
				for (ValueId a : in.args)
				{
					if (inBlock(a))
						edgeList.push_back({static_cast<u32>(idx[a]), i});
					// an operand aliased to an earlier value reads that value
					const ValueId c = C(a);
					if (c != a && inBlock(c))
						edgeList.push_back({static_cast<u32>(idx[c]), i});
				}
				// a memory writer after every reader of the state it replaces
				// (the memory operations, in program order)
				if (in.type == Type::Mem)
					for (ValueId a : in.args)
						if (a && I(a).type == Type::Mem)
							for (u32 j = 0; j < n; j++)
							{
								if (j == i || order_[j] == leave_)
									continue;
								const Inst& r = I(order_[j]);
								if (r.op != Op::LoadQ && r.op != Op::LoadI && r.type != Type::Mem)
									continue;
								for (ValueId b : r.args)
									if (b == a)
									{
										edgeList.push_back({j, i});
										break;
									}
							}
			}
			// A status update that shares its result's MAC word comes after the
			// MacFlags that builds it.
			for (u32 i = 0; i < n; i++)
			{
				const ValueId v = order_[i];
				if (v == leave_ || !emit_[v] || I(v).op != Op::StatusUpd || !(I(v).imm & 2))
					continue;
				for (u32 j = 0; j < n; j++)
				{
					const ValueId m = order_[j];
					if (j != i && emit_[m] && I(m).op == Op::MacFlags && I(m).args[0] == I(v).args[1] && I(m).mask == I(v).mask)
					{
						edgeList.push_back({j, i});
						break;
					}
				}
			}
			// A variable's final value after every read of its incoming value:
			// the final value can then go straight into the variable's home
			// (register homes hold the incoming value until its last read).
			{
				static thread_local ValueId liOf[var::Count];
				std::fill(std::begin(liOf), std::end(liOf), 0);
				for (size_t v = 1; v < f_.insts.size(); v++)
					if (f_.insts[v].op == Op::LiveIn && f_.insts[v].var < var::Count)
						liOf[f_.insts[v].var] = canon_[v];
				const Inst& lv = LeaveInst();
				for (u32 k : leaveKs_)
				{
					if (k >= var::Count)
						break;
					const ValueId fin = C(lv.args[k]);
					if (!inBlock(fin))
						continue;
					const ValueId li = liOf[k];
					if (!li)
						continue;
					// (only reads before it in program order: those edges keep the
					// graph acyclic)
					for (u32 j = 0; j < static_cast<u32>(idx[fin]); j++)
					{
						const ValueId u = order_[j];
						if (u == leave_ || u == fin)
							continue;
						for (ValueId x : I(u).args)
							if (x && C(x) == li)
							{
								edgeList.push_back({j, static_cast<u32>(idx[fin])});
								break;
							}
					}
				}
			}
			// adjacency (CSR) and predecessor counts
			npred.assign(n, 0);
			edgeStart.assign(n + 1, 0);
			for (const auto& e : edgeList)
			{
				edgeStart[e.first + 1]++;
				npred[e.second]++;
			}
			for (u32 i = 0; i < n; i++)
				edgeStart[i + 1] += edgeStart[i];
			edges.resize(edgeList.size());
			{
				static thread_local std::vector<u32> fill;
				fill.assign(edgeStart.begin(), edgeStart.end() - 1);
				for (const auto& e : edgeList)
					edges[fill[e.first]++] = e.second;
			}
			// heights, in reverse order (operands come first in order_)
			height.assign(n, 0);
			for (u32 i = n; i-- > 0;)
			{
				const ValueId v = order_[i];
				u32 h = 0;
				for (u32 k = edgeStart[i]; k < edgeStart[i + 1]; k++)
					h = std::max(h, height[edges[k]]);
				height[i] = h + (emit_[v] ? Latency(I(v)) : 0);
			}
			// cycle-by-cycle, one issue per cycle
			ready.assign(n, 0); // earliest cycle each operation's operands are ready
			out.clear();
			static thread_local std::vector<u32> avail;
			avail.clear();
			u32 leaveIdx = n;
			for (u32 i = 0; i < n; i++)
			{
				if (order_[i] == leave_)
					leaveIdx = i;
				else if (npred[i] == 0)
					avail.push_back(i);
			}
			// Register pressure: vector results still to be read. Above the
			// limit, an operation that frees more registers than it takes goes
			// first (latency only breaks ties), so scheduling does not push the
			// allocator into spills.
			static thread_local std::vector<u32> remUses;
			static thread_local std::vector<u8> liveOutV;
			remUses.assign(n, 0);
			liveOutV.assign(n, 0);
			{
				const Inst& lv = LeaveInst();
				for (u32 k : leaveKs_)
				{
					const ValueId c = C(lv.args[k]);
					if (inBlock(c))
						liveOutV[idx[c]] = 1;
				}
				for (u32 j = 0; j < n; j++)
				{
					const ValueId u = order_[j];
					if (u == leave_ || !emit_[u])
						continue;
					const Inst& x = I(u);
					for (size_t a = 0; a < x.args.size(); a++)
					{
						const ValueId c = C(x.args[a]);
						bool dup = false;
						for (size_t b = 0; b < a && !dup; b++)
							dup = C(x.args[b]) == c;
						if (!dup && inBlock(c))
							remUses[idx[c]]++;
					}
				}
			}
			const auto isVec = [&](u32 j) { return emit_[order_[j]] && cls_[order_[j]] == Cls::V; };
			const auto delta = [&](u32 j) {
				int d = (isVec(j) && (remUses[j] || liveOutV[j])) ? 1 : 0;
				const Inst& x = I(order_[j]);
				for (size_t a = 0; a < x.args.size(); a++)
				{
					const ValueId c = C(x.args[a]);
					bool dup = false;
					for (size_t b = 0; b < a && !dup; b++)
						dup = C(x.args[b]) == c;
					if (!dup && inBlock(c) && isVec(idx[c]) && remUses[idx[c]] == 1 && !liveOutV[idx[c]])
						d--;
				}
				return d;
			};
			const u32 kPressure = 8;
			u32 liveV = 0;
			u32 cycle = 0;
			while (!avail.empty())
			{
				size_t best = 0;
				const bool tight = liveV >= kPressure;
				for (size_t k = 1; k < avail.size(); k++)
				{
					const u32 a = avail[k], b = avail[best];
					if (tight)
					{
						const int da = delta(a), db = delta(b);
						if (da != db)
						{
							if (da < db)
								best = k;
							continue;
						}
					}
					const bool ra = ready[a] <= cycle, rb = ready[b] <= cycle;
					if (ra != rb ? ra
								 : ra ? (height[a] != height[b] ? height[a] > height[b] : a < b)
									  : (ready[a] != ready[b] ? ready[a] < ready[b] : (height[a] != height[b] ? height[a] > height[b] : a < b)))
						best = k;
				}
				const u32 i = avail[best];
				// pressure bookkeeping
				{
					const Inst& x = I(order_[i]);
					if (emit_[order_[i]])
						for (size_t a = 0; a < x.args.size(); a++)
						{
							const ValueId c = C(x.args[a]);
							bool dup = false;
							for (size_t b = 0; b < a && !dup; b++)
								dup = C(x.args[b]) == c;
							if (!dup && inBlock(c) && remUses[idx[c]] && --remUses[idx[c]] == 0 && isVec(idx[c]) && !liveOutV[idx[c]] && liveV)
								liveV--;
						}
					if (isVec(i) && (remUses[i] || liveOutV[i]))
						liveV++;
				}
				avail[best] = avail.back();
				avail.pop_back();
				const ValueId v = order_[i];
				const bool live = emit_[v] != 0;
				if (live)
				{
					cycle = std::max(cycle, ready[i]);
					cycle++;
				}
				out.push_back(i);
				const u32 doneAt = live ? cycle - 1 + Latency(I(v)) : 0;
				for (u32 k = edgeStart[i]; k < edgeStart[i + 1]; k++)
				{
					const u32 s = edges[k];
					ready[s] = std::max(ready[s], doneAt);
					if (--npred[s] == 0 && s != leaveIdx)
						avail.push_back(s);
				}
			}
			pxAssertRel(out.size() + (leaveIdx < n ? 1 : 0) == n, "vu-aot: scheduling lost an operation");
			static thread_local std::vector<ValueId> newOrder;
			newOrder.clear();
			for (u32 i : out)
				newOrder.push_back(order_[i]);
			if (leaveIdx < n)
				newOrder.push_back(leave_);
			order_.assign(newOrder.begin(), newOrder.end());
			for (u32 i = 0; i < n; i++)
				idx[order_[i]] = -1;
		}

		ValueId Lowerer::NewPseudo(Cls k, Remat r, const std::vector<u32>& uses)
		{
			const ValueId pid = static_cast<ValueId>(canon_.size());
			canon_.push_back(pid);
			cls_.push_back(k);
			remat_.push_back(r);
			ListAt(uses_, pid).assign(uses.begin(), uses.end());
			useCur_.push_back(0);
			emit_.push_back(0);
			reg_.push_back(-1);
			slot_.push_back(-1);
			homeReg_.push_back(0);
			noDrop_.push_back(0);
			finalOf_.push_back(0);
			ListAt(finalVars_, pid);
			globAddr_.push_back(nullptr);
			folded_.push_back(0);
			constVal_.push_back(0);
			return pid;
		}

		//------------------------------------------------------------------
		// Operations
		//------------------------------------------------------------------

		// Clamp placement by mode, as microVU's FMAC bodies emit it:
		//   mode 1: the per-op clampType clamps (mVUclamp1 on Fs / Ft) only;
		//   modes 2-3 (clampE): clampType is silent; instead every ADD/SUB/MUL
		//     step clamps both its operands (mVUclampStepOperands: mVUclamp2,
		//     sign-preserving at mode 3), and at mode 2 clamps its result
		//     (mVUclamp4). A multiply-accumulate is two steps: the product's
		//     result clamp stands in for its operand clamp in the second step
		//     at mode 2 (preClamped); at mode 3 the product is clamped there.
		//   MAX/MINI never clamp; mode 0 never clamps.
		void Lowerer::Fmac(ValueId id)
		{
			const Inst& in = I(id);
			const UpperInst u = DecodeUpper(in.word);
			if (in.imm & 1)
			{
				// doSafeSub: Ft == Fs SUB/SUBA writes +0, ahead of every clamp.
				const int d = DefV(id);
				armAsm->Movi(V(d).V4S(), 0);
				return;
			}
			const bool E = env_.clampExtra;
			const bool arith = u.kind != FmacKind::Max && u.kind != FmacKind::Mini;
			const bool step = E && arith; // the step clamps
			const int ct = (env_.clampOverflow && !E) ? ClampType(u) : 0;
			const auto opClamp = [&](int r, bool typeBit) {
				if (step)
					return ClampE(r);
				if (typeBit)
					return ClampInto(r);
				return r;
			};
			(void)opClamp;
			const u32 l0 = vLock_;
			int fs;
			if (step || (ct & 2))
			{
				fs = ClampOperand(SingleSource(in.args, 0), [&] { return VecOperand(in.args, 0); });
				KeepOnly(l0, fs);
			}
			else
				fs = VecOperand(in.args, 0);
			const u32 l1 = vLock_;
			// Ft: a whole register, or a scalar (one register lane) that the
			// multiply reads by element and the other kinds broadcast. A clamp
			// of the whole source register clamps that lane the same way.
			// setupFtReg loads I into a writable temp mapped as VF0 rather than
			// as the I register, so checkVFClamp does not exempt it and I is
			// clamped like any Ft.
			int ft = -1, ftLane = -1;
			const bool ftClamp = step || (ct & 1);
			switch (u.form)
			{
				case FtForm::Reg:
					ft = ftClamp ? ClampOperand(SingleSource(in.args, 4), [&] { return VecOperand(in.args, 4); })
								 : VecOperand(in.args, 4);
					break;
				case FtForm::Bc:
					ft = ftClamp ? ClampOperand(C(in.args[4]), [&] { return UseReg(in.args[4]); }) : UseReg(in.args[4]);
					ftLane = in.lane;
					break;
				case FtForm::I:
				case FtForm::Q:
					if (ftClamp)
						ft = ClampOperand(C(in.args[4]), [&] { return ScalarV(in.args[4], &ftLane); }, &ftLane);
					else
						ft = ScalarV(in.args[4], &ftLane);
					break;
				default:
				{
					ft = TempV();
					armAsm->Movi(V(ft).V4S(), 0);
					if (ftClamp)
						ft = ClampOperand(0, [&] { return ft; });
					break;
				}
			}
			if (ftClamp)
				KeepOnly(l1, ft);
			const bool mulKind = u.kind == FmacKind::Mul || u.kind == FmacKind::Madd || u.kind == FmacKind::Msub;
			if (ftLane >= 0 && !mulKind)
			{
				const int t = TempV();
				armAsm->Dup(V(t).V4S(), V(ft).V4S(), ftLane);
				ft = t;
				ftLane = -1;
			}
			const auto mul = [&](int d) {
				if (ftLane >= 0)
					armAsm->Fmul(V(d).V4S(), V(fs).V4S(), S(ft), ftLane);
				else
					armAsm->Fmul(V(d).V4S(), V(fs).V4S(), V(ft).V4S());
			};
			// The result in d, with mode 2's result clamp when it applies.
			const auto finish = [&](auto emit) {
				if (step && ResultClamp())
				{
					const int t = TempV();
					emit(t);
					const int d = DefV(id);
					armAsm->Fminnm(V(d).V4S(), V(t).V4S(), V(kCMax).V4S());
					armAsm->Fmaxnm(V(d).V4S(), V(d).V4S(), V(kCMin).V4S());
				}
				else
					emit(DefV(id));
			};
			if (u.kind == FmacKind::Madd || u.kind == FmacKind::Msub)
			{
				const u32 l2 = vLock_;
				int acc;
				if (E)
				{
					acc = ClampOperand(SingleSource(in.args, 8), [&] { return VecOperand(in.args, 8); });
					KeepOnly(l2, acc);
				}
				else
					acc = VecOperand(in.args, 8);
				const u32 l3 = vLock_;
				int prod = TempV();
				mul(prod);
				if (E && ResultClamp())
					prod = ClampInto(prod);    // mVUclamp4 of the multiply step
				else if (E)
					prod = SignClampInto(prod); // mode 3: the accumulate step clamps it
				KeepOnly(l3, prod);
				finish([&](int d) {
					if (u.kind == FmacKind::Msub)
						armAsm->Fsub(V(d).V4S(), V(acc).V4S(), V(prod).V4S()); // ACC - product
					else if (u.cls == UClass::FmacB)
						armAsm->Fadd(V(d).V4S(), V(acc).V4S(), V(prod).V4S()); // ACC + product
					else
						armAsm->Fadd(V(d).V4S(), V(prod).V4S(), V(acc).V4S()); // product + ACC
				});
				return;
			}
			switch (u.kind)
			{
				case FmacKind::Add: finish([&](int d) { armAsm->Fadd(V(d).V4S(), V(fs).V4S(), V(ft).V4S()); }); break;
				case FmacKind::Sub: finish([&](int d) { armAsm->Fsub(V(d).V4S(), V(fs).V4S(), V(ft).V4S()); }); break;
				case FmacKind::Mul: finish([&](int d) { mul(d); }); break;
				case FmacKind::Max:
				case FmacKind::Mini:
				{
					// Against VF0 (every word non-negative) the sign-magnitude
					// order is the raw signed order: one SMAX / SMIN, as microVU.
					const bool isMax = u.kind == FmacKind::Max;
					const auto isVf0 = [&](ValueId v) { return v && I(v).op == Op::Const && I(v).type == Type::Vec; };
					const bool vf0 = isVf0(SingleSource(in.args, 0)) ||
									 (u.form == FtForm::Reg ? isVf0(SingleSource(in.args, 4)) : (u.form == FtForm::Bc && isVf0(C(in.args[4]))));
					const int d = DefV(id);
					if (vf0)
					{
						if (isMax)
							armAsm->Smax(V(d).V4S(), V(fs).V4S(), V(ft).V4S());
						else
							armAsm->Smin(V(d).V4S(), V(fs).V4S(), V(ft).V4S());
					}
					else
						MaxMin(fs, ft, isMax, d);
					break;
				}
				default:
				{
					const int d = DefV(id);
					armAsm->Movi(V(d).V4S(), 0);
					break;
				}
			}
		}

		void Lowerer::Opmul(ValueId id)
		{
			// mVU_OPMULA / mVU_OPMSUB: {Fs.y, Fs.z, Fs.x, Fs.w} * {Ft.z, Ft.x, Ft.y, +0},
			// through NEON_MULPS (and NEON_SUBPS): the clampE steps take the
			// rotated multiplicands.
			const Inst& in = I(id);
			const bool E = env_.clampExtra;
			// NEON_MULPS clamps with the op's field mask, and microVU's clamps
			// read a single-lane mask as "lane 0 only" (the single-lane forms
			// rotate the live lane there first; this one does not), so with a
			// single-lane field the multiply step clamps lane 0. OPMSUB's SUB
			// step runs after the rotation to the destination lane, so it
			// clamps that lane.
			const bool lane0Only = Single(in.mask);
			const auto clampMul = [&](int r) {
				const int c = ClampE(r);
				if (!lane0Only)
					return c;
				const int t = TempV();
				armAsm->Mov(V(t).V16B(), V(r).V16B());
				armAsm->Ins(V(t).V4S(), 0, V(c).V4S(), 0);
				return t;
			};
			const auto clampFMul = [&](int r) {
				const int c = ClampInto(r);
				if (!lane0Only)
					return c;
				const int t = TempV();
				armAsm->Mov(V(t).V16B(), V(r).V16B());
				armAsm->Ins(V(t).V4S(), 0, V(c).V4S(), 0);
				return t;
			};
			const int fs = VecOperand(in.args, 0);
			const int ft = VecOperand(in.args, 4);
			int a = TempV(), b = TempV();
			// The w lane of the product is only needed when the field writes it
			// (its flags and its value are masked off otherwise).
			const bool wLive = MaskLane(in.mask, 3);
			// a = Fs.yzxw: rotate the low three lanes
			armAsm->Ext(V(a).V16B(), V(fs).V16B(), V(fs).V16B(), 4);    // y z w x
			armAsm->Ins(V(a).V4S(), 2, V(fs).V4S(), 0);                 // y z x x
			if (wLive)
				armAsm->Ins(V(a).V4S(), 3, V(fs).V4S(), 3);             // y z x w
			// b = Ft.zxy0
			armAsm->Ext(V(b).V16B(), V(ft).V16B(), V(ft).V16B(), 12);   // w x y z
			armAsm->Ins(V(b).V4S(), 0, V(ft).V4S(), 2);                 // z x y z
			if (wLive)
				armAsm->Ins(V(b).V4S(), 3, a64::wzr);                   // z x y 0
			if (E)
			{
				a = clampMul(a);
				b = clampMul(b);
			}
			const auto clampF = [&](int d, int t) {
				armAsm->Fminnm(V(d).V4S(), V(t).V4S(), V(kCMax).V4S());
				armAsm->Fmaxnm(V(d).V4S(), V(d).V4S(), V(kCMin).V4S());
			};
			if (in.op == Op::Opmula)
			{
				if (E && ResultClamp())
				{
					const int t = TempV();
					armAsm->Fmul(V(t).V4S(), V(a).V4S(), V(b).V4S());
					const int c = clampFMul(t);
					const int d = DefV(id);
					armAsm->Mov(V(d).V16B(), V(c).V16B());
				}
				else
				{
					const int d = DefV(id);
					armAsm->Fmul(V(d).V4S(), V(a).V4S(), V(b).V4S());
				}
				return;
			}
			int p = TempV();
			armAsm->Fmul(V(p).V4S(), V(a).V4S(), V(b).V4S());
			if (E && ResultClamp())
				p = clampFMul(p);     // NEON_MULPS's mVUclamp4
			if (E)
				p = ClampE(p);        // the SUB step's operand clamp (NEON_SUBPS takes no preClamped)
			const int acc = E ? ClampOperand(SingleSource(in.args, 8), [&] { return VecOperand(in.args, 8); })
							  : VecOperand(in.args, 8);
			if (E && ResultClamp())
			{
				const int t = TempV();
				armAsm->Fsub(V(t).V4S(), V(acc).V4S(), V(p).V4S());
				clampF(DefV(id), t);
			}
			else
			{
				const int d = DefV(id);
				armAsm->Fsub(V(d).V4S(), V(acc).V4S(), V(p).V4S());
			}
		}

		void Lowerer::Fmac2(ValueId id)
		{
			const Inst& in = I(id);
			const int fs = VecOperand(in.args, 0);
			const UOp op = static_cast<UOp>(in.sub);
			const float* ftoi[4] = {nullptr, mVUglob.FTOI_4, mVUglob.FTOI_12, mVUglob.FTOI_15};
			const float* itof[4] = {nullptr, mVUglob.ITOF_4, mVUglob.ITOF_12, mVUglob.ITOF_15};
			switch (op)
			{
				case UOp::ABS:
				{
					const int d = DefV(id);
					armAsm->Fabs(V(d).V4S(), V(fs).V4S());
					break;
				}
				case UOp::FTOI0: case UOp::FTOI4: case UOp::FTOI12: case UOp::FTOI15:
				{
					// mVU_FTOIx: scale, NaN lanes saturate by sign, Fcvtzs.
					const int k = static_cast<int>(op) - static_cast<int>(UOp::FTOI0);
					int x = fs;
					const int c = TempV();
					if (ftoi[k])
					{
						x = TempV();
						armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(ftoi[k])));
						armAsm->Fmul(V(x).V4S(), V(fs).V4S(), V(c).V4S());
					}
					const int nn = TempV(), sat = TempV();
					armAsm->Fcmeq(V(nn).V4S(), V(x).V4S(), V(x).V4S());
					armAsm->Sshr(V(sat).V4S(), V(x).V4S(), 31);
					armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->Eor(V(sat).V16B(), V(sat).V16B(), V(c).V16B());
					const int d = DefV(id);
					armAsm->Fcvtzs(V(d).V4S(), V(x).V4S());
					armAsm->Bif(V(d).V16B(), V(sat).V16B(), V(nn).V16B());
					break;
				}
				default:
				{
					const int k = static_cast<int>(op) - static_cast<int>(UOp::ITOF0);
					if (itof[k])
					{
						const int c = TempV(), t = TempV();
						armAsm->Scvtf(V(t).V4S(), V(fs).V4S());
						armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(itof[k])));
						const int d = DefV(id);
						armAsm->Fmul(V(d).V4S(), V(t).V4S(), V(c).V4S());
					}
					else
					{
						const int d = DefV(id);
						armAsm->Scvtf(V(d).V4S(), V(fs).V4S());
					}
					break;
				}
			}
		}

		void Lowerer::ClipBits(ValueId id)
		{
			// mVU_CLIP
			const Inst& in = I(id);
			const ArgVec fsArgs = {in.args[0], in.args[1], in.args[2], 0};
			const int fs = VecOperand(fsArgs, 0);
			int lane;
			const int ftw = ScalarV(in.args[3], &lane);
			const int c = TempV(), t2 = TempV(), t1 = TempV(), t0 = TempV();
			armAsm->Dup(V(t1).V4S(), V(ftw).V4S(), I(C(in.args[3])).type == Type::Vec ? 3 : lane);
			armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(&mVUglob.exponent[0])));
			armAsm->And(V(t2).V16B(), V(fs).V16B(), V(c).V16B());
			armAsm->Cmeq(V(t2).V4S(), V(t2).V4S(), 0);
			armAsm->Bic(V(t2).V16B(), V(fs).V16B(), V(t2).V16B());
			armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
			armAsm->And(V(t1).V16B(), V(t1).V16B(), V(c).V16B());
			armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(&mVUglob.signbit[0])));
			armAsm->Eor(V(t0).V16B(), V(t2).V16B(), V(c).V16B());
			armAsm->Cmgt(V(t2).V4S(), V(t2).V4S(), V(t1).V4S());
			armAsm->Cmgt(V(t0).V4S(), V(t0).V4S(), V(t1).V4S());
			armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(&mVUglob.clipWeights[0])));
			armAsm->Uzp1(V(t2).V8H(), V(t2).V8H(), V(t0).V8H());
			armAsm->And(V(t2).V16B(), V(t2).V16B(), V(c).V16B());
			armAsm->Addv(H(t2), V(t2).V8H());
			const int d = DefG(id);
			armAsm->Umov(W(d), V(t2).V8H(), 0);
		}

		void Lowerer::FlagRead(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const u32 imm = static_cast<u32>(in.imm);
			const int src = IntOperand(in.args[0]);
			switch (op)
			{
				case LOp::FCAND:
				{
					const int d = DefG(id);
					if (imm)
					{
						armAsm->Tst(W(src), imm);
						armAsm->Cset(W(d), a64::ne);
					}
					else
						armAsm->Mov(W(d), 0);
					break;
				}
				case LOp::FCEQ:
				{
					const int d = DefG(id);
					armAsm->Eor(W(d), W(src), imm);
					armAsm->Sub(W(d), W(d), 1);
					armAsm->Lsr(W(d), W(d), 31);
					break;
				}
				case LOp::FCOR:
				{
					// (CLIP | imm24) == 0xFFFFFF: every bit the immediate leaves
					// clear is set in CLIP (one BICS, as microVU's mVU_FCOR).
					const u32 need = ~imm & 0xFFFFFFu;
					if (!need)
					{
						const int d = DefG(id);
						armAsm->Mov(W(d), 1);
						break;
					}
					const int shift = __builtin_ctz(need);
					// the strided masks programs use repeat: one register per node
					const ValueId kp = Find(fcorConst_, need >> shift);
					int t;
					if (kp)
						t = UseReg(kp);
					else
					{
						t = TempG();
						armAsm->Mov(W(t), need >> shift);
					}
					armAsm->Bics(a64::wzr, W(t), a64::Operand(W(src), a64::LSR, shift));
					const int d = DefG(id);
					armAsm->Cset(W(d), a64::eq);
					break;
				}
				case LOp::FCGET:
				{
					const int d = DefG(id);
					armAsm->And(W(d), W(src), 0xfff);
					break;
				}
				case LOp::FMAND:
				case LOp::FMEQ:
				case LOp::FMOR:
				{
					const int vi = IntOperand(in.args[1]);
					const int d = DefG(id);
					if (op == LOp::FMAND)
						armAsm->And(W(d), W(vi), W(src));
					else if (op == LOp::FMOR)
						armAsm->Orr(W(d), W(vi), W(src));
					else
					{
						// (the VIs this operand is computed from were zero-extended at
						// the start of the node: NormalizeFmeqInputs)
						armAsm->Eor(W(d), W(vi), W(src));
						armAsm->Sub(W(d), W(d), 1);
						armAsm->Lsr(W(d), W(d), 31);
					}
					break;
				}
				case LOp::FSAND:
				{
					// Only the bits the immediate keeps: the four flag summaries
					// (Z/S from the non-sticky nibbles, ZS/SS from the sticky ones)
					// and the D/I/... bits that the conversion shifts down by 14.
					// Built in temporaries: the result may take src's register.
					static const struct { u32 bit, test; } kSum[] = {{0x01, 0x0f00}, {0x02, 0xf000}, {0x40, 0x000f}, {0x80, 0x00f0}};
					const u32 shifted = imm & 0xf3c;
					int acc = -1, t = -1;
					for (const auto& b : kSum)
					{
						if (!(imm & b.bit))
							continue;
						armAsm->Tst(W(src), b.test);
						if (acc < 0)
						{
							acc = TempG();
							armAsm->Cset(W(acc), a64::ne);
							if (b.bit != 1)
								armAsm->Lsl(W(acc), W(acc), __builtin_ctz(b.bit));
						}
						else
						{
							if (t < 0)
								t = TempG();
							armAsm->Cset(W(t), a64::ne);
							armAsm->Orr(W(acc), W(acc), a64::Operand(W(t), a64::LSL, __builtin_ctz(b.bit)));
						}
					}
					if (shifted)
					{
						if (t < 0)
							t = TempG();
						armAsm->Lsr(W(t), W(src), 14);
						const int d = DefG(id);
						if (acc < 0)
							armAsm->And(W(d), W(t), shifted);
						else
						{
							armAsm->And(W(t), W(t), shifted);
							armAsm->Orr(W(d), W(acc), W(t));
						}
					}
					else
					{
						const int d = DefG(id);
						if (acc < 0)
							armAsm->Mov(W(d), 0);
						else
							armAsm->Mov(W(d), W(acc));
					}
					break;
				}
				default:
				{
					// mVUallocSFLAGc: denormalized status -> architectural form
					//   Z <- any 0x0f00, S <- any 0xf000, ZS <- any 0x000f,
					//   SS <- any 0x00f0, bits 16+ -> bits 2+.
					const int t = TempG(), b = TempG();
					armAsm->Tst(W(src), 0x0f00);
					armAsm->Cset(W(t), a64::ne);
					armAsm->Tst(W(src), 0xf000);
					armAsm->Cset(W(b), a64::ne);
					armAsm->Orr(W(t), W(t), a64::Operand(W(b), a64::LSL, 1));
					armAsm->Tst(W(src), 0x000f);
					armAsm->Cset(W(b), a64::ne);
					armAsm->Orr(W(t), W(t), a64::Operand(W(b), a64::LSL, 6));
					armAsm->Tst(W(src), 0x00f0);
					armAsm->Cset(W(b), a64::ne);
					armAsm->Orr(W(t), W(t), a64::Operand(W(b), a64::LSL, 7));
					armAsm->Lsr(W(b), W(src), 16);
					const int d = DefG(id);
					armAsm->Orr(W(d), W(t), a64::Operand(W(b), a64::LSL, 2));
					if (op == LOp::FSAND)
					{
						if (imm)
							armAsm->And(W(d), W(d), imm);
						else
							armAsm->Mov(W(d), 0);
					}
					else if (op == LOp::FSOR)
					{
						if (imm)
							armAsm->Orr(W(d), W(d), imm);
					}
					else // FSEQ
					{
						if (imm)
							armAsm->Eor(W(d), W(d), imm);
						armAsm->Sub(W(d), W(d), 1);
						armAsm->Lsr(W(d), W(d), 31);
					}
					break;
				}
			}
		}

		void Lowerer::Div(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const LowerInst l = DecodeLower(in.word);
			const bool skipClamp = in.imm & 1;
			// operands into lane 0 of fsR / ftR (temps: the sequences modify them)
			int fsR = -1;
			const auto lane0 = [&](ValueId v, int field) {
				const ValueId c = C(v);
				const int t = TempV();
				if (I(c).type == Type::Vec || (I(c).op == Op::Const && I(c).type == Type::Vec))
					armAsm->Dup(S(t), V(UseReg(v)).V4S(), field);
				else
				{
					int lane;
					const int r = ScalarV(v, &lane);
					armAsm->Dup(S(t), V(r).V4S(), lane);
				}
				return t;
			};
			if (op != LOp::SQRT)
				fsR = lane0(in.args[0], l.fsf);
			const int ftR = lane0(in.args[1], l.ftf);
			// the quotient is built in the result's register
			const int q = DefV(id);
			const int g9 = TempG(), g10 = TempG(), g11 = TempG(), g12 = TempG();
			// Every register is taken before the first branch: allocating
			// later could emit a spill on one path only.
			const int absc = (op == LOp::DIV) ? -1 : TempV();

			a64::Label done, normal;
			const auto quotient = [&](int divisor) {
				// Fdiv, the mode-1 single-lane clamp unless the dividend came from
				// VF0, and the software flush when the FPCR does not flush.
				if (env_.softwareFlush)
					armAsm->Umov(W(g10), V(fsR).V4S(), 0);
				armAsm->Fdiv(S(q), S(fsR), S(divisor));
				if (!skipClamp && (env_.clampOverflow || env_.clampExtra))
				{
					armAsm->Fminnm(S(q), S(q), S(kCMax));
					armAsm->Fmaxnm(S(q), S(q), S(kCMin));
				}
				if (env_.softwareFlush)
				{
					armAsm->Umov(W(g9), V(q).V4S(), 0);
					armAsm->And(W(g11), W(g9), 0x80000000);
					armAsm->Tst(W(g9), kExpMask);
					armAsm->Csel(W(g9), W(g11), W(g9), a64::eq);
					armAsm->Tst(W(g10), kExpMask);
					armAsm->Csel(W(g9), W(g11), W(g9), a64::eq);
					armAsm->Fmov(S(q), W(g9));
				}
			};
			const auto zeroDivisorQ = [&](bool fromBoth) {
				// sign(Fs) [xor sign(Ft)] | the ceiling: maxvals, or at the
				// sign-preserving mode 0x7FFFFFFF (consoleQ)
				armAsm->Umov(W(g9), V(fsR).V4S(), 0);
				if (fromBoth)
				{
					armAsm->Umov(W(g10), V(ftR).V4S(), 0);
					armAsm->Eor(W(g9), W(g9), W(g10));
				}
				if (env_.clampSign)
					armAsm->Orr(W(g9), W(g9), 0x7fffffff);
				else
				{
					armAsm->And(W(g9), W(g9), 0x80000000);
					armAsm->Orr(W(g9), W(g9), 0x7f7fffff);
				}
				armAsm->Fmov(S(q), W(g9));
			};

			switch (op)
			{
				case LOp::DIV:
					// a zero divisor: a zero exponent field (with FZ, one FCMP sees
					// exactly that, as microVU's DIV does)
					if (!env_.softwareFlush)
						armAsm->Fcmp(S(ftR), 0.0);
					else
					{
						armAsm->Umov(W(g9), V(ftR).V4S(), 0);
						armAsm->Tst(W(g9), kExpMask);
					}
					armAsm->B(&normal, a64::ne);
					zeroDivisorQ(true);
					armAsm->B(&done);
					armAsm->Bind(&normal);
					quotient(ftR);
					armAsm->Bind(&done);
					break;
				case LOp::SQRT:
				{
					a64::Label notNeg;
					armAsm->Umov(W(g9), V(ftR).V4S(), 0);
					armAsm->Tbz(W(g9), 31, &notNeg);
					armAsm->Ldr(V(absc), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->And(V(ftR).V16B(), V(ftR).V16B(), V(absc).V16B());
					armAsm->Bind(&notNeg);
					if (env_.softwareFlush)
						armAsm->Umov(W(g10), V(ftR).V4S(), 0);
					if (env_.clampOverflow)
						armAsm->Fminnm(S(ftR), S(ftR), S(kCMax));
					armAsm->Fsqrt(S(q), S(ftR));
					if (env_.softwareFlush)
					{
						armAsm->Umov(W(g9), V(q).V4S(), 0);
						armAsm->Tst(W(g10), kExpMask);
						armAsm->Csel(W(g9), a64::wzr, W(g9), a64::eq);
						armAsm->Fmov(S(q), W(g9));
					}
					break;
				}
				default: // RSQRT
				{
					a64::Label notNeg;
					armAsm->Umov(W(g9), V(ftR).V4S(), 0);
					armAsm->Tbz(W(g9), 31, &notNeg);
					armAsm->Ldr(V(absc), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->And(V(ftR).V16B(), V(ftR).V16B(), V(absc).V16B());
					armAsm->Bind(&notNeg);
					armAsm->Umov(W(g12), V(ftR).V4S(), 0);
					armAsm->Fsqrt(S(ftR), S(ftR));
					armAsm->Tst(W(g12), kExpMask);
					armAsm->B(&normal, a64::ne);
					zeroDivisorQ(false);
					armAsm->B(&done);
					armAsm->Bind(&normal);
					quotient(ftR);
					armAsm->Bind(&done);
					break;
				}
			}
		}

		void Lowerer::DivFlags(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const LowerInst l = DecodeLower(in.word);
			// Relaxation 3 covers the sticky I / D bits too (decision j, 2026-10-14: all the sticky
			// bits of STATUS): with the sticky byte dropped the divide builds only the bits its
			// instance needs. Mutation 3 (test only) drops the instance's own bits instead.
			u32 divI = env_.dropSticky ? (kDivI & kDivCurrent) : kDivI;
			u32 divD = env_.dropSticky ? (kDivD & kDivCurrent) : kDivD;
			if (env_.dropSticky && env_.dropStickyMutation == 3)
				divI = divD = 0;
			// mVU_DIV / mVU_SQRT / mVU_RSQRT's flags, without branches: I where the divisor is negative
			// (SQRT, RSQRT); where it is zero (DIV, RSQRT), I when the dividend is zero too, else D. A
			// dividend from VF0 is a constant (x, y, z: 0.0, w: 1.0), which leaves one select.
			const ValueId fsv = op == LOp::SQRT ? 0 : C(in.args[0]);
			const bool fsConst = fsv && I(fsv).op == Op::Const && I(fsv).type == Type::Vec;
			const bool fsZero = fsConst && l.fsf != 3;
			int fsw = -1;
			if (op != LOp::SQRT && !fsConst)
				fsw = LaneWord(in.args[0], l.fsf);
			const int ftw = LaneWord(in.args[1], l.ftf);
			int zf = -1; // the flags of a zero divisor
			if (op == LOp::DIV || op == LOp::RSQRT)
			{
				const u32 k = fsConst ? (fsZero ? divI : divD) : 0;
				if (fsConst)
				{
					if (k)
					{
						zf = TempG();
						armAsm->Mov(W(zf), k);
					}
				}
				else if (divI | divD)
				{
					zf = TempG();
					const int ti = TempG();
					armAsm->Mov(W(zf), divD);
					armAsm->Mov(W(ti), divI);
					armAsm->Tst(W(fsw), kExpMask);
					armAsm->Csel(W(zf), W(ti), W(zf), a64::eq);
				}
			}
			int nf = -1; // the flags of a negative divisor
			if ((op == LOp::SQRT || op == LOp::RSQRT) && divI)
			{
				nf = TempG();
				armAsm->Mov(W(nf), divI);
			}
			const int f = DefG(id);
			if (op == LOp::SQRT)
			{
				if (nf >= 0)
				{
					armAsm->Tst(W(ftw), 0x80000000u);
					armAsm->Csel(W(f), W(nf), a64::wzr, a64::ne);
				}
				else
					armAsm->Mov(W(f), 0);
				return;
			}
			if (zf >= 0)
			{
				armAsm->Tst(W(ftw), kExpMask);
				armAsm->Csel(W(f), W(zf), a64::wzr, a64::eq);
			}
			else
				armAsm->Mov(W(f), 0);
			if (op == LOp::RSQRT && nf >= 0)
			{
				// (a negative divisor's I with a zero one's flags: -0.0 is both)
				armAsm->Tst(W(ftw), 0x80000000u);
				armAsm->Csel(W(nf), W(nf), a64::wzr, a64::ne);
				armAsm->Orr(W(f), W(f), W(nf));
			}
		}

		void Lowerer::Efu(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const LowerInst l = DecodeLower(in.word);
			// Scalars on lane 0 (NEON_*SS without clamps at mode 1).
			int r[5];
			const int c = TempV();
			const auto mulC = [&](int x, const void* k) {
				armAsm->Ldr(S(c), a64::MemOperand(xGlob, G(k)));
				armAsm->Fmul(S(x), S(x), S(c));
			};
			const auto addC = [&](int x, const void* k) {
				armAsm->Ldr(S(c), a64::MemOperand(xGlob, G(k)));
				armAsm->Fadd(S(x), S(x), S(c));
			};
			const auto subC = [&](int x, const void* k) {
				armAsm->Ldr(S(c), a64::MemOperand(xGlob, G(k)));
				armAsm->Fsub(S(x), S(x), S(c));
			};
			// NEON_MULSS_Series / NEON_ADDSS / NEON_SUBSS / NEON_DIVSS: at clampE
			// each step clamps both operands in place (the EFU's temporaries,
			// as microVU's are) and at mode 2 its result.
			const auto clampIn = [&](int r) {
				if (env_.clampSign)
				{
					armAsm->Smin(V(r).V4S(), V(r).V4S(), V(kCMax).V4S());
					armAsm->Umin(V(r).V4S(), V(r).V4S(), V(kCMin).V4S());
				}
				else
				{
					armAsm->Fminnm(V(r).V4S(), V(r).V4S(), V(kCMax).V4S());
					armAsm->Fmaxnm(V(r).V4S(), V(r).V4S(), V(kCMin).V4S());
				}
			};
			const auto stepClamps = [&](int to, int from) {
				if (!env_.clampExtra)
					return;
				clampIn(from);
				if (to != from)
					clampIn(to);
			};
			const auto resultClamp = [&](int to) {
				if (!ResultClamp())
					return;
				armAsm->Fminnm(V(to).V4S(), V(to).V4S(), V(kCMax).V4S());
				armAsm->Fmaxnm(V(to).V4S(), V(to).V4S(), V(kCMin).V4S());
			};
			const auto mul = [&](int to, int from) { stepClamps(to, from); armAsm->Fmul(S(to), S(to), S(from)); resultClamp(to); };
			const auto add = [&](int to, int from) { stepClamps(to, from); armAsm->Fadd(S(to), S(to), S(from)); resultClamp(to); };
			const auto sub = [&](int to, int from) { stepClamps(to, from); armAsm->Fsub(S(to), S(to), S(from)); resultClamp(to); };
			const auto div = [&](int to, int from) { stepClamps(to, from); armAsm->Fdiv(S(to), S(to), S(from)); resultClamp(to); };
			const auto mov = [&](int dst, int src) { armAsm->Mov(V(dst).V16B(), V(src).V16B()); };
			const auto one = [&](int x) { armAsm->Ldr(S(x), a64::MemOperand(xGlob, G(&mVUglob.one[0]))); };
			// mVU_sumXYZ_arm: (x*x + y*y) + (z*z + 0) into lane 0
			const auto sumXYZ = [&](int dst, int fs) {
				armAsm->Fmul(V(fs).V4S(), V(fs).V4S(), V(fs).V4S());
				armAsm->Ins(V(fs).V4S(), 3, a64::wzr);
				armAsm->Faddp(V(fs).V4S(), V(fs).V4S(), V(fs).V4S());
				armAsm->Faddp(V(fs).V4S(), V(fs).V4S(), V(fs).V4S());
				if (dst != fs)
					armAsm->Ins(V(dst).V4S(), 0, V(fs).V4S(), 0);
			};
			// mVU_EATAN_arm: pq = Fs*c1 + Fs^3*c3 + ... + Fs^15*c15 + pi/4
			const auto eatan = [&](int pq, int fs, int t1, int t2) {
				armAsm->Ins(V(pq).V4S(), 0, V(fs).V4S(), 0);
				mulC(pq, &mVUglob.T1[0]);
				mov(t2, fs);
				const u32* k[7] = {mVUglob.T2, mVUglob.T3, mVUglob.T4, mVUglob.T5, mVUglob.T6, mVUglob.T7, mVUglob.T8};
				for (const u32* kk : k)
				{
					mul(t2, fs);
					mul(t2, fs);
					mov(t1, t2);
					mulC(t1, kk);
					add(pq, t1);
				}
				addC(pq, &mVUglob.Pi4[0]);
			};

			// Operand: the EFU1 forms read lane fsf into lane 0; the EFU2 forms
			// take the whole register. Either way into a temp the series
			// overwrites.
			const bool efu1 = op == LOp::EATAN || op == LOp::EEXP || op == LOp::ERCPR || op == LOp::ERSQRT ||
							  op == LOp::ESIN || op == LOp::ESQRT;
			for (int& x : r)
				x = TempV();
			if (efu1)
			{
				const ValueId v = C(in.args[0]);
				if (I(v).type == Type::Vec || (I(v).op == Op::Const && I(v).type == Type::Vec))
					armAsm->Dup(S(r[0]), V(UseReg(in.args[0])).V4S(), l.fsf);
				else
				{
					int lane;
					const int s = ScalarV(in.args[0], &lane);
					armAsm->Dup(S(r[0]), V(s).V4S(), lane);
				}
			}
			else
				armAsm->Mov(V(r[0]).V16B(), V(VecOperand(in.args, 0)).V16B());

			int res = 1;
			switch (op)
			{
				case LOp::EATAN:
					armAsm->Ins(V(r[1]).V4S(), 0, V(r[0]).V4S(), 0);
					subC(r[0], &mVUglob.one[0]);
					addC(r[1], &mVUglob.one[0]);
					div(r[0], r[1]);
					eatan(r[1], r[0], r[2], r[3]);
					res = 1;
					break;
				case LOp::EATANxy:
				case LOp::EATANxz:
					// t1 = r0 (whole Fs), Fs' = r4
					armAsm->Ins(V(r[4]).V4S(), 0, V(r[0]).V4S(), op == LOp::EATANxy ? 1 : 2);
					armAsm->Ins(V(r[1]).V4S(), 0, V(r[4]).V4S(), 0);
					sub(r[4], r[0]);
					add(r[0], r[1]);
					div(r[4], r[0]);
					eatan(r[1], r[4], r[0], r[3]);
					res = 1;
					break;
				case LOp::EEXP:
					armAsm->Ins(V(r[1]).V4S(), 0, V(r[0]).V4S(), 0);
					mulC(r[1], &mVUglob.E1[0]);
					addC(r[1], &mVUglob.one[0]);
					mov(r[2], r[0]);
					mul(r[2], r[0]);
					mov(r[3], r[2]);
					mulC(r[2], &mVUglob.E2[0]);
					add(r[1], r[2]);
					for (const u32* k : {mVUglob.E3, mVUglob.E4, mVUglob.E5})
					{
						mul(r[3], r[0]);
						mov(r[2], r[3]);
						mulC(r[2], k);
						add(r[1], r[2]);
					}
					mul(r[3], r[0]);
					mulC(r[3], &mVUglob.E6[0]);
					add(r[1], r[3]);
					mul(r[1], r[1]);
					mul(r[1], r[1]);
					one(r[3]);
					div(r[3], r[1]);
					res = 3;
					break;
				case LOp::ELENG:
					sumXYZ(r[1], r[0]);
					armAsm->Fsqrt(S(r[1]), S(r[1]));
					res = 1;
					break;
				case LOp::ERCPR:
					armAsm->Ins(V(r[1]).V4S(), 0, V(r[0]).V4S(), 0);
					one(r[0]);
					div(r[0], r[1]);
					res = 0;
					break;
				case LOp::ERLENG:
					sumXYZ(r[1], r[0]);
					armAsm->Fsqrt(S(r[1]), S(r[1]));
					one(r[0]);
					div(r[0], r[1]);
					res = 0;
					break;
				case LOp::ERSADD:
					sumXYZ(r[1], r[0]);
					one(r[0]);
					div(r[0], r[1]);
					res = 0;
					break;
				case LOp::ERSQRT:
					armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->And(V(r[0]).V16B(), V(r[0]).V16B(), V(c).V16B());
					armAsm->Fsqrt(S(r[1]), S(r[0]));
					one(r[0]);
					div(r[0], r[1]);
					res = 0;
					break;
				case LOp::ESADD:
					sumXYZ(r[0], r[0]);
					res = 0;
					break;
				case LOp::ESIN:
					armAsm->Ins(V(r[1]).V4S(), 0, V(r[0]).V4S(), 0); // pq = X
					mul(r[0], r[0]);                                  // Fs = X^2
					mov(r[2], r[0]);                                  // t1 = X^2
					mul(r[0], r[1]);                                  // Fs = X^3
					mov(r[3], r[0]);                                  // t2 = X^3
					mulC(r[0], &mVUglob.S2[0]);
					add(r[1], r[0]);
					mul(r[3], r[2]);
					mov(r[0], r[3]);
					mulC(r[0], &mVUglob.S3[0]);
					add(r[1], r[0]);
					mul(r[3], r[2]);
					mov(r[0], r[3]);
					mulC(r[0], &mVUglob.S4[0]);
					add(r[1], r[0]);
					mul(r[3], r[2]);
					mulC(r[3], &mVUglob.S5[0]);
					add(r[1], r[3]);
					res = 1;
					break;
				case LOp::ESQRT:
					armAsm->Ldr(V(c), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->And(V(r[0]).V16B(), V(r[0]).V16B(), V(c).V16B());
					armAsm->Fsqrt(S(r[0]), S(r[0]));
					res = 0;
					break;
				case LOp::ESUM:
					armAsm->Rev64(V(r[1]).V4S(), V(r[0]).V4S());
					armAsm->Ext(V(r[1]).V16B(), V(r[1]).V16B(), V(r[1]).V16B(), 8);
					// NEON_ADDPS clamps with the op's field mask: a single-lane
					// field clamps lane 0 only (microVU's single-lane reading).
					if (Single(in.mask) && env_.clampExtra)
					{
						const auto lane0 = [&](int x, bool result) {
							armAsm->Mov(V(c).V16B(), V(x).V16B());
							if (result || !env_.clampSign)
							{
								armAsm->Fminnm(S(c), S(c), S(kCMax));
								armAsm->Fmaxnm(S(c), S(c), S(kCMin));
							}
							else
							{
								armAsm->Smin(V(c).V4S(), V(c).V4S(), V(kCMax).V4S());
								armAsm->Umin(V(c).V4S(), V(c).V4S(), V(kCMin).V4S());
							}
							armAsm->Ins(V(x).V4S(), 0, V(c).V4S(), 0);
						};
						lane0(r[1], false);
						lane0(r[0], false);
						armAsm->Fadd(V(r[0]).V4S(), V(r[0]).V4S(), V(r[1]).V4S());
						if (ResultClamp())
							lane0(r[0], true);
					}
					else
					{
						stepClamps(r[0], r[1]); // NEON_ADDPS
						armAsm->Fadd(V(r[0]).V4S(), V(r[0]).V4S(), V(r[1]).V4S());
						resultClamp(r[0]);
					}
					armAsm->Ins(V(r[1]).V4S(), 0, V(r[0]).V4S(), 1);
					add(r[0], r[1]);
					res = 0;
					break;
				default:
					break;
			}
			const int d = DefV(id);
			armAsm->Mov(V(d).V16B(), V(r[res]).V16B());
		}

		void Lowerer::Kick(ValueId id)
		{
			// mVU_XGKICK_DELAY: the transfer is a C call reading VU memory.
			const Inst& in = I(id);
			const int a = IntOperand(in.args[1]);
			// Spilling stores; every register keeps its contents until the call.
			SpillForCall();
			if (a != 0)
				armAsm->Mov(a64::w0, W(a));
			vLock_ = gLock_ = 0;
			vTemp_ = gTemp_ = 0;
			// Register homes in caller-saved registers ride through the call in
			// their memory homes.
			u32 savedG = 0, savedV = 0;
			for (u32 r = 1; r <= 32; r++)
				if (hm_->clampHome[r] >= 0)
					savedV |= 1u << hm_->clampHome[r];
			if (hm_->vf0Reg >= 0)
				savedV |= 1u << hm_->vf0Reg;
			// the sticky chain's register and accumulators ride through the call too
			if (hm_->saZ >= 0)
				savedV |= 1u << hm_->saZ;
			if (hm_->saS >= 0)
				savedV |= 1u << hm_->saS;
			if (hm_->stkReg >= 0)
				savedG |= 1u << hm_->stkReg;
			for (u32 v = 0; v < var::Count; v++)
			{
				const Home& h = hm_->home[v];
				if (h.kind == HomeKind::VReg && VecHomeOf(v) && (v & 3) == 0)
					savedV |= 1u << h.reg;
				else if (h.kind == HomeKind::GReg && (kGCallerSaved & (1u << h.reg)))
					savedG |= 1u << h.reg;
			}
			const auto homeOfReg = [&](bool vec, int r) -> s32 {
				for (u32 v = 0; v < var::Count; v++)
				{
					const Home& h = hm_->home[v];
					if (vec ? (h.kind == HomeKind::VReg && VecHomeOf(v) && (v & 3) == 0 && h.reg == r)
							: (h.kind == HomeKind::GReg && h.reg == r))
						return static_cast<s32>(v);
				}
				return -1;
			};
			// The register home may hold a value not yet its variable's: the
			// final value computed in place. Save by register through the
			// spill area rather than to the variable's boundary location.
			std::vector<std::pair<int, int>> sv, sg;
			for (u32 m = savedV; m; m &= m - 1)
			{
				const int r = Ctz(m);
				(void)homeOfReg;
				const int slot = AllocSlot();
				SlotStore(true, r, slot);
				sv.push_back({r, slot});
			}
			for (u32 m = savedG; m; m &= m - 1)
			{
				const int r = Ctz(m);
				const int slot = AllocSlot();
				SlotStore(false, r, slot);
				sg.push_back({r, slot});
			}
			// microVU's status instances (w20-w23) and pinned bases are
			// callee-saved; q28 is not.
			armAsm->Sub(a64::sp, a64::sp, 16);
			armAsm->Str(Q(kPQ), a64::MemOperand(a64::sp));
			armEmitCall(env_.xgkickFn);
			armAsm->Ldr(Q(kPQ), a64::MemOperand(a64::sp));
			armAsm->Add(a64::sp, a64::sp, 16);
			if (env_.clampOverflow)
				armAsm->Ldp(Q(kCMin), Q(kCMax), a64::MemOperand(xGlob, G(&mVUglob.minvals[0])));
			for (const auto& p : sv)
			{
				SlotLoad(true, p.first, p.second);
				FreeSlot(p.second);
			}
			for (const auto& p : sg)
			{
				SlotLoad(false, p.first, p.second);
				FreeSlot(p.second);
			}
		}

		void Lowerer::FillWMemLanes()
		{
			if (!hm_->wMem)
				return;
			// the w-lane incoming values of the wMem homes (index 1..32), then which are read
			ValueId l3[33] = {};
			bool any = false;
			for (size_t v = 1; v < f_.insts.size(); v++)
			{
				const Inst& in = f_.insts[v];
				if (in.op != Op::LiveIn || (in.var & 3) != 3)
					continue;
				const u32 r = VecHomeOf(in.var);
				if (r && hm_->WMem(r) && hm_->home[VecVar(r)].kind == HomeKind::VReg && !l3[r])
				{
					l3[r] = static_cast<ValueId>(v);
					any = true;
				}
			}
			if (!any)
				return;
			u64 read = 0;
			for (ValueId v : order_)
			{
				if (!emit_[v])
					continue;
				ForArgs(v, I(v), [&](size_t k, ValueId a) {
					const Inst& x = I(a);
					if (x.op != Op::LiveIn || (x.var & 3) != 3)
						return;
					const u32 r = VecHomeOf(x.var);
					if (r && l3[r] == a && !(v == leave_ && k == x.var))
						read |= u64(1) << r;
				});
			}
			for (u32 r = 1; r <= 32; r++)
			{
				if (!((read >> r) & 1))
					continue;
				const u32 v0 = VecVar(r);
				const MemLoc m = BoundaryLoc(v0, env_);
				armAsm->Ldr(a64::w16, a64::MemOperand(xState, m.off + 12));
				armAsm->Ins(V(hm_->home[v0].reg).V4S(), 3, a64::w16);
			}
		}

		void Lowerer::NormalizeFmeqInputs()
		{
			if (mode_ != LowerMode::Region)
				return;
			std::vector<ValueId>& work = work_;
			work.clear();
			for (ValueId v : order_)
			{
				const Inst& in = I(v);
				if (emit_[v] && canon_[v] == v && in.op == Op::FlagRead && static_cast<LOp>(in.sub) == LOp::FMEQ && in.args.size() > 1)
					work.push_back(C(in.args[1]));
			}
			u32 done = 0; // VI variables already zero-extended
			std::vector<ValueId> seen;
			while (!work.empty())
			{
				const ValueId c = work.back();
				work.pop_back();
				if (!c || std::find(seen.begin(), seen.end(), c) != seen.end())
					continue;
				seen.push_back(c);
				if (folded_[c])
					continue;
				const Inst& in = I(c);
				if (WideViLiveIn(c))
				{
					const u32 bit = 1u << (in.var - var::VI(0));
					if (!(done & bit) && reg_[c] >= 0)
						armAsm->Uxth(W(reg_[c]), W(reg_[c]));
					done |= bit;
					continue;
				}
				if (in.op == Op::IAlu || in.op == Op::IncVI)
				{
					for (ValueId a : in.args)
						if (a)
							work.push_back(C(a));
				}
				else if (in.op == Op::FlagRead && in.args.size() > 1)
				{
					const LOp op = static_cast<LOp>(in.sub);
					if (op == LOp::FMAND || op == LOp::FMOR || op == LOp::FMEQ)
						work.push_back(C(in.args[1]));
				}
			}
		}

		bool Lowerer::WideViLiveIn(ValueId c) const
		{
			if (mode_ != LowerMode::Region || (c < folded_.size() && folded_[c]))
				return false;
			const Inst& in = I(c);
			return in.op == Op::LiveIn && in.var >= var::VI(1) && in.var <= var::VI(15) && hm_->home[in.var].kind == HomeKind::GReg;
		}

		// The end of the node: every variable's final value to its home.
		void Lowerer::Leave(ValueId id)
		{
			(void)id;
			const Inst& in = LeaveInst();
			// A final value that is the variable's own incoming value (directly,
			// or through a copy aliased to it) needs no write.
			const auto ownLiveIn = [&](u32 v, ValueId val) {
				const Inst& x = I(C(val));
				if (x.op != Op::LiveIn)
					return false;
				return VecHomeOf(v) ? (x.var == v || (C(val) != val && VecHomeOf(x.var) == VecHomeOf(v))) : x.var == v;
			};
			const auto homeKind = [&](u32 v) -> HomeKind {
				if (IsStatus(v) || IsPQ(v))
					return HomeKind::GReg; // fixed registers (handled apart)
				const u32 vr = VecHomeOf(v);
				return hm_->home[vr ? VecVar(vr) : v].kind;
			};

			// The branch condition / jump target, first, while its register is
			// certainly valid.
			const ValueId cond = in.args[var::Count];
			const ValueId jump = in.args[var::Count + 1];
			if (mode_ == LowerMode::Region)
			{
				// The flags for the region's conditional branch: the 16-bit value
				// against 0, as microVU's sign-extended compare. N is bit 15, Z is
				// the low half all clear, V and C are clear. Nothing the node end
				// emits after this sets flags.
				if (cond)
					armAsm->Cmn(a64::wzr, a64::Operand(W(IntOperand(cond)), a64::LSL, 16));
				else if (jump)
				{
					// JR/JALR: the target byte address, as microVU stores it in mVU.branch
					armAsm->Lsl(W(kCondReg), W(IntOperand(jump)), 3);
					armAsm->And(W(kCondReg), W(kCondReg), 0x4000 - 8);
				}
			}
			else
			{
				if (cond)
					armAsm->Str(W(IntOperand(cond)), a64::MemOperand(xFlag, env_.offBranch));
				if (jump)
				{
					const int t = TempG();
					armAsm->Lsl(W(t), W(IntOperand(jump)), 3);
					armAsm->And(W(t), W(t), 0x4000 - 8);
					armAsm->Str(W(t), a64::MemOperand(xFlag, env_.offBranch));
				}
			}
			ReleaseLocks();

			// The status chain's last word becomes the next node's incoming word.
			if (acc_ && finalBase_)
			{
				armAsm->Mov(W(hm_->stkReg), W(UseReg(finalBase_)));
				ReleaseLocks();
			}

			// Which memory homes get written.
			u8 memWritten[var::Count] = {};
			for (u32 v : leaveKs_)
			{
				if (v >= var::Count)
					break;
				const ValueId a = in.args[v];
				if (!a || ownLiveIn(v, a) || v == var::MEM || IsStatus(v) || IsPQ(v))
					continue;
				if (homeKind(v) == HomeKind::Mem)
				{
					const u32 vr = VecHomeOf(v);
					memWritten[vr ? VecVar(vr) : v] = 1;
				}
			}
			// An incoming value read through another variable is captured
			// while its memory home still holds it (the VI backup slot carries
			// the old value of the VI the delay slot overwrites), and so is a
			// Q/P lane read through another lane.
			for (u32 v : leaveKs_)
			{
				if (v >= var::Count)
					break;
				const ValueId a = in.args[v];
				if (!a || ownLiveIn(v, a) || v == var::MEM)
					continue;
				const ValueId c = C(a);
				const Inst& x = I(c);
				if (x.op != Op::LiveIn)
					continue;
				const u32 xv = VecHomeOf(x.var) ? VecVar(VecHomeOf(x.var)) : x.var;
				const bool fromWrittenMem = remat_[c] == Remat::Home && memWritten[xv];
				const bool fromPQ = remat_[c] == Remat::PQ;
				if ((fromWrittenMem || fromPQ) && reg_[c] < 0 && slot_[c] < 0)
				{
					noDrop_[c] = 1;
					UseReg(c);
					ReleaseLocks(); // bound is enough; a later eviction spills it
				}
				else if (fromWrittenMem || fromPQ)
					noDrop_[c] = 1;
			}
			ReleaseLocks();

			// Memory homes.
			for (u32 r = 1; r <= 32; r++)
			{
				const u32 v0 = VecVar(r);
				if (hm_->home[v0].kind != HomeKind::Mem)
					continue;
				const MemLoc m = BoundaryLoc(v0, env_);
				ValueId lanes[4];
				int dirty = 0;
				for (int l = 0; l < 4; l++)
				{
					const u32 v = v0 + l;
					lanes[l] = in.args[v];
					if (lanes[l] && !ownLiveIn(v, lanes[l]) && !early_[v])
						dirty |= 1 << l;
					else
						lanes[l] = 0;
				}
				if (!dirty)
					continue;
				// Contiguous groups of lanes from one value: Q / D / S stores.
				int l = 0;
				while (l < 4)
				{
					if (!(dirty & (1 << l)))
					{
						l++;
						continue;
					}
					const ValueId src = C(lanes[l]);
					int e = l + 1;
					while (e < 4 && (dirty & (1 << e)) && C(lanes[e]) == src)
						e++;
					const int n = e - l;
					const int sr = UseReg(src);
					const s32 off = m.off + l * 4;
					if (n == 4)
						armAsm->Str(Q(sr), a64::MemOperand(xState, off));
					else if (n >= 2 && (l == 0 || l == 2))
					{
						if (l == 0)
							armAsm->Str(D(sr), a64::MemOperand(xState, off));
						else
						{
							const int t = TempV();
							armAsm->Dup(D(t), V(sr).V2D(), 1);
							armAsm->Str(D(t), a64::MemOperand(xState, off));
						}
						e = l + 2;
					}
					else
					{
						if (l == 0)
							armAsm->Str(S(sr), a64::MemOperand(xState, off));
						else
						{
							const int t = TempV();
							armAsm->Dup(S(t), V(sr).V4S(), l);
							armAsm->Str(S(t), a64::MemOperand(xState, off));
						}
						e = l + 1;
					}
					ReleaseLocks();
					l = e;
				}
			}
			for (u32 v : leaveKs_)
			{
				if (v >= var::Count)
					break;
				if (VecHomeOf(v) || IsStatus(v) || IsPQ(v) || v == var::MEM || v < 4)
					continue;
				if (hm_->home[v].kind != HomeKind::Mem)
					continue;
				const ValueId a = in.args[v];
				if (!a || ownLiveIn(v, a) || early_[v])
					continue;
				const MemLoc m = BoundaryLoc(v, env_);
				if (!m.base)
					continue;
				const int r = IntOperand(a);
				if (TruncFor(v, C(a)))
					armAsm->Uxth(W(r), W(r)); // (the VI's own home: its low half is unchanged)
				if (m.size == 2)
					armAsm->Strh(W(r), a64::MemOperand(*m.base, m.off));
				else
					armAsm->Str(W(r), a64::MemOperand(*m.base, m.off));
				ReleaseLocks();
			}

			// Register homes: full moves as one parallel move (the status ring
			// renames, the VI backup of a register-homed VI), then lane inserts.
			struct Move
			{
				bool dstV;
				int dst;
				bool srcV;
				int src;     // register, or -1 when the source must be produced
				ValueId val; // when src < 0
				bool trunc16;
			};
			// (buffers kept per thread: a region lowers hundreds of node ends)
			static thread_local std::vector<Move> moves;
			static thread_local std::vector<std::pair<int, ValueId>> statusLike;
			moves.clear();
			statusLike.clear();
			const auto addG = [&](u32 v, int dst) {
				const ValueId a = in.args[v];
				if (!a)
					return;
				const ValueId c = C(a);
				if (reg_[c] == dst && cls_[c] == Cls::G)
					return; // computed in place
				if (ownLiveIn(v, a))
					return;
				Move mv;
				mv.dstV = false;
				mv.dst = dst;
				mv.trunc16 = TruncFor(v, c);
				if (reg_[c] >= 0)
				{
					mv.srcV = cls_[c] == Cls::V;
					mv.src = reg_[c];
					mv.val = 0;
				}
				else
				{
					mv.srcV = false;
					mv.src = -1;
					mv.val = c;
				}
				moves.push_back(mv);
			};
			for (u32 k = 0; k < 4; k++)
				addG(var::S(k), 20 + static_cast<int>(k));
			for (u32 v : leaveKs_)
				if (v < var::Count && GHomeable(v) && hm_->home[v].kind == HomeKind::GReg)
					addG(v, hm_->home[v].reg);
			for (u32 r = 1; r <= 32; r++)
			{
				const u32 v0 = VecVar(r);
				const Home& h = hm_->home[v0];
				if (h.kind != HomeKind::VReg)
					continue;
				const ValueId c = WholeFinal(v0);
				if (!c)
					continue;
				if (reg_[c] == h.reg && cls_[c] == Cls::V)
					continue;
				if (reg_[c] >= 0 && cls_[c] == Cls::V)
					moves.push_back({true, h.reg, true, reg_[c], 0, false});
				else
					moves.push_back({true, h.reg, false, -1, c, false});
			}
			// Resolve: emit a move whose destination no pending move reads;
			// break a cycle through a temporary.
			const auto reads = [&](bool v, int r, size_t except) {
				for (size_t k = 0; k < moves.size(); k++)
					if (k != except && moves[k].src == r && moves[k].srcV == v && !(moves[k].dst == r && moves[k].dstV == v))
						return true;
				return false;
			};
			const auto emitMove = [&](const Move& mv) {
				if (mv.src < 0)
				{
					// produce straight into the destination
					const ValueId c = mv.val;
					if (mv.dstV)
					{
						if (cls_[c] == Cls::V)
						{
							const int r = UseReg(c);
							armAsm->Mov(V(mv.dst).V16B(), V(r).V16B());
						}
						else
							armAsm->Fmov(S(mv.dst), W(UseReg(c)));
					}
					else
					{
						if (cls_[c] == Cls::G)
						{
							const int r = UseReg(c);
							if (mv.trunc16)
								armAsm->Uxth(W(mv.dst), W(r));
							else
								armAsm->Mov(W(mv.dst), W(r));
						}
						else
						{
							const int r = IntOperand(c);
							armAsm->Mov(W(mv.dst), W(r));
						}
					}
					ReleaseLocks();
					return;
				}
				if (mv.dstV)
					armAsm->Mov(V(mv.dst).V16B(), V(mv.src).V16B());
				else if (mv.srcV)
					armAsm->Fmov(W(mv.dst), S(mv.src));
				else if (mv.trunc16)
					armAsm->Uxth(W(mv.dst), W(mv.src));
				else
					armAsm->Mov(W(mv.dst), W(mv.src));
			};
			// Destinations are home registers; nothing else may occupy them.
			// Sources still in a pool register stay valid throughout (pool
			// registers are never destinations), except through TempV/G and
			// UseReg, which only take free pool registers... so produce-
			// moves (src < 0) go last.
			static thread_local std::vector<Move> late;
			late.clear();
			for (auto it = moves.begin(); it != moves.end();)
			{
				if (it->src < 0)
				{
					late.push_back(*it);
					it = moves.erase(it);
				}
				else
					++it;
			}
			while (!moves.empty())
			{
				bool progress = false;
				for (size_t k = 0; k < moves.size(); k++)
				{
					if (reads(moves[k].dstV, moves[k].dst, k))
						continue;
					emitMove(moves[k]);
					moves.erase(moves.begin() + static_cast<long>(k));
					progress = true;
					break;
				}
				if (progress)
					continue;
				// cycle: park one source
				Move& mv = moves[0];
				const int t = mv.srcV ? TempV() : TempG();
				if (mv.srcV)
					armAsm->Mov(V(t).V16B(), V(mv.src).V16B());
				else
					armAsm->Mov(W(t), W(mv.src));
				const int old = mv.src;
				const bool oldV = mv.srcV;
				for (auto& m2 : moves)
					if (m2.src == old && m2.srcV == oldV)
						m2.src = t;
			}
			ReleaseLocks();
			// Homes taken over by the moves no longer hold anything the produce
			// moves could read; values in them are dead now.
			for (const Move& mv : late)
				emitMove(mv);

			// Partial vector homes: insert the changed lanes.
			for (u32 r = 1; r <= 32; r++)
			{
				const u32 v0 = VecVar(r);
				const Home& h = hm_->home[v0];
				if (h.kind != HomeKind::VReg)
					continue;
				if (WholeFinal(v0))
					continue;
				// Every lane rewritten (or dead): the source of most lanes goes in
				// whole, the others are inserted over it.
				ValueId major = 0;
				{
					ValueId srcs[4] = {};
					bool all = true;
					for (u32 k = 0; k < 4 && all; k++)
					{
						const ValueId a = in.args[v0 + k];
						if (LaneFree(r, k))
							continue;
						if (!a || ownLiveIn(v0 + k, a))
							all = false;
						else
							srcs[k] = C(a);
					}
					if (all)
					{
						int best = 0;
						for (u32 k = 0; k < 4; k++)
						{
							if (!srcs[k])
								continue;
							int n = 0;
							for (u32 j = 0; j < 4; j++)
								n += srcs[j] == srcs[k];
							if (n > best)
							{
								best = n;
								major = srcs[k];
							}
						}
						if (best < 2 || reg_[major] == h.reg)
							major = 0;
						// nothing still to be read may live in the home register
						for (u32 v = 0; major && v < var::Count; v++)
						{
							const ValueId a = in.args[v];
							if (a && reg_[C(a)] == h.reg && cls_[C(a)] == Cls::V && VecHomeOf(v) != r)
								major = 0;
						}
						for (u32 k = 0; major && k < 4; k++)
						{
							const ValueId a = in.args[var::QL0 + k];
							if (a && reg_[C(a)] == h.reg && cls_[C(a)] == Cls::V)
								major = 0;
						}
					}
					if (major)
					{
						const int sr = UseReg(major);
						armAsm->Mov(V(h.reg).V16B(), V(sr).V16B());
						ReleaseLocks();
					}
				}
				int l = 0;
				while (l < 4)
				{
					const ValueId a = in.args[v0 + l];
					if (!a || ownLiveIn(v0 + l, a) || (major && C(a) == major) ||
						(reg_[C(a)] == h.reg && cls_[C(a)] == Cls::V))
					{
						l++;
						continue;
					}
					const ValueId src = C(a);
					const int sr = UseReg(src);
					const bool pair = (l == 0 || l == 2) && l + 1 < 4 && in.args[v0 + l + 1] &&
									  !ownLiveIn(v0 + l + 1, in.args[v0 + l + 1]) && C(in.args[v0 + l + 1]) == src;
					if (pair)
					{
						armAsm->Ins(V(h.reg).V2D(), l / 2, V(sr).V2D(), l / 2);
						l += 2;
					}
					else
					{
						armAsm->Ins(V(h.reg).V4S(), l, V(sr).V4S(), l);
						l++;
					}
					ReleaseLocks();
				}
			}

			// Q/P lanes of q28.
			for (u32 k = 0; k < 4; k++)
			{
				const u32 v = var::QL0 + k;
				const ValueId a = in.args[v];
				if (!a || ownLiveIn(v, a))
					continue;
				const ValueId c = C(a);
				if (cls_[c] == Cls::V)
				{
					int lane;
					const int s = ScalarV(c, &lane);
					armAsm->Ins(V(kPQ).V4S(), static_cast<int>(k), V(s).V4S(), lane);
				}
				else
					armAsm->Ins(V(kPQ).V4S(), static_cast<int>(k), W(UseReg(c)));
				ReleaseLocks();
			}
		}

		void Lowerer::One(ValueId id)
		{
			const Inst& in = I(id);
			if (folded_[id] || canon_[id] != id)
				return; // a constant, or the same value as an earlier one
			switch (in.op)
			{
				case Op::Const:
				case Op::IConst:
				case Op::LiveIn:
					break;
				case Op::Fmac: Fmac(id); break;
				case Op::Opmula:
				case Op::Opmsub: Opmul(id); break;
				case Op::Fmac2: Fmac2(id); break;
				case Op::ClipBits: ClipBits(id); break;
				case Op::MacFlags:
				{
					PackFlags(C(in.args[0]), in.mask, true, -1, id);
					break;
				}
				case Op::StatusUpd:
				{
					if (acc_)
					{
						ChainStatusUpd(id);
						break;
					}
					const bool nonSticky = in.imm & 1;
					// The packs of the updates folded into this one (each lands in
					// the sticky byte; their non-sticky copies are overwritten).
					// A pack's vector registers are free again once it is done.
					const auto ch = statusChain_.find(id);
					int acc = -1, t = -1;
					const u32 vLocked = vLock_;
					if (ch != statusChain_.end())
						for (ValueId m : ch->second)
						{
							const Inst& mi = I(m);
							if (acc < 0)
							{
								acc = TempG();
								PackFlags(C(mi.args[1]), mi.mask, (mi.imm & 2) != 0, acc);
								vLock_ = vLocked;
								continue;
							}
							if (t < 0)
								t = TempG();
							PackFlags(C(mi.args[1]), mi.mask, (mi.imm & 2) != 0, t);
							vLock_ = vLocked;
							armAsm->Orr(W(acc), W(acc), W(t));
						}
					const int p = StatusPackReg(id, t >= 0 ? t : -1);
					const int prev = IntOperand(ch != statusChain_.end() ? StatusBase(id) : in.args[0]);
					const int d = DefG(id);
					if (env_.dropSticky)
					{
						pxAssertRel(nonSticky && acc < 0, "vu-aot: a sticky-only STATUS update in a build that drops the sticky byte");
						armAsm->And(W(d), W(prev), env_.dropStickyMutation == 1 ? 0u : 0xfffc0000u);
						if (env_.dropStickyMutation != 2)
							armAsm->Orr(W(d), W(d), a64::Operand(W(p), a64::LSL, 8));
					}
					else if (nonSticky)
					{
						armAsm->And(W(d), W(prev), 0xfffc00ffu);
						if (acc >= 0)
							armAsm->Orr(W(d), W(d), W(acc));
						armAsm->Orr(W(d), W(d), W(p));
						armAsm->Orr(W(d), W(d), a64::Operand(W(p), a64::LSL, 8));
					}
					else
						armAsm->Orr(W(d), W(prev), W(p));
					break;
				}
				case Op::StatusDiv:
				{
					if (acc_)
					{
						ChainStatusDiv(id);
						break;
					}
					const int prev = IntOperand(in.args[0]);
					const int df = IntOperand(in.args[1]);
					const int d = DefG(id);
					armAsm->Bic(W(d), W(prev), 0x000C0000u);
					armAsm->Orr(W(d), W(d), W(df));
					break;
				}
				case Op::StatusSet:
				{
					if (acc_)
					{
						ChainStatusSet(id);
						break;
					}
					u32 imm = 0;
					const u32 i12 = static_cast<u32>(in.imm);
					if (i12 & 0x0040) imm |= 0x000000f;
					if (i12 & 0x0080) imm |= 0x00000f0;
					if (i12 & 0x0100) imm |= 0x0400000;
					if (i12 & 0x0200) imm |= 0x0800000;
					if (i12 & 0x0400) imm |= 0x1000000;
					if (i12 & 0x0800) imm |= 0x2000000;
					const int prev = IntOperand(in.args[0]);
					const int d = DefG(id);
					armAsm->And(W(d), W(prev), 0xfff00u);
					if (imm)
						armAsm->Orr(W(d), W(d), imm);
					break;
				}
				case Op::ClipUpd:
				{
					const int prev = IntOperand(in.args[0]);
					const int bits = IntOperand(in.args[1]);
					const int d = DefG(id);
					armAsm->Lsl(W(d), W(prev), 6);
					armAsm->And(W(d), W(d), 0xffffff);
					armAsm->Orr(W(d), W(d), W(bits));
					break;
				}
				case Op::ClipSet:
				{
					const int d = DefG(id);
					armAsm->Mov(W(d), static_cast<u32>(in.imm));
					break;
				}
				case Op::FlagRead: FlagRead(id); break;
				case Op::IAlu:
				{
					const LOp op = static_cast<LOp>(in.sub);
					const int a = IntOperand(in.args[0]);
					switch (op)
					{
						case LOp::IADD: case LOp::ISUB: case LOp::IAND: case LOp::IOR:
						{
							const int b = IntOperand(in.args[1]);
							const int d = DefG(id, true);
							if (op == LOp::IADD) armAsm->Add(W(d), W(a), W(b));
							else if (op == LOp::ISUB) armAsm->Sub(W(d), W(a), W(b));
							else if (op == LOp::IAND) armAsm->And(W(d), W(a), W(b));
							else armAsm->Orr(W(d), W(a), W(b));
							break;
						}
						case LOp::ISUBIU:
						{
							const int d = DefG(id, true);
							armAsm->Sub(W(d), W(a), static_cast<u32>(in.imm));
							break;
						}
						default: // IADDI / IADDIU
						{
							const int d = DefG(id, true);
							armAsm->Add(W(d), W(a), static_cast<u32>(in.imm));
							break;
						}
					}
					break;
				}
				case Op::IncVI:
				{
					const int a = IntOperand(in.args[0]);
					const int d = DefG(id, true);
					if (in.imm > 0)
						armAsm->Add(W(d), W(a), 1);
					else
						armAsm->Sub(W(d), W(a), 1);
					break;
				}
				case Op::Mtir:
				{
					const LowerInst l = DecodeLower(in.word);
					const ValueId c = C(in.args[0]);
					const int src = UseReg(in.args[0]);
					const int d = DefG(id);
					if (cls_[c] == Cls::V)
						armAsm->Umov(W(d), V(src).V4S(), (I(c).type == Type::Vec || I(c).op == Op::Const) ? l.fsf : 0);
					else
						armAsm->Mov(W(d), W(src));
					break;
				}
				case Op::Xtop:
				{
					const int t = TempG();
					armMoveAddressToReg(X(t), static_cast<LOp>(in.sub) == LOp::XTOP ? env_.vifTop : env_.vifItop);
					const int d = DefG(id);
					armAsm->Ldrh(W(d), a64::MemOperand(X(t)));
					if (static_cast<LOp>(in.sub) == LOp::XITOP)
						armAsm->And(W(d), W(d), 0x3ff);
					break;
				}
				case Op::LoadQ:
				{
					int base, idx;
					MemAddr(in.args[1], in.imm, base, idx);
					const int d = DefV(id);
					armAsm->Ldr(Q(d), MemAt(base, idx));
					break;
				}
				case Op::LoadI:
				{
					int base, idx, ptr;
					s32 off;
					MemAddr(in.args[1], in.imm, base, idx);
					MemPtr(base, idx, ptr, off);
					const int d = DefG(id);
					armAsm->Ldrh(W(d), a64::MemOperand(X(ptr), off + static_cast<s32>(VuIlwLaneOffset(in.word))));
					break;
				}
				case Op::StoreQ:
				{
					const int src = VecOperand(in.args, 2);
					int base, idx;
					MemAddr(in.args[1], in.imm, base, idx);
					if (in.mask == 0xf)
						armAsm->Str(Q(src), MemAt(base, idx));
					else
					{
						int pr;
						s32 po;
						MemPtr(base, idx, pr, po);
						// contiguous lane groups: D / S stores
						int l = 0;
						while (l < 4)
						{
							if (!MaskLane(in.mask, l))
							{
								l++;
								continue;
							}
							if ((l == 0 || l == 2) && MaskLane(in.mask, l + 1))
							{
								if (l == 0)
									armAsm->Str(D(src), a64::MemOperand(X(pr), po));
								else
								{
									const int t = TempV();
									armAsm->Dup(D(t), V(src).V2D(), 1);
									armAsm->Str(D(t), a64::MemOperand(X(pr), po + 8));
								}
								l += 2;
							}
							else
							{
								if (l == 0)
									armAsm->Str(S(src), a64::MemOperand(X(pr), po));
								else
								{
									const int t = TempV();
									armAsm->Dup(S(t), V(src).V4S(), l);
									armAsm->Str(S(t), a64::MemOperand(X(pr), po + l * 4));
								}
								l++;
							}
						}
					}
					break;
				}
				case Op::StoreI:
				{
					const int val = IntOperand(in.args[2]);
					int base, idx, pr;
					s32 po;
					MemAddr(in.args[1], in.imm, base, idx);
					MemPtr(base, idx, pr, po);
					const int t = TempG();
					armAsm->Uxth(W(t), W(val));
					for (int l = 0; l < 4; l++)
						if (MaskLane(in.mask, l))
							armAsm->Str(W(t), a64::MemOperand(X(pr), po + l * 4));
					break;
				}
				case Op::Move:
				{
					// A copy: the result takes over the source's register when
					// the source is a computed value with no later use.
					{
						ValueId only = 0;
						bool single = true;
						for (int l = 0; l < 4; l++)
						{
							const ValueId a = in.args[l];
							if (!a)
								continue;
							if (only && C(a) != C(only))
								single = false;
							if (!only)
								only = a;
						}
						const ValueId sc = C(only);
						if (only && single && cls_[sc] == Cls::V && reg_[sc] >= 0 && remat_[sc] == Remat::None &&
							!homeReg_[sc] && !LiveAfter(sc) && !finalOf_[C(id)])
						{
							const int r = reg_[sc];
							Unbind(sc);
							Bind(C(id), r);
							vLock_ |= 1u << r;
							break;
						}
					}
					const int src = VecOperand(in.args, 0);
					const int d = DefV(id);
					armAsm->Mov(V(d).V16B(), V(src).V16B());
					break;
				}
				case Op::Mr32:
				{
					const int src = VecOperand(in.args, 0);
					const int d = DefV(id);
					armAsm->Ext(V(d).V16B(), V(src).V16B(), V(src).V16B(), 4);
					break;
				}
				case Op::Mfir:
				{
					const int a = IntOperand(in.args[0]);
					const int t = TempG();
					armAsm->Sxth(W(t), W(a));
					const int d = DefV(id);
					armAsm->Dup(V(d).V4S(), W(t));
					break;
				}
				case Op::Mfp:
				{
					int lane;
					const int s = ScalarV(in.args[0], &lane);
					const int d = DefV(id);
					armAsm->Dup(V(d).V4S(), V(s).V4S(), lane);
					break;
				}
				case Op::Div: Div(id); break;
				case Op::DivFlags: DivFlags(id); break;
				case Op::Efu: Efu(id); break;
				case Op::RUpd:
				{
					const LOp op = static_cast<LOp>(in.sub);
					const LowerInst l = DecodeLower(in.word);
					if (op == LOp::RNEXT)
					{
						const int r = IntOperand(in.args[0]);
						const int t = TempG();
						const int d = DefG(id);
						armAsm->Eor(W(t), W(r), a64::Operand(W(r), a64::LSR, 18));
						armAsm->Lsl(W(d), W(r), 1);
						armAsm->Bfxil(W(d), W(t), 4, 1);
						armAsm->And(W(d), W(d), 0x007fffff);
						armAsm->Orr(W(d), W(d), 0x3f800000);
					}
					else
					{
						const int w = LaneWord(in.args[1], l.fsf);
						const int t = TempG();
						armAsm->And(W(t), W(w), 0x007fffff);
						if (op == LOp::RINIT)
						{
							const int d = DefG(id);
							armAsm->Orr(W(d), W(t), 0x3f800000);
						}
						else
						{
							const int r = IntOperand(in.args[0]);
							const int d = DefG(id);
							armAsm->Eor(W(d), W(r), W(t));
						}
					}
					break;
				}
				case Op::RGet:
				{
					const int r = IntOperand(in.args[0]);
					const int d = DefV(id);
					armAsm->Dup(V(d).V4S(), W(r));
					break;
				}
				case Op::AccClamp:
				{
					// mVU_FMACc's clamp step on its read-only ACC mapping.
					const int c = ClampOperand(SingleSource(in.args, 0), [&] { return VecOperand(in.args, 0); });
					const int d = DefV(id);
					armAsm->Mov(V(d).V16B(), V(c).V16B());
					break;
				}
				case Op::Kick: Kick(id); break;
				case Op::Cond:
				{
					const int a = IntOperand(in.args[0]);
					if (in.args.size() > 1 && in.args[1])
					{
						const int b = IntOperand(in.args[1]);
						const int d = DefG(id);
						armAsm->Eor(W(d), W(a), W(b));
					}
					else
					{
						const int d = DefG(id);
						armAsm->Mov(W(d), W(a));
					}
					break;
				}
				case Op::Leave: Leave(id); break;
				default:
					break;
			}
		}

		//------------------------------------------------------------------
		// Setup
		//------------------------------------------------------------------

		bool Lowerer::Check(std::string& why)
		{
			if (f_.unsupported)
			{
				why = f_.unsupportedWhy;
				return false;
			}
			if (env_.vuIndex != 1)
			{
				why = "VU0";
				return false;
			}
			for (size_t b = 0; b < f_.blocks.size(); b++)
				if (f_.blocks[b].node >= 0)
					node_ = static_cast<int>(b);
			if (node_ < 0)
			{
				why = "no node block";
				return false;
			}
			order_ = f_.blocks[node_].insts;
			const size_t N = f_.insts.size() + 1;
			memBase_ = static_cast<ValueId>(f_.insts.size());
			canon_.assign(N, 0);
			cls_.assign(N, Cls::None);
			remat_.assign(N, Remat::None);
			ResetLists(uses_, N);
			useCur_.assign(N, 0);
			emit_.assign(N, 0);
			reg_.assign(N, -1);
			slot_.assign(N, -1);
			homeReg_.assign(N, 0);
			noDrop_.assign(N, 0);
			finalOf_.assign(N, 0);

			// Canonical values: the lanes of one incoming vector home are one
			// value; so are all the VF0 constants.
			ValueId vecLiveIn[33] = {};
			ValueId vf0 = 0;
			for (size_t v = 1; v < f_.insts.size(); v++)
			{
				const Inst& in = f_.insts[v];
				canon_[v] = static_cast<ValueId>(v);
				if (in.op == Op::LiveIn && VecHomeOf(in.var))
				{
					const u32 r = VecHomeOf(in.var);
					if (!vecLiveIn[r])
						vecLiveIn[r] = static_cast<ValueId>(v);
					canon_[v] = vecLiveIn[r];
				}
				else if (in.op == Op::Const && in.type == Type::Vec)
				{
					if (!vf0)
						vf0 = static_cast<ValueId>(v);
					canon_[v] = vf0;
				}
				cls_[v] = ClsOf(in);
				if (in.op == Op::LiveIn)
				{
					if (IsPQ(in.var))
						remat_[v] = Remat::PQ;
					else if (IsStatus(in.var))
						remat_[v] = Remat::None; // lives in its register
					else
					{
						const u32 hv = VecHomeOf(in.var) ? VecVar(VecHomeOf(in.var)) : in.var;
						remat_[v] = hm_->home[hv].kind == HomeKind::Mem ? Remat::Home : Remat::None;
					}
				}
				else if (in.op == Op::Const && in.type == Type::Vec)
					remat_[v] = Remat::VF0;
				else if (in.op == Op::Const || in.op == Op::IConst)
					remat_[v] = Remat::Const;
			}
			canon_[memBase_] = memBase_;
			cls_[memBase_] = Cls::G;
			remat_[memBase_] = Remat::MemBase;
			folded_.assign(N, 0);
			constVal_.assign(N, 0);

			for (ValueId v : order_)
			{
				const Inst& in = I(v);
				switch (in.op)
				{
					case Op::YieldCheck: case Op::ExitIf: case Op::Exit: case Op::Phi:
						why = "whole-program op in a per-node body";
						return false;
					case Op::Kick:
						if (!env_.xgkickFn)
						{
							why = "no XGKICK helper";
							return false;
						}
						break;
					case Op::Xtop:
						if (!env_.vifTop)
						{
							why = "no VIF registers";
							return false;
						}
						break;
					case Op::Leave:
						leave_ = v;
						break;
					default:
						break;
				}
			}
			if (!leave_)
			{
				why = "no leave";
				return false;
			}
			// Milestone 8: variables dead at the node's end (region liveness)
			// are left out of the end: their final values are neither
			// computed nor written.
			hasLeave2_ = false;
			if (liveOut_ && (!f_.leaveArgs.empty() || f_.insts[leave_].args.empty()))
			{
				const Inst& orig = f_.insts[leave_];
				leaveKs_.clear();
				for (u32 k : f_.leaveArgs)
				{
					if (k < var::Count && !liveOut_->test(k))
					{
						if (!hasLeave2_)
						{
							leave2_ = orig;
							hasLeave2_ = true;
						}
						leave2_.args[k] = 0;
						continue;
					}
					leaveKs_.push_back(k);
				}
			}
			else if (!f_.leaveArgs.empty() || f_.insts[leave_].args.empty())
				leaveKs_.assign(f_.leaveArgs.begin(), f_.leaveArgs.end());
			else
			{
				leaveKs_.clear();
				const ValueId* la = LeaveInst().args.data();
				const size_t ln = LeaveInst().args.size();
				for (size_t k = 0; k < ln; k++)
					if (la[k])
						leaveKs_.push_back(static_cast<u32>(k));
			}

			FoldAndAlias();

			acc_ = env_.stickyAcc && mode_ == LowerMode::Region;

			// Dead code: work nothing reaches. Side effects are roots. An
			// aliased value needs the value it stands for. With the sticky
			// accumulators the status chain's operations are effects too: each one
			// moves the chain word or the accumulators on, whether or not anybody
			// reads its instance.
			std::vector<ValueId>& work = work_;
			work.clear();
			for (ValueId v : order_)
			{
				const Op op = I(v).op;
				if (op == Op::StoreQ || op == Op::StoreI || op == Op::Kick || op == Op::Leave || (acc_ && IsChainOp(op)))
				{
					emit_[v] = 1;
					work.push_back(v);
				}
			}
			const Inst& lv = LeaveInst();
			while (!work.empty())
			{
				const ValueId v = work.back();
				work.pop_back();
				const bool chain = acc_ && IsChainOp(I(v).op);
				ForArgs(v, I(v), [&](size_t k, ValueId a) {
					if (chain && k == 0)
						return; // the chain word is STK, not the previous instance
					if (emit_[a])
						return;
					if (I(a).type == Type::Mem)
						return; // memory order only
					// A Leave argument that is its own variable's incoming value
					// needs nothing.
					if (v == leave_ && k < var::Count && I(a).op == Op::LiveIn && I(a).var == k)
						return;
					emit_[a] = 1;
					work.push_back(a);
					const ValueId c = C(a);
					if (c != a && c < f_.insts.size() && !emit_[c])
					{
						emit_[c] = 1;
						work.push_back(c);
					}
				});
			}

			if (env_.schedule)
				Schedule();

			if (acc_)
			{
				// Which chain operations' instance words somebody reads: any use but
				// the next chain operation's previous-instance operand.
				chainReal_.assign(canon_.size(), 0);
				for (ValueId w : order_)
				{
					if (!emit_[w])
						continue;
					const bool chain = IsChainOp(I(w).op);
					ForArgs(w, I(w), [&](size_t k, ValueId a) {
						if (chain && k == 0)
							return;
						const ValueId c = C(a);
						if (c < chainReal_.size() && IsChainOp(I(c).op))
							chainReal_[c] = 1;
					});
				}
				baseIn_.clear();
				finalBase_ = 0;
				for (ValueId w : order_)
				{
					if (!emit_[w] || !IsChainOp(I(w).op))
						continue;
					if (finalBase_)
						baseIn_.push_back({w, finalBase_});
					if (DefinesBase(chainReal_[w], I(w)))
						finalBase_ = w;
				}
			}
			else
				FoldStatusChains();

			// Status updates in the MAC layout reuse the MAC word built from the same
			// result and field, when it comes first.
			macOf_.clear();
			for (u32 p = 0; p < order_.size(); p++)
			{
				const ValueId v = order_[p];
				if (!emit_[v] || I(v).op != Op::StatusUpd || !(I(v).imm & 2))
					continue;
				for (u32 q = 0; q < p; q++)
				{
					const ValueId m = order_[q];
					if (emit_[m] && I(m).op == Op::MacFlags && C(I(m).args[0]) == C(I(v).args[1]) && I(m).mask == I(v).mask)
					{
						macOf_.push_back({v, m});
						break;
					}
				}
			}

			// Uses, by position. A Leave operand that is its own variable's
			// incoming value is not a use.
			for (u32 p = 0; p < order_.size(); p++)
			{
				const ValueId v = order_[p];
				if (!emit_[v])
					continue;
				const Inst& in = I(v);
				const auto addUse = [&](ValueId c) {
					auto& u = uses_[c];
					if (u.empty() || u.back() != p)
						u.push_back(p);
				};
				const auto ch = statusChain_.find(v);
				if (ch != statusChain_.end())
				{
					// a folded chain reads its base and every folded update's result
					addUse(StatusBase(v));
					for (ValueId m : ch->second)
						addUse(C(I(m).args[1]));
				}
				ForArgs(v, in, [&](size_t k, ValueId a) {
					if (I(a).type == Type::Mem)
						return;
					if (v == leave_ && k < var::Count && I(a).op == Op::LiveIn && I(a).var == k)
						return;
					if (k == 0 && ch != statusChain_.end())
						return;
					if (k == 0 && acc_ && IsChainOp(in.op))
						return;
					addUse(C(a));
				});
				if (in.op == Op::StatusUpd)
					if (const ValueId m = MacOf(v))
						addUse(m);
				if (acc_)
				{
					// the chain word a status operation starts from, and the last one at the end
					for (const auto& e : baseIn_)
						if (e.first == v)
							addUse(e.second);
					if (v == leave_ && finalBase_)
						addUse(finalBase_);
				}
				if (in.op == Op::LoadQ || in.op == Op::LoadI || in.op == Op::StoreQ || in.op == Op::StoreI)
				{
					auto& u = uses_[memBase_];
					if (u.empty() || u.back() != p)
						u.push_back(p);
				}
			}

			const u32 leavePos = static_cast<u32>(std::find(order_.begin(), order_.end(), leave_) - order_.begin());
			leavePos_ = leavePos;
			ResetLists(finalVars_, N);
			for (u32 v : leaveKs_)
			{
				if (v >= var::Count)
					break;
				const ValueId a = lv.args[v];
				if (!a || v == var::MEM || (I(a).op == Op::LiveIn && I(a).var == v))
					continue;
				finalVars_[C(a)].push_back(v);
			}
			// The branch condition / jump target is read at the end itself.
			for (size_t k = var::Count; k < lv.args.size(); k++)
				if (lv.args[k])
					finalVars_[C(lv.args[k])].clear(), noEarly_.push_back(C(lv.args[k]));
			for (size_t v = 1; v < f_.insts.size(); v++)
				if (f_.insts[v].op == Op::LiveIn && f_.insts[v].var < var::Count)
					varLiveIn_[f_.insts[v].var] = canon_[v];

			// Values already the output of this mode's operand clamp: mode 2's
			// result-clamped FMAC steps (the float clamp is its own fixed point),
			// MADD's ACC clamp, VF0, and copies / rotations of those.
			clampFixed_.assign(N, 0);
			if (env_.clampOverflow || env_.clampExtra)
			{
				for (size_t v = 1; v < f_.insts.size(); v++)
				{
					const Inst& in = f_.insts[v];
					bool fixed = false;
					switch (in.op)
					{
						case Op::Const:
							fixed = in.type == Type::Vec;
							break;
						case Op::AccClamp:
							fixed = true;
							break;
						case Op::Fmac:
						{
							const UpperInst u = DecodeUpper(in.word);
							fixed = env_.clampExtra && ResultClamp() && !(in.imm & 1) && u.kind != FmacKind::Max && u.kind != FmacKind::Mini;
							break;
						}
						case Op::Opmula:
						case Op::Opmsub:
							fixed = env_.clampExtra && ResultClamp() && !Single(in.mask);
							break;
						case Op::Move:
						case Op::Mr32:
						{
							const ValueId src = SingleSource(in.args, 0);
							fixed = src && clampFixed_[src];
							break;
						}
						default:
							break;
					}
					if (fixed)
						clampFixed_[canon_[v]] = 1;
				}
			}

			// Clamped copies: one pseudo value per value clamped whole, live
			// across its clamped uses.
			{
				Lists& cu = cu_;
				ResetLists(cu, N);
				std::vector<ValueId> ops;
				for (u32 p = 0; p < order_.size(); p++)
				{
					if (!emit_[order_[p]])
						continue;
					ops.clear();
					ClampedOperands(order_[p], ops);
					for (ValueId c : ops)
						if (!clampFixed_[c] && (cu[c].empty() || cu[c].back() != p))
							cu[c].push_back(p);
				}
				clampOf_.assign(N, 0);
				for (size_t c = 1; c < N; c++)
				{
					if (cu[c].empty())
						continue;
					clampOf_[c] = NewPseudo(Cls::V, Remat::None, cu[c]);
				}
				clampLane_.assign(canon_.size(), 0);
			}

			// Shared sign/zero vectors and mVUglob constants used more than once.
			{
				globAddr_.assign(canon_.size(), nullptr);
				// (key, list) pairs, few per node: flat, searched linearly, then
				// taken in key order as a map would give them.
				static thread_local std::vector<std::pair<std::pair<ValueId, int>, std::vector<u32>>> zsT;
				static thread_local std::vector<std::pair<const void*, std::vector<u32>>> glT;
				size_t nzs = 0, ngl = 0;
				const auto slotOf = [](auto& tab, size_t& n, const auto& key) -> std::vector<u32>& {
					for (size_t i = 0; i < n; i++)
						if (tab[i].first == key)
							return tab[i].second;
					if (tab.size() <= n)
						tab.emplace_back();
					tab[n].first = key;
					tab[n].second.clear();
					return tab[n++].second;
				};
				const auto note = [](std::vector<u32>& u, u32 p) {
					if (u.empty() || u.back() != p)
						u.push_back(p);
				};
				for (u32 p = 0; p < order_.size(); p++)
				{
					const ValueId v = order_[p];
					if (!emit_[v] || folded_[v] || canon_[v] != v)
						continue;
					const Inst& in = I(v);
					const auto pack = [&](const Inst& x) {
						const ValueId c = C(x.args[x.op == Op::MacFlags ? 0 : 1]);
						const bool mflag = x.op == Op::MacFlags || (x.imm & 2);
						note(slotOf(zsT, nzs, std::make_pair(c, Single(x.mask) ? SingleLane(x.mask) : -1)), p);
						note(slotOf(glT, ngl, PackWeights(x.mask, mflag)), p);
					};
					if (in.op == Op::MacFlags || (in.op == Op::StatusUpd && !MacOf(v) && !(acc_ && AccCand(in) && !chainReal_[v])))
						pack(in);
					const auto ch = statusChain_.find(v);
					if (ch != statusChain_.end())
						for (ValueId m : ch->second)
							pack(I(m));
				}
				std::sort(zsT.begin(), zsT.begin() + nzs, [](const auto& a, const auto& b) { return a.first < b.first; });
				std::sort(glT.begin(), glT.begin() + ngl, [](const auto& a, const auto& b) { return std::less<const void*>()(a.first, b.first); });
				for (size_t i = 0; i < nzs; i++)
					if (zsT[i].second.size() >= 2)
						zsOf_.push_back({zsT[i].first, NewPseudo(Cls::V, Remat::None, zsT[i].second)});
				for (size_t i = 0; i < ngl; i++)
					if (glT[i].second.size() >= 2)
					{
						const ValueId pid = NewPseudo(Cls::V, Remat::Global, glT[i].second);
						globAddr_[pid] = glT[i].first;
						globOf_.push_back({glT[i].first, pid});
					}
				clampLane_.resize(canon_.size(), 0);
			}
			// FCOR masks used more than once
			{
				static thread_local std::vector<std::pair<u32, std::vector<u32>>> fc;
				size_t nfc = 0;
				for (u32 p = 0; p < order_.size(); p++)
				{
					const ValueId v = order_[p];
					const Inst& in = I(v);
					if (!emit_[v] || in.op != Op::FlagRead || static_cast<LOp>(in.sub) != LOp::FCOR)
						continue;
					const u32 need = ~static_cast<u32>(in.imm) & 0xFFFFFFu;
					if (!need)
						continue;
					const u32 k = need >> __builtin_ctz(need);
					size_t i = 0;
					while (i < nfc && fc[i].first != k)
						i++;
					if (i == nfc)
					{
						if (fc.size() <= nfc)
							fc.emplace_back();
						fc[nfc].first = k;
						fc[nfc].second.clear();
						nfc++;
					}
					fc[i].second.push_back(p);
				}
				for (size_t i = 0; i < nfc; i++)
					if (fc[i].second.size() >= 2)
					{
						const ValueId pid = NewPseudo(Cls::G, Remat::Const, fc[i].second);
						folded_[pid] = 1;
						constVal_[pid] = fc[i].first;
						fcorConst_.push_back({fc[i].first, pid});
					}
				clampLane_.resize(canon_.size(), 0);
			}
			analysisN_ = static_cast<u32>(N);
			return BindHomes(why);
		}

		// The half of Check that depends on the homes: how incoming values are
		// produced again, which results go straight to a register home, the
		// register pools and the pressure bound.
		bool Lowerer::CheckShape(const Function& f_, const LowerEnv& env_, std::string& why)
		{
			if (f_.unsupported)
			{
				why = f_.unsupportedWhy;
				return false;
			}
			if (env_.vuIndex != 1)
			{
				why = "VU0";
				return false;
			}
			int node = -1;
			for (size_t b = 0; b < f_.blocks.size(); b++)
				if (f_.blocks[b].node >= 0)
					node = static_cast<int>(b);
			if (node < 0)
			{
				why = "no node block";
				return false;
			}
			bool leave = false;
			for (ValueId v : f_.blocks[node].insts)
			{
				switch (f_.insts[v].op)
				{
					case Op::YieldCheck: case Op::ExitIf: case Op::Exit: case Op::Phi:
						why = "whole-program op in a per-node body";
						return false;
					case Op::Kick:
						if (!env_.xgkickFn)
						{
							why = "no XGKICK helper";
							return false;
						}
						break;
					case Op::Xtop:
						if (!env_.vifTop)
						{
							why = "no VIF registers";
							return false;
						}
						break;
					case Op::Leave:
						leave = true;
						break;
					default:
						break;
				}
			}
			if (!leave)
			{
				why = "no leave";
				return false;
			}
			return true;
		}

		bool Lowerer::BindHomes(std::string& why)
		{
			const size_t N = analysisN_;
			const Inst& lv = LeaveInst();
			const u32 leavePos = leavePos_;
			for (size_t v = 1; v < f_.insts.size(); v++)
			{
				const Inst& in = f_.insts[v];
				if (in.op != Op::LiveIn || IsPQ(in.var) || IsStatus(in.var))
					continue;
				const u32 hv = VecHomeOf(in.var) ? VecVar(VecHomeOf(in.var)) : in.var;
				remat_[v] = hm_->home[hv].kind == HomeKind::Mem ? Remat::Home : Remat::None;
			}
			finalOf_.assign(canon_.size(), 0);
			// Whole final values of register-homed variables (computed straight
			// into the home).
			for (u32 v : leaveKs_)
			{
				if (v >= var::Count)
					break;
				const ValueId a = lv.args[v];
				if (!a || I(a).op == Op::LiveIn || I(a).op == Op::Const || I(a).op == Op::IConst)
					continue;
				const ValueId c = C(a);
				if (IsStatus(v) || (GHomeable(v) && hm_->home[v].kind == HomeKind::GReg))
				{
					// only if used nowhere else after its definition as another
					// variable's value
					if (!finalOf_[c])
						finalOf_[c] = v + 1;
					else
						finalOf_[c] = ~0u; // final for two variables: no preference
				}
			}
			for (u32 r = 1; r <= 32; r++)
			{
				const u32 v0 = VecVar(r);
				if (hm_->home[v0].kind != HomeKind::VReg)
					continue;
				ValueId c = WholeFinal(v0);
				if (!c)
					c = MajorFinal(v0);
				if (c && c < f_.insts.size())
					finalOf_[c] = finalOf_[c] ? ~0u : v0 + 1;
			}
			for (auto& x : finalOf_)
				if (x == ~0u)
					x = 0;
			// Pools.
			vPool_ = kVAll & ~hm_->vRegs;
			gPool_ = kGAll & ~hm_->gRegs;
			if (hm_->memBase >= 0)
				gPool_ &= ~(1u << hm_->memBase);
			if (hm_->cycles >= 0)
				gPool_ &= ~(1u << hm_->cycles);
			const auto limit = [](u32 pool, u32 n) {
				if (!n)
					return pool;
				u32 out = 0;
				for (u32 m = pool; m && n; m &= m - 1, n--)
					out |= m & (0u - m);
				return out;
			};
			vPool_ = limit(vPool_, env_.poolLimitV);
			gPool_ = limit(gPool_, env_.poolLimitG);

			// Register pressure bound: live values at once must fit the pool
			// plus the spill area with room for temporaries. At most N values
			// are live at once, so a node of fewer values than the spill area
			// holds always fits: only the pool sizes can fail it.
			if (N + 11 <= static_cast<size_t>(kSlots + kOverflowSlots - 16))
			{
				const u32 nv = static_cast<u32>(__builtin_popcount(vPool_));
				const u32 ng = static_cast<u32>(__builtin_popcount(gPool_));
				if (nv < 12 || ng < 8)
				{
					why = "register pressure";
					return false;
				}
			}
			else
			{
				u32 liveV = 0, liveG = 0, maxV = 0, maxG = 0;
				std::vector<u32>& lastUse = lastUse_;
				lastUse.assign(N, 0);
				for (size_t c = 1; c < N; c++)
					if (!uses_[c].empty())
						lastUse[c] = uses_[c].back();
				Lists& endAt = endAt_;
				Lists& startAt = startAt_;
				ResetLists(endAt, order_.size() + 1);
				ResetLists(startAt, order_.size() + 1);
				std::vector<u32>& posOf = posOf_;
				posOf.assign(N, 0);
				for (u32 p = 0; p < order_.size(); p++)
					if (!posOf[order_[p]])
						posOf[order_[p]] = p;
				for (size_t c = 1; c < N; c++)
				{
					if (uses_[c].empty() || canon_[c] != c || (remat_[c] != Remat::None && !homeReg_[c]))
						continue;
					const u32 s = posOf[c];
					u32 e = lastUse[c];
					// A value whose only use is the end of the node can go to its
					// memory home once the incoming values there are dead.
					if (uses_[c].back() == leavePos && !finalVars_[c].empty())
					{
						bool mem = true;
						u32 until = uses_[c].size() > 1 ? uses_[c][uses_[c].size() - 2] : s;
						for (u32 v : finalVars_[c])
						{
							mem = mem && HomeIsMem(v);
							const ValueId li = varLiveIn_[v];
							if (li && !uses_[li].empty())
								until = std::max(until, uses_[li].back());
						}
						if (mem)
						{
							if (until >= leavePos)
								until = leavePos;
							e = until;
						}
					}
					startAt[s].push_back(static_cast<ValueId>(c));
					endAt[e].push_back(static_cast<ValueId>(c));
				}
				for (u32 p = 0; p <= order_.size(); p++)
				{
					for (ValueId c : startAt[p])
						(cls_[c] == Cls::V ? liveV : liveG)++;
					maxV = std::max(maxV, liveV);
					maxG = std::max(maxG, liveG);
					for (ValueId c : endAt[p])
						(cls_[c] == Cls::V ? liveV : liveG)--;
				}
				const u32 nv = static_cast<u32>(__builtin_popcount(vPool_));
				const u32 ng = static_cast<u32>(__builtin_popcount(gPool_));
				// Spill slots are shared; every instruction needs a few temps.
				const u32 overV = maxV + 6 > nv ? maxV + 6 - nv : 0;
				const u32 overG = maxG + 5 > ng ? maxG + 5 - ng : 0;
				if (overV + overG > kSlots + kOverflowSlots - 16 || nv < 12 || ng < 8)
				{
					why = "register pressure";
					return false;
				}
			}

			return true;
		}

		void Lowerer::Emit()
		{
			dirty_ = dirtyIn_;
			// Incoming values already in registers: register homes and the
			// status instances.
			for (size_t v = 1; v < f_.insts.size(); v++)
			{
				const Inst& in = f_.insts[v];
				if (in.op != Op::LiveIn || canon_[v] != v || uses_[v].empty())
					continue;
				if (IsStatus(in.var))
				{
					Bind(static_cast<ValueId>(v), 20 + static_cast<int>(in.var - var::S(0)));
					homeReg_[v] = 1;
					continue;
				}
				const u32 hv = VecHomeOf(in.var) ? VecVar(VecHomeOf(in.var)) : in.var;
				const Home& h = hm_->home[hv];
				if (h.kind != HomeKind::Mem)
				{
					Bind(static_cast<ValueId>(v), h.reg);
					homeReg_[v] = 1;
				}
			}
			// Status instances / register homes whose incoming value is never
			// read are free for their final value from the start.
			if (hm_->memBase >= 0 || env_.vuMemPinned)
			{
				// micro mode pins VU memory in x29 (microVU's gprVUMem)
				Bind(memBase_, env_.vuMemPinned ? 29 : hm_->memBase);
				homeReg_[memBase_] = 1;
			}
			// VF0 in its register, for the region's map.
			if (hm_->vf0Reg >= 0)
				for (size_t v = 1; v < f_.insts.size(); v++)
					if (f_.insts[v].op == Op::Const && f_.insts[v].type == Type::Vec && canon_[v] == v && !uses_[v].empty())
					{
						Bind(static_cast<ValueId>(v), hm_->vf0Reg);
						homeReg_[v] = 1;
					}
			// Clamped copies of invariant vector registers, kept by the region.
			for (size_t c = 1; c < clampOf_.size() && c < f_.insts.size(); c++)
			{
				if (!clampOf_[c])
					continue;
				const Inst& in = f_.insts[c];
				if (in.op != Op::LiveIn || !VecHomeOf(in.var))
					continue;
				const s8 r = hm_->clampHome[VecHomeOf(in.var)];
				if (r < 0)
				{
					// or in memory: reloaded where used, never stored again
					const s8 k = hm_->clampSlot[VecHomeOf(in.var)];
					if (k >= 0)
						slot_[clampOf_[c]] = static_cast<s16>(kClampSlot0 + k);
					continue;
				}
				Bind(clampOf_[c], r);
				homeReg_[clampOf_[c]] = 1;
			}
			FillWMemLanes();
			NormalizeFmeqInputs();
			for (pos_ = 0; pos_ < order_.size(); pos_++)
			{
				const ValueId id = order_[pos_];
				if (!emit_[id])
					continue;
				if (g_lowerMarks)
					g_lowerMarks->push_back({armGetCurrentCodePointer(), id});
				One(id);
				EndInst();
			}
		}
	} // namespace

	u32 HomeMap::VRegMask() const
	{
		u32 m = vf0Reg >= 0 ? (1u << vf0Reg) : 0;
		if (saZ >= 0)
			m |= 1u << saZ;
		if (saS >= 0)
			m |= 1u << saS;
		for (u32 r = 1; r <= 32; r++)
			if (clampHome[r] >= 0)
				m |= 1u << clampHome[r];
		for (u32 r = 1; r <= 32; r++)
		{
			const Home& h = home[VecVar(r)];
			if (h.kind == HomeKind::VReg)
				m |= 1u << h.reg;
		}
		return m;
	}

	u32 HomeMap::GRegMask() const
	{
		u32 m = stkReg >= 0 ? (1u << stkReg) : 0;
		for (u32 v = 0; v < var::Count; v++)
			if (GHomeable(v) && home[v].kind == HomeKind::GReg)
				m |= 1u << home[v].reg;
		return m;
	}

	thread_local std::vector<std::unique_ptr<Lowerer::Scratch>> Lowerer::s_pool;

	LowerResult LowerNode(const ir::Function& f, const LowerEnv& env, const HomeMap& homes, LowerMode mode, bool checkOnly)
	{
		LowerResult r;
		Lowerer lw(f, env, homes, mode);
		if (!lw.Check(r.why))
			return r;
		if (checkOnly)
		{
			r.ok = true;
			return r;
		}
		const u8* start = armGetCurrentCodePointer();
		lw.Emit();
		r.bytes = static_cast<u32>(armGetCurrentCodePointer() - start);
		r.ok = true;
		return r;
	}

	LowerResult LowerNodeBody(const ir::Function& f, const LowerEnv& env)
	{
		static const HomeMap boundary;
		return LowerNode(f, env, boundary, LowerMode::Framed);
	}

	void EmitLoadHomes(const HomeMap& homes, const LowerEnv& env, s32 cyclesOff)
	{
		for (u32 v = 0; v < var::Count; v++)
		{
			const Home& h = homes.home[v];
			if (h.kind == HomeKind::Mem)
				continue;
			const u32 vr = VecHomeOf(v);
			if (vr && (v & 3) != 0)
				continue;
			const MemLoc m = BoundaryLoc(v, env);
			if (h.kind == HomeKind::VReg)
				armAsm->Ldr(Q(h.reg), a64::MemOperand(*m.base, m.off));
			else if (m.size == 2)
				armAsm->Ldrh(W(h.reg), a64::MemOperand(*m.base, m.off));
			else
				armAsm->Ldr(W(h.reg), a64::MemOperand(*m.base, m.off));
		}
		if (homes.memBase >= 0)
			armAsm->Add(X(homes.memBase), xState, kVU1MemFromState);
		if (homes.cycles >= 0 && cyclesOff)
			armAsm->Ldr(W(homes.cycles), a64::MemOperand(xFlag, cyclesOff));
		if (homes.vf0Reg >= 0)
			armAsm->Ldr(Q(homes.vf0Reg), a64::MemOperand(xState, kOffVF));
	}

	// The clamped copy of VF r (from its home in `homes`) into register dst.
	static void ClampHomeInto(const HomeMap& homes, const LowerEnv& env, u32 r, int dst)
	{
		const u32 v0 = VecVar(r);
		int src = dst;
		if (homes.home[v0].kind == HomeKind::VReg)
			src = homes.home[v0].reg;
		else
			armAsm->Ldr(Q(dst), a64::MemOperand(xState, BoundaryLoc(v0, env).off));
		if (env.clampSign)
		{
			armAsm->Smin(V(dst).V4S(), V(src).V4S(), V(kCMax).V4S());
			armAsm->Umin(V(dst).V4S(), V(dst).V4S(), V(kCMin).V4S());
		}
		else
		{
			armAsm->Fminnm(V(dst).V4S(), V(src).V4S(), V(kCMax).V4S());
			armAsm->Fmaxnm(V(dst).V4S(), V(dst).V4S(), V(kCMin).V4S());
		}
	}

	static void ClampSlotFill(const HomeMap& homes, const LowerEnv& env, u32 r)
	{
		ClampHomeInto(homes, env, r, 0);
		armAsm->Str(a64::q0, a64::MemOperand(xFlag, env.offNeonBackup + (kClampSlot0 + homes.clampSlot[r]) * 16));
	}

	void EmitClampHomes(const HomeMap& homes, const LowerEnv& env)
	{
		for (u32 r = 1; r <= 32; r++)
			if (homes.clampSlot[r] >= 0)
				ClampSlotFill(homes, env, r);
		for (u32 r = 1; r <= 32; r++)
		{
			const int ch = homes.clampHome[r];
			if (ch < 0)
				continue;
			const u32 v0 = VecVar(r);
			int src = ch;
			if (homes.home[v0].kind == HomeKind::VReg)
				src = homes.home[v0].reg;
			else
				armAsm->Ldr(Q(ch), a64::MemOperand(xState, BoundaryLoc(v0, env).off));
			if (env.clampSign)
			{
				armAsm->Smin(V(ch).V4S(), V(src).V4S(), V(kCMax).V4S());
				armAsm->Umin(V(ch).V4S(), V(ch).V4S(), V(kCMin).V4S());
			}
			else
			{
				armAsm->Fminnm(V(ch).V4S(), V(src).V4S(), V(kCMax).V4S());
				armAsm->Fmaxnm(V(ch).V4S(), V(ch).V4S(), V(kCMin).V4S());
			}
		}
	}

	// A vector home to microVU's location: x, y and z only for a HomeMap::wMem
	// home (its w lane there is the valid one).
	static void StoreVecHome(const HomeMap& homes, u32 r, int reg, s32 off)
	{
		if (!homes.WMem(r))
		{
			armAsm->Str(Q(reg), a64::MemOperand(xState, off));
			return;
		}
		armAsm->Str(D(reg), a64::MemOperand(xState, off));
		armAsm->Umov(a64::w16, V(reg).V4S(), 2);
		armAsm->Str(a64::w16, a64::MemOperand(xState, off + 8));
	}

	void EmitHomeTransfer(const HomeMap& from, const HomeMap& to, const LiveSet* live, const LowerEnv& env)
	{
		struct Mv
		{
			bool vec;
			int src, dst;
		};
		std::vector<Mv> moves;
		const auto isLive = [&](u32 v, bool vec) {
			if (!live)
				return true;
			if (!vec)
				return live->test(v);
			for (u32 l = 0; l < 4; l++)
				if (live->test(v + l))
					return true;
			return false;
		};
		const auto regOf = [](const HomeMap& m, u32 v, HomeKind k) { return m.home[v].kind == k ? static_cast<int>(m.home[v].reg) : -1; };
		// stores first (their sources are still intact), then the register
		// moves, then the loads
		std::vector<std::pair<u32, int>> loads; // (variable, destination register)
		for (u32 v = 0; v < var::Count; v++)
		{
			const u32 vr = VecHomeOf(v);
			const bool vec = vr != 0;
			if (vec && (v & 3) != 0)
				continue;
			if (!vec && !GHomeable(v))
				continue;
			const HomeKind k = vec ? HomeKind::VReg : HomeKind::GReg;
			const int fr = regOf(from, v, k), tr = regOf(to, v, k);
			if (!isLive(v, vec))
				continue;
			const MemLoc m = BoundaryLoc(v, env);
			// Into a HomeMap::wMem home from a register holding the w lane: the
			// lane goes to microVU's location, where the target map keeps it.
			if (vec && fr >= 0 && tr >= 0 && to.WMem(vr) && !from.WMem(vr))
				armAsm->Str(Q(fr), a64::MemOperand(*m.base, m.off));
			if (fr == tr)
				continue;
			if (fr >= 0 && tr < 0)
			{
				if (vec)
					StoreVecHome(from, vr, fr, m.off);
				else if (m.size == 2)
					armAsm->Strh(W(fr), a64::MemOperand(*m.base, m.off));
				else
					armAsm->Str(W(fr), a64::MemOperand(*m.base, m.off));
			}
			else if (fr >= 0)
				moves.push_back({vec, fr, tr});
			else
				loads.push_back({v, tr});
		}
		// clamped homes: kept, moved, or made below
		std::vector<u32> make;
		for (u32 r = 1; r <= 32; r++)
		{
			const int ch = to.clampHome[r];
			if (ch < 0 || !isLive(VecVar(r), true))
				continue;
			if (from.clampHome[r] == ch)
				continue;
			if (from.clampHome[r] >= 0)
				moves.push_back({true, from.clampHome[r], ch});
			else
				make.push_back(r);
		}
		bool loadVf0 = false;
		if (to.vf0Reg >= 0 && from.vf0Reg != to.vf0Reg)
		{
			if (from.vf0Reg >= 0)
				moves.push_back({true, from.vf0Reg, to.vf0Reg});
			else
				loadVf0 = true;
		}
		// parallel register moves, cycles broken through v0 / w16
		while (!moves.empty())
		{
			bool progress = false;
			for (size_t i = 0; i < moves.size(); i++)
			{
				bool blocked = false;
				for (size_t j = 0; j < moves.size(); j++)
					if (j != i && moves[j].vec == moves[i].vec && moves[j].src == moves[i].dst)
						blocked = true;
				if (blocked)
					continue;
				if (moves[i].vec)
					armAsm->Mov(V(moves[i].dst).V16B(), V(moves[i].src).V16B());
				else
					armAsm->Mov(W(moves[i].dst), W(moves[i].src));
				moves.erase(moves.begin() + i);
				progress = true;
				break;
			}
			if (progress)
				continue;
			// a cycle: park one source in the scratch register
			Mv& m = moves[0];
			if (m.vec)
				armAsm->Mov(a64::v0.V16B(), V(m.src).V16B());
			else
				armAsm->Mov(a64::w16, W(m.src));
			const int parked = m.src;
			for (Mv& x : moves)
				if (x.vec == m.vec && x.src == parked)
					x.src = m.vec ? 0 : 16;
		}
		for (const auto& l : loads)
		{
			const MemLoc m = BoundaryLoc(l.first, env);
			if (VecHomeOf(l.first))
				armAsm->Ldr(Q(l.second), a64::MemOperand(*m.base, m.off));
			else if (m.size == 2)
				armAsm->Ldrh(W(l.second), a64::MemOperand(*m.base, m.off));
			else
				armAsm->Ldr(W(l.second), a64::MemOperand(*m.base, m.off));
		}
		if (loadVf0)
			armAsm->Ldr(Q(to.vf0Reg), a64::MemOperand(xState, kOffVF));
		// A register that held a HomeMap::wMem home and now holds a full one: its w
		// lane from microVU's location (valid: the region never writes it).
		for (u32 r = 1; r <= 32; r++)
		{
			const u32 v0 = VecVar(r);
			if (!from.WMem(r) || to.WMem(r) || from.home[v0].kind != HomeKind::VReg || to.home[v0].kind != HomeKind::VReg ||
				(live && !live->test(v0 + 3)))
				continue;
			armAsm->Ldr(a64::w16, a64::MemOperand(xState, BoundaryLoc(v0, env).off + 12));
			armAsm->Ins(V(to.home[v0].reg).V4S(), 3, a64::w16);
		}
		for (u32 r = 1; r <= 32; r++)
			if (to.clampSlot[r] >= 0 && from.clampSlot[r] != to.clampSlot[r] && isLive(VecVar(r), true))
				ClampSlotFill(to, env, r);
		for (u32 r : make)
		{
			const int ch = to.clampHome[r];
			const u32 v0 = VecVar(r);
			int src = ch;
			if (to.home[v0].kind == HomeKind::VReg)
				src = to.home[v0].reg;
			else
				armAsm->Ldr(Q(ch), a64::MemOperand(xState, BoundaryLoc(v0, env).off));
			if (env.clampSign)
			{
				armAsm->Smin(V(ch).V4S(), V(src).V4S(), V(kCMax).V4S());
				armAsm->Umin(V(ch).V4S(), V(ch).V4S(), V(kCMin).V4S());
			}
			else
			{
				armAsm->Fminnm(V(ch).V4S(), V(src).V4S(), V(kCMax).V4S());
				armAsm->Fmaxnm(V(ch).V4S(), V(ch).V4S(), V(kCMin).V4S());
			}
		}
	}

	struct NodeLowering::Impl
	{
		Lowerer lw;
		bool emitted = false;
		Impl(const ir::Function& f, const LowerEnv& env, const HomeMap& hm)
			: lw(f, env, hm, LowerMode::Region)
		{
		}
	};

	NodeLowering::NodeLowering(std::unique_ptr<Impl> impl)
		: impl_(std::move(impl))
	{
	}

	NodeLowering::~NodeLowering() = default;

	std::unique_ptr<NodeLowering> NodeLowering::Check(const ir::Function& f, const LowerEnv& env, u32 counts[33], const LiveSet* liveOut)
	{
		static const HomeMap boundary;
		auto impl = std::make_unique<Impl>(f, env, boundary);
		impl->lw.SetLiveOut(liveOut);
		std::string why;
		if (!impl->lw.Check(why))
			return nullptr;
		impl->lw.CountClampedLiveIns(counts);
		return std::unique_ptr<NodeLowering>(new NodeLowering(std::move(impl)));
	}

	bool NodeLowering::Lowerable(const ir::Function& f, const LowerEnv& env)
	{
		std::string why;
		if (!Lowerer::CheckShape(f, env, why))
			return false;
		// Beyond the shape, only the register pressure bound can fail, and only
		// for a node of more values than the spill area holds or with the
		// pools cut down for testing: then run the whole check.
		if (f.insts.size() + 12 > static_cast<size_t>(kSlots + kOverflowSlots - 16) || env.poolLimitV || env.poolLimitG)
		{
			static const HomeMap boundary;
			Lowerer lw(f, env, boundary, LowerMode::Region);
			return lw.Check(why);
		}
		return true;
	}

	NodeLowering::ChainEffect NodeLowering::Chain() const
	{
		ChainEffect e;
		bool out[2];
		impl_->lw.ChainEnds(out, e.accumulated, e.reduces);
		e.dirtyOut[0] = out[0];
		e.dirtyOut[1] = out[1];
		return e;
	}

	void NodeLowering::SetDirtyIn(bool dirty)
	{
		impl_->lw.SetDirtyIn(dirty);
	}

	LowerResult NodeLowering::Emit(const HomeMap& homes)
	{
		LowerResult r;
		pxAssertRel(!impl_->emitted, "vu-aot: a node lowering emitted twice");
		impl_->emitted = true;
		if (!impl_->lw.Rebind(homes, r.why))
			return r;
		const u8* start = armGetCurrentCodePointer();
		impl_->lw.Emit();
		r.bytes = static_cast<u32>(armGetCurrentCodePointer() - start);
		r.ok = true;
		return r;
	}

	void EmitStoreHomes(const HomeMap& homes, const LowerEnv& env, s32 cyclesOff)
	{
		for (u32 v = 0; v < var::Count; v++)
		{
			const Home& h = homes.home[v];
			if (h.kind == HomeKind::Mem)
				continue;
			const u32 vr = VecHomeOf(v);
			if (vr && (v & 3) != 0)
				continue;
			const MemLoc m = BoundaryLoc(v, env);
			if (h.kind == HomeKind::VReg)
				StoreVecHome(homes, vr, h.reg, m.off);
			else if (m.size == 2)
				armAsm->Strh(W(h.reg), a64::MemOperand(*m.base, m.off));
			else
				armAsm->Str(W(h.reg), a64::MemOperand(*m.base, m.off));
		}
		if (homes.cycles >= 0 && cyclesOff)
			armAsm->Str(W(homes.cycles), a64::MemOperand(xFlag, cyclesOff));
	}
} // namespace vuaot
