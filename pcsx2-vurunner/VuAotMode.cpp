// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// pcsx2-vurunner --vuaot-ir
//
// For each capture: replay it through microVU (JIT only) with the first-pass
// hook armed, collecting every block microVU compiles together with its
// decisions. Then run vu-aot's port of that pass on the same program image,
// block by block in microVU's compile order and from the same entry state,
// and compare every decision field. Output is one summary line per capture
// plus the mismatching fields; a final line aggregates the run.

#include "VuAotMode.h"

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuReplay.h"

#include "Config.h"
#include "arm64/vuaot/VuAotMvuTrace.h"
#include "arm64/vuaot/VuAotGraph.h"
#include "arm64/vuaot/VuAotIR.h"
#include "arm64/vuaot/VuAotPipe.h"
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
		std::vector<vuaot::MvuBlockTrace> blocks;
	};

	void OnBlock(const vuaot::MvuBlockTrace& b, void* user)
	{
		HookSink* s = static_cast<HookSink*>(user);
		if (b.vuIndex == s->vuIndex)
			s->blocks.push_back(b);
	}

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

int RunVuAotIr(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuAotModeOptions& opts,
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
		vuaot::g_mvuBlockHook = &OnBlock;
		vuaot::g_mvuBlockHookUser = &sink;
		const auto r = recompiler_tests::ReplayJitOnly(rec, opts.cycleOverride);
		vuaot::g_mvuBlockHook = nullptr;
		vuaot::g_mvuBlockHookUser = nullptr;
		if (!r.ok)
		{
			std::printf("[vuaot] %s replay-failed\n", names[fi].c_str());
			failures++;
			continue;
		}

		vuaot::PipeConfig cfg;
		cfg.vuIndex = rec.vu_index;
		cfg.flagHack = opts.flagHack != 0;
		cfg.xgkickHack = EmuConfig.Gamefixes.XgKickHack;
		cfg.ibitHack = EmuConfig.Gamefixes.IbitHack;
		vuaot::PipeAnalyzer an(rec.microcode.data(), cfg);

		u64 matched = 0, ops = 0;
		std::map<std::string, u64> classes;
		std::vector<std::string> firstLines;
		for (const auto& mb : sink.blocks)
		{
			vuaot::PipeState entry;
			std::memcpy(entry.bytes, mb.entryState, 96);
			const vuaot::BlockResult br = an.Analyze(mb.startPC, entry);
			vuaot::MvuBlockTrace ours;
			br.ToTrace(ours, rec.vu_index);
			const auto diffs = vuaot::CompareBlockTraces(mb, ours);
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

		std::printf("[vuaot] %s vu%u pc=%04x blocks=%zu match=%llu ops=%llu ebit=%d",
			names[fi].c_str(), rec.vu_index, rec.start_pc, sink.blocks.size(),
			static_cast<unsigned long long>(matched), static_cast<unsigned long long>(ops), r.jit_ebit ? 1 : 0);
		for (const auto& c : classes)
			std::printf(" %s:%llu", c.first.c_str(), static_cast<unsigned long long>(c.second));
		std::printf("\n");
		for (const auto& l : firstLines)
			std::printf("  %s\n", l.c_str());

		// Entry: the capture's start PC with the key microVU dispatched it with.
		vuaot::GraphOptions gopt;
		gopt.cfg = cfg;
		{
			vuaot::PipeState entry;
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
		vuaot::ProgramGraph graph = vuaot::BuildProgramGraph(rec.microcode.data(), gopt);
		{
			std::vector<u32> extra;
			for (const auto& mb : sink.blocks)
			{
				vuaot::PipeState st;
				std::memcpy(st.bytes, mb.entryState, 96);
				if (graph.Find(mb.startPC, st, cfg.flagHack) < 0 &&
					std::find(extra.begin(), extra.end(), mb.startPC) == extra.end())
					extra.push_back(mb.startPC);
			}
			if (!extra.empty())
			{
				gopt.recordedJumpTargets = extra;
				graph = vuaot::BuildProgramGraph(rec.microcode.data(), gopt);
			}
		}
		u32 covered = 0, pcOnly = 0, missing = 0;
		for (const auto& mb : sink.blocks)
		{
			vuaot::PipeState st;
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
				vuaot::MvuBlockTrace t;
				n.block.ToTrace(t, rec.vu_index);
				std::vector<u32> sigA, sigB;
				sigA.push_back(n.pc);
				int perm[3][4];
				vuaot::SortFlag(n.block.mFC.xStatus, perm[0], n.block.mFC.cycles);
				vuaot::SortFlag(n.block.mFC.xMac, perm[1], n.block.mFC.cycles);
				vuaot::SortFlag(n.block.mFC.xClip, perm[2], n.block.mFC.cycles);
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
		const vuaot::GraphStats& gs = graph.stats;
		std::printf("[vuaot-graph] nodes=%u blockpcs=%u pairs=%u insts=%u pcbbs=%u edges=%u back=%u irr=%u pcback=%u pcirr=%u "
					"jr=%u jrstatic=%u jrrec=%u jrunres=%u cond=%u br=%u ebit=%u tbit=%u evil=%u trunc=%d "
					"mvu_cover=%u/%zu pconly=%u missing=%u codeclasses=%u codeclasses_nostall=%u\n",
			gs.nodes, gs.distinctBlockPCs, gs.reachedPairs, gs.instructions, gs.pcBasicBlocks, gs.edges, gs.backEdges,
			gs.irreducibleEdges, gs.pcBackEdges, gs.pcIrreducibleEdges, gs.jumpSites, gs.jumpSitesStatic, gs.jumpSitesRecorded,
			gs.jumpSitesUnresolved, gs.condBranches, gs.branches, gs.ebitExits, gs.tbitExits, gs.evilJumps, gs.truncated ? 1 : 0,
			covered, sink.blocks.size(), pcOnly, missing, codeClasses, codeClassesNoStall);

		vuaot::ir::BuildOptions bopt;
		bopt.vuIndex = rec.vu_index;
		bopt.clampIImmediate = (rec.vu_index ? EmuConfig.Cpu.Recompiler.vu1Overflow && !EmuConfig.Cpu.Recompiler.vu1SignOverflow
											 : EmuConfig.Cpu.Recompiler.vu0Overflow && !EmuConfig.Cpu.Recompiler.vu0SignOverflow);
		bopt.xgkickHack = cfg.xgkickHack;
		bopt.ibitHack = cfg.ibitHack;
		bopt.yieldChecks = !opts.noYield;
		vuaot::ir::Function fn;
		u64 ssaInsns = 0;
		const auto t0 = std::chrono::steady_clock::now();
		{
			PmuCounters::Group pmu;
			if (pmu.Open() && pmu.IsAvailable(PmuCounters::InstructionsRetired))
			{
				const auto v = pmu.Measure([&]() { fn = vuaot::ir::BuildSSA(graph, bopt); });
				ssaInsns = v[PmuCounters::InstructionsRetired];
			}
			else
				fn = vuaot::ir::BuildSSA(graph, bopt);
		}
		const auto t1 = std::chrono::steady_clock::now();
		const auto problems = vuaot::ir::Verify(fn);
		std::printf("[vuaot-ssa] blocks=%zu insts=%u phis=%u phis_created=%u trivial_removed=%u scc_removed=%u "
					"verify=%zu build_insns=%llu build_us=%lld badops=%u%s%s\n",
			fn.blocks.size(), fn.LiveInsts(), fn.LivePhis(), fn.phisCreated, fn.phisRemovedTrivial, fn.phisRemovedScc,
			problems.size(), static_cast<unsigned long long>(ssaInsns),
			static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()), fn.badOps,
			fn.unsupported ? " unsupported=" : "", fn.unsupported ? fn.unsupportedWhy.c_str() : "");
		for (size_t k = 0; k < problems.size() && k < 5; k++)
			std::printf("  verify: %s\n", problems[k].c_str());
		if (opts.dumpIr)
			std::fputs(vuaot::ir::Dump(fn, graph).c_str(), stdout);

		totBlocks += sink.blocks.size();
		totMatched += matched;
		totOps += ops;
		for (const auto& c : classes)
			totClasses[c.first] += c.second;
	}

	std::printf("[vuaot-total] captures=%zu blocks=%llu match=%llu ops=%llu",
		records.size(), static_cast<unsigned long long>(totBlocks),
		static_cast<unsigned long long>(totMatched), static_cast<unsigned long long>(totOps));
	for (const auto& c : totClasses)
		std::printf(" %s:%llu", c.first.c_str(), static_cast<unsigned long long>(c.second));
	std::printf("\n");
	return failures ? 4 : 0;
}

#include "arm64/vuaot/VuAotTier.h"
#include "arm64/vuaot/VuAotRegion.h"
#include "arm64/vuaot/VuAotLive.h"
#include "arm64/AsmHelpers.h"
#include "harness/VuSnapshot.h"

#include <memory>

namespace vu_capture_internal
{
	void GetCompiledRange(int vu_index, const u8** out_start, const u8** out_end);
}

namespace
{
	struct BodyCtx
	{
		std::unique_ptr<vuaot::BodyTier> tier;
		std::unique_ptr<vuaot::RegionTier> region; // --vuaot-region
		bool regionBg = false;                     // --vuaot-region-bg: `region` is the options template of the worker's jobs
		bool trace = false;
		bool traceOnly = false; // reference arm: trace, never lower
		u32 blocks = 0;

		BodyCtx(u32 vu, const u8* micro, const VuAotModeOptions& o)
			: tier(std::make_unique<vuaot::BodyTier>(vu, micro))
		{
			tier->only = o.lowerOnly;
			tier->limit = o.lowerLimit;
			for (const char* p = o.lowerSkip.c_str(); *p;)
			{
				tier->skip.insert(std::atoi(p));
				while (*p && *p != ',')
					p++;
				if (*p)
					p++;
			}
			tier->dump = o.dumpIr;
			trace = o.trace;
			if (o.region)
			{
				region = std::make_unique<vuaot::RegionTier>(vu, micro);
				region->maxRegions = o.regionLimit;
				region->maxVHomes = o.vHomes;
				region->maxGHomes = o.gHomes;
				region->schedule = o.schedule;
				region->profile = o.profile;
				region->splitHomes = o.splitHomes;
				region->wMemHomes = o.wMemHomes;
				region->stickyAcc = o.stickyAcc;
				region->accAlways = o.accAlways;
				region->handoffLiveness = o.handoff;
				region->dropSticky = o.dropSticky;
				region->handoffMutation = static_cast<vuaot::RegionTier::HandoffMutation>(o.handoffMutation);
				region->dropStickyMutation = o.dropStickyMutation;
				region->merge = o.merge;
				region->dump = o.dumpIr;
				regionBg = o.regionBg;
			}
		}

		void Install();
	};

	bool OnRegion(const vuaot::MvuBlockTrace& b, const vuaot::MvuEmitEnv& env, void* user)
	{
		BodyCtx& c = *static_cast<BodyCtx*>(user);
		return c.region->OnCompile(b, env, nullptr);
	}

	void Uninstall()
	{
		vuaot::g_vuAotBodyHook = nullptr;
		vuaot::g_vuAotBodyHookUser = nullptr;
		vuaot::g_vuAotRegionHook = nullptr;
		vuaot::g_vuAotRegionHookUser = nullptr;
	}

	// --vuaot-trace: every compiled block logs its start PC and entry key on
	// entry, in both runs, to find where two executions part.
	std::vector<u64> g_trace;
	std::vector<u64> g_traceVals;
	long dumpAt = -1; // --vuaot-trace-dump N: print the state at block entry N
	// The entry state of a block: VF, VI (low halves), ACC from VURegs, the
	// MAC and clip instances, the status instances and the Q/P lanes.
	void TraceBlock(u64 v, const u8* flags, u64 s01, u64 s23, u64 pq0, u64 pq1)
	{
		const VURegs& r = vuRegs[1];
		u64 h = 1469598103934665603ull;
		const auto mix = [&](const void* p, size_t n) {
			const u8* b = static_cast<const u8*>(p);
			for (size_t i = 0; i < n; i++)
				h = (h ^ b[i]) * 1099511628211ull;
		};
		mix(&r.VF[1], sizeof(r.VF) - 16);
		for (int i = 1; i < 16; i++)
			mix(&r.VI[i].US[0], 2);
		mix(&r.ACC, 16);
		mix(flags, 32);
		mix(&s01, 8);
		mix(&s23, 8);
		mix(&pq0, 8);
		mix(&pq1, 8);
		if (dumpAt >= 0 && static_cast<long>(g_trace.size()) == dumpAt)
		{
			std::printf("[trace-dump %ld] pc=%04x\n", dumpAt, static_cast<u32>(v >> 32));
			for (int i = 1; i < 32; i++)
				std::printf("  vf%02d %08x %08x %08x %08x\n", i, r.VF[i].UL[0], r.VF[i].UL[1], r.VF[i].UL[2], r.VF[i].UL[3]);
			std::printf("  acc  %08x %08x %08x %08x\n", r.ACC.UL[0], r.ACC.UL[1], r.ACC.UL[2], r.ACC.UL[3]);
			for (int i = 1; i < 16; i++)
				std::printf("  vi%02d %04x\n", i, r.VI[i].US[0]);
			const u32* fl = reinterpret_cast<const u32*>(flags);
			std::printf("  mac %08x %08x %08x %08x clip %08x %08x %08x %08x\n", fl[0], fl[1], fl[2], fl[3], fl[4], fl[5], fl[6], fl[7]);
			std::printf("  status %08x %08x %08x %08x\n", static_cast<u32>(s01), static_cast<u32>(s01 >> 32), static_cast<u32>(s23), static_cast<u32>(s23 >> 32));
			std::printf("  pq %08x %08x %08x %08x\n", static_cast<u32>(pq0), static_cast<u32>(pq0 >> 32), static_cast<u32>(pq1), static_cast<u32>(pq1 >> 32));
		}
		g_trace.push_back(v);
		g_traceVals.push_back(h);
	}

	void EmitTraceCall(u32 pc, const u8* key, s32 offVIbackup)
	{
		namespace a64 = vixl::aarch64;
		(void)offVIbackup;
		u64 h = 1469598103934665603ull;
		for (int i = 0; i < 96; i++)
			h = (h ^ key[i]) * 1099511628211ull;
		const u64 v = (static_cast<u64>(pc) << 32) | (h & 0xffffffffu);
		armAsm->Sub(a64::sp, a64::sp, 64);
		armAsm->Stp(a64::q25, a64::q26, a64::MemOperand(a64::sp));
		armAsm->Str(a64::q28, a64::MemOperand(a64::sp, 32));
		armAsm->Mov(a64::x0, v);
		armAsm->Mov(a64::x1, a64::x24);
		armAsm->Mov(a64::w2, a64::w20);
		armAsm->Orr(a64::x2, a64::x2, a64::Operand(a64::x21, a64::LSL, 32));
		armAsm->Mov(a64::w3, a64::w22);
		armAsm->Orr(a64::x3, a64::x3, a64::Operand(a64::x23, a64::LSL, 32));
		armAsm->Mov(a64::x4, a64::v28.V2D(), 0);
		armAsm->Mov(a64::x5, a64::v28.V2D(), 1);
		armEmitCall(reinterpret_cast<const void*>(&TraceBlock));
		armAsm->Ldp(a64::q25, a64::q26, a64::MemOperand(a64::sp));
		armAsm->Ldr(a64::q28, a64::MemOperand(a64::sp, 32));
		armAsm->Add(a64::sp, a64::sp, 64);
	}

	bool OnBody(const vuaot::MvuBlockTrace& b, const vuaot::MvuEmitEnv& env, void* user)
	{
		BodyCtx& c = *static_cast<BodyCtx*>(user);
		if (b.vuIndex == env.vuIndex)
			c.blocks++;
		if (c.trace)
			EmitTraceCall(b.startPC, b.entryState, env.offVIbackup);
		if (c.traceOnly)
			return false;
		return c.tier->TryLower(b, env, nullptr);
	}

	void BodyCtx::Install()
	{
		vuaot::g_vuAotBodyHook = &OnBody;
		vuaot::g_vuAotBodyHookUser = this;
		if (region)
		{
			vuaot::g_vuAotRegionHook = regionBg ? &vuaot::live::RegionHookViaWorker : &OnRegion;
			vuaot::g_vuAotRegionHookUser = regionBg ? static_cast<void*>(region.get()) : static_cast<void*>(this);
		}
	}

	u64 CompiledBytes(int vu)
	{
		const u8* s = nullptr;
		const u8* e = nullptr;
		vu_capture_internal::GetCompiledRange(vu, &s, &e);
		return (s && e && e > s) ? static_cast<u64>(e - s) : 0;
	}

	struct FieldRange
	{
		const char* name;
		size_t off, size;
	};

	std::vector<std::string> DiffRegs(const VURegs& a, const VURegs& b)
	{
#define FR(f) {#f, offsetof(VURegs, f), sizeof(VURegs::f)}
		static const FieldRange kFields[] = {
			FR(VF), FR(VI), FR(ACC), FR(q), FR(p), FR(idx), FR(cycle), FR(flags), FR(code), FR(start_pc),
			FR(branch), FR(branchpc), FR(delaybranchpc), FR(takedelaybranch), FR(ebit), FR(pending_q), FR(pending_p),
			FR(micro_macflags), FR(micro_clipflags), FR(micro_statusflags), FR(macflag), FR(statusflag), FR(clipflag),
			FR(nextBlockCycles), FR(xgkickaddr), FR(xgkickdiff), FR(xgkicksizeremaining), FR(xgkicklastcycle),
			FR(xgkickcyclecount), FR(xgkickenable), FR(xgkickendpacket), FR(VIBackupCycles), FR(VIOldValue),
			FR(VIRegNumber), FR(fmac), FR(fmacreadpos), FR(fmacwritepos), FR(fmaccount), FR(fdiv), FR(efu), FR(ialu),
			FR(ialureadpos), FR(ialuwritepos), FR(ialucount),
		};
#undef FR
		std::vector<std::string> out;
		const u8* pa = reinterpret_cast<const u8*>(&a);
		const u8* pb = reinterpret_cast<const u8*>(&b);
		for (const FieldRange& f : kFields)
		{
			for (size_t i = 0; i < f.size; i += 4)
			{
				const size_t n = std::min<size_t>(4, f.size - i);
				if (std::memcmp(pa + f.off + i, pb + f.off + i, n) == 0)
					continue;
				u32 va = 0, vb = 0;
				std::memcpy(&va, pa + f.off + i, n);
				std::memcpy(&vb, pb + f.off + i, n);
				char buf[128];
				std::snprintf(buf, sizeof(buf), "%s+%zu: mvu=%08x vuaot=%08x", f.name, i, va, vb);
				out.emplace_back(buf);
			}
		}
		return out;
	}

	// The exactness contract: what a game can observe, field by field.
	// `obs` fields must match always; `ring` fields (the four-deep flag copies the recompiler
	// shadows) must match at a program end and may differ at a budget stop, where the continuation
	// decides; `other` is everything else in VURegs the engines write (the interpreter's scratch,
	// which neither recompiler touches): any difference there is unclassified.
	struct SplitDiff
	{
		std::vector<std::string> obs, ring, other;
		std::vector<std::string> sticky; // relaxation 3: the sticky bits of STATUS (only with `relaxSticky`)
	};

	SplitDiff DiffRegsSplit(const VURegs& a, const VURegs& b, bool relaxSticky)
	{
		struct FieldClass
		{
			const char* name;
			size_t off, size;
			int cls; // 0 observable, 1 ring, 2 other
		};
#define FO(f) {#f, offsetof(VURegs, f), sizeof(VURegs::f), 0}
#define FR(f) {#f, offsetof(VURegs, f), sizeof(VURegs::f), 1}
#define FX(f) {#f, offsetof(VURegs, f), sizeof(VURegs::f), 2}
		static const FieldClass kFields[] = {
			FO(VF), FO(VI), FO(ACC), FO(q), FO(p), FO(cycle), FO(flags), FO(start_pc), FO(ebit), FO(pending_q), FO(pending_p),
			FO(nextBlockCycles), FO(xgkickaddr), FO(xgkickdiff), FO(xgkicksizeremaining), FO(xgkicklastcycle),
			FO(xgkickcyclecount), FO(xgkickenable), FO(xgkickendpacket), FO(VIBackupCycles), FO(VIOldValue), FO(VIRegNumber),
			FR(micro_macflags), FR(micro_clipflags), FR(micro_statusflags),
			FX(idx), FX(code), FX(branch), FX(branchpc), FX(delaybranchpc), FX(takedelaybranch), FX(macflag), FX(statusflag),
			FX(clipflag), FX(fmac), FX(fmacreadpos), FX(fmacwritepos), FX(fmaccount), FX(fdiv), FX(efu), FX(ialu),
			FX(ialureadpos), FX(ialuwritepos), FX(ialucount),
		};
#undef FO
#undef FR
#undef FX
		SplitDiff out;
		const u8* pa = reinterpret_cast<const u8*>(&a);
		const u8* pb = reinterpret_cast<const u8*>(&b);
		for (const FieldClass& f : kFields)
			for (size_t i = 0; i < f.size; i += 4)
			{
				const size_t n = std::min<size_t>(4, f.size - i);
				if (std::memcmp(pa + f.off + i, pb + f.off + i, n) == 0)
					continue;
				u32 va = 0, vb = 0;
				std::memcpy(&va, pa + f.off + i, n);
				std::memcpy(&vb, pb + f.off + i, n);
				char buf[128];
				std::snprintf(buf, sizeof(buf), "%s+%zu: mvu=%08x vuaot=%08x", f.name, i, va, vb);
				// Relaxation 3: the sticky zero/sign bits (bits 6-7 of the STATUS register, the low byte of
				// the status ring copies) and the sticky invalid/divide-by-zero bits (register bits 10-11,
				// ring bits 24-25) may differ; anything else in those words may not.
				if (relaxSticky)
				{
					u32 mask = 0;
					if (std::strcmp(f.name, "VI") == 0 && i == REG_STATUS_FLAG * sizeof(REG_VI))
						mask = 0xCC0;
					else if (std::strcmp(f.name, "micro_statusflags") == 0)
						mask = 0x30000FF;
					if (mask && ((va ^ vb) & ~mask) == 0)
					{
						out.sticky.emplace_back(buf);
						continue;
					}
				}
				// The STATUS, MAC and CLIP registers (VI 16-18) are what a budget stop mirrors from the
				// newest ring copies: the ring class, exact at a program end.
				const bool flagReg = std::strcmp(f.name, "VI") == 0 && i >= REG_STATUS_FLAG * sizeof(REG_VI) &&
									 i < (REG_CLIP_FLAG + 1) * sizeof(REG_VI);
				(flagReg ? out.ring : f.cls == 0 ? out.obs : f.cls == 1 ? out.ring : out.other).emplace_back(buf);
			}
		return out;
	}

	// Memory, GIF bytes, E bit and cycle count: observable always.
	void DiffOutputs(const recompiler_tests::VuSnapshot& sa, const recompiler_tests::VuSnapshot& sb, const std::vector<u8>& pa, const std::vector<u8>& pb, bool ea, bool eb,
		u64 ca, u64 cb, std::vector<std::string>& out)
	{
		for (size_t w = 0; w < sa.mem_windows.size() && w < sb.mem_windows.size(); w++)
			for (size_t i = 0; i < sa.mem_windows[w].bytes.size(); i += 16)
				if (std::memcmp(&sa.mem_windows[w].bytes[i], &sb.mem_windows[w].bytes[i],
						std::min<size_t>(16, sa.mem_windows[w].bytes.size() - i)) != 0)
				{
					char buf[64];
					std::snprintf(buf, sizeof(buf), "mem[0x%04zx]", i);
					out.emplace_back(buf);
				}
		if (pa != pb)
			out.emplace_back("GIF path 1 bytes differ (" + std::to_string(pa.size()) + " vs " + std::to_string(pb.size()) + ")");
		if (ea != eb)
			out.emplace_back("ebit differs");
		if (ca != cb)
			out.emplace_back("cycles differ");
	}

	// Steady-state retired instructions: the least over the iterations after
	// the first. A later iteration can still compile a block it had not
	// reached before (and, in the vu-aot arm, run the hook's analysis), which
	// only ever adds instructions.
	u64 MinCounter(const std::vector<PmuCounters::Values>& v, int counter)
	{
		if (v.size() < 2)
			return 0;
		u64 m = ~0ull;
		for (size_t i = 1; i < v.size(); i++)
			m = std::min<u64>(m, v[i][counter]);
		return m;
	}
} // namespace

int RunVuAotRun(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuAotModeOptions& opts,
	void (*applyConfig)(const vu_capture::CaptureRecord&))
{
	int failures = 0;
	dumpAt = opts.traceDump;
	u64 totIdentical = 0, totBlocks = 0, totLowered = 0;
	for (size_t fi = 0; fi < records.size(); ++fi)
	{
		const auto& rec = records[fi];
		applyConfig(rec);
		EmuConfig.Speedhacks.vuFlagHack = opts.flagHack != 0;
		const int vu = rec.vu_index;

		// Reference: microVU alone (with --vuaot-trace, the hook only traces).
		BodyCtx refCtx(vu, vuRegs[vu].Micro, opts);
		refCtx.traceOnly = true;
		g_trace.clear();
		g_traceVals.clear();
		if (opts.trace)
		{
			vuaot::g_vuAotBodyHook = &OnBody;
			vuaot::g_vuAotBodyHookUser = &refCtx;
		}
		recompiler_tests::VuChainResult refChain;
		if (opts.observe)
			refChain = recompiler_tests::ReplayJitChain(rec, opts.cycleOverride, opts.resumes);
		const auto ref = opts.observe ? refChain.first : recompiler_tests::ReplayJitOnly(rec, opts.cycleOverride);
		vuaot::g_vuAotBodyHook = nullptr;
		const std::vector<u64> refTrace = g_trace;
		const std::vector<u64> refVals = g_traceVals;
		g_trace.clear();
		g_traceVals.clear();
		const u64 refBytes = CompiledBytes(vu);

		// Regions: record the JR/JALR targets first, from a plain microVU
		// replay with every block entry traced (the block that jumped, then the
		// block it reached). In a game these come from earlier runs.
		std::map<u32, std::vector<u32>> jumpTargets;
		u32 mvuBlocks = 0; // blocks the plain microVU replay compiled
		if (opts.region)
		{
			BodyCtx recCtx(vu, vuRegs[vu].Micro, opts);
			recCtx.traceOnly = true;
			recCtx.trace = true;
			g_trace.clear();
			g_traceVals.clear();
			vuaot::g_vuAotBodyHook = &OnBody;
			vuaot::g_vuAotBodyHookUser = &recCtx;
			recompiler_tests::ReplayJitOnly(rec, opts.cycleOverride);
			Uninstall();
			mvuBlocks = recCtx.blocks;
			for (size_t k = 1; k < g_trace.size(); k++)
			{
				const u32 from = static_cast<u32>(g_trace[k - 1] >> 32), to = static_cast<u32>(g_trace[k] >> 32);
				auto& v = jumpTargets[from];
				if (std::find(v.begin(), v.end(), to) == v.end())
					v.push_back(to);
			}
			g_trace.clear();
			g_traceVals.clear();
		}

		BodyCtx ctx(vu, vuRegs[vu].Micro, opts);
		if (ctx.region)
			ctx.region->jumpTargets = jumpTargets;
		const vuaot::live::Stats bgBefore = vuaot::live::GetStats();
		ctx.Install();
		recompiler_tests::VuChainResult runChain;
		if (opts.observe)
			runChain = recompiler_tests::ReplayJitChain(rec, opts.cycleOverride, opts.resumes);
		const auto run = opts.observe ? runChain.first : recompiler_tests::ReplayJitOnly(rec, opts.cycleOverride);
		const vuaot::live::Stats bgAfter = vuaot::live::GetStats();
		if (ctx.region && opts.profile)
			ctx.region->PrintProfile(stdout);
		const u64 runBytes = CompiledBytes(vu);
		u64 bailsEntry = 0, bailsLoop = 0;
		// The region counters: the worker's jobs' (through the live tier's stats) with
		// --vuaot-region-bg, the region tier's own otherwise.
		u32 stickyDropped = 0, stickyImages = 0, statusReaders = 0, stickyReaders = 0, stickyDivReaders = 0;
		if (ctx.region && ctx.regionBg)
		{
			bailsEntry = bgAfter.bailsEntry - bgBefore.bailsEntry;
			bailsLoop = bgAfter.bailsLoopHead - bgBefore.bailsLoopHead;
			stickyDropped = static_cast<u32>(bgAfter.stickyDropped - bgBefore.stickyDropped);
			stickyImages = static_cast<u32>(bgAfter.stickyImages - bgBefore.stickyImages);
			statusReaders = static_cast<u32>(bgAfter.statusReaders - bgBefore.statusReaders);
			stickyReaders = static_cast<u32>(bgAfter.stickyReaders - bgBefore.stickyReaders);
			stickyDivReaders = static_cast<u32>(bgAfter.stickyDivReaders - bgBefore.stickyDivReaders);
		}
		else if (ctx.region)
		{
			ctx.region->BailCounts(bailsEntry, bailsLoop);
			stickyDropped = ctx.region->StickyDropped();
			stickyImages = ctx.region->StickyReaderImages();
			statusReaders = ctx.region->StatusReaders();
			stickyReaders = ctx.region->StickyReaders();
			stickyDivReaders = ctx.region->StickyDivReaders();
		}
		Uninstall();

		if (!ref.ok || !run.ok)
		{
			std::printf("[vuaot-run] %s replay-failed\n", names[fi].c_str());
			failures++;
			continue;
		}

		std::vector<std::string> diffs = DiffRegs(ref.jit_snapshot.regs, run.jit_snapshot.regs);
		const auto& ma = ref.jit_snapshot.mem_windows;
		const auto& mb = run.jit_snapshot.mem_windows;
		for (size_t w = 0; w < ma.size() && w < mb.size(); w++)
		{
			for (size_t i = 0; i < ma[w].bytes.size(); i += 16)
			{
				if (std::memcmp(&ma[w].bytes[i], &mb[w].bytes[i], std::min<size_t>(16, ma[w].bytes.size() - i)) != 0)
				{
					char buf[64];
					std::snprintf(buf, sizeof(buf), "mem[0x%04zx]", i);
					diffs.emplace_back(buf);
				}
			}
		}
		if (ref.path1_packets_jit != run.path1_packets_jit)
			diffs.emplace_back("GIF path 1 bytes differ (" + std::to_string(ref.path1_packets_jit.size()) + " vs " +
							   std::to_string(run.path1_packets_jit.size()) + ")");
		if (ref.jit_ebit != run.jit_ebit)
			diffs.emplace_back("ebit differs");
		if (ref.jit_cycles != run.jit_cycles)
			diffs.emplace_back("cycles differ");

		// The exactness contract: the same, field by field, with the flag-ring copies at a budget
		// stop set apart. A stop's ring may differ only if the continuation (the run re-entered
		// after the stop, as a game does) ends in the same observable state.
		bool observable = true;
		std::string obsText;
		std::vector<std::string> obsDiffs;
		if (opts.observe)
		{
			const bool relaxSticky = stickyDropped > 0;
			const SplitDiff first = DiffRegsSplit(ref.jit_snapshot.regs, run.jit_snapshot.regs, relaxSticky);
			const SplitDiff fin = DiffRegsSplit(refChain.final_snapshot.regs, runChain.final_snapshot.regs, relaxSticky);
			std::vector<std::string> out1, outF;
			DiffOutputs(ref.jit_snapshot, run.jit_snapshot, ref.path1_packets_jit, run.path1_packets_jit, ref.jit_ebit, run.jit_ebit,
				ref.jit_cycles, run.jit_cycles, out1);
			DiffOutputs(refChain.final_snapshot, runChain.final_snapshot, refChain.final_path1, runChain.final_path1,
				refChain.final_ebit, runChain.final_ebit, refChain.final_cycles, runChain.final_cycles, outF);
			for (const auto& d : first.obs)
				obsDiffs.push_back("stop1 " + d);
			for (const auto& d : out1)
				obsDiffs.push_back("stop1 " + d);
			for (const auto& d : first.other)
				obsDiffs.push_back("stop1 unclassified " + d);
			for (const auto& d : fin.obs)
				obsDiffs.push_back("final " + d);
			for (const auto& d : outF)
				obsDiffs.push_back("final " + d);
			for (const auto& d : fin.other)
				obsDiffs.push_back("final unclassified " + d);
			// ring copies: exact at a program end
			if (ref.jit_ebit)
				for (const auto& d : first.ring)
					obsDiffs.push_back("program end ring " + d);
			if (refChain.final_ebit)
				for (const auto& d : fin.ring)
					obsDiffs.push_back("final program end ring " + d);
			observable = obsDiffs.empty();
			char buf[200];
			std::snprintf(buf, sizeof(buf), " obs=%d ringdiff=%zu ringfinal=%zu stopped=%d resumes=%u term=%d", observable ? 1 : 0,
				first.ring.size(), fin.ring.size(), ref.jit_ebit ? 0 : 1, refChain.resumes, refChain.final_ebit ? 1 : 0);
			obsText = buf;
			std::snprintf(buf, sizeof(buf), " stickydrop=%u stickyimg=%u stickydiff=%zu", stickyDropped, stickyImages,
				first.sticky.size() + fin.sticky.size());
			obsText += buf;
			std::snprintf(buf, sizeof(buf), " bails_entry=%llu bails_loophead=%llu status_readers=%u sticky_readers=%u sticky_div_readers=%u",
				static_cast<unsigned long long>(bailsEntry), static_cast<unsigned long long>(bailsLoop),
				statusReaders, stickyReaders, stickyDivReaders);
			obsText += buf;
		}
		if (opts.trace)
		{
			size_t k = 0;
			while (k < refTrace.size() && k < g_trace.size() && refTrace[k] == g_trace[k] && refVals[k] == g_traceVals[k])
				k++;
			char buf[160];
			std::snprintf(buf, sizeof(buf), "trace: %zu vs %zu block entries, first difference at %zu", refTrace.size(), g_trace.size(), k);
			diffs.emplace_back(buf);
			for (size_t j = (k > 3 ? k - 3 : 0); j < k + 3; j++)
			{
				std::snprintf(buf, sizeof(buf), "  [%zu] mvu=%04x/%08x state=%08x/%04x vuaot=%04x/%08x state=%08x/%04x", j,
					j < refTrace.size() ? static_cast<u32>(refTrace[j] >> 32) : 0xffff, j < refTrace.size() ? static_cast<u32>(refTrace[j]) : 0,
					j < refVals.size() ? static_cast<u32>(refVals[j]) : 0, j < refVals.size() ? static_cast<u32>(refVals[j] >> 32) : 0,
					j < g_trace.size() ? static_cast<u32>(g_trace[j] >> 32) : 0xffff, j < g_trace.size() ? static_cast<u32>(g_trace[j]) : 0,
					j < g_traceVals.size() ? static_cast<u32>(g_traceVals[j]) : 0, j < g_traceVals.size() ? static_cast<u32>(g_traceVals[j] >> 32) : 0);
				diffs.emplace_back(buf);
			}
		}

		u64 insRef = 0, insRun = 0, cycRef = 0, cycRun = 0;
		u64 compRef = 0, compRun = 0; // first iteration (compile + run) minus a steady one
		u32 steadyCompiles = 0;
		if (opts.benchIters >= 2)
		{
			const auto refSamples = recompiler_tests::BenchJitSteady(rec, opts.benchIters);
			insRef = MinCounter(refSamples, PmuCounters::InstructionsRetired);
			cycRef = MinCounter(refSamples, PmuCounters::CpuCycles);
			if (insRef)
				compRef = refSamples[0][PmuCounters::InstructionsRetired] - std::min<u64>(insRef, refSamples[0][PmuCounters::InstructionsRetired]);
			VuAotModeOptions bo = opts;
			bo.trace = false;
			bo.dumpIr = false;
			BodyCtx bctx(vu, vuRegs[vu].Micro, bo);
			if (bctx.region)
				bctx.region->jumpTargets = jumpTargets;
			bctx.Install();
			const auto runSamples = recompiler_tests::BenchJitSteady(rec, opts.benchIters);
			insRun = MinCounter(runSamples, PmuCounters::InstructionsRetired);
			cycRun = MinCounter(runSamples, PmuCounters::CpuCycles);
			if (insRun)
				compRun = runSamples[0][PmuCounters::InstructionsRetired] - std::min<u64>(insRun, runSamples[0][PmuCounters::InstructionsRetired]);
			Uninstall();
			// Blocks compiled after the first iteration (the identity run is one
			// such first iteration): their compile cost is inside the counts.
			steadyCompiles = bctx.blocks > ctx.blocks ? bctx.blocks - ctx.blocks : 0;
		}

		const bool identical = diffs.empty();
		std::printf("[vuaot-run] %s vu%d pc=%04x blocks=%u lowered=%u identical=%d ebit=%d refcode=%llu runcode=%llu body=%llu "
					"ins_mvu=%llu ins_vuaot=%llu cyc_mvu=%llu cyc_vuaot=%llu steadycompiles=%u cins_mvu=%llu cins_vuaot=%llu",
			names[fi].c_str(), vu, rec.start_pc, ctx.blocks, ctx.tier->Lowered(), identical ? 1 : 0, ref.jit_ebit ? 1 : 0,
			static_cast<unsigned long long>(refBytes), static_cast<unsigned long long>(runBytes),
			static_cast<unsigned long long>(ctx.tier->BodyBytes()), static_cast<unsigned long long>(insRef),
			static_cast<unsigned long long>(insRun), static_cast<unsigned long long>(cycRef), static_cast<unsigned long long>(cycRun),
			steadyCompiles, static_cast<unsigned long long>(compRef), static_cast<unsigned long long>(compRun));
		if (ctx.region && ctx.regionBg)
		{
			// (the regions were built by the worker's jobs: the live tier's counters)
			std::printf(" regions=%llu rnodes=%llu rmerged=%llu rexits=%llu rbytes=%llu mvublocks=%u bgjobs=%llu bgdeclined=%llu veneers=%llu",
				static_cast<unsigned long long>(bgAfter.regions - bgBefore.regions),
				static_cast<unsigned long long>(bgAfter.regionNodes - bgBefore.regionNodes),
				static_cast<unsigned long long>(bgAfter.regionMerged - bgBefore.regionMerged),
				static_cast<unsigned long long>(bgAfter.regionExits - bgBefore.regionExits),
				static_cast<unsigned long long>(bgAfter.regionBytes - bgBefore.regionBytes), mvuBlocks,
				static_cast<unsigned long long>(bgAfter.jobsDone - bgBefore.jobsDone),
				static_cast<unsigned long long>(bgAfter.jobsDeclined - bgBefore.jobsDeclined),
				static_cast<unsigned long long>(bgAfter.veneers - bgBefore.veneers));
		}
		else if (ctx.region)
		{
			std::printf(" regions=%u rnodes=%u rmerged=%u rmclasses=%u rexits=%u rbytes=%llu mvublocks=%u racc=%u raccnl=%u raccn=%u raccun=%u rhoe=%u rhod=%u", ctx.region->Regions(), ctx.region->Nodes(),
				ctx.region->Merged(), ctx.region->MergeClasses(), ctx.region->Exits(), static_cast<unsigned long long>(ctx.region->Bytes()), mvuBlocks, ctx.region->AccRegions(), ctx.region->AccNonLinear(),
				ctx.region->AccAccumulated(), ctx.region->AccUnprofitable(), ctx.region->HandoffEdges(), ctx.region->HandoffRingsDead());
			for (const auto& d : ctx.region->Declined())
			{
				std::string k = d.first;
				for (char& ch : k)
					if (ch == ' ')
						ch = '_';
				std::printf(" rdecl:%s=%u", k.c_str(), d.second);
			}
		}
		for (const auto& d : ctx.tier->Declined())
		{
			std::string k = d.first;
			for (char& ch : k)
				if (ch == ' ')
					ch = '_';
			std::printf(" decl:%s=%u", k.c_str(), d.second);
		}
		std::printf("%s\n", obsText.c_str());
		for (size_t k = 0; k < diffs.size() && (opts.verbose || k < 8); k++)
			std::printf("  diff: %s\n", diffs[k].c_str());
		for (size_t k = 0; k < obsDiffs.size() && k < 8; k++)
			std::printf("  obsdiff: %s\n", obsDiffs[k].c_str());
		totIdentical += identical;
		totBlocks += ctx.blocks;
		totLowered += ctx.tier->Lowered();
		if (opts.observe ? !observable : !identical)
			failures++;
	}
	std::printf("[vuaot-run-total] captures=%zu identical=%llu blocks=%llu lowered=%llu\n", records.size(),
		static_cast<unsigned long long>(totIdentical), static_cast<unsigned long long>(totBlocks),
		static_cast<unsigned long long>(totLowered));
	return failures ? 4 : 0;
}
