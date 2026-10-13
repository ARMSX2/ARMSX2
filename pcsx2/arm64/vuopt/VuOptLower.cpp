// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Node lowering. See VuOptLower.h for the contract.
//
// Each operation names the microVU emitter whose arithmetic it reproduces.
// Register choice is free (it cannot change a result); the arithmetic
// sequence, operand order and clamp placement are not, because they decide
// rounding, NaN propagation and flushes. Moves, copies and the order of
// independent instructions are free too.

#include "VuOptLower.h"

#include "VU.h"
#include "common/Assertions.h"
#include "arm64/AsmHelpers.h"
#include "arm64/microVU_Misc-arm64.h"

#include <algorithm>
#include <cstdio>

namespace vuopt
{
	namespace a64 = vixl::aarch64;
	using namespace ir;

	std::vector<std::pair<const u8*, ir::ValueId>>* g_lowerMarks = nullptr;

	namespace
	{
		constexpr u32 kExpMask = 0x7F800000;
		constexpr u32 kDivI = 0x1040000;
		constexpr u32 kDivD = 0x2080000;
		constexpr int kSlots = 32; // mVU.neonBackup[32]
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
				, hm_(hm)
				, mode_(mode)
			{
			}

			bool Check(std::string& why);
			void Emit();

		private:
			const Function& f_;
			const LowerEnv& env_;
			const HomeMap& hm_;
			const LowerMode mode_;
			int node_ = -1;
			std::vector<ValueId> order_; // the node's instructions
			ValueId leave_ = 0;

			// Per value (index = canonical ValueId; one extra pseudo value for
			// the memory base).
			std::vector<ValueId> canon_;
			std::vector<Cls> cls_;
			std::vector<Remat> remat_;
			std::vector<std::vector<u32>> uses_;
			std::vector<u32> useCur_;
			std::vector<u8> emit_;      // DCE: instruction is needed
			std::vector<s8> reg_;       // register (in cls_'s file) or -1
			std::vector<s16> slot_;     // spill slot or -1
			std::vector<u8> homeReg_;   // sits in its own register home (pinned there until its last use)
			std::vector<u8> noDrop_;    // remat no longer valid (its memory home is being overwritten)
			std::vector<u8> known16_;   // fits in 16 bits (VI semantics across a register home)
			ValueId memBase_ = 0;

			ValueId vOwner_[32] = {};
			ValueId gOwner_[32] = {};
			u32 vLock_ = 0, gLock_ = 0;   // registers in use by the current instruction
			u32 vTemp_ = 0, gTemp_ = 0;   // temporaries of the current instruction
			u32 vPool_ = 0, gPool_ = 0;   // allocatable (homes excluded)
			std::vector<u8> slotBusy_ = std::vector<u8>(kSlots + kOverflowSlots, 0);
			int AllocSlot();
			void FreeSlot(int s) { slotBusy_[s] = 0; }
			void SlotStore(bool vec, int r, int s);
			void SlotLoad(bool vec, int r, int s);
			u32 pos_ = 0;

