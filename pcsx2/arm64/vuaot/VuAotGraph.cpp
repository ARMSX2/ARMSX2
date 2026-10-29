// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuAotGraph.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>

namespace vuaot
{
	namespace
	{
		bool QuickMatch(const PipeState& a, const PipeState& b, bool flagHack)
		{
			if (flagHack)
				return (a.quick64[0] & ~0x3002ull) == (b.quick64[0] & ~0x3002ull);
			return a.quick64[0] == b.quick64[0];
		}

		// A VI value lattice: a small set of constants, or unknown.
		struct ViVal
		{
			bool top = false;
			bool init = false; // false = no value has reached here yet (bottom)
			std::vector<u16> vals;

			static ViVal Top() { ViVal v; v.top = true; v.init = true; return v; }
			static ViVal Const(u16 c) { ViVal v; v.init = true; v.vals.push_back(c); return v; }
			bool operator==(const ViVal& o) const { return top == o.top && init == o.init && vals == o.vals; }
			void Join(const ViVal& o)
			{
				if (!o.init || top)
					return;
				if (!init)
				{
					*this = o;
					return;
				}
				if (o.top)
				{
					*this = Top();
					return;
				}
				std::vector<u16> m;
				std::set_union(vals.begin(), vals.end(), o.vals.begin(), o.vals.end(), std::back_inserter(m));
				if (m.size() > 16)
					*this = Top();
				else
					vals = std::move(m);
			}
			template <typename F>
			static ViVal Map2(const ViVal& a, const ViVal& b, F f)
			{
				if (!a.init || !b.init)
					return ViVal();
				if (a.top || b.top)
					return Top();
				ViVal r;
				r.init = true;
				for (u16 x : a.vals)
					for (u16 y : b.vals)
						r.vals.push_back(static_cast<u16>(f(x, y)));
				std::sort(r.vals.begin(), r.vals.end());
				r.vals.erase(std::unique(r.vals.begin(), r.vals.end()), r.vals.end());
				if (r.vals.size() > 16)
					return Top();
				return r;
			}
		};

		using ViState = std::array<ViVal, 16>;

		ViState TopState()
		{
			ViState s;
			for (auto& v : s)
				v = ViVal::Top();
			s[0] = ViVal::Const(0);
			return s;
		}

		class GraphBuilder
		{
		public:
			GraphBuilder(const u8* micro, const GraphOptions& opts)
				: opts_(opts)
				, an_(micro, opts.cfg)
				, byPC_(opts.cfg.microMemSize() / 8)
			{
			}

			ProgramGraph Build();

		private:
			const GraphOptions& opts_;
			PipeAnalyzer an_;
			ProgramGraph g_;
			std::vector<std::vector<int>> byPC_;
			bool truncated_ = false;

			u32 MemMask() const { return opts_.cfg.microMemSize() - 8; }
			int Search(u32 pc, const PipeState& st) const;
			int Fetch(u32 pc, const PipeState& st);
			int Compile(u32 pc, const PipeState& st);
			void RebuildPreds();
			bool ResolveJumpsStatic();
			void Stats();
		};

		int GraphBuilder::Search(u32 pc, const PipeState& st) const
		{
			const auto& list = byPC_[pc / 8];
			if (st.needExactMatch)
			{
				for (int id : list)
				{
					const GraphNode& n = g_.nodes[id];
					if (n.entry.needExactMatch && n.entry == st)
						return id;
				}
				return -1;
			}
			for (int id : list)
			{
				if (QuickMatch(g_.nodes[id].entry, st, opts_.cfg.flagHack))
					return id;
			}
			return -1;
		}

		int GraphBuilder::Fetch(u32 pc, const PipeState& st)
		{
			pc &= MemMask();
			const int id = Search(pc, st);
			if (id >= 0)
				return id;
			return Compile(pc, st);
		}

