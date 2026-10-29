// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuAotIR.h"

#include <algorithm>
#include <memory>
#include <array>
#include <cstdio>
#include <functional>

namespace vuaot::ir
{
	std::string var::Name(u32 v)
	{
		static const char kLane[4] = {'x', 'y', 'z', 'w'};
		char buf[32];
		if (v < 128)
			std::snprintf(buf, sizeof(buf), "vf%02u.%c", v / 4, kLane[v & 3]);
		else if (v < 132)
			std::snprintf(buf, sizeof(buf), "acc.%c", kLane[v - 128]);
		else if (v < 148)
			std::snprintf(buf, sizeof(buf), "vi%02u", v - 132);
		else if (v == QL0) return "q.lane0";
		else if (v == QL1) return "q.lane1";
		else if (v == PL0) return "p.lane0";
		else if (v == PL1) return "p.lane1";
		else if (v == I) return "I";
		else if (v == R) return "R";
		else if (v < 158)
			std::snprintf(buf, sizeof(buf), "status%u", v - 154);
		else if (v < 162)
			std::snprintf(buf, sizeof(buf), "mac%u", v - 158);
		else if (v < 166)
			std::snprintf(buf, sizeof(buf), "clip%u", v - 162);
		else if (v == DIVFLAG) return "divflag";
		else if (v == VIBACKUP) return "vibackup";
		else if (v == XGKADDR) return "xgkaddr";
		else if (v == MEM) return "mem";
		else
			std::snprintf(buf, sizeof(buf), "var%u", v);
		return buf;
	}

	namespace
	{
		bool ValidVar(u32 v)
		{
			if (v < 4) // vf00
				return false;
			if (v == var::VI(0))
				return false;
			return v < var::Count;
		}

		Type VarType(u32 v)
		{
			if (v < 132)
				return Type::Vec;
			if (v < 148)
				return Type::Int;
			if (v <= var::R)
				return Type::Float;
			if (v == var::MEM)
				return Type::Mem;
			return Type::Int;
		}

		bool MaskLane(u8 mask, int lane) { return (mask >> (3 - lane)) & 1; }

		class Builder
		{
		public:
			Builder(const ProgramGraph& g, const BuildOptions& o, bool nodeLocal = false)
				: g_(g)
				, o_(o)
				, nodeLocal_(nodeLocal)
			{
				// A region builds one node at a time, hundreds per compile:
				// node builds reuse the thread's buffers.
				if (nodeLocal_)
				{
					auto& pool = s_pool;
					if (!pool.empty())
					{
						scratch_ = std::move(pool.back());
						pool.pop_back();
					}
					else
						scratch_ = std::make_unique<Scratch>();
					Swap();
					repl_.clear();
					users_.clear();
					pendingVF_.clear();
				}
			}
			~Builder()
			{
				if (nodeLocal_)
				{
					Swap();
					if (s_pool.size() < 16)
						s_pool.push_back(std::move(scratch_));
				}
			}
			Builder(const Builder&) = delete;
			Builder& operator=(const Builder&) = delete;

			Function Build();

		private:
			struct Scratch
			{
				std::vector<std::array<ValueId, var::Count>> cur;
				std::vector<bool> sealed, filled;
				std::vector<std::vector<std::pair<u32, ValueId>>> incomplete;
				std::vector<ValueId> repl, vf0, vi0;
				std::vector<std::vector<ValueId>> users;
				std::vector<std::pair<u32, ValueId>> pendingVF;
			};
			static thread_local std::vector<std::unique_ptr<Scratch>> s_pool;
			std::unique_ptr<Scratch> scratch_;
			void Swap()
			{
				cur_.swap(scratch_->cur);
				sealed_.swap(scratch_->sealed);
				filled_.swap(scratch_->filled);
				incomplete_.swap(scratch_->incomplete);
				repl_.swap(scratch_->repl);
				vf0_.swap(scratch_->vf0);
				vi0_.swap(scratch_->vi0);
				users_.swap(scratch_->users);
				pendingVF_.swap(scratch_->pendingVF);
			}

			const ProgramGraph& g_;
			const BuildOptions& o_;
			const bool nodeLocal_;
			Function f_;

			std::vector<std::array<ValueId, var::Count>> cur_;
			std::vector<bool> sealed_, filled_;
			std::vector<std::vector<std::pair<u32, ValueId>>> incomplete_;
			std::vector<ValueId> repl_;
			std::vector<std::vector<ValueId>> users_;
			std::vector<ValueId> vf0_, vi0_; // per-block constants
			int b_ = -1;                     // block being filled
			u32 pc_ = 0;
			u32 word_ = 0;                   // instruction word of the slot being lowered

			// deferred VF lane writes of the current pair
			std::vector<std::pair<u32, ValueId>> pendingVF_;

			// Clamp modes 2/3: MADD clamps microVU's cached ACC in place. The
			// clamped copy is what the rest of the block reads, and it reaches
			// ACC's home only if the block writes ACC (before or after): a
			// clean cached copy is dropped. Per node.
			bool accWritten_ = false;
			bool accClamped_ = false;
			ValueId accEntry_[4] = {};

			ValueId New(Op op, Type t, int block, bool isPhi = false)
			{
				Inst in;
				in.op = op;
				in.type = t;
				in.block = block;
				in.pc = pc_;
				f_.insts.push_back(std::move(in));
				repl_.push_back(0);
				users_.emplace_back();
				const ValueId id = static_cast<ValueId>(f_.insts.size() - 1);
				if (isPhi)
					f_.blocks[block].phis.push_back(id);
				else
					f_.blocks[block].insts.push_back(id);
				return id;
			}

			ValueId Emit(Op op, Type t, ArgVec args, u16 sub = 0, u8 mask = 0, s32 imm = 0)
			{
				const ValueId id = New(op, t, b_);
				Inst& in = f_.insts[id];
				in.sub = sub;
				in.mask = mask;
				in.imm = imm;
				in.word = word_;
				in.args = std::move(args);
				// Only phi users are tracked: trivial-phi removal is the one
				// consumer, and it only revisits phis.
				return id;
			}

			ValueId Resolve(ValueId v) const
			{
				while (v && repl_[v])
					v = repl_[v];
				return v;
			}

			// SSA construction after Braun et al.
			void WriteVar(u32 v, int block, ValueId val) { cur_[block][v] = val; }

			ValueId ReadVar(u32 v, int block)
			{
				const ValueId d = Resolve(cur_[block][v]);
				if (d)
					return d;
				return ReadVarRecursive(v, block);
			}

			ValueId NewPhi(u32 v, int block)
			{
				const ValueId id = New(Op::Phi, VarType(v), block, true);
				f_.insts[id].var = v;
				f_.insts[id].lane = static_cast<u8>(v < 132 ? (v & 3) : 0);
				f_.phisCreated++;
				return id;
			}

			ValueId ReadVarRecursive(u32 v, int block)
			{
				ValueId val;
				const Block& bl = f_.blocks[block];
				if (!sealed_[block])
				{
					val = NewPhi(v, block);
					incomplete_[block].push_back({v, val});
				}
				else if (bl.preds.empty())
				{
					// Program entry: the incoming architectural state.
					const u32 savedPc = pc_;
					pc_ = 0;
					val = New(Op::LiveIn, VarType(v), block);
					pc_ = savedPc;
					f_.insts[val].var = v;
					f_.insts[val].lane = static_cast<u8>(v < 132 ? (v & 3) : 0);
				}
				else if (bl.preds.size() == 1)
				{
					val = ReadVar(v, bl.preds[0]);
				}
				else
				{
					val = NewPhi(v, block);
					WriteVar(v, block, val);
					val = AddPhiOperands(v, val);
				}
				WriteVar(v, block, val);
				return val;
			}

			ValueId AddPhiOperands(u32 v, ValueId phi)
			{
				const int block = f_.insts[phi].block;
				const std::vector<int>& preds = f_.blocks[block].preds; // blocks do not change during construction
				f_.insts[phi].args.reserve(preds.size());
				for (int p : preds)
				{
					const ValueId op = ReadVar(v, p);
					f_.insts[phi].args.push_back(op);
					users_[op].push_back(phi);
				}
				return TryRemoveTrivialPhi(phi);
			}