			// Final value per variable (Leave), for home preference.
			std::vector<u32> finalOf_; // per value: 1 + var it is the whole final value of (G homes / V homes), or 0
			// Early write-back: a value whose only remaining use is the end of
			// the node goes to its memory home when evicted, once nothing still
			// needs the incoming value there.
			u32 leavePos_ = 0;
			std::vector<std::vector<u32>> finalVars_; // per value: the variables (lanes) it is the final value of
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
					vOwner_[r] = c;
				else
					gOwner_[r] = c;
			}
			void Unbind(ValueId c)
			{
				if (reg_[c] < 0)
					return;
				if (cls_[c] == Cls::V)
					vOwner_[reg_[c]] = 0;
				else
					gOwner_[reg_[c]] = 0;
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
			int DefG(ValueId id);
			void EndInst();
			void ReleaseLocks() { vLock_ = gLock_ = 0; vTemp_ = gTemp_ = 0; }
			void SpillForCall();

			// operand helpers
			int VecOperand(const std::vector<ValueId>& args, size_t off);
			int ScalarV(ValueId v, int* lane);   // a register and lane holding the scalar (by-element form)
			int ScalarLane0(ValueId v);          // a register whose lane 0 is the scalar
			int IntOperand(ValueId v);           // a W register
			int LaneWord(ValueId v, int lane);   // lane `lane` of a vector value in a W register
			int Broadcast(ValueId v);            // scalar broadcast to 4 lanes (temp)
			int ClampInto(int src);              // mVUclamp1 into a temp
			int SignClampInto(int src);          // mVUclamp2's sign-preserving integer clamp into a temp
			int ClampE(int src) { return env_.clampSign ? SignClampInto(src) : ClampInto(src); } // clampE operand clamp
			bool ResultClamp() const { return env_.clampExtra && !env_.clampSign; }              // mVUclamp4
			void KeepOnly(u32 before, int t);
			void MemAddr(ValueId addrV, s32 imm, int& base, int& idx);

			void PackFlags(int src, u8 mask, bool mflag, int dst);
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

			bool Known16(ValueId v) const;
		};

		//------------------------------------------------------------------
		// Allocation
		//------------------------------------------------------------------