		int GraphBuilder::Compile(u32 pc, const PipeState& st)
		{
			if (g_.nodes.size() >= opts_.maxNodes)
			{
				truncated_ = true;
				return -1;
			}
			const int id = static_cast<int>(g_.nodes.size());
			g_.nodes.emplace_back();
			g_.nodes[id].id = id;
			g_.nodes[id].pc = pc;
			g_.nodes[id].entry = st;
			byPC_[pc / 8].push_back(id);

			BlockResult b = an_.Analyze(pc, st);
			const BlockEnd kind = b.endKind;
			const PipeState end = b.end;
			const u32 takenPC = b.takenPC, fallPC = b.fallPC;
			const u32 lastPC = b.pcs.back();
			g_.nodes[id].block = std::move(b);

			switch (kind)
			{
				case BlockEnd::Branch:
				{
					const int t = Fetch(takenPC, end);
					g_.nodes[id].taken = t;
					break;
				}
				case BlockEnd::CondBranch:
				{
					// condBranch: the not-taken block first, then the taken one.
					int f = Search(fallPC, end);
					if (f < 0)
						f = Compile(fallPC, end);
					g_.nodes[id].fall = f;
					const int t = Fetch(takenPC, end);
					g_.nodes[id].taken = t;
					break;
				}
				case BlockEnd::MBit:
				{
					const int t = Fetch((lastPC + 8) & MemMask(), end);
					g_.nodes[id].taken = t;
					break;
				}
				default:
					break;
			}
			return id;
		}

		void GraphBuilder::RebuildPreds()
		{
			for (auto& n : g_.nodes)
				n.preds.clear();
			for (auto& n : g_.nodes)
			{
				auto add = [&](int s) {
					if (s >= 0)
						g_.nodes[s].preds.push_back(static_cast<int>(n.id));
				};
				add(n.taken);
				add(n.fall);
				for (int t : n.jumpTargets)
					add(t);
			}
		}