			ValueId TryRemoveTrivialPhi(ValueId phi)
			{
				if (f_.insts[phi].dead)
					return Resolve(phi);
				ValueId same = 0;
				for (ValueId op : f_.insts[phi].args)
				{
					op = Resolve(op);
					if (op == same || op == phi)
						continue;
					if (same)
						return phi;
					same = op;
				}
				if (!same)
					return phi; // only self-references: unreachable, keep it
				repl_[phi] = same;
				f_.insts[phi].dead = true;
				f_.phisRemovedTrivial++;
				// users_[phi] is not appended to below: `same` is never `phi`.
				for (size_t k = 0; k < users_[phi].size(); k++)
				{
					const ValueId u = users_[phi][k];
					users_[same].push_back(u);
					if (u != phi && f_.insts[u].op == Op::Phi && !f_.insts[u].dead)
						TryRemoveTrivialPhi(u);
				}
				return same;
			}

			void Seal(int block)
			{
				for (auto& [v, phi] : incomplete_[block])
					AddPhiOperands(v, phi);
				incomplete_[block].clear();
				sealed_[block] = true;
			}

			void RemoveRedundantSccs();

			ValueId Rd(u32 v) { return ReadVar(v, b_); }
			void Wr(u32 v, ValueId x) { WriteVar(v, b_, x); }

			ValueId VF0()
			{
				if (!vf0_[b_])
					vf0_[b_] = Emit(Op::Const, Type::Vec, {}, 0, 0xf, 0);
				return vf0_[b_];
			}
			ValueId VI0()
			{
				if (!vi0_[b_])
					vi0_[b_] = Emit(Op::Const, Type::Int, {}, 0, 0, 0);
				return vi0_[b_];
			}
			ValueId ReadVF(u32 reg, int lane) { return reg ? Rd(var::VF(reg, lane)) : VF0(); }
			ValueId ReadVI(u32 reg, bool backup = false)
			{
				if (backup)
					return Rd(var::VIBACKUP);
				return reg ? Rd(var::VI(reg)) : VI0();
			}
			void DeferVF(u32 reg, u8 mask, ValueId v)
			{
				if (!reg)
					return;
				for (int l = 0; l < 4; l++)
					if (MaskLane(mask, l))
						pendingVF_.push_back({var::VF(reg, l), v});
			}
			void Lanes(ArgVec& args, u32 reg, u8 mask)
			{
				for (int l = 0; l < 4; l++)
					args.push_back(MaskLane(mask, l) ? ReadVF(reg, l) : 0);
			}

			void WriteVI(const MvuOp& o, u32 reg, ValueId val)
			{
				if (!reg)
					return;
				if (o.lOp.backupVI && reg == o.lOp.VI_write.reg)
					Wr(var::VIBACKUP, Rd(var::VI(reg)));
				Wr(var::VI(reg), val);
			}

			// The variables this block has read or written, each at its current
			// value; 0 for the rest (still their incoming values, which a node's
			// end leaves alone). Keeps a node's IR to what it touches.
			std::vector<ValueId> TouchedVars()
			{
				std::vector<ValueId> a(var::Count, 0);
				for (u32 v = 0; v < var::Count; v++)
					if (ValidVar(v))
						a[v] = Resolve(cur_[b_][v]);
				return a;
			}

			std::vector<ValueId> AllVars()
			{
				std::vector<ValueId> a(var::Count, 0);
				for (u32 v = 0; v < var::Count; v++)
					if (ValidVar(v))
						a[v] = Rd(v);
				return a;
			}

			void Unsupported(const char* why)
			{
				if (!f_.unsupported)
				{
					f_.unsupported = true;
					f_.unsupportedWhy = why;
				}
			}

			void Upper(const MvuOp& o, const InstPair& p);
			void Lower(const MvuOp& o, const InstPair& p, ValueId& cond, ValueId& jumpVal);
			void DivSet(const MvuOp& o);
			void FlushVF();
			void LinkRemap(const BlockResult& b);
			void FillNode(int blockId, const GraphNode& n);
		};

		void Builder::FlushVF()
		{
			for (auto& [v, val] : pendingVF_)
				Wr(v, val);
			pendingVF_.clear();
		}

		void Builder::DivSet(const MvuOp& o)
		{
			if (!o.doDivFlag)
				return;
			const ValueId base = o.sFlag.doFlag ? Rd(var::S(o.sFlag.write)) : Rd(var::S(o.sFlag.lastWrite));
			Wr(var::S(o.sFlag.write), Emit(Op::StatusDiv, Type::Int, {base, Rd(var::DIVFLAG)}));
		}

		void Builder::Upper(const MvuOp& o, const InstPair& p)
		{
			const UpperInst& u = p.upper;
			const u16 sub = static_cast<u16>(u.op);
			ValueId result = 0;
			word_ = u.word;
			switch (u.cls)
			{
				case UClass::FmacA:
				case UClass::FmacB:
				case UClass::FmacC:
				case UClass::FmacD:
				{
					ArgVec a;
					Lanes(a, u.fs, u.mask);
					switch (u.form)
					{
						case FtForm::Reg: Lanes(a, u.ft, u.mask); break;
						case FtForm::Bc: a.push_back(ReadVF(u.ft, u.bc)); a.insert(a.end(), 3, 0); break;
						case FtForm::I: a.push_back(Rd(var::I)); a.insert(a.end(), 3, 0); break;
						case FtForm::Q: a.push_back(Rd(o.readQ ? var::QL1 : var::QL0)); a.insert(a.end(), 3, 0); break;
						default: a.insert(a.end(), 4, 0); break;
					}
					const bool acc = u.kind == FmacKind::Madd || u.kind == FmacKind::Msub;
					if (u.cls == UClass::FmacC && o_.clampExtra)
					{
						// mVU_FMACc reads ACC through a read-only mapping and its
						// clamp step clamps that register in place: every lane, or
						// the one lane of a single-lane op.
						const bool single = u.mask == 8 || u.mask == 4 || u.mask == 2 || u.mask == 1;
						const u8 cl = single ? u.mask : 0xf;
						if (!accWritten_ && !accClamped_)
							for (int l = 0; l < 4; l++)
								accEntry_[l] = Rd(var::ACC(l));
						ArgVec av(4, 0);
						for (int l = 0; l < 4; l++)
							if (MaskLane(cl, l))
								av[l] = Rd(var::ACC(l));
						const ValueId c = Emit(Op::AccClamp, Type::Vec, std::move(av), 0, cl);
						for (int l = 0; l < 4; l++)
							if (MaskLane(cl, l))
								Wr(var::ACC(l), c);
						accClamped_ = true;
					}
					for (int l = 0; l < 4; l++)
						a.push_back(acc && MaskLane(u.mask, l) ? Rd(var::ACC(l)) : 0);
					// microVU's doSafeSub: SUB/SUBA with Ft == Fs writes +0.
					const bool safeSub = u.form == FtForm::Reg && u.kind == FmacKind::Sub && u.ft == u.fs &&
										 (u.op == UOp::SUB || u.op == UOp::SUBA);
					result = Emit(Op::Fmac, Type::Vec, std::move(a), sub, u.mask, safeSub ? 1 : 0);
					f_.insts[result].lane = u.bc;
					if (u.isAcc)
					{
						accWritten_ = true;
						for (int l = 0; l < 4; l++)
							if (MaskLane(u.mask, l))
								Wr(var::ACC(l), result);
					}
					else
						DeferVF(u.fd, u.mask, result);
					break;
				}
				case UClass::Opmula:
				case UClass::Opmsub:
				{
					// lane l of the result reads Fs lane (l+1)%3 and Ft lane (l+2)%3 (xyz only)
					ArgVec fs(4, 0), ft(4, 0);
					for (int l = 0; l < 4; l++)
					{
						if (!MaskLane(u.mask, l))
							continue;
						// lane w multiplies Fs.w by a hard +0: no Ft read
						const int sl = (l == 3) ? 3 : (l + 1) % 3;
						fs[sl] = ReadVF(u.fs, sl);
						if (l != 3)
							ft[(l + 2) % 3] = ReadVF(u.ft, (l + 2) % 3);
					}
					ArgVec a = fs;
					a.insert(a.end(), ft.begin(), ft.end());
					if (u.cls == UClass::Opmsub)
						for (int l = 0; l < 4; l++)
							a.push_back(MaskLane(u.mask, l) ? Rd(var::ACC(l)) : 0);
					result = Emit(u.cls == UClass::Opmula ? Op::Opmula : Op::Opmsub, Type::Vec, std::move(a), sub, u.mask);
					if (u.cls == UClass::Opmula)
					{
						accWritten_ = true;
						for (int l = 0; l < 4; l++)
							if (MaskLane(u.mask, l))
								Wr(var::ACC(l), result);
					}
					else
						DeferVF(u.fd, u.mask, result);
					break;
				}
				case UClass::Fmac2:
				{
					ArgVec a;
					Lanes(a, u.fs, u.mask);
					const ValueId v = Emit(Op::Fmac2, Type::Vec, std::move(a), sub, u.mask);
					DeferVF(u.ft, u.mask, v);
					break;
				}
				case UClass::Clip:
				{
					const ValueId bits = Emit(Op::ClipBits, Type::Int,
						{ReadVF(u.fs, 0), ReadVF(u.fs, 1), ReadVF(u.fs, 2), ReadVF(u.ft, 3)}, sub);
					if (o.cFlag.doFlag)
						Wr(var::C(o.cFlag.write), Emit(Op::ClipUpd, Type::Int, {Rd(var::C(o.cFlag.lastWrite)), bits}));
					break;
				}
				case UClass::Bad:
					// microVU ends the block here and finishes the program as at an
					// E-bit (mVUcheckBadOp); the graph already classifies it so.
					f_.badOps++;
					break;
				case UClass::None:
					break;
			}

			if (result)
			{
				if (o.mFlag.doFlag)
					Wr(var::M(o.mFlag.write), Emit(Op::MacFlags, Type::Int, {result}, 0, u.mask));
				if (o.sFlag.doFlag)
				{
					const ValueId prev = Rd(var::S(o.sFlag.lastWrite));
					if (!o_.dropSticky)
						Wr(var::S(o.sFlag.write), Emit(Op::StatusUpd, Type::Int, {prev, result},
													  0, u.mask, (o.sFlag.doNonSticky ? 1 : 0) | (o.mFlag.doFlag ? 2 : 0)));
					else if (!o.sFlag.doNonSticky)
						Wr(var::S(o.sFlag.write), prev); // (its only effect is the sticky byte)
					else
					{
						// the bits above the sticky byte come from the last instance that set them,
						// which is the one before the update's own run of updates
						const Inst& pi = f_.insts[prev];
						const ValueId hi = pi.op == Op::StatusUpd ? pi.args[0] : prev;
						Wr(var::S(o.sFlag.write), Emit(Op::StatusUpd, Type::Int, {hi, result}, 0, u.mask, 1 | (o.mFlag.doFlag ? 2 : 0)));
					}
				}
			}
		}

