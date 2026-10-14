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
#include <bitset>
#include <cstring>

namespace vuopt
{
	namespace a64 = vixl::aarch64;

	namespace
	{
		constexpr int kCMax = 25, kCMin = 26;
		constexpr int kCyclesReg = 28;
		// A bail record: PC (u32), pad, the block's microBlock (its pipeline
		// state), the address of microVU's own code for the block once
		// compiled, then the microProgram the block belongs to.
		constexpr s32 kBailSlot = 16;
		constexpr u32 kBailRecordSize = 32;
		constexpr int kMemBaseReg = 27;
		constexpr int kVHomeRegs[] = {31, 30, 29, 27, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15};
		constexpr int kGHomeRegs[] = {26, 15, 14, 13, 12, 11};
		// With the sticky accumulators (LowerEnv::stickyAcc) these registers, the
		// top of the allocation pools, are theirs for the whole region. (Taken from
		// the home lists instead they cost the loops more than the accumulators
		// saved: the homes they displaced were read in every block.)
		constexpr int kSaZReg = 14, kSaSReg = 13, kStkReg = 10;

		// The ring slot holding a block's newest STATUS instance when it is entered
		// with this key: the slot before the one the next status write takes.
		int StatusHead(const PipeState& k)
		{
			const int next = (k.needExactMatch & 1) ? 0 : ((k.flagInfo >> 2) & 3);
			return (next - 1) & 3;
		}

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
			LiveSet liveOut;                   // variables live at the node's end (milestone 8)
			LiveSet liveIn;                    // variables live at the node's head
			int map = 0;                       // its home map (maps_)
			int taken = 0x7fffffff, fall = 0x7fffffff; // >= 0: node; < 0: exit -(k+1)
			std::vector<std::pair<u32, int>> jumps;    // JR/JALR: recorded target PC -> node / exit
			a64::Label head;
			void* entry = nullptr;
			// Milestone 8: the cycle budget is tested only at checkpoints (loop
			// heads) and at entries from microVU, against what the run can
			// spend before the next test; short of it, the run goes on in
			// microVU's own code for this block (bail).
			bool checkpoint = false;
			u32 need = 1;                  // budget that guarantees no microVU test fails before the next test
			a64::Label bail, rec;
		};
		struct Exit
		{
			u32 pc = 0;
			PipeState key;
			void* known = nullptr;
			a64::Label jump, far;
			std::vector<std::unique_ptr<a64::Label>> stubs; // per home map: store that map's homes, then leave
		};
		struct Edge // a link between nodes of different home maps
		{
			int map, to;
			std::unique_ptr<a64::Label> label;
		};
		std::vector<Edge> edges_;

		RegionTier& t_;
		const MvuEmitEnv& env_;
		LowerEnv le_;
		bool acc_ = false; // sticky STATUS accumulation in this region
		std::vector<std::unique_ptr<Node>> nodes_;
		std::vector<std::unique_ptr<Exit>> exits_;
		std::vector<HomeMap> maps_; // per map: the homes its nodes keep variables in (milestone 10)
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
			return NodeLowering::Lowerable(n.fn, le_);
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
		void ChooseHomes(int map, const std::vector<u32>& weight);
		void PlanChecks();
		void ComputeLiveness();
		bool WantsAcc() const;
		void FindLoops(std::vector<int>& comp, std::vector<u8>& cyclic, int& ncomp) const;
		void ComputeDirty();
		bool AccPays();
		void DirtyIn(const std::vector<NodeLowering::ChainEffect>& eff, std::vector<u8>& in) const;
		void Emit(const u8*& regionEnd);
		void EmitEnd(const Node& n);
		void EmitJump(int idx);
		std::vector<int> pendingJumps_; // JR nodes whose recorded targets are resolved after the static walk
		void Jump(int from, int target, int next);
		a64::Label* Target(int from, int target);
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