		// VI constant propagation over the graph; returns true if it added a
		// jump edge (and so possibly nodes).
		bool GraphBuilder::ResolveJumpsStatic()
		{
			const size_t N = g_.nodes.size();
			std::vector<ViState> in(N), out(N);
			std::vector<bool> isRoot(N, false);
			for (int r : g_.roots)
				isRoot[r] = true;
			std::vector<int> work;
			for (size_t i = 0; i < N; i++)
				work.push_back(static_cast<int>(i));
			std::vector<bool> queued(N, true);
			std::vector<ViVal> jumpVal(N);

			auto transfer = [&](int id, ViState s) {
				const BlockResult& b = g_.nodes[id].block;
				for (u32 i = 0; i < b.count; i++)
				{
					const MvuOp& o = b.ops[i];
					const InstPair& p = b.insts[i];
					if (o.uOp.iBit)
						continue;
					const LowerInst& l = p.lower;
					if (l.isJump())
						jumpVal[id] = s[l.is];
					if (o.lOp.isNOP)
						continue;
					const u32 link = ((p.pc + 16) & MemMask()) / 8;
					switch (l.op)
					{
						case LOp::IADDI:
							s[l.it] = ViVal::Map2(s[l.is], ViVal::Const(0), [&](u16 a, u16) { return a + l.imm5; });
							break;
						case LOp::IADDIU:
							s[l.it] = ViVal::Map2(s[l.is], ViVal::Const(0), [&](u16 a, u16) { return a + l.imm15; });
							break;
						case LOp::ISUBIU:
							s[l.it] = ViVal::Map2(s[l.is], ViVal::Const(0), [&](u16 a, u16) { return a - l.imm15; });
							break;
						case LOp::IADD:
							s[l.id] = ViVal::Map2(s[l.is], s[l.it], [](u16 a, u16 c) { return a + c; });
							break;
						case LOp::ISUB:
							s[l.id] = ViVal::Map2(s[l.is], s[l.it], [](u16 a, u16 c) { return a - c; });
							break;
						case LOp::IAND:
							s[l.id] = ViVal::Map2(s[l.is], s[l.it], [](u16 a, u16 c) { return a & c; });
							break;
						case LOp::IOR:
							s[l.id] = ViVal::Map2(s[l.is], s[l.it], [](u16 a, u16 c) { return a | c; });
							break;
						case LOp::BAL:
						case LOp::JALR:
							if (l.it && !o.lOp.evilBranch)
								s[l.it] = ViVal::Const(static_cast<u16>(link));
							else if (l.it)
								s[l.it] = ViVal::Top();
							break;
						case LOp::LQI: case LOp::SQI:
						{
							const u8 r = (l.op == LOp::LQI) ? l.is : l.it;
							s[r] = ViVal::Map2(s[r], ViVal::Const(0), [](u16 a, u16) { return a + 1; });
							break;
						}
						case LOp::LQD: case LOp::SQD:
						{
							const u8 r = (l.op == LOp::LQD) ? l.is : l.it;
							s[r] = ViVal::Map2(s[r], ViVal::Const(0), [](u16 a, u16) { return a - 1; });
							break;
						}
						default:
							if (o.lOp.VI_write.reg)
								s[o.lOp.VI_write.reg] = ViVal::Top();
							break;
					}
					s[0] = ViVal::Const(0);
				}
				return s;
			};

			RebuildPreds();
			while (!work.empty())
			{
				const int id = work.back();
				work.pop_back();
				queued[id] = false;
				ViState s;
				if (isRoot[id])
					s = TopState();
				for (int p : g_.nodes[id].preds)
					for (int r = 0; r < 16; r++)
						s[r].Join(out[p][r]);
				in[id] = s;
				ViState o = transfer(id, s);
				bool changed = false;
				for (int r = 0; r < 16; r++)
					if (!(o[r] == out[id][r]))
						changed = true;
				if (!changed)
					continue;
				out[id] = o;
				const GraphNode& n = g_.nodes[id];
				auto push = [&](int s2) {
					if (s2 >= 0 && !queued[s2])
					{
						queued[s2] = true;
						work.push_back(s2);
					}
				};
				push(n.taken);
				push(n.fall);
				for (int t : n.jumpTargets)
					push(t);
			}

			bool added = false;
			for (size_t id = 0; id < N; id++)
			{
				GraphNode& n = g_.nodes[id];
				if (n.block.endKind != BlockEnd::Jump)
					continue;
				const ViVal& v = jumpVal[id];
				if (!v.init || v.top)
					continue;
				for (u16 c : v.vals)
				{
					const u32 tpc = (static_cast<u32>(c) * 8) & MemMask();
					const PipeState end = n.block.end;
					const int t = Fetch(tpc, end);
					if (t < 0)
						continue;
					GraphNode& nn = g_.nodes[id]; // Fetch may have grown the vector
					if (std::find(nn.jumpTargets.begin(), nn.jumpTargets.end(), t) == nn.jumpTargets.end())
					{
						nn.jumpTargets.push_back(t);
						added = true;
					}
				}
				g_.jumpResolution[id] = JumpResolution::Static;
			}
			// Mark jumps the propagation could not bound.
			g_.jumpResolution.resize(g_.nodes.size(), JumpResolution::Unresolved);
			for (size_t id = 0; id < N; id++)
			{
				if (g_.nodes[id].block.endKind != BlockEnd::Jump)
					continue;
				const ViVal& v = jumpVal[id];
				if (!v.init || v.top)
					g_.jumpResolution[id] = JumpResolution::Unresolved;
			}
			return added;
		}

