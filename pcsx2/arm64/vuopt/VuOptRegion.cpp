// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuOptRegion.h"

#include "VuOptIR.h"

#include "VU.h"
#include "common/Assertions.h"
#include "arm64/AsmHelpers.h"
#include "arm64/microVU_Misc-arm64.h"
#include "vixl/aarch64/disasm-aarch64.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vuopt
{
	namespace a64 = vixl::aarch64;

	namespace
	{
		constexpr int kCMax = 25, kCMin = 26;
		constexpr int kCyclesReg = 28;
		constexpr int kMemBaseReg = 27;
		constexpr int kVHomeRegs[] = {31, 30, 29, 27, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15};
		constexpr int kGHomeRegs[] = {26, 15, 14, 13, 12, 11};

		a64::Register W(int n) { return a64::Register(n, 32); }
		a64::VRegister Q(int n) { return a64::VRegister(n, 128); }
		s32 G(const void* p) { return static_cast<s32>(reinterpret_cast<const u8*>(p) - reinterpret_cast<const u8*>(&mVUglob)); }

		bool GHomeable(u32 v)
		{
			using namespace ir;
			return (v >= var::VI(1) && v <= var::VI(15)) || v == var::I || v == var::R || (v >= var::M(0) && v <= var::C(3)) ||
				   v == var::DIVFLAG || v == var::VIBACKUP || v == var::XGKADDR;
		}

		u32 VecKey(u32 v)
		{
			using namespace ir;
			if (v < 128)
				return (v / 4) * 4;
			if (v < 132)
				return var::ACC(0);
			return v;
		}

		// A block a region can hold: it ends in a branch, a conditional branch,
		// a JR/JALR, or a program end (a plain E bit, or one on B/BAL), and
		// nothing in it needs microVU's run-time checks (T/D/M bits, branches
		// in delay slots).
		bool Internal(const BlockResult& b)
		{
			switch (b.endKind)
			{
				case BlockEnd::Branch:
				case BlockEnd::CondBranch:
				case BlockEnd::Jump:
					if (b.eBitOnBranch)
						return false;
					break;
				case BlockEnd::EBit:
					if (b.eBitOnBranch && !(b.branchKind == 1 || b.branchKind == 2))
						return false;
					break;
				default:
					return false;
			}
			if (b.tBitOnBranch || !b.tBitExits.empty() || b.entry.blockType != 0)
				return false;
			for (const MvuOp& o : b.ops)
			{
				if (o.lOp.badBranch || o.lOp.evilBranch || o.uOp.tBit || o.uOp.dBit || o.uOp.mBit)
					return false;
				if (o.uOp.eBit && b.endKind != BlockEnd::EBit)
					return false;
			}
			return true;
		}

		bool ConstJump(const BlockResult& b, u32& target)
		{
			const MvuOp& o = b.ops[b.branchIndex];
			if (!o.lOp.constJump.isValid)
				return false;
			target = (o.lOp.constJump.regValue * 8) & (0x4000 - 8);
			return true;
		}

		a64::Condition BranchCond(u32 kind)
		{
			switch (kind)
			{
				case 3: return a64::eq; // IBEQ
				case 4: return a64::ge; // IBGEZ
				case 5: return a64::gt; // IBGTZ
				case 6: return a64::le; // IBLEZ
				case 7: return a64::lt; // IBLTZ
				default: return a64::ne; // IBNE
			}
		}
	} // namespace

	class RegionBuilder
	{
	public:
		RegionBuilder(RegionTier& tier, const MvuEmitEnv& env, const LowerEnv& le)
			: t_(tier)
			, env_(env)
			, le_(le)
		{
		}

		bool Run(const BlockResult& root, std::string* why);

	private:
		struct Node
		{
			u32 pc = 0;
			PipeState key;
			BlockResult br;
			ir::Function fn;
			void* blk = nullptr;
			u32 clamped[33] = {}; // clamped uses of each incoming vector register (NodeLowering::Check)
			std::unique_ptr<NodeLowering> low; // analysed lowering of fn (refers to fn: Nodes do not move)
			int taken = 0x7fffffff, fall = 0x7fffffff; // >= 0: node; < 0: exit -(k+1)
			std::vector<std::pair<u32, int>> jumps;    // JR/JALR: recorded target PC -> node / exit
			a64::Label head, brk;
			void* entry = nullptr;
		};
		struct Exit
		{
			u32 pc = 0;
			PipeState key;
			void* known = nullptr;
			a64::Label stub, jump, far;
		};

		RegionTier& t_;
		const MvuEmitEnv& env_;
		const LowerEnv& le_;
		std::vector<std::unique_ptr<Node>> nodes_;
		std::vector<std::unique_ptr<Exit>> exits_;
		HomeMap homes_;
		bool hasMem_ = false;

		ir::BuildOptions BuildOpts() const
		{
			ir::BuildOptions bo;
			bo.vuIndex = env_.vuIndex;
			bo.clampIImmediate = env_.clampOverflow && !env_.clampSign;
			bo.clampExtra = env_.clampExtra;
			bo.clampSign = env_.clampSign;
			bo.remapAtLeave = true;
			bo.regionEnds = true;
			return bo;
		}

		// Builds the node's IR and checks it lowers; `clamped`: its clamped uses
		// of incoming vector registers, for the home choice.
		bool Lowerable(BlockResult& br, Node& n) const
		{
			if (!Internal(br))
				return false;
			n.fn = ir::BuildNodeLocal(br, BuildOpts());
			n.low = NodeLowering::Check(n.fn, le_, n.clamped);
			return n.low != nullptr;
		}

		int AddExit(u32 pc, const PipeState& st, void* known)
		{
			for (size_t k = 0; k < exits_.size(); k++)
				if (exits_[k]->pc == pc && exits_[k]->key == st && exits_[k]->known == known)
					return -static_cast<int>(k) - 1;
			auto e = std::make_unique<Exit>();
			e->pc = pc;
			e->key = st;
			e->known = known;
			exits_.push_back(std::move(e));
			return -static_cast<int>(exits_.size());
		}

		int Visit(u32 pc, const PipeState& st);
		void Expand(int id);
		void AssignHomes();
		void Emit(const u8*& regionEnd);
		void EmitEnd(const Node& n);
		void EmitJump(Node& n);
		std::vector<int> pendingJumps_; // JR nodes whose recorded targets are resolved after the static walk
		void Jump(int target, int next);
		a64::Label* Target(int target);
	};

	// microVU's normBranchCompile / condBranch: search, else compile now.
	int RegionBuilder::Visit(u32 pc, const PipeState& st)
	{
		pc &= 0x4000 - 8;
		if (void* blk = env_.searchBlock(env_.vuIndex, pc, st.bytes))
		{
			for (size_t i = 0; i < nodes_.size(); i++)
				if (nodes_[i]->blk == blk)
					return static_cast<int>(i);
			return AddExit(pc, st, env_.blockEntry(blk));
		}
		BlockResult br = t_.an_->Analyze(pc, st);
		auto n = std::make_unique<Node>();
		if (!Lowerable(br, *n))
			return AddExit(pc, st, nullptr);
		const int id = static_cast<int>(nodes_.size());
		n->pc = pc;
		n->key = st;
		n->br = std::move(br);
		n->blk = env_.addBlock(env_.vuIndex, pc, st.bytes);
		nodes_.push_back(std::move(n));
		Expand(id);
		return id;
	}

	void RegionBuilder::Expand(int id)
	{
		const BlockResult& b = nodes_[id]->br;
		const PipeState end = b.end;
		const u32 takenPC = b.takenPC, fallPC = b.fallPC;
		if (b.endKind == BlockEnd::EBit)
			return;
		if (b.endKind == BlockEnd::Jump)
		{
			// A constant target is a plain link to microVU too (normJump ->
			// normBranchCompile). Otherwise microVU links at run time, after the
			// static closure: so do the recorded targets.
			u32 target;
			if (ConstJump(b, target))
			{
				const int t = Visit(target, end);
				nodes_[id]->taken = t;
			}
			else
				pendingJumps_.push_back(id);
			return;
		}
		if (b.endKind == BlockEnd::CondBranch)
		{
			const int f = Visit(fallPC, end);
			nodes_[id]->fall = f;
		}
		const int t = Visit(takenPC, end);
		nodes_[id]->taken = t;
	}

	void RegionBuilder::AssignHomes()
	{
		using namespace ir;
		// What a home saves: one load per node that reads the variable's
		// incoming value and one store per node that writes it. What a clamped
		// home saves: the clamp (two instructions) in each node that clamps the
		// incoming value (a node clamps a value once, milestone 5).
		// Blocks inside a loop (a cycle of region links) count eight times.
		const size_t NN = nodes_.size();
		std::vector<u32> weight(NN, 1);
		{
			std::vector<std::vector<int>> succ(NN);
			for (size_t i = 0; i < NN; i++)
			{
				const Node& n = *nodes_[i];
				if (n.taken >= 0 && n.taken != 0x7fffffff)
					succ[i].push_back(n.taken);
				if (n.fall >= 0 && n.fall != 0x7fffffff)
					succ[i].push_back(n.fall);
				for (const auto& j : n.jumps)
					if (j.second >= 0)
						succ[i].push_back(j.second);
			}
			// A node is in a loop when its strongly connected component has
			// more than one node or it links to itself (Tarjan, iterative).
			std::vector<int> index(NN, -1), low(NN, 0), comp(NN, -1), stk;
			std::vector<u8> onStack(NN, 0);
			std::vector<std::pair<int, size_t>> call;
			int counter = 0, ncomp = 0;
			for (size_t root = 0; root < NN; root++)
			{
				if (index[root] >= 0)
					continue;
				call.push_back({static_cast<int>(root), 0});
				while (!call.empty())
				{
					const int v = call.back().first;
					size_t& k = call.back().second;
					if (k == 0)
					{
						index[v] = low[v] = counter++;
						stk.push_back(v);
						onStack[v] = 1;
					}
					if (k < succ[v].size())
					{
						const int w = succ[v][k++];
						if (index[w] < 0)
							call.push_back({w, 0});
						else if (onStack[w])
							low[v] = std::min(low[v], index[w]);
						continue;
					}
					if (low[v] == index[v])
					{
						int w;
						do
						{
							w = stk.back();
							stk.pop_back();
							onStack[w] = 0;
							comp[w] = ncomp;
						} while (w != v);
						ncomp++;
					}
					call.pop_back();
					if (!call.empty())
					{
						const int u = call.back().first;
						low[u] = std::min(low[u], low[v]);
					}
				}
			}
			std::vector<u32> compSize(ncomp, 0);
			for (size_t i = 0; i < NN; i++)
				compSize[comp[i]]++;
			for (size_t i = 0; i < NN; i++)
			{
				bool cyc = compSize[comp[i]] > 1;
				for (int y : succ[i])
					cyc = cyc || y == static_cast<int>(i);
				if (cyc)
					weight[i] = 8;
			}
		}
		std::vector<u32> count(var::Count, 0);
		u32 clamped[33] = {};
		bool written[33] = {};
		u8 seen[var::Count] = {};
		std::vector<u32> touched;
		for (size_t ni = 0; ni < NN; ni++)
		{
			const auto& n = nodes_[ni];
			const u32 w = weight[ni];
			const Function& f = n->fn;
			touched.clear();
			const auto mark = [&](u32 key, u8 bit) {
				if (!seen[key])
					touched.push_back(key);
				seen[key] |= bit;
			};
			for (const Block& bl : f.blocks)
			{
				if (bl.node < 0)
					continue;
				for (ValueId id : bl.insts)
				{
					const Inst& in = f.insts[id];
					if (in.op == Op::LoadQ || in.op == Op::LoadI || in.op == Op::StoreQ || in.op == Op::StoreI)
						hasMem_ = true;
					if (in.op == Op::Leave)
					{
						const ValueId* la = in.args.data();
						for (u32 v : f.leaveArgs)
						{
							if (v >= var::Count)
								break;
							const ValueId a = la[v];
							if (!a || (f.insts[a].op == Op::LiveIn && f.insts[a].var == v))
								continue;
							mark(VecKey(v), 2);
							if (v < 132)
								written[v < 128 ? v / 4 : 32] = true;
						}
						continue;
					}
					for (ValueId a : in.args)
						if (a && f.insts[a].op == Op::LiveIn && f.insts[a].var < var::Count)
							mark(VecKey(f.insts[a].var), 1);
				}
			}
			for (u32 v : touched)
			{
				count[v] += w * ((seen[v] & 1) + ((seen[v] >> 1) & 1));
				seen[v] = 0;
			}
			for (u32 r = 1; r <= 32; r++)
				clamped[r] += n->clamped[r] ? 2 * w : 0;
		}
		std::vector<std::pair<u32, u32>> vc, gc;
		for (u32 r = 1; r <= 32; r++)
		{
			const u32 v = r == 32 ? var::ACC(0) : var::VF(r, 0);
			if (count[v] >= 2)
				vc.push_back({count[v], v});
			if (!written[r] && clamped[r] >= 2)
				vc.push_back({clamped[r], 0x1000 | r}); // a clamped home
		}
		for (u32 v = 0; v < var::Count; v++)
			if (GHomeable(v) && count[v] >= 2)
				gc.push_back({count[v], v});
		std::sort(vc.begin(), vc.end(), [](auto a, auto b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
		if (t_.dump)
			for (const auto& c : vc)
				std::printf("  home candidate %s%u: %u\n", (c.second & 0x1000) ? "clamped vf" : "var ", c.second & 0xfff, c.first);
		std::sort(gc.begin(), gc.end(), [](auto a, auto b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
		const size_t nv = std::min<size_t>({vc.size(), static_cast<size_t>(std::max(0, t_.maxVHomes)), std::size(kVHomeRegs)});
		for (size_t k = 0; k < nv; k++)
		{
			if (vc[k].second & 0x1000)
			{
				homes_.clampHome[vc[k].second & 0xff] = static_cast<s8>(kVHomeRegs[k]);
				continue;
			}
			const u32 v0 = vc[k].second;
			for (u32 l = 0; l < 4; l++)
			{
				homes_.home[v0 + l].kind = HomeKind::VReg;
				homes_.home[v0 + l].reg = static_cast<u8>(kVHomeRegs[k]);
			}
		}
		const size_t ng = std::min<size_t>({gc.size(), static_cast<size_t>(std::max(0, t_.maxGHomes)), std::size(kGHomeRegs)});
		for (size_t k = 0; k < ng; k++)
		{
			homes_.home[gc[k].second].kind = HomeKind::GReg;
			homes_.home[gc[k].second].reg = static_cast<u8>(kGHomeRegs[k]);
		}
		homes_.cycles = kCyclesReg;
		if (hasMem_)
			homes_.memBase = kMemBaseReg;
		homes_.Seal();
	}

	a64::Label* RegionBuilder::Target(int target)
	{
		if (target >= 0)
			return &nodes_[target]->head;
		return &exits_[-target - 1]->stub;
	}

	void RegionBuilder::Jump(int target, int next)
	{
		if (target >= 0 && target == next)
			return;
		armAsm->B(Target(target));
	}

	void RegionBuilder::Emit(const u8*& regionEnd)
	{
		const s32 cyclesOff = env_.offCycles;
		a64::Label loadAll, breakAll, exitAll;
		// Entries from microVU (the root's is where microVU is compiling):
		// the node's head address in x8, then the shared home loader.
		for (size_t i = 0; i < nodes_.size(); i++)
		{
			nodes_[i]->entry = armGetCurrentCodePointer();
			armAsm->Adr(a64::x8, &nodes_[i]->head);
			armAsm->B(&loadAll);
		}
		// Bodies.
		for (size_t i = 0; i < nodes_.size(); i++)
		{
			Node& n = *nodes_[i];
			armAsm->Bind(&n.head);
			// mVUtestCycles against the counter register.
			const a64::Register wc = W(kCyclesReg);
			armAsm->Subs(wc, wc, 1);
			armAsm->B(&n.brk, a64::mi);
			if (n.br.cycles > 1)
				armAsm->Sub(wc, wc, n.br.cycles - 1);
			const LowerResult r = n.low->Emit(homes_);
			pxAssertRel(r.ok, "vuopt region: node no longer lowers with region homes");
			const int next = static_cast<int>(i + 1);
			if (n.br.endKind == BlockEnd::CondBranch)
			{
				armAsm->Cmp(W(8), 0);
				armAsm->B(Target(n.taken), BranchCond(n.br.branchKind));
				Jump(n.fall, next);
			}
			else if (n.br.endKind == BlockEnd::EBit)
				EmitEnd(n);
			else if (n.br.endKind == BlockEnd::Jump && n.taken == 0x7fffffff)
				EmitJump(n);
			else
				Jump(n.taken, next);
		}
		// Cycle-budget breaks: the block, its PC and its three flag
		// instances (mVUtestCycles' operands) in x0 / w1 / w2, then the shared
		// tail.
		for (size_t i = 0; i < nodes_.size(); i++)
		{
			Node& n = *nodes_[i];
			armAsm->Bind(&n.brk);
			const int fS = GetLastFlagInst(n.br.entry, n.br.mFC.xStatus, 0, 0);
			const int fM = GetLastFlagInst(n.br.entry, n.br.mFC.xMac, 1, 0);
			const int fC = GetLastFlagInst(n.br.entry, n.br.mFC.xClip, 2, 0);
			pxAssertRel(fS >= 0 && fS < 4 && fM >= 0 && fM < 8 && fC >= 0 && fC < 8, "vuopt region: flag instance out of range");
			armMoveAddressToReg(a64::x0, n.blk);
			armAsm->Mov(a64::w1, n.pc);
			armAsm->Mov(a64::w2, static_cast<u32>(fS | (fM << 8) | (fC << 16)));
			armAsm->B(&breakAll);
		}
		// Links out of the region: the target (or its trampoline) in x8.
		for (auto& e : exits_)
		{
			armAsm->Bind(&e->stub);
			if (e->known)
			{
				armMoveAddressToReg(a64::x8, e->known);
				armAsm->B(&exitAll);
			}
			else
			{
				armAsm->Adr(a64::x8, &e->jump);
				armAsm->B(&exitAll);
			}
		}
		// Shared: load every register home from microVU's locations.
		armAsm->Bind(&loadAll);
		EmitLoadHomes(homes_, le_, cyclesOff);
		armAsm->Ldp(Q(kCMin), Q(kCMax), a64::MemOperand(a64::x25, G(&mVUglob.minvals[0])));
		EmitClampHomes(homes_, le_);
		armAsm->Br(a64::x8);
		// Shared: microVU's budget-break exit from microVU's state.
		armAsm->Bind(&breakAll);
		armAsm->Add(W(kCyclesReg), W(kCyclesReg), 1);
		EmitStoreHomes(homes_, le_, cyclesOff);
		// w9 = MAC instance fM (Ldrh, as mVUallocMFLAGa), w10 = clip instance fC
		// (mVUallocCFLAGa: past the ring it is VI[REG_CLIP_FLAG]), w11 = status
		// register fS.
		armAsm->Ubfx(a64::w3, a64::w2, 8, 8);
		armAsm->Add(a64::x4, a64::x24, a64::Operand(a64::x3, a64::LSL, 2));
		armAsm->Ldrh(a64::w9, a64::MemOperand(a64::x4));
		{
			a64::Label ring, clipDone;
			armAsm->Ubfx(a64::w3, a64::w2, 16, 8);
			armAsm->Cmp(a64::w3, 4);
			armAsm->B(&ring, a64::lo);
			armAsm->Ldr(a64::w10, a64::MemOperand(a64::x19, static_cast<s32>(offsetof(VURegs, VI) + REG_CLIP_FLAG * sizeof(REG_VI))));
			armAsm->B(&clipDone);
			armAsm->Bind(&ring);
			armAsm->Add(a64::x4, a64::x24, 16);
			armAsm->Ldr(a64::w10, a64::MemOperand(a64::x4, a64::x3, a64::LSL, 2));
			armAsm->Bind(&clipDone);
		}
		armAsm->And(a64::w3, a64::w2, 3);
		armAsm->Mov(a64::w11, a64::w20);
		armAsm->Cmp(a64::w3, 1);
		armAsm->Csel(a64::w11, a64::w21, a64::w11, a64::eq);
		armAsm->Cmp(a64::w3, 2);
		armAsm->Csel(a64::w11, a64::w22, a64::w11, a64::eq);
		armAsm->Cmp(a64::w3, 3);
		armAsm->Csel(a64::w11, a64::w23, a64::w11, a64::eq);
		armEmitJmp(env_.cycleBreak);
		// Shared: store every register home, then leave through x8.
		armAsm->Bind(&exitAll);
		EmitStoreHomes(homes_, le_, cyclesOff);
		armAsm->Br(a64::x8);
		// Near trampolines for exits microVU has yet to compile; the final
		// jump is bound after microVU compiles them, wherever that lands.
		for (auto& e : exits_)
		{
			if (e->known)
				continue;
			armAsm->Bind(&e->jump);
			armAsm->B(&e->far);
		}
		regionEnd = armGetCurrentCodePointer();
	}

	// mVUendProgram(isEbit 1) after the node's Leave: every home to microVU's
	// locations, then microVU's own end sequence.
	void RegionBuilder::EmitEnd(const Node& n)
	{
		const ir::EndInfo& e = n.fn.end;
		pxAssertRel(e.valid, "vuopt region: program end without its end info");
		EmitStoreHomes(homes_, le_, env_.offCycles);
		armEmitCall(env_.clearLpState);
		// P/Q to VI[REG_Q] / pending_q and VI[REG_P] / pending_p, choosing lanes
		// by qInst / pInst exactly as mVUendProgram rotates qmmPQ.
		const s32 offQ = static_cast<s32>(offsetof(VURegs, VI) + REG_Q * sizeof(REG_VI));
		const s32 offP = static_cast<s32>(offsetof(VURegs, VI) + REG_P * sizeof(REG_VI));
		const a64::VRegister pq = Q(28);
		if (e.qInst)
			armAsm->Ext(pq.V16B(), pq.V16B(), pq.V16B(), 4);
		armAsm->Str(a64::SRegister(28), a64::MemOperand(a64::x19, offQ));
		if (e.qInst)
			armAsm->Ext(pq.V16B(), pq.V16B(), pq.V16B(), 12);
		else
			armAsm->Ext(pq.V16B(), pq.V16B(), pq.V16B(), 4);
		armAsm->Str(a64::SRegister(28), a64::MemOperand(a64::x19, static_cast<s32>(offsetof(VURegs, pending_q))));
		if (!e.qInst)
			armAsm->Ext(pq.V16B(), pq.V16B(), pq.V16B(), 12);
		armAsm->Add(a64::x8, a64::x19, offP);
		armAsm->St1(pq.V4S(), e.pInst ? 3 : 2, a64::MemOperand(a64::x8));
		armAsm->Add(a64::x8, a64::x19, static_cast<s32>(offsetof(VURegs, pending_p)));
		armAsm->St1(pq.V4S(), e.pInst ? 2 : 3, a64::MemOperand(a64::x8));
		// MAC / clip to VI (mVUallocMFLAGa / mVUallocCFLAGa)
		armAsm->Ldrh(a64::w9, a64::MemOperand(a64::x24, e.fM * 4));
		if (e.fC < 4)
			armAsm->Ldr(a64::w10, a64::MemOperand(a64::x24, 16 + e.fC * 4));
		else
			armAsm->Ldr(a64::w10, a64::MemOperand(a64::x19, static_cast<s32>(offsetof(VURegs, VI) + REG_CLIP_FLAG * sizeof(REG_VI))));
		armAsm->Str(a64::w9, a64::MemOperand(a64::x19, static_cast<s32>(offsetof(VURegs, VI) + REG_MAC_FLAG * sizeof(REG_VI))));
		armAsm->Str(a64::w10, a64::MemOperand(a64::x19, static_cast<s32>(offsetof(VURegs, VI) + REG_CLIP_FLAG * sizeof(REG_VI))));
		pxAssertRel(e.fS >= 0 && e.fS < 4, "vuopt region: status instance out of range");
		armAsm->Mov(a64::w11, W(20 + e.fS));
		armEmitCall(env_.endFlagsB);
		armAsm->Add(a64::x8, a64::x19, static_cast<s32>(offsetof(VURegs, VI) + REG_TPC * sizeof(REG_VI)));
		armAsm->Mov(a64::w9, e.tpc);
		armAsm->Str(a64::w9, a64::MemOperand(a64::x8));
		if (!env_.threadVU1)
		{
			armMoveAddressToReg(a64::x8, env_.vpuStat);
			armAsm->Ldr(a64::w9, a64::MemOperand(a64::x8));
			armAsm->And(a64::w9, a64::w9, env_.vuIndex ? ~0x100u : ~0x001u);
			armAsm->Str(a64::w9, a64::MemOperand(a64::x8));
		}
		armEmitJmp(env_.exitEBit);
	}

	// A JR/JALR with run-time targets: the recorded ones compared directly,
	// the rest through microVU's jump cache, entered with microVU's state.
	void RegionBuilder::EmitJump(Node& n)
	{
		// normJumpCompile's probe records every jump's target in start_pc.
		if (!n.jumps.empty())
			armAsm->Str(W(8), a64::MemOperand(a64::x19, static_cast<s32>(offsetof(VURegs, start_pc))));
		for (const auto& j : n.jumps)
		{
			armAsm->Cmp(W(8), j.first);
			armAsm->B(Target(j.second), a64::eq);
		}
		armAsm->Str(W(8), a64::MemOperand(a64::x24, env_.offBranch));
		EmitStoreHomes(homes_, le_, env_.offCycles);
		env_.emitJumpTail(env_.vuIndex, n.blk, n.br.end.bytes);
	}

	bool RegionBuilder::Run(const BlockResult& root, std::string* why)
	{
		// The root: microVU registered it before calling the hook.
		{
			auto n = std::make_unique<Node>();
			n->br = root;
			if (!Lowerable(n->br, *n))
				return t_.Decline("root not internal or not lowerable", why);
			n->pc = root.startPC;
			n->key = root.entry;
			n->blk = env_.rootBlock;
			nodes_.push_back(std::move(n));
		}
		// Single-node regions without a self link gain nothing over the
		// framed body; decide before registering anything else.
		Expand(0);
		// Recorded JR targets, in the order the walk found the jumps.
		for (size_t k = 0; k < pendingJumps_.size(); k++)
		{
			const int id = pendingJumps_[k];
			const auto it = t_.jumpTargets.find(nodes_[id]->pc);
			if (it == t_.jumpTargets.end())
				continue;
			const PipeState end = nodes_[id]->br.end;
			for (u32 pc : it->second)
			{
				const int t = Visit(pc, end);
				nodes_[id]->jumps.push_back({pc & (0x4000 - 8), t});
			}
		}
		bool selfLink = nodes_[0]->taken == 0 || nodes_[0]->fall == 0;
		for (const auto& j : nodes_[0]->jumps)
			selfLink = selfLink || j.second == 0;
		if (nodes_.size() == 1 && !selfLink)
			return t_.Decline("single block, no loop", why);

		AssignHomes();
		const u8* start = armGetCurrentCodePointer();
		const u8* end = nullptr;
		Emit(end);
		pxAssertRel(start == env_.blockEntry(env_.rootBlock), "vuopt region: the root entry is not where microVU expects it");
		for (size_t i = 1; i < nodes_.size(); i++)
			env_.setBlockEntry(nodes_[i]->blk, nodes_[i]->entry);
		for (const auto& n : nodes_)
			env_.addRange(env_.vuIndex, n->pc, n->br.pcs.back());
		if (t_.dump)
		{
			std::printf("[vuopt region] root %04x: %zu nodes, %zu exits, %zu bytes\n", nodes_[0]->pc, nodes_.size(), exits_.size(),
				static_cast<size_t>(end - start));
			for (size_t i = 0; i < nodes_.size(); i++)
				std::printf("  n%zu pc=%04x cycles=%u taken=%d fall=%d entry=%p\n", i, nodes_[i]->pc, nodes_[i]->br.cycles,
					nodes_[i]->taken, nodes_[i]->br.endKind == BlockEnd::CondBranch ? nodes_[i]->fall : 0, nodes_[i]->entry);
			for (u32 v = 0; v < ir::var::Count; v++)
				if (homes_.home[v].kind != HomeKind::Mem && (v >= 132 || (v & 3) == 0))
					std::printf("  home %s -> %c%u\n", ir::var::Name(v).c_str(), homes_.home[v].kind == HomeKind::VReg ? 'v' : 'w',
						homes_.home[v].reg);
			for (u32 r = 1; r <= 32; r++)
				if (homes_.clampHome[r] >= 0)
					std::printf("  clamped home %s -> v%d\n", r == 32 ? "acc" : ("vf" + std::to_string(r)).c_str(), homes_.clampHome[r]);
			std::fflush(stdout);
			vixl::aarch64::PrintDisassembler dis(stdout);
			dis.DisassembleBuffer(reinterpret_cast<const vixl::aarch64::Instruction*>(start),
				reinterpret_cast<const vixl::aarch64::Instruction*>(end));
			std::fflush(stdout);
		}
		t_.regions_++;
		t_.nodes_ += static_cast<u32>(nodes_.size());
		t_.exits_ += static_cast<u32>(exits_.size());
		t_.bytes_ += static_cast<u64>(end - start);
		// microVU compiles the exits now, in the order the walk met them.
		for (auto& e : exits_)
		{
			if (e->known)
				continue;
			void* target = env_.fetchBlock(env_.vuIndex, e->pc, e->key.bytes);
			armAsm->Bind(&e->far);
			armEmitJmp(target);
		}
		return true;
	}

	bool RegionTier::Decline(const std::string& why, std::string* out)
	{
		declined_[why]++;
		if (out)
			*out = why;
		return false;
	}

	bool RegionTier::OnCompile(const MvuBlockTrace& b, const MvuEmitEnv& env, std::string* why)
	{
		if (b.vuIndex != vu_)
			return false;
		compiles_++;
		if (!an_)
		{
			PipeConfig cfg;
			cfg.vuIndex = vu_;
			cfg.flagHack = env.flagHack;
			cfg.xgkickHack = env.xgkickHack;
			cfg.ibitHack = env.ibitHack;
			an_ = std::make_unique<PipeAnalyzer>(micro_, cfg);
		}
		PipeState st;
		std::memcpy(st.bytes, b.entryState, 96);
		BlockResult br = an_->Analyze(b.startPC, st);
		MvuBlockTrace ours;
		br.ToTrace(ours, vu_);
		if (!CompareBlockTraces(b, ours).empty())
			return Decline("timing port disagrees", why);
		if (env.vuIndex != 1)
			return Decline("VU0", why);
		if (env.clampExact)
			return Decline("clamp mode 4", why);
		if (env.xgkickHack || env.ibitHack || env.overflowHack || env.addSubHack || env.vuSyncHack)
			return Decline("gamefix", why);
		if (!env.searchBlock || !env.cycleBreak || !env.emitJumpTail)
			return Decline("no region support in microVU", why);
		if (maxRegions >= 0 && static_cast<s32>(regions_) >= maxRegions)
			return Decline("bisect", why);

		LowerEnv le;
		le.vuIndex = env.vuIndex;
		le.offDivFlag = env.offDivFlag;
		le.offVIbackup = env.offVIbackup;
		le.offVIxgkick = env.offVIxgkick;
		le.offBranch = env.offBranch;
		le.offNeonBackup = env.offNeonBackup;
		le.xgkickFn = env.xgkickFn;
		le.vifTop = env.vifTop;
		le.vifItop = env.vifItop;
		le.clampOverflow = env.clampOverflow;
		le.clampExtra = env.clampExtra;
		le.clampSign = env.clampSign;
		le.softwareFlush = env.softwareFlush;
		RegionBuilder rb(*this, env, le);
		return rb.Run(br, why);
	}
} // namespace vuopt