	// Milestone 8: where the cycle budget is tested. microVU tests it at every
	// block's entry (mVUtestCycles: break when it is below 1, then spend the
	// block's cycles). A run that starts a stretch of blocks with at least
	// the most any path through the stretch spends before its last block,
	// plus one, passes every one of those tests, so only the stretch's head
	// needs one. Stretches end at checkpoints, the targets of the region's
	// back edges (every cycle passes through one), and at exits, where
	// microVU's own blocks test again. Every entry from microVU is tested the
	// same way.
	void RegionBuilder::PlanChecks()
	{
		const size_t NN = nodes_.size();
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
		// back edges of a depth-first walk from the root
		std::vector<u8> state(NN, 0); // 0 new, 1 on stack, 2 done
		std::vector<std::pair<int, size_t>> st;
		for (size_t root = 0; root < NN; root++)
		{
			if (state[root])
				continue;
			st.push_back({static_cast<int>(root), 0});
			state[root] = 1;
			while (!st.empty())
			{
				const int v = st.back().first;
				const size_t k = st.back().second;
				if (k < succ[v].size())
				{
					st.back().second++;
					const int w = succ[v][k];
					if (state[w] == 1)
						nodes_[w]->checkpoint = true;
					else if (state[w] == 0)
					{
						state[w] = 1;
						st.push_back({w, 0});
					}
					continue;
				}
				state[v] = 2;
				st.pop_back();
			}
		}
		// need(n) = max(1, cycles(n) + need(s)) over successors s that are
		// not checkpoints; the stretch below a node is acyclic.
		std::vector<s64> need(NN, -1);
		std::vector<std::pair<int, size_t>> walk;
		for (size_t root = 0; root < NN; root++)
		{
			if (need[root] >= 0)
				continue;
			walk.push_back({static_cast<int>(root), 0});
			while (!walk.empty())
			{
				const int v = walk.back().first;
				size_t& k = walk.back().second;
				bool descended = false;
				while (k < succ[v].size())
				{
					const int w = succ[v][k];
					if (!nodes_[w]->checkpoint && need[w] < 0)
					{
						walk.push_back({w, 0});
						descended = true;
						break;
					}
					k++;
				}
				if (descended)
					continue;
				s64 m = 1;
				for (int w : succ[v])
					if (!nodes_[w]->checkpoint)
						m = std::max<s64>(m, static_cast<s64>(nodes_[v]->br.cycles) + need[w]);
				need[v] = m;
				walk.pop_back();
				if (!walk.empty())
					walk.back().second++;
			}
		}
		for (size_t i = 0; i < NN; i++)
			nodes_[i]->need = static_cast<u32>(std::min<s64>(need[i], 0x7fffffff));
	}

	// Milestone 8: which variables each node's end must hand on. Between the
	// budget tests nothing outside the region sees the state, so a final value
	// that every path overwrites before the next observation is dead: the
	// observations are exits to microVU (links, JR fallbacks, program ends)
	// and the heads of checkpoints (a bail there hands everything to
	// microVU). A node's incoming uses are the incoming values it reads
	// (other than passing its own variable through).
	void RegionBuilder::ComputeLiveness()
	{
		using namespace ir;
		using Bits = LiveSet;
		const size_t NN = nodes_.size();
		std::vector<Bits> use(NN), def(NN), in(NN), out(NN);
		std::vector<u8> observe(NN, 0);
		Bits all;
		all.set();
		std::vector<Bits> obsMask(NN, all); // what an observation at the node's end sees
		std::vector<std::vector<int>> succ(NN);
		for (size_t i = 0; i < NN; i++)
		{
			Node& n = *nodes_[i];
			const Function& f = n.fn;
			for (const Block& bl : f.blocks)
			{
				if (bl.node < 0)
					continue;
				for (ValueId id : bl.insts)
				{
					const Inst& x = f.insts[id];
					if (x.op == Op::Leave)
					{
						for (u32 k : f.leaveArgs)
						{
							const ValueId a = x.args[k];
							if (!a)
								continue;
							const Inst& ai = f.insts[a];
							const bool passThrough = k < var::Count && ai.op == Op::LiveIn && ai.var == k;
							if (k < var::Count && !passThrough)
								def[i].set(k);
							if (!passThrough && ai.op == Op::LiveIn && ai.var < var::Count)
								use[i].set(ai.var);
						}
						continue;
					}
					// (with the sticky accumulators a status update reads the chain
					// word, not the previous instance: only readers use an instance)
					const bool chain = acc_ && (x.op == Op::StatusUpd || x.op == Op::StatusDiv || x.op == Op::StatusSet);
					for (size_t k = chain ? 1 : 0; k < x.args.size(); k++)
					{
						const ValueId a = x.args[k];
						if (a && f.insts[a].op == Op::LiveIn && f.insts[a].var < var::Count)
							use[i].set(f.insts[a].var);
					}
				}
			}
			const auto edge = [&](int t) {
				if (t == 0x7fffffff)
					return;
				if (t < 0 || nodes_[t]->checkpoint)
					observe[i] = 1;
				else
					succ[i].push_back(t);
			};
			if (n.br.endKind == BlockEnd::EBit)
			{
				// A program end hands microVU one instance of each flag (fS /
				// fM / fC, broadcast to the whole ring by mVUendProgram); the
				// other instances are dead. A MAC index past the ring reads the
				// clip instance there (microVU's layout).
				observe[i] = 1;
				const ir::EndInfo& e = n.fn.end;
				if (e.valid)
				{
					Bits m = all;
					for (u32 k = 0; k < 4; k++)
					{
						m.reset(var::S(k));
						m.reset(var::M(k));
						m.reset(var::C(k));
					}
					m.set(var::S(e.fS & 3));
					if (e.fM < 4)
						m.set(var::M(e.fM));
					else
						m.set(var::C((e.fM - 4) & 3));
					if (e.fC < 4)
						m.set(var::C(e.fC));
					obsMask[i] = m;
				}
			}
			else if (n.br.endKind == BlockEnd::Jump && n.taken == 0x7fffffff)
			{
				observe[i] = 1; // the fallback to microVU's dispatch
				for (const auto& j : n.jumps)
					edge(j.second);
			}
			else
			{
				edge(n.taken);
				if (n.br.endKind == BlockEnd::CondBranch)
					edge(n.fall);
			}
		}
		for (bool changed = true; changed;)
		{
			changed = false;
			for (size_t ii = NN; ii-- > 0;)
			{
				Bits o;
				if (observe[ii])
					o = obsMask[ii];
				for (int t : succ[ii])
					o |= in[t];
				o.set(var::MEM);
				const Bits ni = use[ii] | (o & ~def[ii]);
				if (o != out[ii] || ni != in[ii])
				{
					out[ii] = o;
					in[ii] = ni;
					changed = true;
				}
			}
		}
		for (size_t i = 0; i < NN; i++)
		{
			nodes_[i]->liveOut = out[i];
			nodes_[i]->liveIn = in[i];
		}
	}