		// Cooper/Harvey/Kennedy dominators + DFS retreating edges over a graph
		// given as successor lists, with a virtual root feeding `roots`.
		void LoopStats(const std::vector<std::vector<int>>& succ, const std::vector<int>& roots, u32& back, u32& irr)
		{
			const int N = static_cast<int>(succ.size());
			const int V = N; // virtual root id
			std::vector<std::vector<int>> s2(N + 1);
			for (int i = 0; i < N; i++)
				s2[i] = succ[i];
			s2[V] = roots;
			std::vector<int> state(N + 1, 0), post, order(N + 1, -1);
			std::vector<std::pair<int, int>> retreat;
			std::vector<std::pair<int, size_t>> st;
			st.push_back({V, 0});
			state[V] = 1;
			while (!st.empty())
			{
				auto& [u, k] = st.back();
				if (k < s2[u].size())
				{
					const int v = s2[u][k++];
					if (state[v] == 0)
					{
						state[v] = 1;
						st.push_back({v, 0});
					}
					else if (state[v] == 1)
						retreat.push_back({u, v});
				}
				else
				{
					state[u] = 2;
					post.push_back(u);
					st.pop_back();
				}
			}
			std::vector<int> rpoIndex(N + 1, -1);
			std::vector<int> rpo(post.rbegin(), post.rend());
			for (size_t i = 0; i < rpo.size(); i++)
				rpoIndex[rpo[i]] = static_cast<int>(i);
			std::vector<std::vector<int>> pred(N + 1);
			for (int u = 0; u <= N; u++)
				if (rpoIndex[u] >= 0)
					for (int v : s2[u])
						pred[v].push_back(u);
			std::vector<int> idom(N + 1, -1);
			idom[V] = V;
			bool changed = true;
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
			back = irr = 0;
			for (auto [u, v] : retreat)
			{
				if (dominates(v, u))
					back++;
				else
					irr++;
			}
		}

		void GraphBuilder::Stats()
		{
			GraphStats& s = g_.stats;
			s.nodes = static_cast<u32>(g_.nodes.size());
			s.truncated = truncated_;
			std::set<u32> pcs, covered;
			std::vector<std::vector<int>> succ(g_.nodes.size());
			std::map<u32, JumpResolution> sites;
			for (const GraphNode& n : g_.nodes)
			{
				pcs.insert(n.pc);
				s.instructions += n.block.count;
				for (u32 p : n.block.pcs)
					covered.insert(p);
				auto add = [&](int t) {
					if (t >= 0)
					{
						succ[n.id].push_back(t);
						s.edges++;
					}
				};
				add(n.taken);
				add(n.fall);
				for (int t : n.jumpTargets)
					add(t);
				switch (n.block.endKind)
				{
					case BlockEnd::CondBranch: s.condBranches++; break;
					case BlockEnd::Branch: s.branches++; break;
					case BlockEnd::EBit: s.ebitExits++; break;
					case BlockEnd::EvilJump: s.evilJumps++; break;
					case BlockEnd::Jump:
					{
						const u32 site = n.block.pcs[n.block.branchIndex];
						const JumpResolution r = g_.jumpResolution[n.id];
						auto it = sites.find(site);
						if (it == sites.end())
							sites[site] = r;
						else if (static_cast<int>(r) > static_cast<int>(it->second))
							it->second = r; // the weakest resolution over the site's variants
						break;
					}
					default: break;
				}
				s.tbitExits += static_cast<u32>(n.block.tBitExits.size()) + (n.block.tBitOnBranch ? 1 : 0);
			}
			s.distinctBlockPCs = static_cast<u32>(pcs.size());
			s.reachedPairs = static_cast<u32>(covered.size());
			s.jumpSites = static_cast<u32>(sites.size());
			for (auto& [pc, r] : sites)
			{
				if (r == JumpResolution::Static) s.jumpSitesStatic++;
				else if (r == JumpResolution::Recorded) s.jumpSitesRecorded++;
				else s.jumpSitesUnresolved++;
			}
			std::vector<int> roots = g_.roots;
			LoopStats(succ, roots, s.backEdges, s.irreducibleEdges);

			// PC-level CFG: leaders are block start PCs.
			std::map<u32, int> leader;
			for (u32 p : pcs)
				leader.emplace(p, static_cast<int>(leader.size()));
			s.pcBasicBlocks = static_cast<u32>(leader.size());
			std::vector<std::set<int>> psucc(leader.size());
			for (const GraphNode& n : g_.nodes)
			{
				int cur = leader[n.pc];
				for (size_t i = 1; i < n.block.pcs.size(); i++)
				{
					auto it = leader.find(n.block.pcs[i]);
					if (it != leader.end())
					{
						psucc[cur].insert(it->second);
						cur = it->second;
					}
				}
				auto add = [&](int t) {
					if (t >= 0)
						psucc[cur].insert(leader[g_.nodes[t].pc]);
				};
				add(n.taken);
				add(n.fall);
				for (int t : n.jumpTargets)
					add(t);
			}
			std::vector<std::vector<int>> pv(psucc.size());
			for (size_t i = 0; i < psucc.size(); i++)
				pv[i].assign(psucc[i].begin(), psucc[i].end());
			std::vector<int> proots;
			for (int r : g_.roots)
				proots.push_back(leader[g_.nodes[r].pc]);
			std::sort(proots.begin(), proots.end());
			proots.erase(std::unique(proots.begin(), proots.end()), proots.end());
			LoopStats(pv, proots, s.pcBackEdges, s.pcIrreducibleEdges);
		}