		void Builder::Lower(const MvuOp& o, const InstPair& p, ValueId& cond, ValueId& jumpVal)
		{
			const LowerInst& l = p.lower;
			const u16 sub = static_cast<u16>(l.op);
			word_ = l.word;
			const u32 memMask = (o_.vuIndex ? 0x4000u : 0x1000u) - 8;
			const u32 link = ((p.pc + 16) & memMask) / 8;
			switch (l.op)
			{
				case LOp::LQ:
				{
					const ValueId v = Emit(Op::LoadQ, Type::Vec, {Rd(var::MEM), ReadVI(l.is)}, sub, l.mask, l.imm11);
					if (!o.lOp.noWriteVF)
						DeferVF(l.ft, l.mask, v);
					break;
				}
				case LOp::LQI:
				{
					const ValueId addr = ReadVI(l.is);
					const ValueId v = Emit(Op::LoadQ, Type::Vec, {Rd(var::MEM), addr}, sub, l.mask, 0);
					if (!o.lOp.noWriteVF)
						DeferVF(l.ft, l.mask, v);
					if (l.is)
						WriteVI(o, l.is, Emit(Op::IncVI, Type::Int, {addr}, sub, 0, 1));
					break;
				}
				case LOp::LQD:
				{
					const ValueId addr = Emit(Op::IncVI, Type::Int, {ReadVI(l.is)}, sub, 0, -1);
					if (l.is)
						WriteVI(o, l.is, addr);
					const ValueId v = Emit(Op::LoadQ, Type::Vec, {Rd(var::MEM), addr}, sub, l.mask, 0);
					if (!o.lOp.noWriteVF)
						DeferVF(l.ft, l.mask, v);
					break;
				}
				case LOp::SQ:
				case LOp::SQI:
				case LOp::SQD:
				{
					ValueId addr = ReadVI(l.it);
					if (l.op == LOp::SQD)
					{
						addr = Emit(Op::IncVI, Type::Int, {addr}, sub, 0, -1);
						if (l.it)
							WriteVI(o, l.it, addr);
					}
					ArgVec a = {Rd(var::MEM), addr};
					Lanes(a, l.fs, l.mask);
					Wr(var::MEM, Emit(Op::StoreQ, Type::Mem, std::move(a), sub, l.mask, l.op == LOp::SQ ? l.imm11 : 0));
					if (l.op == LOp::SQI && l.it)
						WriteVI(o, l.it, Emit(Op::IncVI, Type::Int, {addr}, sub, 0, 1));
					break;
				}
				case LOp::ILW:
				case LOp::ILWR:
					WriteVI(o, l.it, Emit(Op::LoadI, Type::Int, {Rd(var::MEM), ReadVI(l.is)}, sub, l.mask, l.op == LOp::ILW ? l.imm11 : 0));
					break;
				case LOp::ISW:
				case LOp::ISWR:
					Wr(var::MEM, Emit(Op::StoreI, Type::Mem, {Rd(var::MEM), ReadVI(l.is), ReadVI(l.it)}, sub, l.mask, l.op == LOp::ISW ? l.imm11 : 0));
					break;
				case LOp::IADD:
				case LOp::ISUB:
				case LOp::IAND:
				case LOp::IOR:
					WriteVI(o, l.id, Emit(Op::IAlu, Type::Int, {ReadVI(l.is), ReadVI(l.it)}, sub));
					break;
				case LOp::IADDI:
					WriteVI(o, l.it, Emit(Op::IAlu, Type::Int, {ReadVI(l.is)}, sub, 0, l.imm5));
					break;
				case LOp::IADDIU:
				case LOp::ISUBIU:
					WriteVI(o, l.it, Emit(Op::IAlu, Type::Int, {ReadVI(l.is)}, sub, 0, static_cast<s32>(l.imm15)));
					break;
				case LOp::FCAND:
				case LOp::FCEQ:
				case LOp::FCOR:
					WriteVI(o, 1, Emit(Op::FlagRead, Type::Int, {Rd(var::C(o.cFlag.read))}, sub, 0, static_cast<s32>(l.imm24)));
					break;
				case LOp::FCGET:
					WriteVI(o, l.it, Emit(Op::FlagRead, Type::Int, {Rd(var::C(o.cFlag.read))}, sub));
					break;
				case LOp::FCSET:
					Wr(var::C(o.cFlag.write), Emit(Op::ClipSet, Type::Int, {}, sub, 0, static_cast<s32>(l.imm24)));
					break;
				case LOp::FMAND:
				case LOp::FMEQ:
				case LOp::FMOR:
					WriteVI(o, l.it, Emit(Op::FlagRead, Type::Int, {Rd(var::M(o.mFlag.read)), ReadVI(l.is)}, sub));
					break;
				case LOp::FSAND:
				case LOp::FSEQ:
				case LOp::FSOR:
					WriteVI(o, l.it, Emit(Op::FlagRead, Type::Int, {Rd(var::S(o.sFlag.read))}, sub, 0, static_cast<s32>(l.imm12)));
					break;
				case LOp::FSSET:
				{
					const ValueId base = (o.sFlag.doFlag || o.doDivFlag) ? Rd(var::S(o.sFlag.write)) : Rd(var::S(o.sFlag.lastWrite));
					Wr(var::S(o.sFlag.write), Emit(Op::StatusSet, Type::Int, {base}, sub, 0, static_cast<s32>(l.imm12)));
					break;
				}
				case LOp::B:
					break;
				case LOp::BAL:
					WriteVI(o, l.it, Emit(Op::IConst, Type::Int, {}, sub, 0, static_cast<s32>(link)));
					break;
				case LOp::JR:
					jumpVal = ReadVI(l.is);
					break;
				case LOp::JALR:
					jumpVal = ReadVI(l.is);
					WriteVI(o, l.it, Emit(Op::IConst, Type::Int, {}, sub, 0, static_cast<s32>(link)));
					break;
				case LOp::IBEQ:
				case LOp::IBNE:
					cond = Emit(Op::Cond, Type::Bool, {ReadVI(l.is, o.lOp.memReadIs), ReadVI(l.it, o.lOp.memReadIt)}, sub);
					break;
				case LOp::IBLTZ:
				case LOp::IBGTZ:
				case LOp::IBLEZ:
				case LOp::IBGEZ:
					cond = Emit(Op::Cond, Type::Bool, {ReadVI(l.is, o.lOp.memReadIs)}, sub);
					break;
				case LOp::MOVE:
				{
					ArgVec a;
					Lanes(a, l.fs, l.mask);
					DeferVF(l.ft, l.mask, Emit(Op::Move, Type::Vec, std::move(a), sub, l.mask));
					break;
				}
				case LOp::MR32:
				{
					std::vector<ValueId> a(4, 0);
					for (int ln = 0; ln < 4; ln++)
						if (MaskLane(l.mask, ln))
							a[(ln + 1) & 3] = ReadVF(l.fs, (ln + 1) & 3);
					DeferVF(l.ft, l.mask, Emit(Op::Mr32, Type::Vec, std::move(a), sub, l.mask));
					break;
				}
				case LOp::MFIR:
					DeferVF(l.ft, l.mask, Emit(Op::Mfir, Type::Vec, {ReadVI(l.is)}, sub, l.mask));
					break;
				case LOp::MTIR:
					WriteVI(o, l.it, Emit(Op::Mtir, Type::Int, {ReadVF(l.fs, l.fsf)}, sub));
					break;
				case LOp::MFP:
					DeferVF(l.ft, l.mask, Emit(Op::Mfp, Type::Vec, {Rd(o.readP ? var::PL1 : var::PL0)}, sub, l.mask));
					break;
				case LOp::DIV:
				case LOp::SQRT:
				case LOp::RSQRT:
				{
					const ValueId fs = (l.op == LOp::SQRT) ? 0 : ReadVF(l.fs, l.fsf);
					const ValueId ft = ReadVF(l.ft, l.ftf);
					// imm 1: the dividend register was loaded from VF0, which makes
					// microVU skip the quotient clamp (checkVFClamp sees isZero).
					const bool fromVf0 = (l.op != LOp::SQRT) && l.fs == 0;
					Wr(o.writeQ ? var::QL1 : var::QL0, Emit(Op::Div, Type::Float, {fs, ft}, sub, 0, fromVf0 ? 1 : 0));
					Wr(var::DIVFLAG, Emit(Op::DivFlags, Type::Int, {fs, ft}, sub));
					break;
				}
				case LOp::EATAN: case LOp::EEXP: case LOp::ERCPR: case LOp::ERSQRT: case LOp::ESIN: case LOp::ESQRT:
					Wr(o.writeP ? var::PL1 : var::PL0, Emit(Op::Efu, Type::Float, {ReadVF(l.fs, l.fsf)}, sub));
					break;
				case LOp::EATANxy: case LOp::EATANxz: case LOp::ELENG: case LOp::ERLENG: case LOp::ERSADD: case LOp::ESADD: case LOp::ESUM:
				{
					// microVU loads the whole register for any field but a single
					// lane, and the series read fixed lanes whatever the field says.
					// A single-lane field rotates the operand in a way that depends
					// on microVU's register cache: declined.
					const u8 m = l.mask;
					if (m == 8 || m == 4 || m == 2 || m == 1)
						Unsupported("EFU op with a single-lane field");
					u8 lanes = 0xE; // x y z
					if (l.op == LOp::ESUM)
						lanes = 0xF;
					else if (l.op == LOp::EATANxy)
						lanes = 0xC;
					else if (l.op == LOp::EATANxz)
						lanes = 0xA;
					ArgVec a;
					Lanes(a, l.fs, lanes);
					Wr(o.writeP ? var::PL1 : var::PL0, Emit(Op::Efu, Type::Float, std::move(a), sub, l.mask));
					break;
				}
				case LOp::RINIT:
				case LOp::RXOR:
					Wr(var::R, Emit(Op::RUpd, Type::Float, {Rd(var::R), ReadVF(l.fs, l.fsf)}, sub));
					break;
				case LOp::RGET:
					if (!o.lOp.noWriteVF)
						DeferVF(l.ft, l.mask, Emit(Op::RGet, Type::Vec, {Rd(var::R)}, sub, l.mask));
					break;
				case LOp::RNEXT:
				{
					const ValueId r = Emit(Op::RUpd, Type::Float, {Rd(var::R)}, sub);
					Wr(var::R, r);
					if (!o.lOp.noWriteVF)
						DeferVF(l.ft, l.mask, Emit(Op::RGet, Type::Vec, {r}, sub, l.mask));
					break;
				}
				case LOp::WAITQ:
				case LOp::WAITP:
					break;
				case LOp::XTOP:
				case LOp::XITOP:
					WriteVI(o, l.it, Emit(Op::Xtop, Type::Int, {}, sub));
					break;
				case LOp::XGKICK:
					if (o_.xgkickHack)
					{
						if (!f_.unsupported)
						{
							f_.unsupported = true;
							f_.unsupportedWhy = "XGKickHack";
						}
						break;
					}
					if (o.doXGKICK)
						Wr(var::MEM, Emit(Op::Kick, Type::Mem, {Rd(var::MEM), Rd(var::XGKADDR)}, sub));
					Wr(var::XGKADDR, ReadVI(l.is));
					break;
				case LOp::Unknown:
				default:
					break;
			}
		}