	// Whether a node has a status update the accumulators can take (a sticky-only
	// update in the status layout with more than one lane).
	bool RegionBuilder::WantsAcc() const
	{
		using namespace ir;
		bool any = false;
		for (const auto& n : nodes_)
			for (const Inst& x : n->fn.insts)
				if (x.op == Op::StatusUpd && !(x.imm & 3) && x.mask != 0)
					any = true;
		if (!any)
			return false;
		// every status chain must be what the lowering carries in registers
		for (const auto& n : nodes_)
			if (!StatusChainLinear(n->fn))
			{
				t_.accNonLinear_++;
				return false;
			}
		return true;
	}

	// Where the accumulators may owe the chain word something (LowerEnv::stickyAcc):
	// forward over the links, clean at every entry from microVU, dirty after a node
	// that accumulates, clean after one that builds an instance.
	void RegionBuilder::DirtyIn(const std::vector<NodeLowering::ChainEffect>& eff, std::vector<u8>& in) const
	{
		const size_t NN = nodes_.size();
		in.assign(NN, 0);
		for (bool changed = true; changed;)
		{
			changed = false;
			for (size_t i = 0; i < NN; i++)
			{
				const Node& n = *nodes_[i];
				if (!eff[i].dirtyOut[in[i]])
					continue;
				const auto mark = [&](int t) {
					if (t >= 0 && t != 0x7fffffff && !in[t])
					{
						in[t] = 1;
						changed = true;
					}
				};
				mark(n.taken);
				if (n.br.endKind == BlockEnd::CondBranch)
					mark(n.fall);
				for (const auto& j : n.jumps)
					mark(j.second);
			}
		}
	}

	void RegionBuilder::ComputeDirty()
	{
		const size_t NN = nodes_.size();
		std::vector<NodeLowering::ChainEffect> eff(NN);
		for (size_t i = 0; i < NN; i++)
		{
			eff[i] = nodes_[i]->low->Chain();
			t_.accAccumulated_ += eff[i].accumulated;
		}
		t_.accRegions_++;
		std::vector<u8> in;
		DirtyIn(eff, in);
		for (size_t i = 0; i < NN; i++)
			nodes_[i]->low->SetDirtyIn(in[i] != 0);
	}