		ProgramGraph GraphBuilder::Build()
		{
			for (const auto& [pc, st] : opts_.entries)
			{
				const int id = Fetch(pc, st);
				if (id >= 0 && std::find(g_.roots.begin(), g_.roots.end(), id) == g_.roots.end())
				{
					g_.roots.push_back(id);
					g_.nodes[id].isRoot = true;
				}
			}
			g_.jumpResolution.assign(g_.nodes.size(), JumpResolution::Unresolved);

			if (opts_.staticJumpResolution)
			{
				for (int round = 0; round < 32; round++)
				{
					g_.jumpResolution.resize(g_.nodes.size(), JumpResolution::Unresolved);
					if (!ResolveJumpsStatic())
						break;
				}
			}
			g_.jumpResolution.resize(g_.nodes.size(), JumpResolution::Unresolved);

			// Jumps left unbounded take the run-time observed targets. Those can
			// be incomplete, so the jump keeps its bail-to-microVU exit.
			for (size_t id = 0; id < g_.nodes.size(); id++)
			{
				if (g_.nodes[id].block.endKind != BlockEnd::Jump || g_.jumpResolution[id] == JumpResolution::Static)
					continue;
				g_.nodes[id].jumpUnresolved = true;
				if (opts_.recordedJumpTargets.empty())
					continue;
				for (u32 tpc : opts_.recordedJumpTargets)
				{
					const PipeState end = g_.nodes[id].block.end;
					const int t = Fetch(tpc, end);
					if (t < 0)
						continue;
					auto& jt = g_.nodes[id].jumpTargets;
					if (std::find(jt.begin(), jt.end(), t) == jt.end())
						jt.push_back(t);
				}
				g_.jumpResolution.resize(g_.nodes.size(), JumpResolution::Unresolved);
				g_.jumpResolution[id] = JumpResolution::Recorded;
			}
			g_.jumpResolution.resize(g_.nodes.size(), JumpResolution::Unresolved);
			RebuildPreds();
			Stats();
			return std::move(g_);
		}
	} // namespace

	int ProgramGraph::Find(u32 pc, const PipeState& st, bool flagHack) const
	{
		for (const GraphNode& n : nodes)
		{
			if (n.pc != pc)
				continue;
			if (st.needExactMatch ? (n.entry.needExactMatch && n.entry == st) : QuickMatch(n.entry, st, flagHack))
				return static_cast<int>(n.id);
		}
		return -1;
	}

	ProgramGraph BuildProgramGraph(const u8* micro, const GraphOptions& opts)
	{
		GraphBuilder b(micro, opts);
		return b.Build();
	}
} // namespace vuaot