		// mVUsetupBranch: reorder exact-match flag rings, swap the Q/P lanes.
		void Builder::LinkRemap(const BlockResult& b)
		{
			const auto remap = [&](u32 (*slot)(u32), const int* xFlag) {
				int order[4];
				SortFlag(xFlag, order, b.mFC.cycles);
				ValueId v[4];
				for (int i = 0; i < 4; i++)
					v[i] = Rd(slot(order[i]));
				for (int i = 0; i < 4; i++)
					Wr(slot(i), v[i]);
			};
			if (b.end.needExactMatch & 1)
				remap(var::S, b.mFC.xStatus);
			if (b.end.needExactMatch & 2)
				remap(var::M, b.mFC.xMac);
			if (b.end.needExactMatch & 4)
				remap(var::C, b.mFC.xClip);
			if (b.qLane)
			{
				const ValueId q0 = Rd(var::QL0), q1 = Rd(var::QL1);
				Wr(var::QL0, q1);
				Wr(var::QL1, q0);
			}
			if (b.pLane)
			{
				const ValueId p0 = Rd(var::PL0), p1 = Rd(var::PL1);
				Wr(var::PL0, p1);
				Wr(var::PL1, p0);
			}
		}

		void Builder::FillNode(int bid, const GraphNode& n)
		{
			const BlockResult& b = n.block;
			Block& blk = f_.blocks[bid];
			blk.cycles = b.cycles;
			accWritten_ = accClamped_ = false;

			pc_ = n.pc;
			// mVUtestCycles: a budget yield leaves with the block-entry state.
			if (o_.yieldChecks && !nodeLocal_)
				Emit(Op::YieldCheck, Type::None, AllVars());

			ValueId cond = 0, jumpVal = 0;
			for (u32 i = 0; i < b.count; i++)
			{
				const MvuOp& o = b.ops[i];
				const InstPair& p = b.insts[i];
				pc_ = b.pcs[i];
				const bool lowerXgkick = !o.lOp.isNOP && !o.uOp.iBit && p.lower.op == LOp::XGKICK;
				// mVUexecuteInstruction's order
				if (o.lOp.isNOP)
				{
					Upper(o, p);
					DivSet(o);
					if (o.uOp.iBit)
					{
						u32 imm = p.lower.word;
						if (o_.clampIImmediate && ((imm & 0x7fffffff) >= 0x7f800000))
							imm = (0x80000000 & imm) | 0x7f7fffff;
						Wr(var::I, Emit(Op::Const, Type::Float, {}, 0, 0, static_cast<s32>(imm)));
					}
				}
				else if (!o.swapOps)
				{
					Upper(o, p);
					DivSet(o);
					Lower(o, p, cond, jumpVal);
				}
				else
				{
					Lower(o, p, cond, jumpVal);
					Upper(o, p);
					DivSet(o);
				}
				FlushVF();
				if (o.doXGKICK && !lowerXgkick && !o_.xgkickHack)
					Wr(var::MEM, Emit(Op::Kick, Type::Mem, {Rd(var::MEM), Rd(var::XGKADDR)}));
				if (std::find(b.tBitExits.begin(), b.tBitExits.end(), i) != b.tBitExits.end())
					Emit(Op::ExitIf, Type::None, AllVars(), 1);
			}

			if (nodeLocal_)
			{
				f_.statusHead = ((((b.entry.needExactMatch & 1) ? 0 : ((b.entry.flagInfo >> 2) & 3)) - 1) & 3);
				f_.statusHeadOut = (b.endKind == BlockEnd::Branch || b.endKind == BlockEnd::CondBranch || b.endKind == BlockEnd::Jump)
					? ((((b.end.needExactMatch & 1) ? 0 : ((b.end.flagInfo >> 2) & 3)) - 1) & 3)
					: -1;
				// microVU's framing does the rest: links, ring reorders, Q/P lane
				// swaps, program ends, cycle tests. Hand back every final value.
				if (!b.tBitExits.empty() || b.tBitOnBranch)
					Unsupported("T-bit");
				if (b.endKind == BlockEnd::EvilJump)
					Unsupported("branch in a branch delay slot");
				// The block holding the first branch of such a pair: microVU's
				// emitters store the targets the evil block will pick between.
				for (const MvuOp& o : b.ops)
					if (o.lOp.badBranch || o.lOp.evilBranch)
						Unsupported("branch in a branch delay slot");
				if (b.endKind == BlockEnd::MBit)
					Unsupported("VU0 M-bit");
				if (o_.remapAtLeave && (b.endKind == BlockEnd::Branch || b.endKind == BlockEnd::CondBranch || b.endKind == BlockEnd::Jump))
					LinkRemap(b);
				if (o_.regionEnds && b.endKind == BlockEnd::EBit)
				{
					// mVUendProgram(isEbit 1): no link-time reorder before it.
					// An E bit on a conditional branch or a jump finishes
					// differently (condBranch reorders first; the TPC comes from
					// the branch); those stay with microVU.
					if (b.eBitOnBranch && !(b.branchKind == 1 || b.branchKind == 2))
						Unsupported("E-bit on a conditional branch or jump");
					const int fS = GetLastFlagInst(b.entry, b.mFC.xStatus, 0, 1);
					const int fM = GetLastFlagInst(b.entry, b.mFC.xMac, 1, 1);
					const int fC = GetLastFlagInst(b.entry, b.mFC.xClip, 2, 1);
					// The 100-cycle run-out: a divide whose flags were still in
					// flight lands in the final status instance; a pending kick
					// transfers.
					if (b.end.q > 4)
						Wr(var::S(fS), Emit(Op::StatusDiv, Type::Int, {Rd(var::S(fS)), Rd(var::DIVFLAG)}));
					if (b.end.xgkick && !o_.xgkickHack)
						Wr(var::MEM, Emit(Op::Kick, Type::Mem, {Rd(var::MEM), Rd(var::XGKADDR)}));
					f_.end.valid = true;
					f_.end.fS = fS;
					f_.end.fM = fM;
					f_.end.fC = fC;
					f_.end.qInst = b.qLane ^ (b.end.q ? 1 : 0);
					f_.end.pInst = b.pLane ^ (b.end.p ? 1 : 0);
					f_.end.tpc = b.eBitOnBranch ? b.takenPC : b.endTPC;
				}
				std::vector<ValueId> a = TouchedVars();
				if (accClamped_ && !accWritten_)
					for (int l = 0; l < 4; l++)
						a[var::ACC(l)] = accEntry_[l];
				a.push_back(cond);
				a.push_back(jumpVal);
				f_.leaveArgs.clear();
				for (size_t k = 0; k < a.size(); k++)
					if (a[k])
						f_.leaveArgs.push_back(static_cast<u32>(k));
				pc_ = b.pcs.back();
				Emit(Op::Leave, Type::None, std::move(a));
				blk.term = Term::Exit;
				return;
			}

			const auto ebitExit = [&](bool afterRemap) {
				const int fS = GetLastFlagInst(b.entry, b.mFC.xStatus, 0, 1);
				const int fM = GetLastFlagInst(b.entry, b.mFC.xMac, 1, 1);
				const int fC = GetLastFlagInst(b.entry, b.mFC.xClip, 2, 1);
				// mVUendProgram at an E-bit runs the pipeline out 100 cycles.
				if (b.end.q > 4)
					Wr(var::S(fS), Emit(Op::StatusDiv, Type::Int, {Rd(var::S(fS)), Rd(var::DIVFLAG)}));
				if (b.end.xgkick && !o_.xgkickHack)
					Wr(var::MEM, Emit(Op::Kick, Type::Mem, {Rd(var::MEM), Rd(var::XGKADDR)}));
				const int qInst = b.qLane ^ (b.end.q ? 1 : 0);
				const int pInst = b.pLane ^ (b.end.p ? 1 : 0);
				std::vector<ValueId> a = AllVars();
				if (cond)
					a.push_back(cond);
				else if (jumpVal)
					a.push_back(jumpVal);
				const ValueId x = Emit(Op::Exit, Type::None, std::move(a), static_cast<u16>(ExitKind::EBit));
				f_.insts[x].imm = fS | (fM << 2) | (fC << 4) | (qInst << 8) | (pInst << 9) | (afterRemap ? 0x400 : 0);
				f_.blocks[bid].term = Term::Exit;
			};

			switch (b.endKind)
			{
				case BlockEnd::Branch:
					if (blk.succs.empty())
					{
						Emit(Op::Exit, Type::None, AllVars(), static_cast<u16>(ExitKind::Unsupported));
						blk.term = Term::Exit;
						break;
					}
					if (b.tBitOnBranch)
						Emit(Op::ExitIf, Type::None, AllVars(), 1);
					LinkRemap(b);
					blk.term = Term::Jump;
					break;
				case BlockEnd::CondBranch:
					if (blk.succs.size() != 2)
					{
						Emit(Op::Exit, Type::None, AllVars(), static_cast<u16>(ExitKind::Unsupported));
						blk.term = Term::Exit;
						break;
					}
					LinkRemap(b);
					if (b.tBitOnBranch)
						Emit(Op::ExitIf, Type::None, AllVars(), 1);
					f_.blocks[bid].term = Term::Branch;
					f_.blocks[bid].cond = cond;
					break;
				case BlockEnd::Jump:
					LinkRemap(b);
					if (b.tBitOnBranch)
						Emit(Op::ExitIf, Type::None, AllVars(), 1);
					if (n.jumpUnresolved || n.jumpTargets.empty())
					{
						std::vector<ValueId> a = AllVars();
						a.push_back(jumpVal);
						Emit(Op::ExitIf, Type::None, std::move(a), static_cast<u16>(2));
					}
					f_.blocks[bid].term = blk.succs.empty() && blk.dispatchTargets.empty() ? Term::Exit : Term::IndirectJump;
					f_.blocks[bid].cond = jumpVal;
					break;
				case BlockEnd::EBit:
				{
					// condBranch runs mVUsetupBranch before its E-bit end; normBranch
					// and normJump do not.
					const bool condEbit = b.eBitOnBranch && b.branchKind >= 3 && b.branchKind <= 8;
					if (condEbit)
						LinkRemap(b);
					ebitExit(condEbit);
					break;
				}
				case BlockEnd::EvilJump:
				case BlockEnd::MBit:
				{
					if (!f_.unsupported)
					{
						f_.unsupported = true;
						f_.unsupportedWhy = (b.endKind == BlockEnd::MBit) ? "VU0 M-bit" : "branch in a branch delay slot";
					}
					const ValueId x = Emit(Op::Exit, Type::None, AllVars(), static_cast<u16>(ExitKind::Unsupported));
					(void)x;
					f_.blocks[bid].term = Term::Exit;
					break;
				}
			}
		}

