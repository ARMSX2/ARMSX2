// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Milestone 2 lowering. See VuOptLower.h for the contract.
//
// Each case below names the microVU emitter whose arithmetic it reproduces.
// Register choice is free (it cannot change a result); the instruction
// sequence, operand order and clamp placement are not, because they decide
// rounding, NaN propagation and flushes.

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

	namespace
	{
		constexpr u32 kExpMask = 0x7F800000;
		constexpr u32 kDivI = 0x1040000;
		constexpr u32 kDivD = 0x2080000;
		constexpr u32 kMaxSlots = 4096;

		alignas(64) u8 s_slotArea[2][kMaxSlots * 16];

		// Pinned by microVU's dispatcher.
		const a64::Register& xState = a64::x19;
		const a64::Register& xFlag = a64::x24;
		const a64::Register& xGlob = a64::x25;
		// Ours for the body: a free VI-pool register (callee-saved).
		const a64::Register& xSlot = a64::x28;

		const a64::VRegister qPQ(28, 128);
		const a64::VRegister qCMax(25, 128);
		const a64::VRegister qCMin(26, 128);

		a64::VRegister V(int n) { return a64::VRegister(n, 128); }
		a64::VRegister S(int n) { return a64::VRegister(n, 32); }
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

		class Lowerer
		{
		public:
			Lowerer(const Function& f, const LowerEnv& env)
				: f_(f)
				, env_(env)
			{
			}

			bool Check(std::string& why);
			void Emit();

		private:
			const Function& f_;
			const LowerEnv& env_;
			std::vector<u32> slot_;   // per value; ~0u = none
			std::vector<u32> staged_; // per value: slot holding a LiveIn captured before Leave writes homes
			bool useStaged_ = false;
			int node_ = -1;

			const Inst& I(ValueId v) const { return f_.insts[v]; }

			a64::MemOperand Slot(ValueId v) const { return a64::MemOperand(xSlot, static_cast<int64_t>(slot_[v]) * 16); }

			// Home of a LiveIn value, as a vector load (VF/ACC).
			bool VecHome(ValueId v, s32& off) const
			{
				const Inst& in = I(v);
				if (in.op == Op::Const && in.type == Type::Vec)
				{
					off = kOffVF;
					return true;
				}
				if (in.op != Op::LiveIn)
					return false;
				if (in.var < 128)
				{
					off = kOffVF + static_cast<s32>(in.var / 4) * 16;
					return true;
				}
				if (in.var < 132)
				{
					off = kOffACC;
					return true;
				}
				return false;
			}

			// Identity of the storage a value comes from, so lanes of one VF home
			// count as one source.
			u64 SourceKey(ValueId v) const
			{
				s32 off;
				if (VecHome(v, off))
					return 0x100000000ull | static_cast<u32>(off);
				return v;
			}

			// Full vector of `v` into V(dst) (lane L of a lane variable's value is
			// its own lane). Scalars land in lane 0.
			void LoadVec(ValueId v, int dst)
			{
				const Inst& in = I(v);
				if (useStaged_ && staged_[v] != ~0u)
				{
					if (in.type == Type::Vec)
						armAsm->Ldr(V(dst), a64::MemOperand(xSlot, static_cast<int64_t>(staged_[v]) * 16));
					else
						armAsm->Ldr(S(dst), a64::MemOperand(xSlot, static_cast<int64_t>(staged_[v]) * 16));
					return;
				}
				s32 off;
				if (VecHome(v, off))
				{
					armAsm->Ldr(V(dst), a64::MemOperand(xState, off));
					return;
				}
				if (in.op == Op::LiveIn || in.op == Op::Const)
				{
					LoadWordScalar(v, dst);
					return;
				}
				if (in.type == Type::Vec)
					armAsm->Ldr(V(dst), Slot(v));
				else
					armAsm->Ldr(S(dst), Slot(v));
			}

			// A 32-bit scalar (float or int bits) into lane 0 of V(dst).
			void LoadWordScalar(ValueId v, int dst)
			{
				const Inst& in = I(v);
				if (in.op == Op::LiveIn)
				{
					const u32 var = in.var;
					if (var >= var::QL0 && var <= var::PL1)
					{
						armAsm->Dup(V(dst).V4S(), qPQ.V4S(), static_cast<int>(var - var::QL0));
						return;
					}
				}
				LoadInt(v, 9);
				armAsm->Fmov(S(dst), W(9));
			}

			// A scalar broadcast to all four lanes of V(dst).
			void LoadBroadcast(ValueId v, int dst)
			{
				LoadVec(v, dst);
				armAsm->Dup(V(dst).V4S(), V(dst).V4S(), 0);
			}

			// 32-bit value of an Int/Float value into W(dst).
			void LoadInt(ValueId v, int dst)
			{
				const Inst& in = I(v);
				const a64::Register w = W(dst);
				if (useStaged_ && staged_[v] != ~0u)
				{
					armAsm->Ldr(w, a64::MemOperand(xSlot, static_cast<int64_t>(staged_[v]) * 16));
					return;
				}
				if (in.op == Op::Const || in.op == Op::IConst)
				{
					armAsm->Mov(w, static_cast<u32>(in.imm));
					return;
				}
				if (in.op == Op::LiveIn)
				{
					const u32 var = in.var;
					if (var >= var::VI(1) && var <= var::VI(15))
						armAsm->Ldrh(w, a64::MemOperand(xState, kOffVI + static_cast<s32>(var - var::VI(0)) * kViStride));
					else if (var == var::I)
						armAsm->Ldr(w, a64::MemOperand(xState, kOffVI + REG_I * kViStride));
					else if (var == var::R)
						armAsm->Ldr(w, a64::MemOperand(xState, kOffVI + REG_R * kViStride));
					else if (var >= var::QL0 && var <= var::PL1)
						armAsm->Umov(w, qPQ.V4S(), static_cast<int>(var - var::QL0));
					else if (var >= var::S(0) && var <= var::S(3))
						armAsm->Mov(w, W(20 + static_cast<int>(var - var::S(0))));
					else if (var >= var::M(0) && var <= var::M(3))
						armAsm->Ldr(w, a64::MemOperand(xFlag, static_cast<s32>(var - var::M(0)) * 4));
					else if (var >= var::C(0) && var <= var::C(3))
						armAsm->Ldr(w, a64::MemOperand(xFlag, 16 + static_cast<s32>(var - var::C(0)) * 4));
					else if (var == var::DIVFLAG)
						armAsm->Ldr(w, a64::MemOperand(xFlag, env_.offDivFlag));
					else if (var == var::VIBACKUP)
						armAsm->Ldr(w, a64::MemOperand(xFlag, env_.offVIbackup));
					else if (var == var::XGKADDR)
						armAsm->Ldr(w, a64::MemOperand(xFlag, env_.offVIxgkick));
					else
						armAsm->Mov(w, 0);
					return;
				}
				armAsm->Ldr(w, Slot(v));
			}

			// A LiveIn that leaves the node through a different variable is read
			// before the write-back starts, since its own home may be among those
			// written (the VI backup slot carries the old value of the VI the
			// delay slot overwrites).
			bool CrossLiveIn(u32 var, ValueId v) const
			{
				if (!v)
					return false;
				const Inst& x = I(v);
				return x.op == Op::LiveIn && x.var != var && x.type != Type::Mem;
			}

			// Lane `lane` of a vector value's word, into W(dst).
			void LoadLaneWord(ValueId v, int lane, int dst)
			{
				const Inst& in = I(v);
				if (in.type == Type::Vec)
				{
					LoadVec(v, 16);
					armAsm->Umov(W(dst), V(16).V4S(), lane);
				}
				else
					LoadInt(v, dst);
			}

			// Operand vector from four lane sources args[off..off+3] (0 = not
			// read): one load when a single source covers them, else inserts.
			void Assemble(const std::vector<ValueId>& args, size_t off, int dst, int tmp)
			{
				ValueId first = 0;
				bool multi = false;
				for (int l = 0; l < 4; l++)
				{
					const ValueId v = off + l < args.size() ? args[off + l] : 0;
					if (!v)
						continue;
					if (!first)
						first = v;
					else if (SourceKey(v) != SourceKey(first))
						multi = true;
				}
				if (!first)
				{
					armAsm->Movi(V(dst).V4S(), 0);
					return;
				}
				LoadVec(first, dst);
				if (!multi)
					return;
				for (int l = 0; l < 4; l++)
				{
					const ValueId v = args[off + l];
					if (!v || SourceKey(v) == SourceKey(first))
						continue;
					LoadVec(v, tmp);
					armAsm->Ins(V(dst).V4S(), l, V(tmp).V4S(), l);
				}
			}

			void StoreVec(ValueId v, int src) { armAsm->Str(V(src), Slot(v)); }
			void StoreS(ValueId v, int src) { armAsm->Str(S(src), Slot(v)); }
			void StoreW(ValueId v, int src) { armAsm->Str(W(src), Slot(v)); }

			// mVUclamp1 on a whole register (mode 1 operand clamp).
			void Clamp(int r)
			{
				armAsm->Fminnm(V(r).V4S(), V(r).V4S(), qCMax.V4S());
				armAsm->Fmaxnm(V(r).V4S(), V(r).V4S(), qCMin.V4S());
			}
			void ClampS(int r)
			{
				armAsm->Fminnm(S(r), S(r), a64::VRegister(qCMax.GetCode(), 32));
				armAsm->Fmaxnm(S(r), S(r), a64::VRegister(qCMin.GetCode(), 32));
			}

			// microVU's operand clamp table (clampType) for the shipped mode.
			static int ClampType(const UpperInst& u)
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
			void MaxMin(int to, int from, bool isMax, int out)
			{
				armAsm->Sshr(V(17).V4S(), V(to).V4S(), 31);
				armAsm->Ushr(V(17).V4S(), V(17).V4S(), 1);
				armAsm->Eor(V(17).V16B(), V(17).V16B(), V(to).V16B());
				armAsm->Sshr(V(18).V4S(), V(from).V4S(), 31);
				armAsm->Ushr(V(18).V4S(), V(18).V4S(), 1);
				armAsm->Eor(V(18).V16B(), V(18).V16B(), V(from).V16B());
				if (isMax)
					armAsm->Cmgt(V(17).V4S(), V(17).V4S(), V(18).V4S());
				else
					armAsm->Cmgt(V(17).V4S(), V(18).V4S(), V(17).V4S());
				armAsm->Mov(V(out).V16B(), V(to).V16B());
				armAsm->Bif(V(out).V16B(), V(from).V16B(), V(17).V16B());
			}

			// mVUupdateFlags' sign/zero pack into W(dst).
			void PackFlags(ValueId result, u8 mask, bool mflag, int dst)
			{
				LoadVec(result, 19);
				int src = 19;
				const void* weights;
				if (Single(mask))
				{
					const int lane = SingleLane(mask);
					if (lane)
					{
						armAsm->Ext(V(20).V16B(), V(19).V16B(), V(19).V16B(), lane * 4);
						src = 20;
					}
					const int shift = mflag ? (3 - lane) : 0;
					weights = shift ? static_cast<const void*>(&mVUglob.macWeights.bySSShift[0][shift][0])
									: static_cast<const void*>(&mVUglob.macWeights.byMask[0][0][1][0]);
				}
				else
				{
					weights = &mVUglob.macWeights.byMask[0][mflag ? 1 : 0][mflag ? mask : FlipMask(mask)][0];
				}
				armAsm->Cmlt(V(21).V4S(), V(src).V4S(), 0);
				armAsm->Fcmeq(V(22).V4S(), V(src).V4S(), 0.0);
				armAsm->Sli(V(22).V4S(), V(21).V4S(), 4);
				armAsm->Ldr(V(21), a64::MemOperand(xGlob, G(weights)));
				armAsm->And(V(22).V16B(), V(22).V16B(), V(21).V16B());
				armAsm->Addv(S(22), V(22).V4S());
				armAsm->Fmov(W(dst), S(22));
			}

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
			void MemAddr(ValueId addrV, s32 imm, int dstX);
		};

		bool Lowerer::Check(std::string& why)
		{
			if (f_.unsupported)
			{
				why = f_.unsupportedWhy;
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
			slot_.assign(f_.insts.size(), ~0u);
			u32 n = 0;
			for (ValueId v : f_.blocks[node_].insts)
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
					default:
						break;
				}
				if (in.type == Type::None || in.type == Type::Mem || in.op == Op::Const || in.op == Op::IConst)
					continue;
				slot_[v] = n++;
			}
			staged_.assign(f_.insts.size(), ~0u);
			for (ValueId v : f_.blocks[node_].insts)
			{
				const Inst& in = I(v);
				if (in.op != Op::Leave)
					continue;
				for (u32 var = 0; var < var::Count; var++)
				{
					const ValueId a = in.args[var];
					if (CrossLiveIn(var, a) && staged_[a] == ~0u)
						staged_[a] = n++;
				}
			}
			if (n > kMaxSlots)
			{
				why = "too many values for the slot area";
				return false;
			}
			if (env_.vuIndex != 1)
			{
				why = "VU0";
				return false;
			}
			if (!env_.clampOverflow)
			{
				why = "clamp mode 0";
				return false;
			}
			return true;
		}

		void Lowerer::MemAddr(ValueId addrV, s32 imm, int dstX)
		{
			// mVUaddrFix + mVUmemAtIndex on VU1: ((vi + imm) & 0x3ff) * 16 off the pin
			LoadInt(addrV, dstX);
			if (imm > 0)
				armAsm->Add(W(dstX), W(dstX), static_cast<u32>(imm));
			else if (imm < 0)
				armAsm->Sub(W(dstX), W(dstX), static_cast<u32>(-imm));
			armAsm->Ubfiz(W(dstX), W(dstX), 4, 10);
			armAsm->Add(X(dstX), xState, X(dstX));
		}

		void Lowerer::Fmac(ValueId id)
		{
			const Inst& in = I(id);
			const UpperInst u = DecodeUpper(in.word);
			if (in.imm & 1)
			{
				// doSafeSub: Ft == Fs SUB/SUBA writes +0.
				armAsm->Movi(V(4).V4S(), 0);
				StoreVec(id, 4);
				return;
			}
			Assemble(in.args, 0, 0, 7); // Fs
			switch (u.form)
			{
				case FtForm::Reg: Assemble(in.args, 4, 1, 7); break;
				case FtForm::Bc:
					LoadVec(in.args[4], 1);
					armAsm->Dup(V(1).V4S(), V(1).V4S(), in.lane);
					break;
				case FtForm::I:
				case FtForm::Q:
					LoadBroadcast(in.args[4], 1);
					break;
				default:
					armAsm->Movi(V(1).V4S(), 0);
					break;
			}
			const int ct = ClampType(u);
			if (env_.clampOverflow)
			{
				// setupFtReg loads I into a writable temp mapped as VF0 rather
				// than as the I register, so checkVFClamp does not exempt it and
				// I is clamped like any Ft. A VF0 operand is exempt, but clamping
				// (0,0,0,1) changes nothing.
				if (ct & 1)
					Clamp(1);
				if (ct & 2)
					Clamp(0);
			}
			const bool hasAcc = u.kind == FmacKind::Madd || u.kind == FmacKind::Msub;
			if (hasAcc)
				Assemble(in.args, 8, 2, 7);
			switch (u.kind)
			{
				case FmacKind::Add: armAsm->Fadd(V(4).V4S(), V(0).V4S(), V(1).V4S()); break;
				case FmacKind::Sub: armAsm->Fsub(V(4).V4S(), V(0).V4S(), V(1).V4S()); break;
				case FmacKind::Mul: armAsm->Fmul(V(4).V4S(), V(0).V4S(), V(1).V4S()); break;
				case FmacKind::Max: MaxMin(0, 1, true, 4); break;
				case FmacKind::Mini: MaxMin(0, 1, false, 4); break;
				case FmacKind::Madd:
					armAsm->Fmul(V(3).V4S(), V(0).V4S(), V(1).V4S());
					if (u.cls == UClass::FmacB)
						armAsm->Fadd(V(4).V4S(), V(2).V4S(), V(3).V4S()); // ACC + product
					else
						armAsm->Fadd(V(4).V4S(), V(3).V4S(), V(2).V4S()); // product + ACC
					break;
				case FmacKind::Msub:
					armAsm->Fmul(V(3).V4S(), V(0).V4S(), V(1).V4S());
					armAsm->Fsub(V(4).V4S(), V(2).V4S(), V(3).V4S()); // ACC - product
					break;
				default:
					armAsm->Movi(V(4).V4S(), 0);
					break;
			}
			StoreVec(id, 4);
		}

		void Lowerer::Opmul(ValueId id)
		{
			// mVU_OPMULA / mVU_OPMSUB: {Fs.y, Fs.z, Fs.x, Fs.w} * {Ft.z, Ft.x, Ft.y, +0}
			const Inst& in = I(id);
			Assemble(in.args, 0, 0, 7);
			Assemble(in.args, 4, 1, 7);
			armAsm->Mov(V(2).V16B(), V(0).V16B());
			armAsm->Ins(V(0).V4S(), 0, V(2).V4S(), 1);
			armAsm->Ins(V(0).V4S(), 1, V(2).V4S(), 2);
			armAsm->Ins(V(0).V4S(), 2, V(2).V4S(), 0);
			armAsm->Mov(V(2).V16B(), V(1).V16B());
			armAsm->Ins(V(1).V4S(), 0, V(2).V4S(), 2);
			armAsm->Ins(V(1).V4S(), 1, V(2).V4S(), 0);
			armAsm->Ins(V(1).V4S(), 2, V(2).V4S(), 1);
			armAsm->Ins(V(1).V4S(), 3, a64::wzr);
			if (in.op == Op::Opmula)
			{
				armAsm->Fmul(V(4).V4S(), V(0).V4S(), V(1).V4S());
			}
			else
			{
				armAsm->Fmul(V(3).V4S(), V(0).V4S(), V(1).V4S());
				Assemble(in.args, 8, 2, 7);
				armAsm->Fsub(V(4).V4S(), V(2).V4S(), V(3).V4S());
			}
			StoreVec(id, 4);
		}

		void Lowerer::Fmac2(ValueId id)
		{
			const Inst& in = I(id);
			Assemble(in.args, 0, 0, 7);
			const UOp op = static_cast<UOp>(in.sub);
			const float* ftoi[4] = {nullptr, mVUglob.FTOI_4, mVUglob.FTOI_12, mVUglob.FTOI_15};
			const float* itof[4] = {nullptr, mVUglob.ITOF_4, mVUglob.ITOF_12, mVUglob.ITOF_15};
			switch (op)
			{
				case UOp::ABS:
					armAsm->Fabs(V(4).V4S(), V(0).V4S());
					break;
				case UOp::FTOI0: case UOp::FTOI4: case UOp::FTOI12: case UOp::FTOI15:
				{
					// mVU_FTOIx: scale, NaN lanes saturate by sign, Fcvtzs.
					const int k = static_cast<int>(op) - static_cast<int>(UOp::FTOI0);
					if (ftoi[k])
					{
						armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(ftoi[k])));
						armAsm->Fmul(V(0).V4S(), V(0).V4S(), V(29).V4S());
					}
					armAsm->Fcmeq(V(5).V4S(), V(0).V4S(), V(0).V4S());
					armAsm->Sshr(V(6).V4S(), V(0).V4S(), 31);
					armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->Eor(V(6).V16B(), V(6).V16B(), V(29).V16B());
					armAsm->Fcvtzs(V(4).V4S(), V(0).V4S());
					armAsm->Bif(V(4).V16B(), V(6).V16B(), V(5).V16B());
					break;
				}
				default:
				{
					const int k = static_cast<int>(op) - static_cast<int>(UOp::ITOF0);
					armAsm->Scvtf(V(4).V4S(), V(0).V4S());
					if (itof[k])
					{
						armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(itof[k])));
						armAsm->Fmul(V(4).V4S(), V(4).V4S(), V(29).V4S());
					}
					break;
				}
			}
			StoreVec(id, 4);
		}

		void Lowerer::ClipBits(ValueId id)
		{
			// mVU_CLIP
			const Inst& in = I(id);
			std::vector<ValueId> fs = {in.args[0], in.args[1], in.args[2], 0};
			Assemble(fs, 0, 0, 7);
			LoadVec(in.args[3], 1);
			armAsm->Dup(V(1).V4S(), V(1).V4S(), 3);
			armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(&mVUglob.exponent[0])));
			armAsm->And(V(2).V16B(), V(0).V16B(), V(29).V16B());
			armAsm->Cmeq(V(2).V4S(), V(2).V4S(), 0);
			armAsm->Bic(V(2).V16B(), V(0).V16B(), V(2).V16B());
			armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
			armAsm->And(V(1).V16B(), V(1).V16B(), V(29).V16B());
			armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(&mVUglob.signbit[0])));
			armAsm->Eor(V(0).V16B(), V(2).V16B(), V(29).V16B());
			armAsm->Cmgt(V(2).V4S(), V(2).V4S(), V(1).V4S());
			armAsm->Cmgt(V(0).V4S(), V(0).V4S(), V(1).V4S());
			armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(&mVUglob.clipWeights[0])));
			armAsm->Uzp1(V(2).V8H(), V(2).V8H(), V(0).V8H());
			armAsm->And(V(2).V16B(), V(2).V16B(), V(29).V16B());
			armAsm->Addv(a64::VRegister(2, 16), V(2).V8H());
			armAsm->Umov(W(10), V(2).V8H(), 0);
			StoreW(id, 10);
		}

		void Lowerer::FlagRead(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const u32 imm = static_cast<u32>(in.imm);
			LoadInt(in.args[0], 10);
			const a64::Register w = W(10);
			switch (op)
			{
				case LOp::FCAND:
					if (imm)
					{
						armAsm->Tst(w, imm);
						armAsm->Cset(w, a64::ne);
					}
					else
						armAsm->Mov(w, 0);
					break;
				case LOp::FCEQ:
					armAsm->Mov(W(9), imm);
					armAsm->Eor(w, w, W(9));
					armAsm->Sub(w, w, 1);
					armAsm->Lsr(w, w, 31);
					break;
				case LOp::FCOR:
					armAsm->Mov(W(9), imm);
					armAsm->Orr(w, w, W(9));
					armAsm->Add(w, w, 1);
					armAsm->Lsr(w, w, 24);
					break;
				case LOp::FCGET:
					armAsm->And(w, w, 0xfff);
					break;
				case LOp::FMAND:
				case LOp::FMEQ:
				case LOp::FMOR:
					LoadInt(in.args[1], 11);
					if (op == LOp::FMAND)
						armAsm->And(w, W(11), w);
					else if (op == LOp::FMOR)
						armAsm->Orr(w, W(11), w);
					else
					{
						armAsm->Eor(w, W(11), w);
						armAsm->Sub(w, w, 1);
						armAsm->Lsr(w, w, 31);
					}
					break;
				default:
				{
					// mVUallocSFLAGc: denormalized status -> architectural form
					const a64::Register t = W(11);
					armAsm->Mov(t, 0);
					const auto bit = [&](u32 test, u32 set) {
						a64::Label skip;
						armAsm->Tst(w, test);
						armAsm->B(&skip, a64::eq);
						armAsm->Orr(t, t, set);
						armAsm->Bind(&skip);
					};
					bit(0x0f00, 0x0001);
					bit(0xf000, 0x0002);
					bit(0x000f, 0x0040);
					bit(0x00f0, 0x0080);
					armAsm->And(w, w, 0xffff0000u);
					armAsm->Lsr(w, w, 14);
					armAsm->Orr(w, t, w);
					if (op == LOp::FSAND)
					{
						if (imm)
						{
							armAsm->Mov(W(9), imm);
							armAsm->And(w, w, W(9));
						}
						else
							armAsm->Mov(w, 0);
					}
					else if (op == LOp::FSOR)
					{
						if (imm)
						{
							armAsm->Mov(W(9), imm);
							armAsm->Orr(w, w, W(9));
						}
					}
					else // FSEQ
					{
						if (imm)
						{
							armAsm->Mov(W(9), imm);
							armAsm->Eor(w, w, W(9));
						}
						armAsm->Sub(w, w, 1);
						armAsm->Lsr(w, w, 31);
					}
					break;
				}
			}
			StoreW(id, 10);
		}

		void Lowerer::Div(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const LowerInst l = DecodeLower(in.word);
			const bool skipClamp = in.imm & 1;
			// operands into lane 0 of v0 (fs) and v1 (ft)
			if (op != LOp::SQRT)
			{
				LoadVec(in.args[0], 0);
				if (I(in.args[0]).type == Type::Vec && l.fsf)
					armAsm->Dup(V(0).V4S(), V(0).V4S(), l.fsf);
			}
			LoadVec(in.args[1], 1);
			if (I(in.args[1]).type == Type::Vec && l.ftf)
				armAsm->Dup(V(1).V4S(), V(1).V4S(), l.ftf);

			a64::Label done, normal;
			const auto quotient = [&](int divisor) {
				// Fdiv, the mode-1 single-lane clamp unless the dividend came from
				// VF0, and the software flush when the FPCR does not flush.
				if (env_.softwareFlush)
					armAsm->Umov(W(10), V(0).V4S(), 0);
				armAsm->Fdiv(S(4), S(0), S(divisor));
				if (!skipClamp && env_.clampOverflow)
					ClampS(4);
				if (env_.softwareFlush)
				{
					armAsm->Umov(W(9), V(4).V4S(), 0);
					armAsm->And(W(11), W(9), 0x80000000);
					armAsm->Tst(W(9), kExpMask);
					armAsm->Csel(W(9), W(11), W(9), a64::eq);
					armAsm->Tst(W(10), kExpMask);
					armAsm->Csel(W(9), W(11), W(9), a64::eq);
					armAsm->Fmov(S(4), W(9));
				}
			};
			const auto zeroDivisorQ = [&](bool fromBoth) {
				// sign(Fs) [xor sign(Ft)] | maxvals
				armAsm->Umov(W(9), V(0).V4S(), 0);
				if (fromBoth)
				{
					armAsm->Umov(W(10), V(1).V4S(), 0);
					armAsm->Eor(W(9), W(9), W(10));
				}
				armAsm->And(W(9), W(9), 0x80000000);
				armAsm->Orr(W(9), W(9), 0x7f7fffff);
				armAsm->Fmov(S(4), W(9));
			};

			switch (op)
			{
				case LOp::DIV:
					armAsm->Umov(W(9), V(1).V4S(), 0);
					armAsm->Tst(W(9), kExpMask);
					armAsm->B(&normal, a64::ne);
					zeroDivisorQ(true);
					armAsm->B(&done);
					armAsm->Bind(&normal);
					quotient(1);
					armAsm->Bind(&done);
					break;
				case LOp::SQRT:
				{
					a64::Label notNeg;
					armAsm->Umov(W(9), V(1).V4S(), 0);
					armAsm->Tbz(W(9), 31, &notNeg);
					armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->And(V(1).V16B(), V(1).V16B(), V(29).V16B());
					armAsm->Bind(&notNeg);
					if (env_.softwareFlush)
						armAsm->Umov(W(10), V(1).V4S(), 0);
					if (env_.clampOverflow)
						armAsm->Fminnm(S(1), S(1), a64::VRegister(qCMax.GetCode(), 32));
					armAsm->Fsqrt(S(4), S(1));
					if (env_.softwareFlush)
					{
						armAsm->Umov(W(9), V(4).V4S(), 0);
						armAsm->Tst(W(10), kExpMask);
						armAsm->Csel(W(9), a64::wzr, W(9), a64::eq);
						armAsm->Fmov(S(4), W(9));
					}
					break;
				}
				default: // RSQRT
				{
					a64::Label notNeg;
					armAsm->Umov(W(9), V(1).V4S(), 0);
					armAsm->Tbz(W(9), 31, &notNeg);
					armAsm->Ldr(V(29), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->And(V(1).V16B(), V(1).V16B(), V(29).V16B());
					armAsm->Bind(&notNeg);
					armAsm->Umov(W(12), V(1).V4S(), 0);
					armAsm->Fsqrt(S(1), S(1));
					armAsm->Tst(W(12), kExpMask);
					armAsm->B(&normal, a64::ne);
					zeroDivisorQ(false);
					armAsm->B(&done);
					armAsm->Bind(&normal);
					quotient(1);
					armAsm->Bind(&done);
					break;
				}
			}
			StoreS(id, 4);
		}

		void Lowerer::DivFlags(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const LowerInst l = DecodeLower(in.word);
			if (op != LOp::SQRT)
				LoadLaneWord(in.args[0], l.fsf, 12);
			LoadLaneWord(in.args[1], l.ftf, 13);
			const a64::Register f = W(10);
			armAsm->Mov(f, 0);
			if (op == LOp::SQRT || op == LOp::RSQRT)
			{
				a64::Label pos;
				armAsm->Tbz(W(13), 31, &pos);
				armAsm->Mov(f, kDivI);
				armAsm->Bind(&pos);
			}
			if (op == LOp::DIV || op == LOp::RSQRT)
			{
				a64::Label nonzero;
				armAsm->Tst(W(13), kExpMask);
				armAsm->B(&nonzero, a64::ne);
				armAsm->Mov(W(9), kDivD);
				armAsm->Mov(W(11), kDivI);
				armAsm->Tst(W(12), kExpMask);
				armAsm->Csel(W(9), W(11), W(9), a64::eq);
				armAsm->Orr(f, f, W(9));
				armAsm->Bind(&nonzero);
			}
			StoreW(id, 10);
		}

		void Lowerer::Efu(ValueId id)
		{
			const Inst& in = I(id);
			const LOp op = static_cast<LOp>(in.sub);
			const LowerInst l = DecodeLower(in.word);
			// Scalars on lane 0 (NEON_*SS without clamps at mode 1).
			const auto mulC = [&](int r, const void* c) {
				armAsm->Ldr(S(30), a64::MemOperand(xGlob, G(c)));
				armAsm->Fmul(S(r), S(r), S(30));
			};
			const auto addC = [&](int r, const void* c) {
				armAsm->Ldr(S(30), a64::MemOperand(xGlob, G(c)));
				armAsm->Fadd(S(r), S(r), S(30));
			};
			const auto subC = [&](int r, const void* c) {
				armAsm->Ldr(S(30), a64::MemOperand(xGlob, G(c)));
				armAsm->Fsub(S(r), S(r), S(30));
			};
			const auto mul = [&](int to, int from) { armAsm->Fmul(S(to), S(to), S(from)); };
			const auto add = [&](int to, int from) { armAsm->Fadd(S(to), S(to), S(from)); };
			const auto sub = [&](int to, int from) { armAsm->Fsub(S(to), S(to), S(from)); };
			const auto div = [&](int to, int from) { armAsm->Fdiv(S(to), S(to), S(from)); };
			const auto mov = [&](int dst, int src) { armAsm->Mov(V(dst).V16B(), V(src).V16B()); };
			const auto one = [&](int r) { armAsm->Ldr(S(r), a64::MemOperand(xGlob, G(&mVUglob.one[0]))); };
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
				const u32* c[7] = {mVUglob.T2, mVUglob.T3, mVUglob.T4, mVUglob.T5, mVUglob.T6, mVUglob.T7, mVUglob.T8};
				for (const u32* k : c)
				{
					mul(t2, fs);
					mul(t2, fs);
					mov(t1, t2);
					mulC(t1, k);
					add(pq, t1);
				}
				addC(pq, &mVUglob.Pi4[0]);
			};

			// Operand: the EFU1 forms read lane fsf into lane 0; the EFU2 forms
			// take the whole register.
			const bool efu1 = op == LOp::EATAN || op == LOp::EEXP || op == LOp::ERCPR || op == LOp::ERSQRT ||
							  op == LOp::ESIN || op == LOp::ESQRT;
			if (efu1)
			{
				LoadVec(in.args[0], 0);
				if (I(in.args[0]).type == Type::Vec && l.fsf)
					armAsm->Dup(V(0).V4S(), V(0).V4S(), l.fsf);
			}
			else
				Assemble(in.args, 0, 0, 7);

			int res = 1;
			switch (op)
			{
				case LOp::EATAN:
					armAsm->Ins(V(1).V4S(), 0, V(0).V4S(), 0);
					subC(0, &mVUglob.one[0]);
					addC(1, &mVUglob.one[0]);
					div(0, 1);
					eatan(1, 0, 2, 3);
					res = 1;
					break;
				case LOp::EATANxy:
				case LOp::EATANxz:
					// t1 = v0 (whole Fs), Fs' = v4
					armAsm->Ins(V(4).V4S(), 0, V(0).V4S(), op == LOp::EATANxy ? 1 : 2);
					armAsm->Ins(V(1).V4S(), 0, V(4).V4S(), 0);
					sub(4, 0);
					add(0, 1);
					div(4, 0);
					eatan(1, 4, 0, 3);
					res = 1;
					break;
				case LOp::EEXP:
					armAsm->Ins(V(1).V4S(), 0, V(0).V4S(), 0);
					mulC(1, &mVUglob.E1[0]);
					addC(1, &mVUglob.one[0]);
					mov(2, 0);
					mul(2, 0);
					mov(3, 2);
					mulC(2, &mVUglob.E2[0]);
					add(1, 2);
					for (const u32* k : {mVUglob.E3, mVUglob.E4, mVUglob.E5})
					{
						mul(3, 0);
						mov(2, 3);
						mulC(2, k);
						add(1, 2);
					}
					mul(3, 0);
					mulC(3, &mVUglob.E6[0]);
					add(1, 3);
					mul(1, 1);
					mul(1, 1);
					one(3);
					div(3, 1);
					res = 3;
					break;
				case LOp::ELENG:
					sumXYZ(1, 0);
					armAsm->Fsqrt(S(1), S(1));
					res = 1;
					break;
				case LOp::ERCPR:
					armAsm->Ins(V(1).V4S(), 0, V(0).V4S(), 0);
					one(0);
					div(0, 1);
					res = 0;
					break;
				case LOp::ERLENG:
					sumXYZ(1, 0);
					armAsm->Fsqrt(S(1), S(1));
					one(0);
					div(0, 1);
					res = 0;
					break;
				case LOp::ERSADD:
					sumXYZ(1, 0);
					one(0);
					div(0, 1);
					res = 0;
					break;
				case LOp::ERSQRT:
					armAsm->Ldr(V(30), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->And(V(0).V16B(), V(0).V16B(), V(30).V16B());
					armAsm->Fsqrt(S(1), S(0));
					one(0);
					div(0, 1);
					res = 0;
					break;
				case LOp::ESADD:
					sumXYZ(0, 0);
					res = 0;
					break;
				case LOp::ESIN:
					armAsm->Ins(V(1).V4S(), 0, V(0).V4S(), 0); // pq = X
					mul(0, 0);                                  // Fs = X^2
					mov(2, 0);                                  // t1 = X^2
					mul(0, 1);                                  // Fs = X^3
					mov(3, 0);                                  // t2 = X^3
					mulC(0, &mVUglob.S2[0]);
					add(1, 0);
					mul(3, 2);
					mov(0, 3);
					mulC(0, &mVUglob.S3[0]);
					add(1, 0);
					mul(3, 2);
					mov(0, 3);
					mulC(0, &mVUglob.S4[0]);
					add(1, 0);
					mul(3, 2);
					mulC(3, &mVUglob.S5[0]);
					add(1, 3);
					res = 1;
					break;
				case LOp::ESQRT:
					armAsm->Ldr(V(30), a64::MemOperand(xGlob, G(&mVUglob.absclip[0])));
					armAsm->And(V(0).V16B(), V(0).V16B(), V(30).V16B());
					armAsm->Fsqrt(S(0), S(0));
					res = 0;
					break;
				case LOp::ESUM:
					armAsm->Rev64(V(1).V4S(), V(0).V4S());
					armAsm->Ext(V(1).V16B(), V(1).V16B(), V(1).V16B(), 8);
					armAsm->Fadd(V(0).V4S(), V(0).V4S(), V(1).V4S());
					armAsm->Ins(V(1).V4S(), 0, V(0).V4S(), 1);
					add(0, 1);
					res = 0;
					break;
				default:
					break;
			}
			StoreS(id, res);
		}

		void Lowerer::Kick(ValueId id)
		{
			// mVU_XGKICK_DELAY: the transfer is a C call reading VU memory.
			const Inst& in = I(id);
			LoadInt(in.args[1], 0);
			armAsm->Sub(a64::sp, a64::sp, 16);
			armAsm->Str(qPQ, a64::MemOperand(a64::sp));
			armEmitCall(env_.xgkickFn);
			armAsm->Ldr(qPQ, a64::MemOperand(a64::sp));
			armAsm->Add(a64::sp, a64::sp, 16);
			if (env_.clampOverflow)
				armAsm->Ldp(qCMin, qCMax, a64::MemOperand(xGlob, G(&mVUglob.minvals[0])));
		}

		void Lowerer::Leave(ValueId id)
		{
			const Inst& in = I(id);
			const auto isOwnLiveIn = [&](u32 v, ValueId val) {
				const Inst& x = I(val);
				return x.op == Op::LiveIn && x.var == v;
			};
			// Capture cross-variable LiveIns while every home still holds its
			// incoming value, then let the loads below read the copies.
			std::vector<ValueId> staged;
			for (u32 var = 0; var < var::Count; var++)
			{
				const ValueId a = in.args[var];
				if (!CrossLiveIn(var, a) || std::find(staged.begin(), staged.end(), a) != staged.end())
					continue;
				staged.push_back(a);
				if (I(a).type == Type::Vec)
				{
					LoadVec(a, 0);
					armAsm->Str(V(0), a64::MemOperand(xSlot, static_cast<int64_t>(staged_[a]) * 16));
				}
				else
				{
					LoadInt(a, 10);
					armAsm->Str(W(10), a64::MemOperand(xSlot, static_cast<int64_t>(staged_[a]) * 16));
				}
			}
			useStaged_ = true;
			// VF and ACC: whole-register store when every lane changed to one
			// value, else load, insert the changed lanes, store.
			for (u32 r = 1; r <= 32; r++)
			{
				const s32 home = (r == 32) ? kOffACC : kOffVF + static_cast<s32>(r) * 16;
				ValueId lanes[4];
				int dirty = 0;
				for (int l = 0; l < 4; l++)
				{
					const u32 v = (r == 32) ? var::ACC(l) : var::VF(r, l);
					lanes[l] = in.args[v];
					if (lanes[l] && !isOwnLiveIn(v, lanes[l]))
						dirty |= 1 << l;
					else
						lanes[l] = 0;
				}
				if (!dirty)
					continue;
				if (dirty == 0xf && lanes[0] == lanes[1] && lanes[0] == lanes[2] && lanes[0] == lanes[3])
				{
					LoadVec(lanes[0], 0);
					armAsm->Str(V(0), a64::MemOperand(xState, home));
					continue;
				}
				armAsm->Ldr(V(0), a64::MemOperand(xState, home));
				for (int l = 0; l < 4; l++)
				{
					if (!lanes[l])
						continue;
					LoadVec(lanes[l], 1);
					armAsm->Ins(V(0).V4S(), l, V(1).V4S(), l);
				}
				armAsm->Str(V(0), a64::MemOperand(xState, home));
			}
			for (u32 r = 1; r <= 15; r++)
			{
				const ValueId v = in.args[var::VI(r)];
				if (!v || isOwnLiveIn(var::VI(r), v))
					continue;
				LoadInt(v, 10);
				armAsm->Strh(W(10), a64::MemOperand(xState, kOffVI + static_cast<s32>(r) * kViStride));
			}
			const auto storeWord = [&](u32 var, const a64::MemOperand& mem) {
				const ValueId v = in.args[var];
				if (!v || isOwnLiveIn(var, v))
					return;
				LoadInt(v, 10);
				armAsm->Str(W(10), mem);
			};
			storeWord(var::I, a64::MemOperand(xState, kOffVI + REG_I * kViStride));
			storeWord(var::R, a64::MemOperand(xState, kOffVI + REG_R * kViStride));
			for (u32 k = 0; k < 4; k++)
			{
				storeWord(var::M(k), a64::MemOperand(xFlag, static_cast<s32>(k) * 4));
				storeWord(var::C(k), a64::MemOperand(xFlag, 16 + static_cast<s32>(k) * 4));
			}
			storeWord(var::DIVFLAG, a64::MemOperand(xFlag, env_.offDivFlag));
			storeWord(var::VIBACKUP, a64::MemOperand(xFlag, env_.offVIbackup));
			storeWord(var::XGKADDR, a64::MemOperand(xFlag, env_.offVIxgkick));
			for (u32 k = 0; k < 4; k++)
			{
				const u32 var = var::QL0 + k;
				const ValueId v = in.args[var];
				if (!v || isOwnLiveIn(var, v))
					continue;
				LoadInt(v, 10);
				armAsm->Ins(qPQ.V4S(), static_cast<int>(k), W(10));
			}
			// Status instances live in w20-w23; read everything before writing.
			int tmp = 12;
			int pending[4] = {-1, -1, -1, -1};
			for (u32 k = 0; k < 4; k++)
			{
				const ValueId v = in.args[var::S(k)];
				if (!v || isOwnLiveIn(var::S(k), v))
					continue;
				LoadInt(v, tmp);
				pending[k] = tmp++;
			}
			for (u32 k = 0; k < 4; k++)
				if (pending[k] >= 0)
					armAsm->Mov(W(20 + static_cast<int>(k)), W(pending[k]));
			// The value the branch tail compares, or the jump target.
			const ValueId cond = in.args[var::Count];
			const ValueId jump = in.args[var::Count + 1];
			if (cond)
			{
				LoadInt(cond, 10);
				armAsm->Str(W(10), a64::MemOperand(xFlag, env_.offBranch));
			}
			if (jump)
			{
				LoadInt(jump, 10);
				armAsm->Lsl(W(10), W(10), 3);
				armAsm->And(W(10), W(10), 0x4000 - 8);
				armAsm->Str(W(10), a64::MemOperand(xFlag, env_.offBranch));
			}
		}

		void Lowerer::Emit()
		{
			armMoveAddressToReg(xSlot, s_slotArea[env_.vuIndex ? 1 : 0]);
			for (ValueId id : f_.blocks[node_].insts)
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
						PackFlags(in.args[0], in.mask, true, 10);
						StoreW(id, 10);
						break;
					case Op::StatusUpd:
					{
						const bool nonSticky = in.imm & 1;
						PackFlags(in.args[1], in.mask, (in.imm & 2) != 0, 10);
						LoadInt(in.args[0], 11);
						if (nonSticky)
							armAsm->And(W(11), W(11), 0xfffc00ffu);
						armAsm->Orr(W(11), W(11), W(10));
						if (nonSticky)
							armAsm->Orr(W(11), W(11), a64::Operand(W(10), a64::LSL, 8));
						StoreW(id, 11);
						break;
					}
					case Op::StatusDiv:
						LoadInt(in.args[0], 11);
						LoadInt(in.args[1], 10);
						armAsm->Bic(W(11), W(11), 0x000C0000u);
						armAsm->Orr(W(11), W(11), W(10));
						StoreW(id, 11);
						break;
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
						LoadInt(in.args[0], 11);
						armAsm->Mov(W(9), 0xfff00u);
						armAsm->And(W(11), W(11), W(9));
						if (imm)
						{
							armAsm->Mov(W(9), imm);
							armAsm->Orr(W(11), W(11), W(9));
						}
						StoreW(id, 11);
						break;
					}
					case Op::ClipUpd:
						LoadInt(in.args[0], 11);
						LoadInt(in.args[1], 10);
						armAsm->Lsl(W(11), W(11), 6);
						armAsm->And(W(11), W(11), 0xffffff);
						armAsm->Orr(W(11), W(11), W(10));
						StoreW(id, 11);
						break;
					case Op::ClipSet:
						armAsm->Mov(W(10), static_cast<u32>(in.imm));
						StoreW(id, 10);
						break;
					case Op::FlagRead: FlagRead(id); break;
					case Op::IAlu:
					{
						const LOp op = static_cast<LOp>(in.sub);
						LoadInt(in.args[0], 10);
						switch (op)
						{
							case LOp::IADD: LoadInt(in.args[1], 11); armAsm->Add(W(10), W(10), W(11)); break;
							case LOp::ISUB: LoadInt(in.args[1], 11); armAsm->Sub(W(10), W(10), W(11)); break;
							case LOp::IAND: LoadInt(in.args[1], 11); armAsm->And(W(10), W(10), W(11)); break;
							case LOp::IOR: LoadInt(in.args[1], 11); armAsm->Orr(W(10), W(10), W(11)); break;
							case LOp::ISUBIU:
								armAsm->Mov(W(11), static_cast<u32>(in.imm));
								armAsm->Sub(W(10), W(10), W(11));
								break;
							default: // IADDI / IADDIU
								armAsm->Mov(W(11), static_cast<u32>(in.imm));
								armAsm->Add(W(10), W(10), W(11));
								break;
						}
						StoreW(id, 10);
						break;
					}
					case Op::IncVI:
						LoadInt(in.args[0], 10);
						if (in.imm > 0)
							armAsm->Add(W(10), W(10), 1);
						else
							armAsm->Sub(W(10), W(10), 1);
						StoreW(id, 10);
						break;
					case Op::Mtir:
					{
						const LowerInst l = DecodeLower(in.word);
						LoadLaneWord(in.args[0], l.fsf, 10);
						StoreW(id, 10);
						break;
					}
					case Op::Xtop:
						armMoveAddressToReg(a64::x9, static_cast<LOp>(in.sub) == LOp::XTOP ? env_.vifTop : env_.vifItop);
						armAsm->Ldrh(W(10), a64::MemOperand(a64::x9));
						if (static_cast<LOp>(in.sub) == LOp::XITOP)
							armAsm->And(W(10), W(10), 0x3ff);
						StoreW(id, 10);
						break;
					case Op::LoadQ:
						MemAddr(in.args[1], in.imm, 9);
						armAsm->Ldr(V(4), a64::MemOperand(a64::x9, kVU1MemFromState));
						StoreVec(id, 4);
						break;
					case Op::LoadI:
						MemAddr(in.args[1], in.imm, 9);
						armAsm->Ldrh(W(10), a64::MemOperand(a64::x9, kVU1MemFromState + static_cast<s32>(VuIlwLaneOffset(in.word))));
						StoreW(id, 10);
						break;
					case Op::StoreQ:
						Assemble(in.args, 2, 0, 7);
						MemAddr(in.args[1], in.imm, 9);
						if (in.mask == 0xf)
							armAsm->Str(V(0), a64::MemOperand(a64::x9, kVU1MemFromState));
						else
						{
							armAsm->Ldr(V(1), a64::MemOperand(a64::x9, kVU1MemFromState));
							for (int l = 0; l < 4; l++)
								if (MaskLane(in.mask, l))
									armAsm->Ins(V(1).V4S(), l, V(0).V4S(), l);
							armAsm->Str(V(1), a64::MemOperand(a64::x9, kVU1MemFromState));
						}
						break;
					case Op::StoreI:
						LoadInt(in.args[2], 10);
						MemAddr(in.args[1], in.imm, 9);
						armAsm->Uxth(W(10), W(10));
						for (int l = 0; l < 4; l++)
							if (MaskLane(in.mask, l))
								armAsm->Str(W(10), a64::MemOperand(a64::x9, kVU1MemFromState + l * 4));
						break;
					case Op::Move:
						Assemble(in.args, 0, 4, 7);
						StoreVec(id, 4);
						break;
					case Op::Mr32:
						Assemble(in.args, 0, 0, 7);
						armAsm->Ext(V(4).V16B(), V(0).V16B(), V(0).V16B(), 4);
						StoreVec(id, 4);
						break;
					case Op::Mfir:
						LoadInt(in.args[0], 10);
						armAsm->Sxth(W(10), W(10));
						armAsm->Dup(V(4).V4S(), W(10));
						StoreVec(id, 4);
						break;
					case Op::Mfp:
						LoadBroadcast(in.args[0], 4);
						StoreVec(id, 4);
						break;
					case Op::Div: Div(id); break;
					case Op::DivFlags: DivFlags(id); break;
					case Op::Efu: Efu(id); break;
					case Op::RUpd:
					{
						const LOp op = static_cast<LOp>(in.sub);
						const LowerInst l = DecodeLower(in.word);
						if (op == LOp::RNEXT)
						{
							LoadInt(in.args[0], 11);
							armAsm->Eor(W(9), W(11), a64::Operand(W(11), a64::LSR, 18));
							armAsm->Lsl(W(11), W(11), 1);
							armAsm->Bfxil(W(11), W(9), 4, 1);
							armAsm->And(W(11), W(11), 0x007fffff);
							armAsm->Orr(W(11), W(11), 0x3f800000);
						}
						else
						{
							LoadLaneWord(in.args[1], l.fsf, 9);
							armAsm->And(W(9), W(9), 0x007fffff);
							if (op == LOp::RINIT)
								armAsm->Orr(W(11), W(9), 0x3f800000);
							else
							{
								LoadInt(in.args[0], 11);
								armAsm->Eor(W(11), W(11), W(9));
							}
						}
						StoreW(id, 11);
						break;
					}
					case Op::RGet:
						LoadInt(in.args[0], 10);
						armAsm->Dup(V(4).V4S(), W(10));
						StoreVec(id, 4);
						break;
					case Op::Kick: Kick(id); break;
					case Op::Cond:
					{
						LoadInt(in.args[0], 10);
						if (in.args.size() > 1 && in.args[1])
						{
							LoadInt(in.args[1], 11);
							armAsm->Eor(W(10), W(10), W(11));
						}
						StoreW(id, 10);
						break;
					}
					case Op::Leave: Leave(id); break;
					default:
						break;
				}
			}
		}
	} // namespace

	LowerResult LowerNodeBody(const ir::Function& f, const LowerEnv& env)
	{
		LowerResult r;
		Lowerer lw(f, env);
		if (!lw.Check(r.why))
			return r;
		const u8* start = armGetCurrentCodePointer();
		lw.Emit();
		r.bytes = static_cast<u32>(armGetCurrentCodePointer() - start);
		r.ok = true;
		return r;
	}
} // namespace vuopt