	// Whether the accumulators save instructions in this region. An accumulated
	// update replaces a pack and an OR (about eight instructions) by three or four;
	// an instance built while the accumulators owe the chain something costs a
	// reduce (about seven). A divide-heavy region builds many instances right after
	// a few accumulations and can lose; blocks in a loop count eight times.
	bool RegionBuilder::AccPays()
	{
		const size_t NN = nodes_.size();
		std::vector<NodeLowering::ChainEffect> eff(NN);
		for (size_t i = 0; i < NN; i++)
			eff[i] = nodes_[i]->low->Chain();
		std::vector<u8> in;
		DirtyIn(eff, in);
		std::vector<int> comp;
		std::vector<u8> cyclic;
		int ncomp = 0;
		FindLoops(comp, cyclic, ncomp);
		s64 net = 0;
		for (size_t i = 0; i < NN; i++)
			net += static_cast<s64>(cyclic[i] ? 8 : 1) * (5 * static_cast<s64>(eff[i].accumulated) - 7 * static_cast<s64>(eff[i].reduces[in[i]]));
		if (t_.dump)
			std::printf("[vuopt acc] root %04x: %zu nodes, net %lld\n", nodes_[0]->pc, NN, static_cast<long long>(net));
		return net > 0;
	}

	// The region's loops: each node's strongly connected component and whether it
	// is in a loop (its component has more than one node, or it links to itself).
	void RegionBuilder::FindLoops(std::vector<int>& comp, std::vector<u8>& cyclic, int& ncomp) const
	{
		const size_t NN = nodes_.size();
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
		// Tarjan, iterative.
		std::vector<int> index(NN, -1), low(NN, 0), stk;
		comp.assign(NN, -1);
		std::vector<u8> onStack(NN, 0);
		std::vector<std::pair<int, size_t>> call;
		int counter = 0;
		ncomp = 0;
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
		cyclic.assign(NN, 0);
		for (size_t i = 0; i < NN; i++)
		{
			bool cyc = compSize[comp[i]] > 1;
			for (int y : succ[i])
				cyc = cyc || y == static_cast<int>(i);
			cyclic[i] = cyc;
		}
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
			std::vector<int> comp;
			std::vector<u8> cyclic;
			int ncomp = 0;
			FindLoops(comp, cyclic, ncomp);
			// Milestone 10: each loop (a cyclic strongly connected component)
			// gets its own homes, chosen for its own nodes; the rest share one
			// map. Links between maps transfer the live variables.
			std::vector<int> compMap(ncomp, -1);
			int nmaps = 1;
			for (size_t i = 0; i < NN; i++)
			{
				const bool cyc = cyclic[i] != 0;
				if (cyc)
					weight[i] = 8;
				int m = 0;
				if (cyc && t_.splitHomes)
				{
					if (compMap[comp[i]] < 0)
						compMap[comp[i]] = nmaps++;
					m = compMap[comp[i]];
				}
				nodes_[i]->map = m;
			}
			maps_.assign(nmaps, HomeMap());
		}
		for (size_t m = 0; m < maps_.size(); m++)
			ChooseHomes(static_cast<int>(m), weight);
	}

	void RegionBuilder::ChooseHomes(int map, const std::vector<u32>& weight)
	{
		using namespace ir;
		const size_t NN = nodes_.size();
		HomeMap& homes = maps_[map];
		std::vector<u32> count(var::Count, 0);
		u32 vf0Uses = 0;
		u32 clamped[33] = {};
		bool written[33] = {};
		u8 seen[var::Count] = {};
		std::vector<u32> touched;
		for (size_t ni = 0; ni < NN; ni++)
		{
			const auto& n = nodes_[ni];
			if (map != 0 && n->map != map)
				continue; // a loop's map counts its own nodes; the region's map counts all
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
							if (v < 132)
								written[v < 128 ? v / 4 : 32] = true; // any write ends a clamped home's validity
							if (n->liveOut.test(v))
								mark(VecKey(v), 2);
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
			// VF0 read in the node: one load saved by a VF0 register
			for (const Inst& x : f.insts)
				if (x.op == Op::Const && x.type == Type::Vec)
				{
					vf0Uses += w;
					break;
				}
		}
		std::vector<std::pair<u32, u32>> vc, gc;
		if (vf0Uses >= 2)
			vc.push_back({vf0Uses, 0x2000}); // a VF0 register
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
				std::printf("  map %d home candidate %s%u: %u\n", map, (c.second & 0x1000) ? "clamped vf" : "var ", c.second & 0xfff, c.first);
		std::sort(gc.begin(), gc.end(), [](auto a, auto b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
		const size_t nvMax = std::min<size_t>(static_cast<size_t>(std::max(0, t_.maxVHomes)), std::size(kVHomeRegs));
		const size_t ngMax = std::min<size_t>(static_cast<size_t>(std::max(0, t_.maxGHomes)), std::size(kGHomeRegs));
		// Register -> key: a vector variable's lane 0, 0x1000|r for VF r's
		// clamped copy, 0x2000 for VF0; a general variable.
		std::vector<u32> vKey(nvMax, 0), gKey(ngMax, 0);
		if (map == 0)
		{
			for (size_t k = 0; k < nvMax && k < vc.size(); k++)
				vKey[k] = vc[k].second;
			for (size_t k = 0; k < ngMax && k < gc.size(); k++)
				gKey[k] = gc[k].second;
		}
		else
		{
			// A loop starts from the region's map and swaps a home only for a
			// candidate that saves clearly more inside the loop: every
			// difference costs a move at each entry to and exit from the loop.
			const HomeMap& base = maps_[0];
			for (size_t k = 0; k < nvMax; k++)
			{
				const int reg = kVHomeRegs[k];
				if (base.vf0Reg == reg)
					vKey[k] = 0x2000;
				for (u32 r = 1; r <= 32; r++)
				{
					if (base.clampHome[r] == reg && !written[r])
						vKey[k] = 0x1000 | r;
					const u32 v0 = r == 32 ? var::ACC(0) : var::VF(r, 0);
					if (base.home[v0].kind == HomeKind::VReg && base.home[v0].reg == reg)
						vKey[k] = v0;
				}
			}
			for (size_t k = 0; k < ngMax; k++)
				for (u32 v = 0; v < var::Count; v++)
					if (GHomeable(v) && base.home[v].kind == HomeKind::GReg && base.home[v].reg == kGHomeRegs[k])
						gKey[k] = v;
			const auto improve = [&](std::vector<u32>& keys, const std::vector<std::pair<u32, u32>>& cands) {
				const auto scoreOf = [&](u32 key) -> u32 {
					for (const auto& c : cands)
						if (c.second == key)
							return c.first;
					return 0;
				};
				for (const auto& c : cands)
				{
					if (std::find(keys.begin(), keys.end(), c.second) != keys.end())
						continue;
					size_t victim = keys.size();
					u32 vs = ~0u;
					for (size_t k = 0; k < keys.size(); k++)
					{
						const u32 sc = keys[k] ? scoreOf(keys[k]) : 0;
						if (sc < vs)
						{
							vs = sc;
							victim = k;
						}
					}
					if (victim < keys.size() && c.first > vs + (keys[victim] ? 8u : 0u))
						keys[victim] = c.second;
				}
			};
			improve(vKey, vc);
			improve(gKey, gc);
		}
		std::vector<u8> homed(0x2001 + 1, 0);
		for (size_t k = 0; k < nvMax; k++)
		{
			const u32 key = vKey[k];
			if (!key)
				continue;
			homed[std::min<u32>(key, 0x2000)] = 1;
			if (key == 0x2000)
			{
				homes.vf0Reg = static_cast<s8>(kVHomeRegs[k]);
				continue;
			}
			if (key & 0x1000)
			{
				homes.clampHome[key & 0xff] = static_cast<s8>(kVHomeRegs[k]);
				continue;
			}
			for (u32 l = 0; l < 4; l++)
			{
				homes.home[key + l].kind = HomeKind::VReg;
				homes.home[key + l].reg = static_cast<u8>(kVHomeRegs[k]);
			}
		}
		// clamped copies that got no register: up to twelve in memory
		{
			int slot = 0;
			for (size_t k = 0; k < vc.size() && slot < 12; k++)
				if ((vc[k].second & 0x1000) && !(vc[k].second & 0x2000) && !homed[vc[k].second])
					homes.clampSlot[vc[k].second & 0xff] = static_cast<s8>(slot++);
		}
		for (size_t k = 0; k < ngMax; k++)
		{
			if (!gKey[k])
				continue;
			homes.home[gKey[k]].kind = HomeKind::GReg;
			homes.home[gKey[k]].reg = static_cast<u8>(kGHomeRegs[k]);
		}
		homes.cycles = kCyclesReg;
		if (hasMem_)
			homes.memBase = le_.vuMemPinned ? -1 : kMemBaseReg;
		if (acc_)
		{
			homes.stkReg = kStkReg;
			homes.saZ = kSaZReg;
			homes.saS = kSaSReg;
		}
		homes.Seal();
	}

	// Where a link from node `from` goes: the target's head when both use
	// the same home map, else a transfer stub; an exit's stub for from's map.
	a64::Label* RegionBuilder::Target(int from, int target)
	{
		const int m = nodes_[from]->map;
		if (target < 0)
		{
			Exit& e = *exits_[-target - 1];
			if (e.stubs.size() < maps_.size())
				e.stubs.resize(maps_.size());
			if (!e.stubs[m])
				e.stubs[m] = std::make_unique<a64::Label>();
			return e.stubs[m].get();
		}
		if (nodes_[target]->map == m)
			return &nodes_[target]->head;
		for (Edge& e : edges_)
			if (e.map == m && e.to == target)
				return e.label.get();
		edges_.push_back({m, target, std::make_unique<a64::Label>()});
		return edges_.back().label.get();
	}

	void RegionBuilder::Jump(int from, int target, int next)
	{
		if (target >= 0 && target == next && nodes_[target]->map == nodes_[from]->map)
			return;
		armAsm->B(Target(from, target));
	}

	void RegionBuilder::Emit(const u8*& regionEnd)
	{
		const s32 cyclesOff = env_.offCycles;
		a64::Label bailEntry, bailCompile;
		std::vector<a64::Label> loadAll(maps_.size()), exitAll(maps_.size());
		// Entries from microVU (the root's is where microVU is compiling): the
		// budget test, then the node's head address in x8 and the shared home
		// loader.
		for (size_t i = 0; i < nodes_.size(); i++)
		{
			Node& n = *nodes_[i];
			n.entry = armGetCurrentCodePointer();
			armAsm->Adr(a64::x1, &n.rec);
			armAsm->Ldr(W(8), a64::MemOperand(a64::x24, cyclesOff));
			armAsm->Cmp(W(8), n.need);
			armAsm->B(&bailEntry, a64::lt);
			if (acc_)
			{
				// microVU hands over every STATUS instance whole: the chain starts
				// from the newest, with nothing owed by the accumulators.
				armAsm->Mov(W(kStkReg), W(20 + StatusHead(n.key)));
				armAsm->Movi(Q(kSaZReg).V2D(), 0);
				armAsm->Movi(Q(kSaSReg).V2D(), 0);
			}
			armAsm->Adr(a64::x8, &n.head);
			armAsm->B(&loadAll[n.map]);
		}
		// Bodies.
		for (size_t i = 0; i < nodes_.size(); i++)
		{
			Node& n = *nodes_[i];
			armAsm->Bind(&n.head);
			size_t pi = 0;
			std::vector<std::pair<const u8*, ir::ValueId>> marks;
			if (t_.profile)
			{
				// analysis only: an execution counter (not part of the profile)
				pi = t_.prof_.size();
				t_.prof_.emplace_back();
				t_.prof_[pi].pc = n.pc;
				t_.prof_[pi].count = std::make_unique<u64>(0);
				armMoveAddressToReg(a64::x16, t_.prof_[pi].count.get());
				armAsm->Ldr(a64::x17, a64::MemOperand(a64::x16));
				armAsm->Add(a64::x17, a64::x17, 1);
				armAsm->Str(a64::x17, a64::MemOperand(a64::x16));
			}
			const u8* headStart = armGetCurrentCodePointer();
			// The budget test at a checkpoint (PlanChecks), then the block's
			// cycles off the counter register.
			const a64::Register wc = W(kCyclesReg);
			if (n.checkpoint)
			{
				armAsm->Cmp(wc, n.need);
				armAsm->B(&n.bail, a64::lt);
			}
			if (n.br.cycles)
				armAsm->Sub(wc, wc, n.br.cycles);
			const u8* bodyStart = armGetCurrentCodePointer();
			if (t_.profile)
			{
				t_.prof_[pi].segs.push_back({headStart, bodyStart, "head"});
				g_lowerMarks = &marks;
			}
			const LowerResult r = n.low->Emit(maps_[n.map]);
			g_lowerMarks = nullptr;
			pxAssertRel(r.ok, "vuopt region: node no longer lowers with region homes");
			const u8* tailStart = armGetCurrentCodePointer();
			if (t_.profile)
			{
				const u8* at = bodyStart;
				std::string tag = "entry";
				for (const auto& m : marks)
				{
					if (m.first > at)
						t_.prof_[pi].segs.push_back({at, m.first, tag});
					at = m.first;
					tag = ir::OpName(n.fn.insts[m.second].op);
				}
				if (tailStart > at)
					t_.prof_[pi].segs.push_back({at, tailStart, tag});
			}
			const int next = static_cast<int>(i + 1);
			if (n.br.endKind == BlockEnd::CondBranch)
			{
				armAsm->Cmp(W(8), 0);
				armAsm->B(Target(static_cast<int>(i), n.taken), BranchCond(n.br.branchKind));
				Jump(static_cast<int>(i), n.fall, next);
			}
			else if (n.br.endKind == BlockEnd::EBit)
				EmitEnd(n);
			else if (n.br.endKind == BlockEnd::Jump && n.taken == 0x7fffffff)
				EmitJump(static_cast<int>(i));
			else
				Jump(static_cast<int>(i), n.taken, next);
			if (t_.profile && armGetCurrentCodePointer() > tailStart)
				t_.prof_[pi].segs.push_back({tailStart, armGetCurrentCodePointer(), "tail"});
		}
		// Bails: homes back to microVU's locations (from a checkpoint), then
		// microVU's own code for the block, compiled the first time it is
		// needed; its record holds the PC, the pipeline state and that code's
		// address once there is one.
		for (size_t i = 0; i < nodes_.size(); i++)
		{
			Node& n = *nodes_[i];
			if (!n.checkpoint)
				continue;
			armAsm->Bind(&n.bail);
			armAsm->Adr(a64::x1, &n.rec);
			armAsm->Adr(a64::x8, &bailEntry);
			armAsm->B(&exitAll[n.map]);
		}
		// Shared: the record in x1, state in microVU's locations.
		armAsm->Bind(&bailEntry);
		armAsm->Ldr(a64::x0, a64::MemOperand(a64::x1, kBailSlot));
		armAsm->Cbz(a64::x0, &bailCompile);
		armAsm->Br(a64::x0);
		// Links out of the region: the target (or its trampoline) in x8.
		for (auto& e : exits_)
			for (size_t m = 0; m < e->stubs.size(); m++)
			{
				if (!e->stubs[m])
					continue;
				armAsm->Bind(e->stubs[m].get());
				if (e->known)
					armMoveAddressToReg(a64::x8, e->known);
				else
					armAsm->Adr(a64::x8, &e->jump);
				armAsm->B(&exitAll[m]);
			}
		// Links between home maps: the live variables to the target's map.
		for (Edge& e : edges_)
		{
			armAsm->Bind(e.label.get());
			const Node& t = *nodes_[e.to];
			EmitHomeTransfer(maps_[e.map], maps_[t.map], t.checkpoint ? nullptr : &t.liveIn, le_);
			armAsm->B(&nodes_[e.to]->head);
		}
		// Per map: load every register home from microVU's locations.
		for (size_t m = 0; m < maps_.size(); m++)
		{
			armAsm->Bind(&loadAll[m]);
			EmitLoadHomes(maps_[m], le_, cyclesOff);
			armAsm->Ldp(Q(kCMin), Q(kCMax), a64::MemOperand(a64::x25, G(&mVUglob.minvals[0])));
			EmitClampHomes(maps_[m], le_);
			armAsm->Br(a64::x8);
		}
		// Shared: compile microVU's code for a bailed block (record in x1)
		// and enter it, qmmPQ and the clamp bounds kept across the call.
		armAsm->Bind(&bailCompile);
		armAsm->Str(a64::q28, a64::MemOperand(a64::x24, env_.offNeonBackup + 28 * 16));
		armAsm->Mov(a64::w0, env_.vuIndex);
		armEmitCall(reinterpret_cast<const void*>(env_.bailTarget));
		armAsm->Ldr(a64::q28, a64::MemOperand(a64::x24, env_.offNeonBackup + 28 * 16));
		armAsm->Ldp(Q(kCMin), Q(kCMax), a64::MemOperand(a64::x25, G(&mVUglob.minvals[0])));
		armAsm->Br(a64::x0);
		// Per map: store every register home, then leave through x8.
		for (size_t m = 0; m < maps_.size(); m++)
		{
			armAsm->Bind(&exitAll[m]);
			EmitStoreHomes(maps_[m], le_, cyclesOff);
			armAsm->Br(a64::x8);
		}
		// Near trampolines for exits microVU has yet to compile; the final
		// jump is bound after microVU compiles them, wherever that lands.
		for (auto& e : exits_)
		{
			if (e->known)
				continue;
			armAsm->Bind(&e->jump);
			armAsm->B(&e->far);
		}
		// Bail records (data).
		for (auto& np : nodes_)
		{
			Node& n = *np;
			armAsm->Bind(&n.rec);
			vixl::ExactAssemblyScope scope(armAsm, kBailRecordSize);
			armAsm->dc32(n.pc);
			armAsm->dc32(0);
			armAsm->dc64(reinterpret_cast<u64>(n.blk));
			armAsm->dc64(0);
			armAsm->dc64(reinterpret_cast<u64>(env_.program));
		}
		regionEnd = armGetCurrentCodePointer();
	}

	// mVUendProgram(isEbit 1) after the node's Leave: every home to microVU's
	// locations, then microVU's own end sequence.
	void RegionBuilder::EmitEnd(const Node& n)
	{
		const ir::EndInfo& e = n.fn.end;
		pxAssertRel(e.valid, "vuopt region: program end without its end info");
		EmitStoreHomes(maps_[n.map], le_, env_.offCycles);
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
	void RegionBuilder::EmitJump(int idx)
	{
		Node& n = *nodes_[idx];
		// normJumpCompile's probe records every jump's target in start_pc.
		if (!n.jumps.empty())
			armAsm->Str(W(8), a64::MemOperand(a64::x19, static_cast<s32>(offsetof(VURegs, start_pc))));
		for (const auto& j : n.jumps)
		{
			armAsm->Cmp(W(8), j.first);
			armAsm->B(Target(idx, j.second), a64::eq);
		}
		armAsm->Str(W(8), a64::MemOperand(a64::x24, env_.offBranch));
		EmitStoreHomes(maps_[n.map], le_, env_.offCycles);
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

		PlanChecks();
		acc_ = t_.stickyAcc && WantsAcc();
		for (int pass = 0; pass < 2; pass++)
		{
			le_.stickyAcc = acc_;
			ComputeLiveness();
			for (auto& n : nodes_)
			{
				n->low = NodeLowering::Check(n->fn, le_, n->clamped, &n->liveOut);
				pxAssertRel(n->low != nullptr, "vuopt region: a lowerable node failed its analysis");
			}
			// (the analysis ran with the accumulators on; a region they do not pay in
			// is analysed again without)
			if (pass == 0 && acc_ && !t_.accAlways && !AccPays())
			{
				t_.accUnprofitable_++;
				acc_ = false;
				continue;
			}
			break;
		}
		if (acc_)
			ComputeDirty();
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
			for (size_t m = 0; m < maps_.size(); m++)
			{
				const HomeMap& hm = maps_[m];
				for (u32 v = 0; v < ir::var::Count; v++)
					if (hm.home[v].kind != HomeKind::Mem && (v >= 132 || (v & 3) == 0))
						std::printf("  map %zu home %s -> %c%u\n", m, ir::var::Name(v).c_str(), hm.home[v].kind == HomeKind::VReg ? 'v' : 'w',
							hm.home[v].reg);
				for (u32 r = 1; r <= 32; r++)
					if (hm.clampHome[r] >= 0)
						std::printf("  map %zu clamped home %s -> v%d\n", m, r == 32 ? "acc" : ("vf" + std::to_string(r)).c_str(), hm.clampHome[r]);
			}
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

	void RegionTier::PrintProfile(FILE* out) const
	{
		vixl::aarch64::Disassembler dis;
		vixl::aarch64::Decoder decoder;
		decoder.AppendVisitor(&dis);
		for (const ProfNode& p : prof_)
		{
			std::fprintf(out, "N %04x %llu\n", p.pc, static_cast<unsigned long long>(*p.count));
			for (const ProfSeg& s : p.segs)
				for (const u8* a = s.start; a < s.end; a += 4)
				{
					decoder.Decode(reinterpret_cast<const vixl::aarch64::Instruction*>(a));
					std::fprintf(out, "I %s %s\n", s.tag.c_str(), dis.GetOutput());
				}
		}
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
		if (!env.searchBlock || !env.bailTarget || !env.emitJumpTail)
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
		le.schedule = schedule;
		le.vuMemPinned = env.vuMemPinned;
		RegionBuilder rb(*this, env, le);
		return rb.Run(br, why);
	}
} // namespace vuopt