		// Braun et al. section 3.2: phi cycles with a single outside operand.
		// Flat arrays and an iterative Tarjan: programs reach 100k phis.
		void Builder::RemoveRedundantSccs()
		{
			const size_t NV = f_.insts.size();
			std::vector<u32> mark(NV, 0);   // generation stamp: member of the current set
			std::vector<int> index(NV, -1), low(NV, 0);
			std::vector<u8> onStack(NV, 0);
			u32 gen = 0;

			std::function<void(const std::vector<ValueId>&)> removeRedundant;
			removeRedundant = [&](const std::vector<ValueId>& phis) {
				const u32 g = ++gen;
				for (ValueId p : phis)
				{
					mark[p] = g;
					index[p] = -1;
					onStack[p] = 0;
				}
				std::vector<std::vector<ValueId>> sccs;
				std::vector<ValueId> stack;
				int counter = 0;
				struct Frame { ValueId v; size_t k; };
				std::vector<Frame> call;
				for (ValueId root : phis)
				{
					if (index[root] >= 0)
						continue;
					call.push_back({root, 0});
					index[root] = low[root] = counter++;
					stack.push_back(root);
					onStack[root] = 1;
					while (!call.empty())
					{
						Frame& fr = call.back();
						const ArgVec& args = f_.insts[fr.v].args;
						if (fr.k < args.size())
						{
							const ValueId w = Resolve(args[fr.k++]);
							if (!w || mark[w] != g)
								continue;
							if (index[w] < 0)
							{
								index[w] = low[w] = counter++;
								stack.push_back(w);
								onStack[w] = 1;
								call.push_back({w, 0});
							}
							else if (onStack[w])
								low[fr.v] = std::min(low[fr.v], index[w]);
							continue;
						}
						const ValueId v = fr.v;
						call.pop_back();
						if (!call.empty())
							low[call.back().v] = std::min(low[call.back().v], low[v]);
						if (low[v] == index[v])
						{
							std::vector<ValueId> scc;
							ValueId w;
							do
							{
								w = stack.back();
								stack.pop_back();
								onStack[w] = 0;
								scc.push_back(w);
							} while (w != v);
							if (scc.size() > 1)
								sccs.push_back(std::move(scc));
						}
					}
				}
				// Tarjan completes an SCC after every SCC its operands reach.
				for (const auto& scc : sccs)
				{
					const u32 sg = ++gen;
					for (ValueId phi : scc)
						mark[phi] = sg;
					std::vector<ValueId> inner;
					ValueId outer = 0;
					bool manyOuter = false;
					for (ValueId phi : scc)
					{
						bool isInner = true;
						for (ValueId op : f_.insts[phi].args)
						{
							op = Resolve(op);
							if (mark[op] == sg)
								continue;
							isInner = false;
							if (!outer)
								outer = op;
							else if (op != outer)
								manyOuter = true;
						}
						if (isInner)
							inner.push_back(phi);
					}
					if (outer && !manyOuter)
					{
						for (ValueId phi : scc)
						{
							repl_[phi] = outer;
							f_.insts[phi].dead = true;
							f_.phisRemovedScc++;
						}
					}
					else if (manyOuter && !inner.empty())
						removeRedundant(inner);
				}
			};

			std::vector<ValueId> phis;
			for (const Block& bl : f_.blocks)
				for (ValueId p : bl.phis)
					if (!f_.insts[p].dead)
						phis.push_back(p);
			removeRedundant(phis);
		}

