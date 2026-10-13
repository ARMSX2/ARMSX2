// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// pcsx2-vurunner --vuopt-ir
//
// For each capture: replay it through microVU (JIT only) with the first-pass
// hook armed, collecting every block microVU compiles together with its
// decisions. Then run vuopt's port of that pass on the same program image,
// block by block in microVU's compile order and from the same entry state,
// and compare every decision field. Output is one summary line per capture
// plus the mismatching fields; a final line aggregates the run.

#include "VuOptMode.h"

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuReplay.h"

#include "Config.h"
#include "arm64/vuopt/VuOptMvuTrace.h"
#include "arm64/vuopt/VuOptGraph.h"
#include "arm64/vuopt/VuOptIR.h"
#include "arm64/vuopt/VuOptPipe.h"
#include "common/PmuCounters.h"

#include <chrono>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	struct HookSink
	{
		u32 vuIndex = 1;
		std::vector<vuopt::MvuBlockTrace> blocks;
	};

	void OnBlock(const vuopt::MvuBlockTrace& b, void* user)
	{
		HookSink* s = static_cast<HookSink*>(user);
		if (b.vuIndex == s->vuIndex)
			s->blocks.push_back(b);
	}

	// Field name of a mismatch line, for the per-class histogram.
	std::string MismatchClass(const std::string& line)
	{
		// "op N pc 0xPPPP field: ..." or "block field: ..."
		size_t colon = line.find(':');
		size_t start = 0;
		if (line.rfind("op ", 0) == 0)
		{
			size_t pc = line.find(" pc 0x");
			start = line.find(' ', pc + 6) + 1;
		}
		else if (line.rfind("block ", 0) == 0)
		{
			start = 6;
		}
		std::string f = line.substr(start, colon - start);
		const size_t br = f.find('[');
		if (br != std::string::npos)
			f = f.substr(0, br);
		return f;
	}
} // namespace