		int Lowerer::AllocSlot()
		{
			for (int k = 0; k < kSlots + kOverflowSlots; k++)
				if (!slotBusy_[k])
				{
					slotBusy_[k] = 1;
					return k;
				}
			pxFailRel("vuopt: out of spill slots");
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
			return hm_.home[vr ? VecVar(vr) : v].kind == HomeKind::Mem;
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
					std::fprintf(stderr, "vuopt: no register to spill: op %d at %u, class %d, pool %08x locked %08x\n",
						static_cast<int>(I(order_[std::min<size_t>(pos_, order_.size() - 1)]).op), pos_, static_cast<int>(k),
						k == Cls::V ? vPool_ : gPool_, k == Cls::V ? vLock_ : gLock_);
				pxAssertRel(r >= 0, "vuopt: no register to spill");
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
					if (cls_[c] == Cls::V)
					{
						armAsm->Mov(a64::w16, static_cast<u32>(in.imm));
						armAsm->Fmov(S(r), a64::w16);
					}
					else
						armAsm->Mov(W(r), static_cast<u32>(in.imm));
					break;
				case Remat::VF0:
					armAsm->Ldr(Q(r), a64::MemOperand(xState, kOffVF));
					break;
				default:
					pxFailRel("vuopt: value has no location");
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
				const Home& h = hm_.home[var];
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

		int Lowerer::DefG(ValueId id)
		{
			const ValueId c = C(id);
			if (finalOf_[c])
			{
				const u32 var = finalOf_[c] - 1;
				int hr = -1;
				if (IsStatus(var))
					hr = 20 + static_cast<int>(var - var::S(0));
				else if (hm_.home[var].kind == HomeKind::GReg)
					hr = hm_.home[var].reg;
				if (hr >= 0 && !gOwner_[hr] && !(gLock_ & (1u << hr)))
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
		int Lowerer::VecOperand(const std::vector<ValueId>& args, size_t off)
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
			const int a = TempV(), b = TempV();
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
			armAsm->Mov(V(out).V16B(), V(a).V16B());
			armAsm->Bsl(V(out).V16B(), V(to).V16B(), V(from).V16B());
		}

		// mVUupdateFlags' sign/zero pack of the vector in V(src) into W(dst).
		void Lowerer::PackFlags(int src, u8 mask, bool mflag, int dst)
		{
			const void* weights;
			if (Single(mask))
			{
				const int lane = SingleLane(mask);
				if (lane)
				{
					const int t = TempV();
					armAsm->Ext(V(t).V16B(), V(src).V16B(), V(src).V16B(), lane * 4);
					src = t;
				}
				const int shift = mflag ? (3 - lane) : 0;
				weights = shift ? static_cast<const void*>(&mVUglob.macWeights.bySSShift[0][shift][0])
								: static_cast<const void*>(&mVUglob.macWeights.byMask[0][0][1][0]);
			}
			else
			{
				weights = &mVUglob.macWeights.byMask[0][mflag ? 1 : 0][mflag ? mask : FlipMask(mask)][0];
			}
			const int s = TempV(), z = TempV();
			armAsm->Cmlt(V(s).V4S(), V(src).V4S(), 0);
			armAsm->Fcmeq(V(z).V4S(), V(src).V4S(), 0.0);
			armAsm->Sli(V(z).V4S(), V(s).V4S(), 4);
			armAsm->Ldr(V(s), a64::MemOperand(xGlob, G(weights)));
			armAsm->And(V(z).V16B(), V(z).V16B(), V(s).V16B());
			armAsm->Addv(S(z), V(z).V4S());
			armAsm->Fmov(W(dst), S(z));
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
			const u32 l0 = vLock_;
			int fs = VecOperand(in.args, 0);
			if (step || (ct & 2))
			{
				fs = opClamp(fs, true);
				KeepOnly(l0, fs);
			}
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
					ft = VecOperand(in.args, 4);
					break;
				case FtForm::Bc:
					ft = UseReg(in.args[4]);
					ftLane = in.lane;
					break;
				case FtForm::I:
				case FtForm::Q:
					ft = ScalarV(in.args[4], &ftLane);
					break;
				default:
				{
					ft = TempV();
					armAsm->Movi(V(ft).V4S(), 0);
					break;
				}
			}
			if (ftClamp)
			{
				ft = opClamp(ft, true);
				KeepOnly(l1, ft);
			}
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
				int acc = VecOperand(in.args, 8);
				if (E)
				{
					acc = ClampE(acc);
					KeepOnly(l2, acc);
				}
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
				{
					const int d = DefV(id);
					MaxMin(fs, ft, true, d);
					break;
				}
				case FmacKind::Mini:
				{
					const int d = DefV(id);
					MaxMin(fs, ft, false, d);
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
			// a = Fs.yzxw: rotate the low three lanes
			armAsm->Ext(V(a).V16B(), V(fs).V16B(), V(fs).V16B(), 4);    // y z w x
			armAsm->Ins(V(a).V4S(), 2, V(fs).V4S(), 0);                 // y z x x
			armAsm->Ins(V(a).V4S(), 3, V(fs).V4S(), 3);                 // y z x w
			// b = Ft.zxy0
			armAsm->Ext(V(b).V16B(), V(ft).V16B(), V(ft).V16B(), 12);   // w x y z
			armAsm->Ins(V(b).V4S(), 0, V(ft).V4S(), 2);                 // z x y z
			armAsm->Ins(V(b).V4S(), 3, a64::wzr);                       // z x y 0
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
			int acc = VecOperand(in.args, 8);
			if (E)
				acc = ClampE(acc);
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
			std::vector<ValueId> fsArgs = {in.args[0], in.args[1], in.args[2], 0};
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
					const int d = DefG(id);
					armAsm->Orr(W(d), W(src), imm);
					armAsm->Add(W(d), W(d), 1);
					armAsm->Lsr(W(d), W(d), 24);
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
						armAsm->Eor(W(d), W(vi), W(src));
						armAsm->Sub(W(d), W(d), 1);
						armAsm->Lsr(W(d), W(d), 31);
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
			const int q = TempV();
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
					armAsm->Umov(W(g9), V(ftR).V4S(), 0);
					armAsm->Tst(W(g9), kExpMask);
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
			const int d = DefV(id);
			armAsm->Mov(V(d).V16B(), V(q).V16B());
		}

		void Lowerer::DivFlags(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const LowerInst l = DecodeLower(in.word);
			int fsw = -1;
			if (op != LOp::SQRT)
				fsw = LaneWord(in.args[0], l.fsf);
			const int ftw = LaneWord(in.args[1], l.ftf);
			const int t9 = TempG(), t11 = TempG();
			const int f = DefG(id);
			armAsm->Mov(W(f), 0);
			if (op == LOp::SQRT || op == LOp::RSQRT)
			{
				a64::Label pos;
				armAsm->Tbz(W(ftw), 31, &pos);
				armAsm->Mov(W(f), kDivI);
				armAsm->Bind(&pos);
			}
			if (op == LOp::DIV || op == LOp::RSQRT)
			{
				a64::Label nonzero;
				armAsm->Tst(W(ftw), kExpMask);
				armAsm->B(&nonzero, a64::ne);
				armAsm->Mov(W(t9), kDivD);
				armAsm->Mov(W(t11), kDivI);
				armAsm->Tst(W(fsw), kExpMask);
				armAsm->Csel(W(t9), W(t11), W(t9), a64::eq);
				armAsm->Orr(W(f), W(f), W(t9));
				armAsm->Bind(&nonzero);
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
			for (u32 v = 0; v < var::Count; v++)
			{
				const Home& h = hm_.home[v];
				if (h.kind == HomeKind::VReg && VecHomeOf(v) && (v & 3) == 0)
					savedV |= 1u << h.reg;
				else if (h.kind == HomeKind::GReg && (kGCallerSaved & (1u << h.reg)))
					savedG |= 1u << h.reg;
			}
			const auto homeOfReg = [&](bool vec, int r) -> s32 {
				for (u32 v = 0; v < var::Count; v++)
				{
					const Home& h = hm_.home[v];
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

		bool Lowerer::Known16(ValueId v) const
		{
			const Inst& in = I(v);
			switch (in.op)
			{
				case Op::LiveIn:
					return in.var >= var::VI(1) && in.var <= var::VI(15);
				case Op::Const:
				case Op::IConst:
					return static_cast<u32>(in.imm) < 0x10000;
				case Op::LoadI:
				case Op::Xtop:
					return true;
				default:
					return false;
			}
		}

		// The end of the node: every variable's final value to its home.
		void Lowerer::Leave(ValueId id)
		{
			const Inst& in = I(id);
			const auto ownLiveIn = [&](u32 v, ValueId val) {
				const Inst& x = I(val);
				return x.op == Op::LiveIn && x.var == v;
			};
			const auto homeKind = [&](u32 v) -> HomeKind {
				if (IsStatus(v) || IsPQ(v))
					return HomeKind::GReg; // fixed registers (handled apart)
				const u32 vr = VecHomeOf(v);
				return hm_.home[vr ? VecVar(vr) : v].kind;
			};

			// The branch condition / jump target, first, while its register is
			// certainly valid.
			const ValueId cond = in.args[var::Count];
			const ValueId jump = in.args[var::Count + 1];
			if (mode_ == LowerMode::Region)
			{
				if (cond)
					armAsm->Sxth(W(kCondReg), W(IntOperand(cond)));
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

			// Which memory homes get written.
			std::vector<u8> memWritten(var::Count, 0);
			for (u32 v = 0; v < var::Count; v++)
			{
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
			for (u32 v = 0; v < var::Count; v++)
			{
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
				if (hm_.home[v0].kind != HomeKind::Mem)
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
			for (u32 v = 0; v < var::Count; v++)
			{
				if (VecHomeOf(v) || IsStatus(v) || IsPQ(v) || v == var::MEM || v < 4)
					continue;
				if (hm_.home[v].kind != HomeKind::Mem)
					continue;
				const ValueId a = in.args[v];
				if (!a || ownLiveIn(v, a) || early_[v])
					continue;
				const MemLoc m = BoundaryLoc(v, env_);
				if (!m.base)
					continue;
				const int r = IntOperand(a);
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
			std::vector<Move> moves;
			std::vector<std::pair<int, ValueId>> statusLike;
			const auto addG = [&](u32 v, int dst) {
				const ValueId a = in.args[v];
				if (!a)
					return;
				const ValueId c = C(a);
				if (reg_[c] == dst && cls_[c] == Cls::G)
				{
					// computed in place; a VI home still keeps 16 bits
					if (mode_ == LowerMode::Region && v >= var::VI(1) && v <= var::VI(15) && !Known16(c))
						moves.push_back({false, dst, false, dst, 0, true});
					return;
				}
				if (ownLiveIn(v, a))
					return;
				Move mv;
				mv.dstV = false;
				mv.dst = dst;
				mv.trunc16 = mode_ == LowerMode::Region && v >= var::VI(1) && v <= var::VI(15) && !Known16(c);
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
			for (u32 v = 0; v < var::Count; v++)
				if (GHomeable(v) && hm_.home[v].kind == HomeKind::GReg)
					addG(v, hm_.home[v].reg);
			for (u32 r = 1; r <= 32; r++)
			{
				const u32 v0 = VecVar(r);
				const Home& h = hm_.home[v0];
				if (h.kind != HomeKind::VReg)
					continue;
				const ValueId a0 = in.args[v0];
				bool whole = a0 && !ownLiveIn(v0, a0);
				for (int l = 1; l < 4 && whole; l++)
					whole = in.args[v0 + l] && C(in.args[v0 + l]) == C(a0);
				if (!whole)
					continue;
				const ValueId c = C(a0);
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
			std::vector<Move> late;
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
				const Home& h = hm_.home[v0];
				if (h.kind != HomeKind::VReg)
					continue;
				const ValueId a0 = in.args[v0];
				bool whole = a0 && !ownLiveIn(v0, a0);
				for (int l = 1; l < 4 && whole; l++)
					whole = in.args[v0 + l] && C(in.args[v0 + l]) == C(a0);
				if (whole)
					continue;
				int l = 0;
				while (l < 4)
				{
					const ValueId a = in.args[v0 + l];
					if (!a || ownLiveIn(v0 + l, a))
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
					const int src = UseReg(in.args[0]);
					const int d = DefG(id);
					PackFlags(src, in.mask, true, d);
					break;
				}
				case Op::StatusUpd:
				{
					const bool nonSticky = in.imm & 1;
					const int src = UseReg(in.args[1]);
					const int p = TempG();
					PackFlags(src, in.mask, (in.imm & 2) != 0, p);
					const int prev = IntOperand(in.args[0]);
					const int d = DefG(id);
					if (nonSticky)
					{
						armAsm->And(W(d), W(prev), 0xfffc00ffu);
						armAsm->Orr(W(d), W(d), W(p));
						armAsm->Orr(W(d), W(d), a64::Operand(W(p), a64::LSL, 8));
					}
					else
						armAsm->Orr(W(d), W(prev), W(p));
					break;
				}
				case Op::StatusDiv:
				{
					const int prev = IntOperand(in.args[0]);
					const int df = IntOperand(in.args[1]);
					const int d = DefG(id);
					armAsm->Bic(W(d), W(prev), 0x000C0000u);
					armAsm->Orr(W(d), W(d), W(df));
					break;
				}
				case Op::StatusSet:
				{
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
							const int d = DefG(id);
							if (op == LOp::IADD) armAsm->Add(W(d), W(a), W(b));
							else if (op == LOp::ISUB) armAsm->Sub(W(d), W(a), W(b));
							else if (op == LOp::IAND) armAsm->And(W(d), W(a), W(b));
							else armAsm->Orr(W(d), W(a), W(b));
							break;
						}
						case LOp::ISUBIU:
						{
							const int d = DefG(id);
							armAsm->Sub(W(d), W(a), static_cast<u32>(in.imm));
							break;
						}
						default: // IADDI / IADDIU
						{
							const int d = DefG(id);
							armAsm->Add(W(d), W(a), static_cast<u32>(in.imm));
							break;
						}
					}
					break;
				}
				case Op::IncVI:
				{
					const int a = IntOperand(in.args[0]);
					const int d = DefG(id);
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
					armAsm->Ldr(Q(d), a64::MemOperand(X(base), X(idx)));
					break;
				}
				case Op::LoadI:
				{
					int base, idx;
					MemAddr(in.args[1], in.imm, base, idx);
					armAsm->Add(X(idx), X(base), X(idx));
					const int d = DefG(id);
					armAsm->Ldrh(W(d), a64::MemOperand(X(idx), static_cast<s32>(VuIlwLaneOffset(in.word))));
					break;
				}
				case Op::StoreQ:
				{
					const int src = VecOperand(in.args, 2);
					int base, idx;
					MemAddr(in.args[1], in.imm, base, idx);
					if (in.mask == 0xf)
						armAsm->Str(Q(src), a64::MemOperand(X(base), X(idx)));
					else
					{
						armAsm->Add(X(idx), X(base), X(idx));
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
									armAsm->Str(D(src), a64::MemOperand(X(idx), 0));
								else
								{
									const int t = TempV();
									armAsm->Dup(D(t), V(src).V2D(), 1);
									armAsm->Str(D(t), a64::MemOperand(X(idx), 8));
								}
								l += 2;
							}
							else
							{
								if (l == 0)
									armAsm->Str(S(src), a64::MemOperand(X(idx), 0));
								else
								{
									const int t = TempV();
									armAsm->Dup(S(t), V(src).V4S(), l);
									armAsm->Str(S(t), a64::MemOperand(X(idx), l * 4));
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
					int base, idx;
					MemAddr(in.args[1], in.imm, base, idx);
					armAsm->Add(X(idx), X(base), X(idx));
					const int t = TempG();
					armAsm->Uxth(W(t), W(val));
					for (int l = 0; l < 4; l++)
						if (MaskLane(in.mask, l))
							armAsm->Str(W(t), a64::MemOperand(X(idx), l * 4));
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
					const int src = VecOperand(in.args, 0);
					const int c = ClampE(src);
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
			uses_.assign(N, {});
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
						remat_[v] = hm_.home[hv].kind == HomeKind::Mem ? Remat::Home : Remat::None;
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

			// Dead code: work nothing reaches. Side effects are roots.
			std::vector<ValueId> work;
			for (ValueId v : order_)
			{
				const Op op = I(v).op;
				if (op == Op::StoreQ || op == Op::StoreI || op == Op::Kick || op == Op::Leave)
				{
					emit_[v] = 1;
					work.push_back(v);
				}
			}
			const Inst& lv = I(leave_);
			while (!work.empty())
			{
				const ValueId v = work.back();
				work.pop_back();
				const Inst& in = I(v);
				for (size_t k = 0; k < in.args.size(); k++)
				{
					const ValueId a = in.args[k];
					if (!a || emit_[a])
						continue;
					if (I(a).type == Type::Mem)
						continue; // memory order only
					// A Leave argument that is its own variable's incoming value
					// needs nothing.
					if (v == leave_ && k < var::Count && I(a).op == Op::LiveIn && I(a).var == k)
						continue;
					emit_[a] = 1;
					work.push_back(a);
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
				for (size_t k = 0; k < in.args.size(); k++)
				{
					const ValueId a = in.args[k];
					if (!a || I(a).type == Type::Mem)
						continue;
					if (v == leave_ && k < var::Count && I(a).op == Op::LiveIn && I(a).var == k)
						continue;
					auto& u = uses_[C(a)];
					if (u.empty() || u.back() != p)
						u.push_back(p);
				}
				if (in.op == Op::LoadQ || in.op == Op::LoadI || in.op == Op::StoreQ || in.op == Op::StoreI)
				{
					auto& u = uses_[memBase_];
					if (u.empty() || u.back() != p)
						u.push_back(p);
				}
			}

			// Whole final values of register-homed variables (computed straight
			// into the home).
			const u32 leavePos = static_cast<u32>(std::find(order_.begin(), order_.end(), leave_) - order_.begin());
			for (u32 v = 0; v < var::Count; v++)
			{
				const ValueId a = lv.args[v];
				if (!a || I(a).op == Op::LiveIn || I(a).op == Op::Const || I(a).op == Op::IConst)
					continue;
				const ValueId c = C(a);
				if (IsStatus(v) || (GHomeable(v) && hm_.home[v].kind == HomeKind::GReg))
				{
					// only if used nowhere else after its definition as another
					// variable's value
					if (!finalOf_[c])
						finalOf_[c] = v + 1;
					else
						finalOf_[c] = ~0u; // final for two variables: no preference
				}
				const u32 vr = VecHomeOf(v);
				if (vr && (v & 3) == 0 && hm_.home[v].kind == HomeKind::VReg)
				{
					bool whole = true;
					for (int l = 1; l < 4; l++)
						whole = whole && lv.args[v + l] && C(lv.args[v + l]) == c;
					if (whole)
						finalOf_[c] = finalOf_[c] ? ~0u : v + 1;
				}
			}
			for (auto& x : finalOf_)
				if (x == ~0u)
					x = 0;
			leavePos_ = leavePos;
			finalVars_.assign(N, {});
			for (u32 v = 0; v < var::Count; v++)
			{
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

			// Pools.
			vPool_ = kVAll & ~hm_.VRegMask();
			gPool_ = kGAll & ~hm_.GRegMask();
			if (hm_.memBase >= 0)
				gPool_ &= ~(1u << hm_.memBase);
			if (hm_.cycles >= 0)
				gPool_ &= ~(1u << hm_.cycles);
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
			// plus the spill area with room for temporaries.
			{
				u32 liveV = 0, liveG = 0, maxV = 0, maxG = 0;
				std::vector<u32> lastUse(N, 0), firstDef(N, ~0u);
				for (size_t c = 1; c < N; c++)
					if (!uses_[c].empty())
						lastUse[c] = uses_[c].back();
				std::vector<std::vector<ValueId>> endAt(order_.size() + 1), startAt(order_.size() + 1);
				for (size_t c = 1; c < N; c++)
				{
					if (uses_[c].empty() || canon_[c] != c || (remat_[c] != Remat::None && !homeReg_[c]))
						continue;
					u32 s = 0;
					const auto it = std::find(order_.begin(), order_.end(), static_cast<ValueId>(c));
					if (it != order_.end())
						s = static_cast<u32>(it - order_.begin());
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
				const Home& h = hm_.home[hv];
				if (h.kind != HomeKind::Mem)
				{
					Bind(static_cast<ValueId>(v), h.reg);
					homeReg_[v] = 1;
				}
			}
			// Status instances / register homes whose incoming value is never
			// read are free for their final value from the start.
			if (hm_.memBase >= 0)
			{
				Bind(memBase_, hm_.memBase);
				homeReg_[memBase_] = 1;
			}
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
		u32 m = 0;
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
		u32 m = 0;
		for (u32 v = 0; v < var::Count; v++)
			if (GHomeable(v) && home[v].kind == HomeKind::GReg)
				m |= 1u << home[v].reg;
		return m;
	}

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
				armAsm->Str(Q(h.reg), a64::MemOperand(*m.base, m.off));
			else if (m.size == 2)
				armAsm->Strh(W(h.reg), a64::MemOperand(*m.base, m.off));
			else
				armAsm->Str(W(h.reg), a64::MemOperand(*m.base, m.off));
		}
		if (homes.cycles >= 0 && cyclesOff)
			armAsm->Str(W(homes.cycles), a64::MemOperand(xFlag, cyclesOff));
	}
} // namespace vuopt