		thread_local std::vector<std::unique_ptr<Builder::Scratch>> Builder::s_pool;

		Function Builder::Build()
		{
			const int N = static_cast<int>(g_.nodes.size());
			if (nodeLocal_)
			{
				// a node is a few tens of values; grow once
				f_.insts.reserve(64);
				repl_.reserve(64);
				users_.reserve(64);
				f_.blocks.reserve(2);
			}
			f_.insts.emplace_back(); // id 0 = none
			repl_.push_back(0);
			users_.emplace_back();

			f_.blocks.resize(N);
			for (int i = 0; i < N; i++)
			{
				const GraphNode& n = g_.nodes[i];
				Block& bl = f_.blocks[i];
				bl.node = i;
				const BlockEnd k = n.block.endKind;
				if (!nodeLocal_ && (k == BlockEnd::Branch || k == BlockEnd::CondBranch))
				{
					if (n.taken < 0 || (k == BlockEnd::CondBranch && n.fall < 0))
					{
						// The graph was truncated at its node limit.
						f_.unsupported = true;
						f_.unsupportedWhy = "program graph truncated";
						continue;
					}
					bl.succs.push_back(n.taken);
					if (k == BlockEnd::CondBranch)
						bl.succs.push_back(n.fall);
				}
				if (!nodeLocal_ && k == BlockEnd::Jump)
				{
					// A jump whose targets were only observed at run time is a
					// dispatch boundary, as it is in microVU: state goes to memory
					// and each target starts from it. Statically bounded jumps
					// (returns to a link register) stay ordinary edges.
					if (n.jumpUnresolved)
						bl.dispatchTargets = n.jumpTargets;
					else
						for (int t : n.jumpTargets)
							bl.succs.push_back(t);
				}
			}
			std::vector<int> entryTargets = g_.roots;
			for (int i = 0; i < N; i++)
				for (int t : f_.blocks[i].dispatchTargets)
					if (std::find(entryTargets.begin(), entryTargets.end(), t) == entryTargets.end())
						entryTargets.push_back(t);
			for (int r : entryTargets)
			{
				const int e = static_cast<int>(f_.blocks.size());
				f_.blocks.emplace_back();
				f_.blocks[e].succs.push_back(r);
				f_.blocks[e].term = Term::Jump;
				f_.entryBlocks.push_back(e);
			}
			for (size_t b = 0; b < f_.blocks.size(); b++)
				for (int s : f_.blocks[b].succs)
					f_.blocks[s].preds.push_back(static_cast<int>(b));
			if (nodeLocal_)
				for (Block& bl : f_.blocks)
					bl.insts.reserve(bl.node >= 0 ? 48 : 24);

			const size_t B = f_.blocks.size();
			cur_.assign(B, {});
			for (auto& c : cur_)
				c.fill(0);
			sealed_.assign(B, false);
			filled_.assign(B, false);
			incomplete_.assign(B, {});
			vf0_.assign(B, 0);
			vi0_.assign(B, 0);

			// Reverse postorder from the entry blocks (a node-local build is
			// its entry block, then the node).
			std::vector<int> rpo;
			if (nodeLocal_ && B == 2 && f_.entryBlocks.size() == 1 && f_.entryBlocks[0] == 1)
			{
				rpo.reserve(2);
				rpo.push_back(1);
				rpo.push_back(0);
			}
			else
			{
				std::vector<int> state(B, 0), post;
				std::vector<std::pair<int, size_t>> st;
				for (int e : f_.entryBlocks)
				{
					if (state[e])
						continue;
					st.push_back({e, 0});
					state[e] = 1;
					while (!st.empty())
					{
						auto& [u, k] = st.back();
						if (k < f_.blocks[u].succs.size())
						{
							const int v = f_.blocks[u].succs[k++];
							if (!state[v])
							{
								state[v] = 1;
								st.push_back({v, 0});
							}
						}
						else
						{
							post.push_back(u);
							st.pop_back();
						}
					}
				}
				rpo.assign(post.rbegin(), post.rend());
			}

			auto trySeal = [&](int b) {
				if (sealed_[b])
					return;
				for (int p : f_.blocks[b].preds)
					if (!filled_[p])
						return;
				Seal(b);
			};

			for (int b : rpo)
			{
				trySeal(b);
				b_ = b;
				if (f_.blocks[b].node >= 0)
					FillNode(b, g_.nodes[f_.blocks[b].node]);
				filled_[b] = true;
				for (int s : f_.blocks[b].succs)
					trySeal(s);
			}
			// Blocks not reachable from an entry (should not happen) stay unsealed.
			for (size_t b = 0; b < B; b++)
				if (!sealed_[b] && filled_[b])
					Seal(static_cast<int>(b));

			// Without phis there is nothing to remove or route (repl_ is only
			// written when a phi goes).
			if (f_.phisCreated == 0)
				return std::move(f_);

			RemoveRedundantSccs();

			for (Inst& in : f_.insts)
				for (ValueId& a : in.args)
					a = Resolve(a);
			for (Block& bl : f_.blocks)
			{
				bl.cond = Resolve(bl.cond);
				bl.phis.erase(std::remove_if(bl.phis.begin(), bl.phis.end(), [&](ValueId p) { return f_.insts[p].dead; }), bl.phis.end());
			}
			return std::move(f_);
		}