int RunVuoptIr(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuoptModeOptions& opts,
	void (*applyConfig)(const vu_capture::CaptureRecord&))
{
	u64 totBlocks = 0, totMatched = 0, totOps = 0;
	std::map<std::string, u64> totClasses;
	int failures = 0;

	for (size_t fi = 0; fi < records.size(); ++fi)
	{
		const auto& rec = records[fi];
		applyConfig(rec);
		EmuConfig.Speedhacks.vuFlagHack = opts.flagHack != 0;

		HookSink sink;
		sink.vuIndex = rec.vu_index;
		vuopt::g_mvuBlockHook = &OnBlock;
		vuopt::g_mvuBlockHookUser = &sink;
		const auto r = recompiler_tests::ReplayJitOnly(rec, opts.cycleOverride);
		vuopt::g_mvuBlockHook = nullptr;
		vuopt::g_mvuBlockHookUser = nullptr;
		if (!r.ok)
		{
			std::printf("[vuopt] %s replay-failed\n", names[fi].c_str());
			failures++;
			continue;
		}

		vuopt::PipeConfig cfg;
		cfg.vuIndex = rec.vu_index;
		cfg.flagHack = opts.flagHack != 0;
		cfg.xgkickHack = EmuConfig.Gamefixes.XgKickHack;
		cfg.ibitHack = EmuConfig.Gamefixes.IbitHack;
		vuopt::PipeAnalyzer an(rec.microcode.data(), cfg);

		u64 matched = 0, ops = 0;
		std::map<std::string, u64> classes;
		std::vector<std::string> firstLines;
		for (const auto& mb : sink.blocks)
		{
			vuopt::PipeState entry;
			std::memcpy(entry.bytes, mb.entryState, 96);
			const vuopt::BlockResult br = an.Analyze(mb.startPC, entry);
			vuopt::MvuBlockTrace ours;
			br.ToTrace(ours, rec.vu_index);
			const auto diffs = vuopt::CompareBlockTraces(mb, ours);
			ops += mb.ops.size();
			if (diffs.empty())
			{
				matched++;
				continue;
			}
			std::map<std::string, int> seen;
			for (const auto& d : diffs)
			{
				const std::string c = MismatchClass(d);
				if (!seen[c]++)
					classes[c]++;
				if (opts.verbose || firstLines.size() < 6)
				{
					char head[48];
					std::snprintf(head, sizeof(head), "block@%04x: ", mb.startPC);
					firstLines.push_back(head + d);
				}
			}
		}

		std::printf("[vuopt] %s vu%u pc=%04x blocks=%zu match=%llu ops=%llu ebit=%d",
			names[fi].c_str(), rec.vu_index, rec.start_pc, sink.blocks.size(),
			static_cast<unsigned long long>(matched), static_cast<unsigned long long>(ops), r.jit_ebit ? 1 : 0);
		for (const auto& c : classes)
			std::printf(" %s:%llu", c.first.c_str(), static_cast<unsigned long long>(c.second));
		std::printf("\n");
		for (const auto& l : firstLines)
			std::printf("  %s\n", l.c_str());

		// ---- Whole-program graph + SSA ----
		// Entry: the capture's start PC with the key microVU dispatched it with.
		vuopt::GraphOptions gopt;
		gopt.cfg = cfg;
		{
			vuopt::PipeState entry;
			for (const auto& mb : sink.blocks)
			{
				if (mb.startPC == rec.start_pc)
				{
					std::memcpy(entry.bytes, mb.entryState, 96);
					break;
				}
			}
			gopt.entries.push_back({rec.start_pc, entry});
		}
		// Pass 1, static only; then feed the PCs of microVU blocks the static
		// graph did not reach as observed jump targets (the record half of
		// record-then-recompile).
		vuopt::ProgramGraph graph = vuopt::BuildProgramGraph(rec.microcode.data(), gopt);
		{
			std::vector<u32> extra;
			for (const auto& mb : sink.blocks)
			{
				vuopt::PipeState st;
				std::memcpy(st.bytes, mb.entryState, 96);
				if (graph.Find(mb.startPC, st, cfg.flagHack) < 0 &&
					std::find(extra.begin(), extra.end(), mb.startPC) == extra.end())
					extra.push_back(mb.startPC);
			}
			if (!extra.empty())
			{
				gopt.recordedJumpTargets = extra;
				graph = vuopt::BuildProgramGraph(rec.microcode.data(), gopt);
			}
		}
		u32 covered = 0, pcOnly = 0, missing = 0;
		for (const auto& mb : sink.blocks)
		{
			vuopt::PipeState st;
			std::memcpy(st.bytes, mb.entryState, 96);
			if (graph.Find(mb.startPC, st, cfg.flagHack) >= 0)
				covered++;
			else
			{
				bool anyPc = false;
				for (const auto& n : graph.nodes)
					anyPc |= (n.pc == mb.startPC);
				(anyPc ? pcOnly : missing)++;
			}
		}
		// How many variants would emit identical code: same PC, same decisions
		// for every instruction, same link-time ring/lane permutation. With
		// and without counting stall cycles (which only change the cycle
		// constant a block charges).
		u32 codeClasses = 0, codeClassesNoStall = 0;
		{
			std::map<std::vector<u32>, int> withStall, noStall;
			for (const auto& n : graph.nodes)
			{
				vuopt::MvuBlockTrace t;
				n.block.ToTrace(t, rec.vu_index);
				std::vector<u32> sigA, sigB;
				sigA.push_back(n.pc);
				int perm[3][4];
				vuopt::SortFlag(n.block.mFC.xStatus, perm[0], n.block.mFC.cycles);
				vuopt::SortFlag(n.block.mFC.xMac, perm[1], n.block.mFC.cycles);
				vuopt::SortFlag(n.block.mFC.xClip, perm[2], n.block.mFC.cycles);
				sigA.push_back(n.block.end.needExactMatch);
				for (int a = 0; a < 3; a++)
					for (int b = 0; b < 4; b++)
						sigA.push_back((n.block.end.needExactMatch >> a) & 1 ? perm[a][b] : 9);
				sigA.push_back(n.block.qLane | (n.block.pLane << 1));
				for (auto o : t.ops)
				{
					const u8 stall = o.stall;
					o.stall = 0;
					o.XGKICKPC = 0;
					o.kickcycles = 0;
					const u32* w = reinterpret_cast<const u32*>(&o);
					for (size_t k = 0; k < sizeof(o) / 4; k++)
						sigA.push_back(w[k]);
					sigB.push_back(stall);
				}
				noStall.emplace(sigA, 0);
				sigA.insert(sigA.end(), sigB.begin(), sigB.end());
				withStall.emplace(sigA, 0);
			}
			codeClasses = static_cast<u32>(withStall.size());
			codeClassesNoStall = static_cast<u32>(noStall.size());
		}
		const vuopt::GraphStats& gs = graph.stats;
		std::printf("[vuopt-graph] nodes=%u blockpcs=%u pairs=%u insts=%u pcbbs=%u edges=%u back=%u irr=%u pcback=%u pcirr=%u "
					"jr=%u jrstatic=%u jrrec=%u jrunres=%u cond=%u br=%u ebit=%u tbit=%u evil=%u trunc=%d "
					"mvu_cover=%u/%zu pconly=%u missing=%u codeclasses=%u codeclasses_nostall=%u\n",
			gs.nodes, gs.distinctBlockPCs, gs.reachedPairs, gs.instructions, gs.pcBasicBlocks, gs.edges, gs.backEdges,
			gs.irreducibleEdges, gs.pcBackEdges, gs.pcIrreducibleEdges, gs.jumpSites, gs.jumpSitesStatic, gs.jumpSitesRecorded,
			gs.jumpSitesUnresolved, gs.condBranches, gs.branches, gs.ebitExits, gs.tbitExits, gs.evilJumps, gs.truncated ? 1 : 0,
			covered, sink.blocks.size(), pcOnly, missing, codeClasses, codeClassesNoStall);

		vuopt::ir::BuildOptions bopt;
		bopt.vuIndex = rec.vu_index;
		bopt.clampIImmediate = (rec.vu_index ? EmuConfig.Cpu.Recompiler.vu1Overflow && !EmuConfig.Cpu.Recompiler.vu1SignOverflow
											 : EmuConfig.Cpu.Recompiler.vu0Overflow && !EmuConfig.Cpu.Recompiler.vu0SignOverflow);
		bopt.xgkickHack = cfg.xgkickHack;
		bopt.ibitHack = cfg.ibitHack;
		bopt.yieldChecks = !opts.noYield;
		vuopt::ir::Function fn;
		u64 ssaInsns = 0;
		const auto t0 = std::chrono::steady_clock::now();
		{
			PmuCounters::Group pmu;
			if (pmu.Open() && pmu.IsAvailable(PmuCounters::InstructionsRetired))
			{
				const auto v = pmu.Measure([&]() { fn = vuopt::ir::BuildSSA(graph, bopt); });
				ssaInsns = v[PmuCounters::InstructionsRetired];
			}
			else
				fn = vuopt::ir::BuildSSA(graph, bopt);
		}
		const auto t1 = std::chrono::steady_clock::now();
		const auto problems = vuopt::ir::Verify(fn);
		std::printf("[vuopt-ssa] blocks=%zu insts=%u phis=%u phis_created=%u trivial_removed=%u scc_removed=%u "
					"verify=%zu build_insns=%llu build_us=%lld badops=%u%s%s\n",
			fn.blocks.size(), fn.LiveInsts(), fn.LivePhis(), fn.phisCreated, fn.phisRemovedTrivial, fn.phisRemovedScc,
			problems.size(), static_cast<unsigned long long>(ssaInsns),
			static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()), fn.badOps,
			fn.unsupported ? " unsupported=" : "", fn.unsupported ? fn.unsupportedWhy.c_str() : "");
		for (size_t k = 0; k < problems.size() && k < 5; k++)
			std::printf("  verify: %s\n", problems[k].c_str());
		if (opts.dumpIr)
			std::fputs(vuopt::ir::Dump(fn, graph).c_str(), stdout);

		totBlocks += sink.blocks.size();
		totMatched += matched;
		totOps += ops;
		for (const auto& c : classes)
			totClasses[c.first] += c.second;
	}

	std::printf("[vuopt-total] captures=%zu blocks=%llu match=%llu ops=%llu",
		records.size(), static_cast<unsigned long long>(totBlocks),
		static_cast<unsigned long long>(totMatched), static_cast<unsigned long long>(totOps));
	for (const auto& c : totClasses)
		std::printf(" %s:%llu", c.first.c_str(), static_cast<unsigned long long>(c.second));
	std::printf("\n");
	return failures ? 4 : 0;
}