		// Dominators over the IR block graph (Cooper/Harvey/Kennedy), virtual root
		// feeding the entry blocks. Returns idom (virtual root = B).
		std::vector<int> Dominators(const Function& f, std::vector<int>& rpoIndex)
		{
			const int B = static_cast<int>(f.blocks.size());
			const int V = B;
			std::vector<std::vector<int>> succ(B + 1), pred(B + 1);
			for (int b = 0; b < B; b++)
				succ[b] = f.blocks[b].succs;
			succ[V] = f.entryBlocks;
			std::vector<int> state(B + 1, 0), post;
			std::vector<std::pair<int, size_t>> st;
			st.push_back({V, 0});
			state[V] = 1;
			while (!st.empty())
			{
				auto& [u, k] = st.back();
				if (k < succ[u].size())
				{
					const int v = succ[u][k++];
					if (!state[v])
					{
						state[v] = 1;
						st.push_back({v, 0});
					}
				}
				else
				{
					post.push_back(u);
					st.pop_back();
				}
			}
			std::vector<int> rpo(post.rbegin(), post.rend());
			rpoIndex.assign(B + 1, -1);
			for (size_t i = 0; i < rpo.size(); i++)
				rpoIndex[rpo[i]] = static_cast<int>(i);
			for (int u = 0; u <= B; u++)
				if (rpoIndex[u] >= 0)
					for (int v : succ[u])
						pred[v].push_back(u);
			std::vector<int> idom(B + 1, -1);
			idom[V] = V;
			auto intersect = [&](int a, int b) {
				while (a != b)
				{
					while (rpoIndex[a] > rpoIndex[b])
						a = idom[a];
					while (rpoIndex[b] > rpoIndex[a])
						b = idom[b];
				}
				return a;
			};
			bool changed = true;
			while (changed)
			{
				changed = false;
				for (size_t i = 1; i < rpo.size(); i++)
				{
					const int b = rpo[i];
					int nd = -1;
					for (int p : pred[b])
					{
						if (idom[p] < 0)
							continue;
						nd = (nd < 0) ? p : intersect(p, nd);
					}
					if (nd >= 0 && idom[b] != nd)
					{
						idom[b] = nd;
						changed = true;
					}
				}
			}
			return idom;
		}
	} // namespace

	u32 Function::LivePhis() const
	{
		u32 n = 0;
		for (const Block& b : blocks)
			n += static_cast<u32>(b.phis.size());
		return n;
	}

	u32 Function::LiveInsts() const
	{
		u32 n = 0;
		for (const Block& b : blocks)
			n += static_cast<u32>(b.insts.size());
		return n;
	}

	Function BuildSSA(const ProgramGraph& g, const BuildOptions& opts)
	{
		Builder b(g, opts);
		return b.Build();
	}

	Function BuildNodeLocal(BlockResult& block, const BuildOptions& opts)
	{
		ProgramGraph g;
		g.nodes.emplace_back();
		GraphNode& n = g.nodes.back();
		n.id = 0;
		n.pc = block.startPC;
		n.entry = block.entry;
		std::swap(n.block, block);
		n.isRoot = true;
		g.roots.push_back(0);
		Function f;
		{
			Builder b(g, opts, true);
			f = b.Build();
		}
		std::swap(n.block, block);
		return f;
	}

	Function BuildNodeLocal(const BlockResult& block, const BuildOptions& opts)
	{
		ProgramGraph g;
		g.nodes.emplace_back();
		GraphNode& n = g.nodes.back();
		n.id = 0;
		n.pc = block.startPC;
		n.entry = block.entry;
		n.block = block;
		n.isRoot = true;
		g.roots.push_back(0);
		Builder b(g, opts, true);
		return b.Build();
	}

	bool StatusChainLinear(const Function& f)
	{
		const Block* node = nullptr;
		for (const Block& bl : f.blocks)
			if (bl.node >= 0)
				node = &bl;
		if (!node || f.statusHead < 0)
			return false;
		ValueId head = 0; // the last status operation
		const Inst* leave = nullptr;
		for (ValueId v : node->insts)
		{
			const Inst& in = f.insts[v];
			if (in.op == Op::Leave)
				leave = &in;
			if (in.op != Op::StatusUpd && in.op != Op::StatusDiv && in.op != Op::StatusSet)
				continue;
			const ValueId p = in.args[0];
			const bool ok = head ? p == head : (p && f.insts[p].op == Op::LiveIn && f.insts[p].var == var::S(static_cast<u32>(f.statusHead)));
			if (!ok)
				return false;
			head = v;
		}
		if (f.statusHeadOut < 0)
			return true;
		if (!leave)
			return false;
		const ValueId out = leave->args[var::S(static_cast<u32>(f.statusHeadOut))];
		if (head)
			return out == head;
		// no status operation: the successor's newest is the slot that was newest
		// here (possibly renamed by a ring reorder)
		if (!out)
			return f.statusHeadOut == f.statusHead;
		return f.insts[out].op == Op::LiveIn && f.insts[out].var == var::S(static_cast<u32>(f.statusHead));
	}

	std::vector<std::string> Verify(const Function& f)
	{
		std::vector<std::string> out;
		char buf[160];
		std::vector<int> rpoIndex;
		const std::vector<int> idom = Dominators(f, rpoIndex);
		const int V = static_cast<int>(f.blocks.size());
		auto dominates = [&](int a, int b) {
			while (true)
			{
				if (a == b)
					return true;
				if (b == V || idom[b] < 0)
					return false;
				b = idom[b];
			}
		};
		std::vector<int> pos(f.insts.size(), -1);
		for (const Block& bl : f.blocks)
		{
			for (ValueId p : bl.phis)
				pos[p] = -1;
			for (size_t i = 0; i < bl.insts.size(); i++)
				pos[bl.insts[i]] = static_cast<int>(i);
		}
		auto check = [&](ValueId use, ValueId def, int useBlock, int usePos) {
			if (!def)
				return;
			const Inst& d = f.insts[def];
			if (d.dead)
			{
				std::snprintf(buf, sizeof(buf), "%%%u uses removed value %%%u", use, def);
				out.emplace_back(buf);
				return;
			}
			if (d.block == useBlock)
			{
				if (d.op != Op::Phi && pos[def] >= usePos)
				{
					std::snprintf(buf, sizeof(buf), "%%%u uses %%%u before its definition", use, def);
					out.emplace_back(buf);
				}
				return;
			}
			if (!dominates(d.block, useBlock))
			{
				std::snprintf(buf, sizeof(buf), "%%%u in b%d uses %%%u from b%d, which does not dominate it", use, useBlock, def, d.block);
				out.emplace_back(buf);
			}
		};
		for (size_t b = 0; b < f.blocks.size(); b++)
		{
			const Block& bl = f.blocks[b];
			if (rpoIndex[b] < 0)
				continue;
			for (ValueId p : bl.phis)
			{
				const Inst& in = f.insts[p];
				if (in.args.size() != bl.preds.size())
				{
					std::snprintf(buf, sizeof(buf), "phi %%%u has %zu operands for %zu predecessors", p, in.args.size(), bl.preds.size());
					out.emplace_back(buf);
					continue;
				}
				for (size_t k = 0; k < in.args.size(); k++)
					check(p, in.args[k], bl.preds[k], 1 << 30);
			}
			for (size_t i = 0; i < bl.insts.size(); i++)
				for (ValueId a : f.insts[bl.insts[i]].args)
					check(bl.insts[i], a, static_cast<int>(b), static_cast<int>(i));
			if (bl.cond)
				check(0, bl.cond, static_cast<int>(b), 1 << 30);
		}
		return out;
	}

	static const char* const kOpNames[] = {"none", "livein", "phi", "const", "fmac", "opmula", "opmsub", "fmac2", "clipbits",
		"macflags", "status.upd", "status.div", "status.set", "clip.upd", "clip.set", "flagread", "ialu", "incvi", "mtir",
		"xtop", "loadq", "loadi", "storeq", "storei", "move", "mr32", "mfir", "mfp", "div", "divflags", "efu", "r.upd",
		"r.get", "accclamp", "iconst", "kick", "cond", "yield-check", "exit-if", "exit", "leave"};
	const char* OpName(Op op) { return kOpNames[static_cast<int>(op)]; }

	std::string Dump(const Function& f, const ProgramGraph& g)
	{
		static const char* const kOp[] = {"none", "livein", "phi", "const", "fmac", "opmula", "opmsub", "fmac2", "clipbits",
			"macflags", "status.upd", "status.div", "status.set", "clip.upd", "clip.set", "flagread", "ialu", "incvi", "mtir",
			"xtop", "loadq", "loadi", "storeq", "storei", "move", "mr32", "mfir", "mfp", "div", "divflags", "efu", "r.upd",
			"r.get", "accclamp", "iconst", "kick", "cond", "yield-check", "exit-if", "exit", "leave"};
		static_assert(sizeof(kOp) / sizeof(kOp[0]) == static_cast<size_t>(Op::Leave) + 1, "kOp out of step with Op");
		static const char* const kMask[16] = {"", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"};
		std::string s;
		char buf[512];
		std::snprintf(buf, sizeof(buf), "; vu-aot IR: %zu blocks (%zu entries), %u insts, %u phis%s%s\n",
			f.blocks.size(), f.entryBlocks.size(), f.LiveInsts(), f.LivePhis(),
			f.unsupported ? ", UNSUPPORTED: " : "", f.unsupported ? f.unsupportedWhy.c_str() : "");
		s += buf;

		auto lanes = [&](const ArgVec& a, size_t off) {
			std::string r = "{";
			for (size_t k = 0; k < 4; k++)
			{
				if (k)
					r += ",";
				if (off + k < a.size() && a[off + k])
					r += "%" + std::to_string(a[off + k]);
				else
					r += "-";
			}
			return r + "}";
		};

		for (size_t b = 0; b < f.blocks.size(); b++)
		{
			const Block& bl = f.blocks[b];
			if (bl.node < 0)
			{
				std::snprintf(buf, sizeof(buf), "\nentry b%zu -> b%d\n", b, bl.succs.empty() ? -1 : bl.succs[0]);
				s += buf;
				for (ValueId v : bl.insts)
				{
					std::snprintf(buf, sizeof(buf), "  %%%u = livein %s\n", v, var::Name(f.insts[v].var).c_str());
					s += buf;
				}
				continue;
			}
			const GraphNode& n = g.nodes[bl.node];
			std::string preds;
			for (int p : bl.preds)
				preds += " b" + std::to_string(p);
			std::snprintf(buf, sizeof(buf), "\nb%zu: pc=0x%04x key{exact=%u flagInfo=0x%02x q=%u p=%u xgkick=%u viBackUp=%u blockType=%u} cycles=%u preds:%s\n",
				b, n.pc, n.entry.needExactMatch, n.entry.flagInfo, n.entry.q, n.entry.p, n.entry.xgkick, n.entry.viBackUp,
				n.entry.blockType, bl.cycles, preds.c_str());
			s += buf;
			for (ValueId p : bl.phis)
			{
				const Inst& in = f.insts[p];
				std::string ops;
				for (size_t k = 0; k < in.args.size(); k++)
					ops += " [b" + std::to_string(bl.preds[k]) + ": %" + std::to_string(in.args[k]) + "]";
				std::snprintf(buf, sizeof(buf), "  %%%u = phi %s%s\n", p, var::Name(in.var).c_str(), ops.c_str());
				s += buf;
			}
			u32 lastPc = ~0u;
			size_t opIdx = 0;
			for (ValueId v : bl.insts)
			{
				const Inst& in = f.insts[v];
				if (in.op != Op::YieldCheck && in.pc != lastPc)
				{
					lastPc = in.pc;
					while (opIdx < n.block.pcs.size() && n.block.pcs[opIdx] != in.pc)
						opIdx++;
					if (opIdx < n.block.insts.size())
					{
						char dis[160];
						FormatPair(n.block.insts[opIdx], dis, sizeof(dis));
						const MvuOp& o = n.block.ops[opIdx];
						std::snprintf(buf, sizeof(buf), "  ; %s  [stall=%u%s%s%s%s%s%s]\n", dis, o.stall, o.swapOps ? " swap" : "",
							o.backupVF ? " backupVF" : "", o.lOp.backupVI ? " backupVI" : "", o.lOp.memReadIs ? " memReadIs" : "",
							o.lOp.memReadIt ? " memReadIt" : "", o.lOp.isNOP ? " lowerNOP" : "");
						s += buf;
					}
				}
				const char* name = kOp[static_cast<int>(in.op)];
				switch (in.op)
				{
					case Op::YieldCheck:
					case Op::ExitIf:
					case Op::Exit:
					case Op::Leave:
					{
						u32 live = 0;
						for (ValueId a : in.args)
							live += a ? 1 : 0;
						std::snprintf(buf, sizeof(buf), "    %s%s (%u state values)%s\n", name,
							in.op == Op::ExitIf ? (in.sub == 1 ? ".tbit" : ".jumpbail") : (in.op == Op::Exit ? (in.sub == 0 ? ".ebit" : ".unsupported") : ""),
							live, "");
						break;
					}
					case Op::Fmac:
					case Op::Opmula:
					case Op::Opmsub:
						std::snprintf(buf, sizeof(buf), "    %%%u = %s %s.%s fs=%s ft=%s acc=%s\n", v, name, UOpName(static_cast<UOp>(in.sub)),
							kMask[in.mask], lanes(in.args, 0).c_str(), lanes(in.args, 4).c_str(), lanes(in.args, 8).c_str());
						break;
					case Op::Fmac2:
					case Op::Move:
					case Op::Mr32:
						std::snprintf(buf, sizeof(buf), "    %%%u = %s %s.%s %s\n", v, name,
							in.op == Op::Fmac2 ? UOpName(static_cast<UOp>(in.sub)) : LOpName(static_cast<LOp>(in.sub)),
							kMask[in.mask], lanes(in.args, 0).c_str());
						break;
					case Op::StoreQ:
						std::snprintf(buf, sizeof(buf), "    %%%u = storeq.%s %%%u [%%%u%+d] %s\n", v, kMask[in.mask], in.args[0], in.args[1], in.imm, lanes(in.args, 2).c_str());
						break;
					case Op::Const:
						if (in.type == Type::Vec)
							std::snprintf(buf, sizeof(buf), "    %%%u = const vf00\n", v);
						else
							std::snprintf(buf, sizeof(buf), "    %%%u = const 0x%08x\n", v, static_cast<u32>(in.imm));
						break;
					default:
					{
						std::string ops;
						for (ValueId a : in.args)
							ops += a ? (" %" + std::to_string(a)) : std::string(" -");
						const char* subName = "";
						if (in.op == Op::FlagRead || in.op == Op::IAlu || in.op == Op::Cond || in.op == Op::Div || in.op == Op::Efu ||
							in.op == Op::LoadQ || in.op == Op::LoadI || in.op == Op::StoreI || in.op == Op::Xtop || in.op == Op::RUpd)
							subName = LOpName(static_cast<LOp>(in.sub));
						std::snprintf(buf, sizeof(buf), "    %%%u = %s%s%s%s%s imm=%d\n", v, name, *subName ? " " : "", subName,
							in.mask ? "." : "", in.mask ? kMask[in.mask] : "", in.imm);
						std::string line = buf;
						line.pop_back();
						s += line + ops + "\n";
						continue;
					}
				}
				s += buf;
			}
			switch (bl.term)
			{
				case Term::Jump: std::snprintf(buf, sizeof(buf), "  -> b%d\n", bl.succs[0]); break;
				case Term::Branch: std::snprintf(buf, sizeof(buf), "  -> %%%u ? b%d : b%d\n", bl.cond, bl.succs[0], bl.succs[1]); break;
				case Term::IndirectJump:
				{
					std::string t;
					for (int x : bl.succs)
						t += " b" + std::to_string(x);
					for (int x : bl.dispatchTargets)
						t += " dispatch:b" + std::to_string(x);
					std::snprintf(buf, sizeof(buf), "  -> jump %%%u in {%s }\n", bl.cond, t.c_str());
					break;
				}
				case Term::Exit: std::snprintf(buf, sizeof(buf), "  -> exit\n"); break;
				default: std::snprintf(buf, sizeof(buf), "  -> ?\n"); break;
			}
			s += buf;
		}
		return s;
	}
} // namespace vuaot::ir
